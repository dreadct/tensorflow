/* Copyright 2022 The OpenXLA Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "xla/service/gpu/model/gpu_performance_model.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

#include "absl/container/flat_hash_set.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/time/time.h"
#include "absl/types/span.h"
#include "llvm/ADT/STLExtras.h"
#include "xla/hlo/ir/hlo_casting_utils.h"
#include "xla/hlo/ir/hlo_instruction.h"
#include "xla/hlo/ir/hlo_instructions.h"
#include "xla/hlo/ir/hlo_opcode.h"
#include "xla/service/gpu/backend_configs.pb.h"
#include "xla/service/gpu/hlo_fusion_analysis.h"
#include "xla/service/gpu/launch_dimensions.h"
#include "xla/service/gpu/model/coalescing_analysis.h"
#include "xla/service/gpu/model/gpu_hlo_cost_analysis.h"
#include "xla/service/gpu/model/gpu_performance_model_base.h"
#include "xla/shape_util.h"
#include "xla/stream_executor/device_description.h"
#include "xla/util.h"
#include "tsl/platform/status.h"

namespace xla {
namespace gpu {

/*static*/ EstimateRunTimeData
GpuPerformanceModel::EstimateRunTimeForInstruction(
    const HloInstruction* instr, const GpuHloCostAnalysis* cost_analysis,
    const GpuPerformanceModelOptions& config) {
  VLOG(8) << "EstimateRunTimeForInstruction: " << instr->name();
  const se::DeviceDescription* device_info = cost_analysis->device_info_;

  int64_t flops = cost_analysis->flop_count(*instr);
  int64_t bytes_written = cost_analysis->output_bytes_accessed(*instr);
  int64_t bytes_read = cost_analysis->bytes_accessed(*instr) - bytes_written;

  // Use the analysis cache if present.
  // TODO(jreiffers): Remove this once all callers use a cache.
  std::optional<HloFusionAnalysis> local_analysis;
  if (!config.fusion_analysis_cache) {
    local_analysis = AnalyzeFusion(*instr, *cost_analysis->device_info_);
  }
  const auto& fusion_analysis = config.fusion_analysis_cache
                                    ? config.fusion_analysis_cache->Get(*instr)
                                    : local_analysis.value();
  LaunchDimensions launch_dimensions = EstimateFusionLaunchDimensions(
      ShapeUtil::ElementsInRecursive(instr->shape()), fusion_analysis,
      *device_info);
  int64_t num_threads = launch_dimensions.launch_bound();
  int64_t num_blocks = launch_dimensions.num_blocks();

  absl::Duration compute_time = ComputeTime(*device_info, flops, num_threads);

  // TODO(jreiffers): We should be checking each operand.
  bool coalesced = IsReadCoalescedHeuristic(fusion_analysis, instr,
                                            /*consumer=*/nullptr);

  absl::Duration read_time;
  for (int i = 0; i < instr->operand_count(); ++i) {
    auto element_type = instr->operand(i)->shape().element_type();
    // Information about data read taking into account utilization.
    // If `operand_utilization` is 0, `operand_bytes_accessed` should be also 0.
    int64_t n_bytes_total = cost_analysis->operand_bytes_accessed(*instr, i);
    float operand_utilization = cost_analysis->operand_utilization(*instr, i);

    // An estimate how much data would need to fit into L1/L2 cache to speed up
    // the operand access.
    // If `operand_utilization` < 1, only a part of the full operand size should
    // be read. Otherwise, `n_bytes_total / operand_utilization` is the
    // size of the operand without reuse.
    int64_t n_bytes_net =
        std::llround(n_bytes_total / std::max(operand_utilization, 1.0f));

    read_time +=
        ReadTimeWithDRAMHeuristic(*device_info, num_blocks, n_bytes_net,
                                  n_bytes_total, element_type, coalesced);
  }

  absl::Duration write_time = WriteTime(*device_info, bytes_written);
  absl::Duration exec_time = CombineComputeAndMemoryAccessTime(
      compute_time, read_time + write_time, config);

  if (VLOG_IS_ON(8)) {
    LOG(INFO) << "FLOPs: " << flops;
    LOG(INFO) << "Bytes read: " << bytes_read;
    LOG(INFO) << "Bytes written: " << bytes_written;
    LOG(INFO) << "Num threads: " << num_threads;
    LOG(INFO) << "Compute time: " << compute_time;
    LOG(INFO) << "Input read time: " << read_time;
    LOG(INFO) << "Output write time: " << write_time;
  }

  return {flops, bytes_written, num_threads, write_time, exec_time};
}

