#ifndef NDN_SERVICE_FRAMEWORK_UTILS_HPP
#define NDN_SERVICE_FRAMEWORK_UTILS_HPP
#include <string>
#include <regex>
#include <tuple>

#include <boost/format.hpp>
#include <ndn-cxx/encoding/buffer-stream.hpp>
#include <ndn-cxx/name.hpp>
#include <common.hpp>

namespace ndn_service_framework
{

    // ndn::Name UserIdentity, ServiceProviderIdentity, ServiceName, FunctionName, RequestId
    std::tuple<ndn::Name, ndn::Name, ndn::Name, ndn::Name, ndn::Name>
    parseRequestName(ndn::Name requestName);

    ndn::Name makeRequestName(const ndn::Name &requesterName, const ndn::Name &ServiceProviderName,const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &RequestID);
    ndn::Name makeRequestPrefixName(const ndn::Name &requesterName, const ndn::Name &ServiceProviderName,const ndn::Name &ServiceName, const ndn::Name &FunctionName);

    ndn::Name makeRequestNameWithoutPrefix(const ndn::Name &ServiceProviderName,const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &RequestID);

    // ndn::Name ServiceProviderIdentity, RequesterIdentity, ServiceName, FunctionName, RequestId
    std::tuple<ndn::Name, ndn::Name, ndn::Name, ndn::Name, ndn::Name>
    parseResponseName(ndn::Name responseName);

    ndn::Name makeResponseName(const ndn::Name &ServiceProviderName,const ndn::Name &requesterName, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &RequestID);
    ndn::Name makeResponseNameWithoutPrefix(const ndn::Name &requesterName, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &RequestID);

        // ndn::Name ServiceProviderName, RequesterName, ServiceName, FunctionName, ChallengeId
    std::tuple<ndn::Name, ndn::Name, ndn::Name, ndn::Name, ndn::Name>
    parsePermissionChallengeName(ndn::Name permissionChallengeName);

    ndn::Name makePermissionChallengeName(const ndn::Name &ServiceProviderName,const ndn::Name &requesterName, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &ChallengeID);
    ndn::Name makePermissionChallengeNameWithoutPrefix(const ndn::Name &requesterName, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &ChallengeID);



            // ndn::Name RequesterName, ServiceProviderName,  ServiceName, FunctionName, ChallengeId
    std::tuple<ndn::Name, ndn::Name, ndn::Name, ndn::Name, ndn::Name>
    parsePermissionResponseName(ndn::Name permissionChallengeName);

    ndn::Name makePermissionResponseName(const ndn::Name &requesterName, const ndn::Name &ServiceProviderName,const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &ChallengeID);
    ndn::Name makePermissionResponseNameWithoutPrefix(const ndn::Name &ServiceProviderName,const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &ChallengeID);

    // Combine segments into a buffer
    std::shared_ptr<ndn::Buffer> CombineSegmentsIntoBuffer(ndn::nacabe::SPtrVector<ndn::Data> segments);

    // /name1/name2 -> <name1><name2>
    std::string NameToRegexString(ndn::Name& name);

    std::string RandomString(const int len);
}
#endif

