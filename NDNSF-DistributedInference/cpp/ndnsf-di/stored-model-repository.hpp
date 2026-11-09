#ifndef NDNSF_DI_STORED_MODEL_REPOSITORY_HPP
#define NDNSF_DI_STORED_MODEL_REPOSITORY_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/stored-model-capability.hpp"

#include <ndn-cxx/security/key-chain.hpp>

#include <functional>
#include <map>
#include <optional>
#include <utility>

namespace ndnsf::di {

inline constexpr std::uint64_t NATIVE_STORED_MODEL_MAX_PUBLICATION_BYTES = 100000000000ULL;

/** Upload-only bounds for binary owned INSERT; retrieval keeps its smaller fetch windows. */
inline constexpr std::size_t NATIVE_STORED_MODEL_UPLOAD_PACKETS = 4096;
inline constexpr std::size_t NATIVE_STORED_MODEL_UPLOAD_NAMES = 512U << 10;
inline constexpr std::uint64_t NATIVE_STORED_MODEL_UPLOAD_WIRE = 32U << 20;

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
  /** Preparing owner only: confirm an earlier exact packet group through authenticated Repo
   * metadata, without uploading/downloading its model payload again. Throw if missing/mismatched.
   */
  std::function<void(const ndn::Name&, const std::string&, std::uint64_t)> requireCommitted;
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
  friend NativeStoredModelPublication nativePublishStoredModel(ndn::KeyChain&,
    const ndn::Name&, const ndn::Name&, const ndn::Name&, const std::string&,
    const std::string&, const std::string&,
    const std::function<std::optional<NativeStoredModelInput>()>&, const NativeStoredModelPacketIo&,
    const std::filesystem::path&);
  friend NativeStoredModelPublication nativePublishStoredModel(ndn::KeyChain&,
    const ndn::Name&, const ndn::Name&, const ndn::Name&, const std::string&,
    const std::string&, const std::string&,
    const std::function<std::optional<NativeStoredModelInput>()>&, const NativeStoredModelPacketIo&,
    const std::filesystem::path&, const std::function<void(const ndn::Name&)>&);
  friend std::uint64_t nativePreflightStoredModelPublication(ndn::KeyChain&,
    const ndn::Name&, const ndn::Name&, const std::string&, const std::string&,
    const std::string&, const std::vector<std::pair<std::string, std::uint64_t>>&);

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

/** Validate the actual object/index/root geometry before publication and return a conservative
 * original-wire byte bound. Uses fixed-width digest placeholders, never reads model payloads.
 */
std::uint64_t
nativePreflightStoredModelPublication(ndn::KeyChain& keyChain, const ndn::Name& producer,
  const ndn::Name& publicationPrefix, const std::string& modelDigest,
  const std::string& catalogId, const std::string& manifestId,
  const std::vector<std::pair<std::string, std::uint64_t>>& objects);

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

/** Persist one locked private preparation state and at most one encrypted object spool.
 * Resume reuses the original keys/Names/ciphertext and confirms prior Repo commits. A ready hit
 * uses no source callback or publication. Keep this state outside Repo and Provider mounts.
 */
NativeStoredModelPublication
nativePublishStoredModel(ndn::KeyChain& keyChain, const ndn::Name& producer,
  const ndn::Name& repository, const ndn::Name& publicationPrefix,
  const std::string& modelDigest, const std::string& catalogId, const std::string& manifestId,
  const std::function<std::optional<NativeStoredModelInput>()>& nextObject,
  const NativeStoredModelPacketIo& io, const std::filesystem::path& preparationState);

/** Resolve an omitted prefix inside the existing locked journal, then bind source metadata.
 * An empty prefix requires preparationState. bindPrefix runs synchronously under that owner
 * before nextObject or writes, and is skipped for READY recovery. It must not publish objects.
 */
NativeStoredModelPublication
nativePublishStoredModel(ndn::KeyChain& keyChain, const ndn::Name& producer,
  const ndn::Name& repository, const ndn::Name& publicationPrefix,
  const std::string& modelDigest, const std::string& catalogId, const std::string& manifestId,
  const std::function<std::optional<NativeStoredModelInput>()>& nextObject,
  const NativeStoredModelPacketIo& io, const std::filesystem::path& preparationState,
  const std::function<void(const ndn::Name&)>& bindPrefix);

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
