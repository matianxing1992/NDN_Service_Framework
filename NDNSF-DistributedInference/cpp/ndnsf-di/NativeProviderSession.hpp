#ifndef NDNSF_DISTRIBUTED_INFERENCE_NATIVE_PROVIDER_SESSION_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_NATIVE_PROVIDER_SESSION_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlan.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeModelRunner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderRuntime.hpp"

#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace ndnsf::di {

struct KvStateBinding
{
  std::string sessionId;
  std::string stage;
  std::uint64_t contextEpoch = 0;
  std::string modelDigest;
  std::string planDigest;
  std::string providerName;
  std::string providerBootId;
  std::uint64_t securityEpoch = 0;

  void validate() const;
  bool operator==(const KvStateBinding& other) const;
};

class KvStateStore
{
public:
  explicit KvStateStore(std::size_t maxBytes, std::size_t maxEntries = 128);

  void setProviderBootId(std::string providerBootId);
  bool put(KvStateBinding binding, TensorBundle state);
  std::optional<TensorBundle> lookup(const KvStateBinding& binding);
  bool erase(const std::string& sessionId, const std::string& stage);
  void clear();

  std::size_t size() const;
  std::size_t usedBytes() const;

private:
  struct Entry
  {
    KvStateBinding binding;
    TensorBundle state;
    std::uint64_t lastAccess = 0;
  };

  static std::string keyFor(const std::string& sessionId, const std::string& stage);
  void evictUntilFits(std::size_t incomingBytes, const std::string& replacingKey);

private:
  const std::size_t m_maxBytes;
  const std::size_t m_maxEntries;
  mutable std::mutex m_mutex;
  std::map<std::string, Entry> m_entries;
  std::string m_providerBootId;
  std::size_t m_usedBytes = 0;
  std::uint64_t m_accessSequence = 0;
};

class NativeProviderSession
{
public:
  NativeProviderSession(NativeExecutionPlan plan,
                        NativeProviderAssignment assignment,
                        std::shared_ptr<DependencyIo> dependencyIo,
                        std::shared_ptr<NativeModelRunnerFactory> runnerFactory,
                        std::size_t workerCount = std::thread::hardware_concurrency());

  void
  registerRunner(const NativeModelRunnerSpec& spec);

  bool
  hasRunner(const std::string& role) const;

  RoleSpec
  roleSpec(const std::string& role, const std::string& sessionId) const;

  std::future<ProviderRoleResult>
  executeRoleAsync(const std::string& sessionId, const std::string& role);

  std::future<ProviderRoleResult>
  executeRoleAsync(const std::string& sessionId,
                   const std::string& role,
                   std::map<std::string, TensorBundle> initialInputsByScope);

private:
  void
  ensureKnownRole(const std::string& role) const;

private:
  NativeExecutionPlan m_plan;
  NativeProviderAssignment m_assignment;
  std::shared_ptr<DependencyIo> m_dependencyIo;
  std::shared_ptr<NativeModelRunnerFactory> m_runnerFactory;
  NativeProviderRuntime m_runtime;
};

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_NATIVE_PROVIDER_SESSION_HPP
