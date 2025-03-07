/*Copyright 2022 The OpenXLA Authors.

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

#include "xla/service/gpu/ir_emitter_unnested.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/container/inlined_vector.h"
#include "absl/log/check.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/TypeSwitch.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Linker/Linker.h"
#include "mlir/AsmParser/AsmParser.h"  // from @llvm-project
#include "mlir/Dialect/Arith/IR/Arith.h"  // from @llvm-project
#include "mlir/Dialect/Func/Extensions/AllExtensions.h"  // from @llvm-project
#include "mlir/Dialect/Func/IR/FuncOps.h"  // from @llvm-project
#include "mlir/Dialect/GPU/IR/GPUDialect.h"  // from @llvm-project
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"  // from @llvm-project
#include "mlir/Dialect/MemRef/IR/MemRef.h"  // from @llvm-project
#include "mlir/IR/Attributes.h"  // from @llvm-project
#include "mlir/IR/Builders.h"  // from @llvm-project
#include "mlir/IR/BuiltinAttributes.h"  // from @llvm-project
#include "mlir/IR/BuiltinOps.h"  // from @llvm-project
#include "mlir/IR/BuiltinTypes.h"  // from @llvm-project
#include "mlir/IR/Operation.h"  // from @llvm-project
#include "mlir/IR/Value.h"  // from @llvm-project
#include "mlir/IR/ValueRange.h"  // from @llvm-project
#include "mlir/Support/LLVM.h"  // from @llvm-project
#include "mlir/Target/LLVMIR/Dialect/Builtin/BuiltinToLLVMIRTranslation.h"  // from @llvm-project
#include "mlir/Target/LLVMIR/Dialect/LLVMIR/LLVMToLLVMIRTranslation.h"  // from @llvm-project
#include "mlir/Target/LLVMIR/Dialect/NVVM/NVVMToLLVMIRTranslation.h"  // from @llvm-project
#include "mlir/Target/LLVMIR/Dialect/ROCDL/ROCDLToLLVMIRTranslation.h"  // from @llvm-project
#include "mlir/Target/LLVMIR/Export.h"  // from @llvm-project
#include "xla/ffi/api/c_api.h"
#include "xla/ffi/ffi_api.h"
#include "xla/hlo/ir/hlo_casting_utils.h"
#include "xla/hlo/ir/hlo_computation.h"
#include "xla/hlo/ir/hlo_instruction.h"
#include "xla/hlo/ir/hlo_instructions.h"
#include "xla/hlo/ir/hlo_opcode.h"
#include "xla/hlo/ir/hlo_schedule.h"
#include "xla/hlo/utils/hlo_query.h"
#include "xla/layout_util.h"
#include "xla/literal.h"
#include "xla/mlir_hlo/lhlo/IR/lhlo_ops.h"
#include "xla/mlir_hlo/lhlo_gpu/IR/lhlo_gpu_ops.h"
#include "xla/mlir_hlo/mhlo/IR/hlo_ops.h"
#include "xla/mlir_hlo/transforms/gpu_passes.h"
#include "xla/primitive_util.h"
#include "xla/service/buffer_assignment.h"
#include "xla/service/custom_call_status.h"
#include "xla/service/custom_call_target_registry.h"
#include "xla/service/global_device_id.h"
#include "xla/service/gpu/backend_configs.pb.h"
#include "xla/service/gpu/cublas_cudnn.h"
#include "xla/service/gpu/fusions/fusion_emitter.h"
#include "xla/service/gpu/fusions/fusions.h"
#include "xla/service/gpu/fusions/thunk_util.h"
#include "xla/service/gpu/gpu_asm_opts_util.h"
#include "xla/service/gpu/gpu_conv_runner.h"
#include "xla/service/gpu/gpu_executable.h"
#include "xla/service/gpu/gpu_fused_mha_runner.h"
#include "xla/service/gpu/gpu_norm_runner.h"
#include "xla/service/gpu/hlo_fusion_analysis.h"
#include "xla/service/gpu/ir_emission_utils.h"
#include "xla/service/gpu/ir_emitter_context.h"
#include "xla/service/gpu/ir_emitter_nested.h"
#include "xla/service/gpu/kernel_arguments.h"
#include "xla/service/gpu/kernels/custom_kernel.h"
#include "xla/service/gpu/kernels/topk_custom_kernel.h"
#include "xla/service/gpu/launch_dimensions.h"
#include "xla/service/gpu/matmul_utils.h"
#include "xla/service/gpu/nccl_api.h"
#include "xla/service/gpu/nccl_collective_permute_thunk.h"
#include "xla/service/gpu/nccl_collective_thunk.h"
#include "xla/service/gpu/nccl_recv_thunk.h"
#include "xla/service/gpu/nccl_send_thunk.h"
#include "xla/service/gpu/parallel_loop_emitter.h"
#include "xla/service/gpu/runtime/command_buffer_cmd.h"
#include "xla/service/gpu/runtime/command_buffer_cmd_emitter.h"
#include "xla/service/gpu/runtime/command_buffer_thunk.h"
#include "xla/service/gpu/runtime/conditional_thunk.h"
#include "xla/service/gpu/runtime/convolution_thunk.h"
#include "xla/service/gpu/runtime/copy_thunk.h"
#include "xla/service/gpu/runtime/custom_call_thunk.h"
#include "xla/service/gpu/runtime/fft_thunk.h"
#include "xla/service/gpu/runtime/fused_mha_thunk.h"
#include "xla/service/gpu/runtime/gemm_thunk.h"
#include "xla/service/gpu/runtime/infeed_thunk.h"
#include "xla/service/gpu/runtime/kernel_thunk.h"
#include "xla/service/gpu/runtime/nccl_all_gather_thunk.h"
#include "xla/service/gpu/runtime/nccl_all_reduce_thunk.h"
#include "xla/service/gpu/runtime/nccl_all_to_all_thunk.h"
#include "xla/service/gpu/runtime/norm_thunk.h"
#include "xla/service/gpu/runtime/outfeed_thunk.h"
#include "xla/service/gpu/runtime/replica_id_thunk.h"
#include "xla/service/gpu/runtime/send_recv_thunk.h"
#include "xla/service/gpu/runtime/sequential_thunk.h"
#include "xla/service/gpu/runtime/wait_for_streams_thunk.h"
#include "xla/service/gpu/runtime/while_thunk.h"
#include "xla/service/gpu/thunk.h"
#include "xla/service/llvm_ir/buffer_assignment_util.h"
#include "xla/service/llvm_ir/fused_ir_emitter.h"
#include "xla/service/llvm_ir/ir_array.h"
#include "xla/service/llvm_ir/kernel_support_library.h"
#include "xla/service/llvm_ir/llvm_util.h"
#include "xla/service/llvm_ir/sort_util.h"
#include "xla/service/name_uniquer.h"
#include "xla/shape.h"
#include "xla/shape_util.h"
#include "xla/status.h"
#include "xla/status_macros.h"
#include "xla/statusor.h"
#include "xla/stream_executor/device_description.h"
#include "xla/stream_executor/gpu/gpu_blas_lt.h"
#include "xla/translate/hlo_to_mhlo/hlo_utils.h"
#include "xla/translate/mhlo_to_hlo/attribute_exporter.h"
#include "xla/translate/mhlo_to_hlo/location_exporter.h"
#include "xla/translate/mhlo_to_hlo/mlir_hlo_to_hlo.h"
#include "xla/translate/mhlo_to_lhlo_with_xla/mhlo_to_lhlo_with_xla.h"
#include "xla/util.h"
#include "xla/xla_data.pb.h"
#include "tsl/platform/errors.h"
#include "tsl/platform/human_readable_json.h"
#include "tsl/platform/status.h"
#include "tsl/platform/statusor.h"
#include "tsl/protobuf/dnn.pb.h"

#if GOOGLE_CUDA || TF_HIPBLASLT
#include "xla/service/gpu/runtime/gpublas_lt_matmul_thunk.h"
#endif  // GOOGLE_CUDA || TF_HIPBLASLT

#if GOOGLE_CUDA || TENSORFLOW_USE_ROCM
#include "xla/service/gpu/ir_emitter_triton.h"
#include "xla/service/gpu/runtime/cholesky_thunk.h"
#include "xla/service/gpu/runtime/cub_sort_thunk.h"
#include "xla/service/gpu/runtime/triangular_solve_thunk.h"
#endif  // GOOGLE_CUDA || TENSORFLOW_USE_ROCM

namespace xla {
namespace gpu {
namespace {

// Some HLO operations are not implemented as Thunks, and only available when
// XLA:GPU compiled for XLA runtime. However we still depend on emitting thunk
// sequence during compilation, and for unsupported operations we emit
// unreachable thunk, which is not supposed to be executed, and exists only
// during compilation as we transition from thunks to XLA runtime.
//
// Examples: Point-to-point communication operations (Send and Recv) are only
// available as XLA runtime custom calls. API_VERSION_TYPED_FFI custom calls
// are only implemented when executing with XLA runtime.
class UnreachableThunk : public Thunk {
 public:
  UnreachableThunk(mlir::Operation* op, std::string error_message)
      : Thunk(Kind::kKernel, ThunkInfo(op)),
        error_message_(std::move(error_message)) {}

  UnreachableThunk(const UnreachableThunk&) = delete;
  UnreachableThunk& operator=(const UnreachableThunk&) = delete;

  absl::Status Initialize(const InitializeParams& params) final {
    return tsl::errors::Internal(error_message_);
  }

  absl::Status ExecuteOnStream(const ExecuteParams& params) final {
    return tsl::errors::Internal(error_message_);
  }

 private:
  std::string error_message_;
};

absl::StatusOr<xla::gpu::CudnnfMHAKind> AsCudnnfMHAKind(
    mlir::lmhlo_gpu::FusedMhaDagSignature signature) {
  switch (signature) {
    case mlir::lmhlo_gpu::FusedMhaDagSignature::Default:
      return xla::gpu::CudnnfMHAKind::kBmmBmm;
    case mlir::lmhlo_gpu::FusedMhaDagSignature::ScaleBiasMaskSoftmax:
      return xla::gpu::CudnnfMHAKind::kScaleBiasMaskSoftmax;
    case mlir::lmhlo_gpu::FusedMhaDagSignature::ScaleBiasMaskSoftmaxDropout:
      return xla::gpu::CudnnfMHAKind::kScaleBiasMaskSoftmaxDropout;
    case mlir::lmhlo_gpu::FusedMhaDagSignature::ScaleMaskSoftmax:
      return xla::gpu::CudnnfMHAKind::kScaleMaskSoftmax;
    case mlir::lmhlo_gpu::FusedMhaDagSignature::ScaleMaskSoftmaxDropout:
      return xla::gpu::CudnnfMHAKind::kScaleMaskSoftmaxDropout;
    case mlir::lmhlo_gpu::FusedMhaDagSignature::SoftmaxDropout:
      return xla::gpu::CudnnfMHAKind::kSoftmaxDropout;
    case mlir::lmhlo_gpu::FusedMhaDagSignature::Softmax:
      return xla::gpu::CudnnfMHAKind::kSoftmax;
    case mlir::lmhlo_gpu::FusedMhaDagSignature::ScaleBiasSoftmax:
      return xla::gpu::CudnnfMHAKind::kScaleBiasSoftmax;
    case mlir::lmhlo_gpu::FusedMhaDagSignature::ScaleBiasSoftmaxDropout:
      return xla::gpu::CudnnfMHAKind::kScaleBiasSoftmaxDropout;
    default:
      return xla::Internal("Unsupported fused_mha_dag_signature");
  }
}

absl::StatusOr<xla::gpu::CudnnfMHAKind> AsCudnnBackwardfMHAKind(
    mlir::lmhlo_gpu::FusedMhaBackwardDagSignature signature) {
  switch (signature) {
    // backward
    case mlir::lmhlo_gpu::FusedMhaBackwardDagSignature::
        BackwardScaleBiasSoftmax:
      return xla::gpu::CudnnfMHAKind::kBackwardScaleBiasSoftmax;
    case mlir::lmhlo_gpu::FusedMhaBackwardDagSignature::
        BackwardScaleBiasSoftmaxDropout:
      return xla::gpu::CudnnfMHAKind::kBackwardScaleBiasSoftmaxDropout;
    case mlir::lmhlo_gpu::FusedMhaBackwardDagSignature::
        BackwardScaleBiasMaskSoftmax:
      return xla::gpu::CudnnfMHAKind::kBackwardScaleBiasMaskSoftmax;
    case mlir::lmhlo_gpu::FusedMhaBackwardDagSignature::
        BackwardScaleBiasMaskSoftmaxDropout:
      return xla::gpu::CudnnfMHAKind::kBackwardScaleBiasMaskSoftmaxDropout;
      break;
    case mlir::lmhlo_gpu::FusedMhaBackwardDagSignature::BackwardSoftmax:
      return xla::gpu::CudnnfMHAKind::kBackwardSoftmax;
      break;
    case mlir::lmhlo_gpu::FusedMhaBackwardDagSignature::BackwardSoftmaxDropout:
      return xla::gpu::CudnnfMHAKind::kBackwardSoftmaxDropout;
      break;
    case mlir::lmhlo_gpu::FusedMhaBackwardDagSignature::
        BackwardScaleMaskSoftmax:
      return xla::gpu::CudnnfMHAKind::kBackwardScaleMaskSoftmax;
    case mlir::lmhlo_gpu::FusedMhaBackwardDagSignature::
        BackwardScaleMaskSoftmaxDropout:
      return xla::gpu::CudnnfMHAKind::kBackwardScaleMaskSoftmaxDropout;
      break;
    default:
      return xla::Internal("Unsupported fused_mha_backward_dag_signature");
  }
}

}  // namespace

IrEmitterUnnested::IrEmitterUnnested(IrEmitterContext* ir_emitter_context)
    : IrEmitter(ir_emitter_context, /*is_nested=*/false),
      send_recv_events_(std::make_shared<SendRecvAsyncEvents>()),
      elemental_emitter_(*ir_emitter_context, &b_) {}

std::unique_ptr<IrEmitterUnnested> IrEmitterUnnested::Create(
    IrEmitterContext* ir_emitter_context) {
  return std::unique_ptr<IrEmitterUnnested>(
      new IrEmitterUnnested(ir_emitter_context));
}

absl::StatusOr<BufferAllocation::Slice> IrEmitterUnnested::GetAllocationSlice(
    mlir::Value v) {
  return xla::gpu::GetAllocationSlice(v, ir_emitter_context_->allocations(),
                                      nullptr);
}

absl::StatusOr<std::vector<BufferAllocation::Slice>>
IrEmitterUnnested::GetAllocationSlices(mlir::OperandRange operands) {
  std::vector<BufferAllocation::Slice> slices;
  slices.reserve(operands.size());
  for (mlir::Value operand : operands) {
    TF_ASSIGN_OR_RETURN(auto slice, GetAllocationSlice(operand));
    slices.push_back(slice);
  }
  return slices;
}

absl::Status IrEmitterUnnested::EmitUnreachable(mlir::Operation* op,
                                                std::string error_message) {
  AddThunkToThunkSequence(std::unique_ptr<Thunk>(
      new UnreachableThunk(op, std::move(error_message))));
  return absl::OkStatus();
}

absl::Status IrEmitterUnnested::EmitConstant(mlir::Operation* op,
                                             const Literal& literal) {
  auto get_global = mlir::cast<mlir::memref::GetGlobalOp>(op);
  auto module = get_global->getParentOfType<mlir::ModuleOp>();
  auto global = mlir::cast<mlir::memref::GlobalOp>(
      module.lookupSymbol(get_global.getName()));
  TF_ASSIGN_OR_RETURN(DenseDataIntermediate content,
                      LiteralToXlaFormat(literal));

  int element_bytes = primitive_util::ByteWidth(literal.shape().element_type());
  TF_RET_CHECK(content.span().size() % element_bytes == 0);
  // Treat int4 constant as int8 constant with half the number of elements.
  int num_elements = content.span().size() / element_bytes;

  int64_t arg_index =
      global->getAttrOfType<mlir::IntegerAttr>("lmhlo.alloc").getInt();
  int allocation_index = ir_emitter_context_->allocations()[arg_index]->index();

  ir_emitter_context_->emit_constant(num_elements, element_bytes,
                                     global.getSymName(), allocation_index,
                                     std::move(content), &b_);
  return absl::OkStatus();
}

absl::Status IrEmitterUnnested::EmitConstant(
    const HloConstantInstruction* instr) {
  TF_ASSIGN_OR_RETURN(DenseDataIntermediate content,
                      LiteralToXlaFormat(instr->literal()));

  int element_bytes =
      primitive_util::ByteWidth(instr->literal().shape().element_type());
  TF_RET_CHECK(content.span().size() % element_bytes == 0);
  // Treat int4 constant as int8 constant with half the number of elements.
  int num_elements = content.span().size() / element_bytes;

  std::string global_name = llvm_ir::ConstantHloToGlobalName(*instr);
  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice slice,
                      GetAllocationSliceForHlo(instr, {}));

  ir_emitter_context_->emit_constant(num_elements, element_bytes, global_name,
                                     slice.index(), std::move(content), &b_);
  return absl::OkStatus();
}

static ConditionalThunkConfig GetConditionalThunkConfig(
    mlir::lmhlo::CaseOp op, std::vector<ThunkSequence> branch_thunk_sequences) {
  ConditionalThunkConfig config;
  config.branch_index_is_bool = op.getIndex()
                                    .getType()
                                    .cast<mlir::ShapedType>()
                                    .getElementType()
                                    .isInteger(
                                        /*width=*/1);
  config.branch_count = op.getBranches().size();
  // Pass nullptr as the HloInstruction* to the branch_thunks
  // constructors because these SequentialThunks are logically "part of"
  // this ConditionalThunk, and shouldn't be profiled separately from it.
  config.branch_thunks.reserve(branch_thunk_sequences.size());
  for (auto& branch_thunk_sequence : branch_thunk_sequences) {
    config.branch_thunks.emplace_back(new SequentialThunk(
        Thunk::ThunkInfo(op), std::move(branch_thunk_sequence)));
  }
  return config;
}

absl::Status IrEmitterUnnested::EmitConditional(
    mlir::Operation* op,
    const absl::flat_hash_map<const mlir::Operation*, const HloInstruction*>&
        hlo_for_lmhlo) {
  if (ir_emitter_context_->emit_ir_from_hlo())
    return EmitConditional(hlo_for_lmhlo.at(op));

  auto conditional = mlir::cast<mlir::lmhlo::CaseOp>(op);

  std::vector<ThunkSequence> branch_thunks;

  int branch_count = conditional.getBranches().size();
  branch_thunks.reserve(branch_count);

  for (int j = 0; j < branch_count; ++j) {
    mlir::Region* branch_computation = &conditional.getBranches()[j];
    auto ir_emitter = IrEmitterUnnested::Create(ir_emitter_context_);
    TF_RETURN_IF_ERROR(
        ir_emitter->EmitLmhloRegion(branch_computation, hlo_for_lmhlo));
    branch_thunks.push_back(std::move(*ir_emitter->ConsumeThunkSequence()));
  }

  ConditionalThunkConfig config =
      GetConditionalThunkConfig(conditional, std::move(branch_thunks));

  TF_ASSIGN_OR_RETURN(auto slice, GetAllocationSlice(conditional.getIndex()));
  AddThunkToThunkSequence(std::unique_ptr<Thunk>(new ConditionalThunk(
      Thunk::ThunkInfo::WithProfileAnnotation(op), std::move(config), slice)));
  return absl::OkStatus();
}

static ConditionalThunkConfig GetConditionalThunkConfig(
    const HloInstruction* instr,
    std::vector<ThunkSequence> branch_thunk_sequences) {
  ConditionalThunkConfig config;
  config.branch_index_is_bool =
      instr->operand(0)->shape().element_type() == PRED;
  config.branch_count = instr->branch_count();
  config.branch_thunks.reserve(config.branch_count);
  for (auto& branch_thunk_sequence : branch_thunk_sequences) {
    config.branch_thunks.emplace_back(
        new SequentialThunk(Thunk::ThunkInfo::WithProfileAnnotation(instr),
                            std::move(branch_thunk_sequence)));
  }
  return config;
}

Status IrEmitterUnnested::EmitConditional(const HloInstruction* instr) {
  std::vector<ThunkSequence> branch_thunks;
  branch_thunks.reserve(instr->branch_count());

  for (auto comp : instr->branch_computations()) {
    auto ir_emitter = IrEmitterUnnested::Create(ir_emitter_context_);
    TF_RETURN_IF_ERROR(ir_emitter->EmitHloComputation(comp));
    branch_thunks.push_back(std::move(*ir_emitter->ConsumeThunkSequence()));
  }

  ConditionalThunkConfig config =
      GetConditionalThunkConfig(instr, std::move(branch_thunks));

  TF_ASSIGN_OR_RETURN(auto slice,
                      GetAllocationSliceForHlo(instr->operand(0), {}));
  AddThunkToThunkSequence(std::unique_ptr<Thunk>(
      new ConditionalThunk(Thunk::ThunkInfo::WithProfileAnnotation(instr),
                           std::move(config), slice)));
  return OkStatus();
}

llvm::Value* IrEmitterUnnested::CreateLoad(llvm::Value* address,
                                           llvm::Type* data_type,
                                           int alignment_bytes) {
  int data_bytes = data_type->getPrimitiveSizeInBits() /
                   primitive_util::BitWidth(PrimitiveType::U8);
  if (alignment_bytes == 0) {
    return b_.CreateLoad(data_type, address);
  }

  int alignment_bitwidth =
      alignment_bytes * primitive_util::BitWidth(PrimitiveType::U8);

  llvm::Value* output = llvm::ConstantInt::get(data_type, 0);
  for (int offset_bytes = 0; offset_bytes < data_bytes;
       offset_bytes += alignment_bytes) {
    llvm::Value* offset_address = b_.CreateConstInBoundsGEP1_32(
        b_.getInt8Ty(), address, offset_bytes, "offset_address");
    llvm::Value* partial_value = b_.CreateLoad(b_.getIntNTy(alignment_bitwidth),
                                               offset_address, "partial_value");
    llvm::Value* zextd =
        b_.CreateZExt(partial_value, output->getType(), "partial_value_zextd");
    llvm::Value* shifted = b_.CreateShl(
        zextd, llvm::ConstantInt::get(b_.getInt32Ty(), offset_bytes),
        "partial_input_shifted");
    output = b_.CreateAdd(output, shifted, "output_updated");
  }
  return output;
}

void IrEmitterUnnested::CreateStore(llvm::Value* data, llvm::Value* address,
                                    int alignment_bytes) {
  int data_bytes = data->getType()->getPrimitiveSizeInBits() /
                   primitive_util::BitWidth(PrimitiveType::U8);
  CHECK_GE(data_bytes, alignment_bytes);
  if (alignment_bytes == 0) {
    b_.CreateStore(data, address);
    return;
  }

  int alignment_bitwidth =
      alignment_bytes * primitive_util::BitWidth(PrimitiveType::U8);

  for (int offset_bytes = 0; offset_bytes < data_bytes;
       offset_bytes += alignment_bytes) {
    llvm::Value* offset_address = b_.CreateConstInBoundsGEP1_32(
        b_.getInt8Ty(), address, offset_bytes, "offset_address");
    llvm::Value* shifted_partial = b_.CreateTrunc(
        b_.CreateLShr(data,
                      llvm::ConstantInt::get(b_.getInt32Ty(), offset_bytes)),
        b_.getIntNTy(alignment_bitwidth), "truncated_value");
    b_.CreateStore(shifted_partial, offset_address);
  }
}

// Input = {dynamic array(with dynamic dimension meta data at the end)}
// Output = {static array, dynamic_dim0, dynamic_dim1}
absl::Status IrEmitterUnnested::EmitPadToStatic(
    const HloCustomCallInstruction* instr) {
  int unroll_factor = 1;
  std::string ir_name = std::string(instr->name());

  const Shape& input_shape = instr->operand(0)->shape();

  LaunchDimensions launch_dimensions = CalculateLaunchDimensions(
      input_shape, ir_emitter_context_->gpu_device_info(), {unroll_factor});
  std::vector<llvm_ir::IrArray> input_arrays;
  std::vector<llvm_ir::IrArray> output_arrays;
  TF_ASSIGN_OR_RETURN(std::tie(input_arrays, output_arrays),
                      BuildKernelThunkForNonFusionOp(instr, instr->operands(),
                                                     launch_dimensions));

  CHECK_EQ(output_arrays.size(), 0);
  const llvm_ir::IrArray source_array = input_arrays[0];
  const llvm_ir::IrArray output_array = input_arrays[1];
  auto output_dim_arrays =
      absl::Span<const llvm_ir::IrArray>(input_arrays).subspan(2);

  llvm::Type* index_ty =
      GetIndexTypeForKernel(instr, launch_dimensions.launch_bound(), &b_);

  // pseudo code for PadToStatic on a 2d array
  //   int* source_array = input[0];
  //   int* dest_array = output[0];
  llvm::Value* source_buffer = source_array.GetBasePointer();

  // TODO(jurahul): input_shape here is the static shape of the input (which has
  // a dynamic shape in XLA). Currently, we are mapping that to a static shaped
  // memref. When we change that to a more appropriate representation in MLIR,
  // fix this code to correctly deduce the static shape backing the dynamically
  // shaped memref.
  int64_t raw_data_size = ShapeUtil::ByteSizeOf(input_shape);

  //   int* dyn_dim0_size = source_array + meta_data_offset;
  //   int* dyn_dim1_size = source_array + meta_data_offset + sizeof(int);
  std::vector<llvm::Value*> dynamic_dims;
  int alignment = raw_data_size % sizeof(int32_t);
  std::vector<ShapeUtil::IndexedShape> output_shapes =
      ShapeUtil::GetLeafShapes(instr->shape());

  for (int64_t i = 1; i < output_shapes.size(); ++i) {
    // Dynamic size of each dimension is attached at the end of the source
    // array(operand(0)). We need to extract these value.
    const Shape& dim_shape = output_shapes[i].shape;
    TF_RET_CHECK(Shape::Equal()(dim_shape, ShapeUtil::MakeScalarShape(S32)));

    const int64_t dim_index = i - 1;
    llvm::Value* metadata = b_.CreateConstInBoundsGEP1_32(
        b_.getInt8Ty(), source_buffer,
        raw_data_size + dim_index * sizeof(int32_t));
    llvm::Value* dyn_dim_size =
        CreateLoad(metadata, b_.getInt32Ty(), alignment);
    dynamic_dims.push_back(dyn_dim_size);
  }

  // only one thread need to store the dynamic index
  //   int thread_id = GetThreadId();
  //   int block_id = GetBlockId();
  //   if (thread_id == 0 && block_id == 0) {
  //     *output[1] = *dyn_dim0_size;
  //     *output[2] = *dyn_dim1_size;
  //   }
  KernelSupportLibrary{&b_}.If("is_thread_0", IsBlock0Thread0(&b_), [&] {
    for (int64_t i = 1; i < output_shapes.size(); ++i) {
      const int64_t dim_index = i - 1;
      llvm::Value* dest_dim_size_address =
          output_dim_arrays[dim_index].GetBasePointer();
      // output[i] stores dynamic_dim_(i-1)
      CreateStore(dynamic_dims[dim_index], dest_dim_size_address, alignment);
    }
  });

  //     int dyn_element_total = 1;
  //     dyn_element_total *= *dyn_dim0_size;
  //     dyn_element_total *= *dyn_dim1_size;
  llvm::Value* dyn_element_total = llvm::ConstantInt::get(index_ty, 1);
  for (llvm::Value* dynamic_dim : dynamic_dims) {
    dyn_element_total =
        b_.CreateMul(dyn_element_total,
                     b_.CreateIntCast(dynamic_dim, dyn_element_total->getType(),
                                      /*isSigned=*/true),
                     /*Name=*/"dyn_element_total_pad");
  }

  //   linear_index = block_id * threads_per_block + thread_id;
  //   if (linear_index < max_num_element) {
  //     Index static_index =
  //         delinerized(linerized_index, static_dim0_size, static_dim1_size);
  //     if (linerized_index < dyn_element_total) {
  //       Index dyn_index =
  //           delinerized(linerized_index, *dyn_dim0_size, *dyn_dim1_size);
  //       dest_array[dyn_index.dim0][dyn_index.dim1] =
  //           source_array[static_index.dim0][static_index.dim1];
  //     }
  //   }
  llvm_ir::BodyEmitter body_generator =
      [&](const llvm_ir::IrArray::Index& array_index) -> absl::Status {
    llvm::Value* linearIndex =
        array_index.Linearize(input_shape.dimensions(), &b_);
    auto if_in_dyn_bounds = llvm_ir::EmitIfThenElse(
        b_.CreateICmpULT(linearIndex, dyn_element_total),
        llvm_ir::IrName(ir_name, "in_dyn_bounds"), &b_, false);
    // Set IR builder insertion point to the body of the if structure.
    llvm_ir::SetToFirstInsertPoint(if_in_dyn_bounds.true_block, &b_);
    llvm_ir::IrArray::Index dyn_index(linearIndex, input_shape,
                                      absl::MakeSpan(dynamic_dims), &b_);
    output_array.EmitWriteArrayElement(
        dyn_index,
        source_array.EmitReadArrayElement(array_index, &b_, /*name=*/""), &b_,
        /*use_linear_index=*/false);
    return absl::OkStatus();
  };

  const Shape& data_shape = instr->shape().tuple_shapes(0);
  TF_RETURN_IF_ERROR(ParallelLoopEmitter(body_generator, data_shape,
                                         launch_dimensions, &b_,
                                         {unroll_factor})
                         .EmitLoop(ir_name, index_ty));
  return absl::OkStatus();
}

