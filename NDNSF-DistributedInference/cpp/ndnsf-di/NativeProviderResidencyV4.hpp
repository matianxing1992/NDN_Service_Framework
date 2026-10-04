#pragma once

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ndnsf::di {

constexpr std::size_t MAX_NATIVE_PLACEMENT_CANDIDATE_ARTIFACTS = 32;

/** Request-scoped cache evidence. `known=false` means at least one owner
 * cannot answer the exact candidate query; an empty known snapshot is distinct. */
struct NativeProviderResidencySnapshotV4
{
  bool known = false;
  std::vector<NativeJson> rows;
};

enum class NativeProviderResidencyOwnerV4
{
  ProviderArtifactCache,
  RunnerReuseCache,
  OnnxRuntimeSessionCache,
  ProtectedPlaintextCache,
  PersistentRecipeCache
};

struct NativeProviderResidencyOwnerResultV4
{
  NativeProviderResidencyOwnerV4 owner;
  bool complete = false;
  std::vector<NativeJson> rows;
};

/** Placement identity retained by provider-local cache owners. The backend
 * is the concrete resident backend; the request carries its declared base. */
struct NativeProviderResidencyIdentityV4
{
  std::string artifactDigest;
  std::string modelManifestDigest;
  std::string graphDigest;
  std::string role;
  std::uint64_t rank = 0;
  std::string backend;
  std::string protectionEpoch;
  std::vector<std::string> deviceSet;

  bool isValid() const
  {
    const auto validDigest = [] (const std::string& value) {
      return value.size() == 71 && value.compare(0, 7, "sha256:") == 0 &&
        std::all_of(value.begin() + 7, value.end(), [] (char ch) {
          return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
        });
    };
    if (!validDigest(artifactDigest) || !validDigest(modelManifestDigest) ||
        !validDigest(graphDigest) || role.empty() || backend.empty() ||
        protectionEpoch.empty() || deviceSet.empty())
      return false;
    std::string previous;
    for (const auto& device : deviceSet) {
      if (device.empty() || (!previous.empty() && previous >= device))
        return false;
      previous = device;
    }
    return true;
  }

  NativeJson candidateArtifact() const
  {
    return NativeJson{{"artifact_digest", artifactDigest},
      {"model_manifest_digest", modelManifestDigest},
      {"graph_digest", graphDigest}, {"role", role}, {"rank", rank},
      {"backend", backend}, {"protection_epoch", protectionEpoch}};
  }

  bool matchesCandidateArtifact(const NativeJson& candidate) const
  {
    if (!isValid() || !candidate.is_object() || candidate.size() != 7)
      return false;
    for (const auto* field : {"artifact_digest", "model_manifest_digest", "graph_digest",
                              "role", "rank", "protection_epoch"}) {
      if (!candidate.contains(field) || candidate.at(field) != candidateArtifact().at(field))
        return false;
    }
    if (!candidate.contains("backend") || !candidate.at("backend").is_string())
      return false;
    const auto requested = candidate.at("backend").get<std::string>();
    return backend == requested || backend == requested + "-cpu" ||
      backend == requested + "-cuda";
  }

  NativeJson makeRow(const std::string& state,
                     const std::string& processEpoch,
                     std::uint64_t capturedAtMs,
                     std::uint64_t expiresAtMs,
                     const NativeJson& missingVerifiedBytes,
                     const NativeJson& estimatedAssemblyMs,
                     const NativeJson& estimatedLoadMs,
                     const NativeJson& runtimeGeneration) const
  {
    auto row = candidateArtifact();
    row["device_set"] = deviceSet;
    row["state"] = state;
    row["process_epoch"] = processEpoch;
    row["captured_at_ms"] = capturedAtMs;
    row["expires_at_ms"] = expiresAtMs;
    row["missing_verified_bytes"] = missingVerifiedBytes;
    row["estimated_assembly_ms"] = estimatedAssemblyMs;
    row["estimated_load_ms"] = estimatedLoadMs;
    row["runtime_generation"] = runtimeGeneration;
    return row;
  }
};

} // namespace ndnsf::di
