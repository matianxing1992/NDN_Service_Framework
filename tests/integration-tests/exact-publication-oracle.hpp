#ifndef NDNSF_TESTS_EXACT_PUBLICATION_ORACLE_HPP
#define NDNSF_TESTS_EXACT_PUBLICATION_ORACLE_HPP

#include "ndn-service-framework/ServiceProvider.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace ndn_service_framework::test {

/** Test access to the guarded Context path, never the legacy Provider helper. */
struct ExactPublicationOracle
{
  static auto
  publishManifest(ServiceProvider::CollaborationContext& context, const SegmentManifestScope& scope,
                  uint32_t count = 2, ndn::span<const uint8_t> app = {}, uint64_t diskLimit = 0)
  {
    std::vector<ndn::ConstBufferPtr> contents;
    contents.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
      contents.push_back(std::make_shared<const ndn::Buffer>(ndn::Buffer{
        static_cast<uint8_t>(2 * i + 1), static_cast<uint8_t>(2 * i + 2)}));
    }
    return context.publishManifestAuthenticatedExactData("manifest-authority", scope,
      {4096, 65536, count}, contents, app, 60000, nullptr, diskLimit);
  }

  static ServiceProvider::ExactDataFetchOperation
  readManifest(ServiceProvider::CollaborationContext& context, const SegmentManifestScope& scope,
               uint32_t count, std::function<bool()> onValidatedData,
               std::chrono::milliseconds timeout = std::chrono::seconds(2),
               std::function<void(std::exception_ptr)> onFirstRead = {})
  {
    return context.startVerifiedManifestAuthenticatedExactData("manifest-authority", scope,
      {4096, 65536, count}, std::chrono::steady_clock::now() + timeout, {},
      std::move(onFirstRead), std::move(onValidatedData), 16);
  }

  static bool
  replayManifest(ServiceProvider::CollaborationContext& context,
                 const ServiceProvider::CollaborationContext::ExactManifestPublication& original)
  {
    return context.replayManifestAuthenticatedExactData("manifest-authority", original, nullptr);
  }

  static bool
  publish(ServiceProvider::CollaborationContext& context, const ndn::Name& name,
          const ndn::Buffer& payload, CollaborationTransferMetrics* metrics,
          std::vector<std::shared_ptr<const ndn::Data>>& originals, int freshnessMs = 60000,
          std::uint64_t diskLimit = 0)
  {
    return context.publishOwnedExactData(
      "exact-authority", {{name, payload}}, freshnessMs, metrics, originals, 65536, diskLimit);
  }

  static bool
  publishBatch(ServiceProvider::CollaborationContext& context,
               const std::vector<std::pair<ndn::Name, ndn::Buffer>>& objects,
               std::vector<std::shared_ptr<const ndn::Data>>& originals)
  {
    return context.publishOwnedExactData(
      "exact-authority", objects, 60000, nullptr, originals, 65536, 65536);
  }

  static bool
  replay(ServiceProvider::CollaborationContext& context,
         const std::vector<std::shared_ptr<const ndn::Data>>& originals,
         CollaborationTransferMetrics* metrics)
  {
    return context.replayOwnedExactData("exact-authority", originals, metrics);
  }

  static std::shared_ptr<void>
  holdReadLease(ServiceProvider::CollaborationContext& context)
  {
    std::lock_guard lock(*context.m_workAuthorityCommitMutex);
    return context.m_retainedExactPacketSet.lock();
  }
};

} // namespace ndn_service_framework::test

#endif // NDNSF_TESTS_EXACT_PUBLICATION_ORACLE_HPP
