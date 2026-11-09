#ifndef NDNSF_DI_NATIVE_OWNED_INITIALIZER_FILE_HPP
#define NDNSF_DI_NATIVE_OWNED_INITIALIZER_FILE_HPP

#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.hpp"

namespace ndnsf::di {

/** Consume a private temporary initializer, verifying its digest before return.
 * The file must be caller-owned, mode0600 and singly linked. Its pathname is
 * removed; the returned owner retains a read-only descriptor. When the kernel
 * can establish the absence of writers, the unlinked snapshot reuses its
 * computed digest. Otherwise digest calls retain full verification. This is
 * not intended for a caller's durable model or shared source pathname.
 */
std::shared_ptr<const NativeCanonicalByteRangeSource>
takeNativeCanonicalInitializerFile(const std::string& path, std::uint64_t bytes,
  const std::string& expectedDigest, const NativeAssemblyControl& control);

} // namespace ndnsf::di

#endif // NDNSF_DI_NATIVE_OWNED_INITIALIZER_FILE_HPP
