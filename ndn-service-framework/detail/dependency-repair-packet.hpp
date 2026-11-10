#ifndef NDN_SERVICE_FRAMEWORK_DETAIL_DEPENDENCY_REPAIR_PACKET_HPP
#define NDN_SERVICE_FRAMEWORK_DETAIL_DEPENDENCY_REPAIR_PACKET_HPP

#include <ndn-cxx/data.hpp>
#include <optional>
#include <string>
#include <vector>

namespace ndn_service_framework::detail {

struct DependencyRepairMember
{
  ndn::Name name;
  uint64_t wireLength = 0;
  std::string wireSha256;
};

struct DependencyRepairGroup
{
  uint64_t index = 0;
  std::vector<DependencyRepairMember> members;
  ndn::Name repairRoot;
  uint64_t plaintextLength = 0;
  std::string plaintextSha256;
};

struct DependencyRepairLimits
{
  size_t maxRepairBytes = 64 * 1024;
  size_t maxSourcePackets = 65536;
  size_t maxPacketWireBytes = ndn::MAX_NDN_PACKET_SIZE;
};

std::string dependencyWireDigest(ndn::span<const uint8_t> bytes);
inline std::string dependencyWireDigest(const ndn::Buffer& bytes)
{
  return dependencyWireDigest(ndn::span<const uint8_t>(bytes.data(), bytes.size()));
}

/** Recover exactly one missing complete Data wire. All supplied sources and
 * parity must match the authenticated repair packet. The caller must then parse and
 * revalidate the original name, signature and current authority before use.
 */
std::optional<ndn::Buffer> recoverDependencyWire(const DependencyRepairGroup& group,
    const std::vector<std::optional<ndn::Buffer>>& originals,
    const ndn::Buffer& parity, const DependencyRepairLimits& limits = {});

inline constexpr size_t DEPENDENCY_XOR_GROUP_SIZE = 8;

/** Content of an in-band repair Data packet in the original object's packet
 * set. The enclosing Data is signed by the original Producer. Members describe
 * frozen original wire, including its existing protection. Decoding this value
 * grants neither trust nor permission; recovered Data still requires admission.
 */
struct DependencyRepairPacketV1
{
  ndn::Name originalRootName;
  std::string rootWireSha256;
  DependencyRepairGroup group;
  ndn::Buffer codedBytes;

  ndn::Block
  wireEncode(const ndn::Name& producer, const DependencyRepairLimits& limits = {}) const;

  static DependencyRepairPacketV1
  wireDecode(const ndn::Block& wire, const ndn::Name& producer,
             const DependencyRepairLimits& limits = {});
};

} // namespace ndn_service_framework::detail
#endif
