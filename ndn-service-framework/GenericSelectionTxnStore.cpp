#include "GenericSelectionTxnStore.hpp"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <future>
#include <fcntl.h>
#include <fstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace ndn_service_framework {
namespace {

constexpr std::array<uint8_t, 8> MAGIC{{'N', 'D', 'N', 'T', 'X', 'N', '2', 0}};
constexpr size_t NONCE_SIZE = 12;
constexpr size_t TAG_SIZE = 16;
constexpr uint32_t MAX_FRAME_BYTES = 4 * 1024 * 1024;

uint64_t
unixNowMs()
{
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
}

void
putU32(std::vector<uint8_t>& output, uint32_t value)
{
  for (int shift = 24; shift >= 0; shift -= 8)
    output.push_back(static_cast<uint8_t>((value >> shift) & 0xff));
}

void
putU64(std::vector<uint8_t>& output, uint64_t value)
{
  for (int shift = 56; shift >= 0; shift -= 8)
    output.push_back(static_cast<uint8_t>((value >> shift) & 0xff));
}

void
putString(std::vector<uint8_t>& output, const std::string& value)
{
  if (value.size() > MAX_FRAME_BYTES)
    throw std::length_error("generic selection field exceeds WAL bound");
  putU32(output, static_cast<uint32_t>(value.size()));
  output.insert(output.end(), value.begin(), value.end());
}

void
putBuffer(std::vector<uint8_t>& output, const ndn::Buffer& value)
{
  if (value.size() > MAX_FRAME_BYTES)
    throw std::length_error("generic selection buffer exceeds WAL bound");
  putU32(output, static_cast<uint32_t>(value.size()));
  output.insert(output.end(), value.begin(), value.end());
}

class Decoder
{
public:
  explicit Decoder(const std::vector<uint8_t>& input)
    : m_input(input)
  {
  }

  uint32_t readU32()
  {
    require(4);
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i)
      value = (value << 8) | m_input[m_offset++];
    return value;
  }

  uint64_t readU64()
  {
    require(8);
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i)
      value = (value << 8) | m_input[m_offset++];
    return value;
  }

  std::string readString()
  {
    const auto size = readU32();
    require(size);
    std::string value(
        reinterpret_cast<const char*>(m_input.data() + m_offset), size);
    m_offset += size;
    return value;
  }

  ndn::Buffer readBuffer()
  {
    const auto size = readU32();
    require(size);
    ndn::Buffer value(m_input.data() + m_offset, size);
    m_offset += size;
    return value;
  }

  void requireFinished() const
  {
    if (m_offset != m_input.size())
      throw std::runtime_error("generic selection WAL record has trailing bytes");
  }

private:
  void require(size_t size) const
  {
    if (size > m_input.size() - m_offset)
      throw std::runtime_error("truncated generic selection WAL record");
  }

private:
  const std::vector<uint8_t>& m_input;
  size_t m_offset = 0;
};

struct EncryptedFrame
{
  std::array<uint8_t, NONCE_SIZE> nonce{};
  std::vector<uint8_t> ciphertext;
  std::array<uint8_t, TAG_SIZE> tag{};
};

EncryptedFrame
encrypt(const ndn::Buffer& key, const std::vector<uint8_t>& plaintext,
        const std::string& associatedData)
{
  EncryptedFrame output;
  if (RAND_bytes(output.nonce.data(), output.nonce.size()) != 1)
    throw std::runtime_error("generic selection WAL nonce generation failed");
  output.ciphertext.resize(plaintext.size());
  auto* context = EVP_CIPHER_CTX_new();
  if (context == nullptr)
    throw std::runtime_error("generic selection WAL cipher allocation failed");
  int length = 0;
  int total = 0;
  bool ok =
      EVP_EncryptInit_ex(context, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
      EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN,
                          output.nonce.size(), nullptr) == 1 &&
      EVP_EncryptInit_ex(context, nullptr, nullptr, key.data(),
                         output.nonce.data()) == 1 &&
      EVP_EncryptUpdate(
          context, nullptr, &length,
          reinterpret_cast<const uint8_t*>(associatedData.data()),
          associatedData.size()) == 1 &&
      EVP_EncryptUpdate(context, output.ciphertext.data(), &length,
                        plaintext.data(), plaintext.size()) == 1;
  total = length;
  ok = ok && EVP_EncryptFinal_ex(
      context, output.ciphertext.data() + total, &length) == 1;
  total += length;
  output.ciphertext.resize(total);
  ok = ok && EVP_CIPHER_CTX_ctrl(
      context, EVP_CTRL_GCM_GET_TAG, output.tag.size(), output.tag.data()) == 1;
  EVP_CIPHER_CTX_free(context);
  if (!ok)
    throw std::runtime_error("generic selection WAL encryption failed");
  return output;
}

