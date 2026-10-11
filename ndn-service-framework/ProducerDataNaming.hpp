#ifndef NDN_SERVICE_FRAMEWORK_PRODUCER_DATA_NAMING_HPP
#define NDN_SERVICE_FRAMEWORK_PRODUCER_DATA_NAMING_HPP

#include <ndn-cxx/name.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ndn_service_framework {

struct ParsedProducerDataName {
  ndn::Name producer;
  std::string category;
  ndn::Name suffix;
  std::string nameSpace = "NDNSF";
};

enum class DataNameReadMode { StrictCanonical, AllowLegacyRead };

/** Tagged digest transcript. Kinds: NameWire=1, UTF8=2, UInt64=3,
 * Bytes=4, CanonicalJSON=5. JSON canonicalization belongs to its owner. */
struct NamingDigestField {
  std::uint16_t tag;
  std::uint8_t kind;
  std::vector<std::uint8_t> value;
};

ndn::Name makeProducerDataPrefix(const ndn::Name& producer,
                                const std::string& category);
/** Explicit business namespace; categories are validated independently of labels. */
ndn::Name makeProducerDataPrefix(const ndn::Name& producer,
                                const std::string& nameSpace,
                                const std::string& category);
/** One Generic component. Preserve UTF-8 characters when shortening to 64 bytes.
 * The owner MUST also bind the complete original label in its semantic identity. */
ndn::name::Component makeNamingLabelComponent(const std::string& fullLabel);
bool matchesNamingLabelComponent(const ndn::name::Component& component,
                                 const std::string& fullLabel);
ndn::name::Component makeNamingDigestComponent(const std::string& sha256Hex);
ndn::name::Component makeNamingDecimalComponent(std::uint64_t value);
std::string computeNamingDigest(const std::string& domain,
                               const std::vector<NamingDigestField>& orderedFields);
ndn::name::Component makeReversibleNameComponent(const ndn::Name& name);
std::optional<ndn::Name> parseReversibleNameComponent(const ndn::name::Component& component);
/** expectedProducer must come from trusted context. This parses the envelope;
 * the owner must validate the category's suffix and signer authorization. */
std::optional<ParsedProducerDataName> parseProducerDataName(
  const ndn::Name& name, const ndn::Name& expectedProducer);
void checkNamingNameSize(const ndn::Name& name, std::size_t maxNameWireBytes = 1024);

/** For independent RPC replies only; paper invocation messages retain utils.hpp builders. */
ndn::Name makeProducerRpcName(const ndn::Name& replyProducer,
                              const std::string& operation,
                              const std::string& callIdentityDigest,
                              const std::string& queryNonce);

} // namespace ndn_service_framework
#endif
