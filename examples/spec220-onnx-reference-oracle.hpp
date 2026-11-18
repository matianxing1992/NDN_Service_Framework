#ifndef NDNSF_SPEC220_ONNX_REFERENCE_ORACLE_HPP
#define NDNSF_SPEC220_ONNX_REFERENCE_ORACLE_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace spec220::oracle {

struct CompleteKvCapture
{
  std::filesystem::path directory;
  std::string provider;
  std::string boot;
  const std::vector<ndnsf::di::NativeJson>& requests;
};

/** Independently replay pinned greedy cold/warm inputs through the unsplit CPU model.
 * Authenticates checkpoints and, when supplied, compares complete Provider KV evidence.
 * `absoluteTolerance` is the predeclared absolute term for floating point observations.
 * Deployment key-file paths must be absolute.
 */
ndnsf::di::NativeJson
validateOnnxReference(const ndnsf::di::NativeJson& requester,
                      const std::string& expectedRuntimeVersion,
                      const std::vector<CompleteKvCapture>& captures = {},
                      double absoluteTolerance = 1e-5);

} // namespace spec220::oracle

#endif // NDNSF_SPEC220_ONNX_REFERENCE_ORACLE_HPP
