#ifndef NDNSF_DI_NATIVE_CONVERSATION_COORDINATOR_HPP
#define NDNSF_DI_NATIVE_CONVERSATION_COORDINATOR_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceClient.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace ndnsf::di {

struct NativeConversationContinuation
{
  std::string conversationId;
  std::uint64_t parentContextEpoch = 0;
  std::string serviceName;
  std::string planRoleMapDigest;
  std::string parentCheckpointDigest;
  std::string requestContractDigest;
  std::uint64_t retentionDeadlineMs = 0;
};

struct NativeConversationTurn
{
  NativeConversationContinuation parent;
  std::string requestId;
  std::uint64_t attempt = 1;
  std::uint64_t successorContextEpoch = 0;
  std::vector<std::int64_t> acceptedTokenIds;
  std::string prefixDigest;
  bool aborted = false;
};

struct NativeCompletedAttempt
{
  std::string requestId;
  std::uint64_t attempt = 0;
  std::vector<std::int64_t> tokenIds;
  std::string providerStateDigest;
  bool complete = false;
};

struct NativeConversationCheckpoint
{
  std::string conversationId;
  std::uint64_t parentContextEpoch = 0;
  std::uint64_t successorContextEpoch = 0;
  std::string requestId;
  std::string parentCheckpointDigest;
  std::string prefixDigest;
  std::string providerStateDigest;
  std::string checkpointDigest;
};

struct NativeConversationRecord
{
  NativeConversationCheckpoint checkpoint;
  std::string serviceName;
  std::string planRoleMapDigest;
  std::string requestContractDigest;
  std::uint64_t retentionDeadlineMs = 0;
};

class NativeConversationCoordinator
{
public:
  explicit NativeConversationCoordinator(std::filesystem::path journalRoot);

  NativeConversationTurn beginTurn(const NativeConversationContinuation& continuation,
                                   std::string requestId,
                                   std::uint64_t attempt = 1) const;
  void abortTurn(const NativeConversationTurn& turn, const NativeDiError& error);
  NativeConversationCheckpoint prepareCheckpoint(
    const NativeConversationTurn& turn, const NativeCompletedAttempt& completed) const;
  NativeConversationRecord commitTurn(const NativeConversationTurn& turn,
                                      const NativeConversationCheckpoint& checkpoint);
  void restore(const std::filesystem::path& journalRoot);

  std::optional<NativeConversationRecord> find(const std::string& conversationId) const;

private:
  static std::string canonical(const NativeConversationRecord& record);
  static NativeConversationRecord parse(const std::string& value);
  static std::string fileName(const std::string& conversationId);

  mutable std::mutex m_mutex;
  std::filesystem::path m_journalRoot;
  std::map<std::string, NativeConversationRecord> m_records;
};

} // namespace ndnsf::di

#endif // NDNSF_DI_NATIVE_CONVERSATION_COORDINATOR_HPP
