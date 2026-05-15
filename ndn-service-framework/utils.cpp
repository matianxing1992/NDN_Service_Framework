#include "utils.hpp"

#include <ndn-cxx/security/transform.hpp>

namespace ndn_service_framework
{

    NDN_LOG_INIT(ndn_service_framework.utils);

    std::string requestRegexString = "^(<>+)<NDNSF><REQUEST>(<>)(<>)(<>)(<>)";
    std::string responseRegexString = "^(<>+)<NDNSF><RESPONSE>(<>+)(<>)(<>)(<>)$";
    std::string RequestAckRegexString = "^(<>+)<NDNSF><ACK>(<>*)(<>)(<>)(<>)$";
    std::string serviceCoordinationRegexString = "^(<>+)<NDNSF><COORDINATION>(<>+)(<>)(<>)(<>)$";
    std::string permissionTokenRegexString = "^(<>+)<NDNSF><TOKEN>(<>)(<>)(<>)$";

    namespace
    {
        ndn::Name
        getSubNameByComponentCount(const ndn::Name& name, size_t begin, size_t count)
        {
            ndn::Name result;
            for (size_t i = 0; i < count; ++i) {
                result.append(name.get(begin + i));
            }
            return result;
        }

        void
        appendCountedName(ndn::Name& dst, const ndn::Name& value)
        {
            dst.append(std::to_string(value.size()));
            dst.append(value);
        }

        std::optional<size_t>
        parseComponentCount(const ndn::Name& name, size_t index)
        {
            if (index >= name.size()) {
                return std::nullopt;
            }

            const auto text = name.get(index).toUri();
            if (text.empty()) {
                return std::nullopt;
            }

            size_t value = 0;
            for (const auto ch : text) {
                if (ch < '0' || ch > '9') {
                    return std::nullopt;
                }
                value = (value * 10) + static_cast<size_t>(ch - '0');
            }
            return value;
        }

        std::optional<ndn::Name>
        parseCountedName(const ndn::Name& name, size_t& index)
        {
            auto count = parseComponentCount(name, index);
            if (!count) {
                return std::nullopt;
            }
            ++index;

            if (index + *count > name.size()) {
                return std::nullopt;
            }

            ndn::Name result = getSubNameByComponentCount(name, index, *count);
            index += *count;
            return result;
        }

        std::optional<size_t>
        findNdnsfMessageMarker(const ndn::Name& name, const std::string& messageType)
        {
            for (size_t i = 0; i + 1 < name.size(); ++i) {
                if (name.get(i).toUri() == "NDNSF" &&
                    name.get(i + 1).toUri() == messageType) {
                    return i;
                }
            }
            return std::nullopt;
        }

        ndn::span<const uint8_t>
        bufferToSpan(const ndn::Buffer& buffer)
        {
            return ndn::span<const uint8_t>(buffer.data(), buffer.size());
        }

        ndn::span<uint8_t>
        mutableBufferToSpan(ndn::Buffer& buffer)
        {
            return ndn::span<uint8_t>(buffer.data(), buffer.size());
        }

        ndn::Buffer
        runAesCbc(ndn::span<const uint8_t> input,
                  ndn::span<const uint8_t> key,
                  ndn::span<const uint8_t> iv,
                  ndn::CipherOperator op)
        {
            ndn::OBufferStream output;
            ndn::security::transform::bufferSource(input) >>
                ndn::security::transform::blockCipher(ndn::BlockCipherAlgorithm::AES_CBC,
                                                      op,
                                                      key,
                                                      iv) >>
                ndn::security::transform::streamSink(output);

            const auto encrypted = output.buf();
            return ndn::Buffer(encrypted->begin(), encrypted->end());
        }
    }

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

    ndn::Name makeRequestNameV2(const ndn::Name& requesterName,
                                const ndn::Name& serviceName,
                                const ndn::Name& bloomFilter,
                                const ndn::Name& requestId)
    {
        ndn::Name requestName;
        requestName.append(requesterName).append(ndn::Name("/NDNSF/REQUEST"));
        appendCountedName(requestName, serviceName);
        requestName.append(bloomFilter).append(requestId);
        return requestName;
    }

    ndn::Name makeRequestNameWithoutPrefixV2(const ndn::Name& serviceName,
                                             const ndn::Name& bloomFilter,
                                             const ndn::Name& requestId)
    {
        ndn::Name requestName;
        requestName.append(ndn::Name("/NDNSF/REQUEST"));
        appendCountedName(requestName, serviceName);
        requestName.append(bloomFilter).append(requestId);
        return requestName;
    }

