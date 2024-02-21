#include "utils.hpp"



namespace ndn_service_framework
{

    NDN_LOG_INIT(ndn_service_framework.utils);

    // ndn::Name UserIdentity, ServiceProviderIdentity, ServiceName, FunctionName, RequestId
    std::tuple<ndn::Name, ndn::Name, ndn::Name, ndn::Name, ndn::Name>
    parseRequestName(ndn::Name requestName)
    {
        std::shared_ptr<ndn::Regex> requestMatch = std::make_shared<ndn::Regex>("^(<>+)<NDNSF><REQUEST>(<>*)(<>)(<>)(<>)$");
        bool res = requestMatch->match(requestName);
        if (res)
        {
            // ndn::Name identity = requestMatch->expand("\\1");
            return std::make_tuple<ndn::Name, ndn::Name, ndn::Name, ndn::Name, ndn::Name>(
                requestMatch->expand("\\1"),
                requestMatch->expand("\\2"),
                requestMatch->expand("\\3"),
                requestMatch->expand("\\4"),
                requestMatch->expand("\\5"));
        }
        else
        {
            return std::make_tuple<ndn::Name, ndn::Name, ndn::Name, ndn::Name, ndn::Name>(
                ndn::Name(),
                ndn::Name(),
                ndn::Name(),
                ndn::Name(),
                ndn::Name());
        }
    }

    ndn::Name makeRequestName(const ndn::Name &requesterName, const ndn::Name &ServiceProviderName,const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &RequestID)
    {
        ndn::Name requestName;
        requestName.append(requesterName).append(ndn::Name("/NDNSF/REQUEST")).append(ServiceProviderName)
            .append(ServiceName).append(FunctionName).append(RequestID);
        return requestName;
    }
    ndn::Name makeRequestPrefixName(const ndn::Name &requesterName, const ndn::Name &ServiceProviderName,const ndn::Name &ServiceName, const ndn::Name &FunctionName)
    {
        ndn::Name requestPrefixName;
        requestPrefixName.append(requesterName).append(ndn::Name("/NDNSF/REQUEST")).append(ServiceProviderName)
            .append(ServiceName).append(FunctionName);
        return requestPrefixName;
    }
    ndn::Name makeRequestNameWithoutPrefix(const ndn::Name &ServiceProviderName,const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &RequestID)
    {
        ndn::Name requestName;
        requestName.append(ndn::Name("/NDNSF/REQUEST")).append(ServiceProviderName)
            .append(ServiceName).append(FunctionName).append(RequestID);
        return requestName;
    }

    // ndn::Name ServiceProviderIdentity, RequesterIdentity, ServiceName, FunctionName, RequestId
    std::tuple<ndn::Name, ndn::Name, ndn::Name, ndn::Name, ndn::Name>
    parseResponseName(ndn::Name responseName)
    {
        std::shared_ptr<ndn::Regex> responseMatch = std::make_shared<ndn::Regex>("^(<>+)<NDNSF><RESPONSE>(<>+)(<>)(<>)(<>)$");
        bool res = responseMatch->match(responseName);
        if (res)
        {
            // ndn::Name identity = requestMatch->expand("\\1");
            return std::make_tuple<ndn::Name, ndn::Name, ndn::Name, ndn::Name, ndn::Name>(
                responseMatch->expand("\\1"),
                responseMatch->expand("\\2"),
                responseMatch->expand("\\3"),
                responseMatch->expand("\\4"),
                responseMatch->expand("\\5"));
        }
        else
        {
            return std::make_tuple<ndn::Name, ndn::Name, ndn::Name, ndn::Name, ndn::Name>(
                ndn::Name(),
                ndn::Name(),
                ndn::Name(),
                ndn::Name(),
                ndn::Name());
        }
    }

    ndn::Name makeResponseName(const ndn::Name &ServiceProviderName,const ndn::Name &requesterName, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &RequestID)
    {
        ndn::Name ResponseName;
        ResponseName.append(ServiceProviderName).append(ndn::Name("/NDNSF/RESPONSE")).append(requesterName)
            .append(ServiceName).append(FunctionName).append(RequestID);
        return ResponseName;
    }
    ndn::Name makeResponseNameWithoutPrefix(const ndn::Name &requesterName, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &RequestID)
    {
        ndn::Name ResponseName;
        ResponseName.append(ndn::Name("/NDNSF/RESPONSE")).append(requesterName)
            .append(ServiceName).append(FunctionName).append(RequestID);
        return ResponseName;
    }

