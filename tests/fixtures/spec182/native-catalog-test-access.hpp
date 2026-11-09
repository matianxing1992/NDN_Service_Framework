#ifndef NDNSF_TESTS_SPEC182_NATIVE_CATALOG_TEST_ACCESS_HPP
#define NDNSF_TESTS_SPEC182_NATIVE_CATALOG_TEST_ACCESS_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalPreparationCatalog.hpp"

#include <utility>

namespace ndnsf::di {

class NativeCanonicalPublisherTestAccess
{
public:
  using Transport = NativeCanonicalArtifactPublisher::Transport;

  static NativeCanonicalArtifactPublisher
  create(Transport transport, NativeCanonicalPublicationOptions options,
         NativeCanonicalArtifactPublisher::SourcePort source)
  {
    return NativeCanonicalArtifactPublisher(std::move(transport), "/service",
                                            std::move(options), std::move(source));
  }
};

class NativeCanonicalCatalogTestAccess
{
public:
  static std::shared_ptr<NativeRequestPreparation>
  create(const NativeCanonicalPreparationCatalog& catalog,
         NativeCanonicalPublisherTestAccess::Transport transport)
  {
    return catalog.makePreparation([transport](auto options, auto source) {
      return NativeCanonicalPublisherTestAccess::create(
        transport, std::move(options), std::move(source));
    });
  }

  static void
  releaseSourceBytes(const NativeCanonicalPreparationCatalog& catalog)
  {
    catalog.releaseTransientSourceBytes();
  }
};

} // namespace ndnsf::di

#endif // NDNSF_TESTS_SPEC182_NATIVE_CATALOG_TEST_ACCESS_HPP