/*static*/ EstimateRunTimeData
GpuPerformanceModel::EstimateRunTimeForInstructionCached(
    const HloInstruction* instr, const GpuHloCostAnalysis* cost_analysis,
    const GpuPerformanceModelOptions& config) {
  if (config.gpu_performance_model_cache) {
    if (auto cached_result = config.gpu_performance_model_cache->Get(*instr)) {
      return *cached_result;
    }
  }

  auto runtime_data =
      EstimateRunTimeForInstruction(instr, cost_analysis, config);

  if (config.gpu_performance_model_cache) {
    config.gpu_performance_model_cache->Set(*instr, runtime_data);
  }

  return runtime_data;
}

/*static*/
absl::Duration GpuPerformanceModel::EstimateUnfusedExecTime(
    const HloInstruction* producer, const EstimateRunTimeData& producer_runtime,
    const GpuHloCostAnalysis* cost_analysis,
    const GpuPerformanceModelOptions& config,
    absl::Span<const HloInstruction* const> fused_consumers) {
  const se::DeviceDescription* device_info = cost_analysis->device_info_;

  absl::Duration time_unfused =
      kKernelLaunchOverhead * (fused_consumers.size() + 1) +
      producer_runtime.exec_time;

  for (const HloInstruction* fused_consumer : fused_consumers) {
    VLOG(8) << "Unfused consumer: " << fused_consumer->name();
    float utilization_by_this_consumer =
        GetOperandUtilization(cost_analysis, fused_consumer, producer);

    // Use the analysis cache if present.
    // TODO(jreiffers): Remove this once all callers use a cache.
    std::optional<HloFusionAnalysis> local_analysis;
    if (!config.fusion_analysis_cache) {
      local_analysis = AnalyzeFusion(*fused_consumer, *device_info);
    }
    const auto& analysis_unfused =
        config.fusion_analysis_cache
            ? config.fusion_analysis_cache->Get(*fused_consumer)
            : local_analysis.value();

    LaunchDimensions launch_dimensions_unfused = EstimateFusionLaunchDimensions(
        ShapeUtil::ElementsInRecursive(fused_consumer->shape()),
        analysis_unfused, *device_info);

    int64_t n_bytes_total = std::llround(producer_runtime.bytes_written *
                                         utilization_by_this_consumer);
    int64_t n_bytes_net =
        std::min(producer_runtime.bytes_written, n_bytes_total);

    auto read_time_unfused =
        ReadTime(*device_info, launch_dimensions_unfused.num_blocks(),
                 n_bytes_net, n_bytes_total);

    VLOG(10) << "  Read time unfused: " << read_time_unfused;
    time_unfused += read_time_unfused;
  }

  return time_unfused;
}

/*static*/ absl::Duration GpuPerformanceModel::EstimateRunTimeForFusion(
    const HloInstruction* producer, const HloInstruction* consumer,
    const EstimateRunTimeData& producer_runtime,
    const EstimateRunTimeData& consumer_runtime,
    const GpuHloCostAnalysis* cost_analysis,
    const GpuPerformanceModelOptions& config) {
  VLOG(8) << "EstimateRunTimeForFusion, producer: " << producer->name()
          << " consumer: " << consumer->name();
  const se::DeviceDescription* device_info = cost_analysis->device_info_;

  float utilization_by_this_consumer = cost_analysis->operand_utilization(
      *consumer, consumer->operand_index(producer));

  std::optional<HloFusionAnalysis> local_analysis_fused;
  if (!config.fusion_analysis_cache) {
    local_analysis_fused =
        AnalyzeProducerConsumerFusion(*producer, *consumer, *device_info);
  }
  const auto& fusion_analysis =
      config.fusion_analysis_cache
          ? config.fusion_analysis_cache->Get(*producer, *consumer)
          : local_analysis_fused.value();

  LaunchDimensions launch_dimensions = EstimateFusionLaunchDimensions(
      producer_runtime.num_threads * utilization_by_this_consumer,
      fusion_analysis, *device_info);

  int64_t fused_flops = producer_runtime.flops * utilization_by_this_consumer +
                        consumer_runtime.flops;

  int64_t num_threads = launch_dimensions.launch_bound();
  absl::Duration compute_time =
      ComputeTime(*device_info, fused_flops, num_threads);

  absl::flat_hash_set<const HloInstruction*> fusion_operands;
  for (auto* operand : producer->operands()) {
    fusion_operands.insert(operand);
  }
  for (auto* operand : consumer->operands()) {
    if (operand != producer) {
      fusion_operands.insert(operand);
    }
  }

  absl::Duration read_time;
  for (const auto* operand : fusion_operands) {
    float operand_utilization =
        GetSharedUtilization(cost_analysis, producer, consumer, operand);

    int64_t operand_size = cost_analysis->GetShapeSize(operand->shape());

    int64_t n_bytes_total = std::llround(operand_size * operand_utilization);
    int64_t n_bytes_net = std::min(operand_size, n_bytes_total);

    bool coalesced =
        IsReadCoalescedHeuristic(fusion_analysis, producer, consumer);

    read_time += ReadTimeWithDRAMHeuristic(
        *device_info, launch_dimensions.num_blocks(), n_bytes_net,
        n_bytes_total, operand->shape().element_type(), coalesced);
  }

  if (VLOG_IS_ON(8)) {
    LOG(INFO) << "Fused FLOPs: " << fused_flops;
    LOG(INFO) << "Num threads: " << num_threads;
    LOG(INFO) << "Compute time: " << compute_time;
    LOG(INFO) << "Input read time: " << read_time;
    LOG(INFO) << "Output write time: " << consumer_runtime.write_time;
  }

  return CombineComputeAndMemoryAccessTime(
      compute_time, read_time + consumer_runtime.write_time, config);
}

