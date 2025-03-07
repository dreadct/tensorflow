/* Copyright 2015 The OpenXLA Authors.

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

#include "xla/stream_executor/kernel.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "absl/memory/memory.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "absl/strings/strip.h"
#include "xla/stream_executor/platform.h"
#include "xla/stream_executor/stream_executor.h"
#include "xla/stream_executor/stream_executor_internal.h"
#include "tsl/platform/demangle.h"
#include "tsl/platform/errors.h"

namespace stream_executor {

std::optional<int64_t> KernelMetadata::registers_per_thread() const {
  return registers_per_thread_;
}

std::optional<int64_t> KernelMetadata::shared_memory_bytes() const {
  return shared_memory_bytes_;
}

void KernelMetadata::set_registers_per_thread(int registers_per_thread) {
  registers_per_thread_ = registers_per_thread;
}

void KernelMetadata::set_shared_memory_bytes(int shared_memory_bytes) {
  shared_memory_bytes_ = shared_memory_bytes;
}

//===----------------------------------------------------------------------===//
// Kernel
//===----------------------------------------------------------------------===//

absl::StatusOr<std::unique_ptr<Kernel>> Kernel::Create(
    StreamExecutor *executor, const MultiKernelLoaderSpec &spec) {
  auto kernel = absl::WrapUnique(new Kernel(executor));
  TF_RETURN_IF_ERROR(executor->GetKernel(spec, kernel.get()));
  return kernel;
}

Kernel::Kernel(StreamExecutor *parent)
    : parent_(parent),
      implementation_(parent->implementation()->CreateKernelImplementation()) {}

Kernel::~Kernel() {
  if (parent_) {
    parent_->UnloadKernel(this);
  }
}

unsigned Kernel::Arity() const { return implementation_->Arity(); }

void Kernel::SetPreferredCacheConfig(KernelCacheConfig config) {
  return implementation_->SetPreferredCacheConfig(config);
}

KernelCacheConfig Kernel::GetPreferredCacheConfig() const {
  return implementation_->GetPreferredCacheConfig();
}

absl::StatusOr<int32_t> Kernel::GetMaxOccupiedBlocksPerCore(
    ThreadDim threads, size_t dynamic_shared_memory_bytes) const {
  return implementation_->GetMaxOccupiedBlocksPerCore(
      threads, dynamic_shared_memory_bytes);
}

void Kernel::set_name(absl::string_view name) {
  name_ = std::string(name);

  // CUDA splitter prefixes stub functions with __device_stub_.
  demangled_name_ =
      tsl::port::Demangle(absl::StripPrefix(name, "__device_stub_").data());
}

}  // namespace stream_executor