std::vector<uint8_t>
decrypt(const ndn::Buffer& key, const EncryptedFrame& input,
        const std::string& associatedData)
{
  std::vector<uint8_t> output(input.ciphertext.size());
  auto* context = EVP_CIPHER_CTX_new();
  if (context == nullptr)
    throw std::runtime_error("generic selection WAL cipher allocation failed");
  int length = 0;
  int total = 0;
  bool ok =
      EVP_DecryptInit_ex(context, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
      EVP_CIPHER_CTX_ctrl(context, EVP_CTRL_GCM_SET_IVLEN,
                          input.nonce.size(), nullptr) == 1 &&
      EVP_DecryptInit_ex(context, nullptr, nullptr, key.data(),
                         input.nonce.data()) == 1 &&
      EVP_DecryptUpdate(
          context, nullptr, &length,
          reinterpret_cast<const uint8_t*>(associatedData.data()),
          associatedData.size()) == 1 &&
      EVP_DecryptUpdate(context, output.data(), &length,
                        input.ciphertext.data(), input.ciphertext.size()) == 1;
  total = length;
  ok = ok && EVP_CIPHER_CTX_ctrl(
      context, EVP_CTRL_GCM_SET_TAG, input.tag.size(),
      const_cast<uint8_t*>(input.tag.data())) == 1;
  const int finalResult = EVP_DecryptFinal_ex(
      context, output.data() + total, &length);
  EVP_CIPHER_CTX_free(context);
  if (!ok || finalResult != 1)
    throw std::runtime_error(
        "generic selection WAL authentication failed");
  total += length;
  output.resize(total);
  return output;
}

void
writeAll(int fd, const uint8_t* data, size_t size)
{
  while (size > 0) {
    const auto written = ::write(fd, data, size);
    if (written < 0) {
      if (errno == EINTR)
        continue;
      throw std::runtime_error(
          "generic selection WAL write failed: " +
          std::string(std::strerror(errno)));
    }
    data += written;
    size -= static_cast<size_t>(written);
  }
}

} // namespace

GenericSelectionTxnStore::GenericSelectionTxnStore(
    std::string walPath, ndn::Buffer storageKey,
    std::string storageKeyEpoch, GenericSelectionTxnOptions options)
  : m_walPath(std::move(walPath))
  , m_storageKey(std::move(storageKey))
  , m_storageKeyEpoch(std::move(storageKeyEpoch))
  , m_options(options)
{
  if (m_walPath.empty() || m_storageKey.size() != 32 ||
      m_storageKeyEpoch.empty())
    throw std::invalid_argument(
        "generic selection WAL path/key/epoch is incomplete");
  if (m_options.maxCommitBlobBytes == 0 ||
      m_options.maxAcceptancePayloadBytes == 0 ||
      m_options.maxSelectionPayloadBytes == 0 ||
      m_options.maxPrepareTime.count() <= 0)
    throw std::invalid_argument("generic selection WAL bounds are invalid");
  const auto parent = std::filesystem::path(m_walPath).parent_path();
  if (!parent.empty()) {
    std::filesystem::create_directories(parent);
    ::chmod(parent.c_str(), S_IRWXU);
  }
  load();
}

