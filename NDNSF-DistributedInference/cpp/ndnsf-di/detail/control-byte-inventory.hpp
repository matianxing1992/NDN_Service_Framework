#ifndef NDNSF_DI_DETAIL_CONTROL_BYTE_INVENTORY_HPP
#define NDNSF_DI_DETAIL_CONTROL_BYTE_INVENTORY_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/request-work-statistics.hpp"
#include "ndn-service-framework/control-byte-statistics.hpp"

#include <iterator>

namespace ndnsf::di::detail {

enum class ControlInventoryScope
{
  MAIN,
  GRANT_AUTHORITY
};

/** Transfer scalar representations once; nested entries are not a byte total. */
inline bool
mergeControlByteInventory(NativeWorkStatistics& destination,
                          const ndn_service_framework::ControlByteStatistics::Snapshot& source,
                          ControlInventoryScope scope) noexcept
{
  struct Names
  {
    const char* mainBytes;
    const char* mainCalls;
    const char* grantBytes;
    const char* grantCalls;
  };
  static constexpr Names NAMES[] = {
    {"core.requestPreparedBytes", "core.requestPreparedCalls",
     "grant.core.requestPreparedBytes", "grant.core.requestPreparedCalls"},
    {"core.ackDecodedBytes", "core.ackDecodedCalls",
     "grant.core.ackDecodedBytes", "grant.core.ackDecodedCalls"},
    {"core.ackDecodeFailedBytes", "core.ackDecodeFailedCalls",
     "grant.core.ackDecodeFailedBytes", "grant.core.ackDecodeFailedCalls"},
    {"core.selectionPreparedBytes", "core.selectionPreparedCalls",
     "grant.core.selectionPreparedBytes", "grant.core.selectionPreparedCalls"},
    {"core.assignmentInlineDecisionBytes", "core.assignmentInlineDecisionCalls",
     "grant.core.assignmentInlineDecisionBytes", "grant.core.assignmentInlineDecisionCalls"},
    {"core.assignmentReferenceDecisionBytes", "core.assignmentReferenceDecisionCalls",
     "grant.core.assignmentReferenceDecisionBytes", "grant.core.assignmentReferenceDecisionCalls"},
    {"core.assignmentExternalPlaintextBytes", "core.assignmentExternalPlaintextCalls",
     "grant.core.assignmentExternalPlaintextBytes", "grant.core.assignmentExternalPlaintextCalls"},
    {"core.assignmentExternalReferenceBytes", "core.assignmentExternalReferenceCalls",
     "grant.core.assignmentExternalReferenceBytes", "grant.core.assignmentExternalReferenceCalls"},
    {"core.inputModeInlineBytes", "core.inputModeInlineCalls",
     "grant.core.inputModeInlineBytes", "grant.core.inputModeInlineCalls"},
    {"core.inputModeSanitizedRemovedBytes", "core.inputModeSanitizedCalls",
     "grant.core.inputModeSanitizedRemovedBytes", "grant.core.inputModeSanitizedCalls"},
    {"core.inputModeExplicitDiscoveryBytes", "core.inputModeExplicitDiscoveryCalls",
     "grant.core.inputModeExplicitDiscoveryBytes", "grant.core.inputModeExplicitDiscoveryCalls"},
    {"core.inputModeEmptyDiscoveryBytes", "core.inputModeEmptyDiscoveryCalls",
     "grant.core.inputModeEmptyDiscoveryBytes", "grant.core.inputModeEmptyDiscoveryCalls"},
    {"core.requestInputPublishedBytes", "core.requestInputPublishedCalls",
     "grant.core.requestInputPublishedBytes", "grant.core.requestInputPublishedCalls"},
    {"core.requestInputPacketPublishedBytes", "core.requestInputPacketPublishedCalls",
     "grant.core.requestInputPacketPublishedBytes", "grant.core.requestInputPacketPublishedCalls"},
    {"core.responseDecodedBytes", "core.responseDecodedCalls",
     "grant.core.responseDecodedBytes", "grant.core.responseDecodedCalls"}
  };
  static_assert(std::size(NAMES) == static_cast<std::size_t>(
    ndn_service_framework::ControlByteKind::COUNT));
  const bool grant = scope == ControlInventoryScope::GRANT_AUTHORITY;
  for (std::size_t i = 0; i < source.entries.size(); ++i) {
    const auto& names = NAMES[i];
    if (!destination.tryAddCount(grant ? names.grantBytes : names.mainBytes,
                                source.entries[i].bytes) ||
        !destination.tryAddCount(grant ? names.grantCalls : names.mainCalls,
                                source.entries[i].calls))
      return false;
  }
  if (!destination.tryAddCount(grant ? "grant.core.responseCaptureEnabled" :
                                     "core.responseCaptureEnabled",
                               source.responseBytesCaptured ? 1 : 0) ||
      !destination.tryAddCount(grant ? "grant.core.controlInventorySnapshots" :
                                     "core.controlInventorySnapshots", 1))
    return false;
  return destination.tryAddCount(grant ? "grant.core.controlInventoryComplete" :
                                        "core.controlInventoryComplete", source.complete ? 1 : 0);
}

} // namespace ndnsf::di::detail

#endif // NDNSF_DI_DETAIL_CONTROL_BYTE_INVENTORY_HPP
