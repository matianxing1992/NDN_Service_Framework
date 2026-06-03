#ifndef NDN_SERVICE_FRAMEWORK_UTILS_HPP
#define NDN_SERVICE_FRAMEWORK_UTILS_HPP

#include <string>
#include <regex>
#include <tuple>
#include <optional>
#include <set>
#include <cstddef>

#include <boost/format.hpp>
#include <ndn-cxx/encoding/buffer-stream.hpp>
#include <ndn-cxx/name.hpp>
#include <ndn-cxx/security/certificate.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <common.hpp>
#include <NDNSFMessages.hpp>

namespace ndn_service_framework
{
    extern std::string requestRegexString;
    extern std::string responseRegexString;
    extern std::string RequestAckRegexString;
    extern std::string serviceSelectionRegexString;
    extern std::string permissionTokenRegexString;

    std::optional<std::tuple<ndn::Name, ndn::Name, ndn::Name, ndn::Name, ndn::Name>>
    parseRequestName(ndn::Name requestName);

    ndn::Name makeRequestName(const ndn::Name &requesterName,const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &bloomFilter, const ndn::Name &RequestID);
    ndn::Name makeRequestPrefixName(const ndn::Name &requesterName, const ndn::Name &ServiceName, const ndn::Name &FunctionName);
    ndn::Name makeRequestNameWithoutPrefix(const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &bloomFilter, const ndn::Name &RequestID);

    std::optional<std::tuple<ndn::Name, ndn::Name, ndn::Name, ndn::Name, ndn::Name>>
    parseResponseName(ndn::Name responseName);

    ndn::Name makeResponseName(const ndn::Name &ServiceProviderName,const ndn::Name &requesterName, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &RequestID);
    ndn::Name makeResponseNameWithoutPrefix(const ndn::Name &requesterName, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &RequestID);

    std::optional<std::tuple<ndn::Name, ndn::Name, ndn::Name, ndn::Name, ndn::Name>>
    parseRequestAckName(ndn::Name RequestAckName);

    ndn::Name makeRequestAckName(const ndn::Name &ServiceProviderName,const ndn::Name &requesterName, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &ChallengeID);
    ndn::Name makeRequestAckNameWithoutPrefix(const ndn::Name &requesterName, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &ChallengeID);

    std::optional<std::tuple<ndn::Name, ndn::Name, ndn::Name, ndn::Name, ndn::Name>>
    parseServiceSelectionName(ndn::Name serviceSelectionName);

    ndn::Name makeServiceSelectionName(const ndn::Name &requesterName, const ndn::Name &ServiceProviderName,const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &msgID);
    ndn::Name makeServiceSelectionNameWithoutPrefix(const ndn::Name &ServiceProviderName,const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &msgID);

    struct RequestNameV2
    {
        ndn::Name requesterName;
        ndn::Name serviceName;
        ndn::Name requestId;
    };

    struct ResponseNameV2
    {
        ndn::Name providerName;
        ndn::Name requesterName;
        ndn::Name serviceName;
        ndn::Name requestId;
    };

    struct RequestAckNameV2
    {
        ndn::Name providerName;
        ndn::Name requesterName;
        ndn::Name serviceName;
        ndn::Name requestId;
    };

    struct ServiceSelectionNameV2
    {
        ndn::Name requesterName;
        ndn::Name providerName;
        ndn::Name serviceName;
        ndn::Name requestId;
    };

    struct SelectionStatusQueryName
    {
        ndn::Name providerName;
        ndn::Name serviceName;
        std::string selectionDigest;
    };

    struct CollaborationDataName
    {
        ndn::Name producerName;
        ndn::Name requesterName;
        ndn::Name requestId;
        std::string keyScope;
        ndn::Name topic;
        uint64_t sequence = 0;
    };

    struct LargeDataReference
    {
        ndn::Name dataName;
        std::string objectType;
        std::string objectId;
        size_t plaintextSize = 0;
        bool encrypted = true;
        std::string digest;
    };

    // V2 wire names use one unified serviceName endpoint path, never ServiceName + FunctionName.
    // NAC-ABE attributes follow message direction: REQUEST/SELECTION use
    // /SERVICE/<serviceName>; RESPONSE/ACK use /PERMISSION/<serviceName>.
    // Request:
    //   /<requester>/NDNSF/REQUEST/<serviceName...>/<requestId>
    // Response:
    //   /<provider>/NDNSF/RESPONSE/<requester-uri-component>/<serviceName...>/<requestId>
    // ACK:
    //   /<provider>/NDNSF/ACK/<requester-uri-component>/<serviceName...>/<requestId>
    // Selection:
    //   /<requester>/NDNSF/SELECTION/<provider-uri-component>/<serviceName...>/<requestId>
    ndn::Name makeRequestNameV2(const ndn::Name& requesterName,
                                const ndn::Name& serviceName,
                                const ndn::Name& requestId);
    ndn::Name makeRequestNameWithoutPrefixV2(const ndn::Name& serviceName,
                                             const ndn::Name& requestId);
    std::optional<RequestNameV2> parseRequestNameV2(const ndn::Name& requestName);