// Input = {dynamic array(with dynamic dimension meta data at the end)}
// Output = {static array, dynamic_dim0, dynamic_dim1}
absl::Status IrEmitterUnnested::EmitSliceToDynamic(
    const HloCustomCallInstruction* instr) {
  // TODO(jurahul): Create an op to represent SliceToDynamic.
  int unroll_factor = 1;
  std::string ir_name = std::string(instr->name());

  const Shape& input_shape = instr->operand(0)->shape();

  LaunchDimensions launch_dimensions = CalculateLaunchDimensions(
      input_shape, ir_emitter_context_->gpu_device_info(), {unroll_factor});
  llvm::Type* index_ty =
      GetIndexTypeForKernel(instr, launch_dimensions.launch_bound(), &b_);
  std::vector<llvm_ir::IrArray> input_arrays, output_arrays;
  TF_ASSIGN_OR_RETURN(std::tie(input_arrays, output_arrays),
                      BuildKernelThunkForNonFusionOp(instr, instr->operands(),
                                                     launch_dimensions));

  const Shape& data_shape = ShapeUtil::MakeStaticShape(instr->shape());
  TF_RET_CHECK(data_shape.IsArray());

  // TODO(jurahul): data_shape here is the static shape of the output (which has
  // a dynamic shape in XLA). Currently, we are mapping that to a static shaped
  // memref. When we change that to a more appropriate representation in MLIR,
  // fix this code to correctly deduce the static shape backing the dynamically
  // shaped memref.

  // calculate the location where metadata needs to be inserted
  //   int* dyn_dim0_size = dest_array + meta_data_offset;
  //   int* dyn_dim1_size = dest_array + meta_data_offset + sizeof(int);
  int32_t raw_data_size = ShapeUtil::ByteSizeOf(data_shape);

  // pseudo code for sliceToDynamic on a 2d array
  //   int* source_array = input[0];
  //   int* dest_array = output[0];
  const llvm_ir::IrArray data_array = input_arrays.back();
  llvm::Value* dest_buffer = data_array.GetBasePointer();

  // Load dynamic dimensions from memory.
  std::vector<llvm::Value*> dynamic_dims;
  int alignment = raw_data_size % sizeof(int32_t);
  for (int64_t i = 1; i < instr->operand_count(); ++i) {
    llvm::Value* source_buffer = input_arrays[i].GetBasePointer();
    llvm::Type* source_buffer_pointee_type =
        input_arrays[i].GetBasePointeeType();
    llvm::LoadInst* dyn_dim_size =
        Load(source_buffer_pointee_type, source_buffer, "dyn_dim_size");
    dynamic_dims.push_back(dyn_dim_size);
  }

  // only one thread need to store the dynamic index
  //   int thread_id = GetThreadId();
  //   int block_id = GetBlockId();
  //   if (thread_id == 0 && block_id == 0) {
  //     *dyn_dim0_size = *output[1];
  //     *dyn_dim1_size = *output[2];
  //   }
  KernelSupportLibrary{&b_}.If("is_thread_0", IsBlock0Thread0(&b_), [&] {
    for (int64_t i = 1; i < instr->operand_count(); ++i) {
      const int64_t dim_index = i - 1;
      llvm::Value* metadata = b_.CreateConstInBoundsGEP1_32(
          b_.getInt8Ty(), dest_buffer,
          raw_data_size + dim_index * sizeof(int32_t));
      // output[i] stores dynamic_dim_(i-1)
      CreateStore(dynamic_dims[dim_index], metadata, alignment);
    }
  });

  //     int dyn_element_total = 1;
  //     dyn_element_total *= dyn_dim0_size;
  //     dyn_element_total *= dyn_dim1_size;
  llvm::Value* dyn_element_total = llvm::ConstantInt::get(index_ty, 1);
  for (llvm::Value* dynamic_dim : dynamic_dims) {
    dyn_element_total =
        b_.CreateMul(dyn_element_total,
                     b_.CreateIntCast(dynamic_dim, dyn_element_total->getType(),
                                      /*isSigned=*/true),
                     /*Name=*/"dyn_element_total_slice");
  }

  //   linear_index = block_id * threads_per_block + thread_id;
  //   if (linear_index < max_num_element) {
  //     Index static_index =
  //         delinerized(linerized_index, static_dim0_size, static_dim1_size);
  //     if (linerized_index < dyn_element_total) {
  //       Index dyn_index =
  //           delinerized(linerized_index, *dyn_dim0_size, *dyn_dim1_size);
  //       dest_array[static_index.dim0][static_index.di] =
  //           source_array[dyn_index.dim0][dyn_index.dim1];
  //     }
  //   }
  llvm_ir::BodyEmitter body_generator =
      [&](const llvm_ir::IrArray::Index& array_index) -> absl::Status {
    llvm::Value* linearIndex =
        array_index.Linearize(input_shape.dimensions(), &b_);
    auto if_in_dyn_bounds = llvm_ir::EmitIfThenElse(
        b_.CreateICmpULT(linearIndex, dyn_element_total),
        llvm_ir::IrName(ir_name, "in_dyn_bounds"), &b_, false);
    // Set IR builder insertion point to the body of the if structure.
    llvm_ir::SetToFirstInsertPoint(if_in_dyn_bounds.true_block, &b_);
    llvm_ir::IrArray::Index dyn_index(linearIndex, input_shape,
                                      absl::MakeSpan(dynamic_dims), &b_);

    data_array.EmitWriteArrayElement(
        array_index,
        input_arrays[0].EmitReadArrayElement(dyn_index, &b_, /*name=*/"",
                                             /*use_linear_index=*/false),
        &b_);
    return absl::OkStatus();
  };

  TF_RETURN_IF_ERROR(ParallelLoopEmitter(body_generator, data_shape,
                                         launch_dimensions, &b_,
                                         {unroll_factor})
                         .EmitLoop(ir_name, index_ty));
  return absl::OkStatus();
}

absl::Status IrEmitterUnnested::EmitCommandBufferThunk(
    const HloInstruction* instr) {
  // Spawn a new IrEmitterUnnested to emit thunks for the command buffer
  // computation. Then convert emitted thunks to a sequence of CommandBufferCmd.
  // The resulting thunk added to the thunk sequence is a CommandBufferThunk.
  // Thunks emitted from the command buffer computation are discarded.
  DCHECK_EQ(instr->called_computations().size(), 1);
  const HloComputation* command_buffer = instr->called_computations().front();
  auto ir_emitter = IrEmitterUnnested::Create(ir_emitter_context_);
  TF_RETURN_IF_ERROR(ir_emitter->EmitHloComputation(command_buffer));
  std::unique_ptr<ThunkSequence> thunk_sequence =
      ir_emitter->ConsumeThunkSequence();

  // Linearize all commands in a sequence by forcing barriers between all
  // recorded commands. This guarantees that we execute all device operations
  // in the exact same order as a thunk sequence.
  bool force_barriers = !ir_emitter_context_->debug_options()
                             .xla_gpu_graph_enable_concurrent_region();

  TF_ASSIGN_OR_RETURN(CommandBufferCmdSequence cmd_sequence,
                      ConvertToCommands(*thunk_sequence, force_barriers));
  AddThunkToThunkSequence(std::make_unique<CommandBufferThunk>(
      std::move(cmd_sequence), Thunk::ThunkInfo::WithProfileAnnotation(instr),
      std::move(*thunk_sequence)));

  return absl::OkStatus();
}

absl::Status IrEmitterUnnested::EmitConvolutionThunk(
    const HloCustomCallInstruction* instr) {
  std::vector<BufferAllocation::Slice> operand_slices;
  operand_slices.reserve(instr->operand_count());
  for (const HloInstruction* operand : instr->operands()) {
    TF_ASSIGN_OR_RETURN(BufferAllocation::Slice slice,
                        GetAllocationSliceForHlo(operand, {}));
    operand_slices.push_back(slice);
  }

  // The first and the last element in the result tuple for a convolution are
  // always the result and the scratch buffer. It may have auxiliary results in
  // addition to the main result.
  std::vector<BufferAllocation::Slice> result_slices;
  for (int i = 0; i < instr->shape().tuple_shapes_size() - 1; i++) {
    TF_ASSIGN_OR_RETURN(BufferAllocation::Slice result_slice,
                        GetAllocationSliceForHlo(instr, {i}));
    result_slices.push_back(result_slice);
  }

  TF_ASSIGN_OR_RETURN(CudnnConvKind kind, GetCudnnConvKind(instr));
  TF_ASSIGN_OR_RETURN(auto gpu_config,
                      instr->backend_config<GpuBackendConfig>());
  const CudnnConvBackendConfig& backend_config =
      gpu_config.cudnn_conv_backend_config();
  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice scratch_slice,
                      GetAllocationSliceForHlo(
                          instr, {instr->shape().tuple_shapes_size() - 1}));
  GpuConvDescriptor descriptor = {kind,
                                  backend_config,
                                  instr->operand(0)->shape(),
                                  instr->operand(1)->shape(),
                                  instr->shape().tuple_shapes(0),
                                  static_cast<size_t>(scratch_slice.size()),
                                  instr->window(),
                                  instr->convolution_dimension_numbers(),
                                  instr->feature_group_count()};

  TF_ASSIGN_OR_RETURN(GpuConvConfig config, GetGpuConvConfig(descriptor, ""));
  AddThunkToThunkSequence(std::make_unique<ConvolutionThunk>(
      Thunk::ThunkInfo::WithProfileAnnotation(instr), std::move(config),
      std::move(operand_slices), std::move(result_slices), scratch_slice));
  return OkStatus();
}

absl::Status IrEmitterUnnested::EmitConvolutionThunk(mlir::Operation* op) {
  using mlir::dyn_cast;
  using mlir::lmhlo_gpu::Activation;
  using mlir::lmhlo_gpu::ConvBackwardFilterOp;
  using mlir::lmhlo_gpu::ConvBackwardInputOp;
  using mlir::lmhlo_gpu::ConvForwardFusedOp;
  using mlir::lmhlo_gpu::ConvForwardFusedSideInputOp;
  using mlir::lmhlo_gpu::ConvForwardGraphOp;
  using mlir::lmhlo_gpu::ConvForwardOp;

  std::vector<BufferAllocation::Slice> operand_slices, result_slices;
  int32_t n_aux_outputs = 0;
  if (auto conv = dyn_cast<ConvForwardGraphOp>(op)) {
    n_aux_outputs = conv.getNAuxOutputs();
  }
  int64_t num_operands = op->getNumOperands();
  operand_slices.reserve(num_operands - n_aux_outputs - 2);

  // The operands describe inputs, the main result of the convolution, the
  // scratch workspace and n_aux_outputs return values of ops fused into the
  // convolution.
  for (mlir::Value operand : op->getOperands().drop_back(2 + n_aux_outputs)) {
    TF_ASSIGN_OR_RETURN(auto slice, GetAllocationSlice(operand));
    operand_slices.push_back(slice);
  }

  result_slices.reserve(1 + n_aux_outputs);
  for (mlir::Value result : op->getOperands()
                                .drop_front(num_operands - n_aux_outputs - 2)
                                .drop_back(1)) {
    TF_ASSIGN_OR_RETURN(auto slice, GetAllocationSlice(result));
    result_slices.push_back(slice);
  }
  mlir::Value scratch_result = op->getOperand(num_operands - 1);
  TF_ASSIGN_OR_RETURN(auto scratch_slice, GetAllocationSlice(scratch_result));

  auto apply_layout = [](const Shape& shape,
                         mlir::ArrayRef<int64_t> minor_to_major) {
    return ShapeUtil::MakeShapeWithDenseLayout(
        shape.element_type(), shape.dimensions(), minor_to_major);
  };

  GpuConvDescriptor descriptor;

  auto fill_conv_descriptor = [&](auto op) {
    descriptor.operand0_shape =
        apply_layout(GetShape(op->getOperand(0)),
                     op.getBackendConfig().getOperand_0Layout());
    descriptor.operand1_shape =
        apply_layout(GetShape(op->getOperand(1)),
                     op.getBackendConfig().getOperand_1Layout());
    descriptor.result_shape =
        apply_layout(GetShape(op->getOperand(num_operands - n_aux_outputs - 2)),
                     op.getBackendConfig().getResultLayout());
    descriptor.dnums = ConvertConvDimensionNumbers(op.getDimensionNumbers());
    descriptor.scratch_size = scratch_slice.size();
    mlir::DenseIntElementsAttr window_strides = op.getWindowStrides().value();
    mlir::DenseIntElementsAttr padding = op.getPadding().value();
    mlir::DenseIntElementsAttr lhs_dilation = op.getLhsDilation().value();
    mlir::DenseIntElementsAttr rhs_dilation = op.getRhsDilation().value();
    mlir::DenseElementsAttr window_reversal = op.getWindowReversal().value();
    for (auto index : llvm::seq<int>(0, window_strides.getNumElements())) {
      WindowDimension* dim = descriptor.window.add_dimensions();
      // Window size for a convolution is the same as the kernel size.
      // Kernel size of the convolution is operand1_shape. We need to look at
      // the convolution dimension numbers kernel spatial dimensions to get
      // the window size.
      int kernel_dim = descriptor.dnums.kernel_spatial_dimensions(index);
      dim->set_size(descriptor.operand0_shape.dimensions(kernel_dim));
      dim->set_stride(window_strides.getValues<int64_t>()[index]);
      dim->set_padding_low(padding.getValues<int64_t>()[index]);
      dim->set_padding_high(padding.getValues<int64_t>()[index]);
      dim->set_base_dilation(lhs_dilation.getValues<int64_t>()[index]);
      dim->set_window_dilation(rhs_dilation.getValues<int64_t>()[index]);
      dim->set_window_reversal(window_reversal.getValues<bool>()[index]);
    }
    descriptor.feature_group_count = op.getFeatureGroupCount();
    {
      auto* algorithm = descriptor.backend_config.mutable_algorithm();
      algorithm->set_algo_id(op.getBackendConfig().getAlgorithm());
      algorithm->set_math_type(op.getBackendConfig().getTensorOpsEnabled()
                                   ? se::dnn::AlgorithmProto::TENSOR_OP_MATH
                                   : se::dnn::AlgorithmProto::DEFAULT_MATH);
      for (int i = 0; i < op.getBackendConfig().getKnobIds().size(); ++i) {
        // N.B. tuning_knobs is a map rather than a repeated field, so this
        // doesn't require reserving space up front.
        (*algorithm
              ->mutable_tuning_knobs())[op.getBackendConfig().getKnobIds()[i]] =
            op.getBackendConfig().getKnobValues()[i];
      }
      algorithm->set_is_cudnn_frontend(
          op.getBackendConfig().getIsCudnnFrontend());
      auto workspace_size = op.getBackendConfig().getWorkspaceSize();
      if (workspace_size >= 0) {
        algorithm->mutable_workspace_size()->set_value(workspace_size);
      }
    }
    descriptor.backend_config.set_conv_result_scale(
        op.getResultScale().convertToDouble());
    descriptor.backend_config.set_reordered_int8_nchw_vect(
        op.getBackendConfig().getIsCudnnReorderedInt8());
  };

  auto set_activation_mode = [&](auto op) -> absl::Status {
    TF_ASSIGN_OR_RETURN(stream_executor::dnn::ActivationMode activation_mode,
                        ConvertConvActivationMode(op.getActivationMode()));
    descriptor.backend_config.set_activation_mode(activation_mode);
    return absl::OkStatus();
  };

  if (auto conv = dyn_cast<ConvForwardOp>(op)) {
    descriptor.kind = CudnnConvKind::kForward;
    fill_conv_descriptor(conv);
  } else if (auto conv = dyn_cast<ConvBackwardInputOp>(op)) {
    descriptor.kind = CudnnConvKind::kBackwardInput;
    fill_conv_descriptor(conv);
  } else if (auto conv = dyn_cast<ConvBackwardFilterOp>(op)) {
    descriptor.kind = CudnnConvKind::kBackwardFilter;
    fill_conv_descriptor(conv);
  } else if (auto conv = dyn_cast<ConvForwardGraphOp>(op)) {
    descriptor.kind = CudnnConvKind::kForwardGraph;
    fill_conv_descriptor(conv);
    descriptor.backend_config.set_serialized_graph(
        conv.getSerializedGraph().data());
  } else if (auto conv = dyn_cast<ConvForwardFusedOp>(op)) {
    descriptor.kind = CudnnConvKind::kForwardActivation;
    fill_conv_descriptor(conv);
    TF_RETURN_IF_ERROR(set_activation_mode(conv));
    descriptor.backend_config.set_leakyrelu_alpha(
        conv.getLeakyreluAlpha().convertToDouble());
  } else if (auto conv = dyn_cast<ConvForwardFusedSideInputOp>(op)) {
    descriptor.kind = CudnnConvKind::kForwardActivation;
    fill_conv_descriptor(conv);
    TF_RETURN_IF_ERROR(set_activation_mode(conv));
    descriptor.backend_config.set_side_input_scale(
        conv.getSideInputScale().convertToDouble());
  } else {
    return Internal("EmitConvolutionThunk: Unexpected operation");
  }
  TF_ASSIGN_OR_RETURN(GpuConvConfig config, GetGpuConvConfig(descriptor, ""));
  AddThunkToThunkSequence(std::make_unique<ConvolutionThunk>(
      Thunk::ThunkInfo::WithProfileAnnotation(op), std::move(config),
      std::move(operand_slices), std::move(result_slices), scratch_slice));
  return absl::OkStatus();
}

absl::Status IrEmitterUnnested::EmitGemmThunk(mlir::Operation* op) {
  auto gemm = mlir::dyn_cast<mlir::lmhlo_gpu::GEMMOp>(op);
  TF_RET_CHECK(gemm != nullptr);

  TF_ASSIGN_OR_RETURN(auto a, GetAllocationSlice(gemm.getA()));
  TF_ASSIGN_OR_RETURN(auto b, GetAllocationSlice(gemm.getB()));
  TF_ASSIGN_OR_RETURN(auto c, GetAllocationSlice(gemm.getC()));
  bool deterministic_ops =
      ir_emitter_context_->debug_options().xla_gpu_deterministic_ops();

  TF_ASSIGN_OR_RETURN(GemmConfig config, GemmConfig::For(gemm));
  auto thunk = std::make_unique<GemmThunk>(
      Thunk::ThunkInfo::WithProfileAnnotation(op), std::move(config), a, b, c,
      std::nullopt, deterministic_ops);

  AddThunkToThunkSequence(std::move(thunk));
  return absl::OkStatus();
}

absl::Status IrEmitterUnnested::EmitGemmThunk(
    const HloCustomCallInstruction* instr) {
  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice a,
                      GetAllocationSliceForHlo(instr->operand(0), {}));
  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice b,
                      GetAllocationSliceForHlo(instr->operand(1), {}));

  // Result of a legacy cuBLAS custom call can be a tuple if we explicitly
  // allocate workspace buffer in HLO. If result is an array, it means that
  // workspace is not available, and cuBLAS will allocate its own workspace.
  BufferAllocation::Slice c;
  std::optional<BufferAllocation::Slice> workspace;

  if (instr->shape().IsArray()) {
    TF_ASSIGN_OR_RETURN(c, GetAllocationSliceForHlo(instr, {}));
  } else {
    TF_ASSIGN_OR_RETURN(c, GetAllocationSliceForHlo(instr, {0}));
    TF_ASSIGN_OR_RETURN(workspace, GetAllocationSliceForHlo(instr, {1}));
  }

  bool deterministic_ops =
      ir_emitter_context_->debug_options().xla_gpu_deterministic_ops();

  TF_ASSIGN_OR_RETURN(
      GemmConfig config,
      GemmConfig::For(static_cast<const HloInstruction*>(instr)));
  auto thunk = std::make_unique<GemmThunk>(
      Thunk::ThunkInfo::WithProfileAnnotation(instr), std::move(config), a, b,
      c, workspace, deterministic_ops);
  AddThunkToThunkSequence(std::move(thunk));
  return absl::OkStatus();
}

#if GOOGLE_CUDA || TF_HIPBLASLT

absl::Status IrEmitterUnnested::EmitCublasLtMatmulThunk(
    const HloCustomCallInstruction* instr) {
  TF_ASSIGN_OR_RETURN(const auto gpu_config,
                      instr->backend_config<xla::gpu::GpuBackendConfig>());
  xla::gpu::GemmBackendConfig config = gpu_config.gemm_backend_config();
  xla::gpu::GemmBackendConfig_Epilogue epilogue = config.epilogue();

  TF_ASSIGN_OR_RETURN(bool has_vector_bias,
                      xla::gpu::gpublas_lt::EpilogueAddsVectorBias(epilogue));
  bool has_matrix_bias = config.beta() != 0;

  TF_RET_CHECK(instr->operand_count() ==
               2 + int{has_matrix_bias} + int{has_vector_bias});

  TF_ASSIGN_OR_RETURN(
      bool has_aux_output,
      xla::gpu::gpublas_lt::EpilogueHasAuxiliaryOutput(epilogue));
  xla::ShapeIndex output_index =
      has_aux_output ? xla::ShapeIndex{0} : xla::ShapeIndex{};

  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice a,
                      GetAllocationSliceForHlo(instr->operand(0)));
  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice b,
                      GetAllocationSliceForHlo(instr->operand(1)));
  BufferAllocation::Slice c;
  if (has_matrix_bias) {
    TF_ASSIGN_OR_RETURN(c, GetAllocationSliceForHlo(instr->operand(2)));
  } else {
    TF_ASSIGN_OR_RETURN(c, GetAllocationSliceForHlo(instr, output_index));
  }
  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice d,
                      GetAllocationSliceForHlo(instr, output_index));

  BufferAllocation::Slice bias;
  if (has_vector_bias) {
    TF_ASSIGN_OR_RETURN(bias, GetAllocationSliceForHlo(
                                  instr->operand(has_matrix_bias ? 3 : 2)));
  }

  BufferAllocation::Slice aux;
  if (has_aux_output) {
    TF_ASSIGN_OR_RETURN(aux, GetAllocationSliceForHlo(instr, {1}));
  }

  TF_ASSIGN_OR_RETURN(
      auto gemm_config,
      GemmConfig::For(static_cast<const HloInstruction*>(instr)));

  // Use the first algorithm by default (i.e. fastest according to heuristics).
  int64_t algorithm =
      config.algorithm_case() == GemmBackendConfig::kSelectedAlgorithm
          ? config.selected_algorithm()
          : 0;

  BufferAllocation::Slice a_scale, b_scale, c_scale, d_scale, d_amax;
  TF_ASSIGN_OR_RETURN(se::gpu::BlasLt::Epilogue blas_lt_epilogue,
                      gpublas_lt::AsBlasLtEpilogue(epilogue));
  auto thunk = std::make_unique<CublasLtMatmulThunk>(
      Thunk::ThunkInfo::WithProfileAnnotation(instr), std::move(gemm_config),
      blas_lt_epilogue, algorithm, a, b, c, d, bias, aux, a_scale, b_scale,
      c_scale, d_scale, d_amax);
  AddThunkToThunkSequence(std::move(thunk));
  return absl::OkStatus();
}

absl::Status IrEmitterUnnested::EmitCublasLtMatmulThunk(mlir::Operation* op) {
  auto matmul = mlir::dyn_cast<mlir::lmhlo_gpu::CublasLtMatmulOp>(op);
  TF_RET_CHECK(matmul != nullptr);

  TF_ASSIGN_OR_RETURN(auto a, GetAllocationSlice(matmul.getA()));
  TF_ASSIGN_OR_RETURN(auto b, GetAllocationSlice(matmul.getB()));
  TF_ASSIGN_OR_RETURN(auto c, GetAllocationSlice(matmul.getC()));
  TF_ASSIGN_OR_RETURN(auto d, GetAllocationSlice(matmul.getD()));

  BufferAllocation::Slice bias, a_scale, b_scale, c_scale, d_scale, d_amax;
  if (matmul.getBias() != nullptr) {
    TF_ASSIGN_OR_RETURN(bias, GetAllocationSlice(matmul.getBias()));
  }

  BufferAllocation::Slice aux;
  if (matmul.getAux() != nullptr) {
    TF_ASSIGN_OR_RETURN(aux, GetAllocationSlice(matmul.getAux()));
  }

  TF_ASSIGN_OR_RETURN(GemmConfig gemm_config, GemmConfig::For(matmul));
  TF_ASSIGN_OR_RETURN(auto epilogue,
                      gpublas_lt::AsBlasLtEpilogue(matmul.getEpilogue()));
  auto thunk = std::make_unique<CublasLtMatmulThunk>(
      Thunk::ThunkInfo::WithProfileAnnotation(op), std::move(gemm_config),
      epilogue, matmul.getAlgorithm(), a, b, c, d, bias, aux, a_scale, b_scale,
      c_scale, d_scale, d_amax);

  AddThunkToThunkSequence(std::move(thunk));
  return absl::OkStatus();
}
#endif  // GOOGLE_CUDA || TF_HIPBLASLT

#if GOOGLE_CUDA

absl::Status IrEmitterUnnested::EmitCublasLtMatmulThunkF8(
    const HloCustomCallInstruction* instr) {
  TF_RET_CHECK(instr->operand_count() == 6 || instr->operand_count() == 7 ||
               instr->operand_count() == 8);
  TF_ASSIGN_OR_RETURN(const auto gpu_config,
                      instr->backend_config<xla::gpu::GpuBackendConfig>());
  xla::gpu::GemmBackendConfig config = gpu_config.gemm_backend_config();
  xla::gpu::GemmBackendConfig_Epilogue epilogue = config.epilogue();

  TF_ASSIGN_OR_RETURN(bool has_vector_bias,
                      xla::gpu::gpublas_lt::EpilogueAddsVectorBias(epilogue));
  bool has_damax = instr->shape().IsTuple();
  xla::ShapeIndex output_index =
      has_damax ? xla::ShapeIndex{0} : xla::ShapeIndex{};

  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice a,
                      GetAllocationSliceForHlo(instr->operand(0)));
  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice b,
                      GetAllocationSliceForHlo(instr->operand(1)));
  BufferAllocation::Slice c;
  bool has_matrix_bias = config.beta() != 0;
  if (has_matrix_bias) {
    TF_ASSIGN_OR_RETURN(c, GetAllocationSliceForHlo(instr->operand(2)));
  } else {
    TF_ASSIGN_OR_RETURN(c, GetAllocationSliceForHlo(instr, output_index));
  }
  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice d,
                      GetAllocationSliceForHlo(instr, output_index));

  int a_scale_index = has_matrix_bias ? 3 : 2;
  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice a_scale,
                      GetAllocationSliceForHlo(instr->operand(a_scale_index)));
  TF_ASSIGN_OR_RETURN(
      BufferAllocation::Slice b_scale,
      GetAllocationSliceForHlo(instr->operand(a_scale_index + 1)));
  TF_ASSIGN_OR_RETURN(
      BufferAllocation::Slice c_scale,
      GetAllocationSliceForHlo(instr->operand(a_scale_index + 2)));
  TF_ASSIGN_OR_RETURN(
      BufferAllocation::Slice d_scale,
      GetAllocationSliceForHlo(instr->operand(a_scale_index + 3)));

  BufferAllocation::Slice bias;
  if (has_vector_bias) {
    TF_ASSIGN_OR_RETURN(
        bias, GetAllocationSliceForHlo(instr->operand(a_scale_index + 4)));
  }

  BufferAllocation::Slice d_amax;
  if (has_damax) {
    TF_ASSIGN_OR_RETURN(d_amax, GetAllocationSliceForHlo(instr, {1}));
  }

  TF_ASSIGN_OR_RETURN(
      auto gemm_config,
      GemmConfig::For(static_cast<const HloInstruction*>(instr)));

  // Use the first algorithm by default (i.e. fastest according to heuristics).
  int64_t algorithm =
      config.algorithm_case() == GemmBackendConfig::kSelectedAlgorithm
          ? config.selected_algorithm()
          : 0;

  BufferAllocation::Slice aux;  // Not used.

  TF_ASSIGN_OR_RETURN(se::gpu::BlasLt::Epilogue blas_lt_epilogue,
                      gpublas_lt::AsBlasLtEpilogue(epilogue));
  auto thunk = std::make_unique<CublasLtMatmulThunk>(
      Thunk::ThunkInfo::WithProfileAnnotation(instr), std::move(gemm_config),
      blas_lt_epilogue, algorithm, a, b, c, d, bias, aux, a_scale, b_scale,
      c_scale, d_scale, d_amax);
  AddThunkToThunkSequence(std::move(thunk));
  return absl::OkStatus();
}