std::string
GenericSelectionTxnStore::digest(ndn::span<const uint8_t> bytes)
{
  std::array<uint8_t, 32> output{};
  unsigned int outputSize = 0;
  auto* context = EVP_MD_CTX_new();
  if (context == nullptr ||
      EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1 ||
      EVP_DigestUpdate(context, bytes.data(), bytes.size()) != 1 ||
      EVP_DigestFinal_ex(context, output.data(), &outputSize) != 1) {
    if (context != nullptr)
      EVP_MD_CTX_free(context);
    throw std::runtime_error("generic selection digest failed");
  }
  EVP_MD_CTX_free(context);
  static constexpr char HEX[] = "0123456789abcdef";
  std::string value = "sha256:";
  value.reserve(71);
  for (const auto byte : output) {
    value.push_back(HEX[(byte >> 4) & 0xf]);
    value.push_back(HEX[byte & 0xf]);
  }
  return value;
}

GenericCommittedSelectionView
GenericSelectionTxnStore::commit(
    const AuthenticatedSelectionContext& context,
    ndn::span<const uint8_t> selectionPayload,
    OpaqueSelectionParticipant& participant,
    bool providerTokenStillLive,
    bool leaseStillLive,
    bool notifyParticipant)
{
  if (context.transactionId.empty() || context.attempt == 0 ||
      context.selectionIdentity.empty() ||
      context.selectionPayloadDigest != digest(selectionPayload) ||
      context.providerTokenRecordRef.empty() ||
      context.providerBootEpoch.empty())
    throw std::invalid_argument(
        "authenticated generic Selection context is incomplete");
  if (selectionPayload.size() > m_options.maxSelectionPayloadBytes)
    throw std::length_error("generic Selection payload exceeds bound");
  if (std::chrono::steady_clock::now() >= context.localDeadline ||
      (context.expiresAtUnixMs > 0 && unixNowMs() >= context.expiresAtUnixMs))
    throw std::runtime_error("generic Selection transaction deadline expired");

  const auto validationFingerprint =
      context.selectionPayloadDigest + "\n" + context.selectionIdentity + "\n" +
      context.providerTokenRecordRef + "\n" + participant.participantId() + "\n" +
      std::to_string(participant.participantVersion());
  {
    std::unique_lock<std::mutex> lock(m_mutex);
    while (true) {
      const auto existing = m_records.find(context.transactionId);
      if (existing != m_records.end()) {
        const auto& view = existing->second.view;
        if (existing->second.state == GenericSelectionTxnState::Committed &&
            view.selectionPayloadDigest == context.selectionPayloadDigest &&
            view.selectionIdentity == context.selectionIdentity &&
            view.providerTokenRecordRef == context.providerTokenRecordRef &&
            view.participantId == participant.participantId() &&
            view.participantVersion == participant.participantVersion()) {
          return view;
        }
        if (existing->second.state == GenericSelectionTxnState::Aborted &&
            view.selectionPayloadDigest == context.selectionPayloadDigest &&
            view.selectionIdentity == context.selectionIdentity &&
            view.providerTokenRecordRef == context.providerTokenRecordRef &&
            view.participantId == participant.participantId() &&
            view.participantVersion == participant.participantVersion()) {
          // ABORTED carries no authority. An exact retry may validate again;
          // a later COMMITTED record supersedes it during recovery.
          m_records.erase(existing);
          continue;
        }
        throw std::runtime_error(
            "conflicting generic Selection transaction replay");
      }
      const auto validating =
          m_validatingTransactions.find(context.transactionId);
      if (validating != m_validatingTransactions.end()) {
        if (validating->second != validationFingerprint)
          throw std::runtime_error(
              "conflicting generic Selection transaction is validating");
        if (m_stateChanged.wait_until(lock, context.localDeadline) ==
            std::cv_status::timeout)
          throw std::runtime_error(
              "generic Selection transaction deadline expired while joining");
        continue;
      }
      const auto tokenOwner = m_tokenOwners.find(context.providerTokenRecordRef);
      if (tokenOwner != m_tokenOwners.end() &&
          tokenOwner->second != context.transactionId)
        throw std::runtime_error(
            "generic Selection ProviderToken already committed");
      const auto selectionOwner =
          m_selectionOwners.find(context.selectionIdentity);
      if (selectionOwner != m_selectionOwners.end() &&
          selectionOwner->second != context.transactionId)
        throw std::runtime_error(
            "generic Selection identity already committed");
      if (context.leaseRecordRef) {
        const auto leaseOwner = m_leaseOwners.find(*context.leaseRecordRef);
        if (leaseOwner != m_leaseOwners.end() &&
            leaseOwner->second != context.transactionId)
          throw std::runtime_error(
              "generic Selection lease already committed");
      }
      m_validatingTransactions.emplace(context.transactionId,
                                       validationFingerprint);
      break;
    }
  }
  const auto clearValidating = [this, &context] {
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_validatingTransactions.erase(context.transactionId);
    }
    m_stateChanged.notify_all();
  };
  if (!providerTokenStillLive || (context.leaseRecordRef && !leaseStillLive))
  {
    clearValidating();
    appendAborted(context, participant);
    participant.onAborted(context.transactionId, "AUTHORITY_NOT_LIVE");
    throw std::runtime_error("generic Selection token or lease is not live");
  }

  OpaqueSelectionPrepareResult prepared;
  std::string prepareFailureReason = "PREPARE_REJECTED";
  try {
    ndn::Buffer immutablePayload(selectionPayload.begin(), selectionPayload.end());
    std::packaged_task<OpaqueSelectionPrepareResult()> task(
        [&participant, context, payload = std::move(immutablePayload)] {
          return participant.prepare(
              context, {payload.data(), payload.size()});
        });
    auto future = task.get_future();
    std::thread worker(std::move(task));
    const auto prepareDeadline =
        std::min(context.localDeadline,
                 std::chrono::steady_clock::now() + m_options.maxPrepareTime);
    if (future.wait_until(prepareDeadline) != std::future_status::ready) {
      worker.detach();
      prepareFailureReason = "PREPARE_TIMEOUT";
      throw std::runtime_error(
          "generic Selection participant prepare timed out");
    }
    worker.join();
    prepared = future.get();
  }
  catch (...) {
    clearValidating();
    appendAborted(context, participant);
    participant.onAborted(context.transactionId, prepareFailureReason);
    throw;
  }
  try {
    if (prepared.participantId != participant.participantId() ||
        prepared.participantVersion != participant.participantVersion())
      throw std::runtime_error(
          "generic Selection participant identity mismatch");
    if (prepared.commitBlob.size() > m_options.maxCommitBlobBytes ||
        prepared.acceptancePayload.size() >
            m_options.maxAcceptancePayloadBytes)
      throw std::length_error(
          "generic Selection participant output exceeds bound");
    if (prepared.commitBlobDigest !=
            digest({prepared.commitBlob.data(), prepared.commitBlob.size()}) ||
        prepared.acceptancePayloadDigest !=
            digest({prepared.acceptancePayload.data(),
                    prepared.acceptancePayload.size()}))
      throw std::runtime_error(
          "generic Selection participant output digest mismatch");
    if (std::chrono::steady_clock::now() >= context.localDeadline ||
        (context.expiresAtUnixMs > 0 && unixNowMs() >= context.expiresAtUnixMs))
      throw std::runtime_error(
          "generic Selection transaction deadline expired after prepare");
  }
  catch (...) {
    clearValidating();
    appendAborted(context, participant);
    participant.onAborted(context.transactionId, "PREPARE_OUTPUT_REJECTED");
    throw;
  }

  Record record;
  record.state = GenericSelectionTxnState::Committed;
  record.storageKeyEpoch = m_storageKeyEpoch;
  auto& view = record.view;
  view.transactionId = context.transactionId;
  view.participantId = prepared.participantId;
  view.participantVersion = prepared.participantVersion;
  view.serviceName = context.serviceName;
  view.requestId = context.requestId;
  view.attempt = context.attempt;
  view.selectionIdentity = context.selectionIdentity;
  view.selectionPayloadDigest = context.selectionPayloadDigest;
  view.providerIdentity = context.providerIdentity;
  view.providerBootEpoch = context.providerBootEpoch;
  view.providerTokenRecordRef = context.providerTokenRecordRef;
  view.leaseRecordRef = context.leaseRecordRef;
  view.commitBlob = prepared.commitBlob;
  view.commitBlobDigest = prepared.commitBlobDigest;
  view.acceptancePayload = prepared.acceptancePayload;
  view.acceptancePayloadDigest = prepared.acceptancePayloadDigest;
  view.committedAtUnixMs = unixNowMs();
  view.expiresAtUnixMs = context.expiresAtUnixMs;

  try {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto existing = m_records.find(context.transactionId);
    if (existing != m_records.end()) {
      const auto& committed = existing->second.view;
      if (existing->second.state == GenericSelectionTxnState::Committed &&
          committed.selectionPayloadDigest ==
              context.selectionPayloadDigest &&
          committed.selectionIdentity == context.selectionIdentity &&
          committed.providerTokenRecordRef ==
              context.providerTokenRecordRef &&
          committed.participantId == participant.participantId() &&
          committed.participantVersion ==
              participant.participantVersion()) {
        m_validatingTransactions.erase(context.transactionId);
        m_stateChanged.notify_all();
        return committed;
      }
      throw std::runtime_error(
          "conflicting generic Selection transaction replay");
    }
    const auto tokenOwner =
        m_tokenOwners.find(context.providerTokenRecordRef);
    if (tokenOwner != m_tokenOwners.end())
      throw std::runtime_error(
          "generic Selection ProviderToken already committed");
    const auto selectionOwner =
        m_selectionOwners.find(context.selectionIdentity);
    if (selectionOwner != m_selectionOwners.end())
      throw std::runtime_error(
          "generic Selection identity already committed");
    if (context.leaseRecordRef) {
      const auto leaseOwner = m_leaseOwners.find(*context.leaseRecordRef);
      if (leaseOwner != m_leaseOwners.end())
        throw std::runtime_error(
            "generic Selection lease already committed");
    }
    if (!providerTokenStillLive ||
        (context.leaseRecordRef && !leaseStillLive))
      throw std::runtime_error(
          "generic Selection token or lease changed before commit");
    appendRecord(record);
    m_records.emplace(context.transactionId, record);
    m_tokenOwners.emplace(
        context.providerTokenRecordRef, context.transactionId);
    if (context.leaseRecordRef)
      m_leaseOwners.emplace(*context.leaseRecordRef, context.transactionId);
    m_selectionOwners.emplace(
        context.selectionIdentity, context.transactionId);
    m_validatingTransactions.erase(context.transactionId);
  }
  catch (...) {
    clearValidating();
    participant.onAborted(context.transactionId, "COMMIT_REJECTED");
    throw;
  }
  m_stateChanged.notify_all();
  if (notifyParticipant)
    participant.onCommitted(view);
  return view;
}

