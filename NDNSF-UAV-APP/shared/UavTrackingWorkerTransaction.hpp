#pragma once

#include <ndn-cxx/encoding/buffer.hpp>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace ndnsf::examples::uav {

/**
 * Immutable per-window result/state pair for the Spec210 compute worker.
 * The transaction manifest is the atomic commit marker for both files.
 */
class UavTrackingWorkerTransaction
{
public:
  explicit UavTrackingWorkerTransaction(std::filesystem::path root);

  std::optional<ndn::Buffer>
  loadCommittedResult(const std::string& windowId, const std::string& inputDigest) const;

  std::optional<std::filesystem::path>
  committedStatePath(const std::string& windowId) const;

  bool commit(const std::string& windowId, const std::string& inputDigest,
              const ndn::Buffer& result, const ndn::Buffer& state,
              const std::function<bool()>& stillAuthorized) const;

private:
  std::filesystem::path m_root;
};

} // namespace ndnsf::examples::uav
