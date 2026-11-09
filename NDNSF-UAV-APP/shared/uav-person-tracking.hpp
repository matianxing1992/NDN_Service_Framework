#ifndef NDNSF_EXAMPLES_UAV_PERSON_TRACKING_HPP
#define NDNSF_EXAMPLES_UAV_PERSON_TRACKING_HPP

#include "uav-person-tracking-profile.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ndnsf::examples::uav {

/** A stock tracker number scoped to its actual source and explicit tracker lifetime. */
struct UavPersonLocalKey
{
  std::string cameraId;
  std::string sourceId;
  uint64_t sourceEpoch = 0;
  uint64_t trackerGeneration = 0;
  uint64_t byteTrackId = 0;

  bool operator<(const UavPersonLocalKey& other) const;
};

/** Native-owned context from verified input Data, not selected by the model child. */
struct UavPersonFrameContext
{
  std::string cameraId;
  std::string sourceId;
  std::string inputDigest;
  uint64_t sourceEpoch = 0;
  uint64_t trackerGeneration = 0;
  uint64_t sequence = 0;
  int64_t commonPtsUs = 0;
  int width = 0;
  int height = 0;
  bool clockAvailable = false;
  uint64_t clockUncertaintyUs = 0;

  bool operator==(const UavPersonFrameContext& other) const;
};

struct UavPersonWindowContext
{
  std::string runId;
  std::string missionId;
  std::string windowId;
  std::string computationDigest;
  uint64_t sequence = 0;
  std::array<UavPersonFrameContext, 3> frames;

  bool operator==(const UavPersonWindowContext& other) const;
};

struct UavPersonDetection
{
  UavPersonLocalKey key;
  int64_t commonPtsUs = 0;
  std::array<int, 4> box{};
  double confidence = 0;
  std::optional<std::array<float, 512>> embedding;
  bool active = false;
};

struct UavPersonBackendFrame
{
  UavPersonFrameContext context;
  std::vector<UavPersonDetection> detections;
  std::vector<uint64_t> lostByteTrackIds;
};

/** Admitted computation evidence; no global identity or committed application state. */
struct UavPersonBackendWindow
{
  UavPersonWindowContext context;
  std::string profileDigest;
  std::array<UavPersonBackendFrame, 3> frames;
  std::string backendStateJson;
  std::string requestedDevice;
  std::string actualDetectorDevice;
  std::string detectorModelDevice;
  std::string detectorInputDevice;
  std::string actualReidDevice;
  std::string reidModelDevice;
  std::string reidInputDevice;
  uint64_t reidInferenceBatches = 0;
  uint64_t modelWindowNs = 0;
};

/** Validate all backend output against verified native inputs before returning any candidate. */
UavPersonBackendWindow
parseUavPersonBackendWindow(const std::string& json,
                            const UavPersonWindowContext& expected,
                            const UavPersonTrackingProfile& profile);

/** Eligibility witness for a prototype match, not a unique appearance exemplar. */
struct UavPersonAssociationOrigin
{
  UavPersonLocalKey key;
  int64_t ptsUs = 0;
  bool active = false;
};

/** Public decision row; private appearance vectors never enter application results. */
struct UavPersonTrack
{
  UavPersonLocalKey key;
  int64_t commonPtsUs = 0;
  std::array<int, 4> box{};
  double confidence = 0;
  std::optional<uint64_t> globalId;
  std::string associationStatus;
  std::optional<double> distance;
  std::optional<UavPersonAssociationOrigin> associationOrigin;
};

struct UavPersonWindowResult
{
  UavPersonWindowContext context;
  std::array<std::vector<UavPersonTrack>, 3> tracks;
  size_t gallerySize = 0;
  size_t localKeyCount = 0;
  int64_t watermarkUs = 0;
  std::string candidateStateJson;
};

/** Single-owner, bounded person association; callers serialize access per mission. */
class UavPersonTrackingEngine
{
public:
  explicit UavPersonTrackingEngine(UavPersonTrackingProfile profile);

  /** Consume a parser-admitted window; any failure preserves the previous candidate state. */
  UavPersonWindowResult
  processWindow(const UavPersonWindowContext& context, const UavPersonBackendWindow& backend);

  /** Restore an authenticated predecessor for the verified next window; failure is atomic. */
  void restoreState(const std::string& stateJson, const UavPersonWindowContext& expected);

  /** Produce a bounded deterministic snapshot; an uninitialized engine has no snapshot. */
  std::string serializeState() const;

  /** Validated backend-only state, owned until the next successful mutation or destruction. */
  const std::string& backendStateJson() const noexcept;

private:
  struct Sample
  {
    int64_t ptsUs = 0;
    std::array<float, 512> embedding;
  };

  struct Identity
  {
    int64_t lastSeenUs = 0;
    std::vector<Sample> samples;
  };

  struct LocalMapping
  {
    uint64_t globalId = 0;
    int64_t lastSeenUs = 0;
    bool active = false;
  };

  UavPersonWindowResult
  processValidatedWindow(const UavPersonWindowContext& context,
                         const UavPersonBackendWindow& backend);

  void expireAt(int64_t ptsUs);
  void observe(const UavPersonDetection& detection, uint64_t globalId);
  uint64_t allocateIdentity(int64_t ptsUs);
  std::optional<UavPersonAssociationOrigin>
  eligibilityOrigin(uint64_t globalId, const UavPersonDetection& detection,
                    const UavPersonWindowContext& context) const;
  std::optional<std::array<double, 512>> prototype(const Identity& identity) const;
  void swapState(UavPersonTrackingEngine& candidate) noexcept;

private:
  UavPersonTrackingProfile m_profile;
  std::optional<UavPersonWindowContext> m_binding;
  uint64_t m_nextGlobalId = 1;
  int64_t m_watermarkUs = 0;
  std::map<uint64_t, Identity> m_gallery;
  std::map<UavPersonLocalKey, LocalMapping> m_localToGlobal;
  std::string m_backendTrackerState;
};

} // namespace ndnsf::examples::uav

#endif // NDNSF_EXAMPLES_UAV_PERSON_TRACKING_HPP