std::optional<GenericCommittedSelectionView>
GenericSelectionTxnStore::findCommitted(
    const std::string& transactionId) const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto found = m_records.find(transactionId);
  if (found == m_records.end() ||
      found->second.state != GenericSelectionTxnState::Committed)
    return std::nullopt;
  return found->second.view;
}

void
GenericSelectionTxnStore::replayCommitted(
    const std::map<std::string,
                   std::shared_ptr<OpaqueSelectionParticipant>>& participants,
    const std::string& currentProviderBootEpoch)
{
  std::vector<std::pair<std::shared_ptr<OpaqueSelectionParticipant>,
                        GenericCommittedSelectionView>> callbacks;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& [transactionId, record] : m_records) {
      if (record.state != GenericSelectionTxnState::Committed ||
          record.view.providerBootEpoch != currentProviderBootEpoch)
        continue;
      const auto participant = participants.find(record.view.participantId);
      if (participant == participants.end() ||
          participant->second->participantVersion() !=
              record.view.participantVersion)
        continue;
      callbacks.emplace_back(participant->second, record.view);
    }
  }
  for (const auto& [participant, view] : callbacks)
    participant->onCommitted(view);
}

bool
GenericSelectionTxnStore::tombstone(
    const std::string& transactionId, uint64_t nowUnixMs)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto found = m_records.find(transactionId);
  if (found == m_records.end())
    return false;
  if (found->second.view.expiresAtUnixMs > nowUnixMs)
    return false;
  auto tombstone = found->second;
  tombstone.state = GenericSelectionTxnState::Tombstoned;
  appendRecord(tombstone);
  found->second = tombstone;
  return true;
}