        // ndn::Name ServiceProviderName, RequesterName, ServiceName, FunctionName, ChallengeId
    std::tuple<ndn::Name, ndn::Name, ndn::Name, ndn::Name, ndn::Name>
    parsePermissionChallengeName(ndn::Name permissionChallengeName)
    {
        std::shared_ptr<ndn::Regex> permissionChallengeMatch = std::make_shared<ndn::Regex>("^(<>+)<NDNSF><PERMISSION><CHALLENGE>(<>*)(<>)(<>)(<>)$");
        bool res = permissionChallengeMatch->match(permissionChallengeName);
        if (res)
        {
            // ndn::Name identity = requestMatch->expand("\\1");
            return std::make_tuple<ndn::Name, ndn::Name, ndn::Name, ndn::Name, ndn::Name>(
                permissionChallengeMatch->expand("\\1"),
                permissionChallengeMatch->expand("\\2"),
                permissionChallengeMatch->expand("\\3"),
                permissionChallengeMatch->expand("\\4"),
                permissionChallengeMatch->expand("\\5"));
        }
        else
        {
            return std::make_tuple<ndn::Name, ndn::Name, ndn::Name, ndn::Name, ndn::Name>(
                ndn::Name(),
                ndn::Name(),
                ndn::Name(),
                ndn::Name(),
                ndn::Name());
        }
    }

    ndn::Name makePermissionChallengeName(const ndn::Name &ServiceProviderName,const ndn::Name &requesterName, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &ChallengeID)
    {
        ndn::Name permissionChallengeName;
        permissionChallengeName.append(ServiceProviderName).append(ndn::Name("/NDNSF/PERMISSION/CHALLENGE")).append(requesterName).append(ServiceName)
            .append(FunctionName).append(ChallengeID);
        return permissionChallengeName;
    }
    ndn::Name makePermissionChallengeNameWithoutPrefix(const ndn::Name &requesterName, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &ChallengeID)
    {
        ndn::Name permissionChallengeName;
        permissionChallengeName.append(ndn::Name("/NDNSF/PERMISSION/CHALLENGE")).append(requesterName).append(ServiceName)
            .append(FunctionName).append(ChallengeID);
        return permissionChallengeName;
    }



            // ndn::Name RequesterName, ServiceProviderName,  ServiceName, FunctionName, ChallengeId
    std::tuple<ndn::Name, ndn::Name, ndn::Name, ndn::Name, ndn::Name>
    parsePermissionResponseName(ndn::Name permissionChallengeName)
    {
        std::shared_ptr<ndn::Regex> permissionResponseMatch = std::make_shared<ndn::Regex>("^(<>+)<NDNSF><PERMISSION><RESPONSE>(<>*)(<>)(<>)(<>)$");
        bool res = permissionResponseMatch->match(permissionChallengeName);
        if (res)
        {
            return std::make_tuple<ndn::Name, ndn::Name, ndn::Name, ndn::Name, ndn::Name>(
                permissionResponseMatch->expand("\\1"),
                permissionResponseMatch->expand("\\2"),
                permissionResponseMatch->expand("\\3"),
                permissionResponseMatch->expand("\\4"),
                permissionResponseMatch->expand("\\5"));
        }
        else
        {
            return std::make_tuple<ndn::Name, ndn::Name, ndn::Name, ndn::Name, ndn::Name>(
                ndn::Name(),
                ndn::Name(),
                ndn::Name(),
                ndn::Name(),
                ndn::Name());
        }
    }

    ndn::Name makePermissionResponseName(const ndn::Name &requesterName, const ndn::Name &ServiceProviderName,const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &ChallengeID)
    {
        ndn::Name permissionResponseName;
        permissionResponseName.append(requesterName).append(ndn::Name("/NDNSF/PERMISSION/RESPONSE")).append(ServiceProviderName).append(ServiceName)
            .append(FunctionName).append(ChallengeID);
        return permissionResponseName;
    }

    ndn::Name makePermissionResponseNameWithoutPrefix(const ndn::Name &ServiceProviderName,const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &ChallengeID)
    {
        ndn::Name permissionResponseName;
        permissionResponseName.append(ndn::Name("/NDNSF/PERMISSION/RESPONSE")).append(ServiceProviderName).append(ServiceName)
            .append(FunctionName).append(ChallengeID);
        return permissionResponseName;
    }

    // Combine segments into a buffer
    std::shared_ptr<ndn::Buffer> CombineSegmentsIntoBuffer(ndn::nacabe::SPtrVector<ndn::Data> segments)
    {
        ndn::OBufferStream buf;
        for (auto segment : segments)
        {
            buf.write(reinterpret_cast<const char *>(segment->getContent().data()), segment->getContent().size());
        }
        return buf.buf();
    }

    // /name1/name2 -> <name1><name2>
    std::string NameToRegexString(ndn::Name& name){
        std::string tmp = "";
        for(int i= 0;i<(int)name.size();i++)
        {
            tmp = tmp + "<" + name[i].toUri().substr (0,name[i].toUri().length()) +">";
        }
        NDN_LOG_INFO(tmp);
        return tmp;
    }

    std::string RandomString(const int len)
    {
        static const char alphanum[] =
            "0123456789"
            "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
            "abcdefghijklmnopqrstuvwxyz";
        std::string tmp_s;
        tmp_s.reserve(len);

        for (int i = 0; i < len; ++i)
        {
            tmp_s += alphanum[rand() % (sizeof(alphanum) - 1)];
        }

        return tmp_s;
    }
}

