#ifndef NDNSF_DI_STORED_MODEL_REPOSITORY_HPP
#define NDNSF_DI_STORED_MODEL_REPOSITORY_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/stored-model-capability.hpp"

#include <ndn-cxx/security/key-chain.hpp>

#include <atomic>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <thread>
#include <utility>

namespace ndnsf::di {

inline constexpr std::uint64_t NATIVE_STORED_MODEL_MAX_PUBLICATION_BYTES = 100000000000ULL;

/** Upload-only bounds for binary owned INSERT; retrieval keeps its smaller fetch windows. */
inline constexpr std::size_t NATIVE_STORED_MODEL_UPLOAD_PACKETS = 4096;
inline constexpr std::size_t NATIVE_STORED_MODEL_UPLOAD_NAMES = 512U << 10;
inline constexpr std::uint64_t NATIVE_STORED_MODEL_UPLOAD_WIRE = 32U << 20;

/** Existing Repo adapters own transport, current storage authorization and cancellation.
 * Calls are synchronous, off the Face thread. Each bounded commit confirms durable original
 * wire storage against expectedDigest before returning. Fetch returns the exact requested order.
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
  /** Explicit native capability: synchronous durable commit may run concurrently.
   * Both the original current check and this additional cancellation check must
   * be honored. The original check must be thread-safe. Generic IO omits this
   * capability and retains caller-thread serial commit behavior.
   */
  std::function<void(PacketWires, const std::string&, const std::function<void()>&)>
    commitConcurrent;
};

/** Own at most six asynchronous original-packet commits on one caller thread.
 * Tasks must honor their supplied cancellation check. Observers record verified
 * durable successes on the owner thread in submission order, including during
 * failure drain; they must not advance an object/journal or publish a root.
 * All referenced observer inputs must outlive this scope. Destruction cancels
 * and drains without throwing; normal callers explicitly finish before use.
 * Six in-flight groups consume at most 192 MiB of wires. A caller preparing a
 * seventh group can retain another 32 MiB; this is not a total RSS bound.
 */
class NativeModelCommitPipeline
{
public:
  using CurrentCheck = std::function<void()>;
  using Commit = std::function<void(const CurrentCheck&)>;
  static constexpr std::size_t MAX_ACTIVE_COMMITS = 6;

  explicit NativeModelCommitPipeline(CurrentCheck requireCurrent);
  ~NativeModelCommitPipeline() noexcept;
  NativeModelCommitPipeline(const NativeModelCommitPipeline&) = delete;
  NativeModelCommitPipeline& operator=(const NativeModelCommitPipeline&) = delete;

  void submit(std::uint64_t wireBytes, Commit commit, std::function<void()> observe = {});
  /** Confirm outstanding groups before submitting a dependent object. */
  void drain();
  void finish();

private:
  struct Pending
  {
    std::future<void> completion;
    std::function<void()> observe;
  };

  void requireOwner() const;
  void completeFirst();
  void cancelAndDrain() noexcept;
  [[noreturn]] void fail(std::exception_ptr error);

  CurrentCheck m_requireCurrent;
  std::thread::id m_owner;
  std::shared_ptr<std::atomic<bool>> m_cancelled;
  std::deque<Pending> m_pending;
  bool m_closed = false;
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