absl::Status IrEmitterUnnested::EmitCublasLtMatmulThunkF8(mlir::Operation* op) {
  auto matmul = mlir::dyn_cast<mlir::lmhlo_gpu::CublasLtMatmulF8Op>(op);
  TF_RET_CHECK(matmul != nullptr);

  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice a,
                      GetAllocationSlice(matmul.getA()));
  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice b,
                      GetAllocationSlice(matmul.getB()));
  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice c,
                      GetAllocationSlice(matmul.getC()));
  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice d,
                      GetAllocationSlice(matmul.getD()));
  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice a_scale,
                      GetAllocationSlice(matmul.getAScale()));
  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice b_scale,
                      GetAllocationSlice(matmul.getBScale()));
  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice c_scale,
                      GetAllocationSlice(matmul.getCScale()));
  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice d_scale,
                      GetAllocationSlice(matmul.getDScale()));
  BufferAllocation::Slice d_amax, bias;
  if (matmul.getDAmax() != nullptr) {
    TF_ASSIGN_OR_RETURN(d_amax, GetAllocationSlice(matmul.getDAmax()));
  }
  if (matmul.getBias() != nullptr) {
    TF_ASSIGN_OR_RETURN(bias, GetAllocationSlice(matmul.getBias()));
  }

  BufferAllocation::Slice aux;  // Not used.

  TF_ASSIGN_OR_RETURN(GemmConfig gemm_config, GemmConfig::For(matmul));
  TF_ASSIGN_OR_RETURN(auto epilogue,
                      gpublas_lt::AsBlasLtEpilogue(matmul.getEpilogue()));
  auto thunk = std::make_unique<CublasLtMatmulThunk>(
      Thunk::ThunkInfo::WithProfileAnnotation(op), std::move(gemm_config),
      epilogue, matmul.getAlgorithm(), a, b, c, d, bias, aux, a_scale, b_scale,
      c_scale, d_scale, d_amax);

  AddThunkToThunkSequence(std::move(thunk));
  return absl::OkStatus();
}

absl::Status IrEmitterUnnested::EmitConvolutionReorderThunk(
    const HloCustomCallInstruction* instr) {
  bool has_bias = instr->operand_count() > 1;
  Shape shape = has_bias ? instr->shape().tuple_shapes(0) : instr->shape();
  if (shape.rank() != 5 || shape.dimensions(4) != 32) {
    return Internal("Unexpected shape for convolution reorder: %s",
                    instr->ToString());
  }
  absl::InlinedVector<int64_t, 4> filter_dims = {
      shape.dimensions(0), shape.dimensions(1) * 32, shape.dimensions(2),
      shape.dimensions(3)};

  absl::InlinedVector<BufferAllocation::Slice, 2> operand_slices;
  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice filter_input,
                      GetAllocationSliceForHlo(instr->operand(0)));
  operand_slices.push_back(filter_input);
  if (has_bias) {
    TF_ASSIGN_OR_RETURN(BufferAllocation::Slice bias_input,
                        GetAllocationSliceForHlo(instr->operand(1)));
    operand_slices.push_back(bias_input);
  }

  absl::InlinedVector<BufferAllocation::Slice, 2> result_slices;
  if (has_bias) {
    TF_ASSIGN_OR_RETURN(BufferAllocation::Slice filter_output,
                        GetAllocationSliceForHlo(instr, {0}));
    result_slices.push_back(filter_output);
    TF_ASSIGN_OR_RETURN(BufferAllocation::Slice bias_output,
                        GetAllocationSliceForHlo(instr, {1}));
    result_slices.push_back(bias_output);
  } else {
    TF_ASSIGN_OR_RETURN(BufferAllocation::Slice filter_output,
                        GetAllocationSliceForHlo(instr));
    result_slices.push_back(filter_output);
  }

  auto thunk = std::make_unique<ConvolutionReorderThunk>(
      Thunk::ThunkInfo::WithProfileAnnotation(instr),
      absl::MakeSpan(filter_dims), operand_slices, result_slices);
  AddThunkToThunkSequence(std::move(thunk));
  return absl::OkStatus();
}

absl::Status IrEmitterUnnested::EmitConvolutionReorderThunk(
    mlir::Operation* op) {
  using mlir::dyn_cast;
  using mlir::lmhlo_gpu::CudnnConvReorderFilterAndBiasOp;
  using mlir::lmhlo_gpu::CudnnConvReorderFilterOp;

  absl::InlinedVector<BufferAllocation::Slice, 2> operand_slices;
  absl::InlinedVector<BufferAllocation::Slice, 2> result_slices;
  std::vector<int64_t> filter_dims;

  auto set_filter_data = [&](auto op) -> absl::Status {
    TF_ASSIGN_OR_RETURN(BufferAllocation::Slice filter_input,
                        GetAllocationSlice(op.getFilterInput()));
    operand_slices.push_back(filter_input);

    TF_ASSIGN_OR_RETURN(BufferAllocation::Slice filter_output,
                        GetAllocationSlice(op.getFilterOutput()));
    result_slices.push_back(filter_output);

    auto filter_dims_values = op.getFilterDims().template getValues<int64_t>();
    filter_dims.assign(filter_dims_values.begin(), filter_dims_values.end());
    return absl::OkStatus();
  };

  if (auto reorder = dyn_cast<CudnnConvReorderFilterAndBiasOp>(op)) {
    TF_RETURN_IF_ERROR(set_filter_data(reorder));

    TF_ASSIGN_OR_RETURN(BufferAllocation::Slice bias_input,
                        GetAllocationSlice(reorder.getBiasInput()));
    operand_slices.push_back(bias_input);

    TF_ASSIGN_OR_RETURN(BufferAllocation::Slice bias_output,
                        GetAllocationSlice(reorder.getBiasOutput()));
    result_slices.push_back(bias_output);
  } else if (auto reorder = dyn_cast<CudnnConvReorderFilterOp>(op)) {
    TF_RETURN_IF_ERROR(set_filter_data(reorder));
  } else {
    return Internal("Unexpected operation");
  }

  auto thunk = std::make_unique<ConvolutionReorderThunk>(
      Thunk::ThunkInfo::WithProfileAnnotation(op), absl::MakeSpan(filter_dims),
      operand_slices, result_slices);

  AddThunkToThunkSequence(std::move(thunk));
  return absl::OkStatus();
}

absl::Status IrEmitterUnnested::EmitNormThunk(
    const HloCustomCallInstruction* instr) {
  if (instr->shape().tuple_shapes_size() != 2 &&
      instr->shape().tuple_shapes_size() != 4) {
    return Internal("Unexpected shape for norm: %s", instr->ToString());
  }

  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice input_slice,
                      GetAllocationSliceForHlo(instr->operand(0)));
  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice scale_slice,
                      GetAllocationSliceForHlo(instr->operand(1)));
  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice bias_slice,
                      GetAllocationSliceForHlo(instr->operand(2)));
  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice output_slice,
                      GetAllocationSliceForHlo(instr, {0}));

  bool has_aux_outputs = instr->shape().tuple_shapes_size() == 4;
  std::optional<BufferAllocation::Slice> expectation_slice, norm_factor_slice;
  std::optional<Shape> expectation_shape, norm_factor_shape;
  BufferAllocation::Slice scratch_slice;
  Shape scratch_shape;
  if (has_aux_outputs) {
    TF_ASSIGN_OR_RETURN(expectation_slice,
                        GetAllocationSliceForHlo(instr, {1}));
    TF_ASSIGN_OR_RETURN(norm_factor_slice,
                        GetAllocationSliceForHlo(instr, {2}));
    TF_ASSIGN_OR_RETURN(scratch_slice, GetAllocationSliceForHlo(instr, {3}));
    expectation_shape = ShapeUtil::GetSubshape(instr->shape(), {1});
    norm_factor_shape = ShapeUtil::GetSubshape(instr->shape(), {2});
    scratch_shape = ShapeUtil::GetSubshape(instr->shape(), {3});
  } else {
    TF_ASSIGN_OR_RETURN(scratch_slice, GetAllocationSliceForHlo(instr, {1}));
    scratch_shape = ShapeUtil::GetSubshape(instr->shape(), {1});
  }

  TF_ASSIGN_OR_RETURN(const auto gpu_config,
                      instr->backend_config<xla::gpu::GpuBackendConfig>());
  GpuNormDescriptor descriptor = {
      gpu_config.cudnn_norm_backend_config(),
      /*input_shape=*/instr->operand(0)->shape(),
      /*scale_shape=*/instr->operand(1)->shape(),
      /*bias_shape=*/instr->operand(2)->shape(),
      /*output_shape=*/ShapeUtil::GetSubshape(instr->shape(), {0}),
      expectation_shape, norm_factor_shape,
      /*scratch_size=*/
      static_cast<size_t>(ShapeUtil::ByteSizeOf(scratch_shape))};
  TF_ASSIGN_OR_RETURN(GpuNormConfig config, GpuNormConfig::For(descriptor));

  auto thunk = std::make_unique<NormThunk>(
      Thunk::ThunkInfo::WithProfileAnnotation(instr), std::move(config),
      input_slice, scale_slice, bias_slice, output_slice, expectation_slice,
      norm_factor_slice, scratch_slice);
  AddThunkToThunkSequence(std::move(thunk));
  return absl::OkStatus();
}

absl::Status IrEmitterUnnested::EmitNormThunk(mlir::Operation* op) {
  auto norm = mlir::dyn_cast<mlir::lmhlo_gpu::CudnnNormOp>(op);
  TF_RET_CHECK(norm != nullptr);

  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice input_slice,
                      GetAllocationSlice(norm.getInput()));
  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice scale_slice,
                      GetAllocationSlice(norm.getScale()));
  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice bias_slice,
                      GetAllocationSlice(norm.getBias()));
  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice output_slice,
                      GetAllocationSlice(norm.getOutput()));

  int64_t num_operands = op->getNumOperands();
  std::optional<BufferAllocation::Slice> expectation_slice, norm_factor_slice;
  if (num_operands == 7) {
    TF_ASSIGN_OR_RETURN(expectation_slice,
                        GetAllocationSlice(norm.getExpectation()));
    TF_ASSIGN_OR_RETURN(norm_factor_slice,
                        GetAllocationSlice(norm.getNormFactor()));
  }

  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice scratch_slice,
                      GetAllocationSlice(norm.getScratch()));

  GpuNormDescriptor descriptor;
  auto* algorithm = descriptor.backend_config.mutable_algorithm();
  algorithm->set_algo_id(norm.getAlgorithmConfig().getAlgorithm());
  algorithm->set_is_cudnn_frontend(true);
  auto workspace_size = norm.getAlgorithmConfig().getWorkspaceSize();
  algorithm->mutable_workspace_size()->set_value(workspace_size);

  descriptor.input_shape = GetShape(norm->getOperand(0));
  descriptor.scale_shape = GetShape(norm->getOperand(1));
  descriptor.bias_shape = GetShape(norm->getOperand(2));
  descriptor.output_shape = GetShape(norm->getOperand(3));
  if (num_operands == 7) {
    descriptor.expectation_shape = GetShape(norm->getOperand(4));
    descriptor.norm_factor_shape = GetShape(norm->getOperand(5));
  }
  descriptor.backend_config.set_epsilon(norm.getEpsilon().convertToDouble());

  TF_ASSIGN_OR_RETURN(GpuNormConfig config, GpuNormConfig::For(descriptor));

  auto thunk = std::make_unique<NormThunk>(
      Thunk::ThunkInfo::WithProfileAnnotation(op), std::move(config),
      input_slice, scale_slice, bias_slice, output_slice, expectation_slice,
      norm_factor_slice, scratch_slice);

  AddThunkToThunkSequence(std::move(thunk));

  return absl::OkStatus();
}

absl::Status IrEmitterUnnested::EmitFusedMHAThunk(
    const HloCustomCallInstruction* instr) {
  const HloInstruction* lhs_bmm1 = instr->operand(0);
  const HloInstruction* rhs_bmm1 = instr->operand(1);
  const HloInstruction* rhs_bmm2 = instr->operand(2);

  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice lhs_bmm1_slice,
                      GetAllocationSliceForHlo(lhs_bmm1));
  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice rhs_bmm1_slice,
                      GetAllocationSliceForHlo(rhs_bmm1));
  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice rhs_bmm2_slice,
                      GetAllocationSliceForHlo(rhs_bmm2));
  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice output_slice,
                      GetAllocationSliceForHlo(instr, {0}));
  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice scratch_slice,
                      GetAllocationSliceForHlo(instr, {1}));
  BufferAllocation::Slice activation_slice;
  bool has_activation = xla::ShapeUtil::TupleElementCount(instr->shape()) == 3;
  if (has_activation) {
    TF_ASSIGN_OR_RETURN(activation_slice, GetAllocationSliceForHlo(instr, {2}));
  }

  TF_ASSIGN_OR_RETURN(const xla::gpu::CudnnfMHAKind kind,
                      xla::gpu::GetCudnnfMHAKind(instr));
  BufferAllocation::Slice mask_slice, bias_slice;
  std::optional<Shape> mask_shape, bias_shape;
  {
    bool has_mask = kind == CudnnfMHAKind::kScaleMaskSoftmax ||
                    kind == CudnnfMHAKind::kScaleMaskSoftmaxDropout ||
                    kind == CudnnfMHAKind::kScaleBiasMaskSoftmax ||
                    kind == CudnnfMHAKind::kScaleBiasMaskSoftmaxDropout;
    bool has_bias = kind == CudnnfMHAKind::kScaleBiasMaskSoftmax ||
                    kind == CudnnfMHAKind::kScaleBiasSoftmaxDropout ||
                    kind == CudnnfMHAKind::kScaleBiasSoftmax ||
                    kind == CudnnfMHAKind::kScaleBiasSoftmaxDropout;

    if (has_mask) {
      const HloInstruction* mask = instr->operand(3);
      TF_ASSIGN_OR_RETURN(mask_slice, GetAllocationSliceForHlo(mask));
      mask_shape = mask->shape();
      if (has_bias) {
        const HloInstruction* bias = instr->operand(4);
        TF_ASSIGN_OR_RETURN(bias_slice, GetAllocationSliceForHlo(bias));
        bias_shape = bias->shape();
      }
    } else if (has_bias) {
      const HloInstruction* bias = instr->operand(3);
      TF_ASSIGN_OR_RETURN(bias_slice, GetAllocationSliceForHlo(bias));
      bias_shape = bias->shape();
    }
  }

  TF_ASSIGN_OR_RETURN(const auto gpu_config,
                      instr->backend_config<xla::gpu::GpuBackendConfig>());
  const xla::gpu::CudnnfMHABackendConfig& config =
      gpu_config.cudnn_fmha_backend_config();
  Shape intermediate_tensor_shape(config.intermediate_tensor_shape());
  absl::InlinedVector<Shape, 2> output_shapes = {
      ShapeUtil::GetSubshape(instr->shape(), {0})};
  if (has_activation) {
    output_shapes.push_back(ShapeUtil::GetSubshape(instr->shape(), {2}));
  }

  GpufMHADescriptor descriptor = {kind,
                                  config,
                                  config.is_flash_attention(),
                                  config.is_causal_mask(),
                                  lhs_bmm1->shape(),
                                  rhs_bmm1->shape(),
                                  rhs_bmm2->shape(),
                                  intermediate_tensor_shape,
                                  output_shapes,
                                  config.bmm1_dot_dimension_numbers(),
                                  config.bmm2_dot_dimension_numbers(),
                                  mask_shape,
                                  bias_shape};

  TF_ASSIGN_OR_RETURN(GpufMHAConfig fmha_config,
                      GpufMHAConfig::For(descriptor));
  AddThunkToThunkSequence(std::make_unique<FusedMHAThunk>(
      Thunk::ThunkInfo::WithProfileAnnotation(instr), std::move(fmha_config),
      lhs_bmm1_slice, rhs_bmm1_slice, rhs_bmm2_slice, output_slice,
      scratch_slice, mask_slice, bias_slice, activation_slice));
  return absl::OkStatus();
}

absl::Status IrEmitterUnnested::EmitFusedMHAThunk(mlir::Operation* op) {
  using mlir::dyn_cast;
  using mlir::lmhlo_gpu::fusedMHAOp;
  GpufMHADescriptor descriptor;
  BufferAllocation::Slice lhs_bmm1_slice, rhs_bmm1_slice, rhs_bmm2_slice,
      output_slice, scratch_slice, activation_slice, mask_slice, bias_slice;

  auto populate_common = [&](auto fmha) -> absl::Status {
    descriptor.backend_config.set_fmha_scale(
        fmha.getFmhaScale().convertToDouble());

    if (fmha.getDropoutRate()) {
      descriptor.backend_config.set_dropout_rate(
          (*fmha.getDropoutRate()).convertToDouble());
    }

    if (fmha.getSeed()) {
      descriptor.backend_config.set_seed((*fmha.getSeed()));
    }

    auto* algorithm = descriptor.backend_config.mutable_algorithm();
    algorithm->set_algo_id(fmha.getAlgorithmConfig().getAlgorithm());
    for (int i = 0; i < fmha.getAlgorithmConfig().getKnobIds().size(); ++i) {
      // N.B. tuning_knobs is a map rather than a repeated field, so this
      // doesn't require reserving space up front.
      (*algorithm->mutable_tuning_knobs())[fmha.getAlgorithmConfig()
                                               .getKnobIds()[i]] =
          fmha.getAlgorithmConfig().getKnobValues()[i];
    }
    algorithm->set_is_cudnn_frontend(true);
    auto workspace_size = fmha.getAlgorithmConfig().getWorkspaceSize();
    if (workspace_size >= 0) {
      algorithm->mutable_workspace_size()->set_value(workspace_size);
    }

    descriptor.bmm1_dnums =
        ConvertDotDimensionNumbers(fmha.getBmm1DotDimensionNumbers());
    descriptor.bmm2_dnums =
        ConvertDotDimensionNumbers(fmha.getBmm2DotDimensionNumbers());

    descriptor.lhs_bmm1_shape = ShapeUtil::MakeShapeWithDenseLayout(
        GetShape(fmha.getLhsBmm1()).element_type(),
        GetShape(fmha.getLhsBmm1()).dimensions(),
        GetShape(fmha.getLhsBmm1()).layout().minor_to_major());
    TF_ASSIGN_OR_RETURN(lhs_bmm1_slice, GetAllocationSlice(fmha.getLhsBmm1()));

    descriptor.rhs_bmm1_shape = ShapeUtil::MakeShapeWithDenseLayout(
        GetShape(fmha.getRhsBmm1()).element_type(),
        GetShape(fmha.getRhsBmm1()).dimensions(),
        GetShape(fmha.getRhsBmm1()).layout().minor_to_major());
    TF_ASSIGN_OR_RETURN(rhs_bmm1_slice, GetAllocationSlice(fmha.getRhsBmm1()));

    descriptor.rhs_bmm2_shape = ShapeUtil::MakeShapeWithDenseLayout(
        GetShape(fmha.getRhsBmm2()).element_type(),
        GetShape(fmha.getRhsBmm2()).dimensions(),
        GetShape(fmha.getRhsBmm2()).layout().minor_to_major());
    TF_ASSIGN_OR_RETURN(rhs_bmm2_slice, GetAllocationSlice(fmha.getRhsBmm2()));

    descriptor.output_shapes.push_back(ShapeUtil::MakeShapeWithDenseLayout(
        GetShape(fmha.getOutput()).element_type(),
        GetShape(fmha.getOutput()).dimensions(),
        GetShape(fmha.getOutput()).layout().minor_to_major()));
    TF_ASSIGN_OR_RETURN(output_slice, GetAllocationSlice(fmha.getOutput()));

    TF_ASSIGN_OR_RETURN(scratch_slice, GetAllocationSlice(fmha.getScratch()));

    TF_ASSIGN_OR_RETURN(auto intermediate_tensor_dims_array,
                        ConvertMlirArrayAttrToInt64Array(
                            fmha.getIntermediateTensorDimensions()));
    if (fmha.getActivation() != nullptr) {
      descriptor.output_shapes.push_back(ShapeUtil::MakeShapeWithDenseLayout(
          GetShape(fmha.getActivation()).element_type(),
          GetShape(fmha.getActivation()).dimensions(),
          GetShape(fmha.getActivation()).layout().minor_to_major()));
      TF_ASSIGN_OR_RETURN(activation_slice,
                          GetAllocationSlice(fmha.getActivation()));
    }

    if (fmha.getBias() != nullptr) {
      descriptor.bias_shape = ShapeUtil::MakeShapeWithDenseLayout(
          GetShape(fmha.getBias()).element_type(),
          GetShape(fmha.getBias()).dimensions(),
          GetShape(fmha.getBias()).layout().minor_to_major());

      TF_ASSIGN_OR_RETURN(bias_slice, GetAllocationSlice(fmha.getBias()));
    }

    if (fmha.getMask() != nullptr) {
      descriptor.mask_shape = ShapeUtil::MakeShapeWithDenseLayout(
          GetShape(fmha.getMask()).element_type(),
          GetShape(fmha.getMask()).dimensions(),
          GetShape(fmha.getMask()).layout().minor_to_major());

      TF_ASSIGN_OR_RETURN(mask_slice, GetAllocationSlice(fmha.getMask()));
    }
    TF_ASSIGN_OR_RETURN(
        auto intermediate_tensor_layout_array,
        ConvertMlirArrayAttrToInt64Array(fmha.getIntermediateTensorLayout()));

    descriptor.intermediate_lhs_bmm2_shape =
        ShapeUtil::MakeShapeWithDenseLayout(
            GetShape(fmha.getOutput()).element_type(),
            intermediate_tensor_dims_array, intermediate_tensor_layout_array);

    // set if flash attention here
    descriptor.is_flash_attention = fmha.getIsFlashAttention();
    // set if causal mask here
    descriptor.is_causal_mask = fmha.getIsCausalMask();
    return absl::OkStatus();
  };

  if (auto fmha_op = dyn_cast<fusedMHAOp>(op)) {
    TF_RET_CHECK(fmha_op != nullptr);
    TF_ASSIGN_OR_RETURN(CudnnfMHAKind kind,
                        AsCudnnfMHAKind(fmha_op.getFusedMhaDag()));
    descriptor.kind = kind;
    TF_RETURN_IF_ERROR(populate_common(fmha_op));
  } else {
    return Internal("Unexpected operation");
  }
  TF_ASSIGN_OR_RETURN(GpufMHAConfig config, GpufMHAConfig::For(descriptor));
  AddThunkToThunkSequence(std::make_unique<FusedMHAThunk>(
      Thunk::ThunkInfo::WithProfileAnnotation(op), std::move(config),
      lhs_bmm1_slice, rhs_bmm1_slice, rhs_bmm2_slice, output_slice,
      scratch_slice, mask_slice, bias_slice, activation_slice));
  return absl::OkStatus();
}