    ndn::Name makeResponseNameV2(const ndn::Name& providerName,
                                 const ndn::Name& requesterName,
                                 const ndn::Name& serviceName,
                                 const ndn::Name& requestId);
    ndn::Name makeResponseNameWithoutPrefixV2(const ndn::Name& requesterName,
                                              const ndn::Name& serviceName,
                                              const ndn::Name& requestId);
    std::optional<ResponseNameV2> parseResponseNameV2(const ndn::Name& responseName);

    ndn::Name makeRequestAckNameV2(const ndn::Name& providerName,
                                   const ndn::Name& requesterName,
                                   const ndn::Name& serviceName,
                                   const ndn::Name& requestId);
    ndn::Name makeRequestAckNameWithoutPrefixV2(const ndn::Name& requesterName,
                                                const ndn::Name& serviceName,
                                                const ndn::Name& requestId);
    std::optional<RequestAckNameV2> parseRequestAckNameV2(const ndn::Name& requestAckName);

    ndn::Name makeServiceSelectionNameV2(const ndn::Name& requesterName,
                                            const ndn::Name& providerName,
                                            const ndn::Name& serviceName,
                                            const ndn::Name& requestId);
    ndn::Name makeServiceSelectionNameWithoutPrefixV2(const ndn::Name& providerName,
                                                         const ndn::Name& serviceName,
                                                         const ndn::Name& requestId);
    std::optional<ServiceSelectionNameV2>
    parseServiceSelectionNameV2(const ndn::Name& serviceSelectionName);

    ndn::Name makeSelectionStatusQueryName(const ndn::Name& providerName,
                                           const ndn::Name& serviceName,
                                           const std::string& selectionDigest);
    std::optional<SelectionStatusQueryName>
    parseSelectionStatusQueryName(const ndn::Name& statusQueryName);
    std::string computeSelectionDigest(const ServiceSelectionMessage& message);

    ndn::Name makeCollaborationDataName(const ndn::Name& producerName,
                                        const ndn::Name& requesterName,
                                        const ndn::Name& requestId,
                                        const std::string& keyScope,
                                        const ndn::Name& topic,
                                        uint64_t sequence);
    std::optional<CollaborationDataName>
    parseCollaborationDataName(const ndn::Name& collaborationDataName);

    ndn::Buffer encodeLargeDataReferencePayload(const LargeDataReference& reference);
    std::optional<LargeDataReference> parseLargeDataReferencePayload(const ndn::Buffer& payload);
    bool isLargeDataReferencePayload(const ndn::Buffer& payload);

    // /muas/drone1/NDNSF/TOKEN/ObjectDetection/YOLOv8/0
    // <provider> <service> <function> <seqNum>
    std::optional<std::tuple<ndn::Name, ndn::Name, ndn::Name, ndn::Name>>
    parsePermissionTokenName(ndn::Name permissionTokenName);

    ndn::Name makePermissionTokenNameWithoutPrefix(const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &seqNum);

    std::shared_ptr<ndn::Buffer> CombineSegmentsIntoBuffer(ndn::nacabe::SPtrVector<ndn::Data> segments);

    std::string NameToRegexString(ndn::Name& name);

    std::string RandomString(const int len);

    std::optional<std::vector<std::string>> GetAttributesByName(const ndn::Name& name);

    // ndn::Block to ndn::span<const uint8_t>
    ndn::span<const uint8_t> blockToSpan(const ndn::Block& block);

    // concatnate std::vector<std::string> using " and "
    std::string ConcatenateString(const std::vector<std::string>& strs);

    // Function to merge content values of multiple ndn::Data objects into a uint8_t array
    std::vector<uint8_t> mergeDataContents(const std::vector<std::shared_ptr<ndn::Data>>& dataPackets);

    // PermissionResponse-only encryption helpers. These do not use NAC-ABE.
    // Intended algorithm: RSA-wrapped AES-CBC.
    EncryptedPermissionResponse
    encryptPermissionResponseForCertificate(const PermissionResponse& response,
                                            const ndn::security::Certificate& recipientCert);

    PermissionResponse
    decryptPermissionResponseWithKeyChain(const EncryptedPermissionResponse& encryptedResponse,
                                          const ndn::security::KeyChain& keyChain);

} // namespace ndn_service_framework
#endif
