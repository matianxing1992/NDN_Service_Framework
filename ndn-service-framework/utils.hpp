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
#include <common.hpp>

namespace ndn_service_framework
{
    extern std::string requestRegexString;
    extern std::string responseRegexString;
    extern std::string RequestAckRegexString;
    extern std::string serviceCoordinationRegexString;
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
    parseServiceCoordinationName(ndn::Name serviceCoordinationName);

    ndn::Name makeServiceCoordinationName(const ndn::Name &requesterName, const ndn::Name &ServiceProviderName,const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &msgID);
    ndn::Name makeServiceCoordinationNameWithoutPrefix(const ndn::Name &ServiceProviderName,const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &msgID);

    struct RequestNameV2
    {
        ndn::Name requesterName;
        ndn::Name serviceName;
        ndn::Name bloomFilter;
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

    struct ServiceCoordinationNameV2
    {
        ndn::Name requesterName;
        ndn::Name providerName;
        ndn::Name serviceName;
        ndn::Name requestId;
    };

    // V2 wire names use one unified serviceName endpoint path, never ServiceName + FunctionName.
    // Request:
    //   /<requester>/NDNSF/REQUEST/<serviceComponentCount>/<serviceName...>/<bloomFilter>/<requestId>
    // Response:
    //   /<provider>/NDNSF/RESPONSE/<requesterComponentCount>/<requester...>/<serviceComponentCount>/<serviceName...>/<requestId>
    // ACK:
    //   /<provider>/NDNSF/ACK/<requesterComponentCount>/<requester...>/<serviceComponentCount>/<serviceName...>/<requestId>
    // Coordination:
    //   /<requester>/NDNSF/COORDINATION/<providerComponentCount>/<provider...>/<serviceComponentCount>/<serviceName...>/<requestId>
    ndn::Name makeRequestNameV2(const ndn::Name& requesterName,
                                const ndn::Name& serviceName,
                                const ndn::Name& bloomFilter,
                                const ndn::Name& requestId);
    ndn::Name makeRequestNameWithoutPrefixV2(const ndn::Name& serviceName,
                                             const ndn::Name& bloomFilter,
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

    ndn::Name makeServiceCoordinationNameV2(const ndn::Name& requesterName,
                                            const ndn::Name& providerName,
                                            const ndn::Name& serviceName,
                                            const ndn::Name& requestId);
    ndn::Name makeServiceCoordinationNameWithoutPrefixV2(const ndn::Name& providerName,
                                                         const ndn::Name& serviceName,
                                                         const ndn::Name& requestId);
    std::optional<ServiceCoordinationNameV2>
    parseServiceCoordinationNameV2(const ndn::Name& serviceCoordinationName);

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

} // namespace ndn_service_framework
#endif