absl::Status IrEmitterUnnested::EmitFusedMHABackwardThunk(mlir::Operation* op) {
  using mlir::dyn_cast;
  using mlir::lmhlo_gpu::fusedMHABackwardOp;

  GpufMHABackwardDescriptor descriptor;
  BufferAllocation::Slice bmm1_grad_gemm1_rhs_slice, bmm1_grad_gemm2_rhs_slice,
      bmm2_grad_gemm1_lhs_slice, bmm2_grad_gemm2_rhs_slice, d_output_slice,
      scratch_slice, mask_slice, fwd_output_slice, bias_slice;
  BufferAllocation::Slice d_bmm1_lhs_slice, d_bmm1_rhs_slice, d_bmm2_rhs_slice,
      d_s_slice, softmax_sum_slice, d_Q_accum_slice, d_bias_slice;

  auto populate_common = [&](auto fmha) -> absl::Status {
    descriptor.backend_config.set_fmha_scale(
        fmha.getFmhaScale().convertToDouble());

    if (fmha.getDropoutRate()) {
      descriptor.backend_config.set_dropout_rate(
          (*fmha.getDropoutRate()).convertToDouble());
    }

    if (fmha.getSeed()) {
      descriptor.backend_config.set_seed((*fmha.getSeed()));
    }

    auto* algorithm = descriptor.backend_config.mutable_algorithm();
    algorithm->set_algo_id(fmha.getAlgorithmConfig().getAlgorithm());
    for (int i = 0; i < fmha.getAlgorithmConfig().getKnobIds().size(); ++i) {
      // N.B. tuning_knobs is a map rather than a repeated field, so this
      // doesn't require reserving space up front.
      (*algorithm->mutable_tuning_knobs())[fmha.getAlgorithmConfig()
                                               .getKnobIds()[i]] =
          fmha.getAlgorithmConfig().getKnobValues()[i];
    }
    algorithm->set_is_cudnn_frontend(true);
    auto workspace_size = fmha.getAlgorithmConfig().getWorkspaceSize();
    if (workspace_size >= 0) {
      algorithm->mutable_workspace_size()->set_value(workspace_size);
    }

    // set if flash attention here
    descriptor.is_flash_attention = fmha.getIsFlashAttention();
    // set if causal mask here
    descriptor.is_causal_mask = fmha.getIsCausalMask();
    descriptor.bmm1_grad_gemm1_dnums =
        ConvertDotDimensionNumbers(fmha.getBmm1GradGemm1DotDimensionNumbers());
    descriptor.bmm1_grad_gemm2_dnums =
        ConvertDotDimensionNumbers(fmha.getBmm1GradGemm2DotDimensionNumbers());
    descriptor.bmm2_grad_gemm1_dnums =
        ConvertDotDimensionNumbers(fmha.getBmm2GradGemm1DotDimensionNumbers());
    descriptor.bmm2_grad_gemm2_dnums =
        ConvertDotDimensionNumbers(fmha.getBmm2GradGemm2DotDimensionNumbers());

    descriptor.bmm1_grad_gemm1_rhs_shape = ShapeUtil::MakeShapeWithDenseLayout(
        GetShape(fmha.getBmm1GradGemm1Rhs()).element_type(),
        GetShape(fmha.getBmm1GradGemm1Rhs()).dimensions(),
        GetShape(fmha.getBmm1GradGemm1Rhs()).layout().minor_to_major());
    TF_ASSIGN_OR_RETURN(bmm1_grad_gemm1_rhs_slice,
                        GetAllocationSlice(fmha.getBmm1GradGemm1Rhs()));

    descriptor.bmm1_grad_gemm2_rhs_shape = ShapeUtil::MakeShapeWithDenseLayout(
        GetShape(fmha.getBmm1GradGemm2Rhs()).element_type(),
        GetShape(fmha.getBmm1GradGemm2Rhs()).dimensions(),
        GetShape(fmha.getBmm1GradGemm2Rhs()).layout().minor_to_major());
    TF_ASSIGN_OR_RETURN(bmm1_grad_gemm2_rhs_slice,
                        GetAllocationSlice(fmha.getBmm1GradGemm2Rhs()));

    // fwd activation
    // fmha.getBmm2GradGemm1Lhs() could be bmm2_grad_gemm1_lhs for regular
    // attention or softmax stats for flash attention here we set the shape to
    // be bmm2_grad_gemm1_lhs even it is flash attention
    if (descriptor.is_flash_attention) {
      // flash attention TODO: make sure the layout is correct for
      // bmm2_grad_gemm1_lhs
      TF_ASSIGN_OR_RETURN(auto intermediate_tensor_dims_array,
                          ConvertMlirArrayAttrToInt64Array(
                              fmha.getIntermediateTensorDimensions()));
      TF_ASSIGN_OR_RETURN(
          auto intermediate_tensor_layout_array,
          ConvertMlirArrayAttrToInt64Array(fmha.getIntermediateTensorLayout()));

      descriptor.bmm2_grad_gemm1_lhs_shape =
          ShapeUtil::MakeShapeWithDenseLayout(
              GetShape(fmha.getDOutput()).element_type(),
              intermediate_tensor_dims_array, intermediate_tensor_layout_array);
    } else {
      descriptor.bmm2_grad_gemm1_lhs_shape =
          ShapeUtil::MakeShapeWithDenseLayout(
              GetShape(fmha.getBmm2GradGemm1Lhs()).element_type(),
              GetShape(fmha.getBmm2GradGemm1Lhs()).dimensions(),
              GetShape(fmha.getBmm2GradGemm1Lhs()).layout().minor_to_major());
    }
    TF_ASSIGN_OR_RETURN(bmm2_grad_gemm1_lhs_slice,
                        GetAllocationSlice(fmha.getBmm2GradGemm1Lhs()));

    descriptor.bmm2_grad_gemm2_rhs_shape = ShapeUtil::MakeShapeWithDenseLayout(
        GetShape(fmha.getBmm2GradGemm2Rhs()).element_type(),
        GetShape(fmha.getBmm2GradGemm2Rhs()).dimensions(),
        GetShape(fmha.getBmm2GradGemm2Rhs()).layout().minor_to_major());
    TF_ASSIGN_OR_RETURN(bmm2_grad_gemm2_rhs_slice,
                        GetAllocationSlice(fmha.getBmm2GradGemm2Rhs()));

    descriptor.d_output_shape = ShapeUtil::MakeShapeWithDenseLayout(
        GetShape(fmha.getDOutput()).element_type(),
        GetShape(fmha.getDOutput()).dimensions(),
        GetShape(fmha.getDOutput()).layout().minor_to_major());
    TF_ASSIGN_OR_RETURN(d_output_slice, GetAllocationSlice(fmha.getDOutput()));
    descriptor.d_bmm1_lhs_shape = ShapeUtil::MakeShapeWithDenseLayout(
        GetShape(fmha.getDBmm1Lhs()).element_type(),
        GetShape(fmha.getDBmm1Lhs()).dimensions(),
        GetShape(fmha.getDBmm1Lhs()).layout().minor_to_major());
    TF_ASSIGN_OR_RETURN(d_bmm1_lhs_slice,
                        GetAllocationSlice(fmha.getDBmm1Lhs()));

    descriptor.d_bmm1_rhs_shape = ShapeUtil::MakeShapeWithDenseLayout(
        GetShape(fmha.getDBmm1Rhs()).element_type(),
        GetShape(fmha.getDBmm1Rhs()).dimensions(),
        GetShape(fmha.getDBmm1Rhs()).layout().minor_to_major());
    TF_ASSIGN_OR_RETURN(d_bmm1_rhs_slice,
                        GetAllocationSlice(fmha.getDBmm1Rhs()));

    descriptor.d_bmm2_rhs_shape = ShapeUtil::MakeShapeWithDenseLayout(
        GetShape(fmha.getDBmm2Rhs()).element_type(),
        GetShape(fmha.getDBmm2Rhs()).dimensions(),
        GetShape(fmha.getDBmm2Rhs()).layout().minor_to_major());
    TF_ASSIGN_OR_RETURN(d_bmm2_rhs_slice,
                        GetAllocationSlice(fmha.getDBmm2Rhs()));

    TF_ASSIGN_OR_RETURN(scratch_slice, GetAllocationSlice(fmha.getScratch()));

    if (fmha.getD_S() != nullptr) {
      descriptor.d_s_shape = ShapeUtil::MakeShapeWithDenseLayout(
          GetShape(fmha.getD_S()).element_type(),
          GetShape(fmha.getD_S()).dimensions(),
          GetShape(fmha.getD_S()).layout().minor_to_major());
      TF_ASSIGN_OR_RETURN(d_s_slice, GetAllocationSlice(fmha.getD_S()));
    }

    if (fmha.getDBias() != nullptr) {
      descriptor.d_bias_shape = ShapeUtil::MakeShapeWithDenseLayout(
          GetShape(fmha.getDBias()).element_type(),
          GetShape(fmha.getDBias()).dimensions(),
          GetShape(fmha.getDBias()).layout().minor_to_major());
      TF_ASSIGN_OR_RETURN(d_bias_slice, GetAllocationSlice(fmha.getDBias()));
    }

    if (fmha.getMask() != nullptr) {
      // has mask input
      TF_RET_CHECK(
          descriptor.kind != xla::gpu::CudnnfMHAKind::kBackwardBmmBmm &&
          descriptor.kind != xla::gpu::CudnnfMHAKind::kBackwardSoftmaxDropout &&
          descriptor.kind != xla::gpu::CudnnfMHAKind::kBackwardSoftmax);

      descriptor.mask_shape = ShapeUtil::MakeShapeWithDenseLayout(
          GetShape(fmha.getMask()).element_type(),
          GetShape(fmha.getMask()).dimensions(),
          GetShape(fmha.getMask()).layout().minor_to_major());

      TF_ASSIGN_OR_RETURN(mask_slice, GetAllocationSlice(fmha.getMask()));
    }
    // add flash attention backward related slice here
    if (fmha.getBias() != nullptr) {
      descriptor.bias_shape = ShapeUtil::MakeShapeWithDenseLayout(
          GetShape(fmha.getBias()).element_type(),
          GetShape(fmha.getBias()).dimensions(),
          GetShape(fmha.getBias()).layout().minor_to_major());
      TF_ASSIGN_OR_RETURN(bias_slice, GetAllocationSlice(fmha.getBias()));
    }

    if (fmha.getSoftmaxSum() != nullptr) {
      TF_ASSIGN_OR_RETURN(softmax_sum_slice,
                          GetAllocationSlice(fmha.getSoftmaxSum()));
    }

    if (fmha.getD_QAccum() != nullptr) {
      TF_ASSIGN_OR_RETURN(d_Q_accum_slice,
                          GetAllocationSlice(fmha.getD_QAccum()));
    }

    if (fmha.getFwdOutput() != nullptr) {
      descriptor.fwd_output_shape = ShapeUtil::MakeShapeWithDenseLayout(
          GetShape(fmha.getFwdOutput()).element_type(),
          GetShape(fmha.getFwdOutput()).dimensions(),
          GetShape(fmha.getFwdOutput()).layout().minor_to_major());
      TF_ASSIGN_OR_RETURN(fwd_output_slice,
                          GetAllocationSlice(fmha.getFwdOutput()));
    }
    return absl::OkStatus();
  };

  if (auto fmha_backward_op = dyn_cast<fusedMHABackwardOp>(op)) {
    TF_RET_CHECK(fmha_backward_op != nullptr);
    TF_ASSIGN_OR_RETURN(
        CudnnfMHAKind kind,
        AsCudnnBackwardfMHAKind(fmha_backward_op.getFusedMhaDag()));
    descriptor.kind = kind;
    TF_RETURN_IF_ERROR(populate_common(fmha_backward_op));
  } else {
    return Internal("Unexpected operation");
  }
  TF_ASSIGN_OR_RETURN(GpufMHABackwardConfig config,
                      GpufMHABackwardConfig::For(descriptor));

  AddThunkToThunkSequence(std::make_unique<FusedMHABackwardThunk>(
      Thunk::ThunkInfo::WithProfileAnnotation(op), std::move(config),
      bmm1_grad_gemm1_rhs_slice, bmm1_grad_gemm2_rhs_slice,
      bmm2_grad_gemm1_lhs_slice, bmm2_grad_gemm2_rhs_slice, d_output_slice,
      scratch_slice, d_bmm1_lhs_slice, d_bmm1_rhs_slice, d_bmm2_rhs_slice,
      d_s_slice, softmax_sum_slice, d_Q_accum_slice, mask_slice, d_bias_slice,
      fwd_output_slice, bias_slice));

  return absl::OkStatus();
}
#endif  // GOOGLE_CUDA

absl::StatusOr<BufferAllocation::Slice>
IrEmitterUnnested::GetAllocationSliceForHlo(const HloInstruction* instr,
                                            const ShapeIndex& index) const {
  return xla::gpu::GetAllocationSlice(ir_emitter_context_->buffer_assignment(),
                                      instr, index);
}

#if GOOGLE_CUDA || TENSORFLOW_USE_ROCM

absl::Status IrEmitterUnnested::EmitCubDeviceRadixSort(
    const HloCustomCallInstruction* instr) {
  if (instr->operand_count() != 1 && instr->operand_count() != 2) {
    return Internal("Invalid number of operands for radix sort");
  }

  absl::InlinedVector<BufferAllocation::Slice, 2> operands;
  for (int i = 0; i < instr->operand_count(); ++i) {
    TF_ASSIGN_OR_RETURN(BufferAllocation::Slice operand,
                        GetAllocationSliceForHlo(instr->operand(i), {}));
    operands.push_back(operand);
  }

  absl::InlinedVector<BufferAllocation::Slice, 2> results;
  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice result,
                      GetAllocationSliceForHlo(instr, {0}));
  results.push_back(result);

  BufferAllocation::Slice scratch;
  if (instr->operand_count() == 1) {
    TF_ASSIGN_OR_RETURN(scratch, GetAllocationSliceForHlo(instr, {1}));
  } else {
    TF_ASSIGN_OR_RETURN(BufferAllocation::Slice result,
                        GetAllocationSliceForHlo(instr, {1}));
    results.push_back(result);
    TF_ASSIGN_OR_RETURN(scratch, GetAllocationSliceForHlo(instr, {2}));
  }

  TF_ASSIGN_OR_RETURN(xla::SortOptions options,
                      instr->backend_config<xla::SortOptions>());
  auto thunk = std::make_unique<CubSortThunk>(
      Thunk::ThunkInfo::WithProfileAnnotation(instr),
      instr->operand(0)->shape().element_type(),
      instr->operand_count() == 2
          ? std::optional(instr->operand(1)->shape().element_type())
          : std::nullopt,
      operands, results, scratch, options.descending());
  AddThunkToThunkSequence(std::move(thunk));
  return absl::OkStatus();
}

absl::Status IrEmitterUnnested::EmitCubDeviceRadixSort(mlir::Operation* op) {
  auto radix_sort_op = mlir::cast<mlir::lmhlo_gpu::RadixSortOp>(op);
  if (radix_sort_op.getInputs().size() != 1 &&
      radix_sort_op.getInputs().size() != 2) {
    return Internal("Invalid number of operands for radix sort");
  }

  TF_ASSIGN_OR_RETURN(std::vector<BufferAllocation::Slice> inputs,
                      GetAllocationSlices(radix_sort_op.getInputs()));
  absl::InlinedVector<BufferAllocation::Slice, 2> operands(inputs.begin(),
                                                           inputs.end());
  TF_ASSIGN_OR_RETURN(std::vector<BufferAllocation::Slice> outputs,
                      GetAllocationSlices(radix_sort_op.getOutput()));
  absl::InlinedVector<BufferAllocation::Slice, 2> results(outputs.begin(),
                                                          outputs.end());
  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice scratch,
                      GetAllocationSlice(radix_sort_op.getScratch()));

  auto thunk = std::make_unique<CubSortThunk>(
      Thunk::ThunkInfo::WithProfileAnnotation(op),
      GetShape(op->getOperand(0)).element_type(),
      radix_sort_op.getInputs().size() == 2
          ? std::optional(GetShape(op->getOperand(1)).element_type())
          : std::nullopt,
      operands, results, scratch, radix_sort_op.getDescending());

  AddThunkToThunkSequence(std::move(thunk));
  return absl::OkStatus();
}

absl::Status IrEmitterUnnested::EmitCholeskyThunk(mlir::Operation* op) {
  auto cholesky_op = mlir::cast<mlir::lmhlo_gpu::CholeskyOp>(op);

  const Shape shape = GetShape(cholesky_op.getInput());
  int ndim = shape.dimensions_size();
  CHECK_GE(ndim, 2);
  int64_t n = shape.dimensions(ndim - 1);

  const auto& dims = shape.dimensions();
  int64_t batch_size =
      std::accumulate(dims.begin(), dims.end() - 2, int64_t{1},
                      [](int64_t a, int64_t b) { return a * b; });

  TF_ASSIGN_OR_RETURN(auto operand_buffer,
                      GetAllocationSlice(cholesky_op.getInput()));
  TF_ASSIGN_OR_RETURN(auto a_buffer,
                      GetAllocationSlice(cholesky_op.getOutput()));
  TF_ASSIGN_OR_RETURN(auto workspace_buffer,
                      GetAllocationSlice(cholesky_op.getScratch()));
  TF_ASSIGN_OR_RETURN(auto info_buffer,
                      GetAllocationSlice(cholesky_op.getInfo()));

  ThunkSequence thunks;

  if (operand_buffer != a_buffer) {
    thunks.push_back(std::make_unique<DeviceToDeviceCopyThunk>(
        Thunk::ThunkInfo::WithProfileAnnotation(op),
        /*source_buffer=*/operand_buffer,
        /*destination_buffer=*/a_buffer,
        /*mem_size=*/ShapeUtil::ByteSizeOf(shape),
        /*source_value=*/cholesky_op.getInput(),
        /*destination_value=*/cholesky_op.getOutput()));
  }

  CholeskyOptions options;
  options.set_lower(cholesky_op.getIsLower());
  thunks.push_back(std::make_unique<CholeskyThunk>(
      Thunk::ThunkInfo::WithProfileAnnotation(op), options,
      PtxOptsFromDebugOptions(ir_emitter_context_->debug_options()), a_buffer,
      workspace_buffer, info_buffer, shape.element_type(), batch_size, n));

  // Elide the sequential thunk if there's no copy.
  if (thunks.size() == 1) {
    AddThunkToThunkSequence(std::move(thunks[0]));
  } else {
    AddThunkToThunkSequence(std::make_unique<SequentialThunk>(
        Thunk::ThunkInfo::WithProfileAnnotation(op), std::move(thunks)));
  }

  return absl::OkStatus();
}

absl::Status IrEmitterUnnested::EmitCholeskyThunk(const HloInstruction* instr) {
  TF_ASSIGN_OR_RETURN(CholeskyOptions options,
                      instr->backend_config<CholeskyOptions>());
  const Shape& shape = instr->operand(0)->shape();
  int ndim = shape.dimensions_size();
  CHECK_GE(ndim, 2);
  int64_t n = shape.dimensions(ndim - 1);

  const absl::Span<const int64_t>& dims = shape.dimensions();
  int64_t batch_size =
      std::accumulate(dims.begin(), dims.end() - 2, int64_t{1},
                      [](int64_t a, int64_t b) { return a * b; });

  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice operand_buffer,
                      GetAllocationSliceForHlo(instr->operand(0), {}));
  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice a_buffer,
                      GetAllocationSliceForHlo(instr, {0}));
  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice workspace_buffer,
                      GetAllocationSliceForHlo(instr, {1}));
  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice info_buffer,
                      GetAllocationSliceForHlo(instr, {2}));

  ThunkSequence thunks;

  if (operand_buffer != a_buffer) {
    thunks.push_back(std::make_unique<DeviceToDeviceCopyThunk>(
        Thunk::ThunkInfo::WithProfileAnnotation(instr),
        /*source_buffer=*/operand_buffer,
        /*destination_buffer=*/a_buffer,
        /*mem_size=*/ShapeUtil::ByteSizeOf(shape),
        /*source_value=*/nullptr,
        /*destination_value=*/nullptr));
  }

  thunks.push_back(std::make_unique<CholeskyThunk>(
      Thunk::ThunkInfo::WithProfileAnnotation(instr), options,
      PtxOptsFromDebugOptions(ir_emitter_context_->debug_options()), a_buffer,
      workspace_buffer, info_buffer, shape.element_type(), batch_size, n));

  // Elide the sequential thunk if there's no copy.
  if (thunks.size() == 1) {
    AddThunkToThunkSequence(std::move(thunks[0]));
  } else {
    AddThunkToThunkSequence(std::make_unique<SequentialThunk>(
        Thunk::ThunkInfo::WithProfileAnnotation(instr), std::move(thunks)));
  }

  return absl::OkStatus();
}
#endif  // GOOGLE_CUDA || TENSORFLOW_USE_ROCM

// Converts MLIR dictionary attribute attached to a custom call operation to a
// custom call thunk attributes that are forwarded to the FFI handler.
static absl::StatusOr<CustomCallThunk::AttributesMap> BuildAttributesMap(
    mlir::DictionaryAttr dict) {
  CustomCallThunk::AttributesMap attributes;
  for (auto& kv : dict) {
    std::string_view name = kv.getName().strref();

    auto integer = [&](mlir::IntegerAttr integer) {
      switch (integer.getType().getIntOrFloatBitWidth()) {
        case 32:
          attributes[name] = static_cast<int32_t>(integer.getInt());
          return absl::OkStatus();
        case 64:
          attributes[name] = static_cast<int64_t>(integer.getInt());
          return absl::OkStatus();
        default:
          return absl::InvalidArgumentError(absl::StrCat(
              "Unsupported integer attribute bit width for attribute: ", name));
      }
    };

    auto fp = [&](mlir::FloatAttr fp) {
      switch (fp.getType().getIntOrFloatBitWidth()) {
        case 32:
          attributes[name] = static_cast<float>(fp.getValue().convertToFloat());
          return absl::OkStatus();
        default:
          return absl::InvalidArgumentError(absl::StrCat(
              "Unsupported float attribute bit width for attribute: ", name));
      }
    };

    auto str = [&](mlir::StringAttr str) {
      attributes[name] = str.getValue().str();
      return absl::OkStatus();
    };

    TF_RETURN_IF_ERROR(
        llvm::TypeSwitch<mlir::Attribute, Status>(kv.getValue())
            .Case<mlir::IntegerAttr>(integer)
            .Case<mlir::FloatAttr>(fp)
            .Case<mlir::StringAttr>(str)
            .Default([&](mlir::Attribute) {
              return absl::InvalidArgumentError(absl::StrCat(
                  "Unsupported attribute type for attribute: ", name));
            }));
  }
  return attributes;
}

absl::Status IrEmitterUnnested::EmitCustomCallThunk(
    mlir::Operation* op, const HloCustomCallInstruction* instr) {
  if (ir_emitter_context_->emit_ir_from_hlo())
    return EmitCustomCallThunk(instr);
  auto custom_call = mlir::cast<mlir::lmhlo::CustomCallOp>(op);
  const std::string call_target_name = custom_call.getCallTargetName().str();

  // Typed FFI custom calls is a replacement for legacy custom calls with
  // a rich type safe API. It's under construction and not fully supported.
  bool is_ffi_custom_call =
      custom_call.getApiVersion() ==
      mlir::mhlo::CustomCallApiVersion::API_VERSION_TYPED_FFI;

  void* call_target = CustomCallTargetRegistry::Global()->Lookup(
      call_target_name, std::string(platform_name()));

  absl::StatusOr<XLA_FFI_Handler*> handler =
      ffi::FindHandler(call_target_name, platform_name());

  // At least one implementation should be available at run time.
  bool found_custom_call = !is_ffi_custom_call && call_target != nullptr;
  bool found_ffi_handler = is_ffi_custom_call && handler.ok();

  if (!found_custom_call && !found_ffi_handler) {
    auto& debug_options = ir_emitter_context_->debug_options();

    // If true, then all custom calls that are not found in custom call or FFI
    // registries will become no-op (we don't emit any thunks for them).
    if (debug_options.xla_gpu_mock_custom_calls()) {
      return absl::OkStatus();
    }

    // TODO(ezhulenev): Custom calls registered with an XLA runtime are not part
    // of a legacy registry, or an FFI registry. For now we simply ignore them.
    if (debug_options.xla_gpu_enable_xla_runtime_executable()) {
      return absl::OkStatus();
    }

    return absl::UnimplementedError(
        absl::StrCat("No registered implementation for custom call to ",
                     call_target_name, " for platform ", platform_name()));
  }

  using Slices = std::vector<std::optional<CustomCallThunk::Slice>>;

  // Initialize slices and shapes from the value range.
  auto init_from_values = [&](mlir::ValueRange values, Slices* slices) {
    for (mlir::Value value : values) {
      TF_ASSIGN_OR_RETURN(auto slice, GetAllocationSlice(value));
      slices->push_back(CustomCallThunk::Slice{slice, GetShape(value)});
    }
    return absl::OkStatus();
  };

  // Initialize slices and shapes from the value range with token holes.
  auto init_from_mapped_values = [&](mlir::ValueRange values,
                                     absl::Span<const int64_t> target_mapping,
                                     int64_t target_size, Slices* slices) {
    slices->resize(target_size);
    for (auto [index, value] : llvm::zip(target_mapping, values)) {
      TF_ASSIGN_OR_RETURN(auto slice, GetAllocationSlice(value));
      (*slices)[index] = CustomCallThunk::Slice{slice, GetShape(value)};
    }
    return absl::OkStatus();
  };

  Slices operands, results;

  // If we have a target mapping, than the number of operands and results of a
  // custom call handler can be more than a number of operands and results in
  // the IR. These holes are coming from the HLO token operands and results.
  if (auto target_mapping = custom_call.getTargetArgMapping()) {
    auto arg_mapping = target_mapping->getArgsToTargetArgs();
    auto res_mapping = target_mapping->getResultsToTargetResults();

    TF_RETURN_IF_ERROR(
        init_from_mapped_values(custom_call.getArgs(), arg_mapping,
                                target_mapping->getNumArgs(), &operands));
    TF_RETURN_IF_ERROR(
        init_from_mapped_values(custom_call.getOutput(), res_mapping,
                                target_mapping->getNumResults(), &results));

  } else {
    TF_RETURN_IF_ERROR(init_from_values(custom_call.getArgs(), &operands));
    TF_RETURN_IF_ERROR(init_from_values(custom_call.getOutput(), &results));
  }

  // For legacy custom calls we convert all API versions into the the latest
  // status-returning one and pass backend config as an opaque string.
  CustomCallThunk::CustomCallTarget custom_call_target;
  std::string opaque;

  // For XLA FFI handlers we decode opaque backend config into attributes map
  // at IR emission time, so that we do not need to parse MLIR at run time. For
  // FFI handlers backend config must be a compatible MLIR dictionary.
  CustomCallThunk::AttributesMap attributes;

  // For information about this calling convention, see
  // xla/g3doc/custom_call.md.
  switch (custom_call.getApiVersion()) {
    case mlir::mhlo::CustomCallApiVersion::API_VERSION_ORIGINAL:
      using original_call_type =
          void (*)(CustomCallThunk::Stream /*stream*/, void** /*buffers*/,
                   const char* /*opaque*/, size_t /*opaque_len*/);
      custom_call_target = [call_target](CustomCallThunk::Stream stream,
                                         void** buffers, const char* opaque,
                                         size_t opaque_len,
                                         XlaCustomCallStatus*) {
        auto typed_call_target =
            reinterpret_cast<original_call_type>(call_target);
        typed_call_target(stream, buffers, opaque, opaque_len);
      };
      break;
    case mlir::mhlo::CustomCallApiVersion::API_VERSION_STATUS_RETURNING:
    case mlir::mhlo::CustomCallApiVersion::API_VERSION_STATUS_RETURNING_UNIFIED:
      using status_returning_call_type =
          void (*)(CustomCallThunk::Stream /*stream*/, void** /*buffers*/,
                   const char* /*opaque*/, size_t /*opaque_len*/,
                   XlaCustomCallStatus* /*status*/);
      custom_call_target =
          reinterpret_cast<status_returning_call_type>(call_target);
      break;
    case mlir::mhlo::CustomCallApiVersion::API_VERSION_TYPED_FFI:
      // We already checked `handler` above.
      break;
    default:
      return Internal("Unknown custom-call API version enum value: %d",
                      custom_call.getApiVersion());
  }

  auto backend_config =
      custom_call.getBackendConfig().value_or(mlir::Attribute());

  switch (custom_call.getApiVersion()) {
    case mlir::mhlo::CustomCallApiVersion::API_VERSION_ORIGINAL:
    case mlir::mhlo::CustomCallApiVersion::API_VERSION_STATUS_RETURNING:
    case mlir::mhlo::CustomCallApiVersion::API_VERSION_STATUS_RETURNING_UNIFIED:
      if (auto str = backend_config.dyn_cast_or_null<mlir::StringAttr>()) {
        opaque = str.str();
        break;
      }
      return absl::InternalError(
          "Unsupported backend config. Expected a string attribute");

    case mlir::mhlo::CustomCallApiVersion::API_VERSION_TYPED_FFI:
      if (auto dict = backend_config.dyn_cast_or_null<mlir::DictionaryAttr>()) {
        TF_ASSIGN_OR_RETURN(attributes, BuildAttributesMap(dict));
        break;
      }
      return absl::InternalError(
          "Unsupported backend config. Expected a dictionary attribute");

    default:
      return Internal("Unknown custom-call API version enum value: %d",
                      custom_call.getApiVersion());
  }

  auto ffi_thunk = [&] {
    auto& called_computations = instr->called_computations();
    return std::make_unique<CustomCallThunk>(
        Thunk::ThunkInfo::WithProfileAnnotation(op), *handler,
        std::move(operands), std::move(results), std::move(attributes),
        called_computations.empty() ? nullptr : called_computations[0]);
  };

  auto legacy_thunk = [&] {
    return std::make_unique<CustomCallThunk>(
        Thunk::ThunkInfo::WithProfileAnnotation(op),
        std::move(custom_call_target), std::move(operands), std::move(results),
        std::move(opaque));
  };

  AddThunkToThunkSequence(found_ffi_handler ? ffi_thunk() : legacy_thunk());

  return absl::OkStatus();
}

absl::Status IrEmitterUnnested::EmitCustomCallThunk(
    const HloCustomCallInstruction* instr) {
  const std::string call_target_name = instr->custom_call_target();

  // Typed FFI custom calls is a replacement for legacy custom calls with
  // a rich type safe API. It's under construction and not fully supported.
  bool is_ffi_custom_call =
      instr->api_version() == CustomCallApiVersion::API_VERSION_TYPED_FFI;

  void* call_target = CustomCallTargetRegistry::Global()->Lookup(
      call_target_name, std::string(platform_name()));

  absl::StatusOr<XLA_FFI_Handler*> handler =
      ffi::FindHandler(call_target_name, platform_name());

  // At least one implementation should be available at run time.
  bool found_custom_call = !is_ffi_custom_call && call_target != nullptr;
  bool found_ffi_handler = is_ffi_custom_call && handler.ok();

  if (!found_custom_call && !found_ffi_handler) {
    auto& debug_options = ir_emitter_context_->debug_options();

    // If true, then all custom calls that are not found in custom call or FFI
    // registries will become no-op (we don't emit any thunks for them).
    if (debug_options.xla_gpu_mock_custom_calls()) {
      return absl::OkStatus();
    }

    // TODO(ezhulenev): Custom calls registered with an XLA runtime are not part
    // of a legacy registry, or an FFI registry. For now we simply ignore them.
    if (debug_options.xla_gpu_enable_xla_runtime_executable()) {
      return absl::OkStatus();
    }

    return absl::UnimplementedError(
        absl::StrCat("No registered implementation for custom call to ",
                     call_target_name, " for platform ", platform_name()));
  }

  using Slices = std::vector<std::optional<CustomCallThunk::Slice>>;

  Slices operands;
  for (auto* operand : instr->operands()) {
    TF_RETURN_IF_ERROR(ShapeUtil::ForEachSubshapeWithStatus(
        operand->shape(), [&](const Shape& subshape, const ShapeIndex& index) {
          if (subshape.IsToken()) {
            operands.push_back(std::nullopt);
            return absl::OkStatus();
          }
          if (!subshape.IsArray()) {
            return absl::OkStatus();
          }
          TF_ASSIGN_OR_RETURN(auto slice,
                              GetAllocationSliceForHlo(operand, index));
          operands.push_back(CustomCallThunk::Slice{slice, subshape});
          return absl::OkStatus();
        }));
  }

  Slices results;
  TF_RETURN_IF_ERROR(ShapeUtil::ForEachSubshapeWithStatus(
      instr->shape(), [&](const Shape& subshape, const ShapeIndex& index) {
        if (subshape.IsToken()) {
          results.push_back(std::nullopt);
          return absl::OkStatus();
        }
        if (!subshape.IsArray()) {
          return absl::OkStatus();
        }
        TF_ASSIGN_OR_RETURN(auto slice, GetAllocationSliceForHlo(instr, index));
        results.push_back(CustomCallThunk::Slice{slice, subshape});
        return absl::OkStatus();
      }));

  // For legacy custom calls we convert all API versions into the latest
  // status-returning one and pass backend config as an opaque string.
  CustomCallThunk::CustomCallTarget custom_call_target;
  std::string opaque;

  // For XLA FFI handlers we decode opaque backend config into attributes map
  // at IR emission time, so that we do not need to parse MLIR at run time. For
  // FFI handlers backend config must be a compatible MLIR dictionary.
  CustomCallThunk::AttributesMap attributes;

  // For information about this calling convention, see
  // xla/g3doc/custom_call.md.
  switch (instr->api_version()) {
    case CustomCallApiVersion::API_VERSION_ORIGINAL:
      using original_call_type =
          void (*)(CustomCallThunk::Stream /*stream*/, void** /*buffers*/,
                   const char* /*opaque*/, size_t /*opaque_len*/);
      custom_call_target = [call_target](CustomCallThunk::Stream stream,
                                         void** buffers, const char* opaque,
                                         size_t opaque_len,
                                         XlaCustomCallStatus*) {
        auto typed_call_target =
            reinterpret_cast<original_call_type>(call_target);
        typed_call_target(stream, buffers, opaque, opaque_len);
      };
      break;
    case CustomCallApiVersion::API_VERSION_STATUS_RETURNING:
    case CustomCallApiVersion::API_VERSION_STATUS_RETURNING_UNIFIED:
      using status_returning_call_type =
          void (*)(CustomCallThunk::Stream /*stream*/, void** /*buffers*/,
                   const char* /*opaque*/, size_t /*opaque_len*/,
                   XlaCustomCallStatus* /*status*/);
      custom_call_target =
          reinterpret_cast<status_returning_call_type>(call_target);
      break;
    case CustomCallApiVersion::API_VERSION_TYPED_FFI:
      // We already checked `handler` above.
      break;
    default:
      return Internal("Unknown custom-call API version enum value: %d",
                      instr->api_version());
  }

  auto& backend_config_str = instr->raw_backend_config_string();
  switch (instr->api_version()) {
    case CustomCallApiVersion::API_VERSION_ORIGINAL:
    case CustomCallApiVersion::API_VERSION_STATUS_RETURNING:
    case CustomCallApiVersion::API_VERSION_STATUS_RETURNING_UNIFIED:
      if (!backend_config_str.empty()) {
        opaque = backend_config_str;
      }
      break;

    case CustomCallApiVersion::API_VERSION_TYPED_FFI:
      if (!backend_config_str.empty()) {
        mlir::Attribute attr = mlir::parseAttribute(
            backend_config_str, ir_emitter_context_->mlir_context());
        if (auto dict = attr.dyn_cast_or_null<mlir::DictionaryAttr>()) {
          TF_ASSIGN_OR_RETURN(attributes, BuildAttributesMap(dict));
          break;
        }
        return absl::InternalError(
            "Unsupported backend config. Expected a string parsable into "
            "dictionary attribute");
      }
      break;

    default:
      return Internal("Unknown custom-call API version enum value: %d",
                      instr->api_version());
  }

  auto ffi_thunk = [&] {
    auto& called_computations = instr->called_computations();
    return std::make_unique<CustomCallThunk>(
        Thunk::ThunkInfo::WithProfileAnnotation(instr), *handler,
        std::move(operands), std::move(results), std::move(attributes),
        called_computations.empty() ? nullptr : called_computations[0]);
  };

  auto legacy_thunk = [&] {
    return std::make_unique<CustomCallThunk>(
        Thunk::ThunkInfo::WithProfileAnnotation(instr),
        std::move(custom_call_target), std::move(operands), std::move(results),
        std::move(opaque));
  };

  AddThunkToThunkSequence(found_ffi_handler ? ffi_thunk() : legacy_thunk());

  return absl::OkStatus();
}