    std::optional<RequestNameV2> parseRequestNameV2(const ndn::Name& requestName)
    {
        auto marker = findNdnsfMessageMarker(requestName, "REQUEST");
        if (!marker) {
            return std::nullopt;
        }

        size_t index = *marker + 2;
        auto serviceName = parseCountedName(requestName, index);
        if (!serviceName || index + 2 != requestName.size()) {
            return std::nullopt;
        }

        return RequestNameV2{
            getSubNameByComponentCount(requestName, 0, *marker),
            *serviceName,
            getSubNameByComponentCount(requestName, index, 1),
            getSubNameByComponentCount(requestName, index + 1, 1)};
    }

    ndn::Name makeResponseNameV2(const ndn::Name& providerName,
                                 const ndn::Name& requesterName,
                                 const ndn::Name& serviceName,
                                 const ndn::Name& requestId)
    {
        ndn::Name responseName;
        responseName.append(providerName).append(ndn::Name("/NDNSF/RESPONSE"));
        appendCountedName(responseName, requesterName);
        appendCountedName(responseName, serviceName);
        responseName.append(requestId);
        return responseName;
    }

    ndn::Name makeResponseNameWithoutPrefixV2(const ndn::Name& requesterName,
                                              const ndn::Name& serviceName,
                                              const ndn::Name& requestId)
    {
        ndn::Name responseName;
        responseName.append(ndn::Name("/NDNSF/RESPONSE"));
        appendCountedName(responseName, requesterName);
        appendCountedName(responseName, serviceName);
        responseName.append(requestId);
        return responseName;
    }

    std::optional<ResponseNameV2> parseResponseNameV2(const ndn::Name& responseName)
    {
        auto marker = findNdnsfMessageMarker(responseName, "RESPONSE");
        if (!marker) {
            return std::nullopt;
        }

        size_t index = *marker + 2;
        auto requesterName = parseCountedName(responseName, index);
        auto serviceName = parseCountedName(responseName, index);
        if (!requesterName || !serviceName || index + 1 != responseName.size()) {
            return std::nullopt;
        }

        return ResponseNameV2{
            getSubNameByComponentCount(responseName, 0, *marker),
            *requesterName,
            *serviceName,
            getSubNameByComponentCount(responseName, index, 1)};
    }

    ndn::Name makeRequestAckNameV2(const ndn::Name& providerName,
                                   const ndn::Name& requesterName,
                                   const ndn::Name& serviceName,
                                   const ndn::Name& requestId)
    {
        ndn::Name requestAckName;
        requestAckName.append(providerName).append(ndn::Name("/NDNSF/ACK"));
        appendCountedName(requestAckName, requesterName);
        appendCountedName(requestAckName, serviceName);
        requestAckName.append(requestId);
        return requestAckName;
    }

    ndn::Name makeRequestAckNameWithoutPrefixV2(const ndn::Name& requesterName,
                                                const ndn::Name& serviceName,
                                                const ndn::Name& requestId)
    {
        ndn::Name requestAckName;
        requestAckName.append(ndn::Name("/NDNSF/ACK"));
        appendCountedName(requestAckName, requesterName);
        appendCountedName(requestAckName, serviceName);
        requestAckName.append(requestId);
        return requestAckName;
    }

    std::optional<RequestAckNameV2> parseRequestAckNameV2(const ndn::Name& requestAckName)
    {
        auto marker = findNdnsfMessageMarker(requestAckName, "ACK");
        if (!marker) {
            return std::nullopt;
        }

        size_t index = *marker + 2;
        auto requesterName = parseCountedName(requestAckName, index);
        auto serviceName = parseCountedName(requestAckName, index);
        if (!requesterName || !serviceName || index + 1 != requestAckName.size()) {
            return std::nullopt;
        }

        return RequestAckNameV2{
            getSubNameByComponentCount(requestAckName, 0, *marker),
            *requesterName,
            *serviceName,
            getSubNameByComponentCount(requestAckName, index, 1)};
    }

    ndn::Name makeServiceCoordinationNameV2(const ndn::Name& requesterName,
                                            const ndn::Name& providerName,
                                            const ndn::Name& serviceName,
                                            const ndn::Name& requestId)
    {
        ndn::Name serviceCoordinationName;
        serviceCoordinationName.append(requesterName).append(ndn::Name("/NDNSF/COORDINATION"));
        appendCountedName(serviceCoordinationName, providerName);
        appendCountedName(serviceCoordinationName, serviceName);
        serviceCoordinationName.append(requestId);
        return serviceCoordinationName;
    }

