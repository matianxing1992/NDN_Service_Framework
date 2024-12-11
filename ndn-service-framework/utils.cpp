#include "utils.hpp"

namespace ndn_service_framework
{

    NDN_LOG_INIT(ndn_service_framework.utils);

    std::string requestRegexString = "^(<>+)<NDNSF><REQUEST>(<>)(<>)(<>)(<>)";
    std::string responseRegexString = "^(<>+)<NDNSF><RESPONSE>(<>+)(<>)(<>)(<>)$";
    std::string RequestAckRegexString = "^(<>+)<NDNSF><ACK>(<>*)(<>)(<>)(<>)$";
    std::string serviceCoordinationRegexString = "^(<>+)<NDNSF><COORDINATION>(<>+)(<>)(<>)(<>)$";
    std::string permissionTokenRegexString = "^(<>+)<NDNSF><TOKEN>(<>)(<>)(<>)$";

    std::optional<std::tuple<ndn::Name, ndn::Name, ndn::Name, ndn::Name, ndn::Name>>
    parseRequestName(ndn::Name requestName)
    {
        std::shared_ptr<ndn::Regex> requestMatch = std::make_shared<ndn::Regex>(requestRegexString);
        bool res = requestMatch->match(requestName);
        if (res)
        {
            return std::make_tuple<ndn::Name, ndn::Name, ndn::Name, ndn::Name, ndn::Name>(
                requestMatch->expand("\\1"),
                requestMatch->expand("\\2"),
                requestMatch->expand("\\3"),
                requestMatch->expand("\\4"),
                requestMatch->expand("\\5"));
        }
        else
        {
            return std::nullopt;
        }
    }

    ndn::Name makeRequestName(const ndn::Name &requesterName,const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &bloomFilter, const ndn::Name &RequestID)
    {
        ndn::Name requestName;
        requestName.append(requesterName).append(ndn::Name("/NDNSF/REQUEST"))
            .append(ServiceName).append(FunctionName).append(bloomFilter).append(RequestID);
        return requestName;
    }

    ndn::Name makeRequestPrefixName(const ndn::Name &requesterName, const ndn::Name &ServiceName, const ndn::Name &FunctionName)
    {
        ndn::Name requestPrefixName;
        requestPrefixName.append(requesterName).append(ndn::Name("/NDNSF/REQUEST"))
            .append(ServiceName).append(FunctionName);
        return requestPrefixName;
    }

    ndn::Name makeRequestNameWithoutPrefix(const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &bloomFilter, const ndn::Name &RequestID)
    {
        ndn::Name requestName;
        requestName.append(ndn::Name("/NDNSF/REQUEST"))
            .append(ServiceName).append(FunctionName).append(bloomFilter).append(RequestID);
        return requestName;
    }

