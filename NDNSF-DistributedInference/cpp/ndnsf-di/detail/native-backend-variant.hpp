#ifndef NDNSF_DI_DETAIL_NATIVE_BACKEND_VARIANT_HPP
#define NDNSF_DI_DETAIL_NATIVE_BACKEND_VARIANT_HPP

#include <string_view>

namespace ndnsf::di {

/** A generic declaration admits either concrete ORT backend. */
inline bool
matchesNativeBackendVariant(std::string_view declared, std::string_view actual) noexcept
{
  return !declared.empty() &&
    (declared == actual || (declared == "onnxruntime" &&
      (actual == "onnxruntime-cpu" || actual == "onnxruntime-cuda")));
}

} // namespace ndnsf::di

#endif