    ndn::Name makeServiceCoordinationNameWithoutPrefixV2(const ndn::Name& providerName,
                                                         const ndn::Name& serviceName,
                                                         const ndn::Name& requestId)
    {
        ndn::Name serviceCoordinationName;
        serviceCoordinationName.append(ndn::Name("/NDNSF/COORDINATION"));
        appendCountedName(serviceCoordinationName, providerName);
        appendCountedName(serviceCoordinationName, serviceName);
        serviceCoordinationName.append(requestId);
        return serviceCoordinationName;
    }

    std::optional<ServiceCoordinationNameV2>
    parseServiceCoordinationNameV2(const ndn::Name& serviceCoordinationName)
    {
        auto marker = findNdnsfMessageMarker(serviceCoordinationName, "COORDINATION");
        if (!marker) {
            return std::nullopt;
        }

        size_t index = *marker + 2;
        auto providerName = parseCountedName(serviceCoordinationName, index);
        auto serviceName = parseCountedName(serviceCoordinationName, index);
        if (!providerName || !serviceName || index + 1 != serviceCoordinationName.size()) {
            return std::nullopt;
        }

        return ServiceCoordinationNameV2{
            getSubNameByComponentCount(serviceCoordinationName, 0, *marker),
            *providerName,
            *serviceName,
            getSubNameByComponentCount(serviceCoordinationName, index, 1)};
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

    EncryptedPermissionResponse
    encryptPermissionResponseForCertificate(const PermissionResponse& response,
                                            const ndn::security::Certificate& recipientCert)
    {
        ndn::security::transform::PublicKey recipientPublicKey;
        recipientPublicKey.loadPkcs8(recipientCert.getPublicKey());
        if (recipientPublicKey.getKeyType() != ndn::KeyType::RSA) {
            throw std::invalid_argument("PermissionResponse encryption requires an RSA recipient certificate");
        }

        ndn::Block plaintext = response.WireEncode();
        plaintext.encode();

        ndn::Buffer aesKey(32);
        ndn::Buffer iv(16);
        ndn::random::generateSecureBytes(mutableBufferToSpan(aesKey));
        ndn::random::generateSecureBytes(mutableBufferToSpan(iv));

        ndn::Buffer cipherText = runAesCbc(ndn::span<const uint8_t>(plaintext.data(), plaintext.size()),
                                           bufferToSpan(aesKey),
                                           bufferToSpan(iv),
                                           ndn::CipherOperator::ENCRYPT);
        auto encryptedAesKey = recipientPublicKey.encrypt(bufferToSpan(aesKey));

        EncryptedPermissionResponse encryptedResponse;
        encryptedResponse.setRecipientCertName(recipientCert.getName().toUri());
        encryptedResponse.setAlgorithm("RSA-WRAPPED-AES-CBC");
        encryptedResponse.setEncryptedAesKey(ndn::Buffer(encryptedAesKey->begin(), encryptedAesKey->end()));
        encryptedResponse.setIv(iv);
        encryptedResponse.setCipherText(cipherText);
        return encryptedResponse;
    }

    PermissionResponse
    decryptPermissionResponseWithKeyChain(const EncryptedPermissionResponse& encryptedResponse,
                                          const ndn::security::KeyChain& keyChain)
    {
        if (encryptedResponse.getAlgorithm() != "RSA-WRAPPED-AES-CBC") {
            throw std::invalid_argument("Unsupported encrypted PermissionResponse algorithm: " +
                                        encryptedResponse.getAlgorithm());
        }

        const ndn::Name recipientCertName(encryptedResponse.getRecipientCertName());
        const ndn::Name recipientKeyName = ndn::security::extractKeyNameFromCertName(recipientCertName);
        auto aesKey = keyChain.getTpm().decrypt(bufferToSpan(encryptedResponse.getEncryptedAesKey()),
                                                recipientKeyName);
        if (aesKey == nullptr) {
            throw std::runtime_error("Cannot decrypt PermissionResponse AES key with local KeyChain");
        }

        ndn::Buffer plaintext = runAesCbc(bufferToSpan(encryptedResponse.getCipherText()),
                                          ndn::span<const uint8_t>(aesKey->data(), aesKey->size()),
                                          bufferToSpan(encryptedResponse.getIv()),
                                          ndn::CipherOperator::DECRYPT);

        auto [ok, block] = ndn::Block::fromBuffer(bufferToSpan(plaintext));
        if (!ok) {
            throw std::runtime_error("Decrypted PermissionResponse is not a valid TLV block");
        }

        PermissionResponse response;
        if (!response.WireDecode(block)) {
            throw std::runtime_error("Decrypted TLV block is not a PermissionResponse");
        }
        return response;
    }
}
