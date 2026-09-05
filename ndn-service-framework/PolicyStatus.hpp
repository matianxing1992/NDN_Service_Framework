#ifndef NDN_SERVICE_FRAMEWORK_POLICY_STATUS_HPP
#define NDN_SERVICE_FRAMEWORK_POLICY_STATUS_HPP

#include "ControllerVersion.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace ndn_service_framework {

enum class RevocationKind : uint8_t
{
  IDENTITY = 1,
  CERTIFICATE = 2,
  SERVICE_AUTHORIZATION = 3,
};

struct RevocationTarget
{
  RevocationKind kind = RevocationKind::IDENTITY;
  ndn::Name targetIdentity;
  ndn::Name serviceName;
  std::string certificateDigest;
  // Exact NAC-ABE authorization attribute for service-scoped targets:
  // /PERMISSION/<service> (User use) or /SERVICE/<service> (Provider provision).
  ndn::Name authorizationAttribute;

  bool isValid() const;
  ndn::Block wireEncode() const;
  bool wireDecode(const ndn::Block& block);
};

/** Controller-signed, immutable authorization status snapshot.
 *
 * This class validates the canonical structure and validity interval.  The
 * caller remains responsible for authenticating the enclosing Data signature.
 */
class PolicyStatusData
{
public:
  static constexpr uint32_t TYPE = 0xF710;
  static constexpr uint32_t ServiceNameType = 0xF711;
  static constexpr uint32_t ControllerVersionType = 0xF712;
  static constexpr uint32_t ValidFromType = 0xF713;
  static constexpr uint32_t ValidUntilType = 0xF714;
  static constexpr uint32_t PolicyDigestType = 0xF715;
  static constexpr uint32_t AbePublicParametersNameType = 0xF71E;
  static constexpr uint32_t AbePublicParametersDigestType = 0xF71F;
  static constexpr uint32_t ControllerCertificateType = 0xF716;
  static constexpr uint32_t SignatureType = 0xF717;
  static constexpr uint32_t RevocationTargetType = 0xF718;

  void setServiceName(const ndn::Name& serviceName);
  const ndn::Name& getServiceName() const;
  void setControllerVersion(const ControllerVersion& version);
  const ControllerVersion& getControllerVersion() const;
  void setValidity(uint64_t validFromMs, uint64_t validUntilMs);
  uint64_t getValidFromMs() const;
  uint64_t getValidUntilMs() const;
  void setPolicyDigest(const std::string& digest);
  const std::string& getPolicyDigest() const;
  /** Exact immutable NAC-ABE public-parameter Data name for this status. */
  void setAbePublicParametersName(const ndn::Name& name);
  const ndn::Name& getAbePublicParametersName() const;
  /** SHA-256 digest of the canonical public-parameter Data Content. */
  void setAbePublicParametersDigest(const std::string& digest);
  const std::string& getAbePublicParametersDigest() const;
  void setControllerCertificate(const ndn::Name& certificate);
  const ndn::Name& getControllerCertificate() const;
  void setSignature(const ndn::Buffer& signature);
  const ndn::Buffer& getSignature() const;
  void addRevocation(const RevocationTarget& target);
  const std::vector<RevocationTarget>& getRevocations() const;

  bool validate(uint64_t nowMs) const;
  ndn::Block wireEncode() const;
  bool wireDecode(const ndn::Block& block);

private:
  ndn::Name serviceName_;
  ControllerVersion controllerVersion_;
  uint64_t validFromMs_ = 0;
  uint64_t validUntilMs_ = 0;
  std::string policyDigest_;
  ndn::Name abePublicParametersName_;
  std::string abePublicParametersDigest_;
  ndn::Name controllerCertificate_;
  ndn::Buffer signature_;
  std::vector<RevocationTarget> revocations_;
};

} // namespace ndn_service_framework

#endif // NDN_SERVICE_FRAMEWORK_POLICY_STATUS_HPP