size_t
GenericSelectionTxnStore::size() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_records.size();
}

GenericCommittedSelectionView
GenericSelectionTxnStore::toView(const Record& record) const
{
  return record.view;
}

void
GenericSelectionTxnStore::appendAborted(
    const AuthenticatedSelectionContext& context,
    OpaqueSelectionParticipant& participant)
{
  Record record;
  record.state = GenericSelectionTxnState::Aborted;
  record.storageKeyEpoch = m_storageKeyEpoch;
  auto& view = record.view;
  view.transactionId = context.transactionId;
  view.participantId = participant.participantId();
  view.participantVersion = participant.participantVersion();
  view.serviceName = context.serviceName;
  view.requestId = context.requestId;
  view.attempt = context.attempt;
  view.selectionIdentity = context.selectionIdentity;
  view.selectionPayloadDigest = context.selectionPayloadDigest;
  view.providerIdentity = context.providerIdentity;
  view.providerBootEpoch = context.providerBootEpoch;
  view.providerTokenRecordRef = context.providerTokenRecordRef;
  view.leaseRecordRef = context.leaseRecordRef;
  view.commitBlobDigest = digest(ndn::span<const uint8_t>());
  view.acceptancePayloadDigest = digest(ndn::span<const uint8_t>());
  view.committedAtUnixMs = unixNowMs();
  view.expiresAtUnixMs = context.expiresAtUnixMs;
  std::lock_guard<std::mutex> lock(m_mutex);
  appendRecord(record);
  m_records[context.transactionId] = std::move(record);
}

