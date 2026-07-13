#ifndef NDNSF_DISTRIBUTED_INFERENCE_QWEN_GENERATION_SESSION_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_QWEN_GENERATION_SESSION_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace ndnsf::di {

struct QwenRoleBinding
{
  std::string role;
  std::string provider;
  std::string providerBootId;
};

struct QwenGenerationSessionSpec
{
  std::string schema = "ndnsf-di-qwen-generation-session-v1";
  std::string candidateId;
  std::string planDigest;
  std::string modelDigest;
  std::string artifactDigest;
  std::string logicalSessionId;
  std::string requestId;
  std::string serviceName;
  std::uint64_t attemptEpoch = 0;
  std::uint32_t tokenEpoch = 0;
  std::uint32_t inputTokenCount = 0;
  std::uint32_t maxGeneratedTokens = 0;
  std::uint64_t deadlineEpochMs = 0;
  std::string contextReference;
  std::string feedbackTopic;
  std::vector<QwenRoleBinding> roles;

  void validate() const;
};

std::string
qwenGenerationSessionSpecToJson(const QwenGenerationSessionSpec& spec);

QwenGenerationSessionSpec
qwenGenerationSessionSpecFromJson(const std::string& json);

enum class QwenGenerationState
{
  Created,
  Selecting,
  Active,
  Rebuilding,
  Completed,
  Terminal,
  Cancelled,
};

enum class QwenGenerationTerminal
{
  None,
  ProviderLost,
  DependencyMissing,
  DependencyHashMismatch,
  CacheMissFullContextRequired,
  NoCompatibleReplacement,
  RequestDeadline,
  AttemptCancelled,
};

const char* toString(QwenGenerationState state) noexcept;
const char* toString(QwenGenerationTerminal reason) noexcept;

class QwenGenerationSessionStateMachine
{
public:
  explicit QwenGenerationSessionStateMachine(QwenGenerationSessionSpec spec);

  QwenGenerationState state() const noexcept;
  QwenGenerationTerminal terminalReason() const noexcept;
  std::uint64_t attemptEpoch() const noexcept;
  std::uint32_t generatedTokenCount() const noexcept;
  bool isTerminal() const noexcept;

  void beginSelection();
  void activate();
  std::uint32_t completeTokenEpoch();
  void beginReplacement();
  void complete();
  void terminate(QwenGenerationTerminal reason);
  void cancel();

private:
  void requireState(QwenGenerationState expected, const char* operation) const;

private:
  QwenGenerationSessionSpec m_spec;
  QwenGenerationState m_state = QwenGenerationState::Created;
  QwenGenerationTerminal m_terminalReason = QwenGenerationTerminal::None;
  std::uint64_t m_attemptEpoch = 0;
  std::uint32_t m_generatedTokenCount = 0;
};

enum class QwenResourceKind : std::size_t
{
  Generation = 0,
  Request,
  Wait,
  Callback,
  TokenPair,
  Assignment,
  Tensor,
  Metrics,
  Count,
};

const char* toString(QwenResourceKind kind) noexcept;

struct QwenGenerationResourceLimits
{
  std::size_t generationCapacity = 4;
  std::size_t requestCapacity = 60;
  std::size_t waitCapacity = 1024;
  std::size_t callbackCapacity = 256;
  std::size_t tokenPairCapacity = 64;
  std::size_t assignmentCapacity = 64;
  std::size_t tensorCapacity = 256;
  std::size_t metricsCapacity = 1024;

  void validate() const;
  std::size_t capacity(QwenResourceKind kind) const;
};

struct QwenResourceAcquireResult
{
  bool accepted = false;
  std::string reason;
  std::size_t occupancy = 0;
  std::size_t capacity = 0;
};

struct QwenResourceSnapshot
{
  QwenResourceKind kind = QwenResourceKind::Generation;
  std::string name;
  std::size_t occupancy = 0;
  std::size_t capacity = 0;
  std::size_t rejected = 0;
};

class QwenGenerationResourceLedger
{
public:
  explicit QwenGenerationResourceLedger(QwenGenerationResourceLimits limits = {});

  QwenResourceAcquireResult tryAcquire(QwenResourceKind kind);
  void release(QwenResourceKind kind);
  QwenResourceSnapshot snapshot(QwenResourceKind kind) const;

private:
  static std::size_t index(QwenResourceKind kind);

private:
  QwenGenerationResourceLimits m_limits;
  mutable std::mutex m_mutex;
  std::array<std::size_t, static_cast<std::size_t>(QwenResourceKind::Count)> m_occupancy{};
  std::array<std::size_t, static_cast<std::size_t>(QwenResourceKind::Count)> m_rejected{};
};

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_QWEN_GENERATION_SESSION_HPP
