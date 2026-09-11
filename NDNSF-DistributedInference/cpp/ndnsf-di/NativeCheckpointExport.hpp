#pragma once

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"

#include <filesystem>
#include <functional>

namespace ndnsf::di {

enum class NativeCheckpointExportStage {
  BeforeWrite,
  BeforeFileSync,
  BeforeRename,
  BeforeDirectorySync,
};

struct NativeCheckpointExportOptions
{
  // Test-only fault/observation hook. Production callers leave it empty.
  std::function<void(NativeCheckpointExportStage)> beforeStage;
};

/**
 * Atomically export a canonical conversation state owned by the native
 * requester. The destination is never followed when it is a symlink; a
 * same-directory temporary file is created with mode 0600, synced, renamed,
 * and followed by a directory fsync.
 */
void nativeExportPrivateCheckpoint(
  const std::filesystem::path& destination,
  const NativeJson& state,
  const NativeCheckpointExportOptions& options = {});

} // namespace ndnsf::di
