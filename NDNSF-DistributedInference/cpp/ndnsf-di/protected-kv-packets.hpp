#ifndef NDNSF_DI_PROTECTED_KV_PACKETS_HPP
#define NDNSF_DI_PROTECTED_KV_PACKETS_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/protected-kv-checkpoint.hpp"

#include <ndn-cxx/data.hpp>

#include <functional>
#include <vector>

namespace ndnsf::di {

struct NativeProtectedKvPacketLimits
{
  NativeProtectedKvCheckpointLimitsV1 checkpoint;
  std::uint64_t maxPacketWireBytes = 0;
  std::size_t maxPackets = 0;
};

struct NativeProtectedKvPacketSet
{
  ndn::Name rootName;
  std::vector<ndn::Name> packetNames;
  std::vector<ndn::Buffer> packetWires;
  std::uint64_t wireBytes = 0;
  std::string wireDigest;
};

// Synchronous owner-thread adapter. Must sign only this borrowed Data and return
// after its callbacks drain; it may not change Name, Content or MetaInfo.
using NativeKvPacketSigner = std::function<void(ndn::Data&)>;
// Configured trust/namespace validation; certificates validate their exact subject
// and issuer, and subsequent application packets validate their Producer.
using NativeKvObjectVerifier = std::function<bool(const ndn::Buffer&, const ndn::Name&)>;

ndn::Name
nativeProtectedKvPacketRootName(const NativeProtectedKvCheckpointReferenceV1& reference,
                               const ndn::security::Certificate& signingCertificate);

NativeProtectedKvPacketSet
nativePrepareProtectedKvPackets(const std::filesystem::path& protectedFile,
  const NativeProtectedKvCheckpointReferenceV1& reference,
  const ndn::security::Certificate& signingCertificate,
  const NativeProtectedKvPacketLimits& limits, const NativeKvPacketSigner& signer,
  const std::function<void()>& requireCurrent);

// Transport validation only. Permission, recipient unwrapping and state import
// remain separate mandatory operations; a signature is not execution authority.
void
nativeMaterializeProtectedKvPackets(const std::vector<ndn::Buffer>& wires,
  const NativeProtectedKvCheckpointReferenceV1& reference, const ndn::Name& expectedRoot,
  const std::filesystem::path& protectedOutput, const NativeProtectedKvPacketLimits& limits,
  const NativeKvObjectVerifier& verifier, const std::function<void()>& requireCurrent,
  const NativeCheckpointExportOptions& options = {});

} // namespace ndnsf::di

#endif // NDNSF_DI_PROTECTED_KV_PACKETS_HPP
