#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeConversationCoordinator.hpp"

#include <boost/property_tree/json_parser.hpp>
#include <openssl/sha.h>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

namespace ndnsf::di {
namespace {
std::string digest(const std::string& value)
{
  unsigned char hash[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), hash);
  std::ostringstream result;
  result << "sha256:";
  for (const auto byte : hash)
    result << "0123456789abcdef"[byte >> 4] << "0123456789abcdef"[byte & 15];
  return result.str();
}

void requireDigest(const std::string& value, const char* field)
{
  if (value.size() != 71 || value.compare(0, 7, "sha256:") != 0)
    throw std::invalid_argument(std::string(field) + " must be a sha256 digest");
}

std::string quote(const std::string& value)
{
  std::ostringstream out;
  out << '"';
  for (const auto ch : value) {
    if (ch == '"' || ch == '\\') out << '\\';
    out << ch;
  }
  out << '"';
  return out.str();
}
} // namespace

NativeConversationCoordinator::NativeConversationCoordinator(std::filesystem::path journalRoot)
  : m_journalRoot(std::move(journalRoot))
{
  if (m_journalRoot.empty()) throw std::invalid_argument("conversation journal root is empty");
}

NativeConversationTurn NativeConversationCoordinator::beginTurn(
  const NativeConversationContinuation& continuation, std::string requestId,
  std::uint64_t attempt) const
{
  if (continuation.conversationId.empty() || continuation.parentContextEpoch == 0 ||
      continuation.serviceName.empty() || continuation.planRoleMapDigest.empty() ||
      continuation.parentCheckpointDigest.empty() || continuation.requestContractDigest.empty() ||
      continuation.retentionDeadlineMs == 0 || requestId.empty() || attempt == 0) {
    throw std::invalid_argument("conversation continuation is incomplete");
  }
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto found = m_records.find(continuation.conversationId);
  if (found == m_records.end() || found->second.checkpoint.successorContextEpoch !=
      continuation.parentContextEpoch || found->second.checkpoint.checkpointDigest !=
      continuation.parentCheckpointDigest || found->second.serviceName != continuation.serviceName ||
      found->second.planRoleMapDigest != continuation.planRoleMapDigest) {
    throw std::runtime_error("DI_NATIVE_CONVERSATION_PARENT_MISMATCH");
  }
  return {continuation, std::move(requestId), attempt,
          continuation.parentContextEpoch + 1, {}, {}, false};
}

void NativeConversationCoordinator::abortTurn(const NativeConversationTurn& turn,
                                              const NativeDiError&)
{
  if (turn.requestId.empty() || turn.attempt == 0) return;
  std::lock_guard<std::mutex> lock(m_mutex);
  // Only the in-memory turn is fenced; a previous committed record remains
  // the recovery point. No partial checkpoint is written.
}

NativeConversationCheckpoint NativeConversationCoordinator::prepareCheckpoint(
  const NativeConversationTurn& turn, const NativeCompletedAttempt& completed) const
{
  if (turn.aborted || turn.requestId != completed.requestId || turn.attempt != completed.attempt ||
      !completed.complete || completed.tokenIds.empty() || completed.providerStateDigest.empty()) {
    throw std::runtime_error("DI_NATIVE_CONVERSATION_ATTEMPT_INCOMPLETE");
  }
  std::ostringstream canonicalValue;
  canonicalValue << turn.parent.conversationId << '|' << turn.parent.parentContextEpoch << '|'
                 << turn.successorContextEpoch << '|' << turn.requestId << '|'
                 << turn.parent.parentCheckpointDigest << '|' << completed.providerStateDigest << '|';
  for (const auto token : completed.tokenIds) canonicalValue << token << ',';
  const auto prefix = digest(canonicalValue.str());
  return {turn.parent.conversationId, turn.parent.parentContextEpoch,
          turn.successorContextEpoch, turn.requestId, turn.parent.parentCheckpointDigest,
          prefix, completed.providerStateDigest, digest("checkpoint|" + prefix)};
}

NativeConversationRecord NativeConversationCoordinator::commitTurn(
  const NativeConversationTurn& turn, const NativeConversationCheckpoint& checkpoint)
{
  if (turn.aborted || checkpoint.conversationId != turn.parent.conversationId ||
      checkpoint.parentContextEpoch != turn.parent.parentContextEpoch ||
      checkpoint.successorContextEpoch != turn.successorContextEpoch ||
      checkpoint.requestId != turn.requestId || checkpoint.parentCheckpointDigest !=
      turn.parent.parentCheckpointDigest || checkpoint.checkpointDigest.empty()) {
    throw std::invalid_argument("conversation checkpoint is not bound to turn");
  }
  NativeConversationRecord record{checkpoint, turn.parent.serviceName,
                                  turn.parent.planRoleMapDigest,
                                  turn.parent.requestContractDigest,
                                  turn.parent.retentionDeadlineMs};
  const auto bytes = canonical(record);
  const auto path = m_journalRoot / fileName(record.checkpoint.conversationId);
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto current = m_records.find(record.checkpoint.conversationId);
  if (current != m_records.end() && current->second.checkpoint.checkpointDigest !=
      record.checkpoint.parentCheckpointDigest) {
    throw std::runtime_error("DI_NATIVE_CONVERSATION_STATE_CHANGED");
  }
  std::filesystem::create_directories(m_journalRoot);
  const auto temporary = path.string() + ".tmp-" + std::to_string(::getpid());
  { std::ofstream output(temporary, std::ios::binary | std::ios::trunc); output << bytes; }
  std::filesystem::rename(temporary, path);
  m_records[record.checkpoint.conversationId] = record;
  return record;
}

