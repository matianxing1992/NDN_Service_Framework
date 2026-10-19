/*
 * Copyright (C) 2026
 *
 * This file is part of NDNSF.
 */

#ifndef NDNSF_EXAMPLES_UAV_SOURCE_GENERATION_HPP
#define NDNSF_EXAMPLES_UAV_SOURCE_GENERATION_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ndnsf::examples::uav {

struct UavSourceGeneration
{
  size_t firstWindowIndex = 0;
  uint64_t sourceEpoch = 0;
  std::array<std::string, 3> sourceIds;
};

struct UavSourceGenerationSchedule
{
  size_t windowCount = 0;
  std::vector<UavSourceGeneration> generations;
};

/** Load and validate one complete uav-source-generations-v1 run manifest. */
UavSourceGenerationSchedule
loadUavSourceGenerationSchedule(const std::string& path, size_t expectedWindowCount);

/** Construct the backward-compatible, single-generation schedule. */
UavSourceGenerationSchedule
makeUavSourceGenerationSchedule(uint64_t sourceEpoch,
                                const std::array<std::string, 3>& sourceIds,
                                size_t windowCount);

/** Return the one generation that owns a zero-based window index. */
const UavSourceGeneration&
uavSourceGenerationForWindow(const UavSourceGenerationSchedule& schedule,
                             size_t windowIndex);

} // namespace ndnsf::examples::uav

#endif // NDNSF_EXAMPLES_UAV_SOURCE_GENERATION_HPP
