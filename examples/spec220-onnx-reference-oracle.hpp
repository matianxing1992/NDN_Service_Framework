#ifndef NDNSF_SPEC220_ONNX_REFERENCE_ORACLE_HPP
#define NDNSF_SPEC220_ONNX_REFERENCE_ORACLE_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"

#include <string>

namespace spec220::oracle {

/** Independently replay pinned greedy cold/warm inputs through the unsplit CPU model.
 * Does not authenticate checkpoints or compare provider-resident state values.
 */
ndnsf::di::NativeJson
validateOnnxReference(const ndnsf::di::NativeJson& requester,
                      const std::string& expectedRuntimeVersion);

} // namespace spec220::oracle

#endif // NDNSF_SPEC220_ONNX_REFERENCE_ORACLE_HPP
