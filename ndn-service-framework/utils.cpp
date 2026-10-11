#include "utils.hpp"
#include "ProducerDataNaming.hpp"

#include <ndn-cxx/security/transform.hpp>
#include <ndn-cxx/util/sha256.hpp>

#include <sstream>
#include <algorithm>
#include <iomanip>
#include <stdexcept>
#include <openssl/rand.h>

namespace ndn_service_framework
{

    NDN_LOG_INIT(ndn_service_framework.utils);


    ndn::Name makeDeploymentRecordName(const ndn::Name& producer,
        const ndn::Name& application, const std::string& deploymentId,
        const std::string& kind, const std::string& revisionDigest,
        uint64_t epoch, const std::string& payloadDigest)
    {
        if (application.empty() || deploymentId.empty() ||
            (kind != "DEFINITION" && kind != "ACTIVATION" && kind != "REVOCATION"))
            throw std::invalid_argument("incomplete deployment record identity");
        const auto digest = [](const std::string& value) {
            if (value.size() != 71 || value.compare(0, 7, "sha256:") != 0)
                throw std::invalid_argument("deployment requires complete canonical SHA-256");
            return makeNamingDigestComponent(value.substr(7));
        };
        const auto& appWire = application.wireEncode();
        const auto deploymentKey = computeNamingDigest("ndnsf-naming/deployment", {
            {1, 1, {appWire.data(), appWire.data() + appWire.size()}},
            {2, 2, {deploymentId.begin(), deploymentId.end()}}});
        auto name = makeProducerDataPrefix(producer, "DEPLOYMENT");
        name.append(makeNamingLabelComponent(application.toUri(ndn::name::UriFormat::CANONICAL)))
            .append(makeNamingDigestComponent(deploymentKey));
        if (kind == "DEFINITION") {
            if (!revisionDigest.empty() || epoch != 0)
                throw std::invalid_argument("definition has unrelated revision or epoch");
            name.append("DEFINITION");
        }
        else {
            name.append("REVISION").append(digest(revisionDigest)).append(kind);
            if (kind == "REVOCATION") {
                if (epoch == 0) throw std::invalid_argument("revocation epoch must be positive");
                name.append("EPOCH").append(makeNamingDecimalComponent(epoch));
            }
            else if (epoch != 0) throw std::invalid_argument("activation has unrelated epoch suffix");
        }
        name.append(digest(payloadDigest));
        checkNamingNameSize(name);
        return name;
    }

    ndn::Block
    encodeControlQueryParameters(const ControlQueryParameters& query)
    {
        if ((query.generation == 0) != (query.epoch == 0))
            throw std::invalid_argument("incomplete control query version");
        ndn::Name carrier;
        carrier.append("CONTROL-QUERY").append(makeReversibleNameComponent(query.subject))
          .append(makeReversibleNameComponent(query.service))
          .append(makeNamingDecimalComponent(query.generation))
          .append(makeNamingDecimalComponent(query.epoch));
        return carrier.wireEncode();
    }

    std::optional<ControlQueryParameters>
    decodeControlQueryParameters(const ndn::Block& parameters)
    {
        try {
            auto block = parameters;
            if (block.type() == ndn::tlv::ApplicationParameters) {
                block.parse();
                if (block.elements().size() != 1) return std::nullopt;
                block = block.elements().front();
            }
            if (block.type() != ndn::tlv::Name) return std::nullopt;
            const ndn::Name carrier(block);
            if (carrier.size() != 5 || carrier[0] != ndn::name::Component("CONTROL-QUERY"))
                return std::nullopt;
            const auto subject = parseReversibleNameComponent(carrier[1]);
            const auto service = parseReversibleNameComponent(carrier[2]);
            if (!subject || !service) return std::nullopt;
            const auto decimal = [](const ndn::name::Component& c) {
                if (c.type() != 8) throw std::invalid_argument("invalid control version component");
                const std::string text(reinterpret_cast<const char*>(c.value()), c.value_size());
                if (text.empty() || (text.size() > 1 && text.front() == '0') ||
                    !std::all_of(text.begin(), text.end(), [](char ch) { return ch >= '0' && ch <= '9'; }))
                    throw std::invalid_argument("invalid control version decimal");
                const auto result = std::stoull(text);
                if (std::to_string(result) != text) throw std::invalid_argument("invalid control version range");
                return result;
            };
            ControlQueryParameters query{*subject, *service, decimal(carrier[3]), decimal(carrier[4])};
            if (!(encodeControlQueryParameters(query) == block)) return std::nullopt;
            return query;
        }
        catch (const std::exception&) { return std::nullopt; }
    }

    ndn::Name
    makeControlRpcName(const ndn::Name& producer, const std::string& operation,
                       const ndn::Block& parameters, const std::string& nonce)
    {
        static const std::set<std::string> operations{
            "PROVIDER-READY", "EXECUTION-ACTIVATE", "SELECTION-STATUS", "USER-PERMISSIONS",
            "PROVIDER-PERMISSIONS", "POLICY-MANIFEST", "POLICY-STATUS", "SERVICEACCESS",
            "SERVICEPROVISION", "CERTBOOTSTRAP", "AA-READY", "FRONTIER"};
        if (!operations.count(operation) || nonce.empty() || nonce.size() > 128 ||
            !parameters.isValid())
            throw std::invalid_argument("invalid control RPC context");
        ndn::Interest normalized(producer);
        normalized.setApplicationParameters(parameters);
        const auto app = normalized.getApplicationParameters();
        if (app.size() > 65536) throw std::invalid_argument("control RPC parameters exceed limit");
        const auto digest = computeNamingDigest("ndnsf-naming/control-call",
          {{1, 2, std::vector<uint8_t>(operation.begin(), operation.end())},
           {2, 4, std::vector<uint8_t>(app.data(), app.data() + app.size())}});
        auto name = makeProducerRpcName(producer, operation, digest, nonce);
        checkNamingNameSize(name);
        return name;
    }

