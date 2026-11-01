#pragma once

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCheckpointExport.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePortableKv.hpp"

namespace ndnsf::di {

struct NativePortableKvCheckpointLimitsV1
{
  NativePortableKvLimitsV1 portable;
  std::uint64_t maxMetadataBytes = 0;
  std::uint64_t maxCheckpointBytes = 0;
};

// Same limits check used before private export and by configuration preflight.
void validateNativePortableKvCheckpointLimits(
  const NativePortableKvCheckpointLimitsV1& limits);

// Private plaintext only. Caller owns immutable snapshot/directory through return.
void nativeExportPrivatePortableKvCheckpoint(
  const std::filesystem::path& destination,
  const NativePortableKvSnapshotV1& snapshot,
  const NativePortableKvCheckpointLimitsV1& limits,
  const NativeCheckpointExportOptions& options = {});

// Representation validation does not authorize execution or persistent storage.
NativePortableKvSnapshotV1 nativeReadPrivatePortableKvCheckpoint(
  const std::filesystem::path& source,
  const NativePortableKvCheckpointLimitsV1& limits);

} // namespace ndnsf::di
