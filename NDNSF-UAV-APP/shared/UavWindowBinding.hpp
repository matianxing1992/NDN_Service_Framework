#ifndef NDNSF_UAV_WINDOW_BINDING_HPP
#define NDNSF_UAV_WINDOW_BINDING_HPP

#include <ndn-cxx/name.hpp>

#include <cstdint>
#include <string>
#include <variant>

namespace ndnsf::examples::uav {

constexpr size_t MAX_WINDOW_BINDING_BYTES = 16384;

/** One exact Named Data reference carried by a window binding. */
struct UavBindingObjectRef
{
  std::string name;
  std::string digest;
  ndn::Name producerIdentity;
};

/**
 * Immutable Source-owned binding for one selected collaboration window.
 *
 * It fixes the exact FRAME/OBSERVATION objects and per-source capture context
 * that the Compute role must admit; it is not a result and carries no state.
 */
struct UavWindowBinding
{
  std::string runId;
  std::string wireMissionId;
  std::string windowId;
  uint64_t windowSequence = 0;
  std::string requestId;
  std::string cameraId;
  std::string sourceId;
  uint64_t sourceEpoch = 0;
  uint64_t captureSequence = 0;
  uint64_t trackerGeneration = 0;
  std::string descriptorDigest;
  int64_t commonPtsUs = 0;
  uint64_t clockUncertaintyUs = 0;
  UavBindingObjectRef frameRef;
  UavBindingObjectRef observationRef;
};

/** A selected Source delivered a terminal outcome without a captured frame. */
struct UavWindowFailure
{
  ndn::Name producerIdentity;
  std::string runId;
  std::string wireMissionId;
  std::string windowId;
  uint64_t windowSequence = 0;
  std::string requestId;
  std::string cameraId;
  std::string sourceId;
  uint64_t sourceEpoch = 0;
  std::string descriptorDigest;
  std::string reason;
};

using UavWindowOutcome = std::variant<UavWindowBinding, UavWindowFailure>;

/** Producer-owned name in the tracking/window-binding category. */
ndn::Name
makeUavWindowBindingName(const ndn::Name& producerIdentity,
                         const std::string& runId,
                         const std::string& wireMissionId,
                         const std::string& windowId,
                         const std::string& requestId);

/** Canonical Source descriptor returned by the protected source-info operation. */
struct UavSourceDescriptor
{
  ndn::Name producerIdentity;
  std::string cameraId;
  std::string sourceId;
  uint64_t sourceEpoch = 0;
  std::string acquisitionKind;
  std::string deliveryMode;
};

std::string
renderUavSourceDescriptor(const UavSourceDescriptor& descriptor);

UavSourceDescriptor
parseUavSourceDescriptor(const std::string& json,
                         size_t maximumBytes = MAX_WINDOW_BINDING_BYTES);

std::string
uavSourceDescriptorDigest(const UavSourceDescriptor& descriptor);

/** Source-info request; optional filters let the caller ask for its camera. */
struct UavSourceInfoRequest
{
  std::string cameraId;
  std::string sourceId;
  uint64_t sourceEpoch = 0;
};

std::string
renderUavSourceInfoRequest(const UavSourceInfoRequest& request);

UavSourceInfoRequest
parseUavSourceInfoRequest(const std::string& json,
                          size_t maximumBytes = MAX_WINDOW_BINDING_BYTES);

/** Protected source-info response: canonical descriptor bytes plus its digest. */
struct UavSourceInfoResponse
{
  UavSourceDescriptor descriptor;
  std::string digest;
};

std::string
renderUavSourceInfoResponse(const UavSourceInfoResponse& response);

UavSourceInfoResponse
parseUavSourceInfoResponse(const std::string& json,
                           size_t maximumBytes = MAX_WINDOW_BINDING_BYTES);

/** Digest of the bounded source descriptor used by source-info/binding admission. */
std::string
makeUavSourceDescriptorDigest(const ndn::Name& producerIdentity,
                              const std::string& cameraId,
                              const std::string& sourceId,
                              uint64_t sourceEpoch,
                              const std::string& acquisitionKind,
                              const std::string& deliveryMode);

/** Validate every field and both exact references before publication. */
void
validateUavWindowBinding(const UavWindowBinding& binding);

std::string
renderUavWindowBinding(const UavWindowBinding& binding);

UavWindowBinding
parseUavWindowBinding(const std::string& json,
                      size_t maximumBytes = MAX_WINDOW_BINDING_BYTES);

std::string
renderUavWindowFailure(const UavWindowFailure& failure);

/** Parse one ready or failure input; each schema has a disjoint exact key set. */
UavWindowOutcome
parseUavWindowOutcome(const std::string& json,
                      size_t maximumBytes = MAX_WINDOW_BINDING_BYTES);

} // namespace ndnsf::examples::uav

#endif // NDNSF_UAV_WINDOW_BINDING_HPP
