#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRunnerPreparation.hpp"

#include <algorithm>
#include <atomic>
#include <set>
#include <stdexcept>
#include <sstream>
#include <unistd.h>

namespace ndnsf::di {
namespace {

std::vector<std::pair<std::string, std::string>>
parseSuccessorMap(const std::string& encoded)
{
  std::vector<std::pair<std::string, std::string>> result;
  std::size_t start = 0;
  while (start < encoded.size()) {
    const auto end = encoded.find(',', start);
    const auto item = encoded.substr(start, end == std::string::npos
      ? std::string::npos : end - start);
    const auto separator = item.find('=');
    if (separator == std::string::npos || separator == 0 ||
        separator + 1 >= item.size() || item.find('=', separator + 1) != std::string::npos) {
      throw std::invalid_argument("generation state successor map is malformed");
    }
    result.emplace_back(item.substr(0, separator), item.substr(separator + 1));
    start = end == std::string::npos ? encoded.size() : end + 1;
  }
  std::set<std::string> inputs;
  std::set<std::string> outputs;
  for (const auto& item : result) {
    if (!inputs.insert(item.first).second || !outputs.insert(item.second).second) {
      throw std::invalid_argument("generation state successor map contains duplicates");
    }
  }
  return result;
}

bool containsName(const std::vector<std::string>& names, const std::string& value)
{
  return std::find(names.begin(), names.end(), value) != names.end();
}

std::vector<std::string>
splitMetadataNames(const std::string& encoded)
{
  std::vector<std::string> result;
  std::size_t start = 0;
  while (start < encoded.size()) {
    const auto end = encoded.find(',', start);
    const auto value = encoded.substr(
      start, end == std::string::npos ? std::string::npos : end - start);
    if (!value.empty())
      result.push_back(value);
    start = end == std::string::npos ? encoded.size() : end + 1;
  }
  return result;
}

} // namespace

void
bindNativeRunnerPreparationContext(NativeModelRunnerSpec& spec,
                                   const NativeSelectionProjectionV3& projection,
                                   const NativeRunnerPreparationContext& context)
{
  spec.metadata["evidence.providerName"] = context.providerName;
  spec.metadata["evidence.providerBootId"] = context.providerBootId;
  spec.metadata["evidence.epoch"] = "1";
  spec.metadata["evidence.createdAtMs"] = std::to_string(context.providerStartedAtMs);
  spec.metadata["evidence.planDigest"] = projection.planDigest;
  spec.metadata["evidence.modelDigest"] = projection.assembly.modelManifestDigest.empty()
    ? projection.planDigest : projection.assembly.modelManifestDigest;
  spec.metadata["evidence.artifactDigest"] = projection.assembly.artifactDigest;

  // Observing adapters consume this unique request profile context. Other
  // adapters may ignore it; no model name or generation mode is required.
  static std::atomic<std::uint64_t> profileSequence{0};
  spec.metadata["providerProfilePrefix"] = context.cacheDirectory +
    "/ort-profile-" + std::to_string(getpid()) + "-" +
    std::to_string(profileSequence.fetch_add(1));
  spec.metadata["profileAfterRequest"] = "true";

  // Generation state is part of the authenticated Selection projection, not
  // an adapter-local default.  Preserve it on the prepared runner spec so the
  // ORT runner can distinguish epoch-zero zero-state inputs from ordinary
  // application tensors and enforce predecessor state on later epochs.
  if (projection.generationContract.enabled) {
    // These fields are projection-owned.  Clear adapter-provided values before
    // applying the authenticated contract so an absent optional field cannot
    // leave a stale successor or position binding in the runner spec.
    spec.metadata.erase("stateSuccessorMap");
    spec.metadata.erase("kvTensorMap");
    spec.metadata.erase("positionInputPolicy");
    spec.metadata.erase("attentionMaskInputName");
    spec.metadata.erase("positionIdsInputName");
    spec.metadata.erase("cachePositionInputName");
    const auto localNames = [] (const auto& declared, const auto& boundary) {
      std::vector<std::string> result;
      for (const auto& name : declared) {
        if (std::any_of(boundary.begin(), boundary.end(),
                        [&name] (const auto& tensor) {
                          return tensor.name == name;
                        })) {
          result.push_back(name);
        }
      }
      return result;
    };
    const auto stateInputs = localNames(
      projection.generationContract.stateInputNames,
      projection.assembly.expectedInputs);
    const auto stateOutputs = localNames(
      projection.generationContract.stateOutputNames,
      projection.assembly.expectedOutputs);
    if (stateInputs.empty() || stateInputs.size() != stateOutputs.size()) {
      throw std::invalid_argument(
        "generation state contract does not match prepared role boundary");
    }
    const auto join = [] (const std::vector<std::string>& values) {
      std::ostringstream result;
      for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) result << ',';
        result << values[index];
      }
      return result.str();
    };
    spec.metadata["statefulModel"] = "true";
    spec.metadata["stateInputNames"] = join(stateInputs);
    spec.metadata["stateOutputNames"] = join(stateOutputs);
    std::vector<std::string> inputNames;
    inputNames.reserve(projection.assembly.expectedInputs.size());
    for (const auto& tensor : projection.assembly.expectedInputs)
      inputNames.push_back(tensor.name);