absl::Status IrEmitterUnnested::EmitFftThunk(mlir::Operation* op) {
  auto fft_op = mlir::cast<mlir::lmhlo::FftOp>(op);
  const Shape operand_shape = GetShape(fft_op.getOperand());
  const Shape output_shape = GetShape(fft_op.getOutput());
  TF_RET_CHECK(LayoutUtil::IsMonotonicWithDim0Major(operand_shape.layout()));
  TF_RET_CHECK(LayoutUtil::IsMonotonicWithDim0Major(output_shape.layout()));

  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice arg_slice,
                      GetAllocationSlice(fft_op.getOperand()));
  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice dest_slice,
                      GetAllocationSlice(fft_op.getOutput()));
  TF_ASSIGN_OR_RETURN(
      xla::FftType fft_type,
      ConvertFftType(mlir::mhlo::stringifyFftType(fft_op.getFftType())));
  auto fft_length_values = fft_op.getFftLength().getValues<int64_t>();
  std::vector<int64_t> fft_length(fft_length_values.begin(),
                                  fft_length_values.end());

  AddThunkToThunkSequence(std::make_unique<FftThunk>(
      Thunk::ThunkInfo::WithProfileAnnotation(op), fft_type, fft_length,
      /*input_buffer=*/arg_slice,
      /*output_buffer=*/dest_slice,
      /*input_shape=*/operand_shape,
      /*output_shape=*/output_shape));
  return absl::OkStatus();
}

absl::Status IrEmitterUnnested::EmitFftThunk(const HloFftInstruction* instr) {
  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice arg_slice,
                      GetAllocationSliceForHlo(instr->operand(0)));
  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice dest_slice,
                      GetAllocationSliceForHlo(instr));
  AddThunkToThunkSequence(
      std::make_unique<FftThunk>(Thunk::ThunkInfo::WithProfileAnnotation(instr),
                                 instr->fft_type(), instr->fft_length(),
                                 /*input_buffer=*/arg_slice,
                                 /*output_buffer=*/dest_slice,
                                 /*input_shape=*/instr->operand(0)->shape(),
                                 /*output_shape=*/instr->shape()));
  return absl::OkStatus();
}

#if GOOGLE_CUDA || TENSORFLOW_USE_ROCM
absl::Status IrEmitterUnnested::EmitTriangularSolveCustomCall(
    mlir::Operation* op) {
  auto custom_call = mlir::cast<mlir::lmhlo::CustomCallOp>(op);

  auto operands = op->getOperands();
  TF_RET_CHECK(operands.size() == 4);

  // We expect Fortran layout for everything other than the temp buffer (the
  // last operand).  Fortran layout is not XLA default layout with elements 0
  // and 1 swapped.  For example instead of default layout {3,2,1,0} we'd have
  // Fortran layout {2,3,1,0}.
  TF_RET_CHECK(absl::c_all_of(operands.drop_back(1), [&](mlir::Value v) {
    const Shape& shape = GetShape(v);
    const Layout& layout = shape.layout();
    int n = layout.minor_to_major_size();
    if (n < 2) {
      return false;
    }
    // Unfortunately the HLO -> LMHLO -> HLO conversion loses layout information
    // if the shape has any dimensions of size 1: In that case, the new HLO
    // (which we see here) will have an arbitrary value for the location of the
    // size-1 dimension.  Just skip this assertion if the shape has any
    // degenerate dimensions.
    if (absl::c_any_of(shape.dimensions(),
                       [](int64_t dim) { return dim == 1; })) {
      return true;
    }
    return layout.minor_to_major(0) == n - 2 &&
           layout.minor_to_major(1) == n - 1 &&
           std::is_sorted(layout.minor_to_major().begin() + 2,
                          layout.minor_to_major().end(),
                          std::greater<int64_t>());
  }));

  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice a_slice,
                      GetAllocationSlice(operands[0]));
  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice b_slice,
                      GetAllocationSlice(operands[1]));
  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice result_slice,
                      GetAllocationSlice(operands[2]));
  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice temp_slice,
                      GetAllocationSlice(operands[3]));

  const Shape b_shape = GetShape(operands[1]);
  const PrimitiveType elem_ty = b_shape.element_type();

  TriangularSolveOptions backend_config;
  if (auto str = custom_call.getBackendConfig()
                     .value_or(mlir::Attribute())
                     .dyn_cast_or_null<mlir::StringAttr>())
    TF_RETURN_IF_ERROR(
        tsl::HumanReadableJsonToProto(str.str(), &backend_config));

  ThunkSequence thunks;

  // Triangular solve is in-place on 'b', so copy 'b' to the output if they
  // aren't the same buffer.
  if (b_slice != result_slice) {
    thunks.push_back(std::make_unique<DeviceToDeviceCopyThunk>(
        Thunk::ThunkInfo(op),
        /*source_buffer=*/b_slice,
        /*destination_buffer=*/result_slice,
        /*mem_size=*/ShapeUtil::ByteSizeOf(b_shape),
        /*source_value=*/operands[1],
        /*destination_value=*/operands[2]));
  }

  int64_t m = b_shape.dimensions(b_shape.rank() - 2);
  int64_t n = b_shape.dimensions(b_shape.rank() - 1);
  int64_t batch_size = std::accumulate(
      b_shape.dimensions().begin(), b_shape.dimensions().end() - 2, int64_t{1},
      [](int64_t a, int64_t b) { return a * b; });
  int64_t elem_size = ShapeUtil::ByteSizeOfPrimitiveType(elem_ty);
  int64_t a_batch_stride =
      backend_config.left_side() ? m * m * elem_size : n * n * elem_size;
  int64_t b_batch_stride = m * n * elem_size;
  thunks.push_back(std::make_unique<TriangularSolveThunk>(
      Thunk::ThunkInfo::WithProfileAnnotation(op), backend_config,
      PtxOptsFromDebugOptions(ir_emitter_context_->debug_options()),
      /*a_buffer=*/a_slice, /*b_buffer=*/result_slice, temp_slice, elem_ty,
      batch_size, m, n, a_batch_stride, b_batch_stride));

  // Elide the sequential thunk if there's no copy.
  if (thunks.size() == 1) {
    AddThunkToThunkSequence(std::move(thunks[0]));
  } else {
    AddThunkToThunkSequence(std::make_unique<SequentialThunk>(
        Thunk::ThunkInfo::WithProfileAnnotation(op), std::move(thunks)));
  }
  return absl::OkStatus();
}

absl::Status IrEmitterUnnested::EmitTriangularSolveCustomCall(
    const HloInstruction* instr) {
  TF_RET_CHECK(instr->operand_count() == 2);
  auto operands = instr->operands();
  TF_RET_CHECK(instr->shape().IsTuple() &&
               instr->shape().tuple_shapes_size() == 2);

  // We expect Fortran layout for everything other than the temp buffer (the
  // last operand).  Fortran layout is not XLA default layout with elements 0
  // and 1 swapped.  For example instead of default layout {3,2,1,0} we'd have
  // Fortran layout {2,3,1,0}.
  auto has_fortran_layout = [](const Layout& layout) {
    int n = layout.minor_to_major_size();
    return layout.minor_to_major(0) == n - 2 &&
           layout.minor_to_major(1) == n - 1;
  };
  TF_RET_CHECK(has_fortran_layout(operands[0]->shape().layout()));
  TF_RET_CHECK(has_fortran_layout(operands[1]->shape().layout()));
  TF_RET_CHECK(has_fortran_layout(instr->shape().tuple_shapes(0).layout()));

  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice a_slice,
                      GetAllocationSliceForHlo(operands[0]));
  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice b_slice,
                      GetAllocationSliceForHlo(operands[1]));
  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice result_slice,
                      GetAllocationSliceForHlo(instr, {0}));
  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice temp_slice,
                      GetAllocationSliceForHlo(instr, {1}));

  const Shape b_shape = operands[1]->shape();
  const PrimitiveType elem_ty = b_shape.element_type();

  TriangularSolveOptions backend_config;
  auto& backend_config_str = instr->raw_backend_config_string();
  if (!backend_config_str.empty()) {
    TF_RETURN_IF_ERROR(
        tsl::HumanReadableJsonToProto(backend_config_str, &backend_config));
  }

  ThunkSequence thunks;

  // Triangular solve is in-place on 'b', so copy 'b' to the output if they
  // aren't the same buffer.
  if (b_slice != result_slice) {
    thunks.push_back(std::make_unique<DeviceToDeviceCopyThunk>(
        Thunk::ThunkInfo::WithProfileAnnotation(instr),
        /*source_buffer=*/b_slice,
        /*destination_buffer=*/result_slice,
        /*mem_size=*/ShapeUtil::ByteSizeOf(b_shape),
        /*source_value=*/nullptr,
        /*destination_value=*/nullptr));
  }

  int64_t m = b_shape.dimensions(b_shape.rank() - 2);
  int64_t n = b_shape.dimensions(b_shape.rank() - 1);
  int64_t batch_size = std::accumulate(
      b_shape.dimensions().begin(), b_shape.dimensions().end() - 2, int64_t{1},
      [](int64_t a, int64_t b) { return a * b; });
  int64_t elem_size = ShapeUtil::ByteSizeOfPrimitiveType(elem_ty);
  int64_t a_batch_stride =
      backend_config.left_side() ? m * m * elem_size : n * n * elem_size;
  int64_t b_batch_stride = m * n * elem_size;
  thunks.push_back(std::make_unique<TriangularSolveThunk>(
      Thunk::ThunkInfo::WithProfileAnnotation(instr), backend_config,
      PtxOptsFromDebugOptions(ir_emitter_context_->debug_options()),
      /*a_buffer=*/a_slice, /*b_buffer=*/result_slice, temp_slice, elem_ty,
      batch_size, m, n, a_batch_stride, b_batch_stride));

  // Elide the sequential thunk if there's no copy.
  if (thunks.size() == 1) {
    AddThunkToThunkSequence(std::move(thunks[0]));
  } else {
    AddThunkToThunkSequence(std::make_unique<SequentialThunk>(
        Thunk::ThunkInfo::WithProfileAnnotation(instr), std::move(thunks)));
  }
  return absl::OkStatus();
}
#endif  // GOOGLE_CUDA || TENSORFLOW_USE_ROCM

absl::Status IrEmitterUnnested::EmitTopKCustomCall(
    const HloCustomCallInstruction* instr) {
  auto operands = instr->operands();
  auto shape = instr->shape();
  TF_RET_CHECK(operands.size() == 1)
      << "Expect only 1 operand for TopK custom call.";
  TF_RET_CHECK(shape.IsTuple())
      << "Expect TopK custom call to have tuple shape.";
  TF_RET_CHECK(shape.tuple_shapes_size() == 2)
      << "Expect TopK custom call shape to have exactly 2 sub-shapes.";

  auto data_shape = operands[0]->shape();
  auto top_elements_shape = shape.tuple_shapes()[0];
  auto indices_shape = shape.tuple_shapes()[1];

  TF_RET_CHECK(data_shape.rank() <= 2) << "Invalid input shape.";
  TF_RET_CHECK(indices_shape.element_type() == PrimitiveType::S32)
      << "Indices should be S32.";

  bool has_batch = data_shape.rank() == 2;
  auto [batch_size, n, k] =
      has_batch
          ? std::tuple<size_t, size_t, size_t>{data_shape.dimensions(0),
                                               data_shape.dimensions(1),
                                               top_elements_shape.dimensions(1)}
          : std::tuple<size_t, size_t, size_t>{
                1, data_shape.dimensions(0), top_elements_shape.dimensions(0)};

  // Load TopK custom kernel.
  TF_ASSIGN_OR_RETURN(CustomKernel kernel,
                      kernel::topk::GetTopKKernel(
                          "topk", data_shape.element_type(), n, k, batch_size));

  // Prepare kernel arguments.
  TF_ASSIGN_OR_RETURN(
      auto kernel_arguments,
      KernelArguments::Create(ir_emitter_context_->buffer_assignment(), instr,
                              operands));

  auto thunk = std::make_unique<CustomKernelThunk>(
      instr, std::move(kernel), std::move(kernel_arguments.args()));
  AddThunkToThunkSequence(std::move(thunk));

  return absl::OkStatus();
}

// Convert the following form of fusion region:
//   fusion() {
//     %0 = tensor_load %external_memref0
//     %1 = tensor_load %external_memref1
//     ...
//     materialize_in_destination %ret, %external_memref2
//   }
// to
//   fusion(%external_memref0, %external_memref1) (^bb(%0, %1) {
//     ...
//     mhlo.return %ret
//   })
//
// So that it's suitable for MHLO -> XLA HLO conversion.
// This function won't be needed once ElementalIrEmitter migrates to take MHLO
// instead.
static absl::Status ProcessFusionForConversion(
    mlir::Region* region, std::vector<Shape>* operand_shapes,
    std::vector<Shape>* output_shapes) {
  std::vector<mlir::bufferization::ToTensorOp> loads;
  std::vector<mlir::bufferization::MaterializeInDestinationOp> stores;

  region->walk([&](mlir::bufferization::ToTensorOp load) {
    if (load.getMemref().getParentRegion() != region) {
      loads.push_back(load);
    }
  });

  region->walk([&](mlir::bufferization::MaterializeInDestinationOp store) {
    if (!llvm::isa<mlir::TensorType>(store.getDest().getType())) return;
    if (store.getDest().getParentRegion() != region) {
      stores.push_back(store);
    }
  });

  for (auto& load : loads) {
    auto arg = region->addArgument(load.getType(), region->getLoc());
    load.replaceAllUsesWith(arg);
    Shape shape = GetShape(load.getResult());
    operand_shapes->push_back(std::move(shape));
    load.erase();
  }

  std::vector<mlir::Value> returned_values;
  for (auto store : stores) {
    Shape shape = GetShape(store.getDest());
    output_shapes->push_back(shape);

    returned_values.push_back(store.getSource());
    store.erase();
  }

  region->back().back().erase();
  auto b = mlir::OpBuilder::atBlockEnd(&region->back());
  auto loc = returned_values[0].getLoc();
  b.create<mlir::mhlo::ReturnOp>(loc, returned_values);
  return absl::OkStatus();
}

absl::Status IrEmitterUnnested::EmitFusion(const HloFusionInstruction* instr,
                                           HloFusionAnalysis& fusion_analysis) {
  TF_ASSIGN_OR_RETURN(
      std::unique_ptr<FusionInterface> emitter,
      GetFusionEmitter(HloFusionInfo(
          fusion_analysis, instr, &ir_emitter_context_->buffer_assignment())));
  return AddThunksToThunkSequence(
      emitter->Emit(*ir_emitter_context_, nullptr, *instr));
}

absl::Status IrEmitterUnnested::EmitFusion(
    mlir::Operation* op,
    const absl::flat_hash_map<const mlir::Operation*, const HloInstruction*>&
        hlo_for_lmhlo) {
  auto fusion_op = mlir::cast<mlir::lmhlo::FusionOp>(op);
  auto* fusion = Cast<HloFusionInstruction>(hlo_for_lmhlo.at(fusion_op));

  // Create HloFusionAnalysis instance.
  const se::DeviceDescription& device_info =
      ir_emitter_context_->gpu_device_info();
  auto fusion_analysis = HloFusionAnalysis::Create(fusion, &device_info);

  TF_ASSIGN_OR_RETURN(
      std::unique_ptr<FusionInterface> emitter,
      GetFusionEmitter(LmhloFusionInfo(fusion_analysis, fusion_op,
                                       ir_emitter_context_->allocations())));
  return AddThunksToThunkSequence(
      emitter->Emit(*ir_emitter_context_, fusion_op, *fusion));
}

absl::Status IrEmitterUnnested::AssertNonDeterminismIsOkay(
    const std::string& op_name) {
  if (ir_emitter_context_->debug_options().xla_gpu_deterministic_ops()) {
    return Unimplemented(
        "HLO instruction %s does not have a deterministic implementation, "
        "but run-to-run determinism is required by "
        "--xla_gpu_deterministic_ops.",
        op_name);
  }
  return absl::OkStatus();
}

absl::Status IrEmitterUnnested::EmitSelectAndScatter(
    mlir::Operation* op,
    const absl::flat_hash_map<const mlir::Operation*, const HloInstruction*>&
        hlo_for_lmhlo) {
  auto select_and_scatter_op = mlir::cast<mlir::lmhlo::SelectAndScatterOp>(op);
  auto* select_and_scatter =
      Cast<HloSelectAndScatterInstruction>(hlo_for_lmhlo.at(op));

  const Shape source_shape = GetShape(select_and_scatter_op.getSource());
  const Shape operand_shape = GetShape(select_and_scatter_op.getOperand());
  const int64_t rank = operand_shape.rank();

  CHECK_EQ(rank, source_shape.rank());
  if (select_and_scatter_op.getWindowDimensions()) {
    CHECK_EQ(rank, select_and_scatter_op.getWindowDimensions()->size());
  }

  TF_RETURN_IF_ERROR(AssertNonDeterminismIsOkay(
      mlir::mhlo::GetDebugNameFromLocation(select_and_scatter_op.getLoc())));

  std::string name = GetIrNameFromLoc(select_and_scatter_op.getLoc());

  const HloInstruction* init_value = select_and_scatter->operand(2);
  // IrEmitterUnnested implements kSelectAndScatter as a SequentialThunk
  // consisting of two thunks, an initializer KernelThunk that initializes
  // the output and another KernelThunk that accumulates the scattered
  // elements.
  TF_RETURN_IF_ERROR(BuildInitializerThunk(op, select_and_scatter, init_value,
                                           select_and_scatter_op.getInitValue(),
                                           select_and_scatter_op.getOut()));

  LaunchDimensions launch_dimensions = CalculateLaunchDimensions(
      source_shape, ir_emitter_context_->gpu_device_info());

  // Init value is not needed in IR emission.
  TF_ASSIGN_OR_RETURN(auto ir_arrays, BuildKernelThunkForNonFusionOp(
                                          select_and_scatter_op,
                                          {select_and_scatter_op.getOperand(),
                                           select_and_scatter_op.getSource(),
                                           select_and_scatter_op.getOut()},
                                          launch_dimensions));

  auto& [inputs, outputs] = ir_arrays;
  CHECK_EQ(inputs.size(), 3);
  CHECK_EQ(outputs.size(), 0);
  const llvm_ir::IrArray& operand_array = inputs[0];
  const llvm_ir::IrArray& source_array = inputs[1];
  const llvm_ir::IrArray& out_array = inputs[2];

  llvm::Type* index_type = GetIndexTypeForKernel(
      select_and_scatter_op, launch_dimensions.launch_bound(), &b_);
  auto index_typed_constant = [&](uint64_t c) -> llvm::Constant* {
    return llvm::ConstantInt::get(index_type, c);
  };

  // kSelectAndScatter is implemented as two kernel launches: the first launch
  // initializes the output array to the given initial value,
  // and the second accumulates the "source" matrix to the
  // selected elements in the output array. The first launch is already
  // implemented by the initializer thunk generated earlier, so this function
  // only needs to take care of the select-and-scatter part.
  //
  // Pseudo code for select-and-scatter:
  //
  // for (coordinates S in the source):  # This loop is parallel.
  //   initialized_flag = false
  //   for (coordinates W in the window):
  //     I = S * stride + W - pad_low
  //     if I within bounds of operand:
  //       if !(initialized_flag and select(selected_value, operand(I))):
  //         selected_value = operand(I)
  //         selected_index = I
  //         initialized_flag = true
  //   if initialized_flag:
  //     output(selected_index) = scatter(output(selected_index), source(S))
  auto loop_body_emitter =
      [&](const llvm_ir::IrArray::Index& source_index) -> absl::Status {
    // Allocate space to keep the currently selected value, its index, and a
    // boolean flag if the value is initialized. The initialized_flag is set
    // false.
    llvm::Value* selected_value_address = llvm_ir::EmitAllocaAtFunctionEntry(
        llvm_ir::PrimitiveTypeToIrType(operand_shape.element_type(), module_),
        "selected_value_address", &b_);

    llvm::AllocaInst* selected_index_address =
        llvm_ir::EmitAllocaAtFunctionEntryWithCount(
            index_type, index_typed_constant(rank), "selected_index_address",
            &b_);

    llvm::AllocaInst* initialized_flag_address =
        llvm_ir::EmitAllocaAtFunctionEntry(b_.getInt1Ty(),
                                           "initialized_flag_address", &b_);
    Store(b_.getInt1(false), initialized_flag_address);

    // Create the inner loop to iterate over the window.
    llvm_ir::ForLoopNest window_loops(absl::StrCat(name, "inner"), &b_,
                                      index_type);

    DimensionVector window_size;
    mlir::DenseIntElementsAttr window_dimensions =
        select_and_scatter_op.getWindowDimensions().value();
    for (const auto& dim : window_dimensions) {
      window_size.push_back(dim.getSExtValue());
      CHECK_GT(dim.getSExtValue(), 0);
    }

    const llvm_ir::IrArray::Index window_index = window_loops.AddLoopsForShape(
        ShapeUtil::MakeShape(operand_shape.element_type(), window_size),
        "window");
    llvm_ir::SetToFirstInsertPoint(window_loops.GetInnerLoopBodyBasicBlock(),
                                   &b_);

    // Compute the operand index to visit and evaluate the condition whether the
    // operand index is within the bounds. The unsigned comparison includes
    // checking whether the operand index >= 0.
    std::vector<llvm::Value*> operand_multi_index(source_index.size());
    llvm::Value* in_bounds_condition = b_.getInt1(true);

    auto strides = *select_and_scatter_op.getWindowStrides();
    auto paddings = *select_and_scatter_op.getPadding();

    for (const auto& stride_and_padding :
         llvm::enumerate(llvm::zip(strides, paddings))) {
      const int i = stride_and_padding.index();
      int64_t stride = std::get<0>(stride_and_padding.value()).getSExtValue();
      int64_t padding = std::get<1>(stride_and_padding.value()).getSExtValue();

      llvm::Value* strided_index =
          NSWMul(source_index[i], index_typed_constant(stride));
      operand_multi_index[i] = NSWSub(NSWAdd(strided_index, window_index[i]),
                                      index_typed_constant(padding));
      llvm::Value* index_condition = ICmpULT(
          operand_multi_index[i],
          index_typed_constant(ShapeUtil::GetDimension(operand_shape, i)));
      in_bounds_condition = And(in_bounds_condition, index_condition);
    }

    // Only need to do something if the operand index is within the bounds.
    // First check if the initialized_flag is set.
    llvm_ir::LlvmIfData if_in_bounds =
        llvm_ir::EmitIfThenElse(in_bounds_condition, "in-bounds", &b_);
    llvm_ir::SetToFirstInsertPoint(if_in_bounds.true_block, &b_);
    llvm_ir::LlvmIfData if_initialized = llvm_ir::EmitIfThenElse(
        Load(initialized_flag_address->getAllocatedType(),
             initialized_flag_address),
        "initialized", &b_);

    // If the initialized_flag is false, initialize the selected value and index
    // with the currently visiting operand.
    llvm_ir::SetToFirstInsertPoint(if_initialized.false_block, &b_);
    const auto save_operand_index =
        [&](const llvm_ir::IrArray::Index& operand_index) {
          for (int64_t i = 0; i < rank; ++i) {
            llvm::Value* selected_index_address_slot =
                InBoundsGEP(selected_index_address->getAllocatedType(),
                            selected_index_address, {b_.getInt32(i)});
            Store(operand_index[i], selected_index_address_slot);
          }
        };
    llvm_ir::IrArray::Index operand_index(operand_multi_index, operand_shape,
                                          index_type);
    llvm::Value* operand_data =
        operand_array.EmitReadArrayElement(operand_index, &b_);
    Store(operand_data, selected_value_address);
    save_operand_index(operand_index);
    Store(b_.getInt1(true), initialized_flag_address);

    // If the initialized_flag is true, call the `select` function to
    // potentially update the selected value and index with the currently
    // visiting operand.
    llvm_ir::SetToFirstInsertPoint(if_initialized.true_block, &b_);
    llvm::Value* operand_address =
        operand_array.EmitArrayElementAddress(operand_index, &b_);
    llvm::AllocaInst* select_return_buffer = llvm_ir::EmitAllocaAtFunctionEntry(
        llvm_ir::PrimitiveTypeToIrType(PRED, module_), "select_return_buffer",
        &b_);

    const HloComputation* select_computation = select_and_scatter->select();
    TF_RETURN_IF_ERROR(CallNestedComputation(
        &b_, *ir_emitter_context_, *select_computation,
        {selected_value_address, operand_address}, select_return_buffer));
    llvm::Value* result =
        Load(select_return_buffer->getAllocatedType(), select_return_buffer);

    // If the 'select' function returns false, update the selected value and the
    // index to the currently visiting operand.
    llvm::Value* cond =
        ICmpNE(result,
               llvm::ConstantInt::get(
                   llvm_ir::PrimitiveTypeToIrType(PRED, module_), 0),
               "boolean_predicate");
    llvm_ir::LlvmIfData if_select_lhs =
        llvm_ir::EmitIfThenElse(cond, "if-select-lhs", &b_);
    llvm_ir::SetToFirstInsertPoint(if_select_lhs.false_block, &b_);
    Store(Load(operand_array.GetElementLlvmType(), operand_address),
          selected_value_address);
    save_operand_index(operand_index);

    // If the initialized_flag is true, write to the selected index of the
    // output; otherwise the window is outside the source (in the padding) and
    // should be ignored.
    llvm_ir::SetToFirstInsertPoint(window_loops.GetOuterLoopExitBasicBlock(),
                                   &b_);
    llvm_ir::LlvmIfData if_should_store = llvm_ir::EmitIfThenElse(
        Load(initialized_flag_address->getAllocatedType(),
             initialized_flag_address),
        "should-store", &b_, /*emit_else=*/false);
    llvm_ir::SetToFirstInsertPoint(if_should_store.true_block, &b_);

    // After iterating over the window elements, scatter the source element to
    // the selected index of the output. The value we store at the output
    // location is computed by calling the `scatter` function with the source
    // value and the current output value.
    std::vector<llvm::Value*> selected_multi_index;
    for (int64_t i = 0; i < rank; ++i) {
      llvm::Value* selected_index_address_slot =
          InBoundsGEP(selected_index_address->getAllocatedType(),
                      selected_index_address, {b_.getInt32(i)});
      selected_multi_index.push_back(
          Load(selected_index_address->getAllocatedType(),
               selected_index_address_slot));
    }
    const Shape output_shape = GetShape(select_and_scatter_op.getOut());
    llvm::Value* source_value_address =
        source_array.EmitArrayElementAddress(source_index, &b_);
    llvm_ir::IrArray::Index selected_index(selected_multi_index, output_shape,
                                           operand_index.GetType());
    llvm::Value* output_value_address =
        out_array.EmitArrayElementAddress(selected_index, &b_);

    const HloComputation* scatter_computation = select_and_scatter->scatter();
    return EmitAtomicOperationForNestedComputation(
        &b_, *ir_emitter_context_, *scatter_computation, output_value_address,
        source_value_address, source_array.GetElementLlvmType());
  };

  return ParallelLoopEmitter(loop_body_emitter, source_shape, launch_dimensions,
                             &b_)
      .EmitLoop(name, index_type);
}