void
GenericSelectionTxnStore::appendRecord(const Record& record)
{
  const auto& view = record.view;
  std::vector<uint8_t> plaintext;
  putU32(plaintext, static_cast<uint32_t>(record.state));
  putString(plaintext, record.storageKeyEpoch);
  putString(plaintext, view.transactionId);
  putString(plaintext, view.participantId);
  putU32(plaintext, view.participantVersion);
  putString(plaintext, view.serviceName.toUri());
  putString(plaintext, view.requestId.toUri());
  putU64(plaintext, view.attempt);
  putString(plaintext, view.selectionIdentity);
  putString(plaintext, view.selectionPayloadDigest);
  putString(plaintext, view.providerIdentity.toUri());
  putString(plaintext, view.providerBootEpoch);
  putString(plaintext, view.providerTokenRecordRef);
  putString(plaintext, view.leaseRecordRef.value_or(""));
  putBuffer(plaintext, view.commitBlob);
  putString(plaintext, view.commitBlobDigest);
  putBuffer(plaintext, view.acceptancePayload);
  putString(plaintext, view.acceptancePayloadDigest);
  putU64(plaintext, view.committedAtUnixMs);
  putU64(plaintext, view.expiresAtUnixMs);

  const auto aad =
      std::string("NDNSF-GENERIC-SELECTION-WAL-V2|") + m_storageKeyEpoch;
  const auto encrypted = encrypt(m_storageKey, plaintext, aad);
  const auto frameSize =
      encrypted.nonce.size() + encrypted.tag.size() +
      encrypted.ciphertext.size();
  if (frameSize > MAX_FRAME_BYTES)
    throw std::length_error("generic Selection WAL frame exceeds bound");
  std::vector<uint8_t> frame;
  frame.insert(frame.end(), MAGIC.begin(), MAGIC.end());
  putU32(frame, static_cast<uint32_t>(frameSize));
  frame.insert(frame.end(), encrypted.nonce.begin(), encrypted.nonce.end());
  frame.insert(frame.end(), encrypted.tag.begin(), encrypted.tag.end());
  frame.insert(frame.end(), encrypted.ciphertext.begin(),
               encrypted.ciphertext.end());

  const int fd = ::open(
      m_walPath.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
      S_IRUSR | S_IWUSR);
  if (fd < 0)
    throw std::runtime_error(
        "generic Selection WAL open failed: " +
        std::string(std::strerror(errno)));
  try {
    writeAll(fd, frame.data(), frame.size());
    if (::fsync(fd) != 0)
      throw std::runtime_error(
          "generic Selection WAL fsync failed: " +
          std::string(std::strerror(errno)));
    ::close(fd);
    ::chmod(m_walPath.c_str(), S_IRUSR | S_IWUSR);
  }
  catch (...) {
    ::close(fd);
    throw;
  }
}

