#ifndef NDN_SERVICE_FRAMEWORK_CONTROLLER_GENERATION_STORE_HPP
#define NDN_SERVICE_FRAMEWORK_CONTROLLER_GENERATION_STORE_HPP

#include "ControllerVersion.hpp"
#include "PolicyStatus.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ndn_service_framework {

/** Small durable fenced writer store for one Controller identity. */
class ControllerGenerationStore
{
public:
  explicit ControllerGenerationStore(std::filesystem::path statePath);
  ~ControllerGenerationStore();

  ControllerGenerationStore(const ControllerGenerationStore&) = delete;
  ControllerGenerationStore& operator=(const ControllerGenerationStore&) = delete;

  bool acquireWriter(const std::string& owner);
  bool ownsWriter() const;
  uint64_t fencingValue() const;
  void releaseWriter();

  std::optional<ControllerVersion> load() const;
  std::vector<RevocationTarget> loadRevocations() const;
  ControllerVersion startGeneration(
      uint64_t nowMs, const std::vector<RevocationTarget>& revocations = {});
  ControllerVersion advanceEpoch(
      const std::vector<RevocationTarget>& revocations = {});

private:
  struct PersistedState
  {
    ControllerVersion version;
    std::vector<RevocationTarget> revocations;
  };

  void requireWriter() const;
  PersistedState loadState() const;
  void persist(const ControllerVersion& version,
               const std::vector<RevocationTarget>& revocations) const;

  std::filesystem::path m_statePath;
  std::filesystem::path m_lockPath;
  std::string m_owner;
  uint64_t m_fence = 0;
};

} // namespace ndn_service_framework

#endif // NDN_SERVICE_FRAMEWORK_CONTROLLER_GENERATION_STORE_HPP