absl::Status IrEmitterUnnested::EmitSelectAndScatter(
    const HloSelectAndScatterInstruction* instr) {
  const HloInstruction* operand = instr->operand(0);
  const HloInstruction* source = instr->operand(1);
  const Shape source_shape = source->shape();
  const Shape operand_shape = operand->shape();
  const int64_t rank = operand_shape.rank();

  Window window = instr->window();

  CHECK_EQ(rank, source_shape.rank());
  CHECK_EQ(rank, window.dimensions_size());

  std::string name = llvm_ir::IrName(instr);

  TF_RETURN_IF_ERROR(AssertNonDeterminismIsOkay(name));

  const HloInstruction* init_value = instr->operand(2);
  // IrEmitterUnnested implements kSelectAndScatter as a SequentialThunk
  // consisting of two thunks, an initializer KernelThunk that initializes
  // the output and another KernelThunk that accumulates the scattered
  // elements.
  TF_RETURN_IF_ERROR(
      BuildInitializerThunk(nullptr, instr, init_value, nullptr, nullptr));

  LaunchDimensions launch_dimensions = CalculateLaunchDimensions(
      source_shape, ir_emitter_context_->gpu_device_info());

  // Init value is not needed in IR emission.
  TF_ASSIGN_OR_RETURN(auto ir_arrays,
                      BuildKernelThunkForNonFusionOp(instr, {operand, source},
                                                     launch_dimensions));

  auto& [inputs, outputs] = ir_arrays;
  CHECK_EQ(inputs.size(), 3);
  CHECK_EQ(outputs.size(), 0);
  const llvm_ir::IrArray& operand_array = inputs[0];
  const llvm_ir::IrArray& source_array = inputs[1];
  const llvm_ir::IrArray& out_array = inputs[2];

  llvm::Type* index_type =
      GetIndexTypeForKernel(instr, launch_dimensions.launch_bound(), &b_);
  auto index_typed_constant = [&](uint64_t c) -> llvm::Constant* {
    return llvm::ConstantInt::get(index_type, c);
  };

  // kSelectAndScatter is implemented as two kernel launches: the first launch
  // initializes the output array to the given initial value,
  // and the second accumulates the "source" matrix to the
  // selected elements in the output array. The first launch is already
  // implemented by the initializer thunk generated earlier, so this function
  // only needs to take care of the select-and-scatter part.
  //
  // Pseudo code for select-and-scatter:
  //
  // for (coordinates S in the source):  # This loop is parallel.
  //   initialized_flag = false
  //   for (coordinates W in the window):
  //     I = S * stride + W - pad_low
  //     if I within bounds of operand:
  //       if !(initialized_flag and select(selected_value, operand(I))):
  //         selected_value = operand(I)
  //         selected_index = I
  //         initialized_flag = true
  //   if initialized_flag:
  //     output(selected_index) = scatter(output(selected_index), source(S))
  auto loop_body_emitter =
      [&](const llvm_ir::IrArray::Index& source_index) -> absl::Status {
    // Allocate space to keep the currently selected value, its index, and a
    // boolean flag if the value is initialized. The initialized_flag is set
    // false.
    llvm::Value* selected_value_address = llvm_ir::EmitAllocaAtFunctionEntry(
        llvm_ir::PrimitiveTypeToIrType(operand_shape.element_type(), module_),
        "selected_value_address", &b_);

    llvm::AllocaInst* selected_index_address =
        llvm_ir::EmitAllocaAtFunctionEntryWithCount(
            index_type, index_typed_constant(rank), "selected_index_address",
            &b_);

    llvm::AllocaInst* initialized_flag_address =
        llvm_ir::EmitAllocaAtFunctionEntry(b_.getInt1Ty(),
                                           "initialized_flag_address", &b_);
    Store(b_.getInt1(false), initialized_flag_address);

    // Create the inner loop to iterate over the window.
    llvm_ir::ForLoopNest window_loops(absl::StrCat(name, "inner"), &b_,
                                      index_type);

    DimensionVector window_size;
    for (const WindowDimension& dim : window.dimensions()) {
      auto size = static_cast<int64_t>(dim.size());
      window_size.push_back(size);
      CHECK_GT(size, 0);
    }

    const llvm_ir::IrArray::Index window_index = window_loops.AddLoopsForShape(
        ShapeUtil::MakeShape(operand_shape.element_type(), window_size),
        "window");
    llvm_ir::SetToFirstInsertPoint(window_loops.GetInnerLoopBodyBasicBlock(),
                                   &b_);

    // Compute the operand index to visit and evaluate the condition whether the
    // operand index is within the bounds. The unsigned comparison includes
    // checking whether the operand index >= 0.
    std::vector<llvm::Value*> operand_multi_index(source_index.size());
    llvm::Value* in_bounds_condition = b_.getInt1(true);

    for (const auto [i, value] : llvm::enumerate(window.dimensions())) {
      auto stride = static_cast<int64_t>(value.stride());
      auto padding = static_cast<int64_t>(value.padding_low());

      llvm::Value* strided_index =
          NSWMul(source_index[i], index_typed_constant(stride));
      operand_multi_index[i] = NSWSub(NSWAdd(strided_index, window_index[i]),
                                      index_typed_constant(padding));
      llvm::Value* index_condition = ICmpULT(
          operand_multi_index[i],
          index_typed_constant(ShapeUtil::GetDimension(operand_shape, i)));
      in_bounds_condition = And(in_bounds_condition, index_condition);
    }

    // Only need to do something if the operand index is within the bounds.
    // First check if the initialized_flag is set.
    llvm_ir::LlvmIfData if_in_bounds =
        llvm_ir::EmitIfThenElse(in_bounds_condition, "in-bounds", &b_);
    llvm_ir::SetToFirstInsertPoint(if_in_bounds.true_block, &b_);
    llvm_ir::LlvmIfData if_initialized = llvm_ir::EmitIfThenElse(
        Load(initialized_flag_address->getAllocatedType(),
             initialized_flag_address),
        "initialized", &b_);

    // If the initialized_flag is false, initialize the selected value and index
    // with the currently visiting operand.
    llvm_ir::SetToFirstInsertPoint(if_initialized.false_block, &b_);
    const auto save_operand_index =
        [&](const llvm_ir::IrArray::Index& operand_index) {
          for (int64_t i = 0; i < rank; ++i) {
            llvm::Value* selected_index_address_slot =
                InBoundsGEP(selected_index_address->getAllocatedType(),
                            selected_index_address, {b_.getInt32(i)});
            Store(operand_index[i], selected_index_address_slot);
          }
        };
    llvm_ir::IrArray::Index operand_index(operand_multi_index, operand_shape,
                                          index_type);
    llvm::Value* operand_data =
        operand_array.EmitReadArrayElement(operand_index, &b_);
    Store(operand_data, selected_value_address);
    save_operand_index(operand_index);
    Store(b_.getInt1(true), initialized_flag_address);

    // If the initialized_flag is true, call the `select` function to
    // potentially update the selected value and index with the currently
    // visiting operand.
    llvm_ir::SetToFirstInsertPoint(if_initialized.true_block, &b_);
    llvm::Value* operand_address =
        operand_array.EmitArrayElementAddress(operand_index, &b_);
    llvm::AllocaInst* select_return_buffer = llvm_ir::EmitAllocaAtFunctionEntry(
        llvm_ir::PrimitiveTypeToIrType(PRED, module_), "select_return_buffer",
        &b_);

    const HloComputation* select_computation = instr->select();
    TF_RETURN_IF_ERROR(CallNestedComputation(
        &b_, *ir_emitter_context_, *select_computation,
        {selected_value_address, operand_address}, select_return_buffer));
    llvm::Value* result =
        Load(select_return_buffer->getAllocatedType(), select_return_buffer);

    // If the 'select' function returns false, update the selected value and the
    // index to the currently visiting operand.
    llvm::Value* cond =
        ICmpNE(result,
               llvm::ConstantInt::get(
                   llvm_ir::PrimitiveTypeToIrType(PRED, module_), 0),
               "boolean_predicate");
    llvm_ir::LlvmIfData if_select_lhs =
        llvm_ir::EmitIfThenElse(cond, "if-select-lhs", &b_);
    llvm_ir::SetToFirstInsertPoint(if_select_lhs.false_block, &b_);
    Store(Load(operand_array.GetElementLlvmType(), operand_address),
          selected_value_address);
    save_operand_index(operand_index);

    // If the initialized_flag is true, write to the selected index of the
    // output; otherwise the window is outside the source (in the padding) and
    // should be ignored.
    llvm_ir::SetToFirstInsertPoint(window_loops.GetOuterLoopExitBasicBlock(),
                                   &b_);
    llvm_ir::LlvmIfData if_should_store = llvm_ir::EmitIfThenElse(
        Load(initialized_flag_address->getAllocatedType(),
             initialized_flag_address),
        "should-store", &b_, /*emit_else=*/false);
    llvm_ir::SetToFirstInsertPoint(if_should_store.true_block, &b_);

    // After iterating over the window elements, scatter the source element to
    // the selected index of the output. The value we store at the output
    // location is computed by calling the `scatter` function with the source
    // value and the current output value.
    std::vector<llvm::Value*> selected_multi_index;
    for (int64_t i = 0; i < rank; ++i) {
      llvm::Value* selected_index_address_slot =
          InBoundsGEP(selected_index_address->getAllocatedType(),
                      selected_index_address, {b_.getInt32(i)});
      selected_multi_index.push_back(
          Load(selected_index_address->getAllocatedType(),
               selected_index_address_slot));
    }
    const Shape output_shape = instr->shape();
    llvm::Value* source_value_address =
        source_array.EmitArrayElementAddress(source_index, &b_);
    llvm_ir::IrArray::Index selected_index(selected_multi_index, output_shape,
                                           operand_index.GetType());
    llvm::Value* output_value_address =
        out_array.EmitArrayElementAddress(selected_index, &b_);

    const HloComputation* scatter_computation = instr->scatter();
    return EmitAtomicOperationForNestedComputation(
        &b_, *ir_emitter_context_, *scatter_computation, output_value_address,
        source_value_address, source_array.GetElementLlvmType());
  };

  return ParallelLoopEmitter(loop_body_emitter, source_shape, launch_dimensions,
                             &b_)
      .EmitLoop(name, index_type);
}

absl::Status IrEmitterUnnested::EmitWhile(
    mlir::Operation* op,
    const absl::flat_hash_map<const mlir::Operation*, const HloInstruction*>&
        hlo_for_lmhlo) {
  auto while_op = mlir::cast<mlir::lmhlo::WhileOp>(op);

  auto cond_result = GetHloOutputs(while_op);
  TF_RET_CHECK(cond_result.size() == 1);
  TF_RET_CHECK(cond_result[0]
                   .getType()
                   .cast<mlir::ShapedType>()
                   .getElementType()
                   .isInteger(/*width=*/1))
      << "While condition computation must return bool";

  TF_ASSIGN_OR_RETURN(
      auto thunk,
      BuildWhileThunk(while_op, Thunk::ThunkInfo::WithProfileAnnotation(op),
                      hlo_for_lmhlo, while_op.getTripCount()));
  AddThunkToThunkSequence(std::move(thunk));
  return absl::OkStatus();
}

absl::Status IrEmitterUnnested::EmitWhile(const HloInstruction* instr) {
  TF_ASSIGN_OR_RETURN(auto config,
                      instr->backend_config<xla::WhileLoopBackendConfig>());

  std::optional<int64_t> trip_count = std::nullopt;
  if (config.has_known_trip_count()) trip_count = config.known_trip_count().n();

  TF_ASSIGN_OR_RETURN(
      auto thunk,
      BuildWhileThunk(instr, Thunk::ThunkInfo::WithProfileAnnotation(instr),
                      trip_count));

  AddThunkToThunkSequence(std::move(thunk));
  return absl::OkStatus();
}

absl::Status IrEmitterUnnested::EmitRngGetAndUpdateState(mlir::Operation* op) {
  auto rng_op = mlir::dyn_cast<mlir::lmhlo::RngGetAndUpdateStateOp>(op);

  // Emit a kernel to increment the global state for Philox RNG algorithm.
  TF_ASSIGN_OR_RETURN(auto ir_arrays,
                      BuildKernelThunkForNonFusionOp(
                          rng_op /*, rng_op.getState(),*/, LaunchDimensions()));
  auto& [inputs, outputs] = ir_arrays;

  llvm::Value* old_state =
      llvm_ir::RngGetAndUpdateState(rng_op.getDelta(), module_, &b_);

  const Shape shape = GetShape(rng_op.getState());

  llvm::Value* output_address = inputs[0].EmitArrayElementAddress(
      llvm_ir::IrArray::Index(
          /*linear=*/b_.getInt64(0), shape, &b_),
      &b_, "rng_state_address");
  Store(old_state, output_address);

  return absl::OkStatus();
}

absl::Status IrEmitterUnnested::EmitRngGetAndUpdateState(
    const HloRngGetAndUpdateStateInstruction* instr) {
  // Emit a kernel to increment the global state for Philox RNG algorithm.
  TF_ASSIGN_OR_RETURN(auto ir_arrays, BuildKernelThunkForNonFusionOp(
                                          instr, {}, LaunchDimensions()));
  auto& [inputs, outputs] = ir_arrays;
  llvm::Value* old_state =
      llvm_ir::RngGetAndUpdateState(instr->delta(), module_, &b_);
  llvm::Value* output_address = inputs[0].EmitArrayElementAddress(
      llvm_ir::IrArray::Index(
          /*linear=*/b_.getInt64(0), instr->shape(), &b_),
      &b_, "rng_state_address");
  Store(old_state, output_address);
  return absl::OkStatus();
}

absl::Status IrEmitterUnnested::EmitSort(mlir::Operation* op,
                                         const HloSortInstruction* sort) {
  auto sort_op = mlir::dyn_cast_or_null<mlir::lmhlo::SortOp>(op);
  if (!ir_emitter_context_->emit_ir_from_hlo() && !sort_op) {
    return absl::InternalError("MLIR operations must be not null");
  }

  std::string op_name(sort->name());
  const Shape& keys_shape = sort->operand(0)->shape();
  int64_t dimension_to_sort = sort->sort_dimension();
  for (int64_t i = 0; i < sort->operand_count(); ++i) {
    ShapeIndex shape_index =
        sort->operand_count() > 1 ? ShapeIndex({i}) : ShapeIndex({});
    // We assume that the layout of all involved operands and outputs is the
    // same.
    TF_RET_CHECK(LayoutUtil::LayoutsInShapesEqual(keys_shape,
                                                  sort->operand(i)->shape()));
    TF_RET_CHECK(LayoutUtil::LayoutsInShapesEqual(
        keys_shape, ShapeUtil::GetSubshape(sort->shape(), shape_index)));

    BufferAllocation::Slice destination_buffer;
    BufferAllocation::Slice source_address;

    // If possible, we share buffers. If that is not possible, we need to
    // copy the values, because the emitter does the sorting in-place.
    if (ir_emitter_context_->emit_ir_from_hlo()) {
      TF_ASSIGN_OR_RETURN(destination_buffer,
                          GetAllocationSliceForHlo(sort, shape_index));
      TF_ASSIGN_OR_RETURN(source_address,
                          GetAllocationSliceForHlo(sort->operand(i), {}));
    } else {
      TF_ASSIGN_OR_RETURN(destination_buffer,
                          GetAllocationSlice(sort_op.getOutput()[i]));
      TF_ASSIGN_OR_RETURN(source_address,
                          GetAllocationSlice(sort_op.getOperands()[i]));
    }

    if (destination_buffer != source_address) {
      // TODO(b/26783907): Figure out why we never seem to share buffers for
      // key/value sort.
      VLOG(2) << op_name << " requires initial D2D copy for operand " << i;
      AddThunkToThunkSequence(std::make_unique<DeviceToDeviceCopyThunk>(
          Thunk::ThunkInfo(op),
          /*source_buffer=*/source_address,
          /*destination_buffer=*/destination_buffer,
          /*mem_size=*/ShapeUtil::ByteSizeOf(sort->operand(i)->shape()),
          /*source_value=*/sort_op ? sort_op.getOperands()[i] : nullptr,
          /*destination_value=*/sort_op ? sort_op.getOutput()[i] : nullptr));
    }
  }

  uint64_t dimension_to_sort_bound = keys_shape.dimensions(dimension_to_sort);
  int64_t num_stages = Log2Ceiling(dimension_to_sort_bound);
  VLOG(2) << op_name << " requires " << num_stages << " stages.";
  CHECK_GE(1ULL << num_stages, dimension_to_sort_bound);
  CHECK_LT(1ULL << (num_stages - 1), dimension_to_sort_bound);

  // Naive C++ code for the outer loops:
  //
  // for (int64_t stage = 0; stage < Log2Ceiling(dimension_to_sort_bound);
  //     ++stage) {
  //   int64_t first_xor_mask = (1LL << (stage + 1)) - 1;
  //   SortInPlace(first_xor_mask);
  //   for (int64_t mask = stage - 1; mask >= 0; --mask) {
  //     int64_t later_xor_mask = 1LL << mask;
  //     SortInPlace(later_xor_mask);
  //   }
  // }
  //
  // This follows the alternative representation of the algorithm described on
  // Wikipedia: https://en.wikipedia.org/wiki/Bitonic_sorter
  //
  // Each mask specifies how to derive from one position in the array the
  // position with which it should be compared (we calculate the xor of the
  // position with the mask).
  // As an optimization, we can move the 'mask' loop to inside the
  // sorting/comparison loop if the comparisons happen within a small block of
  // the array. To make this work, we collect all consecutive masks that are
  // smaller than our chosen power of 2 tile size, and pass them to SortInPlace.
  // Each thread then processes one tile of data.

  const uint64_t kTileSize = std::min(2048ULL, 1ULL << num_stages);

  // If we cannot combine several xor masks together, we don't use tiling, so we
  // calculate the standard launch dimensions for the shape. However we only
  // need to iterate through ~half of the dimension to sort (rounded up to the
  // next highest power of 2), because each iteration compares one pair of
  // elements.
  Shape standard_iteration_shape = keys_shape;
  uint64_t standard_num_iterations_in_sort_dim = 1ULL << (num_stages - 1);
  standard_iteration_shape.set_dimensions(dimension_to_sort,
                                          standard_num_iterations_in_sort_dim);

  LaunchDimensions standard_launch_dimensions = CalculateLaunchDimensions(
      standard_iteration_shape, ir_emitter_context_->gpu_device_info());

  // Calculate the launch dimensions for the case where we use tiling. We split
  // the dimension that should be sorted into tiles of size 'kTileSize'. This
  // means we first need to round 'dimension_to_sort_bound' up to be a multiple
  // of the tile size.
  int64_t rounded_bound = RoundUpTo(dimension_to_sort_bound, kTileSize);
  Shape iteration_shape = keys_shape;

  // We iterate through the element pairs that should be compared.
  uint64_t num_iterations_in_sort_dim = rounded_bound / 2;
  iteration_shape.set_dimensions(dimension_to_sort, num_iterations_in_sort_dim);
  uint64_t num_iterations = ShapeUtil::ElementsIn(iteration_shape);

  // For correctness reasons we need exactly 'kTileSize' / 2 many threads per
  // block. Each thread is responsible for copying exactly two adjacent elements
  // into shared memory, and then does a comparison of two possibly different
  // elements taken from shared memory.
  const uint64_t kThreadsPerBlock = kTileSize / 2;

  // Check whether we should use any tiling. We might not be able to use it if
  // we have not enough threads, or not enough shared memory.
  int64_t total_shared_memory_needed = 0;
  for (int64_t i = 0; i < sort->operand_count(); ++i) {
    total_shared_memory_needed +=
        kTileSize * ShapeUtil::ByteSizeOfPrimitiveType(
                        sort->operand(i)->shape().element_type());
  }
  bool no_tiling =
      kThreadsPerBlock >
          ir_emitter_context_->gpu_device_info().threads_per_block_limit() ||
      total_shared_memory_needed >
          ir_emitter_context_->gpu_device_info().shared_memory_per_block();
  VLOG(2) << absl::StreamFormat(
      "%s %s use tiling. No tiling if any of the following is true: "
      "kThreadsPerBlock=%d > threads_per_block_limit=%d, "
      "total_shared_memory_needed=%d > shared_memory_per_block=%d",
      op_name, (no_tiling ? "won't" : "will"), kThreadsPerBlock,
      ir_emitter_context_->gpu_device_info().threads_per_block_limit(),
      total_shared_memory_needed,
      ir_emitter_context_->gpu_device_info().shared_memory_per_block());

  uint64_t num_blocks = CeilOfRatio(num_iterations, kThreadsPerBlock);
  LaunchDimensions tiled_launch_dimensions(num_blocks, kThreadsPerBlock);
  VLOG(2) << absl::StreamFormat("%s launch dims: %d blocks, %d threads/block",
                                op_name, num_blocks, kThreadsPerBlock);
  auto emit_kernel = [&](absl::Span<const int64_t> xor_masks) {
    VLOG(2) << absl::StreamFormat(
        "%s uses kernel for xor masks [%s]", op_name,
        absl::StrJoin(xor_masks, ", ", [](std::string* out, int64_t xor_mask) {
          absl::StrAppendFormat(out, "0x%x", xor_mask);
        }));
    LaunchDimensions launch_dimensions = xor_masks.size() > 1
                                             ? tiled_launch_dimensions
                                             : standard_launch_dimensions;
    TF_ASSIGN_OR_RETURN(
        auto ir_arrays,
        ir_emitter_context_->emit_ir_from_hlo()
            ? BuildKernelThunkForNonFusionOp(sort, {}, launch_dimensions)
            : BuildKernelThunkForNonFusionOp(sort_op, sort_op.getOutput(),
                                             launch_dimensions));

    auto& [inputs, outputs] = ir_arrays;
    auto* comparator = sort->called_computations().front();
    return llvm_ir::EmitSortInPlace(
        dimension_to_sort, inputs, llvm_ir::IrName(op_name), xor_masks, &b_,
        launch_dimensions,
        xor_masks.size() > 1 ? num_iterations_in_sort_dim
                             : standard_num_iterations_in_sort_dim,
        kTileSize,
        [&](absl::Span<llvm::Value* const> operands, llvm::Value* output) {
          return CallNestedComputation(&b_, *ir_emitter_context_, *comparator,
                                       operands, output);
        });
  };
  std::vector<int64_t> xor_masks;
  for (int64_t stage = 0; stage < num_stages; ++stage) {
    for (int64_t mask = stage; mask >= 0; --mask) {
      int64_t xor_mask;
      if (mask == stage) {
        xor_mask = (1LL << (stage + 1)) - 1;
      } else {
        xor_mask = 1LL << mask;
      }
      if (xor_mask >= kTileSize || no_tiling) {
        if (!xor_masks.empty()) {
          TF_RETURN_IF_ERROR(emit_kernel(xor_masks));
          xor_masks.clear();
        }
        TF_RETURN_IF_ERROR(emit_kernel({xor_mask}));
      } else {
        xor_masks.push_back(xor_mask);
      }
    }
  }
  if (!xor_masks.empty()) {
    TF_RETURN_IF_ERROR(emit_kernel(xor_masks));
  }
  return absl::OkStatus();
}

absl::Status IrEmitterUnnested::EmitSort(const HloSortInstruction* sort) {
  CHECK(ir_emitter_context_->emit_ir_from_hlo());  // NOLINT
  return EmitSort(nullptr, sort);
}

template <typename ThunkType, typename OpT>
absl::Status IrEmitterUnnested::EmitReplicaOrPartitionId(mlir::Operation* op) {
  auto casted = mlir::cast<OpT>(op);
  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice result_slice,
                      GetAllocationSlice(casted.getOperand()));
  auto thunk = std::make_unique<ThunkType>(
      Thunk::ThunkInfo::WithProfileAnnotation(op), result_slice);
  AddThunkToThunkSequence(std::move(thunk));
  return absl::OkStatus();
}

template <typename ThunkType>
absl::Status IrEmitterUnnested::EmitReplicaOrPartitionId(
    const HloInstruction* instr) {
  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice result_slice,
                      GetAllocationSliceForHlo(instr, {}));
  auto thunk = std::make_unique<ThunkType>(
      Thunk::ThunkInfo::WithProfileAnnotation(instr), result_slice);
  AddThunkToThunkSequence(std::move(thunk));
  return absl::OkStatus();
}

Status IrEmitterUnnested::EmitCollectivePermute(mlir::Operation* op) {
  auto collective_permute_op =
      mlir::cast<mlir::lmhlo_gpu::CollectivePermuteStartOp>(op);

  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice source_slice,
                      GetAllocationSlice(collective_permute_op.getOperand()));
  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice result_slice,
                      GetAllocationSlice(collective_permute_op.getOutput()));

  const Shape shape = GetShape(collective_permute_op.getOperand());
  const auto& hlo_config = ir_emitter_context_->hlo_module().config();
  const int64_t replica_count = hlo_config.replica_count();
  const int64_t partition_count = hlo_config.num_partitions();

  if (NcclCollectivePermuteStartThunk::IsDegenerate(
          collective_permute_op, replica_count, partition_count)) {
    // For a degenerate collective permute, just generate a copy thunk.
    AddThunkToThunkSequence(std::make_unique<DeviceToDeviceCopyThunk>(
        Thunk::ThunkInfo::WithProfileAnnotation(op),
        /*source_buffer=*/source_slice,
        /*destination_buffer=*/result_slice,
        /*mem_size=*/ShapeUtil::ByteSizeOf(shape),
        /*source_value=*/collective_permute_op.getOperand(),
        /*destination_value=*/collective_permute_op.getOutput()));

    // Signal that start thunk not created with nullptr.
    collectives_async_events_.try_emplace(op, nullptr);
  } else {
    const NcclCollectiveThunk::Buffer buffer = {
        /*element_count=*/ShapeUtil::ElementsIn(shape),
        /*source_buffer=*/source_slice,
        /*destination_buffer=*/result_slice};
    auto thunk = std::make_unique<NcclCollectivePermuteStartThunk>(
        Thunk::ThunkInfo::WithProfileAnnotation(op), NcclApi::Default(),
        collective_permute_op, replica_count, partition_count, buffer);
    collectives_async_events_.try_emplace(op, thunk->async_events());
    AddThunkToThunkSequence(std::move(thunk));
  }
  return absl::OkStatus();
}

Status IrEmitterUnnested::EmitCollectivePermute(
    const HloCollectivePermuteInstruction* instr) {
  TF_RET_CHECK(instr->operand_count() == 1);
  auto* operand = instr->operand(0);
  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice source_slice,
                      GetAllocationSliceForHlo(operand));
  // First output is aliased.
  TF_RET_CHECK(
      instr->shape().IsTuple() && instr->shape().tuple_shapes_size() == 2 &&
      instr->shape().tuple_shapes(0) == instr->shape().tuple_shapes(1));
  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice result_slice,
                      GetAllocationSliceForHlo(instr, {1}));

  const Shape shape = operand->shape();
  const auto& hlo_config = ir_emitter_context_->hlo_module().config();
  const int64_t replica_count = hlo_config.replica_count();
  const int64_t partition_count = hlo_config.num_partitions();

  if (NcclCollectivePermuteStartThunk::IsDegenerate(instr, replica_count,
                                                    partition_count)) {
    // For a degenerate collective permute, just generate a copy thunk.
    AddThunkToThunkSequence(std::make_unique<DeviceToDeviceCopyThunk>(
        Thunk::ThunkInfo::WithProfileAnnotation(instr),
        /*source_buffer=*/source_slice,
        /*destination_buffer=*/result_slice,
        /*mem_size=*/ShapeUtil::ByteSizeOf(shape),
        /*source_value=*/nullptr,
        /*destination_value=*/nullptr));
    // Signal that start thunk not created with nullptr.
    collectives_async_events_.try_emplace(instr, nullptr);

  } else {
    const NcclCollectiveThunk::Buffer buffer = {
        /*element_count=*/ShapeUtil::ElementsIn(shape),
        /*source_buffer=*/source_slice,
        /*destination_buffer=*/result_slice};
    auto thunk = std::make_unique<NcclCollectivePermuteStartThunk>(
        Thunk::ThunkInfo::WithProfileAnnotation(instr), NcclApi::Default(),
        instr, replica_count, partition_count, buffer);
    collectives_async_events_.try_emplace(instr, thunk->async_events());
    AddThunkToThunkSequence(std::move(thunk));
  }
  return absl::OkStatus();
}

