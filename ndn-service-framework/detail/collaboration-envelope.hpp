#ifndef NDN_SERVICE_FRAMEWORK_DETAIL_COLLABORATION_ENVELOPE_HPP
#define NDN_SERVICE_FRAMEWORK_DETAIL_COLLABORATION_ENVELOPE_HPP
#include "../HybridMessageCrypto.hpp"
#include <optional>

namespace ndn_service_framework::detail {
ndn::Name makeCollaborationObjectGrantName(
    const ndn::Name& producer, const ndn::Name& service, const std::string& scope,
    const ndn::Name& object, const ndn::Name& request);

std::optional<ndn::Buffer> decryptCollaborationEnvelope(
    const ndn::Name& object, const ndn::Name& request, const std::string& scope,
    const ndn::Buffer& key, ndn::ConstBufferPtr encoded);

std::optional<ndn::Buffer> decryptReusableCollaborationEnvelope(
    const ndn::Name& object, const ndn::Name& service, const std::string& scope,
    const ndn::Buffer& key, ndn::ConstBufferPtr encoded);
}
#endif