void NativeConversationCoordinator::restore(const std::filesystem::path& journalRoot)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  m_journalRoot = journalRoot;
  m_records.clear();
  if (!std::filesystem::exists(m_journalRoot)) return;
  for (const auto& entry : std::filesystem::directory_iterator(m_journalRoot)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
    std::ifstream input(entry.path());
    std::stringstream bytes; bytes << input.rdbuf();
    auto record = parse(bytes.str());
    m_records.emplace(record.checkpoint.conversationId, std::move(record));
  }
}

std::optional<NativeConversationRecord> NativeConversationCoordinator::find(
  const std::string& conversationId) const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto it = m_records.find(conversationId);
  return it == m_records.end() ? std::nullopt : std::optional<NativeConversationRecord>(it->second);
}

std::string NativeConversationCoordinator::canonical(const NativeConversationRecord& record)
{
  std::ostringstream out;
  out << "{\"conversationId\":" << quote(record.checkpoint.conversationId)
      << ",\"parentContextEpoch\":" << record.checkpoint.parentContextEpoch
      << ",\"successorContextEpoch\":" << record.checkpoint.successorContextEpoch
      << ",\"requestId\":" << quote(record.checkpoint.requestId)
      << ",\"parentCheckpointDigest\":" << quote(record.checkpoint.parentCheckpointDigest)
      << ",\"prefixDigest\":" << quote(record.checkpoint.prefixDigest)
      << ",\"providerStateDigest\":" << quote(record.checkpoint.providerStateDigest)
      << ",\"checkpointDigest\":" << quote(record.checkpoint.checkpointDigest)
      << ",\"serviceName\":" << quote(record.serviceName)
      << ",\"planRoleMapDigest\":" << quote(record.planRoleMapDigest)
      << ",\"requestContractDigest\":" << quote(record.requestContractDigest)
      << ",\"retentionDeadlineMs\":" << record.retentionDeadlineMs << "}";
  return out.str();
}

NativeConversationRecord NativeConversationCoordinator::parse(const std::string& value)
{
  boost::property_tree::ptree tree;
  std::istringstream input(value);
  try { boost::property_tree::read_json(input, tree); }
  catch (...) { throw std::runtime_error("DI_NATIVE_CONVERSATION_PARTIAL_RECORD"); }
  NativeConversationRecord record;
  try {
    record.checkpoint.conversationId = tree.get<std::string>("conversationId");
    record.checkpoint.parentContextEpoch = tree.get<std::uint64_t>("parentContextEpoch");
    record.checkpoint.successorContextEpoch = tree.get<std::uint64_t>("successorContextEpoch");
    record.checkpoint.requestId = tree.get<std::string>("requestId");
    record.checkpoint.parentCheckpointDigest = tree.get<std::string>("parentCheckpointDigest");
    record.checkpoint.prefixDigest = tree.get<std::string>("prefixDigest");
    record.checkpoint.providerStateDigest = tree.get<std::string>("providerStateDigest");
    record.checkpoint.checkpointDigest = tree.get<std::string>("checkpointDigest");
    record.serviceName = tree.get<std::string>("serviceName");
    record.planRoleMapDigest = tree.get<std::string>("planRoleMapDigest");
    record.requestContractDigest = tree.get<std::string>("requestContractDigest");
    record.retentionDeadlineMs = tree.get<std::uint64_t>("retentionDeadlineMs");
  }
  catch (...) { throw std::runtime_error("DI_NATIVE_CONVERSATION_RECORD_FIELDS"); }
  requireDigest(record.checkpoint.parentCheckpointDigest, "parentCheckpointDigest");
  requireDigest(record.checkpoint.prefixDigest, "prefixDigest");
  requireDigest(record.checkpoint.providerStateDigest, "providerStateDigest");
  requireDigest(record.checkpoint.checkpointDigest, "checkpointDigest");
  if (record.checkpoint.conversationId.empty() || record.checkpoint.requestId.empty() ||
      record.checkpoint.parentContextEpoch == 0 || record.checkpoint.successorContextEpoch == 0 ||
      record.serviceName.empty() || record.planRoleMapDigest.empty() ||
      record.requestContractDigest.empty() || record.retentionDeadlineMs == 0) {
    throw std::runtime_error("DI_NATIVE_CONVERSATION_RECORD_FIELDS");
  }
  return record;
}

std::string NativeConversationCoordinator::fileName(const std::string& conversationId)
{
  return digest(conversationId).substr(7) + ".json";
}

} // namespace ndnsf::di
