#ifndef NDN_SERVICE_FRAMEWORK_COLLABORATION_DATAFLOW_HPP
#define NDN_SERVICE_FRAMEWORK_COLLABORATION_DATAFLOW_HPP

#include <ndn-cxx/name.hpp>
#include <map>
#include <string>
#include <vector>
#include <cstdint>

namespace ndn_service_framework {

enum class DependencyRedundancy { None, Xor };

/** Request-wide transport choice. None disables FEC, not reliable retrieval. */
struct DependencyTransportOptions
{
  DependencyRedundancy redundancy = DependencyRedundancy::None;
};

/** Provider-owned limits, frozen with the scoped registration generation. */
struct CollaborationAdmissionLimits
{
  size_t maxRetainedObjects = 1024;
  size_t maxRetainedBytes = 64 * 1024 * 1024;
  size_t maxQueuedCallbacks = 256;
  size_t maxInFlightReads = 128;
  uint64_t maxInputAcquisitionMs = 5000;
  void validate() const;
};

/** Framework-owned metadata inside the authenticated, recipient-protected
 * Selection. Locations and application assignments are not role authority. */
struct CollaborationDataflowMetadataV1
{
  std::string profile = "data-admission-v1";
  std::string dependencyRecoveryProfile = "arq-v1";
  std::map<std::string, std::vector<ndn::Name>> roleBindings;

  ndn::Block wireEncode() const;
  /** Reject malformed, duplicate, noncanonical or unsupported metadata. */
  static CollaborationDataflowMetadataV1 wireDecode(const ndn::Block& wire);
};

} // namespace ndn_service_framework
#endif
