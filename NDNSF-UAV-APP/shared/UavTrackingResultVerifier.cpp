#include "UavTrackingResultVerifier.hpp"

#include "UavNames.hpp"
#include "UavProtocol.hpp"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <charconv>
#include <set>
#include <sstream>
#include <stdexcept>

namespace ndnsf::examples::uav {
namespace {

using boost::property_tree::ptree;

constexpr size_t MAX_JSON_NESTING_DEPTH = 64;

void
requireJsonNestingBounded(const std::string& json)
{
  size_t depth = 0;
  bool inString = false;
  bool escaped = false;
  for (const char c : json) {
    if (inString) {
      if (escaped) {
        escaped = false;
      }
      else if (c == '\\') {
        escaped = true;
      }
      else if (c == '"') {
        inString = false;
      }
      continue;
    }
    if (c == '"') {
      inString = true;
    }
    else if (c == '{' || c == '[') {
      if (++depth > MAX_JSON_NESTING_DEPTH) {
        throw std::runtime_error("tracking result JSON nesting exceeds 64 levels");
      }
    }
    else if (c == '}' || c == ']') {
      if (depth == 0) {
        throw std::runtime_error("tracking result JSON has unbalanced delimiters");
      }
      --depth;
    }
  }
  if (inString || escaped || depth != 0) {
    throw std::runtime_error("tracking result JSON has unbalanced strings or delimiters");
  }
}

void
requireUniqueKeys(const ptree& tree, size_t depth = 0)
{
  if (depth > MAX_JSON_NESTING_DEPTH) {
    throw std::runtime_error("tracking result JSON nesting exceeds 64 levels");
  }
  std::set<std::string> keys;
  for (const auto& child : tree) {
    if (!child.first.empty() && !keys.insert(child.first).second) {
      throw std::runtime_error("tracking result JSON contains a duplicate object key");
    }
    requireUniqueKeys(child.second, depth + 1);
  }
}

const ptree&
requiredChild(const ptree& tree, const std::string& key)
{
  const ptree* found = nullptr;
  for (const auto& child : tree) {
    if (child.first != key) continue;
    if (found != nullptr) {
      throw std::runtime_error("tracking result has a duplicate " + key + " field");
    }
    found = &child.second;
  }
  if (found == nullptr) {
    throw std::runtime_error("tracking result is missing " + key);
  }
  return *found;
}

std::string
requiredScalar(const ptree& tree, const std::string& key)
{
  const auto& child = requiredChild(tree, key);
  if (!child.empty()) {
    throw std::runtime_error("tracking result field is not scalar: " + key);
  }
  return child.data();
}

uint64_t
requiredPositiveInteger(const ptree& tree, const std::string& key)
{
  const auto value = requiredScalar(tree, key);
  uint64_t parsed = 0;
  const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (result.ec != std::errc() || result.ptr != value.data() + value.size() || parsed == 0 ||
      std::to_string(parsed) != value) {
    throw std::runtime_error("tracking result field is not a positive canonical integer: " + key);
  }
  return parsed;
}

void
requireDigest(const std::string& digest, const std::string& label)
{
  if (digest.size() != 71 || digest.compare(0, 7, "sha256:") != 0 ||
      digest.find_first_not_of("0123456789abcdef", 7) != std::string::npos) {
    throw std::runtime_error("tracking result has a malformed " + label + " digest");
  }
}

ndn::Name
requireCanonicalName(const std::string& value, const std::string& label)
{
  ndn::Name name(value);
  if (name.empty() || name.toUri() != value) {
    throw std::runtime_error("tracking result has a noncanonical " + label + " name");
  }
  return name;
}

} // namespace

void
validateUavTrackingResultContext(const std::string& resultName,
                                 const std::string& responseProvider,
                                 const std::string& resultJson,
                                 const UavTrackingResultExpectation& expected)
{
  if (expected.runId.empty() || expected.missionId.empty() || expected.windowId.empty() ||
      expected.sequence == 0 || expected.sourceEpoch == 0 || expected.computeProvider.empty() ||
      resultJson.empty()) {
    throw std::invalid_argument("tracking result expectation is incomplete");
  }
  for (size_t i = 0; i < expected.sourceIds.size(); ++i) {
    if (expected.sourceIds[i].empty()) {
      throw std::invalid_argument("tracking result source identity is incomplete");
    }
    requireDigest(expected.inputDigests[i], "input");
  }

  const auto provider = requireCanonicalName(responseProvider, "provider");
  if (provider != expected.computeProvider) {
    throw std::runtime_error("tracking result provider does not match the selected compute role");
  }
  const auto namedResult = requireCanonicalName(resultName, "result");
  const auto expectedName = makeUavMultiViewResultName(
    expected.computeProvider, expected.missionId, expected.windowId, expected.sequence, 1);
  if (namedResult != expectedName ||
      !isUavProviderMultiViewDataName(expected.computeProvider, namedResult, "RESULT",
                                      expected.missionId, expected.windowId)) {
    throw std::runtime_error("tracking result name does not match the request context");
  }

  requireJsonNestingBounded(resultJson);
  std::istringstream input(resultJson);
  ptree result;
  boost::property_tree::read_json(input, result);
  requireUniqueKeys(result);
  if (requiredScalar(result, "schema") != "spec210-tracking-result-v2" ||
      requiredScalar(result, "runId") != expected.runId ||
      requiredScalar(result, "missionId") != expected.missionId ||
      requiredScalar(result, "windowId") != expected.windowId ||
      requiredPositiveInteger(result, "sequence") != expected.sequence ||
      requiredScalar(result, "terminal") != "true") {
    throw std::runtime_error("tracking result header does not match the request context");
  }

  const auto& frames = requiredChild(result, "frames");
  if (frames.size() != expected.sourceIds.size()) {
    throw std::runtime_error("tracking result must contain exactly three ordered source frames");
  }
  size_t index = 0;
  for (const auto& frameEntry : frames) {
    if (!frameEntry.first.empty()) {
      throw std::runtime_error("tracking result frames must be an array");
    }
    const auto& frame = frameEntry.second;
    const std::string camera = "UAV" + std::to_string(index + 1);
    if (requiredScalar(frame, "cameraId") != camera ||
        requiredPositiveInteger(frame, "sequence") != expected.sequence ||
        requiredScalar(frame, "sourceId") != expected.sourceIds[index] ||
        requiredPositiveInteger(frame, "sourceEpoch") != expected.sourceEpoch ||
        requiredScalar(frame, "inputDigest") != expected.inputDigests[index]) {
      throw std::runtime_error("tracking result frame does not match its requested source");
    }

    const auto expectedObservationName = makeUavObservationDataName(
      droneIdentity(camera), camera, expected.sourceEpoch, expected.sequence, "OBSERVATION", 1);
    const auto observationName = requiredScalar(frame, "observationName");
    const auto observationDigest = requiredScalar(frame, "observationDigest");
    requireDigest(observationDigest, "observation");
    const auto& observation = requiredChild(frame, "observation");
    if (requiredScalar(observation, "schema") != "uav-observation-v2" ||
        requiredScalar(observation, "sourceId") != expected.sourceIds[index] ||
        requiredPositiveInteger(observation, "sourceEpoch") != expected.sourceEpoch ||
        requiredPositiveInteger(observation, "sequence") != expected.sequence) {
      throw std::runtime_error("tracking result observation identity does not match its frame");
    }
    const auto& observationRef = requiredChild(observation, "observationRef");
    const auto referenceName = requireCanonicalName(
      requiredScalar(observationRef, "name"), "observation reference");
    if (referenceName != expectedObservationName || observationName != referenceName.toUri() ||
        requiredScalar(observationRef, "digest") != observationDigest) {
      throw std::runtime_error("tracking result observation reference does not match its frame");
    }
    ++index;
  }
}

} // namespace ndnsf::examples::uav
