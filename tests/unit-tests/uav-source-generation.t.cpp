/*
 * Copyright (C) 2026
 *
 * This file is part of NDNSF.
 */

#include <boost/test/unit_test.hpp>

#include "../../NDNSF-UAV-APP/shared/UavSourceGeneration.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace ndnsf::examples::uav::tests {
namespace {

class TemporaryManifest
{
public:
  explicit TemporaryManifest(const std::string& content)
  {
    m_path = std::filesystem::temp_directory_path() /
      ("uav-source-generations-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
    std::ofstream output(m_path, std::ios::binary);
    output << content;
    if (!output) throw std::runtime_error("cannot write temporary source generation manifest");
  }

  ~TemporaryManifest()
  {
    std::error_code error;
    std::filesystem::remove(m_path, error);
  }

  const std::string path() const { return m_path.string(); }

private:
  std::filesystem::path m_path;
};

const std::string VALID_MANIFEST = R"({
  "schema":"uav-source-generations-v1",
  "windowCount":5,
  "generations":[
    {"firstWindowIndex":0,"sourceEpoch":11,"sourceIds":{"UAV1":"source-a1","UAV2":"source-a2","UAV3":"source-a3"}},
    {"firstWindowIndex":2,"sourceEpoch":12,"sourceIds":{"UAV1":"source-b1","UAV2":"source-b2","UAV3":"source-b3"}}
  ]
})";

std::string
replaceOnce(std::string input, const std::string& from, const std::string& to)
{
  const auto offset = input.find(from);
  if (offset == std::string::npos) {
    throw std::runtime_error("test fixture token is missing");
  }
  input.replace(offset, from.size(), to);
  return input;
}

} // namespace

BOOST_AUTO_TEST_CASE(LoadsCompleteScheduleAndSelectsOneGenerationPerWindow)
{
  TemporaryManifest manifest(VALID_MANIFEST);
  const auto schedule = loadUavSourceGenerationSchedule(manifest.path(), 5);

  BOOST_REQUIRE_EQUAL(schedule.windowCount, 5U);
  BOOST_REQUIRE_EQUAL(schedule.generations.size(), 2U);
  BOOST_CHECK_EQUAL(uavSourceGenerationForWindow(schedule, 0).sourceEpoch, 11U);
  BOOST_CHECK_EQUAL(uavSourceGenerationForWindow(schedule, 1).sourceIds[1], "source-a2");
  BOOST_CHECK_EQUAL(uavSourceGenerationForWindow(schedule, 2).sourceEpoch, 12U);
  BOOST_CHECK_EQUAL(uavSourceGenerationForWindow(schedule, 4).sourceIds[2], "source-b3");
  BOOST_CHECK_THROW(uavSourceGenerationForWindow(schedule, 5), std::out_of_range);
}

BOOST_AUTO_TEST_CASE(RejectsWindowCountGapEpochIdAndSchemaErrors)
{
  TemporaryManifest valid(VALID_MANIFEST);
  BOOST_CHECK_THROW(loadUavSourceGenerationSchedule(valid.path(), 4), std::runtime_error);

  const auto expectInvalid = [] (std::string input) {
    TemporaryManifest manifest(input);
    BOOST_CHECK_THROW(loadUavSourceGenerationSchedule(manifest.path(), 5), std::runtime_error);
  };
  expectInvalid(replaceOnce(VALID_MANIFEST, "\"firstWindowIndex\":0",
                            "\"firstWindowIndex\":1"));
  expectInvalid(replaceOnce(VALID_MANIFEST, "\"sourceEpoch\":12",
                            "\"sourceEpoch\":13"));
  expectInvalid(replaceOnce(VALID_MANIFEST, "\"windowCount\":5",
                            "\"windowCount\":\"5\""));
  expectInvalid(replaceOnce(VALID_MANIFEST, "\"firstWindowIndex\":0",
                            "\"firstWindowIndex\":\"0\""));
  expectInvalid(replaceOnce(VALID_MANIFEST, "\"sourceEpoch\":12",
                            "\"sourceEpoch\":\"12\""));
  expectInvalid(replaceOnce(VALID_MANIFEST, "\"UAV1\":\"source-a1\"",
                            "\"UAV1\":123"));
  expectInvalid(replaceOnce(VALID_MANIFEST, "source-b2", "source-a2"));
  expectInvalid(replaceOnce(VALID_MANIFEST, "\"windowCount\":5",
                            "\"windowCount\":5,\"extra\":true"));
  expectInvalid(replaceOnce(VALID_MANIFEST, "uav-source-generations-v1",
                            "uav-source-generations-v2"));
}

BOOST_AUTO_TEST_CASE(SingleGenerationCompatibilityPathRetainsConfiguredIdentity)
{
  const auto schedule = makeUavSourceGenerationSchedule(
    7, {"source-1", "source-2", "source-3"}, 3);
  BOOST_CHECK_EQUAL(schedule.generations.size(), 1U);
  BOOST_CHECK_EQUAL(uavSourceGenerationForWindow(schedule, 2).sourceEpoch, 7U);
  BOOST_CHECK_EQUAL(uavSourceGenerationForWindow(schedule, 1).sourceIds[0], "source-1");
}

} // namespace ndnsf::examples::uav::tests
