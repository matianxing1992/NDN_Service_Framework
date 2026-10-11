#ifndef NDNSF_DI_DETAIL_NATIVE_TENSOR_TRANSPORT_DECLARATION_HPP
#define NDNSF_DI_DETAIL_NATIVE_TENSOR_TRANSPORT_DECLARATION_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include <cstdint>
#include <exception>
#include <string>

namespace ndnsf::di::detail {

// Parsing only. The caller must first authenticate the enclosing current ACK.
// A V4 caller also supplies its independently admitted boot identity.
inline std::uint64_t
nativeTensorTransportSegmentBytesV1(const std::string& wire,
                                    std::uint64_t maxPacketBytes,
                                    const std::string& expectedBoot = {}) noexcept
{
  try {
    if (wire.empty() || wire.size() > 256) return 0;
    const auto value = nativeParseJson(wire);
    if (!value.is_object() || value.size() != 3 ||
        !value.contains("schema") || !value.contains("boot_epoch") ||
        !value.contains("segment_payload_bytes") ||
        value.at("schema") != "ndnsf-di-tensor-transport-v1" ||
        !value.at("boot_epoch").is_string() ||
        value.at("boot_epoch").get<std::string>().empty() ||
        (!expectedBoot.empty() && value.at("boot_epoch") != expectedBoot) ||
        !value.at("segment_payload_bytes").is_number_unsigned()) return 0;
    const auto bytes = value.at("segment_payload_bytes").get<std::uint64_t>();
    return bytes != 0 && bytes <= maxPacketBytes ? bytes : 0;
  }
  catch (const std::exception&) {
    return 0;
  }
}

} // namespace ndnsf::di::detail
#endif