template <typename NcclThunkType, typename OpT>
absl::Status IrEmitterUnnested::EmitNcclThunk(mlir::Operation* untyped_op) {
  OpT op = mlir::cast<OpT>(untyped_op);
  const auto& hlo_config = ir_emitter_context_->hlo_module().config();
  int64_t replica_count = hlo_config.replica_count();
  int64_t partition_count = hlo_config.num_partitions();
  VLOG(2) << NcclThunkType::GetHloOpName()
          << "; replica count: " << replica_count
          << "; partition count: " << partition_count
          << "; operand count: " << op.getOperands().size();

  // A given collective op can be degenerate if across all groups formed
  // by it are singleton. In such a case, we don't need to do any communication
  // and we can just copy the input to the output.
  bool is_degenerate =
      GetNcclCollectiveConfigForMlir(op, op.getUseGlobalDeviceIds())
          .IsDegenerate(replica_count, partition_count);
  absl::Status implementable_status =
      NcclThunkType::CheckImplementable(op, replica_count, partition_count);
  bool should_use_nccl_thunk = !is_degenerate && implementable_status.ok();

  // Stash relevant information in NcclCollectiveThunk::Buffer even if we may
  // not generate an NcclCollectiveThunk.
  std::vector<NcclCollectiveThunk::Buffer> buffers;
  buffers.reserve(op.getInputs().size());
  for (auto it : llvm::zip(op.getInputs(), op.getOutputs())) {
    mlir::Value operand = std::get<0>(it);
    mlir::Value result = std::get<1>(it);
    const Shape shape = GetShape(operand);
    TF_ASSIGN_OR_RETURN(auto source_slice, GetAllocationSlice(operand));
    TF_ASSIGN_OR_RETURN(auto dest_slice, GetAllocationSlice(result));
    buffers.push_back(NcclCollectiveThunk::Buffer{
        /*element_count=*/ShapeUtil::ElementsIn(shape),
        /*source_buffer=*/source_slice,
        /*destination_buffer=*/dest_slice,
        /*source_memory_space=*/0,       // always 0 for LMHLO
        /*destination_memory_space=*/0,  // always 0 for LMHLO
        /*source_value=*/operand,
        /*destination_value=*/result});
  }

  if (should_use_nccl_thunk) {
    auto thunk = std::make_unique<NcclThunkType>(
        Thunk::ThunkInfo::WithProfileAnnotation(op), NcclApi::Default(), op,
        /*buffers=*/std::move(buffers));
    collectives_async_events_.try_emplace(untyped_op, thunk->async_events());
    AddThunkToThunkSequence(std::move(thunk));
    return absl::OkStatus();
  }

  if (!is_degenerate) {
    return implementable_status;
  }

  // Signal that start thunk not created with nullptr.
  collectives_async_events_.try_emplace(untyped_op, nullptr);

  VLOG(1) << "Collective call is degenerate, not doing NCCL call";

  // Degenerate collectives are simply identity function. Buffer
  // assignment expects a copy, so that's what we do.
  ThunkSequence thunks;
  for (int64_t i = 0; i < buffers.size(); i++) {
    const Shape shape = GetShape(op.getOperands()[i]);
    thunks.push_back(std::make_unique<DeviceToDeviceCopyThunk>(
        buffers.size() == 1 ? Thunk::ThunkInfo::WithProfileAnnotation(op)
                            : Thunk::ThunkInfo(op),
        /*source_buffer=*/buffers[i].source_buffer,
        /*destination_buffer=*/buffers[i].destination_buffer,
        /*mem_size=*/ShapeUtil::ByteSizeOf(shape),
        /*source_value=*/buffers[i].source_value,
        /*destination_value=*/buffers[i].destination_value));
  }
  if (thunks.size() == 1) {
    AddThunkToThunkSequence(std::move(thunks[0]));
  } else {
    AddThunkToThunkSequence(std::make_unique<SequentialThunk>(
        Thunk::ThunkInfo::WithProfileAnnotation(op), std::move(thunks)));
  }
  return absl::OkStatus();
}

absl::Status IrEmitterUnnested::EmitNcclAsyncDone(Thunk::Kind kind,
                                                  mlir::Operation* op,
                                                  mlir::Value token) {
  auto start_op = token.getDefiningOp();
  auto async_events = collectives_async_events_.extract(start_op);
  TF_RET_CHECK(async_events) << "couldn't find async events for start op";

  // Can be null if no start thunk was created (e.g. if the start op is
  // degenerate), in which case there's nothing to do here.
  if (async_events.mapped()) {
    AddThunkToThunkSequence(std::make_unique<NcclCollectiveDoneThunk>(
        kind, Thunk::ThunkInfo::WithProfileAnnotation(op),
        std::move(async_events.mapped())));
  }
  return absl::OkStatus();
}

template <typename NcclThunkType, typename HloInstType>
absl::Status IrEmitterUnnested::EmitNcclThunk(
    Thunk::Kind kind, const HloInstruction* async_start,
    const HloInstType* inst, std::optional<bool> use_global_device_ids) {
  const auto& hlo_config = ir_emitter_context_->hlo_module().config();
  int64_t replica_count = hlo_config.replica_count();
  int64_t partition_count = hlo_config.num_partitions();
  VLOG(2) << NcclThunkType::GetHloOpName()
          << "; replica count: " << replica_count
          << "; partition count: " << partition_count
          << "; operand count: " << inst->operand_count();

  // A given collective op can be degenerate if across all groups formed
  // by it are singleton. In such a case, we don't need to do any communication
  // and we can just copy the input to the output.
  bool is_degenerate = GetNcclCollectiveConfig(inst, use_global_device_ids)
                           .IsDegenerate(replica_count, partition_count);
  absl::Status implementable_status =
      NcclThunkType::CheckImplementable(inst, replica_count, partition_count);
  bool should_use_nccl_thunk = !is_degenerate && implementable_status.ok();

  // Stash relevant information in NcclCollectiveThunk::Buffer even if we may
  // not generate an NcclCollectiveThunk.
  std::vector<NcclCollectiveThunk::Buffer> buffers;

  int64_t operand_count = inst->operand_count();
  buffers.reserve(operand_count);

  // Adds a source and destination buffers pair to `buffers`.
  auto add_buffer = [&](int64_t element_count, BufferAllocation::Slice src,
                        int64_t src_memory_space, BufferAllocation::Slice dst,
                        int64_t dst_memory_space) {
    buffers.push_back(NcclCollectiveThunk::Buffer{
        /*element_count=*/element_count,
        /*source_buffer=*/src,
        /*destination_buffer=*/dst,
        /*source_memory_space=*/src_memory_space,
        /*destination_memory_space=*/dst_memory_space,
        /*source_value=*/nullptr,
        /*destination_value=*/nullptr});
  };

  if (kind == Thunk::Kind::kNcclAllGatherStart) {
    // Start operations return a tuple of (<<inputs>>, <<outputs>>) where
    // outputs can be a tuple itself (if operation has multiple operands).
    for (int64_t i = 0; i < operand_count; i++) {
      ShapeIndex idx = operand_count > 1 ? ShapeIndex({1, i}) : ShapeIndex({1});
      const Shape& src_shape = inst->operand(i)->shape();
      const Shape& dst_shape = ShapeUtil::GetSubshape(inst->shape(), idx);
      TF_ASSIGN_OR_RETURN(auto src, GetAllocationSliceForHlo(inst->operand(i)));
      TF_ASSIGN_OR_RETURN(auto dst, GetAllocationSliceForHlo(inst, idx));
      add_buffer(ShapeUtil::ElementsIn(src_shape), src,
                 src_shape.layout().memory_space(), dst,
                 dst_shape.layout().memory_space());
    }

  } else {
    // For other operations simply zip operands with results.
    for (int64_t i = 0; i < operand_count; i++) {
      ShapeIndex idx = operand_count > 1 ? ShapeIndex({i}) : ShapeIndex({});
      const Shape& src_shape = inst->operand(i)->shape();
      const Shape& dst_shape = ShapeUtil::GetSubshape(inst->shape(), idx);
      TF_ASSIGN_OR_RETURN(auto src, GetAllocationSliceForHlo(inst->operand(i)));
      TF_ASSIGN_OR_RETURN(auto dst, GetAllocationSliceForHlo(inst, idx));
      add_buffer(ShapeUtil::ElementsIn(src_shape), src,
                 src_shape.layout().memory_space(), dst,
                 dst_shape.layout().memory_space());
    }
  }

  if (should_use_nccl_thunk) {
    auto thunk = std::make_unique<NcclThunkType>(
        Thunk::ThunkInfo::WithProfileAnnotation(inst), NcclApi::Default(), inst,
        /*buffers=*/std::move(buffers));
    collectives_async_events_.insert({async_start, thunk->async_events()});
    AddThunkToThunkSequence(std::move(thunk));
    return absl::OkStatus();
  }

  if (!is_degenerate) {
    return implementable_status;
  }

  // Signal that start thunk not created with nullptr.
  collectives_async_events_.insert({async_start, nullptr});

  VLOG(1) << "Collective call is degenerate, not doing NCCL call";

  // Degenerate collectives are simply identity function. Buffer
  // assignment expects a copy, so that's what we do.
  ThunkSequence thunks;
  for (int64_t i = 0; i < buffers.size(); i++) {
    const Shape shape = inst->operand(i)->shape();
    thunks.push_back(std::make_unique<DeviceToDeviceCopyThunk>(
        Thunk::ThunkInfo::WithProfileAnnotation(inst),
        /*source_buffer=*/buffers[i].source_buffer,
        /*destination_buffer=*/buffers[i].destination_buffer,
        /*mem_size=*/ShapeUtil::ByteSizeOf(shape),
        /*source_value=*/buffers[i].source_value,
        /*destination_value=*/buffers[i].destination_value));
  }
  if (thunks.size() == 1) {
    AddThunkToThunkSequence(std::move(thunks[0]));
  } else {
    AddThunkToThunkSequence(std::make_unique<SequentialThunk>(
        Thunk::ThunkInfo::WithProfileAnnotation(inst), std::move(thunks)));
  }
  return absl::OkStatus();
}

absl::Status IrEmitterUnnested::EmitNcclAsyncDone(Thunk::Kind kind,
                                                  const HloInstruction* inst) {
  const HloInstruction* start = inst->operand(0);
  auto async_events = collectives_async_events_.extract(start);
  TF_RET_CHECK(async_events)
      << "couldn't find async events for start operation";

  // Can be null if no start thunk was created (e.g. if the start op is
  // degenerate), in which case there's nothing to do here.
  if (async_events.mapped()) {
    AddThunkToThunkSequence(std::make_unique<NcclCollectiveDoneThunk>(
        kind, Thunk::ThunkInfo::WithProfileAnnotation(inst),
        std::move(async_events.mapped())));
  }
  return absl::OkStatus();
}

absl::Status IrEmitterUnnested::EmitWaitForStreamsThunk(
    const HloInstruction* inst, GpuBackendConfig& gpu_config,
    bool is_async_done) {
  std::vector<ExecutionStreamId> wait_on_streams;
  ExecutionStreamId source_stream_id = Thunk::GetMainComputeStreamId();
  // If it's for an async done, then we need to sychronize on the execution
  // stream of the instruction from main compute stream
  if (is_async_done) {
    wait_on_streams.push_back(
        ExecutionStreamId(gpu_config.operation_queue_id()));
  } else if (gpu_config.wait_on_operation_queues().size() == 0) {
    // If wait on queue is empty, we just synchronize on the main compute
    // stream from the execution stream.
    wait_on_streams.push_back(Thunk::GetMainComputeStreamId());
    source_stream_id = gpu_config.operation_queue_id();
  } else {
    // Else, we synchronize on all specified
    // streams from the execution stream.
    for (int64_t stream_id : gpu_config.wait_on_operation_queues()) {
      wait_on_streams.push_back(ExecutionStreamId(stream_id));
    }
    source_stream_id = gpu_config.operation_queue_id();
  }

  AddThunkToThunkSequence(std::make_unique<WaitForStreamsThunk>(
      Thunk::ThunkInfo::WithProfileAnnotation(inst), source_stream_id,
      wait_on_streams));
  return absl::OkStatus();
}

absl::StatusOr<std::vector<ShapedSlice>> IrEmitterUnnested::GetShapedSlices(
    mlir::Operation::operand_range operands) {
  std::vector<ShapedSlice> shaped_slices;
  shaped_slices.reserve(operands.size());
  for (mlir::Value opnd : operands) {
    TF_ASSIGN_OR_RETURN(auto slice, GetAllocationSlice(opnd));
    shaped_slices.push_back(ShapedSlice{slice, GetShape(opnd)});
  }
  return shaped_slices;
}

absl::Status IrEmitterUnnested::EmitInfeed(mlir::Operation* op) {
  mlir::Operation::operand_range operands =
      mlir::cast<mlir::lmhlo::InfeedOp>(op).getOutputs();
  TF_ASSIGN_OR_RETURN(auto shaped_slices, GetShapedSlices(operands));
  auto thunk = std::make_unique<InfeedThunk>(
      Thunk::ThunkInfo::WithProfileAnnotation(op), std::move(shaped_slices));
  AddThunkToThunkSequence(std::move(thunk));

  return absl::OkStatus();
}

absl::Status IrEmitterUnnested::EmitInfeed(const HloInfeedInstruction* instr) {
  // Infeed instruction returns a tuple containing the result data and a token.
  // We only need the result data to construct the infeed thunk.
  std::vector<ShapedSlice> shaped_slices;
  TF_RETURN_IF_ERROR(ShapeUtil::ForEachSubshapeWithStatus(
      instr->shape(), [&](const Shape& subshape, const ShapeIndex& index) {
        if (subshape.IsTuple() || subshape.IsToken()) return absl::OkStatus();
        if (subshape.IsArray()) {
          TF_ASSIGN_OR_RETURN(BufferAllocation::Slice data,
                              GetAllocationSliceForHlo(instr, index));
          ShapedSlice shaped_slice = {data, subshape};
          shaped_slices.push_back(shaped_slice);
          return absl::OkStatus();
        }
        return Internal("Unexpected shape kind for %s and shape index %s",
                        instr->ToString(), index.ToString());
      }));

  auto thunk = std::make_unique<InfeedThunk>(
      Thunk::ThunkInfo::WithProfileAnnotation(instr), std::move(shaped_slices));
  AddThunkToThunkSequence(std::move(thunk));
  return absl::OkStatus();
}

absl::Status IrEmitterUnnested::EmitOutfeed(mlir::Operation* op) {
  mlir::Operation::operand_range operands =
      mlir::cast<mlir::lmhlo::OutfeedOp>(op).getInputs();
  TF_ASSIGN_OR_RETURN(auto shaped_slices, GetShapedSlices(operands));
  auto thunk = std::make_unique<OutfeedThunk>(
      Thunk::ThunkInfo::WithProfileAnnotation(op), std::move(shaped_slices));
  AddThunkToThunkSequence(std::move(thunk));

  return absl::OkStatus();
}

absl::Status IrEmitterUnnested::EmitOutfeed(
    const HloOutfeedInstruction* instr) {
  // HLO outfeed instruction has 2 operands, the source and a token, and a
  // single token output.
  const HloInstruction* source = instr->operand(0);
  std::vector<ShapedSlice> shaped_slices;
  TF_RETURN_IF_ERROR(ShapeUtil::ForEachSubshapeWithStatus(
      source->shape(), [&](const Shape& subshape, const ShapeIndex& index) {
        if (subshape.IsTuple()) return absl::OkStatus();
        if (subshape.IsArray()) {
          TF_ASSIGN_OR_RETURN(BufferAllocation::Slice data,
                              GetAllocationSliceForHlo(source, index));
          ShapedSlice shaped_slice = {data, subshape};
          shaped_slices.push_back(shaped_slice);
          return absl::OkStatus();
        }
        return Internal("Unexpected shape kind for %s and shape index %s",
                        source->ToString(), index.ToString());
      }));

  auto thunk = std::make_unique<OutfeedThunk>(
      Thunk::ThunkInfo::WithProfileAnnotation(instr), std::move(shaped_slices));
  AddThunkToThunkSequence(std::move(thunk));
  return absl::OkStatus();
}

absl::StatusOr<
    std::pair<std::vector<llvm_ir::IrArray>, std::vector<llvm_ir::IrArray>>>
IrEmitterUnnested::BuildKernelThunkForNonFusionOp(
    mlir::Operation* op, mlir::ValueRange needed_operands,
    const LaunchDimensions& launch_dimensions) {
  TF_RET_CHECK(!mlir::isa<mlir::lmhlo::FusionOp>(op))
      << "Please use BuildKernelThunkForFusion!";

  std::string suggested_kernel_name = GetIrNameFromLoc(op->getLoc());

  TF_ASSIGN_OR_RETURN(
      auto kernel_arguments,
      KernelArguments::Create(ir_emitter_context_->allocations(), op,
                              needed_operands));

  VLOG(3) << "Generating (without reuse check): " << suggested_kernel_name;

  llvm::Function* kernel;
  std::vector<llvm_ir::IrArray> inputs;
  std::vector<llvm_ir::IrArray> outputs;
  TF_ASSIGN_OR_RETURN(
      std::tie(kernel, inputs, outputs),
      BuildKernelPrototype(*ir_emitter_context_, suggested_kernel_name,
                           kernel_arguments.args(), needed_operands.size(),
                           launch_dimensions, &b_));

  AddThunkToThunkSequence(std::make_unique<KernelThunk>(
      op, kernel->getName().str(), kernel_arguments.args(), launch_dimensions,
      /*cluster_dim=*/std::nullopt,
      /*shmem_bytes=*/0));

  return {{inputs, outputs}};
}

absl::StatusOr<std::pair<std::vector<llvm_ir::IrArray> /*inputs*/,
                         std::vector<llvm_ir::IrArray> /*outputs*/>>
IrEmitterUnnested::BuildKernelThunkForNonFusionOp(
    const HloInstruction* hlo,
    absl::Span<const HloInstruction* const> needed_operands,
    const LaunchDimensions& launch_dimensions) {
  std::string suggested_kernel_name(hlo->name());

  TF_ASSIGN_OR_RETURN(
      auto kernel_arguments,
      KernelArguments::Create(ir_emitter_context_->buffer_assignment(), hlo,
                              needed_operands));

  VLOG(3) << "Generating (without reuse check): " << suggested_kernel_name;

  llvm::Function* kernel;
  std::vector<llvm_ir::IrArray> inputs;
  std::vector<llvm_ir::IrArray> outputs;
  TF_ASSIGN_OR_RETURN(
      std::tie(kernel, inputs, outputs),
      BuildKernelPrototype(
          *ir_emitter_context_, suggested_kernel_name, kernel_arguments.args(),
          kernel_arguments.args().size(), launch_dimensions, &b_));

  AddThunkToThunkSequence(std::make_unique<KernelThunk>(
      hlo, kernel->getName().str(), kernel_arguments.args(), launch_dimensions,
      /*cluster_dim=*/std::nullopt,
      /*shmem_bytes=*/0));

  return {{inputs, outputs}};
}

absl::StatusOr<
    std::pair<std::vector<llvm_ir::IrArray>, std::vector<llvm_ir::IrArray>>>
IrEmitterUnnested::BuildKernelThunkForNonFusionOp(
    mlir::Operation* op, const LaunchDimensions& launch_dimensions) {
  return BuildKernelThunkForNonFusionOp(op, op->getOperands(),
                                        launch_dimensions);
}

absl::Status IrEmitterUnnested::BuildInitializerThunk(
    mlir::Operation* op, const HloInstruction* instr,
    const HloInstruction* init_value, mlir::Value init_value_mlir,
    mlir::Value dest) {
  // initial value must be a scalar memref.
  TF_RET_CHECK(init_value->shape().rank() == 0);

  auto maybe_dest_slice = ir_emitter_context_->emit_ir_from_hlo()
                              ? GetAllocationSliceForHlo(instr, {})
                              : GetAllocationSlice(dest);
  if (!maybe_dest_slice.ok()) return maybe_dest_slice.status();

  BufferAllocation::Slice dest_slice = *maybe_dest_slice;

  TF_ASSIGN_OR_RETURN(
      std::optional<std::unique_ptr<Thunk>> constant_init_thunk,
      BuildConstantInitializerThunk(*ir_emitter_context_, op, instr, init_value,
                                    dest, dest_slice));
  if (constant_init_thunk) {
    AddThunkToThunkSequence(*std::move(constant_init_thunk));
    return absl::OkStatus();
  }

  // Otherwise fall back to our slow initializer code. The thunk in this case
  // will just need the IR arrays for the initial value and the destination.
  const Shape dest_shape =
      ir_emitter_context_->emit_ir_from_hlo() ? instr->shape() : GetShape(dest);

  LaunchDimensions launch_dimensions = CalculateLaunchDimensions(
      dest_shape, ir_emitter_context_->gpu_device_info());
  TF_ASSIGN_OR_RETURN(
      auto ir_arrays,
      ir_emitter_context_->emit_ir_from_hlo()
          ? BuildKernelThunkForNonFusionOp(instr, {init_value},
                                           launch_dimensions)
          : BuildKernelThunkForNonFusionOp(op, {init_value_mlir, dest},
                                           launch_dimensions));
  auto& [inputs, outputs] = ir_arrays;
  auto init_array = inputs[0];

  std::string name = ir_emitter_context_->emit_ir_from_hlo()
                         ? llvm_ir::IrName(instr, "init")
                         : GetIrNameFromLoc(op->getLoc());
  TF_RETURN_IF_ERROR(ParallelLoopEmitter(
                         [=](const llvm_ir::IrArray::Index& index) {
                           return init_array.EmitReadArrayElement(index, &b_);
                         },
                         {inputs[1]}, launch_dimensions, &b_)
                         .EmitLoop(name));
  return absl::OkStatus();
}

absl::StatusOr<std::unique_ptr<Thunk>> IrEmitterUnnested::BuildWhileThunk(
    mlir::lmhlo::WhileOp while_op, const Thunk::ThunkInfo& thunk_info,
    const absl::flat_hash_map<const mlir::Operation*, const HloInstruction*>&
        hlo_for_lmhlo,
    std::optional<int64_t> trip_count) {
  // Generate thunk sequence for while 'condition'.
  mlir::Region* condition = &while_op.getCond();
  auto ir_emitter_condition = IrEmitterUnnested::Create(ir_emitter_context_);

  TF_RETURN_IF_ERROR(
      ir_emitter_condition->EmitLmhloRegion(condition, hlo_for_lmhlo));

  // Generate thunk sequence for while 'body'.
  mlir::Region* body = &while_op.getBody();
  auto ir_emitter_body = IrEmitterUnnested::Create(ir_emitter_context_);

  TF_RETURN_IF_ERROR(ir_emitter_body->EmitLmhloRegion(body, hlo_for_lmhlo));

  // Extract the condition value from the last op (excluding the terminator op)
  // in the condition region.
  auto cond_result = GetHloOutputs(while_op);
  TF_RET_CHECK(cond_result.size() == 1);
  TF_ASSIGN_OR_RETURN(auto cond_result_slice,
                      GetAllocationSlice(cond_result[0]));

  return std::unique_ptr<Thunk>(
      new WhileThunk(thunk_info, cond_result_slice,
                     ir_emitter_condition->ConsumeThunkSequence(),
                     ir_emitter_body->ConsumeThunkSequence(), trip_count));
}

absl::StatusOr<std::unique_ptr<Thunk>> IrEmitterUnnested::BuildWhileThunk(
    const HloInstruction* instr, const Thunk::ThunkInfo& thunk_info,
    std::optional<int64_t> trip_count) {
  HloComputation* condition = instr->while_condition();
  HloComputation* body = instr->while_body();

  // Generate thunk sequence for while 'condition'.
  auto ir_emitter_condition = IrEmitterUnnested::Create(ir_emitter_context_);
  TF_RETURN_IF_ERROR(ir_emitter_condition->EmitHloComputation(condition));

  // Generate thunk sequence for while 'body'.
  auto ir_emitter_body = IrEmitterUnnested::Create(ir_emitter_context_);
  TF_RETURN_IF_ERROR(ir_emitter_body->EmitHloComputation(body));

  // Buffer slice holding while loop predicate.
  TF_ASSIGN_OR_RETURN(
      auto pred, GetAllocationSliceForHlo(condition->root_instruction(), {}));

  return std::unique_ptr<Thunk>(new WhileThunk(
      thunk_info, pred, ir_emitter_condition->ConsumeThunkSequence(),
      ir_emitter_body->ConsumeThunkSequence(), trip_count));
}

absl::Status IrEmitterUnnested::EmitTargetElementLoop(
    const HloInstruction& hlo, const llvm_ir::ElementGenerator& body_emitter) {
  return Internal("This should be unreachable");
}

static absl::flat_hash_map<std::string, std::string> ConvertFrontendAttributes(
    const FrontendAttributes& attrs) {
  absl::flat_hash_map<std::string, std::string> result;
  for (auto& [k, v] : attrs.map()) result[k] = v;
  return result;
}

static std::optional<GlobalDeviceId> DeviceConstraint(
    const HloInstruction* hlo) {
  if (hlo->has_sharding() && hlo->sharding().HasUniqueDevice()) {
    return GlobalDeviceId(hlo->sharding().GetUniqueDevice());
  }
  return std::nullopt;
}

absl::Status IrEmitterUnnested::EmitSendThunk(const HloSendInstruction* instr) {
  if (!instr->channel_id().has_value())
    return absl::InternalError("Unknown send instruction channel id");

  const HloInstruction* src = instr->operand(0);
  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice buffer,
                      GetAllocationSliceForHlo(src, {}));
  if (!instr->is_host_transfer()) {
    const auto& hlo_config = ir_emitter_context_->hlo_module().config();
    const int64_t replica_count = hlo_config.replica_count();
    const int64_t partition_count = hlo_config.num_partitions();
    const NcclCollectiveThunk::Buffer nccl_buffer = {
        /*element_count=*/ShapeUtil::ElementsIn(src->shape()),
        /*source_buffer=*/buffer,
        /*destination_buffer=*/buffer};
    auto thunk = std::make_unique<NcclSendThunk>(
        Thunk::ThunkInfo::WithProfileAnnotation(instr), NcclApi::Default(),
        instr, replica_count, partition_count, nccl_buffer);
    collectives_async_events_.try_emplace(instr, thunk->async_events());
    AddThunkToThunkSequence(std::move(thunk));
    return absl::OkStatus();
  }

  AddThunkToThunkSequence(std::make_unique<SendThunk>(
      Thunk::ThunkInfo::WithProfileAnnotation(instr), src->shape(), buffer,
      *instr->channel_id(), send_recv_events_,
      ConvertFrontendAttributes(instr->frontend_attributes()),
      DeviceConstraint(instr)));

  return absl::OkStatus();
}

absl::Status IrEmitterUnnested::EmitSendDoneThunk(
    const HloSendDoneInstruction* instr) {
  if (!instr->channel_id().has_value())
    return absl::InternalError("Unknown send done instruction channel id");

  if (!instr->is_host_transfer()) {
    return EmitNcclAsyncDone(Thunk::kNcclSendDone, instr);
  }

  AddThunkToThunkSequence(std::make_unique<SendDoneThunk>(
      Thunk::ThunkInfo::WithProfileAnnotation(instr), *instr->channel_id(),
      send_recv_events_, DeviceConstraint(instr)));

  return absl::OkStatus();
}

absl::Status IrEmitterUnnested::EmitRecvThunk(const HloRecvInstruction* instr) {
  if (!instr->channel_id().has_value())
    return absl::InternalError("Unknown recv instruction channel id");
  TF_RET_CHECK(instr->shape().IsTuple());
  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice buffer,
                      GetAllocationSliceForHlo(instr, {0}));
  if (!instr->is_host_transfer()) {
    const auto& hlo_config = ir_emitter_context_->hlo_module().config();
    const int64_t replica_count = hlo_config.replica_count();
    const int64_t partition_count = hlo_config.num_partitions();
    const NcclCollectiveThunk::Buffer nccl_buffer = {
        /*element_count=*/ShapeUtil::ElementsIn(instr->shape().tuple_shapes(0)),
        /*source_buffer=*/buffer,
        /*destination_buffer=*/buffer};
    auto thunk = std::make_unique<NcclRecvThunk>(
        Thunk::ThunkInfo::WithProfileAnnotation(instr), NcclApi::Default(),
        instr, replica_count, partition_count, nccl_buffer);
    collectives_async_events_.try_emplace(instr, thunk->async_events());
    AddThunkToThunkSequence(std::move(thunk));
    return absl::OkStatus();
  }

  AddThunkToThunkSequence(std::make_unique<RecvThunk>(
      Thunk::ThunkInfo::WithProfileAnnotation(instr),
      instr->shape().tuple_shapes()[0], buffer, *instr->channel_id(),
      send_recv_events_,
      ConvertFrontendAttributes(instr->frontend_attributes()),
      DeviceConstraint(instr)));

  return absl::OkStatus();
}

