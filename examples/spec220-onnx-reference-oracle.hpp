#ifndef NDNSF_SPEC220_ONNX_REFERENCE_ORACLE_HPP
#define NDNSF_SPEC220_ONNX_REFERENCE_ORACLE_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"

#include <string>

namespace spec220::oracle {

/** Independently replay pinned greedy cold/warm inputs through the unsplit CPU model.
 * Authenticates checkpoints with the configured private journal keys, but does not
 * compare provider-resident state values. Deployment key-file paths must be absolute.
 */
ndnsf::di::NativeJson
validateOnnxReference(const ndnsf::di::NativeJson& requester,
                      const std::string& expectedRuntimeVersion);

} // namespace spec220::oracle

#endif // NDNSF_SPEC220_ONNX_REFERENCE_ORACLE_HPP
