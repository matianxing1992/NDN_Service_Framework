#include "PolicyStatus.hpp"

#include <algorithm>
#include <stdexcept>

namespace ndn_service_framework {
namespace {

bool
isKnownRevocationKind(uint64_t value)
{
  return value >= static_cast<uint64_t>(RevocationKind::IDENTITY) &&
         value <= static_cast<uint64_t>(RevocationKind::SERVICE_AUTHORIZATION);
}

bool
isSha256Digest(const std::string& value)
{
  return value.size() == 7 + 64 && value.compare(0, 7, "sha256:") == 0 &&
         std::all_of(value.begin() + 7, value.end(), [] (char ch) {
           return (ch >= '0' && ch <= '9') ||
                  (ch >= 'a' && ch <= 'f') ||
                  (ch >= 'A' && ch <= 'F');
         });
}

bool
isCanonicalAuthorizationAttribute(const ndn::Name& serviceName,
                                  const ndn::Name& attribute)
{
  if (serviceName.empty() || attribute.empty())
    return false;
  ndn::Name permission("/PERMISSION");
  permission.append(serviceName);
  ndn::Name service("/SERVICE");
  service.append(serviceName);
  return attribute == permission || attribute == service;
}

const ndn::Block&
take(const std::vector<ndn::Block>& elements, size_t& index, uint32_t type)
{
  if (index >= elements.size() || elements[index].type() != type)
    throw std::invalid_argument("non-canonical policy status field order");
  return elements[index++];
}

} // namespace

bool
sameCertificateDigest(const std::string& left, const std::string& right)
{
  if (left == right)
    return true;
  if (!isSha256Digest(left) || !isSha256Digest(right))
    return false;
  const auto lowerHex = [](char value) {
    return value >= 'A' && value <= 'F' ? value + ('a' - 'A') : value;
  };
  return std::equal(left.begin() + 7, left.end(), right.begin() + 7,
                    [&](char a, char b) { return lowerHex(a) == lowerHex(b); });
}

bool
RevocationTarget::isValid() const
{
  switch (kind) {
  case RevocationKind::IDENTITY:
    return !targetIdentity.empty() && serviceName.empty() &&
           certificateDigest.empty() && authorizationAttribute.empty();
  case RevocationKind::CERTIFICATE:
    return !certificateDigest.empty() && serviceName.empty() &&
           authorizationAttribute.empty();
  case RevocationKind::SERVICE_AUTHORIZATION:
    return !targetIdentity.empty() && !serviceName.empty() &&
           certificateDigest.empty() &&
           isCanonicalAuthorizationAttribute(serviceName, authorizationAttribute);
  default:
    return false;
  }
}

ndn::Block
RevocationTarget::wireEncode() const
{
  if (!isValid())
    throw std::invalid_argument("invalid RevocationTarget");
  ndn::Block block(PolicyStatusData::RevocationTargetType);
  block.push_back(ndn::makeNonNegativeIntegerBlock(
      0xF719, static_cast<uint64_t>(kind)));
  if (!targetIdentity.empty())
    block.push_back(ndn::makeStringBlock(0xF71A, targetIdentity.toUri()));
  if (!serviceName.empty())
    block.push_back(ndn::makeStringBlock(0xF71B, serviceName.toUri()));
  if (!authorizationAttribute.empty())
    block.push_back(ndn::makeStringBlock(0xF71D, authorizationAttribute.toUri()));
  if (!certificateDigest.empty())
    block.push_back(ndn::makeStringBlock(0xF71C, certificateDigest));
  block.encode();
  return block;
}

bool
RevocationTarget::wireDecode(const ndn::Block& block)
{
  try {
    if (block.type() != PolicyStatusData::RevocationTargetType)
      return false;
    block.parse();
    const auto& elements = block.elements();
    if (elements.empty() || elements[0].type() != 0xF719 || elements.size() > 5)
      return false;
    RevocationTarget decoded;
    const auto kind = ndn::readNonNegativeInteger(elements[0]);
    if (!isKnownRevocationKind(kind))
      return false;
    decoded.kind = static_cast<RevocationKind>(kind);
    bool identitySeen = false;
    bool serviceSeen = false;
    bool digestSeen = false;
    bool attributeSeen = false;
    for (size_t i = 1; i < elements.size(); ++i) {
      switch (elements[i].type()) {
      case 0xF71A:
        if (identitySeen) return false;
        decoded.targetIdentity = ndn::Name(ndn::readString(elements[i]));
        identitySeen = true;
        break;
      case 0xF71B:
        if (serviceSeen) return false;
        decoded.serviceName = ndn::Name(ndn::readString(elements[i]));
        serviceSeen = true;
        break;
      case 0xF71C:
        if (digestSeen) return false;
        decoded.certificateDigest = ndn::readString(elements[i]);
        digestSeen = true;
        break;
      case 0xF71D:
        if (attributeSeen) return false;
        decoded.authorizationAttribute = ndn::Name(ndn::readString(elements[i]));
        attributeSeen = true;
        break;
      default:
        return false;
      }
    }
    if (!decoded.isValid())
      return false;
    *this = decoded;
    return true;
  }
  catch (const std::exception&) {
    return false;
  }
}

void PolicyStatusData::setServiceName(const ndn::Name& value) { serviceName_ = value; }
const ndn::Name& PolicyStatusData::getServiceName() const { return serviceName_; }
void PolicyStatusData::setControllerVersion(const ControllerVersion& value) { controllerVersion_ = value; }
const ControllerVersion& PolicyStatusData::getControllerVersion() const { return controllerVersion_; }
void PolicyStatusData::setValidity(uint64_t from, uint64_t until) { validFromMs_ = from; validUntilMs_ = until; }
uint64_t PolicyStatusData::getValidFromMs() const { return validFromMs_; }
uint64_t PolicyStatusData::getValidUntilMs() const { return validUntilMs_; }
void PolicyStatusData::setPolicyDigest(const std::string& value) { policyDigest_ = value; }
const std::string& PolicyStatusData::getPolicyDigest() const { return policyDigest_; }
void PolicyStatusData::setAbePublicParametersName(const ndn::Name& value)
{
  abePublicParametersName_ = value;
}
const ndn::Name& PolicyStatusData::getAbePublicParametersName() const
{
  return abePublicParametersName_;
}
void PolicyStatusData::setAbePublicParametersDigest(const std::string& value)
{
  abePublicParametersDigest_ = value;
}
const std::string& PolicyStatusData::getAbePublicParametersDigest() const
{
  return abePublicParametersDigest_;
}
void PolicyStatusData::setControllerCertificate(const ndn::Name& value) { controllerCertificate_ = value; }
const ndn::Name& PolicyStatusData::getControllerCertificate() const { return controllerCertificate_; }
void PolicyStatusData::setSignature(const ndn::Buffer& value) { signature_ = value; }
const ndn::Buffer& PolicyStatusData::getSignature() const { return signature_; }
void PolicyStatusData::addRevocation(const RevocationTarget& value) { revocations_.push_back(value); }
const std::vector<RevocationTarget>& PolicyStatusData::getRevocations() const { return revocations_; }

bool
PolicyStatusData::validate(uint64_t nowMs) const
{
  if (serviceName_.empty() || !controllerVersion_.isValid() ||
      validFromMs_ >= validUntilMs_ || nowMs < validFromMs_ ||
      nowMs >= validUntilMs_ || !isSha256Digest(policyDigest_) ||
      controllerCertificate_.empty())
    return false;
  // The ABE generation identity is optional for pre-Spec179 synthetic status
  // fixtures, but it is all-or-nothing whenever present.  Controller-produced
  // status always carries both fields; a runtime must never accept a mixed
  // name/digest pair as a generation identity.
  if (abePublicParametersName_.empty() != abePublicParametersDigest_.empty())
    return false;
  if (!abePublicParametersDigest_.empty() &&
      !isSha256Digest(abePublicParametersDigest_))
    return false;
  for (size_t i = 0; i < revocations_.size(); ++i) {
    if (!revocations_[i].isValid())
      return false;
    if (revocations_[i].kind == RevocationKind::SERVICE_AUTHORIZATION &&
        revocations_[i].serviceName != serviceName_)
      return false;
    for (size_t j = 0; j < i; ++j) {
      if (revocations_[i].kind == revocations_[j].kind &&
          revocations_[i].targetIdentity == revocations_[j].targetIdentity &&
          revocations_[i].serviceName == revocations_[j].serviceName &&
          sameCertificateDigest(revocations_[i].certificateDigest,
                                revocations_[j].certificateDigest) &&
          revocations_[i].authorizationAttribute ==
              revocations_[j].authorizationAttribute)
        return false;
    }
  }
  return true;
}

ndn::Block
PolicyStatusData::wireEncode() const
{
  if (!validate(validFromMs_))
    throw std::invalid_argument("invalid PolicyStatusData");
  ndn::Block block(TYPE);
  block.push_back(ndn::makeStringBlock(ServiceNameType, serviceName_.toUri()));
  block.push_back(controllerVersion_.wireEncode());
  block.push_back(ndn::makeNonNegativeIntegerBlock(ValidFromType, validFromMs_));
  block.push_back(ndn::makeNonNegativeIntegerBlock(ValidUntilType, validUntilMs_));
  block.push_back(ndn::makeStringBlock(PolicyDigestType, policyDigest_));
  if (!abePublicParametersName_.empty()) {
    block.push_back(ndn::makeStringBlock(AbePublicParametersNameType,
                                         abePublicParametersName_.toUri()));
    block.push_back(ndn::makeStringBlock(AbePublicParametersDigestType,
                                         abePublicParametersDigest_));
  }
  block.push_back(ndn::makeStringBlock(ControllerCertificateType,
                                       controllerCertificate_.toUri()));
  block.push_back(ndn::makeBinaryBlock(SignatureType, signature_.begin(), signature_.end()));
  for (const auto& target : revocations_)
    block.push_back(target.wireEncode());
  block.encode();
  return block;
}

bool
PolicyStatusData::wireDecode(const ndn::Block& block)
{
  try {
    if (block.type() != TYPE)
      return false;
    block.parse();
    const auto& elements = block.elements();
    if (elements.size() < 7)
      return false;
    size_t index = 0;
    PolicyStatusData decoded;
    decoded.serviceName_ = ndn::Name(ndn::readString(take(elements, index, ServiceNameType)));
    if (!decoded.controllerVersion_.wireDecode(
          take(elements, index, ControllerVersion::TYPE)))
      return false;
    decoded.validFromMs_ = ndn::readNonNegativeInteger(take(elements, index, ValidFromType));
    decoded.validUntilMs_ = ndn::readNonNegativeInteger(take(elements, index, ValidUntilType));
    decoded.policyDigest_ = ndn::readString(take(elements, index, PolicyDigestType));
    if (index < elements.size() &&
        elements[index].type() == AbePublicParametersNameType) {
      decoded.abePublicParametersName_ = ndn::Name(
          ndn::readString(take(elements, index, AbePublicParametersNameType)));
      decoded.abePublicParametersDigest_ = ndn::readString(
          take(elements, index, AbePublicParametersDigestType));
    }
    decoded.controllerCertificate_ = ndn::Name(
        ndn::readString(take(elements, index, ControllerCertificateType)));
    const auto& signature = take(elements, index, SignatureType);
    decoded.signature_ = ndn::Buffer(signature.value_begin(), signature.value_end());
    while (index < elements.size()) {
      RevocationTarget target;
      if (!target.wireDecode(elements[index++]))
        return false;
      decoded.revocations_.push_back(target);
    }
    if (!decoded.validate(decoded.validFromMs_))
      return false;
    *this = decoded;
    return true;
  }
  catch (const std::exception&) {
    return false;
  }
}

} // namespace ndn_service_framework
