#pragma once

// Shared by the Spec189 log CLI and its C++ parser regression target.
// This validates material event evidence, not model correctness or network qualification.
#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace spec189::oracle {

struct Marker
{
  std::string stage;
  std::string status;
  std::string requestId;
  std::string attemptEpoch;
  std::string provider;
  std::string role;
  std::string planDigest;
  std::string manifestDigest;
  std::string graphDigest;
  std::string initializerDigest;
  std::string artifactDigest;
  std::string layerBegin;
  std::string layerEnd;
  std::string preparationId;
  std::size_t line = 0;
};

struct PlacementObservation
{
  Marker selection;
  std::size_t selectionLine = 0;
  std::size_t grantLine = 0;
};

inline std::string
readFile(const std::filesystem::path& path)
{
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("missing Spec189 log: " + path.string());
  }
  std::ostringstream content;
  content << input.rdbuf();
  return content.str();
}

inline std::string
field(const std::string& line, const std::string& key)
{
  const auto prefix = key + "=";
  const auto begin = line.find(prefix);
  if (begin == std::string::npos) {
    return {};
  }
  const auto valueBegin = begin + prefix.size();
  const auto valueEnd = line.find_first_of(" \t\r\n", valueBegin);
  return line.substr(valueBegin, valueEnd == std::string::npos
                                 ? std::string::npos : valueEnd - valueBegin);
}

inline std::vector<Marker>
markers(const std::string& text)
{
  std::vector<Marker> result;
  std::istringstream lines(text);
  std::string line;
  std::size_t lineNumber = 0;
  while (std::getline(lines, line)) {
    ++lineNumber;
    if (line.find("NDNSF_DI_PROVIDER_STAGE") == std::string::npos) {
      continue;
    }
    Marker marker;
    marker.stage = field(line, "stage");
    marker.status = field(line, "status");
    marker.requestId = field(line, "requestId");
    marker.attemptEpoch = field(line, "attemptEpoch");
    marker.provider = field(line, "provider");
    marker.role = field(line, "role");
    marker.planDigest = field(line, "planDigest");
    marker.manifestDigest = field(line, "manifestDigest");
    marker.graphDigest = field(line, "graphDigest");
    marker.initializerDigest = field(line, "initializerDigest");
    marker.artifactDigest = field(line, "artifactDigest");
    marker.layerBegin = field(line, "layerBegin");
    marker.layerEnd = field(line, "layerEnd");
    marker.preparationId = field(line, "preparationId");
    marker.line = lineNumber;
    if (!marker.stage.empty()) {
      result.push_back(std::move(marker));
    }
  }
  return result;
}

struct MaterialFetchObservation
{
  std::size_t preSelection = 0;
  std::size_t postSelection = 0;
};

