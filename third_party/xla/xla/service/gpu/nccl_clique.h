/* Copyright 2024 The OpenXLA Authors.

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

#ifndef XLA_SERVICE_GPU_NCCL_CLIQUE_H_
#define XLA_SERVICE_GPU_NCCL_CLIQUE_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/node_hash_map.h"
#include "absl/functional/function_ref.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
#include "xla/executable_run_options.h"
#include "xla/service/global_device_id.h"
#include "xla/service/gpu/nccl_api.h"
#include "xla/service/gpu/nccl_clique_key.h"
#include "xla/service/lockable.h"
#include "tsl/lib/gtl/int_type.h"

namespace xla::gpu {

// NCCL clique (collective clique) is a set of devices that execute collective
// operations (e.g. all-reduce). It is notoriously easy to misuse NCCL
// communicators (see link below) and get a dead lock at run time, so in XLA we
// take extra care to order all collective operations in a way that would not
// lead to a deadlock.
//
// We rely on exclusive access to a NCCL clique (using Lockable<T> mechanism) to
// guarantee that only a set of threads executing a particular collective
// operation can schedule new work using communicators belonging to a clique.
//
// In XLA process we have multiple cliques for different combinations of
// participating devices and properties of collective operations launched on
// them, e.g. mixing NCCL operations launched from CUDA graphs with regularly
// launched operations is prone to dead locks, and we keep them separate. See
// NcclCliqueKey for details.
//
// https://docs.nvidia.com/deeplearning/nccl/user-guide/docs/usage/communicators.html#using-multiple-nccl-communicators-concurrently

// Forward declare.
class NcclCliqueCommunicators;

//===----------------------------------------------------------------------===//
// NcclUniqueId
//===----------------------------------------------------------------------===//

// Returns true if the NCCL config is global (NCCL_COMM_ID env variable is set).
bool IsGlobalNcclConfig();

// Returns a clique id callback passed as an argument if it's not null or a
// default callback to get create a clique id if we are running in local mode.
absl::StatusOr<const NcclCliqueIdCallback*> GetNcclCliqueIdCallback(
    const NcclCliqueIdCallback* clique_id_callback,  // may be null
    bool is_local);

//===----------------------------------------------------------------------===//
// NcclComm
//===----------------------------------------------------------------------===//

// TODO(b/319655685): Lockable NcclComm should be deleted and NcclClique should
// become the owner of all communicators making up a clique and responsible for
// synchronizing access to communicators.

TSL_LIB_GTL_DEFINE_INT_TYPE(OpId, int64_t);

struct NcclCommName {
  static std::string ToString(NcclApi::NcclCommHandle comm) {
    return absl::StrFormat("lockable comm %p", comm);
  }
};

struct NcclComm : public Lockable<NcclApi::NcclCommHandle, NcclCommName> {
  friend class NcclCliqueCommunicators;

  explicit NcclComm(NcclApi::NcclCommHandle comm) : Lockable(comm) {}
};

//===----------------------------------------------------------------------===//
// NcclClique
//===----------------------------------------------------------------------===//

// A group of NCCL communicators making up a clique. With NCCL it's notoriously
// easy to get a deadlock, so we take extra care by grouping communicators into
// cliques and making sure that we have a well defined order of all collective
// operations that does not lead to deadlocks.
class NcclCliqueCommunicators {
 public:
  NcclCliqueCommunicators(NcclCliqueKey clique_key, NcclCliqueId,
                          absl::node_hash_map<int32_t, NcclComm> communicators);

  // Returns a NCCL communicator for a given rank if it's in a clique.
  std::optional<NcclComm*> comm(int32_t rank);

  // Calls `fn` for each communicator in the clique.
  void ForEachComm(absl::FunctionRef<void(int32_t, NcclComm&)> fn);

  const NcclCliqueKey& clique_key() const { return clique_key_; }
  const NcclCliqueId& clique_id() const { return clique_id_; }
  size_t size() const { return communicators_.size(); }

  std::string DebugString() const;

 private:
  NcclCliqueKey clique_key_;
  NcclCliqueId clique_id_;

  // TODO(ezhulenev): Switch this map to GlobalDeviceId key.
  absl::node_hash_map<int32_t, NcclComm> communicators_;
};

struct NcclCliqueName {
  static std::string ToString(const NcclCliqueCommunicators& comms) {
    return absl::StrFormat("lockable clique %s", comms.clique_key().ToString());
  }
};

struct NcclClique : public Lockable<NcclCliqueCommunicators, NcclCliqueName> {
  NcclClique(NcclCliqueKey clique_key, NcclCliqueId clique_id,
             absl::node_hash_map<int32_t, NcclComm> communicators)
      : Lockable(NcclCliqueCommunicators{std::move(clique_key), clique_id,
                                         std::move(communicators)}) {}

  std::string DebugString() const;
};

// Acquires an shared access to a NCCL clique (NcclClique::Lock collectively
// owned by `num_local_participants` threads). XLA uses this lock to serialize
// execution of all collective operations sharing a `clique_id`.
absl::StatusOr<std::shared_ptr<NcclClique::Lock>> AcquireNcclClique(
    RunId run_id, NcclCliqueKey clique_key,
    const NcclCliqueIdCallback& clique_id_callback, int32_t rank,
    size_t num_local_participants, bool may_skip_rendezvous);

}  // namespace xla::gpu

#endif  // XLA_SERVICE_GPU_NCCL_CLIQUE_H_
