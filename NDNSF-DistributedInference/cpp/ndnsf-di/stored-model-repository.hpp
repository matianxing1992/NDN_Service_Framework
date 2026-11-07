#ifndef NDNSF_DI_STORED_MODEL_REPOSITORY_HPP
#define NDNSF_DI_STORED_MODEL_REPOSITORY_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/stored-model-capability.hpp"

#include <ndn-cxx/security/key-chain.hpp>

#include <functional>
#include <map>
#include <optional>

namespace ndnsf::di {

/** Existing Repo adapters own transport, current storage authorization and cancellation.
 * Calls are synchronous, off the Face thread. Each bounded commit must confirm durable original
 * wire readback against expectedDigest before returning. Fetch returns the exact requested order.
 * The owner supplies a deadline/close check; no new worker or authorization state lives here.
 */
struct NativeStoredModelPacketIo
{
  using PacketWires = std::vector<std::vector<std::uint8_t>>;
  std::function<void(PacketWires, const std::string& expectedDigest)> commit;
  std::function<PacketWires(const std::vector<ndn::Name>&)> fetch;
  std::function<void()> requireCurrent;
};

struct NativeStoredModelIndexedObject
{
  NativeStoredModelObject object;
  std::uint32_t segmentBytes = 0;
};

/** One owned logical object from the existing model/material preparation owner. */
struct NativeStoredModelInput
{
  std::string id;
  std::vector<std::uint8_t> plaintext;
};

struct NativeStoredModelPublication;

/** Factory-owned root-authenticated index; callers cannot modify its admitted object mapping. */
class NativeStoredModelCatalog
{
public:
  const std::string& modelDigest() const noexcept { return m_modelDigest; }
  const std::string& catalogId() const noexcept { return m_catalogId; }
  const std::string& manifestId() const noexcept { return m_manifestId; }
  const std::map<std::string, NativeStoredModelIndexedObject>& objects() const noexcept
  { return m_objects; }
  const NativeStoredModelReference& publication() const noexcept { return m_publication; }

private:
  NativeStoredModelCatalog() = default;
  friend NativeStoredModelCatalog nativeLoadStoredModelCatalog(
    const NativeStoredModelReference&, const NativeStoredModelPacketIo&);
  friend NativeStoredModelPublication nativePublishStoredModel(ndn::KeyChain&,
    const ndn::Name&, const ndn::Name&, const ndn::Name&, const std::string&,
    const std::string&, const std::string&,
    const std::function<std::optional<NativeStoredModelInput>()>&, const NativeStoredModelPacketIo&);

  NativeStoredModelReference m_publication;
  std::string m_modelDigest;
  std::string m_catalogId;
  std::string m_manifestId;
  std::map<std::string, NativeStoredModelIndexedObject> m_objects;
};

struct NativeStoredModelPublication
{
  NativeStoredModelHandle handle;
  std::uint64_t sourceBytes = 0;
  std::uint64_t wireBytes = 0;
  std::uint64_t packetGroups = 0;
  std::uint64_t objectCount = 0;
};

/** Derive the original prefix of one immutable object in this publication. */
ndn::Name
nativeStoredModelObjectName(const ndn::Name& publicationPrefix, const std::string& id);

/** Publish objects, then index, then root. The private handle is returned only after all commits.
 * publicationPrefix is a fresh Producer-owned immutable version selected by the preparation owner.
 * The lazy source must honor io.requireCurrent and never retains all model objects in memory.
 * Ambiguous failure leaves committed groups for the existing preparation owner's reconciliation.
 */
NativeStoredModelPublication
nativePublishStoredModel(ndn::KeyChain& keyChain, const ndn::Name& producer,
  const ndn::Name& repository, const ndn::Name& publicationPrefix,
  const std::string& modelDigest, const std::string& catalogId, const std::string& manifestId,
  const std::function<std::optional<NativeStoredModelInput>()>& nextObject,
  const NativeStoredModelPacketIo& io);

/** Read-only admission; no source loader or commit callback is invoked. */
NativeStoredModelCatalog
nativeLoadStoredModelCatalog(const NativeStoredModelReference& publication,
                             const NativeStoredModelPacketIo& io);

/** Fetch only the named admitted logical object; does not load or publish other objects. */
std::vector<std::uint8_t>
nativeReadStoredModelObject(const NativeStoredModelHandle& handle,
                            const NativeStoredModelCatalog& catalog, const std::string& id,
                            const NativeStoredModelPacketIo& io);

} // namespace ndnsf::di

#endif // NDNSF_DI_STORED_MODEL_REPOSITORY_HPP
