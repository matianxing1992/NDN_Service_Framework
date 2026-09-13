#include "NDNSF-DistributedInference/cpp/ndnsf-di/PreparedModel.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/PreparedModelPackage.hpp"

#include <stdexcept>

namespace ndnsf::di {

PreparedModel::PreparedModel(std::shared_ptr<const PreparedModelPackage> package,
                             PreparationReceipt receipt, std::shared_ptr<void> lease)
  : m_package(std::move(package)), m_receipt(std::move(receipt)), m_lease(std::move(lease))
{
  if (!m_package)
    throw std::invalid_argument("prepared model requires a verified package");
}

const ModelManifest& PreparedModel::manifest() const noexcept
{
  static const ModelManifest empty;
  return m_package ? m_package->manifest : empty;
}

const PreparationReceipt& PreparedModel::receipt() const noexcept
{
  return m_receipt;
}

ModelCapabilities PreparedModel::capabilities() const
{
  if (!m_package)
    throw std::runtime_error("prepared model is empty");
  return m_package->capabilities;
}

} // namespace ndnsf::di
