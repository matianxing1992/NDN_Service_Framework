#include "NDNSF-DistributedInference/cpp/adapters/onnx/automatic-onnx-catalog.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"

#include <stdexcept>

namespace ndnsf::di {

NativeJson
automaticOnnxCatalogJson(const AutomaticOnnxModel& model,
                         const AutomaticOnnxCatalogSpec& spec,
                         const NativeAssemblyControl& control)
{
  control.requireActive();
  if (spec.sourceFile.empty() || spec.dataName.empty() ||
      spec.modelManifestDigest.empty() || spec.artifactProfileDigest.empty() ||
      spec.assemblerDescriptorDigest.empty())
    throw std::invalid_argument("automatic catalog identity is incomplete");
  NativeJson catalog;
  catalog["schema"] = "ndnsf-di-native-request-catalog-v1";
  catalog["model"] = nativeParseJson(model.descriptor.canonicalJson());
  catalog["source"] = {
    {"file", spec.sourceFile},
    {"data_name", spec.dataName},
    {"digest", model.opened.modelDigest},
    {"bytes", spec.sourceBytes != 0 ? spec.sourceBytes :
      std::uint64_t(model.opened.source.modelBytes.size())},
    {"model_manifest_digest", spec.modelManifestDigest},
    {"canonical_graph_digest", model.inspection.canonicalIdentity.graphDigest}};
  if (!spec.initializerFile.empty()) {
    if (spec.initializerDigest.empty() || model.opened.externalFiles.size() > 1)
      throw std::invalid_argument("automatic catalog external initializer has no digest");
    catalog["source"]["initializer_file"] = spec.initializerFile;
    catalog["source"]["initializer_digest"] = spec.initializerDigest;
    // A pinned initializer object may be declared without a model-level external
    // file (the caller authenticates the object separately); only a discovered
    // single external file contributes its byte count here.
    if (model.opened.externalFiles.size() == 1)
      catalog["source"]["initializer_bytes"] = model.opened.externalFiles.front().bytes;
  }
  catalog["recipe"] = {
    {"artifact_profile_digest", spec.artifactProfileDigest},
    {"assembler_descriptor_digest", spec.assemblerDescriptorDigest},
    {"backend_abi", spec.backendAbi},
    {"precision", model.descriptor.precision},
    {"quantization", model.descriptor.quantizationSubtype},
    {"layout", spec.layout},
    {"padding", "none"},
    {"protection_epoch", spec.protectionEpoch},
    {"max_source_bytes", spec.maxSourceBytes},
    {"max_assembled_bytes", spec.maxAssembledBytes},
    {"max_nodes", spec.maxNodes}};
  catalog["publication"] = {{"artifact_root", "/ndnsf/di/automatic/artifacts"}};
  catalog["input_format"] = "JSON";
  catalog["max_payload_bytes"] = std::uint64_t{64};
  catalog["splitter"] = {{"kind", "AUTOMATIC"}};
  return catalog;
}

} // namespace ndnsf::di
