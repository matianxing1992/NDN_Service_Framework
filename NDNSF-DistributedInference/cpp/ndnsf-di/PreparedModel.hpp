#ifndef NDNSF_DI_PREPARED_MODEL_HPP
#define NDNSF_DI_PREPARED_MODEL_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/PreparedModelTypes.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ndnsf::di {

struct PreparedModelPackage;

/** Receipt for one caller's preparation operation. */
struct PreparationReceipt
{
  enum class Origin { CacheHit, JoinedInFlight, Fetched, Refreshed };
  Origin origin = Origin::Fetched;
  std::string preparationKeyDigest;
  std::string manifestDigest;
  std::chrono::milliseconds elapsed{0};
};

/** Public immutable model view. Request operations are added by T005. */
class PreparedModel
{
public:
  PreparedModel() = delete;

  const ModelManifest& manifest() const noexcept;
  const PreparationReceipt& receipt() const noexcept;
  ModelCapabilities capabilities() const;

private:
  PreparedModel(std::shared_ptr<const PreparedModelPackage> package,
                PreparationReceipt receipt, std::shared_ptr<void> lease = {});

  std::shared_ptr<const PreparedModelPackage> m_package;
  PreparationReceipt m_receipt;
  std::shared_ptr<void> m_lease;
  friend class ModelPreparationCache;
};

} // namespace ndnsf::di

#endif // NDNSF_DI_PREPARED_MODEL_HPP
