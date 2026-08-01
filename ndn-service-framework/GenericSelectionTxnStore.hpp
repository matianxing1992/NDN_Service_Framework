#ifndef NDN_SERVICE_FRAMEWORK_GENERIC_SELECTION_TXN_STORE_HPP
#define NDN_SERVICE_FRAMEWORK_GENERIC_SELECTION_TXN_STORE_HPP

#include "common.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace ndn_service_framework {

enum class GenericSelectionTxnState
{
  Absent,
  Validating,
  Committed,
  Aborted,
  Tombstoned
};

struct AuthenticatedSelectionContext
{
  std::string transactionId;
  ndn::Name serviceName;
  ndn::Name requestId;
  uint64_t attempt = 0;
  std::string selectionIdentity;
  std::string selectionPayloadDigest;
  ndn::Name providerIdentity;
  std::string providerBootEpoch;
  std::chrono::steady_clock::time_point localDeadline;
  uint64_t expiresAtUnixMs = 0;
  std::string providerTokenRecordRef;
  std::optional<std::string> leaseRecordRef;
};

struct OpaqueSelectionPrepareResult
{
  std::string participantId;
  uint32_t participantVersion = 0;
  ndn::Buffer commitBlob;
  std::string commitBlobDigest;
  ndn::Buffer acceptancePayload;
  std::string acceptancePayloadDigest;
};

struct GenericCommittedSelectionView
{
  std::string transactionId;
  std::string participantId;
  uint32_t participantVersion = 0;
  ndn::Name serviceName;
  ndn::Name requestId;
  uint64_t attempt = 0;
  std::string selectionIdentity;
  std::string selectionPayloadDigest;
  ndn::Name providerIdentity;
  std::string providerBootEpoch;
  std::string providerTokenRecordRef;
  std::optional<std::string> leaseRecordRef;
  ndn::Buffer commitBlob;
  std::string commitBlobDigest;
  ndn::Buffer acceptancePayload;
  std::string acceptancePayloadDigest;
  uint64_t committedAtUnixMs = 0;
  uint64_t expiresAtUnixMs = 0;
};

class OpaqueSelectionParticipant
{
public:
  virtual ~OpaqueSelectionParticipant() = default;

  virtual std::string participantId() const = 0;
  virtual uint32_t participantVersion() const = 0;

  virtual OpaqueSelectionPrepareResult
  prepare(const AuthenticatedSelectionContext& context,
          ndn::span<const uint8_t> payload) = 0;

  virtual void
  onCommitted(const GenericCommittedSelectionView& committed) = 0;

  virtual void
  onAborted(const std::string& transactionId,
            const std::string& reasonCode) = 0;
};

struct GenericSelectionTxnOptions
{
  size_t maxCommitBlobBytes = 1024 * 1024;
  size_t maxAcceptancePayloadBytes = 64 * 1024;
  size_t maxSelectionPayloadBytes = 1024 * 1024;
  std::chrono::milliseconds maxPrepareTime{1000};
};

class GenericSelectionTxnStore
{
public:
  GenericSelectionTxnStore(std::string walPath,
                           ndn::Buffer storageKey,
                           std::string storageKeyEpoch,
                           GenericSelectionTxnOptions options = {});

  GenericCommittedSelectionView
  commit(const AuthenticatedSelectionContext& context,
         ndn::span<const uint8_t> selectionPayload,
         OpaqueSelectionParticipant& participant,
         bool providerTokenStillLive,
         bool leaseStillLive = true,
         bool notifyParticipant = true);

  std::optional<GenericCommittedSelectionView>
  findCommitted(const std::string& transactionId) const;

  void replayCommitted(
      const std::map<std::string, std::shared_ptr<OpaqueSelectionParticipant>>&
          participants,
      const std::string& currentProviderBootEpoch);

  bool tombstone(const std::string& transactionId, uint64_t nowUnixMs);

  size_t size() const;

  static std::string digest(ndn::span<const uint8_t> bytes);

private:
  struct Record
  {
    GenericSelectionTxnState state = GenericSelectionTxnState::Absent;
    GenericCommittedSelectionView view;
    std::string storageKeyEpoch;
  };

  void load();
  void appendRecord(const Record& record);
  void appendAborted(const AuthenticatedSelectionContext& context,
                     OpaqueSelectionParticipant& participant);
  GenericCommittedSelectionView toView(const Record& record) const;

private:
  std::string m_walPath;
  ndn::Buffer m_storageKey;
  std::string m_storageKeyEpoch;
  GenericSelectionTxnOptions m_options;
  mutable std::mutex m_mutex;
  std::condition_variable m_stateChanged;
  std::map<std::string, Record> m_records;
  std::map<std::string, std::string> m_validatingTransactions;
  std::map<std::string, std::string> m_tokenOwners;
  std::map<std::string, std::string> m_leaseOwners;
  std::map<std::string, std::string> m_selectionOwners;
};

} // namespace ndn_service_framework

#endif
