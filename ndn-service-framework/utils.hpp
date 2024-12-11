#ifndef NDN_SERVICE_FRAMEWORK_UTILS_HPP
#define NDN_SERVICE_FRAMEWORK_UTILS_HPP

#include <string>
#include <regex>
#include <tuple>
#include <optional>
#include <set>

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
