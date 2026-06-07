#include "utils.hpp"

#include <ndn-cxx/security/transform.hpp>
#include <ndn-cxx/util/sha256.hpp>

#include <sstream>

namespace ndn_service_framework
{

    NDN_LOG_INIT(ndn_service_framework.utils);

    std::string requestRegexString = "^(<>+)<NDNSF><REQUEST>(<>)(<>)(<>)(<>)";
    std::string responseRegexString = "^(<>+)<NDNSF><RESPONSE>(<>+)(<>)(<>)(<>)$";
    std::string RequestAckRegexString = "^(<>+)<NDNSF><ACK>(<>*)(<>)(<>)(<>)$";
    std::string serviceSelectionRegexString = "^(<>+)<NDNSF><SELECTION>(<>+)(<>)(<>)(<>)$";
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

        void
        appendNameUriComponent(ndn::Name& dst, const ndn::Name& value)
        {
            dst.append(ndn::name::Component(value.toUri()));
        }

        std::optional<ndn::Name>
        parseNameUriComponent(const ndn::Name& name, size_t index)
        {
            if (index >= name.size()) {
                return std::nullopt;
            }

            const auto& component = name.get(index);
            std::string uri(reinterpret_cast<const char*>(component.value()),
                            component.value_size());
            if (uri.empty() || uri.front() != '/') {
                return std::nullopt;
            }

            return ndn::Name(uri);
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

        constexpr const char* LARGE_DATA_REFERENCE_MAGIC = "NDNSF-LARGE-DATA-REF";

        std::string
        bufferToString(const ndn::Buffer& payload)
        {
            return std::string(reinterpret_cast<const char*>(payload.data()),
                               payload.size());
        }

        std::string
        sanitizeReferenceField(std::string value)
        {
            for (auto& ch : value) {
                if (ch == '\n' || ch == '\r') {
                    ch = ' ';
                }
            }
            return value;
        }

        bool
        parseBoolText(const std::string& value)
        {
            return value == "1" || value == "true" || value == "yes";
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
    parseServiceSelectionName(ndn::Name ServiceSelectionName)
    {
        std::shared_ptr<ndn::Regex> serviceSelectionMatch = std::make_shared<ndn::Regex>(serviceSelectionRegexString);
        bool res = serviceSelectionMatch->match(ServiceSelectionName);
        if (res)
        {
            return std::make_tuple<ndn::Name, ndn::Name, ndn::Name, ndn::Name, ndn::Name>(
                serviceSelectionMatch->expand("\\1"),
                serviceSelectionMatch->expand("\\2"),
                serviceSelectionMatch->expand("\\3"),
                serviceSelectionMatch->expand("\\4"),
                serviceSelectionMatch->expand("\\5"));
        }
        else
        {
            return std::nullopt;
        }
    }

    ndn::Name makeServiceSelectionName(const ndn::Name &requesterName, const ndn::Name &ServiceProviderName,const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &msgID)
    {
        ndn::Name serviceSelectionName;
        serviceSelectionName.append(requesterName).append(ndn::Name("/NDNSF/SELECTION")).append(ServiceProviderName).append(ServiceName)
            .append(FunctionName).append(msgID);
        return serviceSelectionName;
    }

    ndn::Name makeServiceSelectionNameWithoutPrefix(const ndn::Name &ServiceProviderName,const ndn::Name &ServiceName, const ndn::Name &FunctionName, const ndn::Name &msgID)
    {
        ndn::Name serviceSelectionName;
        serviceSelectionName.append(ndn::Name("/NDNSF/SELECTION")).append(ServiceProviderName).append(ServiceName)
            .append(FunctionName).append(msgID);
        return serviceSelectionName;
    }

    ndn::Name makeRequestNameV2(const ndn::Name& requesterName,
                                const ndn::Name& serviceName,
                                const ndn::Name& requestId)
    {
        ndn::Name requestName;
        requestName.append(requesterName).append(ndn::Name("/NDNSF/REQUEST"));
        requestName.append(serviceName);
        requestName.append(requestId);
        return requestName;
    }

    ndn::Name makeRequestNameWithoutPrefixV2(const ndn::Name& serviceName,
                                             const ndn::Name& requestId)
    {
        ndn::Name requestName;
        requestName.append(ndn::Name("/NDNSF/REQUEST"));
        requestName.append(serviceName);
        requestName.append(requestId);
        return requestName;
    }

    std::optional<RequestNameV2> parseRequestNameV2(const ndn::Name& requestName)
    {
        auto marker = findNdnsfMessageMarker(requestName, "REQUEST");
        if (!marker) {
            return std::nullopt;
        }

        const size_t index = *marker + 2;
        if (index + 2 > requestName.size()) {
            return std::nullopt;
        }
        const size_t serviceComponentCount = requestName.size() - index - 1;

        return RequestNameV2{
            getSubNameByComponentCount(requestName, 0, *marker),
            getSubNameByComponentCount(requestName, index, serviceComponentCount),
            getSubNameByComponentCount(requestName, index + serviceComponentCount, 1)};
    }

    ndn::Name makeResponseNameV2(const ndn::Name& providerName,
                                 const ndn::Name& requesterName,
                                 const ndn::Name& serviceName,
                                 const ndn::Name& requestId)
    {
        ndn::Name responseName;
        responseName.append(providerName).append(ndn::Name("/NDNSF/RESPONSE"));
        appendNameUriComponent(responseName, requesterName);
        responseName.append(serviceName);
        responseName.append(requestId);
        return responseName;
    }

    ndn::Name makeResponseNameWithoutPrefixV2(const ndn::Name& requesterName,
                                              const ndn::Name& serviceName,
                                              const ndn::Name& requestId)
    {
        ndn::Name responseName;
        responseName.append(ndn::Name("/NDNSF/RESPONSE"));
        appendNameUriComponent(responseName, requesterName);
        responseName.append(serviceName);
        responseName.append(requestId);
        return responseName;
    }

    std::optional<ResponseNameV2> parseResponseNameV2(const ndn::Name& responseName)
    {
        auto marker = findNdnsfMessageMarker(responseName, "RESPONSE");
        if (!marker) {
            return std::nullopt;
        }

        const size_t index = *marker + 2;
        if (index + 3 > responseName.size()) {
            return std::nullopt;
        }
        auto requesterName = parseNameUriComponent(responseName, index);
        if (!requesterName) {
            return std::nullopt;
        }
        const size_t serviceIndex = index + 1;
        const size_t serviceComponentCount = responseName.size() - serviceIndex - 1;

        return ResponseNameV2{
            getSubNameByComponentCount(responseName, 0, *marker),
            *requesterName,
            getSubNameByComponentCount(responseName, serviceIndex, serviceComponentCount),
            getSubNameByComponentCount(responseName, responseName.size() - 1, 1)};
    }

    ndn::Name makeRequestAckNameV2(const ndn::Name& providerName,
                                   const ndn::Name& requesterName,
                                   const ndn::Name& serviceName,
                                   const ndn::Name& requestId)
    {
        ndn::Name requestAckName;
        requestAckName.append(providerName).append(ndn::Name("/NDNSF/ACK"));
        appendNameUriComponent(requestAckName, requesterName);
        requestAckName.append(serviceName);
        requestAckName.append(requestId);
        return requestAckName;
    }

    ndn::Name makeRequestAckNameWithoutPrefixV2(const ndn::Name& requesterName,
                                                const ndn::Name& serviceName,
                                                const ndn::Name& requestId)
    {
        ndn::Name requestAckName;
        requestAckName.append(ndn::Name("/NDNSF/ACK"));
        appendNameUriComponent(requestAckName, requesterName);
        requestAckName.append(serviceName);
        requestAckName.append(requestId);
        return requestAckName;
    }

    std::optional<RequestAckNameV2> parseRequestAckNameV2(const ndn::Name& requestAckName)
    {
        auto marker = findNdnsfMessageMarker(requestAckName, "ACK");
        if (!marker) {
            return std::nullopt;
        }

        const size_t index = *marker + 2;
        if (index + 3 > requestAckName.size()) {
            return std::nullopt;
        }
        auto requesterName = parseNameUriComponent(requestAckName, index);
        if (!requesterName) {
            return std::nullopt;
        }
        const size_t serviceIndex = index + 1;
        const size_t serviceComponentCount = requestAckName.size() - serviceIndex - 1;

        return RequestAckNameV2{
            getSubNameByComponentCount(requestAckName, 0, *marker),
            *requesterName,
            getSubNameByComponentCount(requestAckName, serviceIndex, serviceComponentCount),
            getSubNameByComponentCount(requestAckName, requestAckName.size() - 1, 1)};
    }

    ndn::Name makeServiceSelectionNameV2(const ndn::Name& requesterName,
                                            const ndn::Name& providerName,
                                            const ndn::Name& serviceName,
                                            const ndn::Name& requestId)
    {
        ndn::Name serviceSelectionName;
        serviceSelectionName.append(requesterName).append(ndn::Name("/NDNSF/SELECTION"));
        appendNameUriComponent(serviceSelectionName, providerName);
        serviceSelectionName.append(serviceName);
        serviceSelectionName.append(requestId);
        return serviceSelectionName;
    }

    ndn::Name makeServiceSelectionNameWithoutPrefixV2(const ndn::Name& providerName,
                                                         const ndn::Name& serviceName,
                                                         const ndn::Name& requestId)
    {
        ndn::Name serviceSelectionName;
        serviceSelectionName.append(ndn::Name("/NDNSF/SELECTION"));
        appendNameUriComponent(serviceSelectionName, providerName);
        serviceSelectionName.append(serviceName);
        serviceSelectionName.append(requestId);
        return serviceSelectionName;
    }

    std::optional<ServiceSelectionNameV2>
    parseServiceSelectionNameV2(const ndn::Name& serviceSelectionName)
    {
        auto marker = findNdnsfMessageMarker(serviceSelectionName, "SELECTION");
        if (!marker) {
            return std::nullopt;
        }

        const size_t index = *marker + 2;
        if (index + 3 > serviceSelectionName.size()) {
            return std::nullopt;
        }
        auto providerName = parseNameUriComponent(serviceSelectionName, index);
        if (!providerName) {
            return std::nullopt;
        }
        const size_t serviceIndex = index + 1;
        const size_t serviceComponentCount = serviceSelectionName.size() - serviceIndex - 1;

        return ServiceSelectionNameV2{
            getSubNameByComponentCount(serviceSelectionName, 0, *marker),
            *providerName,
            getSubNameByComponentCount(serviceSelectionName, serviceIndex, serviceComponentCount),
            getSubNameByComponentCount(serviceSelectionName, serviceSelectionName.size() - 1, 1)};
    }

    ndn::Name makeCompactServiceSelectionNameV2(const ndn::Name& requesterName,
                                                const ndn::Name& serviceName,
                                                const ndn::Name& requestId)
    {
        ndn::Name serviceSelectionName;
        serviceSelectionName.append(requesterName).append(ndn::Name("/NDNSF/SELECTION"));
        appendNameUriComponent(serviceSelectionName, ndn::Name("/NDNSF/COMPACT"));
        serviceSelectionName.append(serviceName);
        serviceSelectionName.append(requestId);
        return serviceSelectionName;
    }

    ndn::Name makeCompactServiceSelectionNameWithoutPrefixV2(const ndn::Name& serviceName,
                                                             const ndn::Name& requestId)
    {
        ndn::Name serviceSelectionName;
        serviceSelectionName.append(ndn::Name("/NDNSF/SELECTION"));
        appendNameUriComponent(serviceSelectionName, ndn::Name("/NDNSF/COMPACT"));
        serviceSelectionName.append(serviceName);
        serviceSelectionName.append(requestId);
        return serviceSelectionName;
    }

    std::optional<CompactServiceSelectionNameV2>
    parseCompactServiceSelectionNameV2(const ndn::Name& serviceSelectionName)
    {
        auto marker = findNdnsfMessageMarker(serviceSelectionName, "SELECTION");
        if (!marker) {
            return std::nullopt;
        }
        const size_t compactIndex = *marker + 2;
        if (compactIndex + 3 > serviceSelectionName.size()) {
            return std::nullopt;
        }
        const auto compactMarker = parseNameUriComponent(serviceSelectionName, compactIndex);
        if (!compactMarker || *compactMarker != ndn::Name("/NDNSF/COMPACT")) {
            return std::nullopt;
        }

        const size_t serviceIndex = compactIndex + 1;
        const size_t serviceComponentCount = serviceSelectionName.size() - serviceIndex - 1;
        return CompactServiceSelectionNameV2{
            getSubNameByComponentCount(serviceSelectionName, 0, *marker),
            getSubNameByComponentCount(serviceSelectionName, serviceIndex, serviceComponentCount),
            getSubNameByComponentCount(serviceSelectionName, serviceSelectionName.size() - 1, 1)};
    }

    std::string computeSelectionProviderTokenProofHash(const ndn::Name& requesterName,
                                                       const ndn::Name& providerName,
                                                       const ndn::Name& serviceName,
                                                       const std::string& providerToken)
    {
        if (providerToken.empty()) {
            return "";
        }
        ndn::util::Sha256 digest;
        digest << "SELECTION";
        digest << requesterName.toUri();
        digest << providerName.toUri();
        digest << serviceName.toUri();
        digest << providerToken;
        return digest.toString();
    }

    ndn::Name
    makeSelectionStatusQueryName(const ndn::Name& providerName,
                                 const ndn::Name& serviceName,
                                 const std::string& selectionDigest)
    {
        ndn::Name name(providerName);
        name.append(ndn::Name("/NDNSF/SELECTION-STATUS"));
        name.append(serviceName);
        name.append(selectionDigest);
        return name;
    }

    std::optional<SelectionStatusQueryName>
    parseSelectionStatusQueryName(const ndn::Name& statusQueryName)
    {
        auto marker = findNdnsfMessageMarker(statusQueryName, "SELECTION-STATUS");
        if (!marker || *marker + 4 > statusQueryName.size()) {
            return std::nullopt;
        }
        const size_t serviceIndex = *marker + 2;
        const size_t serviceComponentCount = statusQueryName.size() - serviceIndex - 1;
        return SelectionStatusQueryName{
            getSubNameByComponentCount(statusQueryName, 0, *marker),
            getSubNameByComponentCount(statusQueryName, serviceIndex, serviceComponentCount),
            statusQueryName.get(statusQueryName.size() - 1).toUri()};
    }

    std::string
    computeSelectionDigest(const ServiceSelectionMessage& message)
    {
        const auto block = message.WireEncode();
        ndn::util::Sha256 digest;
        digest << std::string(reinterpret_cast<const char*>(block.data()),
                              block.size());
        return digest.toString();
    }

    ndn::Name makeCollaborationDataName(const ndn::Name& producerName,
                                        const ndn::Name& requesterName,
                                        const ndn::Name& requestId,
                                        const std::string& keyScope,
                                        const ndn::Name& topic,
                                        uint64_t sequence)
    {
        ndn::Name name(producerName);
        name.append(ndn::Name("/NDNSF/COLLAB"));
        appendCountedName(name, requesterName);
        name.append(requestId);
        name.append(keyScope);
        name.append(std::to_string(topic.size()));
        name.append(topic);
        name.append(std::to_string(sequence));
        return name;
    }

    std::optional<CollaborationDataName>
    parseCollaborationDataName(const ndn::Name& collaborationDataName)
    {
        auto marker = findNdnsfMessageMarker(collaborationDataName, "COLLAB");
        if (!marker) {
            return std::nullopt;
        }

        size_t index = *marker + 2;
        auto requesterName = parseCountedName(collaborationDataName, index);
        if (!requesterName || index >= collaborationDataName.size()) {
            return std::nullopt;
        }

        ndn::Name requestId(collaborationDataName.get(index++).toUri());
        if (index >= collaborationDataName.size()) {
            return std::nullopt;
        }
        const std::string keyScope = collaborationDataName.get(index++).toUri();

        auto topicCount = parseComponentCount(collaborationDataName, index);
        if (!topicCount) {
            return std::nullopt;
        }
        ++index;
        if (index + *topicCount > collaborationDataName.size()) {
            return std::nullopt;
        }
        ndn::Name topic = getSubNameByComponentCount(collaborationDataName, index, *topicCount);
        index += *topicCount;
        if (index >= collaborationDataName.size()) {
            return std::nullopt;
        }

        uint64_t sequence = 0;
        const auto seqText = collaborationDataName.get(index).toUri();
        for (const auto ch : seqText) {
            if (ch < '0' || ch > '9') {
                return std::nullopt;
            }
            sequence = (sequence * 10) + static_cast<uint64_t>(ch - '0');
        }

        return CollaborationDataName{
            getSubNameByComponentCount(collaborationDataName, 0, *marker),
            *requesterName,
            requestId,
            keyScope,
            topic,
            sequence
        };
    }

    ndn::Buffer
    encodeLargeDataReferencePayload(const LargeDataReference& reference)
    {
        std::ostringstream os;
        os << LARGE_DATA_REFERENCE_MAGIC << "\n"
           << "version=1\n"
           << "name=" << reference.dataName.toUri() << "\n"
           << "type=" << sanitizeReferenceField(reference.objectType) << "\n"
           << "object_id=" << sanitizeReferenceField(reference.objectId) << "\n"
           << "plaintext_size=" << reference.plaintextSize << "\n"
           << "encrypted=" << (reference.encrypted ? "1" : "0") << "\n"
           << "digest=" << sanitizeReferenceField(reference.digest) << "\n";
        const auto text = os.str();
        return ndn::Buffer(reinterpret_cast<const uint8_t*>(text.data()), text.size());
    }

    std::optional<LargeDataReference>
    parseLargeDataReferencePayload(const ndn::Buffer& payload)
    {
        const auto text = bufferToString(payload);
        if (text.rfind(std::string(LARGE_DATA_REFERENCE_MAGIC) + "\n", 0) != 0 &&
            text != LARGE_DATA_REFERENCE_MAGIC) {
            return std::nullopt;
        }

        LargeDataReference reference;
        std::istringstream input(text);
        std::string line;
        std::getline(input, line);

        while (std::getline(input, line)) {
            const auto pos = line.find('=');
            if (pos == std::string::npos) {
                continue;
            }
            const auto key = line.substr(0, pos);
            const auto value = line.substr(pos + 1);
            if (key == "name") {
                try {
                    reference.dataName = ndn::Name(value);
                }
                catch (const std::exception&) {
                    return std::nullopt;
                }
            }
            else if (key == "type") {
                reference.objectType = value;
            }
            else if (key == "object_id") {
                reference.objectId = value;
            }
            else if (key == "plaintext_size") {
                try {
                    reference.plaintextSize = static_cast<size_t>(std::stoull(value));
                }
                catch (const std::exception&) {
                    return std::nullopt;
                }
            }
            else if (key == "encrypted") {
                reference.encrypted = parseBoolText(value);
            }
            else if (key == "digest") {
                reference.digest = value;
            }
        }

        if (reference.dataName.empty()) {
            return std::nullopt;
        }
        return reference;
    }

    bool
    isLargeDataReferencePayload(const ndn::Buffer& payload)
    {
        return parseLargeDataReferencePayload(payload).has_value();
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
        // V2 NAC-ABE routing is intentionally service-scoped:
        // requests and selection require /SERVICE/<service>, while
        // responses and ACKs require /PERMISSION/<service>.
        if (auto requestV2 = parseRequestNameV2(name)) {
            return std::vector<std::string>{"/SERVICE" + requestV2->serviceName.toUri()};
        }
        if (auto responseV2 = parseResponseNameV2(name)) {
            return std::vector<std::string>{"/PERMISSION" + responseV2->serviceName.toUri()};
        }
        if (auto ackV2 = parseRequestAckNameV2(name)) {
            return std::vector<std::string>{"/PERMISSION" + ackV2->serviceName.toUri()};
        }
        if (auto compactSelectionV2 = parseCompactServiceSelectionNameV2(name)) {
            return std::vector<std::string>{"/SERVICE" + compactSelectionV2->serviceName.toUri()};
        }
        if (auto selectionV2 = parseServiceSelectionNameV2(name)) {
            return std::vector<std::string>{"/SERVICE" + selectionV2->serviceName.toUri()};
        }

        std::shared_ptr<ndn::Regex> requestMatch = std::make_shared<ndn::Regex>(requestRegexString);
        std::shared_ptr<ndn::Regex> responseMatch = std::make_shared<ndn::Regex>(responseRegexString);
        std::shared_ptr<ndn::Regex> RequestAckMatch = std::make_shared<ndn::Regex>(RequestAckRegexString);
        std::shared_ptr<ndn::Regex> serviceSelectionMatch = std::make_shared<ndn::Regex>(serviceSelectionRegexString);
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
        else if (serviceSelectionMatch->match(name))
        {
            // attributes:{"/SERVICE/<serviceName>/<fucntionName>"}
            matchedAttributes.push_back("/SERVICE"+serviceSelectionMatch->expand("\\3").toUri()+serviceSelectionMatch->expand("\\4").toUri());
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