    const auto& successorMap = projection.generationContract.stateSuccessorMap;
    if (!successorMap.empty()) {
      const auto pairs = parseSuccessorMap(successorMap);
      std::vector<std::pair<std::string, std::string>> localPairs;
      for (const auto& item : pairs) {
        const bool localInput = containsName(stateInputs, item.first);
        const bool localOutput = containsName(stateOutputs, item.second);
        if (localInput != localOutput) {
          throw std::invalid_argument(
            "generation state successor map crosses a prepared role boundary");
        }
        if (localInput) {
          localPairs.push_back(item);
        }
      }
      if (localPairs.size() != stateOutputs.size()) {
        throw std::invalid_argument(
          "generation state successor map does not cover prepared role state");
      }
      std::ostringstream encoded;
      for (std::size_t index = 0; index < localPairs.size(); ++index) {
        if (index != 0) encoded << ',';
        encoded << localPairs[index].first << '=' << localPairs[index].second;
      }
      spec.metadata["stateSuccessorMap"] = encoded.str();
      spec.metadata["kvTensorMap"] = encoded.str();
    }

    const auto& position = projection.generationContract;
    if (!position.positionInputPolicy.empty()) {
      if (position.attentionMaskInputName.empty() ||
          position.positionIdsInputName.empty()) {
        throw std::invalid_argument(
          "generation position input policy has missing input names");
      }
      // The authenticated contract is global; downstream subgraphs may
      // consume derived position tensors instead of the original inputs.
      // A partially present local position interface is still invalid.
      const bool hasLocalPositionInput =
        containsName(inputNames, position.attentionMaskInputName) ||
        containsName(inputNames, position.positionIdsInputName) ||
        (!position.cachePositionInputName.empty() &&
         containsName(inputNames, position.cachePositionInputName));
      if (!hasLocalPositionInput) {
        return;
      }
      if (!containsName(inputNames, position.attentionMaskInputName) ||
          !containsName(inputNames, position.positionIdsInputName) ||
          (!position.cachePositionInputName.empty() &&
           !containsName(inputNames, position.cachePositionInputName))) {
        throw std::invalid_argument(
          "generation position input policy does not match prepared role boundary");
      }
      spec.metadata["positionInputPolicy"] = position.positionInputPolicy;
      spec.metadata["attentionMaskInputName"] = position.attentionMaskInputName;
      spec.metadata["positionIdsInputName"] = position.positionIdsInputName;
      if (!position.cachePositionInputName.empty()) {
        spec.metadata["cachePositionInputName"] = position.cachePositionInputName;
      }
    }
  }

}

} // namespace ndnsf::di