    std::optional<ControlRpcName>
    parseControlRpcName(const ndn::Interest& interest, const ndn::Name& producer,
                        const std::string& operation)
    {
        try {
            const auto& final = interest.getName();
            const auto parsed = parseProducerDataName(final, producer);
            if (!parsed || parsed->category != "RPC" || parsed->suffix.size() != 4 ||
                parsed->suffix[0] != ndn::name::Component(operation) ||
                parsed->suffix[1].type() != 8 || parsed->suffix[1].value_size() != 32 ||
                parsed->suffix[2].type() != 8 || !parsed->suffix[3].isParametersSha256Digest() ||
                !interest.hasApplicationParameters() || !interest.isParametersDigestValid()) return std::nullopt;
            const auto nonce = std::string(reinterpret_cast<const char*>(parsed->suffix[2].value()),
                                            parsed->suffix[2].value_size());
            const auto logical = makeControlRpcName(producer, operation,
              interest.getApplicationParameters(), nonce);
            if (final.getPrefix(-1) != logical) return std::nullopt;
            std::ostringstream hex;
            hex << std::hex << std::setfill('0');
            for (auto byte : ndn::span<const uint8_t>(parsed->suffix[1].value(), 32))
                hex << std::setw(2) << unsigned(byte);
            return ControlRpcName{producer, operation, hex.str(), nonce};
        }
        catch (const std::exception&) { return std::nullopt; }
    }

    std::optional<PolicyStatusName>
    parsePolicyStatusName(const ndn::Name& controller, const ndn::Interest& interest)
    {
        if (!parseControlRpcName(interest, controller, "POLICY-STATUS")) return std::nullopt;
        const auto query = decodeControlQueryParameters(interest.getApplicationParameters());
        if (!query || !query->subject.empty() || query->service.empty()) return std::nullopt;
        PolicyStatusName parsed{query->service, std::nullopt};
        if (query->generation != 0) {
            const ControllerVersion version{query->generation, query->epoch};
            if (!version.isValid()) return std::nullopt;
            parsed.version = version;
        }
        return parsed;
    }

    std::string
    makeControlQueryNonce()
    {
        const auto millis = ndn::time::duration_cast<ndn::time::milliseconds>(
          ndn::time::system_clock::now().time_since_epoch()).count();
        return std::to_string(millis) + "-" + makeOpaqueControlHandle(16);
    }

std::optional<uint64_t> controlNonceExpiry(const ndn::Name& controller,
                                          const ndn::Interest& interest)
{
  const auto parsed = parseProducerDataName(interest.getName(), controller);
  if (!parsed || parsed->category != "RPC" || parsed->suffix.size() != 4 || parsed->suffix[2].type() != 8)
    return std::nullopt;
  const auto& component = parsed->suffix[2];
  const std::string nonce(reinterpret_cast<const char*>(component.value()), component.value_size());
  const auto separator = nonce.find('-');
  if (separator == std::string::npos || !isValidOpaqueControlHandle(nonce.substr(separator + 1)))
    return std::nullopt;
  try {
    const auto text = nonce.substr(0, separator);
    const auto minted = std::stoull(text);
    const auto now = static_cast<uint64_t>(ndn::time::duration_cast<ndn::time::milliseconds>(
      ndn::time::system_clock::now().time_since_epoch()).count());
    if (std::to_string(minted) != text || minted > now + 30000 ||
        (minted <= now && now - minted >= 300000)) return std::nullopt;
    return minted + 300000;
  }
  catch (const std::exception&) { return std::nullopt; }
}
    namespace
    {
        constexpr const char* POLICY_STATUS_MARKER = "version";

        ndn::Name
        policyStatusPrefix(const ndn::Name& controllerPrefix)
        {
            ndn::Name prefix(controllerPrefix);
            prefix.append("NDNSF").append("POLICY-STATUS");
            return prefix;
        }
    }

    ndn::Name
    makePolicyStatusName(const ndn::Name& controllerPrefix,
                         const ndn::Name& serviceName,
                         const ControllerVersion& version)
    {
        if (controllerPrefix.empty() || serviceName.empty() || !version.isValid()) {
            throw std::invalid_argument("invalid policy-status name inputs");
        }
        auto name = policyStatusPrefix(controllerPrefix);
        name.append(serviceName)
            .append(POLICY_STATUS_MARKER)
            .appendNumber(version.controllerGenerationTimestamp)
            .appendNumber(version.controllerEpoch);
        return name;
    }