/*static*/
absl::Duration GpuPerformanceModel::EstimateRunTimeForFusionCached(
    const HloInstruction* producer, const HloInstruction* consumer,
    const EstimateRunTimeData& producer_runtime,
    const EstimateRunTimeData& consumer_runtime,
    const GpuHloCostAnalysis* cost_analysis,
    const GpuPerformanceModelOptions& config) {
  if (config.gpu_performance_model_cache) {
    if (auto fusion_runtime =
            config.gpu_performance_model_cache->Get(*producer, *consumer)) {
      return *fusion_runtime;
    }
  }

  auto fusion_runtime =
      EstimateRunTimeForFusion(producer, consumer, producer_runtime,
                               consumer_runtime, cost_analysis, config);

  if (config.gpu_performance_model_cache) {
    config.gpu_performance_model_cache->Set(*producer, *consumer,
                                            fusion_runtime);
  }
  return fusion_runtime;
}

/*static*/
absl::Duration GpuPerformanceModel::EstimateFusedExecTime(
    const HloInstruction* producer, const EstimateRunTimeData& producer_runtime,
    const GpuHloCostAnalysis* cost_analysis,
    const GpuPerformanceModelOptions& config,
    absl::Span<const HloInstruction* const> fused_consumers,
    bool multi_output) {
  const se::DeviceDescription* device_info = cost_analysis->device_info_;

  absl::Duration exec_time_fused =
      kKernelLaunchOverhead * fused_consumers.size();
  for (auto [idx, fused_consumer] : llvm::enumerate(fused_consumers)) {
    VLOG(8) << "Fused consumer: " << fused_consumer->name();

    float utilization_by_this_consumer = cost_analysis->operand_utilization(
        *fused_consumer, fused_consumer->operand_index(producer));

    std::optional<HloFusionAnalysis> local_analysis_fused;
    if (!config.fusion_analysis_cache) {
      local_analysis_fused = AnalyzeProducerConsumerFusion(
          *producer, *fused_consumer, *device_info);
    }
    const auto& analysis_fused =
        config.fusion_analysis_cache
            ? config.fusion_analysis_cache->Get(*producer, *fused_consumer)
            : local_analysis_fused.value();

    LaunchDimensions launch_dimensions_fused = EstimateFusionLaunchDimensions(
        producer_runtime.num_threads * utilization_by_this_consumer,
        analysis_fused, *device_info);

    absl::Duration compute_time_by_this_consumer = ComputeTime(
        *device_info, producer_runtime.flops * utilization_by_this_consumer,
        launch_dimensions_fused.launch_bound());

    // Here, we assume that the read is distributed over all the threads in the
    // launch grid. Usually this is the case, but not always: for example, a
    // reduce -> broadcast -> elementwise fusion will recompute the reduce. We
    // don't currently have an analysis that is able to detect these cases.
    absl::Duration input_access_time_by_this_consumer = ProducerInputAccessTime(
        cost_analysis, *device_info, launch_dimensions_fused.num_blocks(),
        producer, analysis_fused, config, fused_consumer);
    VLOG(10) << "  Compute time by consumer: " << compute_time_by_this_consumer;
    VLOG(10) << "  Input access time by consumer: "
             << input_access_time_by_this_consumer;

    exec_time_fused += CombineComputeAndMemoryAccessTime(
        compute_time_by_this_consumer, input_access_time_by_this_consumer,
        config);
  }

  // Multi-output fusion still writes the initial output of the producer.
  // For now assume that the producer's output does not need to be recomputed.
  if (multi_output) {
    exec_time_fused += producer_runtime.write_time;
  }

  return exec_time_fused;
}

