#ifndef NDNSF_DI_NATIVE_KV_SAVE_HPP
#define NDNSF_DI_NATIVE_KV_SAVE_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/native-kv-handoff.hpp"

namespace ndnsf::di {

/** Validate the actual Source transport owner and its private disk workspace. */
void
nativeValidateKvSaveConfig(const NativeProviderHandoffConfig& config,
  const ndn::Name& producer);

} // namespace ndnsf::di

#endif // NDNSF_DI_NATIVE_KV_SAVE_HPP
