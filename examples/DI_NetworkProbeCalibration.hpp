#pragma once

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include <cstddef>
#include <string>

namespace ndn_service_framework { class ResponseMessage; }

// Private calibration-tool operations shared with its C++ behavior oracle.
namespace ndnsf::di::probe_calibration {
bool isValidRepetitionBudget(unsigned repeats, unsigned warmup, unsigned intervalMs) noexcept;
bool hasAuthenticatedSigner(const ndn_service_framework::ResponseMessage& response,
                            const std::string& expectedProvider) noexcept;
NativeJson recordFromResponse(const NativeJson& response, const NativeJson& request,
                              std::size_t round);
NativeJson summarize(NativeJson records, double referenceBandwidthMbps);
NativeJson summarizeMatrix(const NativeJson& cells);
}