    std::optional<PolicyStatusName>
    parsePolicyStatusName(const ndn::Name& controllerPrefix,
                          const ndn::Name& inputName)
    {
        if (controllerPrefix.empty()) {
            return std::nullopt;
        }
        auto name = inputName;
        if (!name.empty() && name[-1].isParametersSha256Digest()) {
            name = name.getPrefix(-1);
        }
        const auto prefix = policyStatusPrefix(controllerPrefix);
        if (!prefix.isPrefixOf(name) || name.size() <= prefix.size()) {
            return std::nullopt;
        }
        const auto suffix = name.getSubName(prefix.size());
        PolicyStatusName result;
        if (suffix.size() >= 3 &&
            suffix[-3].toUri() == POLICY_STATUS_MARKER) {
            const auto serviceSize = suffix.size() - 3;
            if (serviceSize == 0) {
                return std::nullopt;
            }
            result.serviceName = suffix.getPrefix(serviceSize);
            try {
                const ControllerVersion version{
                    suffix[-2].toNumber(), suffix[-1].toNumber()};
                if (!version.isValid()) {
                    return std::nullopt;
                }
                result.version = version;
            }
            catch (const std::exception&) {
                return std::nullopt;
            }
            return result;
        }

        result.serviceName = suffix;
        if (result.serviceName.empty()) {
            return std::nullopt;
        }
        return result;
    }


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
            dst.append(ndn::name::Component(value.toUri(ndn::name::UriFormat::CANONICAL)));
        }

        void
        appendCollaborationRequestId(ndn::Name& dst, const ndn::Name& requestId)
        {
            if (requestId.size() <= 1) dst.append(requestId);
            else appendNameUriComponent(dst, requestId);
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
            if (component.type() != ndn::tlv::GenericNameComponent ||
                uri.empty() || uri.front() != '/') {
                return std::nullopt;
            }
            try {
                const ndn::Name parsed(uri);
                if (parsed.empty() || parsed.toUri(ndn::name::UriFormat::CANONICAL) != uri) {
                    return std::nullopt;
                }
                return parsed;
            }
            catch (const std::exception&) {
                return std::nullopt;
            }
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
                if (name.get(i) == ndn::name::Component("NDNSF") &&
                    name.get(i + 1) == ndn::name::Component(messageType)) {
                    return i;
                }
            }
            return std::nullopt;
        }

        // Native DI uses a structured multi-component request identity
        // (/NDNSF/DI/REQUEST/<counter>) while legacy callers use one component.
        // The V2 name format has no length field for requestId, so recognize
        // this reserved suffix before falling back to the legacy final
        // component split.
        std::optional<size_t>
        findStructuredRequestIdStart(const ndn::Name& name, size_t begin)
        {
            if (name.size() < begin + 4) {
                return std::nullopt;
            }
            for (size_t i = name.size() - 3; i >= begin; --i) {
                if (name.get(i) == ndn::name::Component("NDNSF") &&
                    name.get(i + 1) == ndn::name::Component("DI") &&
                    name.get(i + 2) == ndn::name::Component("REQUEST") &&
                    i + 3 < name.size()) {
                    return i;
                }
                if (i == begin) {
                    break;
                }
            }
            return std::nullopt;
        }

        size_t
        requestIdStartOrFinal(const ndn::Name& name, size_t begin)
        {
            return findStructuredRequestIdStart(name, begin).value_or(name.size() - 1);
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

    std::optional<ndn::Name> parseSvsProducerActor(const ndn::Name& node,
                                                   const std::string& role)
    {
        if (node.size() < 3 || node[-2] != ndn::name::Component(role) ||
            node[-1].type() != ndn::tlv::GenericNameComponent) return std::nullopt;
        const auto& session = node[-1];
        if (session.value_size() == 0 || session.value_size() > 20) return std::nullopt;
        for (size_t i = 0; i < session.value_size(); ++i)
            if (session.value()[i] < '0' || session.value()[i] > '9') return std::nullopt;
        return node.getPrefix(-2);
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
        const size_t requestIdStart = requestIdStartOrFinal(requestName, index);
        if (requestIdStart <= index) {
            return std::nullopt;
        }

        return RequestNameV2{
            getSubNameByComponentCount(requestName, 0, *marker),
            getSubNameByComponentCount(requestName, index, requestIdStart - index),
            getSubNameByComponentCount(requestName, requestIdStart,
                                        requestName.size() - requestIdStart)};
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

    ndn::Name stripTrailingResponseSegments(const ndn::Name& responseName)
    {
        std::size_t componentCount = responseName.size();
        while (componentCount > 0 &&
               (responseName[componentCount - 1].isSegment() ||
                responseName[componentCount - 1].isVersion())) {
            --componentCount;
        }
        return componentCount == responseName.size()
          ? responseName : responseName.getPrefix(componentCount);
    }

    std::optional<ResponseNameV2> parseResponseNameV2(const ndn::Name& responseName)
    {
        const auto baseName = stripTrailingResponseSegments(responseName);
        auto marker = findNdnsfMessageMarker(baseName, "RESPONSE");
        if (!marker) {
            return std::nullopt;
        }

        const size_t index = *marker + 2;
        if (index + 3 > baseName.size()) {
            return std::nullopt;
        }
        auto requesterName = parseNameUriComponent(baseName, index);
        if (!requesterName) {
            return std::nullopt;
        }
        const size_t serviceIndex = index + 1;
        const size_t requestIdStart = requestIdStartOrFinal(baseName, serviceIndex);
        if (requestIdStart <= serviceIndex) {
            return std::nullopt;
        }

        return ResponseNameV2{
            getSubNameByComponentCount(baseName, 0, *marker),
            *requesterName,
            getSubNameByComponentCount(baseName, serviceIndex, requestIdStart - serviceIndex),
            getSubNameByComponentCount(baseName, requestIdStart,
                                        baseName.size() - requestIdStart)};
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
        const size_t requestIdStart = requestIdStartOrFinal(requestAckName, serviceIndex);
        if (requestIdStart <= serviceIndex) {
            return std::nullopt;
        }

        return RequestAckNameV2{
            getSubNameByComponentCount(requestAckName, 0, *marker),
            *requesterName,
            getSubNameByComponentCount(requestAckName, serviceIndex,
                                        requestIdStart - serviceIndex),
            getSubNameByComponentCount(requestAckName, requestIdStart,
                                        requestAckName.size() - requestIdStart)};
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
        // R1 appends a positive attempt after requestId. Preserve the legacy
        // parsed view for generic crypto/routing callers; the dedicated parser
        // retains the attempt for authority checks.
        if (serviceSelectionName.size() >= 2) {
            const auto attempt = serviceSelectionName[-1].toUri();
            const auto previous = serviceSelectionName[-2].toUri();
            const auto isNumeric = [] (const std::string& value) {
                return !value.empty() && std::all_of(value.begin(), value.end(), [] (char ch) {
                    return ch >= '0' && ch <= '9';
                });
            };
            if (!attempt.empty() &&
                isNumeric(attempt) && attempt != "0" && isNumeric(previous)) {
                const auto marker = findNdnsfMessageMarker(serviceSelectionName, "SELECTION");
                const auto structured = marker ?
                    findStructuredRequestIdStart(serviceSelectionName, *marker + 3) :
                    std::nullopt;
                // A structured request id itself ends in a numeric counter;
                // only a second numeric component is the Selection attempt.
                if (!structured || serviceSelectionName.size() - (*structured + 3) >= 2) {
                    return parseServiceSelectionNameV2(serviceSelectionName.getPrefix(-1));
                }
            }
        }
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
        const size_t requestIdStart = requestIdStartOrFinal(serviceSelectionName, serviceIndex);
        if (requestIdStart <= serviceIndex) {
            return std::nullopt;
        }

        return ServiceSelectionNameV2{
            getSubNameByComponentCount(serviceSelectionName, 0, *marker),
            *providerName,
            getSubNameByComponentCount(serviceSelectionName, serviceIndex,
                                        requestIdStart - serviceIndex),
            getSubNameByComponentCount(serviceSelectionName, requestIdStart,
                                        serviceSelectionName.size() - requestIdStart)};
    }

    ndn::Name makeServiceSelectionDecisionNameV2(const ndn::Name& requesterName,
                                                  const ndn::Name& providerName,
                                                  const ndn::Name& serviceName,
                                                  const ndn::Name& requestId,
                                                  uint64_t attempt)
    {
        if (attempt == 0) throw std::invalid_argument("Selection attempt must be positive");
        auto name = makeServiceSelectionNameV2(requesterName, providerName,
                                               serviceName, requestId);
        name.append(std::to_string(attempt));
        return name;
    }

    ndn::Name makeServiceSelectionDecisionNameWithoutPrefixV2(
                                                  const ndn::Name& providerName,
                                                  const ndn::Name& serviceName,
                                                  const ndn::Name& requestId,
                                                  uint64_t attempt)
    {
        if (attempt == 0) throw std::invalid_argument("Selection attempt must be positive");
        auto name = makeServiceSelectionNameWithoutPrefixV2(providerName,
                                                             serviceName, requestId);
        name.append(std::to_string(attempt));
        return name;
    }

    std::optional<ServiceSelectionDecisionNameV2>
    parseServiceSelectionDecisionNameV2(const ndn::Name& name)
    {
        if (name.size() < 2) return std::nullopt;
        uint64_t attempt = 0;
        try {
            const auto text = name[-1].toUri();
            size_t used = 0;
            attempt = std::stoull(text, &used);
            if (attempt == 0 || used != text.size()) return std::nullopt;
        }
        catch (const std::exception&) {
            return std::nullopt;
        }
        // A structured Native DI request identity ends in a numeric counter.
        // Treat the final numeric component as a Selection attempt only when
        // a second component follows that structured identity.
        const auto marker = findNdnsfMessageMarker(name, "SELECTION");
        if (marker) {
            const auto structured = findStructuredRequestIdStart(name, *marker + 3);
            // Legacy request IDs may be arbitrary components (for example
            // "request-1"); preserve their final numeric decision suffix.
            // A structured native request identity is a decision only when
            // exactly one decision component follows its numeric counter.
            if (structured && name.size() - (*structured + 3) != 2) {
                return std::nullopt;
            }
        }
        const auto base = name.getPrefix(-1);
        auto parsed = parseServiceSelectionNameV2(base);
        if (!parsed) return std::nullopt;
        return ServiceSelectionDecisionNameV2{parsed->requesterName,
                                              parsed->providerName,
                                              parsed->serviceName,
                                              parsed->requestId,
                                              attempt};
    }

    // Context fixes the actor and service boundaries. All remaining components
    // belong to the request identity; reserved words inside either Name are data.
    namespace {
    std::optional<size_t> contextualRequestStart(const ndn::Name& name,
                                                const ndn::Name& actor,
                                                const ndn::Name& service,
                                                const char* category,
                                                bool recipient)
    {
        if (actor.empty() || service.empty() || !actor.isPrefixOf(name)) return std::nullopt;
        const size_t marker = actor.size();
        const size_t serviceStart = marker + 2 + (recipient ? 1 : 0);
        const size_t requestStart = serviceStart + service.size();
        if (name.size() <= requestStart ||
            name[marker] != ndn::name::Component("NDNSF") ||
            name[marker + 1] != ndn::name::Component(category) ||
            name.getSubName(serviceStart, service.size()) != service) return std::nullopt;
        return requestStart;
    }
    }

    std::optional<RequestNameV2> parseRequestNameV2(
        const ndn::Name& name, const ndn::Name& actor, const ndn::Name& service)
    {
        const auto start = contextualRequestStart(name, actor, service, "REQUEST", false);
        if (!start) return std::nullopt;
        return RequestNameV2{actor, service, name.getSubName(*start)};
    }

    std::optional<ResponseNameV2> parseResponseNameV2(
        const ndn::Name& name, const ndn::Name& actor, const ndn::Name& service)
    {
        const auto start = contextualRequestStart(name, actor, service, "RESPONSE", true);
        if (!start) return std::nullopt;
        const auto recipient = parseNameUriComponent(name, actor.size() + 2);
        if (!recipient) return std::nullopt;
        return ResponseNameV2{actor, *recipient, service, name.getSubName(*start)};
    }

    std::optional<RequestAckNameV2> parseRequestAckNameV2(
        const ndn::Name& name, const ndn::Name& actor, const ndn::Name& service)
    {
        const auto start = contextualRequestStart(name, actor, service, "ACK", true);
        if (!start) return std::nullopt;
        const auto recipient = parseNameUriComponent(name, actor.size() + 2);
        if (!recipient) return std::nullopt;
        return RequestAckNameV2{actor, *recipient, service, name.getSubName(*start)};
    }

    std::optional<ServiceSelectionNameV2> parseServiceSelectionNameV2(
        const ndn::Name& name, const ndn::Name& actor, const ndn::Name& service)
    {
        const auto start = contextualRequestStart(name, actor, service, "SELECTION", true);
        if (!start) return std::nullopt;
        const auto recipient = parseNameUriComponent(name, actor.size() + 2);
        if (!recipient) return std::nullopt;
        return ServiceSelectionNameV2{actor, *recipient, service, name.getSubName(*start)};
    }

    std::optional<ServiceSelectionDecisionNameV2> parseServiceSelectionDecisionNameV2(
        const ndn::Name& name, const ndn::Name& actor, const ndn::Name& service)
    {
        if (name.empty()) return std::nullopt;
        const auto& component = name[-1];
        if (component.type() != ndn::tlv::GenericNameComponent) return std::nullopt;
        const std::string text(reinterpret_cast<const char*>(component.value()), component.value_size());
        if (text.empty() || text.front() == '0' ||
            !std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; }))
            return std::nullopt;
        uint64_t attempt;
        try { attempt = std::stoull(text); }
        catch (const std::exception&) { return std::nullopt; }
        const auto parsed = parseServiceSelectionNameV2(name.getPrefix(-1), actor, service);
        if (!parsed) return std::nullopt;
        return ServiceSelectionDecisionNameV2{actor, parsed->providerName, service,
                                               parsed->requestId, attempt};
    }

    ndn::Name makeCompactServiceSelectionNameV2(const ndn::Name& requesterName,
                                                const ndn::Name& serviceName,
                                                const ndn::Name& requestId)
    {
        ndn::Name serviceSelectionName;
        serviceSelectionName.append(requesterName).append(ndn::Name("/NDNSF/SELECTION"));
        serviceSelectionName.append(serviceName);
        serviceSelectionName.append(requestId);
        return serviceSelectionName;
    }

    ndn::Name makeCompactServiceSelectionNameWithoutPrefixV2(const ndn::Name& serviceName,
                                                             const ndn::Name& requestId)
    {
        ndn::Name serviceSelectionName;
        serviceSelectionName.append(ndn::Name("/NDNSF/SELECTION"));
        serviceSelectionName.append(serviceName);
        serviceSelectionName.append(requestId);
        return serviceSelectionName;
    }

    std::optional<CompactServiceSelectionNameV2>
    parseCompactServiceSelectionNameV2(const ndn::Name& serviceSelectionName)
    {
        if (parseServiceSelectionDecisionNameV2(serviceSelectionName)) {
            return std::nullopt;
        }
        auto marker = findNdnsfMessageMarker(serviceSelectionName, "SELECTION");
        if (!marker) {
            return std::nullopt;
        }
        size_t serviceIndex = *marker + 2;
        if (serviceIndex + 2 > serviceSelectionName.size()) {
            return std::nullopt;
        }

        // Transitional compatibility: an earlier compact-selection prototype
        // used an encoded /NDNSF/COMPACT sentinel in the provider slot. New V2
        // selection names use the unified shape
        // /<requester>/NDNSF/SELECTION/<service...>/<requestId>.
        const auto compactMarker = parseNameUriComponent(serviceSelectionName, serviceIndex);
        if (compactMarker && *compactMarker == ndn::Name("/NDNSF/COMPACT")) {
            ++serviceIndex;
        }
        else if (parseServiceSelectionNameV2(serviceSelectionName)) {
            // Provider-bound and compact Selection names share the same V2
            // prefix.  The former is distinguished by its first post-marker
            // component being an encoded provider Name.  Reject that shape
            // here so callers cannot silently treat the provider component as
            // the first component of serviceName.
            return std::nullopt;
        }
        if (serviceIndex + 2 > serviceSelectionName.size()) {
            return std::nullopt;
        }
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

    ndn::Block
    encodeSelectionStatusQueryParameters(const ndn::Name& service, const std::string& digest)
    {
        if (service.empty() || digest.empty()) throw std::invalid_argument("empty selection status scope");
        ndn::Name carrier;
        carrier.append("SELECTION-STATUS-QUERY").append(makeReversibleNameComponent(service))
            .append(ndn::name::Component(ndn::span<const uint8_t>(reinterpret_cast<const uint8_t*>(digest.data()), digest.size())));
        checkNamingNameSize(carrier);
        return carrier.wireEncode();
    }

    std::optional<SelectionStatusQueryName>
    parseSelectionStatusQueryName(const ndn::Interest& interest, const ndn::Name& producer)
    {
        if (!parseControlRpcName(interest, producer, "SELECTION-STATUS")) return std::nullopt;
        try {
            auto block = interest.getApplicationParameters();
            block.parse();
            if (block.elements().size() != 1 || block.elements().front().type() != ndn::tlv::Name)
                return std::nullopt;
            block = block.elements().front();
            const ndn::Name carrier(block);
            if (carrier.size() != 3 || carrier[0] != ndn::name::Component("SELECTION-STATUS-QUERY") ||
                carrier[2].type() != 8) return std::nullopt;
            const auto service = parseReversibleNameComponent(carrier[1]);
            if (!service || service->empty()) return std::nullopt;
            const std::string digest(reinterpret_cast<const char*>(carrier[2].value()), carrier[2].value_size());
            if (!(encodeSelectionStatusQueryParameters(*service, digest) == block)) return std::nullopt;
            return SelectionStatusQueryName{producer, *service, digest};
        }
        catch (const std::exception&) { return std::nullopt; }
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

    std::string
    computeSelectionDigestWithoutKeyEnvelope(
        const ServiceSelectionMessage& message)
    {
        ServiceSelectionMessage withoutEnvelope(message);
        withoutEnvelope.clearSelectionKeyEnvelope();
        // RequestSecurityBinding carries typed SHA-256 digests as
        // ``sha256:<hex>``.  The legacy selection-status paths intentionally
        // keep the bare hexadecimal digest, so canonicalize only this
        // confidentiality-binding helper instead of changing those wire
        // identifiers.
        const auto digest = computeSelectionDigest(withoutEnvelope);
        if (digest.compare(0, 7, "sha256:") == 0) {
            return digest;
        }
        return "sha256:" + digest;
    }

    std::string
    makeOpaqueControlHandle(size_t bytes)
    {
        if (bytes < 16 || bytes > 64) {
            throw std::invalid_argument("opaque control handle must contain 16..64 random bytes");
        }
        std::vector<uint8_t> random(bytes);
        if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1) {
            throw std::runtime_error("RAND_bytes failed for opaque control handle");
        }
        std::ostringstream os;
        os << std::hex << std::setfill('0');
        for (const auto byte : random) os << std::setw(2) << unsigned(byte);
        return os.str();
    }

    bool
    isValidOpaqueControlHandle(const std::string& handle)
    {
        if (handle.size() < 32 || handle.size() > 128 || handle.size() % 2 != 0) return false;
        return std::all_of(handle.begin(), handle.end(), [] (unsigned char ch) {
            return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
        });
    }

    namespace {
    template<typename Parsed>
    std::optional<Parsed>
    parseOpaqueControlName(const ndn::Name& name, const char* markerText)
    {
        auto marker = findNdnsfMessageMarker(name, markerText);
        if (!marker || name.size() != *marker + 4) return std::nullopt;
        const auto versionText = name.get(*marker + 2).toUri();
        uint64_t version = 0;
        if (versionText.empty()) return std::nullopt;
        for (char ch : versionText) {
            if (ch < '0' || ch > '9') return std::nullopt;
            version = version * 10 + static_cast<uint64_t>(ch - '0');
        }
        const auto handle = name.get(*marker + 3).toUri();
        if (version != DeploymentControlMessage::VERSION || !isValidOpaqueControlHandle(handle)) {
            return std::nullopt;
        }
        return Parsed{getSubNameByComponentCount(name, 0, *marker), version, handle};
    }

    ndn::Name
    makeOpaqueControlName(const ndn::Name& prefix, const char* marker,
                          uint64_t version, const std::string& handle)
    {
        if (version != DeploymentControlMessage::VERSION || !isValidOpaqueControlHandle(handle)) {
            throw std::invalid_argument("invalid opaque control name arguments");
        }
        ndn::Name name(prefix);
        name.append("NDNSF").append(marker).append(std::to_string(version)).append(handle);
        return name;
    }
    } // namespace

    ndn::Name makeSecureSelectionStatusName(const ndn::Name& providerName,
                                             uint64_t version,
                                             const std::string& statusHandle)
    {
        return makeOpaqueControlName(providerName, "SELECTION-STATUS", version, statusHandle);
    }

    std::optional<SecureSelectionStatusName>
    parseSecureSelectionStatusName(const ndn::Name& name)
    {
        return parseOpaqueControlName<SecureSelectionStatusName>(name, "SELECTION-STATUS");
    }

    namespace {
    template<typename Parsed>
    std::optional<Parsed>
    parseContextualControlName(const ndn::Name& name, const ndn::Name& actor,
                               const char* operation)
    {
        const auto parsed = parseProducerDataName(name, actor);
        if (!parsed || parsed->category != "RPC" || parsed->suffix.size() != 4 ||
            parsed->suffix[0] != ndn::name::Component(operation) ||
            parsed->suffix[1].type() != 8 || parsed->suffix[1].value_size() != 32 ||
            parsed->suffix[2].type() != 8 || !parsed->suffix[3].isParametersSha256Digest())
            return std::nullopt;
        const auto& component = parsed->suffix[2];
        const std::string handle(reinterpret_cast<const char*>(component.value()), component.value_size());
        if (!isValidOpaqueControlHandle(handle)) return std::nullopt;
        return Parsed{actor, DeploymentControlMessage::VERSION, handle};
    }
    }

    std::optional<ProviderReadyName>
    parseProviderReadyName(const ndn::Name& name, const ndn::Name& requester)
    {
        return parseContextualControlName<ProviderReadyName>(name, requester, "PROVIDER-READY");
    }

    std::optional<ExecutionActivateName>
    parseExecutionActivateName(const ndn::Name& name, const ndn::Name& provider)
    {
        return parseContextualControlName<ExecutionActivateName>(name, provider, "EXECUTION-ACTIVATE");
    }

    ndn::Name makeProviderReadyName(const ndn::Name& requesterName,
                                     uint64_t version,
                                     const std::string& controlHandle)
    {
        return makeOpaqueControlName(requesterName, "PROVIDER-READY", version, controlHandle);
    }

    std::optional<ProviderReadyName> parseProviderReadyName(const ndn::Name& name)
    {
        return parseOpaqueControlName<ProviderReadyName>(name, "PROVIDER-READY");
    }

    ndn::Name makeExecutionActivateName(const ndn::Name& providerName,
                                         uint64_t version,
                                         const std::string& controlHandle)
    {
        return makeOpaqueControlName(providerName, "EXECUTION-ACTIVATE", version, controlHandle);
    }

    std::optional<ExecutionActivateName> parseExecutionActivateName(const ndn::Name& name)
    {
        return parseOpaqueControlName<ExecutionActivateName>(name, "EXECUTION-ACTIVATE");
    }

    ndn::Name makeCollaborationDataName(const ndn::Name& producerName,
                                        const ndn::Name& requesterName,
                                        const ndn::Name& requestId,
                                        const std::string& keyScope,
                                        const ndn::Name& topic,
                                        uint64_t sequence)
    {
        ndn::Name name = makeProducerDataPrefix(producerName, "COLLAB");
        appendCountedName(name, requesterName);
        appendCollaborationRequestId(name, requestId);
        name.append(keyScope);
        name.append(std::to_string(topic.size()));
        name.append(topic);
        name.append(std::to_string(sequence));
        checkNamingNameSize(name);
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

        ndn::Name requestId;
        if (auto encodedRequestId = parseNameUriComponent(collaborationDataName,
                                                          index);
            encodedRequestId && encodedRequestId->size() > 1) {
            requestId = *encodedRequestId;
            ++index;
        }
        else {
            // Accept names emitted by older builds, which appended a
            // structured request identity component-by-component and had no
            // requestId length field. The topic-count component is the only
            // unambiguous boundary from the end of the name; choose it after
            // the reserved NDNSF/DI/REQUEST marker and retain the legacy
            // single-component fallback for all other request identities.
            const auto structured = findStructuredRequestIdStart(
                collaborationDataName, index);
            if (structured) {
                for (size_t topicCount = *structured + 3;
                     topicCount + 1 < collaborationDataName.size(); ++topicCount) {
                    const auto count = parseComponentCount(collaborationDataName,
                                                           topicCount);
                    if (!count || topicCount + 1 + *count !=
                                      collaborationDataName.size() - 1 ||
                        topicCount <= index) {
                        continue;
                    }
                    const auto requestEnd = topicCount - 1;
                    if (requestEnd <= index) {
                        continue;
                    }
                    requestId = getSubNameByComponentCount(
                        collaborationDataName, index, requestEnd - index);
                    index = requestEnd;
                    break;
                }
            }
            if (requestId.empty()) {
                if (index >= collaborationDataName.size()) {
                    return std::nullopt;
                }
                requestId = ndn::Name(collaborationDataName.get(index++).toUri());
            }
        }
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

    std::optional<CollaborationDataName>
    parseCollaborationDataName(const ndn::Name& name, const ndn::Name& producer)
    {
        try {
            checkNamingNameSize(name);
            const auto root = makeProducerDataPrefix(producer, "COLLAB");
            if (!root.isPrefixOf(name)) return std::nullopt;
            size_t index = root.size();
            const auto number = [&]() -> uint64_t {
                if (index >= name.size()) throw std::invalid_argument("missing COLLAB number");
                const auto& c = name[index++];
                if (c.type() != ndn::tlv::GenericNameComponent)
                    throw std::invalid_argument("invalid COLLAB number type");
                const std::string text(reinterpret_cast<const char*>(c.value()), c.value_size());
                const auto value = std::stoull(text);
                if (c != makeNamingDecimalComponent(value))
                    throw std::invalid_argument("noncanonical COLLAB number");
                return value;
            };
            const auto counted = [&]() {
                const auto count = number();
                if (count > name.size() - index) throw std::invalid_argument("COLLAB count overflow");
                auto value = name.getSubName(index, static_cast<size_t>(count));
                index += static_cast<size_t>(count);
                return value;
            };
            const auto requester = counted();
            if (requester.empty() || index >= name.size()) return std::nullopt;
            ndn::Name request;
            if (const auto decoded = parseNameUriComponent(name, index);
                decoded && decoded->size() > 1) request = *decoded;
            else request.append(name[index]);
            ++index;
            if (index >= name.size() || name[index].type() != ndn::tlv::GenericNameComponent)
                return std::nullopt;
            const auto& scopeComponent = name[index++];
            const std::string scope(reinterpret_cast<const char*>(scopeComponent.value()), scopeComponent.value_size());
            const auto topic = counted();
            const auto sequence = number();
            if (index != name.size() ||
                makeCollaborationDataName(producer, requester, request, scope, topic, sequence) != name)
                return std::nullopt;
            return CollaborationDataName{producer, requester, request, scope, topic, sequence};
        }
        catch (const std::exception&) { return std::nullopt; }
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
           << (reference.keyScope.empty() ? std::string() :
               "key_scope=" + sanitizeReferenceField(reference.keyScope) + "\n")
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
            else if (key == "key_scope") {
                reference.keyScope = value;
                // Do not silently fall back to the legacy service-wide key
                // when a producer advertises an unknown scope.  An unknown
                // scope is ambiguous and must fail closed at the reference
                // boundary.
                if (reference.keyScope != "request") {
                    return std::nullopt;
                }
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

    std::shared_ptr<ndn::Buffer> CombineSegmentsIntoBuffer(ndn::nacabe::SPtrVector<ndn::Data> segments)
    {
        ndn::OBufferStream buf;
        for (auto segment : segments)
        {
            buf.write(reinterpret_cast<const char *>(segment->getContent().data()), segment->getContent().size());
        }
        return buf.buf();
    }

    std::string NameToRegexString(ndn::Name& name)
    {
        return NameToRegexString(static_cast<const ndn::Name&>(name));
    }

    std::string NameToRegexString(const ndn::Name& name){
        std::string tmp = "";
        for(int i= 0;i<(int)name.size();i++)
       
        {
            std::string component;
            for (const char c : name[i].toUri()) {
                if (std::string("\\.^$|()[]{}*+?").find(c) != std::string::npos) component += '\\';
                component += c;
            }
            tmp += "<" + component + ">";
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
        if (auto decisionSelectionV2 = parseServiceSelectionDecisionNameV2(name)) {
            return std::vector<std::string>{"/SERVICE" + decisionSelectionV2->serviceName.toUri()};
        }
        if (auto compactSelectionV2 = parseCompactServiceSelectionNameV2(name)) {
            return std::vector<std::string>{"/SERVICE" + compactSelectionV2->serviceName.toUri()};
        }
        if (auto selectionV2 = parseServiceSelectionNameV2(name)) {
            return std::vector<std::string>{"/SERVICE" + selectionV2->serviceName.toUri()};
        }

        return std::nullopt;
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

namespace ndn_service_framework {

void
registerInterestFilterWithRetry(
    ndn::Face& face,
    const ndn::Name& prefix,
    std::function<void(const ndn::InterestFilter&, const ndn::Interest&)> onInterest,
    std::function<void(const ndn::Name&)> onSuccess,
    std::function<void(const ndn::Name&, const std::string&)> onFailure,
    size_t attempts,
    std::chrono::milliseconds delayBetweenAttempts,
    std::shared_ptr<ndn::ScopedRegisteredPrefixHandle> registrationHolder)
{
    if (attempts == 0) {
        if (onFailure) {
            onFailure(prefix, "registration retry budget exhausted");
        }
        return;
    }
    // The interest handler must stay identical across attempts.  Passing a
    // moved-from std::function on a retry would register an empty filter.
    auto interestHandler = std::make_shared<
        std::function<void(const ndn::InterestFilter&, const ndn::Interest&)>>(
        std::move(onInterest));
    // A reference parameter is a local alias whose lifetime ends when this
    // function returns.  Capturing it with [&face] would leave every async
    // failure/timer callback with a dangling reference.  Keep the referred
    // Face stable through an explicit pointer instead.
    ndn::Face* facePtr = &face;
    // A RegisteredPrefixHandle unregisters on destruction.  Keep it in the
    // caller-owned holder; discarding the temporary would race registration
    // with immediate unregistration during the async command exchange.
    if (!registrationHolder) {
        throw std::invalid_argument("registration retry requires a handle holder");
    }
    const std::weak_ptr<ndn::ScopedRegisteredPrefixHandle> registrationOwner =
        registrationHolder;
    *registrationHolder = face.setInterestFilter(
        prefix,
        [interestHandler](const ndn::InterestFilter& filter,
                          const ndn::Interest& interest) {
            (*interestHandler)(filter, interest);
        },
        [onSuccess](const ndn::Name& registered) {
            if (onSuccess) {
                onSuccess(registered);
            }
        },
        [facePtr, prefix, interestHandler, onSuccess, onFailure,
         registrationOwner,
         attempts, delayBetweenAttempts](
            const ndn::Name& failed, const std::string& reason) {
            (void)failed;
            (void)reason;
            if (registrationOwner.expired()) {
                return;
            }
            auto timer = std::make_shared<boost::asio::steady_timer>(
                facePtr->getIoContext());
            timer->expires_after(delayBetweenAttempts);
            timer->async_wait(
                [facePtr, prefix, interestHandler,
                 onSuccess, onFailure, attempts, delayBetweenAttempts,
                 registrationOwner, timer](const boost::system::error_code& ec) {
                    if (ec) {
                        return;
                    }
                    auto registrationHolder = registrationOwner.lock();
                    if (!registrationHolder) {
                        return;
                    }
                    registerInterestFilterWithRetry(
                        *facePtr, prefix, *interestHandler, onSuccess, onFailure,
                        attempts - 1, delayBetweenAttempts,
                        std::move(registrationHolder));
                });
        });
}

} // namespace ndn_service_framework
