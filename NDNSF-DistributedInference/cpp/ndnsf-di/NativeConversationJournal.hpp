#pragma once

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeConversationWire.hpp"
#include <filesystem>
#include <memory>
#include <map>

namespace ndnsf::di {

struct NativeConversationJournalConfig
{
  std::filesystem::path stateRoot;
  std::string identity;
  std::vector<NativeConversationJournalKey> keys;
  std::size_t quotaBytes = 64 * 1024 * 1024;
  bool testOnlyAllowEphemeralRoot = false;
};

// Owns the old RuntimeJournal journal.jsonl transaction format and writer
// lease. The conversation coordinator still owns authenticated parent CAS.
class NativeConversationJournal
{
public:
  explicit NativeConversationJournal(NativeConversationJournalConfig config);
  ~NativeConversationJournal();
  NativeConversationJournal(const NativeConversationJournal&) = delete;
  NativeConversationJournal& operator=(const NativeConversationJournal&) = delete;

  std::vector<std::vector<std::uint8_t>> authenticationKeys() const;
  std::filesystem::path stateRoot() const;
  std::vector<NativeJson> readConversations(std::uint64_t nowMs) const;
  void appendConversation(const std::string& checkpointWire,
                          const NativeJson& transcript, std::uint64_t nowMs,
                          std::optional<std::size_t> nativeInitialPromptTokenCount = std::nullopt,
                          const std::map<std::string, std::string>& providersByRole = {});

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace ndnsf::di
