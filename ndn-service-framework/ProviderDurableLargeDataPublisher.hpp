#pragma once

#include "EncryptedLargeDataRangeStore.hpp"
#include "PublishedContentName.hpp"

#include <ndn-cxx/name.hpp>
#include <ndn-cxx/util/time.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ndn_service_framework {

class ServiceProvider;

struct ProviderDurableLargeDataPublishRequestV1
{
    ndn::Name serviceName;
    ndn::Name requestId;
    std::vector<std::uint8_t> protectedPayload;
    std::string objectLabel;
    ndn::time::milliseconds freshness;
    PublishedContentName contentName;
    ndn::Name manifestSuffix;
    EncryptedLargeDataCommitOptions commitOptions;
};

struct ProviderDurableLargeDataPublishResultV1
{
    bool success = false;
    ndn::Name encryptedDataName;
    ndn::Name manifestName;
    ndn::Name dataPrefix;
    std::string objectId;
    std::uint64_t plaintextSize = 0;
    std::uint64_t envelopeSize = 0;
    std::string contentDigest;
    std::string manifestDigest;
    std::string authorizationScope;
    std::string protectionEpoch;
    std::string publicationIdentity;
    std::string keyReferenceId;
    std::string keyReferenceVersion;
    std::string ciphertextManifestDigest;
    std::string servingLocator;
    bool encrypted = true;
    bool fileBacked = true;
    std::vector<ndn::Name> auxiliaryPacketNames;
    std::shared_ptr<const EncryptedLargeDataRangeSource> durableStorageSource;
    std::string errorMessage;
};

/** Worker-only durable publication using an existing Provider identity/runtime. */
class ProviderDurableLargeDataPublisher
{
public:
    static ProviderDurableLargeDataPublishResultV1 publish(
        std::shared_ptr<ServiceProvider> producer,
        ProviderDurableLargeDataPublishRequestV1 request,
        std::shared_ptr<EncryptedLargeDataRangeStore> store,
        DurableStorageGrantResolver resolveGrant,
        DurableStorageGrantChallengeObserverV1 onGrantRequired,
        const std::function<void()>& requireActive,
        std::function<void()> onDurablePacketSetBoundary = {});
};

} // namespace ndn_service_framework
