#ifndef NDNSF_DI_NATIVE_PROTECTED_ARTIFACT_STORE_HPP
#define NDNSF_DI_NATIVE_PROTECTED_ARTIFACT_STORE_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProtectedRuntime.hpp"
#include <filesystem>
#include <functional>

namespace ndnsf::di {
struct NativeAssembledEntryContext
{
  std::string modelManifestDigest;
  std::string roleAssemblySpecDigest;
  std::string storageProfileDigest;
  std::string entryKind = "MODEL_PROTO";
};

// Hash the exact assembly object in the canonical, authenticated Selection wire.
std::string nativeAssemblyDigestFromCanonicalProjection(const std::string& wire);

std::vector<std::uint8_t> sealNativeAssembledEntry(
  const std::vector<std::uint8_t>& contentKey,
  const std::vector<std::uint8_t>& plaintext,
  const NativeAssembledEntryContext& context);

std::vector<std::uint8_t> openNativeAssembledEntry(
  const std::vector<std::uint8_t>& contentKey,
  const std::vector<std::uint8_t>& wire,
  const NativeAssembledEntryContext& expected,
  std::uint64_t maxPlaintextBytes);

// Own an existing private staging directory before any plaintext is written.
// Cleanup walks the pinned directory fd, never following symlinks.
using NativePlaintextFileEraser =
  std::function<void(const std::filesystem::path& file)>;

void registerNativePlaintextDirectory(
  ProtectedRuntime& runtime, const std::filesystem::path& directory,
  const std::string& leaseId);

// Register the same protected directory lease and return an eraser bound to
// its pinned fd.  The eraser is for an authenticated direct child that must be
// removed before the runtime's final directory drain.
NativePlaintextFileEraser registerNativePlaintextDirectoryWithFileEraser(
  ProtectedRuntime& runtime, const std::filesystem::path& directory,
  const std::string& leaseId);

struct NativePlaintextBufferGuard
{
  std::vector<std::uint8_t>& bytes;
  ~NativePlaintextBufferGuard();
};
} // namespace ndnsf::di
#endif