/*static*/
GpuPerformanceModel::RunTimes
GpuPerformanceModel::EstimateRunTimesForPriorityFusion(
    const HloInstruction* producer, const GpuHloCostAnalysis* cost_analysis,
    const GpuPerformanceModelOptions& config,
    absl::Span<const HloInstruction* const> fused_consumers,
    bool multi_output) {
  EstimateRunTimeData producer_runtime =
      EstimateRunTimeForInstructionCached(producer, cost_analysis, config);

  absl::Duration time_unfused =
      kKernelLaunchOverhead * (fused_consumers.size() + 1) +
      producer_runtime.exec_time;

  absl::Duration time_fused = kKernelLaunchOverhead * fused_consumers.size();

  for (auto fused_consumer : fused_consumers) {
    VLOG(8) << "Fused consumer: " << fused_consumer->name();

    EstimateRunTimeData consumer_runtime = EstimateRunTimeForInstructionCached(
        fused_consumer, cost_analysis, config);

    time_unfused += consumer_runtime.exec_time;

    time_fused += EstimateRunTimeForFusionCached(
        producer, fused_consumer, producer_runtime, consumer_runtime,
        cost_analysis, config);
  }

  // Multi-output fusion still writes the initial output of the producer.
  // For now assume that the producer's output does not need to be recomputed.
  if (multi_output) {
    time_fused += producer_runtime.write_time;
  }

  if (VLOG_IS_ON(8)) {
    LOG(INFO) << "Consumer count: " << fused_consumers.size();
    LOG(INFO) << "Unfused time: " << time_unfused;
    LOG(INFO) << "Fused time: " << time_fused;
  }

  return {time_unfused, time_fused};
}

/*static*/
GpuPerformanceModel::RunTimes GpuPerformanceModel::EstimateRunTimes(
    const HloInstruction* producer, const GpuHloCostAnalysis* cost_analysis,
    const GpuPerformanceModelOptions& config,
    absl::Span<const HloInstruction* const> fused_consumers,
    bool multi_output) {
  VLOG(8) << "Producer: " << producer->name();
  if (producer->opcode() == HloOpcode::kFusion) {
    VLOG(10) << producer->fused_instructions_computation()->ToString();
  }

  EstimateRunTimeData producer_runtime =
      EstimateRunTimeForInstructionCached(producer, cost_analysis, config);

  absl::Duration time_unfused = EstimateUnfusedExecTime(
      producer, producer_runtime, cost_analysis, config, fused_consumers);

  absl::Duration time_fused =
      EstimateFusedExecTime(producer, producer_runtime, cost_analysis, config,
                            fused_consumers, multi_output);

  int64_t fused_consumer_count = fused_consumers.size();
  float total_producer_utilization = 0;

  for (const HloInstruction* fused_consumer : fused_consumers) {
    float utilization_by_this_consumer = cost_analysis->operand_utilization(
        *fused_consumer, fused_consumer->operand_index(producer));
    total_producer_utilization += utilization_by_this_consumer;
  }

  if (VLOG_IS_ON(8)) {
    LOG(INFO) << "Consumer count: " << fused_consumer_count;
    LOG(INFO) << "Utilization of producer output: "
              << total_producer_utilization;
    LOG(INFO) << "Unfused time: " << time_unfused;
    LOG(INFO) << "Fused time: " << time_fused;
  }

  return {time_unfused, time_fused};
}

/*static*/
void GpuPerformanceModel::RecordEstimatedRunTime(
    HloInstruction* instruction, const GpuHloCostAnalysis* cost_analysis,
    const GpuPerformanceModelOptions& config) {
  DCHECK(Cast<const HloFusionInstruction>(instruction)) << "expected fusion";
  DCHECK(cost_analysis != nullptr) << "expected cost analysis";

  EstimateRunTimeData data =
      EstimateRunTimeForInstructionCached(instruction, cost_analysis, config);
  double cycles = absl::ToDoubleNanoseconds(data.exec_time) *
                  cost_analysis->device_info_->clock_rate_ghz();

  auto gpu_config = instruction->backend_config<GpuBackendConfig>();
  TF_CHECK_OK(gpu_config.status()) << instruction->ToString();
  FusionBackendConfig& backend_config =
      *gpu_config->mutable_fusion_backend_config();
  backend_config.mutable_reification_cost()->set_end_to_end_cycles(cycles);
  TF_CHECK_OK(instruction->set_backend_config(*gpu_config));

  VLOG(8) << "RecordEstimatedRunTime: " << instruction->ToString();
}

}  // namespace gpu
}  // namespace xla