inline MaterialFetchObservation
validateMaterialFetches(const std::filesystem::path& path,
                        const PlacementObservation& observation,
                        const std::string& expectedProvider)
{
  const auto text = readFile(path);
  MaterialFetchObservation result;
  using Key = std::pair<std::string, std::string>;
  std::map<Key, int> state;
  std::map<Key, std::string> expectedDigests;
  std::map<Key, std::string> returnedDigests;
  std::map<Key, std::string> returnedBytes;
  std::map<std::string, std::size_t> completed;
  std::istringstream lines(text);
  std::string line;
  std::size_t lineNumber = 0;
  while (std::getline(lines, line)) {
    ++lineNumber;
    if (line.find("NDNSF_DI_PROVIDER_MATERIAL_FETCH") == std::string::npos) {
      continue;
    }
    const auto status = field(line, "status");
    if (status != "begin" && status != "returned" && status != "verified" &&
        status != "empty" && status != "error") {
      throw std::runtime_error(
        "SPEC189_CPP_ORACLE_FAIL boundary=MATERIAL_FETCH reason=unknown-status log=" +
        path.string());
    }
    const auto requestId = field(line, "requestId");
    if (requestId != observation.selection.requestId ||
        field(line, "attemptEpoch") != observation.selection.attemptEpoch ||
        field(line, "provider") != expectedProvider ||
        field(line, "role") != observation.selection.role ||
        field(line, "planDigest") != observation.selection.planDigest) {
      const auto reason = requestId.empty() ? "identity-missing" :
        (requestId != observation.selection.requestId ? "multi-request-run" :
         "identity-mismatch");
      throw std::runtime_error(
        std::string("SPEC189_CPP_ORACLE_FAIL boundary=MATERIAL_FETCH reason=") +
        reason + " log=" +
        path.string());
    }
    if (lineNumber <= observation.selectionLine) {
      ++result.preSelection;
      throw std::runtime_error(
        "SPEC189_CPP_ORACLE_FAIL boundary=PRE_SELECTION_SIDE_EFFECT reason=material-fetch log=" +
        path.string());
    }
    if (lineNumber <= observation.grantLine) {
      throw std::runtime_error(
        "SPEC189_CPP_ORACLE_FAIL boundary=GRANT_VERIFIED reason=material-fetch-before-grant log=" +
        path.string());
    }
    const auto kind = field(line, "kind");
    const auto name = field(line, "name");
    const auto expectedDigest = field(line, "expectedDigest");
    if (kind != "root" && kind != "material-manifest" && kind != "material-payload") {
      throw std::runtime_error(
        "SPEC189_CPP_ORACLE_FAIL boundary=MATERIAL_FETCH reason=unknown-kind log=" +
        path.string());
    }
    if (name.empty() || expectedDigest.empty()) {
      throw std::runtime_error(
        "SPEC189_CPP_ORACLE_FAIL boundary=MATERIAL_FETCH reason=identity-missing log=" +
        path.string());
    }
    const Key key{kind, name};
    if (state[key] == 0) {
      expectedDigests[key] = expectedDigest;
    }
    else if (expectedDigests[key] != expectedDigest) {
      throw std::runtime_error(
        "SPEC189_CPP_ORACLE_FAIL boundary=MATERIAL_FETCH reason=material-identity-changed log=" +
        path.string());
    }
    if (status == "begin") {
      if (state[key] != 0) {
        throw std::runtime_error(
          "SPEC189_CPP_ORACLE_FAIL boundary=MATERIAL_FETCH reason=duplicate-begin log=" +
          path.string());
      }
      if ((kind == "material-manifest" && completed["root"] != 1) ||
          (kind == "material-payload" && completed["material-manifest"] != 1)) {
        throw std::runtime_error(
          "SPEC189_CPP_ORACLE_FAIL boundary=MATERIAL_FETCH reason=unauthenticated-parent log=" +
          path.string());
      }
      state[key] = 1;
      continue;
    }
    if (status == "returned") {
      if (state[key] != 1) {
        throw std::runtime_error(
          "SPEC189_CPP_ORACLE_FAIL boundary=MATERIAL_FETCH reason=returned-before-begin log=" +
          path.string());
      }
      const auto bytes = field(line, "bytes");
      const auto digest = field(line, "digest");
      if (bytes.empty() || digest.empty()) {
        throw std::runtime_error(
          "SPEC189_CPP_ORACLE_FAIL boundary=MATERIAL_FETCH reason=returned-evidence-missing log=" +
          path.string());
      }
      returnedDigests[key] = digest;
      returnedBytes[key] = bytes;
      state[key] = 2;
      continue;
    }
    if (status == "verified") {
      if (state[key] != 2) {
        throw std::runtime_error(
          "SPEC189_CPP_ORACLE_FAIL boundary=MATERIAL_FETCH reason=verified-before-returned log=" +
          path.string());
      }
      const auto bytes = field(line, "bytes");
      const auto digest = field(line, "digest");
      if (bytes.empty() || digest.empty() || digest != expectedDigest ||
          digest != returnedDigests[key] || bytes != returnedBytes[key]) {
        throw std::runtime_error(
          "SPEC189_CPP_ORACLE_FAIL boundary=MATERIAL_FETCH reason=verification-evidence-mismatch log=" +
          path.string());
      }
      try {
        if (bytes.find_first_not_of("0123456789") != std::string::npos ||
            std::stoull(bytes) == 0) {
          throw std::runtime_error("zero-byte material");
        }
      }
      catch (const std::exception&) {
        throw std::runtime_error(
          "SPEC189_CPP_ORACLE_FAIL boundary=MATERIAL_FETCH reason=invalid-bytes log=" +
          path.string());
      }
      if (kind == "root" && expectedDigest != observation.selection.manifestDigest) {
        throw std::runtime_error(
          "SPEC189_CPP_ORACLE_FAIL boundary=MATERIAL_FETCH reason=manifest-digest-mismatch log=" +
          path.string());
      }
      state[key] = 3;
      ++completed[kind];
      ++result.postSelection;
      continue;
    }
    if (status == "empty" || status == "error") {
      throw std::runtime_error(
        "SPEC189_CPP_ORACLE_FAIL boundary=MATERIAL_FETCH reason=" + status +
        " log=" + path.string());
    }
  }
  if (completed["root"] != 1 || completed["material-manifest"] != 1 ||
      completed["material-payload"] == 0 ||
      std::any_of(state.begin(), state.end(), [](const auto& entry) { return entry.second != 3; })) {
    throw std::runtime_error(
      "SPEC189_CPP_ORACLE_FAIL boundary=MATERIAL_FETCH reason=incomplete-material-sequence log=" +
      path.string());
  }
  return result;
}

} // namespace spec189::oracle