void
GenericSelectionTxnStore::load()
{
  std::ifstream input(m_walPath, std::ios::binary);
  if (!input)
    return;
  size_t validBytes = 0;
  while (true) {
    std::array<uint8_t, 12> header{};
    input.read(reinterpret_cast<char*>(header.data()), header.size());
    const auto headerRead = static_cast<size_t>(input.gcount());
    if (headerRead == 0)
      break;
    if (headerRead != header.size()) {
      std::filesystem::resize_file(m_walPath, validBytes);
      break;
    }
    if (!std::equal(MAGIC.begin(), MAGIC.end(), header.begin()))
      throw std::runtime_error("generic Selection WAL magic mismatch");
    const uint32_t frameSize =
        (static_cast<uint32_t>(header[8]) << 24) |
        (static_cast<uint32_t>(header[9]) << 16) |
        (static_cast<uint32_t>(header[10]) << 8) |
        static_cast<uint32_t>(header[11]);
    if (frameSize < NONCE_SIZE + TAG_SIZE ||
        frameSize > MAX_FRAME_BYTES)
      throw std::runtime_error("generic Selection WAL frame size invalid");
    std::vector<uint8_t> frame(frameSize);
    input.read(reinterpret_cast<char*>(frame.data()), frame.size());
    if (static_cast<size_t>(input.gcount()) != frame.size()) {
      std::filesystem::resize_file(m_walPath, validBytes);
      break;
    }
    EncryptedFrame encrypted;
    std::copy_n(frame.begin(), NONCE_SIZE, encrypted.nonce.begin());
    std::copy_n(
        frame.begin() + NONCE_SIZE, TAG_SIZE, encrypted.tag.begin());
    encrypted.ciphertext.assign(
        frame.begin() + NONCE_SIZE + TAG_SIZE, frame.end());
    const auto aad =
        std::string("NDNSF-GENERIC-SELECTION-WAL-V2|") +
        m_storageKeyEpoch;
    const auto plaintext = decrypt(m_storageKey, encrypted, aad);
    Decoder decoder(plaintext);
    Record record;
    record.state = static_cast<GenericSelectionTxnState>(
        decoder.readU32());
    record.storageKeyEpoch = decoder.readString();
    if (record.storageKeyEpoch != m_storageKeyEpoch)
      throw std::runtime_error(
          "generic Selection WAL storage key epoch unavailable");
    auto& view = record.view;
    view.transactionId = decoder.readString();
    view.participantId = decoder.readString();
    view.participantVersion = decoder.readU32();
    view.serviceName = ndn::Name(decoder.readString());
    view.requestId = ndn::Name(decoder.readString());
    view.attempt = decoder.readU64();
    view.selectionIdentity = decoder.readString();
    view.selectionPayloadDigest = decoder.readString();
    view.providerIdentity = ndn::Name(decoder.readString());
    view.providerBootEpoch = decoder.readString();
    view.providerTokenRecordRef = decoder.readString();
    const auto lease = decoder.readString();
    if (!lease.empty())
      view.leaseRecordRef = lease;
    view.commitBlob = decoder.readBuffer();
    view.commitBlobDigest = decoder.readString();
    view.acceptancePayload = decoder.readBuffer();
    view.acceptancePayloadDigest = decoder.readString();
    view.committedAtUnixMs = decoder.readU64();
    view.expiresAtUnixMs = decoder.readU64();
    decoder.requireFinished();
    if (view.commitBlobDigest != digest({
            view.commitBlob.data(), view.commitBlob.size()}) ||
        view.acceptancePayloadDigest != digest({
            view.acceptancePayload.data(),
            view.acceptancePayload.size()}))
      throw std::runtime_error(
          "generic Selection WAL decrypted digest mismatch");
    m_records[view.transactionId] = record;
    if (record.state == GenericSelectionTxnState::Committed) {
      m_tokenOwners[view.providerTokenRecordRef] = view.transactionId;
      if (view.leaseRecordRef)
        m_leaseOwners[*view.leaseRecordRef] = view.transactionId;
      m_selectionOwners[view.selectionIdentity] = view.transactionId;
    }
    validBytes += header.size() + frame.size();
  }
}

} // namespace ndn_service_framework