absl::Status IrEmitterUnnested::EmitRecvDoneThunk(
    const HloRecvDoneInstruction* instr) {
  if (!instr->channel_id().has_value())
    return absl::InternalError("Unknown recv done instruction channel id");

  if (!instr->is_host_transfer()) {
    return EmitNcclAsyncDone(Thunk::kNcclRecvDone, instr);
  }

  AddThunkToThunkSequence(std::make_unique<RecvDoneThunk>(
      Thunk::ThunkInfo::WithProfileAnnotation(instr), *instr->channel_id(),
      send_recv_events_, DeviceConstraint(instr)));

  return absl::OkStatus();
}

absl::Status IrEmitterUnnested::EmitOp(
    mlir::Operation* op,
    const absl::flat_hash_map<const mlir::Operation*, const HloInstruction*>&
        hlo_for_lmhlo) {
  if (mlir::isa<mlir::memref::CollapseShapeOp, mlir::func::ConstantOp,
                mlir::arith::ConstantOp, mlir::memref::ReinterpretCastOp,
                mlir::func::ReturnOp, mlir::lmhlo::TerminatorOp,
                mlir::memref::ViewOp>(op)) {
    return absl::OkStatus();
  }

  if (mlir::isa<mlir::memref::GetGlobalOp>(op)) {
    const HloConstantInstruction* hlo_const_instr =
        DynCast<HloConstantInstruction>(hlo_for_lmhlo.at(op));
    TF_RET_CHECK(hlo_const_instr);
    return EmitConstant(op, hlo_const_instr->literal());
  }

  bool is_gpu_runtime = ir_emitter_context_->debug_options()
                            .xla_gpu_enable_xla_runtime_executable();

  if (auto call = mlir::dyn_cast<mlir::lmhlo::CustomCallOp>(op)) {
    if (call.getCallTargetName() == "PadToStatic") {
      return EmitPadToStatic(
          Cast<HloCustomCallInstruction>(hlo_for_lmhlo.at(op)));
    }
    if (call.getCallTargetName() == "SliceToDynamic") {
      return EmitSliceToDynamic(
          Cast<HloCustomCallInstruction>(hlo_for_lmhlo.at(op)));
    }
    const llvm::StringRef call_target = call.getCallTargetName();
#if GOOGLE_CUDA || TENSORFLOW_USE_ROCM
    if (absl::string_view(call_target.data(), call_target.size()) ==
        kTriangularSolveCallTarget) {
      return EmitTriangularSolveCustomCall(op);
    }
#endif  // GOOGLE_CUDA || TENSORFLOW_USE_ROCM

    if (!is_gpu_runtime && call.getCallTargetName() == "__gpu$TopK") {
      return EmitTopKCustomCall(
          Cast<HloCustomCallInstruction>(hlo_for_lmhlo.at(op)));
    }

    return EmitCustomCallThunk(
        op, Cast<HloCustomCallInstruction>(hlo_for_lmhlo.at(op)));
  }

  if (mlir::isa<mlir::lmhlo_gpu::GEMMOp>(op)) {
    if (ir_emitter_context_->emit_ir_from_hlo()) {
      const HloCustomCallInstruction* instr =
          Cast<HloCustomCallInstruction>(hlo_for_lmhlo.at(op));
      return EmitGemmThunk(instr);
    }
    return EmitGemmThunk(op);
  }

#if GOOGLE_CUDA || TF_HIPBLASLT
  if (mlir::isa<mlir::lmhlo_gpu::CublasLtMatmulOp>(op)) {
    if (ir_emitter_context_->emit_ir_from_hlo()) {
      const auto* instr = Cast<HloCustomCallInstruction>(hlo_for_lmhlo.at(op));
      return EmitCublasLtMatmulThunk(instr);
    }
    return EmitCublasLtMatmulThunk(op);
  }
#endif  // GOOGLE_CUDA || TF_HIPBLASLT
#if GOOGLE_CUDA
  if (mlir::isa<mlir::lmhlo_gpu::CublasLtMatmulF8Op>(op)) {
    if (ir_emitter_context_->emit_ir_from_hlo()) {
      const auto* instr = Cast<HloCustomCallInstruction>(hlo_for_lmhlo.at(op));
      return EmitCublasLtMatmulThunkF8(instr);
    }
    return EmitCublasLtMatmulThunkF8(op);
  }
  if (mlir::isa<mlir::lmhlo_gpu::CudnnConvReorderFilterOp,
                mlir::lmhlo_gpu::CudnnConvReorderFilterAndBiasOp>(op)) {
    if (ir_emitter_context_->emit_ir_from_hlo()) {
      const auto* instr = Cast<HloCustomCallInstruction>(hlo_for_lmhlo.at(op));
      return EmitConvolutionReorderThunk(instr);
    }
    return EmitConvolutionReorderThunk(op);
  }
  if (mlir::isa<mlir::lmhlo_gpu::CudnnNormOp>(op)) {
    if (ir_emitter_context_->emit_ir_from_hlo()) {
      const auto* instr = Cast<HloCustomCallInstruction>(hlo_for_lmhlo.at(op));
      return EmitNormThunk(instr);
    }
    return EmitNormThunk(op);
  }
  if (mlir::isa<mlir::lmhlo_gpu::fusedMHAOp>(op)) {
    if (ir_emitter_context_->emit_ir_from_hlo()) {
      const auto* instr = Cast<HloCustomCallInstruction>(hlo_for_lmhlo.at(op));
      return EmitFusedMHAThunk(instr);
    }
    return EmitFusedMHAThunk(op);
  }
  if (mlir::isa<mlir::lmhlo_gpu::fusedMHABackwardOp>(op)) {
    return EmitFusedMHABackwardThunk(op);
  }
#endif  // GOOGLE_CUDA

  if (mlir::isa<mlir::lmhlo_gpu::ConvForwardOp,
                mlir::lmhlo_gpu::ConvForwardGraphOp,
                mlir::lmhlo_gpu::ConvForwardFusedOp,
                mlir::lmhlo_gpu::ConvForwardFusedSideInputOp,
                mlir::lmhlo_gpu::ConvBackwardFilterOp,
                mlir::lmhlo_gpu::ConvBackwardInputOp>(op)) {
    if (ir_emitter_context_->emit_ir_from_hlo()) {
      return EmitConvolutionThunk(
          Cast<HloCustomCallInstruction>(hlo_for_lmhlo.at(op)));
    }
    return EmitConvolutionThunk(op);
  }

#if GOOGLE_CUDA || TENSORFLOW_USE_ROCM
  if (mlir::isa<mlir::lmhlo_gpu::RadixSortOp>(op)) {
    if (ir_emitter_context_->emit_ir_from_hlo()) {
      auto* instr = Cast<HloCustomCallInstruction>(hlo_for_lmhlo.at(op));
      return EmitCubDeviceRadixSort(instr);
    }
    return EmitCubDeviceRadixSort(op);
  }
  if (mlir::isa<mlir::lmhlo_gpu::CholeskyOp>(op)) {
    if (ir_emitter_context_->emit_ir_from_hlo()) {
      return EmitCholeskyThunk(hlo_for_lmhlo.at(op));
    } else {
      return EmitCholeskyThunk(op);
    }
  }
#endif  // GOOGLE_CUDA || TENSORFLOW_USE_ROCM

  if (mlir::isa<mlir::lmhlo::FftOp>(op)) {
    if (ir_emitter_context_->emit_ir_from_hlo()) {
      return EmitFftThunk(Cast<HloFftInstruction>(hlo_for_lmhlo.at(op)));
    }
    return EmitFftThunk(op);
  }

  if (mlir::isa<mlir::lmhlo::TriangularSolveOp>(op)) {
    return Internal(
        "TriangularSolve is implemented as a custom-call; we do not expect to "
        "lower a true HLO TriangularSolve op.");
  }

  if (mlir::isa<mlir::lmhlo::FusionOp>(op)) {
    if (ir_emitter_context_->emit_ir_from_hlo()) {
      const HloFusionInstruction* instr =
          Cast<HloFusionInstruction>(hlo_for_lmhlo.at(op));
      const se::DeviceDescription& device_info =
          ir_emitter_context_->gpu_device_info();
      auto fusion_analysis = HloFusionAnalysis::Create(instr, &device_info);
      return EmitFusion(instr, fusion_analysis);
    }

    return EmitFusion(op, hlo_for_lmhlo);
  }

  if (mlir::isa<mlir::lmhlo::SelectAndScatterOp>(op)) {
    if (ir_emitter_context_->emit_ir_from_hlo()) {
      return EmitSelectAndScatter(
          Cast<HloSelectAndScatterInstruction>(hlo_for_lmhlo.at(op)));
    }
    return EmitSelectAndScatter(op, hlo_for_lmhlo);
  }

  if (mlir::isa<mlir::lmhlo::RngGetAndUpdateStateOp>(op)) {
    if (ir_emitter_context_->emit_ir_from_hlo()) {
      return EmitRngGetAndUpdateState(
          Cast<HloRngGetAndUpdateStateInstruction>(hlo_for_lmhlo.at(op)));
    }
    return EmitRngGetAndUpdateState(op);
  }

  if (mlir::isa<mlir::lmhlo::SortOp>(op)) {
    return EmitSort(op, Cast<HloSortInstruction>(hlo_for_lmhlo.at(op)));
  }

  if (mlir::isa<mlir::lmhlo::ReplicaIdOp>(op)) {
    if (ir_emitter_context_->emit_ir_from_hlo()) {
      return EmitReplicaOrPartitionId<ReplicaIdThunk>(hlo_for_lmhlo.at(op));
    }
    return EmitReplicaOrPartitionId<ReplicaIdThunk, mlir::lmhlo::ReplicaIdOp>(
        op);
  }

  if (mlir::isa<mlir::lmhlo::PartitionIdOp>(op)) {
    if (ir_emitter_context_->emit_ir_from_hlo()) {
      return EmitReplicaOrPartitionId<PartitionIdThunk>(hlo_for_lmhlo.at(op));
    }
    return EmitReplicaOrPartitionId<PartitionIdThunk,
                                    mlir::lmhlo::PartitionIdOp>(op);
  }

  if (mlir::isa<mlir::lmhlo_gpu::CollectivePermuteStartOp>(op)) {
    if (ir_emitter_context_->emit_ir_from_hlo()) {
      return EmitCollectivePermute(
          Cast<HloCollectivePermuteInstruction>(hlo_for_lmhlo.at(op)));
    }
    return EmitCollectivePermute(op);
  }

  if (mlir::isa<mlir::lmhlo_gpu::CollectivePermuteDoneOp>(op)) {
    if (ir_emitter_context_->emit_ir_from_hlo()) {
      return EmitNcclAsyncDone(Thunk::kNcclCollectivePermuteDone,
                               hlo_for_lmhlo.at(op));
    }
    return EmitNcclAsyncDone(
        Thunk::kNcclCollectivePermuteDone, op,
        mlir::cast<mlir::lmhlo_gpu::CollectivePermuteDoneOp>(op).getToken());
  }

  if (mlir::isa<mlir::lmhlo_gpu::AllGatherStartOp>(op)) {
    if (ir_emitter_context_->emit_ir_from_hlo()) {
      auto* all_gather = Cast<HloAllGatherInstruction>(hlo_for_lmhlo.at(op));
      return EmitNcclThunk<NcclAllGatherStartThunk, HloAllGatherInstruction>(
          Thunk::kNcclAllGatherStart, all_gather, all_gather,
          all_gather->use_global_device_ids());
    }
    return EmitNcclThunk<NcclAllGatherStartThunk,
                         mlir::lmhlo_gpu::AllGatherStartOp>(op);
  }

  if (mlir::isa<mlir::lmhlo_gpu::AllGatherDoneOp>(op)) {
    if (ir_emitter_context_->emit_ir_from_hlo()) {
      return EmitNcclAsyncDone(Thunk::kNcclAllGatherDone, hlo_for_lmhlo.at(op));
    }
    return EmitNcclAsyncDone(
        Thunk::kNcclAllGatherDone, op,
        mlir::cast<mlir::lmhlo_gpu::AllGatherDoneOp>(op).getToken());
  }

  if (mlir::isa<mlir::lmhlo_gpu::AllReduceStartOp>(op)) {
    if (ir_emitter_context_->emit_ir_from_hlo()) {
      auto* all_reduce = Cast<HloAllReduceInstruction>(hlo_for_lmhlo.at(op));
      return EmitNcclThunk<NcclAllReduceStartThunk, HloAllReduceInstruction>(
          Thunk::kNcclAllReduceStart, all_reduce, all_reduce,
          all_reduce->use_global_device_ids());
    }
    return EmitNcclThunk<NcclAllReduceStartThunk,
                         mlir::lmhlo_gpu::AllReduceStartOp>(op);
  }

  if (mlir::isa<mlir::lmhlo_gpu::AllReduceDoneOp>(op)) {
    if (ir_emitter_context_->emit_ir_from_hlo()) {
      return EmitNcclAsyncDone(Thunk::kNcclAllReduceDone, hlo_for_lmhlo.at(op));
    }
    return EmitNcclAsyncDone(
        Thunk::kNcclAllReduceDone, op,
        mlir::cast<mlir::lmhlo_gpu::AllReduceDoneOp>(op).getToken());
  }

  if (mlir::isa<mlir::lmhlo_gpu::ReduceScatterStartOp>(op)) {
    if (ir_emitter_context_->emit_ir_from_hlo()) {
      auto* async_start = hlo_for_lmhlo.at(op);
      auto* reduce_scatter = Cast<HloReduceScatterInstruction>(
          async_start->async_wrapped_instruction());
      return EmitNcclThunk<NcclReduceScatterStartThunk,
                           HloReduceScatterInstruction>(
          Thunk::kNcclReduceScatterStart, async_start, reduce_scatter,
          reduce_scatter->use_global_device_ids());
    }
    return EmitNcclThunk<NcclReduceScatterStartThunk,
                         mlir::lmhlo_gpu::ReduceScatterStartOp>(op);
  }

  if (mlir::isa<mlir::lmhlo_gpu::ReduceScatterDoneOp>(op)) {
    if (ir_emitter_context_->emit_ir_from_hlo()) {
      return EmitNcclAsyncDone(Thunk::kNcclReduceScatterDone,
                               hlo_for_lmhlo.at(op));
    }
    return EmitNcclAsyncDone(
        Thunk::kNcclReduceScatterDone, op,
        mlir::cast<mlir::lmhlo_gpu::ReduceScatterDoneOp>(op).getToken());
  }

  if (mlir::isa<mlir::lmhlo_gpu::AllToAllStartOp>(op)) {
    return EmitNcclThunk<NcclAllToAllStartThunk,
                         mlir::lmhlo_gpu::AllToAllStartOp>(op);
  }

  if (mlir::isa<mlir::lmhlo_gpu::AllToAllDoneOp>(op)) {
    return EmitNcclAsyncDone(
        Thunk::kNcclAllToAllDone, op,
        mlir::cast<mlir::lmhlo_gpu::AllToAllDoneOp>(op).getToken());
  }

  if (mlir::isa<mlir::lmhlo::InfeedOp>(op)) {
    if (ir_emitter_context_->emit_ir_from_hlo()) {
      return EmitInfeed(Cast<HloInfeedInstruction>(hlo_for_lmhlo.at(op)));
    }
    return EmitInfeed(op);
  }

  if (mlir::isa<mlir::lmhlo::OutfeedOp>(op)) {
    if (ir_emitter_context_->emit_ir_from_hlo()) {
      return EmitOutfeed(Cast<HloOutfeedInstruction>(hlo_for_lmhlo.at(op)));
    }
    return EmitOutfeed(op);
  }

  if (mlir::isa<mlir::lmhlo::CaseOp>(op)) {
    return EmitConditional(op, hlo_for_lmhlo);
  }

  if (mlir::isa<mlir::lmhlo::WhileOp>(op)) {
    // TODO(ezhulenev): While loops may contain instructions that do not support
    // emitting from HLO, so we can't yet enable while thunk emission here.
    static constexpr bool kWhileThunkNotSupported = true;
    if (ir_emitter_context_->emit_ir_from_hlo() && !kWhileThunkNotSupported) {
      return EmitWhile(hlo_for_lmhlo.at(op));
    }
    return EmitWhile(op, hlo_for_lmhlo);
  }

  // Remaining arith.constant ops are the gpu.launch_func dimensions as a result
  // of inlining the fusion region after lowering. They can safely be skipped
  // because constants have no side effects.
  if (mlir::isa<mlir::arith::ConstantOp>(op)) {
    return absl::OkStatus();
  }

  if (mlir::isa<mlir::lmhlo::CommandBufferOp>(op)) {
    return EmitCommandBufferThunk(hlo_for_lmhlo.at(op));
  }

  // In GPU runtime point-to-point communications implemented as runtime custom
  // calls, and we do not need real thunks to construct them, so we can emit
  // stubs that always fail. This is deprecated and will be removed in Q1 2024.
  if (is_gpu_runtime &&
      mlir::isa<mlir::lmhlo::SendOp, mlir::lmhlo::RecvOp,
                mlir::lmhlo::SendDoneOp, mlir::lmhlo::RecvDoneOp>(op)) {
    return EmitUnreachable(op,
                           "Point-to-point communication operations are not "
                           "implemented as thunks");
  }

  if (mlir::isa<mlir::lmhlo::SendOp>(op)) {
    return EmitSendThunk(Cast<HloSendInstruction>(hlo_for_lmhlo.at(op)));
  }

  if (mlir::isa<mlir::lmhlo::SendDoneOp>(op)) {
    return EmitSendDoneThunk(
        Cast<HloSendDoneInstruction>(hlo_for_lmhlo.at(op)));
  }

  if (mlir::isa<mlir::lmhlo::RecvOp>(op)) {
    return EmitRecvThunk(Cast<HloRecvInstruction>(hlo_for_lmhlo.at(op)));
  }

  if (mlir::isa<mlir::lmhlo::RecvDoneOp>(op)) {
    return EmitRecvDoneThunk(
        Cast<HloRecvDoneInstruction>(hlo_for_lmhlo.at(op)));
  }

  return Internal("Unrecognized op: %s", llvm_ir::DumpToString(op));
}

absl::Status IrEmitterUnnested::EmitLmhloRegion(
    mlir::Region* region,
    const absl::flat_hash_map<const mlir::Operation*, const HloInstruction*>&
        hlo_for_lmhlo) {
  for (mlir::Operation& op : llvm::make_early_inc_range(region->front())) {
    TF_RETURN_IF_ERROR(EmitOp(&op, hlo_for_lmhlo));
  }
  return absl::OkStatus();
}

absl::Status IrEmitterUnnested::EmitHloInstruction(
    const HloInstruction* instr) {
  // TODO(anlunx): Support other instruction opcodes.
  switch (instr->opcode()) {
    case HloOpcode::kAllGatherDone:
      return EmitNcclAsyncDone(Thunk::kNcclAllGatherDone, instr);
    case HloOpcode::kAllGatherStart: {
      auto* all_gather = Cast<HloAllGatherInstruction>(instr);
      return EmitNcclThunk<NcclAllGatherStartThunk, HloAllGatherInstruction>(
          Thunk::kNcclAllGatherStart, all_gather, all_gather,
          all_gather->use_global_device_ids());
    }

    case HloOpcode::kAllReduceDone:
      return EmitNcclAsyncDone(Thunk::kNcclAllReduceDone, instr);
    case HloOpcode::kAllReduceStart: {
      auto* all_reduce = Cast<HloAllReduceInstruction>(instr);
      return EmitNcclThunk<NcclAllReduceStartThunk, HloAllReduceInstruction>(
          Thunk::kNcclAllReduceStart, all_reduce, all_reduce,
          all_reduce->use_global_device_ids());
    }

    case HloOpcode::kAsyncDone: {
      const HloInstruction* wrapped = instr->async_wrapped_instruction();
      switch (wrapped->opcode()) {
        case HloOpcode::kReduceScatter:
          return EmitNcclAsyncDone(Thunk::kNcclReduceScatterDone, instr);
        case HloOpcode::kAllToAll:
          return EmitNcclAsyncDone(Thunk::kNcclAllToAllDone, instr);
        default: {
          if (wrapped->has_backend_config()) {
            TF_ASSIGN_OR_RETURN(
                xla::gpu::GpuBackendConfig gpu_config,
                wrapped->backend_config<xla::gpu::GpuBackendConfig>());
            if (gpu_config.operation_queue_id() != 0) {
              // If there an async-done instruction that wraps an instruction
              // that runs on a non-default stream, then we will
              // just emit syncOnStreamThunk().
              return EmitWaitForStreamsThunk(instr, gpu_config,
                                             /*is_async_done=*/true);
            }
          }

          return Internal("Unsupported async done wrapped instruction: %s",
                          HloOpcodeString(wrapped->opcode()));
        }
      }
    }
    case HloOpcode::kAsyncStart: {
      const HloInstruction* wrapped = instr->async_wrapped_instruction();
      switch (wrapped->opcode()) {
        case HloOpcode::kReduceScatter: {
          auto* reduce_scatter = Cast<HloReduceScatterInstruction>(wrapped);
          return EmitNcclThunk<NcclReduceScatterStartThunk,
                               HloReduceScatterInstruction>(
              Thunk::kNcclReduceScatter, instr, reduce_scatter,
              reduce_scatter->use_global_device_ids());
        }
        case HloOpcode::kAllToAll: {
          auto* all_to_all = Cast<HloAllToAllInstruction>(wrapped);
          return EmitNcclThunk<NcclAllToAllStartThunk, HloAllToAllInstruction>(
              Thunk::kNcclAllToAll, instr, all_to_all, std::nullopt);
        }
        default: {
          if (wrapped->has_backend_config()) {
            TF_ASSIGN_OR_RETURN(
                xla::gpu::GpuBackendConfig gpu_config,
                wrapped->backend_config<xla::gpu::GpuBackendConfig>());
            if (gpu_config.operation_queue_id() != 0) {
              // If there an async instruction that wraps an instruction
              // that runs on a non-default stream, then we will
              // emit syncOnStreamThunk(source=execution_stream,
              //                        wait_on=main_compute_stream)
              // then the thunk of wrapped instruction.
              TF_RETURN_IF_ERROR(
                  EmitWaitForStreamsThunk(instr, gpu_config,
                                          /*is_async_done=*/false));
              return EmitHloInstruction(wrapped);
            }
          }
          return Internal("Unsupported async start wrapped instruction: %s",
                          HloOpcodeString(wrapped->opcode()));
        }
      }
    }

    case HloOpcode::kCall:
      return EmitCommandBufferThunk(instr);

    case HloOpcode::kCollectivePermuteDone:
      return EmitNcclAsyncDone(Thunk::kNcclCollectivePermuteDone, instr);
    case HloOpcode::kCollectivePermuteStart:
      return EmitCollectivePermute(
          Cast<HloCollectivePermuteInstruction>(instr));

    case HloOpcode::kConditional:
      return EmitConditional(instr);
    case HloOpcode::kConstant:
      return EmitConstant(Cast<HloConstantInstruction>(instr));
    case HloOpcode::kCustomCall: {
      auto* custom_call = Cast<HloCustomCallInstruction>(instr);
      if (IsLegacyCublasMatmul(*instr)) {
        return EmitGemmThunk(custom_call);
      }
#if GOOGLE_CUDA || TF_HIPBLASLT
      if (IsCublasLtMatmul(*instr)) {
        return EmitCublasLtMatmulThunk(custom_call);
      }
#endif  // GOOGLE_CUDA || TF_HIPBLASLT
#if GOOGLE_CUDA
      if (IsCublasLtMatmulF8(*instr)) {
        return EmitCublasLtMatmulThunkF8(custom_call);
      }
      if (IsCudnnConvolutionReorder(*instr)) {
        return EmitConvolutionReorderThunk(custom_call);
      }
      if (IsCustomCallToDnnNorm(*instr)) {
        return EmitNormThunk(custom_call);
      }
      if (IsFwdCustomCallTofMHA(*instr)) {
        return EmitFusedMHAThunk(custom_call);
      }
#endif  // GOOGLE_CUDA
      if (IsCustomCallToTopK(*instr)) {
        return EmitTopKCustomCall(custom_call);
      }
      if (IsCustomCallToDnnConvolution(*instr)) {
        return EmitConvolutionThunk(custom_call);
      }
#if GOOGLE_CUDA || TENSORFLOW_USE_ROCM
      if (IsCustomCallToCusolver(*instr)) {
        return EmitCholeskyThunk(instr);
      }
      if (IsTriangularSolve(*instr)) {
        return EmitTriangularSolveCustomCall(instr);
      }
      if (IsCubDeviceRadixSort(*instr)) {
        return EmitCubDeviceRadixSort(custom_call);
      }
#endif  // GOOGLE_CUDA || TENSORFLOW_USE_ROCM
      if (custom_call->custom_call_target() == "PadToStatic") {
        return EmitPadToStatic(custom_call);
      }
      if (instr->custom_call_target() == "SliceToDynamic") {
        return EmitSliceToDynamic(custom_call);
      }
      return EmitCustomCallThunk(custom_call);
    }
    case HloOpcode::kFusion: {
      auto* fusion = Cast<HloFusionInstruction>(instr);
      const se::DeviceDescription& device_info =
          ir_emitter_context_->gpu_device_info();
      auto fusion_analysis = HloFusionAnalysis::Create(fusion, &device_info);
      return EmitFusion(fusion, fusion_analysis);
    }
    case HloOpcode::kInfeed:
      return EmitInfeed(Cast<HloInfeedInstruction>(instr));
    case HloOpcode::kOutfeed:
      return EmitOutfeed(Cast<HloOutfeedInstruction>(instr));
    case HloOpcode::kPartitionId:
      return EmitReplicaOrPartitionId<PartitionIdThunk>(instr);
    case HloOpcode::kFft:
      return EmitFftThunk(Cast<HloFftInstruction>(instr));

    case HloOpcode::kRecv:
      return EmitRecvThunk(Cast<HloRecvInstruction>(instr));
    case HloOpcode::kRecvDone:
      return EmitRecvDoneThunk(Cast<HloRecvDoneInstruction>(instr));

    case HloOpcode::kReplicaId:
      return EmitReplicaOrPartitionId<ReplicaIdThunk>(instr);
    case HloOpcode::kRngGetAndUpdateState:
      return EmitRngGetAndUpdateState(
          Cast<HloRngGetAndUpdateStateInstruction>(instr));
    case HloOpcode::kSelectAndScatter:
      return EmitSelectAndScatter(Cast<HloSelectAndScatterInstruction>(instr));

    case HloOpcode::kSend:
      return EmitSendThunk(Cast<HloSendInstruction>(instr));
    case HloOpcode::kSendDone:
      return EmitSendDoneThunk(Cast<HloSendDoneInstruction>(instr));

    case HloOpcode::kSort:
      return EmitSort(Cast<HloSortInstruction>(instr));
    case HloOpcode::kWhile:
      return EmitWhile(instr);

    // HLO module is already ordered, so kAfterAll is a noop.
    case HloOpcode::kAfterAll:
    // We don't need to emit thunks for these operations because their semantics
    // are encoded by buffers.
    case HloOpcode::kBitcast:
    case HloOpcode::kGetTupleElement:
    case HloOpcode::kParameter:
    case HloOpcode::kTuple:
      return absl::OkStatus();
    default:
      return Internal("Unsupported instruction opcode: %s",
                      HloOpcodeString(instr->opcode()));
  }

  return Internal("Unhandled HLO instruction");
}

absl::Status IrEmitterUnnested::EmitHloComputation(
    const HloComputation* computation) {
  const HloSchedule& schedule = computation->parent()->schedule();
  if (!schedule.is_computation_scheduled(computation))
    return Internal("Sequence not found for computation: %s",
                    computation->name());

  const HloInstructionSequence& sequence = schedule.sequence(computation);
  for (HloInstruction* instr : sequence.instructions()) {
    TF_RETURN_IF_ERROR(EmitHloInstruction(instr));
  }
  return absl::OkStatus();
}

void IrEmitterUnnested::GetDependentDialects(mlir::DialectRegistry& registry) {
  registry.insert<mlir::arith::ArithDialect, mlir::func::FuncDialect,
                  mlir::gpu::GPUDialect, mlir::lmhlo::LmhloDialect,
                  mlir::lmhlo_gpu::LmhloGpuDialect, mlir::mhlo::MhloDialect,
                  mlir::memref::MemRefDialect>();
  mlir::registerBuiltinDialectTranslation(registry);
  mlir::registerLLVMDialectTranslation(registry);
  mlir::registerNVVMDialectTranslation(registry);
  mlir::registerROCDLDialectTranslation(registry);
  mlir::func::registerAllExtensions(registry);
}

}  // namespace gpu
}  // namespace xla
