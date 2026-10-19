/*
 * Copyright (C) 2026
 *
 * This file is part of NDNSF.
 */

#include "UavSourceGeneration.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <algorithm>
#include <charconv>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>

namespace ndnsf::examples::uav {
namespace {

using boost::property_tree::ptree;

constexpr size_t MAX_WINDOWS = 600;
constexpr size_t MAX_MANIFEST_BYTES = 64 * 1024;
constexpr std::array<const char*, 3> CAMERAS{"UAV1", "UAV2", "UAV3"};

void
requireExactFields(const ptree& tree, const std::set<std::string>& expected,
                   const std::string& label)
{
  std::set<std::string> actual;
  for (const auto& item : tree) {
    if (!actual.insert(item.first).second) {
      throw std::runtime_error(label + " has a duplicate " + item.first + " field");
    }
  }
  if (actual != expected) {
    throw std::runtime_error(label + " has missing or unknown fields");
  }
}

const ptree&
requiredChild(const ptree& tree, const std::string& key)
{
  const auto child = tree.get_child_optional(key);
  if (!child) {
    throw std::runtime_error("source generation manifest is missing " + key);
  }
  return *child;
}

std::string
requiredScalar(const ptree& tree, const std::string& key)
{
  const auto& child = requiredChild(tree, key);
  if (!child.empty()) {
    throw std::runtime_error("source generation field is not scalar: " + key);
  }
  return child.data();
}

uint64_t
positiveInteger(const ptree& tree, const std::string& key)
{
  const auto value = requiredScalar(tree, key);
  uint64_t parsed = 0;
  const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (result.ec != std::errc() || result.ptr != value.data() + value.size() || parsed == 0 ||
      std::to_string(parsed) != value) {
    throw std::runtime_error("source generation field is not a positive canonical integer: " + key);
  }
  return parsed;
}

void
validateSourceId(const std::string& sourceId)
{
  if (sourceId.empty() || sourceId.size() > 128 ||
      std::any_of(sourceId.begin(), sourceId.end(), [] (unsigned char c) {
        return c < 0x20 || c == 0x7f;
      })) {
    throw std::runtime_error("source generation contains an invalid sourceId");
  }
}

void
validateIds(const std::array<std::string, 3>& sourceIds)
{
  std::set<std::string> unique;
  for (const auto& sourceId : sourceIds) {
    validateSourceId(sourceId);
    if (!unique.insert(sourceId).second) {
      throw std::runtime_error("source generation sourceIds must be unique per camera");
    }
  }
}

UavSourceGeneration
parseGeneration(const ptree& tree)
{
  requireExactFields(tree, {"firstWindowIndex", "sourceEpoch", "sourceIds"}, "generation");
  const auto firstWindow = requiredScalar(tree, "firstWindowIndex");
  size_t index = 0;
  const auto parsedIndex = std::from_chars(firstWindow.data(), firstWindow.data() + firstWindow.size(), index);
  if (parsedIndex.ec != std::errc() || parsedIndex.ptr != firstWindow.data() + firstWindow.size() ||
      std::to_string(index) != firstWindow) {
    throw std::runtime_error("generation firstWindowIndex is not a canonical integer");
  }

  UavSourceGeneration generation;
  generation.firstWindowIndex = index;
  generation.sourceEpoch = positiveInteger(tree, "sourceEpoch");
  const auto& ids = requiredChild(tree, "sourceIds");
  requireExactFields(ids, {"UAV1", "UAV2", "UAV3"}, "generation sourceIds");
  for (size_t i = 0; i < CAMERAS.size(); ++i) {
    generation.sourceIds[i] = requiredScalar(ids, CAMERAS[i]);
  }
  validateIds(generation.sourceIds);
  return generation;
}

void
validateSchedule(const UavSourceGenerationSchedule& schedule)
{
  if (schedule.windowCount == 0 || schedule.windowCount > MAX_WINDOWS ||
      schedule.generations.empty() || schedule.generations.size() > schedule.windowCount ||
      schedule.generations.front().firstWindowIndex != 0) {
    throw std::runtime_error("source generation schedule does not cover the configured windows");
  }
  for (size_t i = 0; i < schedule.generations.size(); ++i) {
    const auto& current = schedule.generations[i];
    validateIds(current.sourceIds);
    if (current.firstWindowIndex >= schedule.windowCount || current.sourceEpoch == 0) {
      throw std::runtime_error("source generation has an out-of-range window or zero epoch");
    }
    if (i == 0) continue;
    const auto& previous = schedule.generations[i - 1];
    if (current.firstWindowIndex <= previous.firstWindowIndex ||
        previous.sourceEpoch == std::numeric_limits<uint64_t>::max() ||
        current.sourceEpoch != previous.sourceEpoch + 1) {
      throw std::runtime_error("source generation boundaries or epochs are not strictly sequential");
    }
    for (size_t camera = 0; camera < CAMERAS.size(); ++camera) {
      if (current.sourceIds[camera] == previous.sourceIds[camera]) {
        throw std::runtime_error("sourceId must change for every camera at a generation boundary");
      }
    }
  }
}

} // namespace

UavSourceGenerationSchedule
loadUavSourceGenerationSchedule(const std::string& path, size_t expectedWindowCount)
{
  if (path.empty() || expectedWindowCount == 0 || expectedWindowCount > MAX_WINDOWS) {
    throw std::invalid_argument("source generation path or expected window count is invalid");
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot open source generation manifest: " + path);
  }
  input.seekg(0, std::ios::end);
  const auto size = input.tellg();
  if (size <= 0 || static_cast<uint64_t>(size) > MAX_MANIFEST_BYTES) {
    throw std::runtime_error("source generation manifest is empty or exceeds 64 KiB");
  }
  input.seekg(0, std::ios::beg);

  const std::string manifestText((std::istreambuf_iterator<char>(input)),
                                 std::istreambuf_iterator<char>());
  std::istringstream treeInput(manifestText);
  ptree root;
  boost::property_tree::read_json(treeInput, root);
  requireExactFields(root, {"schema", "windowCount", "generations"}, "source generation manifest");
  if (requiredScalar(root, "schema") != "uav-source-generations-v1") {
    throw std::runtime_error("unsupported source generation manifest schema");
  }

  ndnsf::di::NativeJson typedRoot;
  try {
    typedRoot = ndnsf::di::nativeParseJson(manifestText);
  }
  catch (const std::exception& error) {
    throw std::runtime_error("cannot parse typed source generation manifest: " +
                             std::string(error.what()));
  }
  const auto isInteger = [] (const ndnsf::di::NativeJson& value) {
    return value.is_number_integer() || value.is_number_unsigned();
  };
  if (!typedRoot.is_object() || !isInteger(typedRoot.at("windowCount"))) {
    throw std::runtime_error("source generation manifest windowCount must be a JSON integer");
  }
  const auto& typedGenerations = typedRoot.at("generations");
  if (!typedGenerations.is_array()) {
    throw std::runtime_error("source generation manifest generations must be an array");
  }
  for (const auto& generation : typedGenerations) {
    if (!generation.is_object() || !generation.contains("firstWindowIndex") ||
        !generation.contains("sourceEpoch") ||
        !isInteger(generation.at("firstWindowIndex")) ||
        !isInteger(generation.at("sourceEpoch"))) {
      throw std::runtime_error("source generation boundary and epoch must be JSON integers");
    }
    if (!generation.contains("sourceIds") || !generation.at("sourceIds").is_object()) {
      throw std::runtime_error("source generation sourceIds must be an object");
    }
    const auto& typedIds = generation.at("sourceIds");
    for (const auto* camera : CAMERAS) {
      const auto sourceId = typedIds.find(camera);
      if (sourceId == typedIds.end() || !sourceId->is_string()) {
        throw std::runtime_error("source generation sourceIds must contain JSON strings");
      }
    }
  }
  const auto count = positiveInteger(root, "windowCount");
  if (count != expectedWindowCount) {
    throw std::runtime_error("source generation manifest windowCount does not match the process");
  }

  UavSourceGenerationSchedule schedule;
  schedule.windowCount = static_cast<size_t>(count);
  const auto& generations = requiredChild(root, "generations");
  for (const auto& entry : generations) {
    if (!entry.first.empty()) {
      throw std::runtime_error("source generation manifest generations must be an array");
    }
    schedule.generations.push_back(parseGeneration(entry.second));
    if (schedule.generations.size() > expectedWindowCount) {
      throw std::runtime_error("source generation manifest exceeds the window bound");
    }
  }
  validateSchedule(schedule);
  return schedule;
}

UavSourceGenerationSchedule
makeUavSourceGenerationSchedule(uint64_t sourceEpoch,
                                const std::array<std::string, 3>& sourceIds,
                                size_t windowCount)
{
  UavSourceGenerationSchedule schedule;
  schedule.windowCount = windowCount;
  schedule.generations.push_back({0, sourceEpoch, sourceIds});
  validateSchedule(schedule);
  return schedule;
}

const UavSourceGeneration&
uavSourceGenerationForWindow(const UavSourceGenerationSchedule& schedule,
                             size_t windowIndex)
{
  if (windowIndex >= schedule.windowCount || schedule.generations.empty()) {
    throw std::out_of_range("window index is outside the source generation schedule");
  }
  const auto next = std::upper_bound(
    schedule.generations.begin(), schedule.generations.end(), windowIndex,
    [] (size_t index, const UavSourceGeneration& generation) {
      return index < generation.firstWindowIndex;
    });
  if (next == schedule.generations.begin()) {
    throw std::logic_error("source generation schedule has no initial generation");
  }
  return *std::prev(next);
}

} // namespace ndnsf::examples::uav
