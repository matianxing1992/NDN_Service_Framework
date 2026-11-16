#include "NDNSF-DistributedInference/cpp/adapters/onnx/automatic-onnx-catalog.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/automatic-onnx-model.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

/**
 * Emit the maintained automatic ONNX request catalog for one self-contained
 * source model.
 *
 * Usage:
 *   spec233-automatic-catalog --model FILE
 *       [--source-file NAME] [--data-name NAME] [--manifest-digest DIGEST]
 *       [--artifact-profile DIGEST] [--assembler DIGEST]
 *       [--protection-epoch EPOCH] [--max-source-bytes N]
 *       [--max-assembled-bytes N] [--max-nodes N]
 *
 * The caller supplies the identities that are not derived from the source
 * bytes; every derived identity comes from the same inspection the runtime
 * uses, so the emitted catalog can be used as a deployment input.
 */
int
main(int argc, char** argv)
{
  ndnsf::di::AutomaticOnnxCatalogSpec spec;
  std::string modelFile;
  for (int index = 1; index < argc; ++index) {
    const std::string option = argv[index];
    const auto value = [&] () -> std::string {
      if (index + 1 >= argc)
        throw std::invalid_argument("missing value for " + option);
      return argv[++index];
    };
    if (option == "--model") modelFile = value();
    else if (option == "--source-file") spec.sourceFile = value();
    else if (option == "--data-name") spec.dataName = value();
    else if (option == "--manifest-digest") spec.modelManifestDigest = value();
    else if (option == "--artifact-profile") spec.artifactProfileDigest = value();
    else if (option == "--assembler") spec.assemblerDescriptorDigest = value();
    else if (option == "--protection-epoch") spec.protectionEpoch = value();
    else if (option == "--max-source-bytes") spec.maxSourceBytes = std::stoull(value());
    else if (option == "--max-assembled-bytes") spec.maxAssembledBytes = std::stoull(value());
    else if (option == "--max-nodes") spec.maxNodes = std::stoull(value());
    else throw std::invalid_argument("unknown option " + option);
  }
  if (modelFile.empty()) {
    std::cerr << "spec233-automatic-catalog: --model is required" << std::endl;
    return 2;
  }
  try {
    const auto start = std::chrono::steady_clock::now();
    ndnsf::di::NativeAssemblyControl control{
      start + std::chrono::minutes(30), [] {}, spec.maxSourceBytes, spec.maxAssembledBytes};
    auto opened = ndnsf::di::openAutomaticOnnxSource(modelFile, control);
    spec.sourceBytes = opened.source.modelBytes.size();
    auto model = ndnsf::di::inspectAutomaticOnnxModel(std::move(opened), control);
    if (!model.opened.externalFiles.empty()) {
      if (model.opened.externalFiles.size() != 1) {
        std::cerr << "spec233-automatic-catalog: more than one external initializer is not "
                     "supported" << std::endl;
        return 3;
      }
      const auto& file = model.opened.externalFiles.front();
      spec.initializerFile = std::filesystem::path(file.location).filename().string();
      spec.initializerDigest = file.digest;
    }
    const auto catalog = ndnsf::di::automaticOnnxCatalogJson(model, spec, control);
    std::cout << ndnsf::di::nativeCanonicalJson(catalog) << std::endl;
    return 0;
  }
  catch (const std::exception& error) {
    std::cerr << "spec233-automatic-catalog: " << error.what() << std::endl;
    return 1;
  }
}