    std::optional<std::tuple<ndn::Name, ndn::Name, ndn::Name, ndn::Name, ndn::Name>>
    parseResponseName(ndn::Name responseName)
    {
        std::shared_ptr<ndn::Regex> responseMatch = std::make_shared<ndn::Regex>(responseRegexString);
        bool res = responseMatch->match(responseName);
        if (res)
        {
            return std::make_tuple<ndn::Name, ndn::Name, ndn::Name, ndn::Name, ndn::Name>(
                responseMatch->expand("\\1"),
                responseMatch->expand("\\2"),
                responseMatch->expand("\\3"),
                responseMatch->expand("\\4"),
                responseMatch->expand("\\5"));
        }
        else
        {
            return std::nullopt;
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

    std::optional<std::tuple<ndn::Name, ndn::Name, ndn::Name, ndn::Name, ndn::Name>>
    parseRequestAckName(ndn::Name RequestAckName)
    {
        std::shared_ptr<ndn::Regex> RequestAckMatch = std::make_shared<ndn::Regex>(RequestAckRegexString);
        bool res = RequestAckMatch->match(RequestAckName);
        if (res)
        {
            return std::make_tuple<ndn::Name, ndn::Name, ndn::Name, ndn::Name, ndn::Name>(
                RequestAckMatch->expand("\\1"),
                RequestAckMatch->expand("\\2"),
                RequestAckMatch->expand("\\3"),
                RequestAckMatch->expand("\\4"),
                RequestAckMatch->expand("\\5"));
        }
        else
        {
            return std::nullopt;
        }
    }

    ndn::Name makeRequestAckName(const ndn::Name &ServiceProviderName,const ndn::Name &requesterName, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &ChallengeID)
    {
        ndn::Name RequestAckName;
        RequestAckName.append(ServiceProviderName).append(ndn::Name("/NDNSF/ACK")).append(requesterName).append(ServiceName)
            .append(FunctionName).append(ChallengeID);
        return RequestAckName;
    }

    ndn::Name makeRequestAckNameWithoutPrefix(const ndn::Name &requesterName, const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &ChallengeID)
    {
        ndn::Name RequestAckName;
        RequestAckName.append(ndn::Name("/NDNSF/ACK")).append(requesterName).append(ServiceName)
            .append(FunctionName).append(ChallengeID);
        return RequestAckName;
    }

    std::optional<std::tuple<ndn::Name, ndn::Name, ndn::Name, ndn::Name, ndn::Name>>
    parseServiceCoordinationName(ndn::Name ServiceCoordinationName)
    {
        std::shared_ptr<ndn::Regex> serviceCoordinationMatch = std::make_shared<ndn::Regex>(serviceCoordinationRegexString);
        bool res = serviceCoordinationMatch->match(ServiceCoordinationName);
        if (res)
        {
            return std::make_tuple<ndn::Name, ndn::Name, ndn::Name, ndn::Name, ndn::Name>(
                serviceCoordinationMatch->expand("\\1"),
                serviceCoordinationMatch->expand("\\2"),
                serviceCoordinationMatch->expand("\\3"),
                serviceCoordinationMatch->expand("\\4"),
                serviceCoordinationMatch->expand("\\5"));
        }
        else
        {
            return std::nullopt;
        }
    }

    ndn::Name makeServiceCoordinationName(const ndn::Name &requesterName, const ndn::Name &ServiceProviderName,const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &msgID)
    {
        ndn::Name serviceCoordinationName;
        serviceCoordinationName.append(requesterName).append(ndn::Name("/NDNSF/COORDINATION")).append(ServiceProviderName).append(ServiceName)
            .append(FunctionName).append(msgID);
        return serviceCoordinationName;
    }

    ndn::Name makeServiceCoordinationNameWithoutPrefix(const ndn::Name &ServiceProviderName,const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &msgID)
    {
        ndn::Name serviceCoordinationName;
        serviceCoordinationName.append(ndn::Name("/NDNSF/COORDINATION")).append(ServiceProviderName).append(ServiceName)
            .append(FunctionName).append(msgID);
        return serviceCoordinationName;
    }

    std::optional<std::tuple<ndn::Name, ndn::Name, ndn::Name, ndn::Name>>
    parsePermissionTokenName(ndn::Name permissionTokenName)
    {
        std::shared_ptr<ndn::Regex> permissionTokenMatch = std::make_shared<ndn::Regex>("^(<>+)<NDNSF><TOKEN>(<>)(<>)(<>)$");
        bool res = permissionTokenMatch->match(permissionTokenName);
        if (res)
        {
            return std::make_tuple<ndn::Name, ndn::Name, ndn::Name, ndn::Name>(
                permissionTokenMatch->expand("\\1"),
                permissionTokenMatch->expand("\\2"),
                permissionTokenMatch->expand("\\3"),
                permissionTokenMatch->expand("\\4"));
        }
        else
        {
            return std::nullopt;
        }
    }

    ndn::Name makePermissionTokenNameWithoutPrefix(const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &seqNum)
    {
        ndn::Name permissionTokenName;
        permissionTokenName.append(ndn::Name("/NDNSF/TOKEN")).append(ServiceName)
            .append(FunctionName).append(seqNum);
        return permissionTokenName;
    }

    std::shared_ptr<ndn::Buffer> CombineSegmentsIntoBuffer(ndn::nacabe::SPtrVector<ndn::Data> segments)
    {
        ndn::OBufferStream buf;
        for (auto segment : segments)
        {
            buf.write(reinterpret_cast<const char *>(segment->getContent().data()), segment->getContent().size());
        }
        return buf.buf();
    }

    std::string NameToRegexString(ndn::Name& name){
        std::string tmp = "";
        for(int i= 0;i<(int)name.size();i++)
       
        {
            tmp = tmp + "<" + name[i].toUri().substr(0, name[i].toUri().length()) + ">";
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

    std::optional<std::vector<std::string>> GetAttributesByName(const ndn::Name& name)
    {
        std::shared_ptr<ndn::Regex> requestMatch = std::make_shared<ndn::Regex>(requestRegexString);
        std::shared_ptr<ndn::Regex> responseMatch = std::make_shared<ndn::Regex>(responseRegexString);
        std::shared_ptr<ndn::Regex> RequestAckMatch = std::make_shared<ndn::Regex>(RequestAckRegexString);
        std::shared_ptr<ndn::Regex> serviceCoordinationMatch = std::make_shared<ndn::Regex>(serviceCoordinationRegexString);
        std::shared_ptr<ndn::Regex> permissionTokenMatch = std::make_shared<ndn::Regex>(permissionTokenRegexString);
        
        std::vector<std::string> matchedAttributes;

        if (requestMatch->match(name))
        {
            // attributes:{"/SERVICE/<serviceName>/<fucntionName>"}
            matchedAttributes.push_back("/SERVICE"+requestMatch->expand("\\2").toUri()+requestMatch->expand("\\3").toUri());
            return matchedAttributes;
        }
        else if (responseMatch->match(name))
        {
            // attributes:{"/ID/<requesterName>"}
            matchedAttributes.push_back("/ID"+responseMatch->expand("\\2").toUri());
            return matchedAttributes;
        }
        else if (RequestAckMatch->match(name))
        {
            // attributes:{"/ID/<requesterName>"}
            matchedAttributes.push_back("/ID"+RequestAckMatch->expand("\\2").toUri());
            return matchedAttributes;
        }
        else if (serviceCoordinationMatch->match(name))
        {
            // attributes:{"/SERVICE/<serviceName>/<fucntionName>"}
            matchedAttributes.push_back("/SERVICE"+serviceCoordinationMatch->expand("\\3").toUri()+serviceCoordinationMatch->expand("\\4").toUri());
            return matchedAttributes;
        }
        else if (permissionTokenMatch->match(name))
        {
            // attributes:{"/PERMISSION/<providerName>/<serviceName>/<fucntionName>"}
            matchedAttributes.push_back("/PERMISSION/"+permissionTokenMatch->expand("\\1").toUri()+
                permissionTokenMatch->expand("\\2").toUri()+permissionTokenMatch->expand("\\3").toUri());
            return matchedAttributes;
        }
        else
        {
            return std::nullopt;
        }
    }
    
    ndn::span<const uint8_t> blockToSpan(const ndn::Block &block)
    {
        return ndn::span<const uint8_t>(block.data(), block.size());
    }
    std::string ConcatenateString(const std::vector<std::string> &words)
    {
        // 使用 std::accumulate 连接字符串
        std::string result = std::accumulate(std::begin(words), std::end(words), std::string{},
                                             [](const std::string &accumulated, const std::string &current)
                                             {
                                                 return accumulated.empty() ? current : accumulated + " and " + current;
                                             });
        return result;
    }

    std::vector<uint8_t> mergeDataContents(const std::vector<std::shared_ptr<ndn::Data>>& dataPackets) {
        // Calculate total size needed
        size_t totalSize = 0;
        for (const auto& dataPtr : dataPackets) {
            const auto& content = dataPtr->getContent();
            totalSize += content.value_size(); // Use value_size() instead of size()
        }

        // Allocate a vector to store the merged content
        std::vector<uint8_t> mergedContent;
        mergedContent.reserve(totalSize); // Reserve space to avoid frequent reallocations

        // Copy content of each ndn::Data object into mergedContent vector
        for (const auto& dataPtr : dataPackets) {
            const auto& content = dataPtr->getContent();
            mergedContent.insert(mergedContent.end(), content.value_begin(), content.value_end());
        }

        return mergedContent;
    }
}
