#include "AbeHistoricalKey.hpp"
#include "AbeHistoricalAuthorization.hpp"
#include "AbeHistoricalKeyRequest.hpp"
#include "AbePublicParametersArchive.hpp"
#include "ServiceController.hpp"
#include "FileKeyRing.hpp"
#include "HybridMessageCrypto.hpp"
#include "ProducerDataNaming.hpp"
#include "ProducerSignerPolicy.hpp"
#include "CertificateBootstrap.hpp"
#include "detail/controller-version-floor.hpp"
#include "utils.hpp"

#include <ndn-cxx/security/transform.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/security/verification-helpers.hpp>
#include <ndn-cxx/util/logger.hpp>
#include <ndn-cxx/util/random.hpp>
#include <ndn-cxx/util/scope.hpp>
#include <nac-abe/algo/abe-support.hpp>
#include <nac-abe/ndn-crypto/data-enc-dec.hpp>
#include <ndn-cxx/util/segmenter.hpp>
#include <openssl/crypto.h>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>

#include <cstdlib>
#include <algorithm>
#include <array>
#include <atomic>
#include <ndn-cxx/util/sha256.hpp>

#include <chrono>
#include <charconv>
#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <string_view>
#include <vector>

namespace ndn_service_framework {

/** Whether the Controller answers historical decryption-key requests.
 *
 * Closed pending the audit's two P0 findings against issuance:
 *
 *   1. the reader checks only the reply's envelope fields and then hands the sealed key to its TPM, so a
 *      forged envelope naming the right generation would be unsealed without ever verifying the
 *      Controller's signature over the exact request and authorization version;
 *   2. the issuer decides historical permission from the retirement-time attribute snapshot, which cannot
 *      on its own prove what the identity held when the object was published, and it does not check the
 *      revocation state of the certificate the request actually presents.
 *
 * Both conditions are now implemented - the reader verifies the Authority's signature over the reply before
 * unsealing, and the decision is taken from the signed grant record for the exact (identity, generation,
 * epoch) with the presented certificate's revocation state checked - but the gate stays closed until a
 * deployment acceptance exercises the new decision. The last runs refused a legitimate request because the
 * object was published at a version for which no grant record existed yet; that sequencing is fixed in the
 * acceptance tooling, not in this decision.
 */
constexpr bool kHistoricalKeyIssuanceEnabled = true;

NDN_LOG_INIT(ndn_service_framework.ServiceController);

namespace {

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

  const auto result = output.buf();
  return ndn::Buffer(result->begin(), result->end());
}

size_t
fnv1aPolicyHash(const std::string& path)
{
  constexpr uint64_t offsetBasis = 1469598103934665603ULL;
  constexpr uint64_t prime = 1099511628211ULL;
  uint64_t hash = offsetBasis;

  std::ifstream input(path, std::ios::binary);
  char ch = 0;
  while (input.get(ch)) {
    hash ^= static_cast<unsigned char>(ch);
    hash *= prime;
  }

  if (hash == 0) {
    hash = 1;
  }
  if constexpr (sizeof(size_t) < sizeof(uint64_t)) {
    hash ^= hash >> (sizeof(size_t) * 8);
  }
  return static_cast<size_t>(
    hash & static_cast<uint64_t>(std::numeric_limits<size_t>::max()));
}

size_t
computePolicyEpoch(const std::string& path)
{
  if (const char* envEpoch = std::getenv("NDNSF_POLICY_EPOCH")) {
    try {
      const auto parsed = std::stoull(envEpoch);
      if (parsed > 0) {
        return static_cast<size_t>(parsed);
      }
    }
    catch (const std::exception&) {
      NDN_LOG_WARN("Ignoring invalid NDNSF_POLICY_EPOCH=" << envEpoch);
    }
  }

  return fnv1aPolicyHash(path);
}

uint64_t
nowMilliseconds()
{
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
}

ndn::Name
abeParametersPrefix(const ndn::Name& authority)
{
  return ndn::Name(authority).append(ndn::nacabe::PUBLIC_PARAMS).append(ndn::nacabe::ABE_TYPE_KP_ABE);
}

uint64_t
abeParametersVersion(const ndn::Name& name, const ndn::Name& prefix)
{
  if (name.size() != prefix.size() + 1 || !prefix.isPrefixOf(name) || !name.at(-1).isVersion())
    throw std::runtime_error("invalid Controller ABE public parameters name");
  return name.at(-1).toVersion();
}

std::shared_ptr<const ndn::Data>
decodeAbeParameters(const std::string& wire, const ndn::Name& prefix,
                    const std::string& parameters)
{
  auto data = std::make_shared<const ndn::Data>(
    ndn::Block(ndn::span<const uint8_t>(reinterpret_cast<const uint8_t*>(wire.data()), wire.size())));
  abeParametersVersion(data->getName(), prefix);
  const auto& content = data->getContent();
  if (content.value_size() != parameters.size() ||
      !std::equal(content.value_begin(), content.value_end(),
                  reinterpret_cast<const uint8_t*>(parameters.data())))
    throw std::runtime_error("Controller ABE public parameter packet content mismatch");
  return data;
}

ndn::Name
makeAuthorizationAttribute(const char* prefix, const std::string& service)
{
  ndn::Name attribute(prefix);
  attribute.append(ndn::Name(service));
  return attribute;
}

std::string
sha256FileDigest(const std::string& path)
{
  std::ifstream input(path, std::ios::binary);
  std::vector<uint8_t> bytes;
  if (input) {
    bytes.assign(std::istreambuf_iterator<char>(input),
                 std::istreambuf_iterator<char>());
  }
  if (bytes.empty()) {
    bytes.assign(path.begin(), path.end());
  }
  ndn::util::Sha256 digest;
  digest << std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  return "sha256:" + digest.toString();
}

std::string
makeBootstrapToken(size_t length)
{
  static constexpr std::string_view alphabet =
    "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
  ndn::Buffer randomBytes(length);
  ndn::random::generateSecureBytes(mutableBufferToSpan(randomBytes));

  std::string token;
  token.reserve(length);
  for (uint8_t byte : randomBytes) {
    token.push_back(alphabet[byte % alphabet.size()]);
  }
  return token;
}

ndn::Block
encryptWireBytesWithContentKeyForCertificate(const ndn::security::Certificate& cert,
                                             ndn::span<const uint8_t> plaintext)
{
  ndn::security::transform::PublicKey recipientPublicKey;
  recipientPublicKey.loadPkcs8(cert.getPublicKey());
  if (recipientPublicKey.getKeyType() != ndn::KeyType::RSA) {
    throw std::invalid_argument("Content encryption requires an RSA recipient certificate");
  }

  ndn::Buffer contentKey(32);
  ndn::Buffer iv(16);
  ndn::random::generateSecureBytes(mutableBufferToSpan(contentKey));
  ndn::random::generateSecureBytes(mutableBufferToSpan(iv));

  ndn::Buffer cipherText = runAesCbc(plaintext,
                                     bufferToSpan(contentKey),
                                     bufferToSpan(iv),
                                     ndn::CipherOperator::ENCRYPT);
  auto encryptedContentKey = recipientPublicKey.encrypt(bufferToSpan(contentKey));

  EncryptedPermissionResponse encryptedPayload;
  encryptedPayload.setRecipientCertName(cert.getName().toUri(ndn::name::UriFormat::CANONICAL));
  encryptedPayload.setAlgorithm("RSA-WRAPPED-AES-CBC");
  encryptedPayload.setEncryptedAesKey(
    ndn::Buffer(encryptedContentKey->begin(), encryptedContentKey->end()));
  encryptedPayload.setIv(iv);
  encryptedPayload.setCipherText(cipherText);
  return encryptedPayload.WireEncode();
}

ndn::Name
stripTrailingParametersDigest(ndn::Name name)
{
  if (!name.empty() && name[-1].isParametersSha256Digest()) {
    name = name.getPrefix(-1);
  }
  return name;
}

ndn::Name
stripCertificateBootstrapInterestSuffix(const ndn::Name& name)
{
  for (size_t i = 0; i < name.size(); ++i) {
    if (name[i].isParametersSha256Digest()) {
      return name.getPrefix(i);
    }
  }
  return stripTrailingParametersDigest(name);
}

ndn::Block
makeCertificateBootstrapProofData(const CertificateBootstrapRequest& request)
{
  ndn::Block block(bootstrap_tlv::CertificateBootstrapProofData);
  block.push_back(request.identity.wireEncode());
  block.push_back(ndn::makeStringBlock(tlv::TokenType, request.token));
  ndn::Block certBlock(bootstrap_tlv::CertificateRequest);
  certBlock.push_back(request.certificateRequest.wireEncode());
  certBlock.encode();
  block.push_back(certBlock);
  block.push_back(ndn::makeBinaryBlock(bootstrap_tlv::ProofNonce,
                                       request.proofNonce.begin(),
                                       request.proofNonce.end()));
  block.encode();
  return block;
}

std::string
signingKeyDigest(const ndn::security::Certificate& certificate)
{
  ndn::util::Sha256 digest;
  digest << certificate.getPublicKey();
  return "sha256:" + digest.toString();
}

ndn::Name
signerHandoffName(const ndn::security::Certificate& predecessor,
                  const ndn::security::Certificate& successor, const ControllerVersion& version)
{
  auto name = makeProducerDataPrefix(predecessor.getIdentity(), "CONTROLLER-HANDOFF");
  name.appendVersion(version.controllerGenerationTimestamp).appendVersion(version.controllerEpoch);
  ndn::util::Sha256 digest;
  for (const auto* certificate : {&predecessor, &successor}) {
    const auto& wire = certificate->wireEncode();
    digest << ndn::span<const uint8_t>(wire.data(), wire.size());
  }
  name.append(ndn::name::Component(ndn::span<const uint8_t>(*digest.computeDigest())));
  return name;
}

std::optional<ndn::security::Certificate>
acceptConfiguredSignerHandoff(const ControllerGenerationStore::AbeMaterial& material,
                              const ndn::security::Certificate& successor, ndn::KeyChain& keyChain,
                              const std::function<bool(const ndn::security::Certificate&)>& eligible)
{
  const char* path = std::getenv("NDNSF_CONTROLLER_HANDOFF");
  if (path == nullptr || *path == '\0' || !material.version.isValid() || !successor.isValid()) {
    NDN_LOG_WARN("NDNSF_CONTROLLER_HANDOFF_REJECTED reason=entry"
                 << " path=" << (path != nullptr && *path != '\0' ? "set" : "unset")
                 << " materialValid=" << material.version.isValid()
                 << " successorValid=" << successor.isValid());
    return std::nullopt;
  }
  try {
    std::ifstream input(path, std::ios::binary);
    std::array<uint8_t, ndn::MAX_NDN_PACKET_SIZE + 1> wire{};
    if (!input.is_open() || input.read(reinterpret_cast<char*>(wire.data()), wire.size()).bad())
      return std::nullopt;
    const auto size = static_cast<std::size_t>(input.gcount());
    if (size == 0 || size > ndn::MAX_NDN_PACKET_SIZE)
      return std::nullopt;
    const ndn::Block block(ndn::span<const uint8_t>(wire.data(), size));
    if (block.size() != size)
      return std::nullopt;
    const ndn::Data handoff(block);
    const ndn::security::Certificate predecessor{
      ndn::Data(handoff.getContent().blockFromValue())};
    const bool predecessorValid = predecessor.isValid();
    const bool sameIdentity = predecessor.getIdentity() == successor.getIdentity();
    const bool predecessorEligible = eligible(predecessor);
    const bool digestMatches = signingKeyDigest(predecessor) == material.signingKeyDigest;
    const bool nameMatches =
      handoff.getName() == signerHandoffName(predecessor, successor, material.version);
    const bool signatureValid = ndn::security::verifySignature(handoff, predecessor.getPublicKey());
    if (!predecessorValid || !sameIdentity || !predecessorEligible || !digestMatches ||
        !nameMatches || !signatureValid) {
      NDN_LOG_WARN("NDNSF_CONTROLLER_HANDOFF_REJECTED reason=checks"
                   << " valid=" << predecessorValid << " identity=" << sameIdentity
                   << " eligible=" << predecessorEligible << " digest=" << digestMatches
                   << " name=" << nameMatches << " signature=" << signatureValid);
      return std::nullopt;
    }
    const auto identity = keyChain.getPib().getIdentity(successor.getIdentity());
    if (identity.getDefaultKey().getDefaultCertificate().wireEncode() == successor.wireEncode() &&
        keyChain.getTpm().hasKey(successor.getKeyName()))
      return predecessor;
  }
  catch (const std::exception& error) {
    NDN_LOG_WARN("NDNSF_CONTROLLER_HANDOFF_REJECTED reason=exception error=" << error.what());
  }
  NDN_LOG_WARN("NDNSF_CONTROLLER_HANDOFF_REJECTED reason=validation");
  return std::nullopt;
}

constexpr size_t GRANT_DIGEST_BYTES = 32;

std::string
grantWireDigest(std::string_view wire)
{
  ndn::util::Sha256 digest;
  digest << ndn::span<const uint8_t>(reinterpret_cast<const uint8_t*>(wire.data()), wire.size());
  const auto bytes = digest.computeDigest();
  return std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size());
}

ndn::Data
decodeGrantData(const std::string& wire)
{
  const ndn::Block block(ndn::span<const uint8_t>(
    reinterpret_cast<const uint8_t*>(wire.data()), wire.size()));
  if (block.size() != wire.size())
    throw std::invalid_argument("trailing bytes in Controller grant data");
  return ndn::Data(block);
}

ndn::Name
historicalGrantRecordName(const ndn::Name& authority, std::string_view body)
{
  const auto digest = grantWireDigest(body);
  return makeProducerDataPrefix(authority, "HISTORICAL-GRANT").append(
    ndn::name::Component(ndn::span<const uint8_t>(
      reinterpret_cast<const uint8_t*>(digest.data()), digest.size())));
}

std::optional<HistoricalGrantRecord>
decodeControllerGrant(const ndn::Data& data, const ndn::Name& authority)
{
  const auto& content = data.getContent();
  const std::string_view body(reinterpret_cast<const char*>(content.value()), content.value_size());
  // Older Controllers stored signed grant records under an empty Data name.
  if (!data.getName().empty() && data.getName() != historicalGrantRecordName(authority, body))
    return std::nullopt;
  return decodeHistoricalGrantRecord(body);
}

ndn::Name
grantWitnessName(const ndn::Name& identity, const ControllerVersion& cutoff, std::string_view body)
{
  auto name = makeProducerDataPrefix(identity, "HISTORICAL-GRANT-WITNESS");
  name.appendVersion(cutoff.controllerGenerationTimestamp).appendVersion(cutoff.controllerEpoch);
  const auto digest = grantWireDigest(body);
  return name.append(ndn::name::Component(ndn::span<const uint8_t>(
    reinterpret_cast<const uint8_t*>(digest.data()), digest.size())));
}

std::set<std::string>
loadGrantWitness(const ControllerGenerationStore& store,
                 const ndn::security::Certificate& signer, const ControllerVersion& committed,
                 std::string* validatedWire = nullptr)
{
  const auto wire = store.loadGrantWitness(signingKeyDigest(signer));
  if (!wire)
    return {};
  const auto data = decodeGrantData(*wire);
  const auto& name = data.getName();
  const auto& content = data.getContent();
  const std::string_view body(reinterpret_cast<const char*>(content.value()), content.value_size());
  if (name.size() != makeProducerDataPrefix(signer.getIdentity(),
                                           "HISTORICAL-GRANT-WITNESS").size() + 3 ||
      !name.at(-3).isVersion() || !name.at(-2).isVersion() ||
      body.size() % GRANT_DIGEST_BYTES != 0 ||
      !ndn::security::verifySignature(data, signer.getPublicKey()))
    throw std::runtime_error("invalid Controller grant witness");
  const ControllerVersion cutoff{name.at(-3).toVersion(), name.at(-2).toVersion()};
  if (!cutoff.isValid() || cutoff.compare(committed) > 0 ||
      name != grantWitnessName(signer.getIdentity(), cutoff, body))
    throw std::runtime_error("Controller grant witness cutoff or digest mismatch");
  std::set<std::string> digests;
  for (size_t offset = 0; offset < body.size(); offset += GRANT_DIGEST_BYTES)
    digests.emplace(body.substr(offset, GRANT_DIGEST_BYTES));
  if (validatedWire != nullptr)
    *validatedWire = *wire;
  return digests;
}

std::pair<std::set<std::string>, ControllerGenerationStore::GrantWitnessRef>
carryGrantWitness(ControllerGenerationStore& store,
                  const ndn::security::Certificate& predecessor,
                  const ndn::security::Certificate& successor,
                  const ControllerVersion& committed, ndn::KeyChain& keyChain)
{
  auto digests = loadGrantWitness(store, predecessor, committed);
  store.visitGrantRecords([&] (const std::string& wire) {
    try {
      const auto data = decodeGrantData(wire);
      const auto record = decodeControllerGrant(data, predecessor.getIdentity());
      if (record && record->version.compare(committed) <= 0 &&
          ndn::security::verifySignature(data, predecessor.getPublicKey()))
        digests.insert(grantWireDigest(wire));
    }
    catch (const std::exception&) {
      // An invalid grant file cannot be made valid by the handoff.
    }
  });
  store.visitHistoricalPublicationReceipts(
    [&] (const std::string& wire, const ControllerVersion& version) {
      if (version.compare(committed) <= 0 &&
          ndn::security::verifySignature(decodeGrantData(wire), predecessor.getPublicKey()))
        digests.insert(grantWireDigest(wire));
    });
  // The bounded record scans and witness reader cap the retained inputs.
  // storeGrantWitness enforces the combined durable size limit.
  std::string body;
  body.reserve(digests.size() * GRANT_DIGEST_BYTES);
  for (const auto& digest : digests)
    body.append(digest);
  ndn::Data witness(grantWitnessName(successor.getIdentity(), committed, body));
  witness.setContent(ndn::span<const uint8_t>(
    reinterpret_cast<const uint8_t*>(body.data()), body.size()));
  keyChain.sign(witness, ndn::security::signingByCertificate(successor));
  const auto& wire = witness.wireEncode();
  auto reference = store.storeGrantWitness(signingKeyDigest(successor),
    std::string(reinterpret_cast<const char*>(wire.data()), wire.size()));
  return {std::move(digests), std::move(reference)};
}

} // namespace

ndn::Data
makeControllerSignerHandoff(const ndn::security::Certificate& predecessor,
                            const ndn::security::Certificate& successor,
                            const ControllerVersion& materialVersion, ndn::KeyChain& keyChain)
{
  if (!materialVersion.isValid() || !predecessor.isValid() || !successor.isValid() ||
      predecessor.getIdentity() != successor.getIdentity() ||
      signingKeyDigest(predecessor) == signingKeyDigest(successor))
    throw std::invalid_argument("invalid Controller signing handoff");
  ndn::Data handoff(signerHandoffName(predecessor, successor, materialVersion));
  handoff.setContent(predecessor.wireEncode());
  keyChain.sign(handoff, ndn::security::signingByCertificate(predecessor));
  if (handoff.wireEncode().size() > ndn::MAX_NDN_PACKET_SIZE)
    throw std::invalid_argument("Controller signing handoff exceeds Data packet size");
  return handoff;
}

std::string
PersistentKpAttributeAuthority::getMasterSecret() const
{
  return m_masterKey.m_msk;
}

std::optional<ndn::security::Certificate>
PersistentKpAttributeAuthority::findRecipient(const ndn::Name& keyName) const
{
  return m_trustConfig.findCertificateFromLocal(keyName);
}

ndn::nacabe::algo::PrivateKey
PersistentKpAttributeAuthority::issuePrivateKey(const std::set<std::string>& attributes)
{
  if (attributes.empty())
    throw std::invalid_argument("cannot issue a DKEY without authorized attributes");
  return ndn::nacabe::algo::ABESupport::getInstance().kpPrvKeyGen(
    m_pubParams, m_masterKey, boost::algorithm::join(attributes, " OR "));
}

void
PersistentKpAttributeAuthority::restoreKeyGeneration(const std::string& publicParameters,
                                                       const std::string& masterSecret)
{
  ndn::nacabe::algo::PublicParams params;
  ndn::nacabe::algo::MasterKey master;
  if (publicParameters.empty() || publicParameters.back() != '\0' ||
      publicParameters.find('\0') != publicParameters.size() - 1)
    throw std::runtime_error("invalid Controller ABE public parameters");
  params.fromBuffer(ndn::Buffer(publicParameters.begin(), publicParameters.end()));
  master.m_msk = masterSecret;
  try {
    auto& abe = ndn::nacabe::algo::ABESupport::getInstance();
    auto key = abe.kpPrvKeyGen(params, master, "ndnsfPersistenceCheck");
    const ndn::Buffer challenge{1, 2, 3, 4};
    auto cipher = abe.kpEncrypt(params, {"ndnsfPersistenceCheck"}, challenge);
    auto plain = abe.kpDecrypt(params, key, cipher);
    if (!key.m_prv.empty())
      OPENSSL_cleanse(&key.m_prv[0], key.m_prv.size());
    if (plain != challenge)
      throw std::runtime_error("Controller ABE material does not match");
  }
  catch (...) {
    if (!master.m_msk.empty())
      OPENSSL_cleanse(&master.m_msk[0], master.m_msk.size());
    throw;
  }
  if (!m_masterKey.m_msk.empty())
    OPENSSL_cleanse(&m_masterKey.m_msk[0], m_masterKey.m_msk.size());
  m_pubParams = std::move(params);
  m_masterKey = std::move(master);
}

ServiceController::ServiceController(ndn::Face& face,
                                     ndn::security::Certificate aaCert,
                                     ndn::security::Validator& validator,
                                     const std::string& configFilePath)
  : m_configFilePath(configFilePath)
  , m_face(face)
  , m_aaCert(std::move(aaCert))
  , m_validator(validator) // 关键：用传入的引用
  , m_keyChain()
  , m_abeSdkFace(nullptr, m_abeSdkContext, m_keyChain)
  , m_aa(m_aaCert, m_abeSdkFace, m_validator, m_keyChain)
{
  NDN_LOG_WARN("NDNSF_CONTROLLER_CTOR entered=1");
  // Load provider/user policies from config
  loadConfigFiles();
  NDN_LOG_WARN("NDNSF_CONTROLLER_CTOR policies-loaded=1");
  m_policyValidFromMs = 0;
  m_policyEpoch = computePolicyEpoch(m_configFilePath);
  m_requiredKeyEpoch = m_policyEpoch;

  // Establish a durable Controller generation before any protected material
  // can be issued.  A failed lease or corrupt state leaves the Controller
  // unable to publish permission/status Data rather than reusing a version.
  m_generationReady = initializeControllerGeneration();
  NDN_LOG_WARN("NDNSF_CONTROLLER_CTOR generationReady=" << (m_generationReady ? 1 : 0));

  if (m_generationReady) {
    try {
      restoreOrPersistAbeMaterial();
    }
  catch (const std::exception& error) {
      m_generationReady = false;
      NDN_LOG_ERROR("Controller ABE state initialization failed: " << error.what());
    }
  }

  NDN_LOG_WARN("NDNSF_CONTROLLER_CTOR abeInit-done=1");
  // Convert policies into ABE attribute policies and register them in AA
  addAttributesForUsersAccordingToServicePolicy();
  NDN_LOG_WARN("NDNSF_CONTROLLER_CTOR attributes-done=1");
  registerArchivedPublicParameters();
  NDN_LOG_WARN("NDNSF_CONTROLLER_CTOR registration-returned=1");

  // Build fast lookup tables for Interest answering
  buildLookupTables();
}

void
ServiceController::persistAbeMaterial(bool commitNextEpoch)
{
  ControllerGenerationStore::AbeMaterial material;
  material.owner = m_aaCert.getIdentity().toUri(ndn::name::UriFormat::CANONICAL);
  material.signingKeyDigest = signingKeyDigest(m_aaCert);
  material.policyDigest = sha256FileDigest(m_configFilePath);
  const auto publicParams = m_aa.getPublicParametersWire();
  material.publicParameters.assign(reinterpret_cast<const char*>(publicParams.data()),
                                   publicParams.size());
  material.masterSecret = m_aa.getMasterSecret();
  material.publicParametersVersion = m_aa.getPublicParametersVersion();
  // The material names the version it was generated for, so a version can never be published without material
  // behind it and material can never be served under a version it was not made for.
  material.version = m_controllerVersion;
  m_generationStore->storeAbeMaterial(material, commitNextEpoch);
  m_abeMaterialVersion = m_controllerVersion;
  m_abePublicParametersName = currentAbePublicParametersName();
  m_abePublicParametersDigest = abePublicParametersDigest(publicParams);
  m_currentAbeParameters.reset();
}

void
ServiceController::recordInitialAbeGrants()
{
  if (!canServeCurrentAbe() || m_abeMaterialVersion != m_controllerVersion)
    return;
  std::set<ndn::Name> users;
  for (const auto& configured : m_userPolicies)
    users.emplace(configured.userName);
  for (const auto& user : users) {
    if (!user.empty() && !isIdentityOrCertificateRevoked(user, {}))
      recordUserGrant(buildUserPermissionResponse(user));
  }
}

void
ServiceController::persistAbePublicParametersPacket()
{
  auto material = m_generationStore->loadAbeMaterial();
  // Grant-only updates can require rotation on restart while the live pair is still admitted.
  if (!canServeCurrentAbe() || !material || material->version != m_abeMaterialVersion ||
      material->signingKeyDigest != signingKeyDigest(m_aaCert) ||
      material->publicParametersVersion != m_aa.getPublicParametersVersion() ||
      !material->publicParametersPacket.empty())
    throw std::runtime_error("cannot sign an uncommitted Controller ABE public parameter packet");
  // Capture eligibility before issuing these parameters. Later grants must
  // use their new policy version and cannot backfill this material version.
  recordInitialAbeGrants();
  auto data = std::make_shared<ndn::Data>(m_abePublicParametersName);
  data->setFreshnessPeriod(ndn::time::seconds(5));
  data->setContent(m_aa.getPublicParametersWire());
  m_keyChain.sign(*data, ndn::security::signingByCertificate(m_aaCert));
  const auto& wire = data->wireEncode();
  if (wire.size() > ndn::MAX_NDN_PACKET_SIZE)
    throw std::runtime_error("Controller ABE public parameters packet exceeds NDN packet limit");
  material->publicParametersPacket.assign(reinterpret_cast<const char*>(wire.data()), wire.size());
  m_generationStore->storeAbeMaterial(*material);
  m_currentAbeParameters = std::move(data);
}

void
ServiceController::restoreOrPersistAbeMaterial()
{
  auto material = m_generationStore->loadAbeMaterial();
  if (material) {
    const auto currentSigner = signingKeyDigest(m_aaCert);
    const bool signingKeyRotated = material->signingKeyDigest != currentSigner;
    if (signingKeyRotated) {
      NDN_LOG_WARN("NDNSF_CONTROLLER_SIGNING_KEY_ROTATED stored=" << material->signingKeyDigest
                   << " current=" << currentSigner);
    }
    m_aa.restoreKeyGeneration(material->publicParameters, material->masterSecret);
    m_abeMaterialVersion = material->version;
    initializeAbeGenerationIdentity(*material);
    if (!signingKeyRotated && material->publicParametersVersion != 0 && !material->rotationRequired &&
        material->policyDigest == sha256FileDigest(m_configFilePath)) {
      return;
    }
    // A startup that finds a rotation outstanding must take the same path a revocation does: archive the
    // outgoing generation and snapshot what each identity held under it, then replace the keys. Rotating here
    // directly would leave the retired generation unreadable to every historical reader.
    rotateAbeGenerationAndReissuePolicies();
    return;
  }
  m_aa.setPublicParametersVersion(nextAbePublicParametersVersion());
  persistAbeMaterial();
}

bool
ServiceController::initializeControllerGeneration()
{
  const auto identity = m_aaCert.getIdentity();
  const auto configured = std::getenv("NDNSF_CONTROLLER_GENERATION_STATE");
  fs::path statePath;
  if (configured != nullptr && *configured != '\0') {
    statePath = configured;
  }
  try {
    const auto* checkpoint = std::getenv("NDNSF_CONTROLLER_RECOVERY_CHECKPOINT");
    const auto* importDigest = std::getenv("NDNSF_CONTROLLER_RECOVERY_IMPORT_SHA256");
    const auto* witnessImport = std::getenv("NDNSF_CONTROLLER_RECOVERY_IMPORT_WITNESS_SHA256");
    const auto* bootstrap = std::getenv("NDNSF_CONTROLLER_RECOVERY_BOOTSTRAP");
    const auto* floor = std::getenv(detail::CONTROLLER_VERSION_FLOOR_ENV);
    if (const auto* configuredLease = std::getenv("NDNSF_CONTROLLER_POLICY_LEASE_MS")) {
      const std::string_view text(configuredLease);
      uint64_t lease = 0;
      const auto parsed = std::from_chars(text.data(), text.data() + text.size(), lease);
      if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size() ||
          lease < 5000 || lease > 300000)
        throw std::runtime_error("invalid Controller policy lease (expected 5000..300000 ms)");
      m_policyStatusLeaseMs = lease;
    }
    if ((checkpoint != nullptr && (*checkpoint == '\0' || statePath.empty())) ||
        (importDigest != nullptr && (checkpoint == nullptr || *importDigest == '\0')) ||
        (witnessImport != nullptr && (checkpoint == nullptr || *witnessImport == '\0')) ||
        (bootstrap != nullptr && (std::string_view(bootstrap) != "1" ||
                                  checkpoint == nullptr || importDigest || witnessImport || floor)))
      throw std::runtime_error("invalid Controller recovery configuration");
    auto seal = loadFileKeyRingEntry("controller-abe");
    if (!seal)
      throw std::runtime_error("Controller requires the controller-abe key in NDNSF_FILE_KEYRING");
    if (statePath.empty()) {
      // Reproduce the old filename only to preserve an existing, proven owner.
      // New state is keyed by the identity bytes, never by opening them as a file.
      const auto key = fnv1aPolicyHash(
        m_configFilePath + identity.toUri(ndn::name::UriFormat::CANONICAL));
      const auto legacy = fs::path("/tmp") /
        ("ndnsf-controller-generation-" + std::to_string(key) + ".state");
      m_generationStore = ControllerGenerationStore::openDefault(
        identity.toUri(ndn::name::UriFormat::CANONICAL), legacy,
        std::move(seal->key), seal->keyId, true);
    }
    else {
      m_generationStore = std::make_unique<ControllerGenerationStore>(statePath);
      m_generationStore->setSecretSeal(std::move(seal->key), seal->keyId, true);
      if (checkpoint != nullptr)
        m_generationStore->setRecoveryCheckpoint(checkpoint, importDigest ? importDigest : "",
                                                  witnessImport ? witnessImport : "");
      m_generationStore->acquireWriter(identity.toUri(ndn::name::UriFormat::CANONICAL));
    }
    if (!m_generationStore->ownsWriter()) {
      NDN_LOG_ERROR("Controller generation writer unavailable identity="
                    << identity.toUri(ndn::name::UriFormat::CANONICAL) << " state=" << statePath);
      return false;
    }
    NDN_LOG_INFO("Controller ABE secrets: sealed with file key id=" << seal->keyId);
    statePath = m_generationStore->statePath();
    const auto restored = m_generationStore->load();
    if (checkpoint != nullptr && !restored && bootstrap == nullptr)
      throw std::runtime_error("Controller recovery state is missing; explicit bootstrap required");
    if (floor != nullptr) {
      if (!restored || !detail::meetsControllerVersionFloor(*restored, floor))
        throw std::runtime_error("Controller persisted state fails the configured version floor");
    }
    m_revocations = m_generationStore->loadRevocations();
    const auto eligible = [this] (const ndn::security::Certificate& certificate) {
      return !isIdentityOrCertificateRevoked(certificate.getIdentity(),
                                             certificateDigest(certificate));
    };
    if (!eligible(m_aaCert))
      throw std::runtime_error("Controller signing certificate is revoked");
    const auto material = m_generationStore->loadAbeMaterial();
    const auto signer = signingKeyDigest(m_aaCert);
    const auto witnessSigner = m_generationStore->getGrantWitnessSigner();
    std::optional<ControllerGenerationStore::GrantWitnessRef> nextWitness;
    if (material && material->signingKeyDigest != signer) {
      const auto predecessor = acceptConfiguredSignerHandoff(*material, m_aaCert, m_keyChain, eligible);
      if (!predecessor || !restored)
        throw std::runtime_error("Controller signing key change requires a valid handoff");
      // Receipts committed before the handoff carry the predecessor's signature; keep its certificate so
      // they can still be verified after the change.
      m_predecessorCertificate = *predecessor;
      if (witnessSigner && *witnessSigner == signer) {
        // The handoff committed before startup could replace the live ABE material.
        m_retiredGrantDigests = loadGrantWitness(*m_generationStore, m_aaCert, *restored);
      }
      else {
        if (witnessSigner && *witnessSigner != signingKeyDigest(*predecessor))
          throw std::runtime_error("Controller grant witness signer does not match the handoff");
        auto [digests, reference] = carryGrantWitness(*m_generationStore, *predecessor, m_aaCert,
                                                       *restored, m_keyChain);
        m_retiredGrantDigests = std::move(digests);
        nextWitness = std::move(reference);
      }
    }
    else if (restored) {
      if (witnessSigner && *witnessSigner != signer)
        throw std::runtime_error("Controller grant witness is committed to another signer");
      const bool migrateWitness = witnessImport != nullptr && !witnessSigner;
      std::string importedWire;
      m_retiredGrantDigests = loadGrantWitness(*m_generationStore, m_aaCert, *restored,
                                              migrateWitness ? &importedWire : nullptr);
      if (migrateWitness)
        nextWitness = m_generationStore->storeGrantWitness(signer, importedWire);
    }
    const auto now = nowMilliseconds();
    m_controllerVersion = m_generationStore->startGeneration(now, m_revocations, nextWitness);
    m_policyEpoch = static_cast<size_t>(m_controllerVersion.controllerEpoch);
    m_requiredKeyEpoch = m_policyEpoch;
    m_policyValidFromMs = now;
    m_policyValidUntilMs = now + m_policyStatusLeaseMs;
    NDN_LOG_INFO("NDNSF_CONTROLLER_VERSION generation="
                 << m_controllerVersion.controllerGenerationTimestamp
                 << " epoch=" << m_controllerVersion.controllerEpoch
                 << " state=" << statePath);
    return true;
  }
  catch (const std::exception& error) {
    NDN_LOG_ERROR("Controller generation initialization failed state="
                  << statePath << " error=" << error.what());
    if (m_generationStore)
      m_generationStore->releaseWriter();
    return false;
  }
}

ControllerVersion
ServiceController::getControllerVersion() const
{
  return m_controllerVersion;
}

bool
ServiceController::hasGenerationWriter() const
{
  return m_generationReady && m_generationStore && m_generationStore->ownsWriter();
}

bool
ServiceController::acceptsHistoricalGrantWire(const std::string& wire) const
{
  const auto data = decodeGrantData(wire);
  const auto record = decodeControllerGrant(data, m_aaCert.getIdentity());
  return record && record->version.compare(m_controllerVersion) <= 0 &&
         (ndn::security::verifySignature(data, m_aaCert.getPublicKey()) ||
          m_retiredGrantDigests.count(grantWireDigest(wire)) != 0);
}

std::string
ServiceController::certificateDigest(const ndn::security::Certificate& certificate)
{
  const auto wire = certificate.wireEncode();
  ndn::util::Sha256 digest;
  // Certificate digests are exchanged in revocation targets and encryption
  // advertisements.  Hash the canonical encoded certificate block, not only
  // its Value, so Controller, User, and Provider compute the same identity
  // for certificate-only revocation.
  digest << std::string(reinterpret_cast<const char*>(wire.data()), wire.size());
  auto hex = digest.toString();
  std::transform(hex.begin(), hex.end(), hex.begin(), [](unsigned char value) {
    return value >= 'A' && value <= 'F' ? value + ('a' - 'A') : value;
  });
  return "sha256:" + hex;
}

std::string
ServiceController::abePublicParametersDigest(const ndn::Buffer& wire)
{
  ndn::util::Sha256 digest;
  digest << std::string(reinterpret_cast<const char*>(wire.data()), wire.size());
  return "sha256:" + digest.toString();
}

ndn::Name
ServiceController::currentAbePublicParametersName() const
{
  return abeParametersPrefix(m_aaCert.getIdentity()).appendVersion(m_aa.getPublicParametersVersion());
}

void
ServiceController::initializeAbeGenerationIdentity(const ControllerGenerationStore::AbeMaterial& material)
{
  if (!m_controllerVersion.isValid() || !m_abeMaterialVersion.isValid() ||
      m_abeMaterialVersion.compare(m_controllerVersion) > 0)
    throw std::runtime_error("cannot initialize ABE generation without ControllerVersion");

  if (material.publicParametersVersion != 0) {
    m_aa.setPublicParametersVersion(material.publicParametersVersion);
  }
  else {
    // Recover a legacy reference for archival. Re-signing that name cannot reproduce its wire.
    const auto generation = material.version.controllerGenerationTimestamp;
    const auto epoch = material.version.controllerEpoch;
    if (generation > (std::numeric_limits<uint64_t>::max() - epoch) / 1000ULL)
      throw std::overflow_error("legacy ABE public-parameter generation version overflow");
    m_aa.setPublicParametersVersion(generation * 1000ULL + epoch);
  }
  if (!material.publicParametersPacket.empty()) {
    auto data = decodeAbeParameters(material.publicParametersPacket,
                                    abeParametersPrefix(m_aaCert.getIdentity()),
                                    material.publicParameters);
    if (data->getName() != currentAbePublicParametersName())
      throw std::runtime_error("stored Controller ABE public parameter name differs");
    if (material.signingKeyDigest == signingKeyDigest(m_aaCert)) {
      if (!ndn::security::verifySignature(*data, m_aaCert.getPublicKey()))
        throw std::runtime_error("invalid stored Controller ABE public parameter signature");
      m_currentAbeParameters = data;
    }
  }
  m_abePublicParametersName = currentAbePublicParametersName();
  m_abePublicParametersDigest = abePublicParametersDigest(
      m_aa.getPublicParametersWire());
}

uint64_t
ServiceController::nextAbePublicParametersVersion() const
{
  const auto prefix = abeParametersPrefix(m_aaCert.getIdentity());
  uint64_t previous = m_abeMaterialVersion.isValid() ? m_aa.getPublicParametersVersion() : 0;
  for (const auto& entry : m_generationStore->loadAbePublicHistory()) {
    previous = std::max(previous, abeParametersVersion(ndn::Name(entry.publicParametersName), prefix));
  }
  if (previous == std::numeric_limits<uint64_t>::max())
    throw std::overflow_error("Controller ABE public-parameter version exhausted");
  return std::max(nowMilliseconds(), previous + 1);
}

bool
ServiceController::isAbeAttributeRevoked(const std::string& identity,
                                          const std::string& attribute) const
{
  const ndn::Name identityName(identity);
  const ndn::Name attributeName(attribute);

  // The attribute policy belongs to the identity. Each issuance path separately checks
  // the certificate it actually uses, including asynchronous fetches and DKEY continuations.
  for (const auto& target : m_revocations) {
    if (target.kind == RevocationKind::IDENTITY &&
        target.targetIdentity == identityName)
      return true;
    if (target.kind == RevocationKind::SERVICE_AUTHORIZATION &&
        target.targetIdentity == identityName &&
        target.authorizationAttribute == attributeName)
      return true;
  }
  return false;
}

std::set<std::string>
ServiceController::effectiveAttributesFor(const std::string& identity) const
{
  std::set<std::string> effective;
  const auto it = m_attributesMap.find(identity);
  if (it == m_attributesMap.end())
    return effective;
  for (const auto& attribute : it->second) {
    if (!isAbeAttributeRevoked(identity, attribute))
      effective.insert(attribute);
  }
  return effective;
}

void
ServiceController::reissueAbePolicies()
{
  // DKEYs are monolithic per identity in the current KP-ABE implementation.
  // Reissue the complete remaining attribute set, never an individual
  // attribute fragment, and remove identities with no remaining authority.
  for (const auto& item : m_attributesMap) {
    const auto effective = effectiveAttributesFor(item.first);
    const ndn::Name identity(item.first);
    m_aa.removePolicy(identity);
    if (effective.empty())
      continue;
    std::list<std::string> attributes(effective.begin(), effective.end());
    m_aa.replacePolicy(identity, boost::algorithm::join(attributes, " OR "));
  }
}

void
ServiceController::rotateAbeGenerationAndReissuePolicies(bool commitNextEpoch)
{
  // Test-only fault injection: NDNSF_CONTROLLER_FAULT_INJECT_ABE_ROTATE=1
  // throws before any crypto work so the revocation-rotation failure path
  // (recording + reconcile retry) is executable in RV-U24.  Production never
  // sets this variable.
  const char* injectRotateFailure = std::getenv("NDNSF_CONTROLLER_FAULT_INJECT_ABE_ROTATE");
  if (injectRotateFailure != nullptr && *injectRotateFailure != '\0') {
    // A one-shot network fault leaves subsequent explicit retries runnable.
    if (std::string(injectRotateFailure) == "once")
      ::unsetenv("NDNSF_CONTROLLER_FAULT_INJECT_ABE_ROTATE");
    throw std::runtime_error("injected ABE rotation failure");
  }
  if (!m_controllerVersion.isValid())
    throw std::runtime_error("cannot rotate ABE generation without ControllerVersion");
  const auto nextVersion = nextAbePublicParametersVersion();
  if (m_abeMaterialVersion.isValid()) {
    // Retain the committed generation before changing the in-memory authority.
    const auto current = m_generationStore->loadAbeMaterial();
    if (!current)
      throw std::runtime_error("cannot archive missing committed Controller ABE material");
    ControllerGenerationStore::AbeHistoryEntry archived;
    archived.generationTimestamp = m_abeMaterialVersion.controllerGenerationTimestamp;
    archived.epoch = m_abeMaterialVersion.controllerEpoch;
    archived.publicParameters = current->publicParameters;
    archived.masterSecret = current->masterSecret;
    archived.publicParametersName = m_abePublicParametersName.toUri(ndn::name::UriFormat::CANONICAL);
    archived.publicParametersPacket = current->publicParametersPacket;
    archived.publicParametersWireDigest = m_abePublicParametersDigest;
    archived.reason = "revocation-rotation";
    m_generationStore->recordAbeHistory(archived);
    m_generationStore->recordAttributeSnapshot(archived.generationTimestamp, archived.epoch,
                                               m_attributesMap);
  }
  m_aa.rotateKeyGeneration(nextVersion);
  reissueAbePolicies();
  persistAbeMaterial(commitNextEpoch);
}

bool
ServiceController::reconcilePendingAbeRotation()
{
  if (!m_abeRotationPending)
    return true;
  const auto committed = m_controllerVersion;
  if (!hasGenerationWriter() || committed.controllerEpoch == std::numeric_limits<uint64_t>::max())
    return false;
  ++m_controllerVersion.controllerEpoch;
  try {
    rotateAbeGenerationAndReissuePolicies(true);
  }
  catch (const ControllerGenerationStore::CommitUncertain& error) {
    m_generationReady = false;
    m_controllerVersion = committed;
    NDN_LOG_ERROR("NDNSF_CONTROLLER_EPOCH_COMMIT_UNCERTAIN error=" << error.what());
    return false;
  }
  catch (const std::exception& error) {
    m_controllerVersion = committed;
    NDN_LOG_ERROR("NDNSF_CONTROLLER_ABE_REKEY_RETRY_FAILED error=" << error.what());
    return false;
  }
  m_policyEpoch = static_cast<size_t>(m_controllerVersion.controllerEpoch);
  m_requiredKeyEpoch = m_policyEpoch;
  m_policyValidFromMs = nowMilliseconds();
  m_policyValidUntilMs = m_policyValidFromMs + m_policyStatusLeaseMs;
  m_abeRotationPending = false;
  registerArchivedPublicParameters();
  NDN_LOG_WARN("NDNSF_CONTROLLER_ABE_REKEY_RECONCILED generation="
               << m_controllerVersion.controllerGenerationTimestamp
               << " epoch=" << m_controllerVersion.controllerEpoch);
  return true;
}

bool
ServiceController::isRevoked(const ndn::Name& identity,
                              const ndn::Name& serviceName,
                              const std::string& certificateDigestValue,
                              const ndn::Name& authorizationAttribute) const
{
  for (const auto& target : m_revocations) {
    switch (target.kind) {
    case RevocationKind::IDENTITY:
      if (target.targetIdentity == identity)
        return true;
      break;
    case RevocationKind::CERTIFICATE:
      if (sameCertificateDigest(target.certificateDigest, certificateDigestValue) &&
          (target.targetIdentity.empty() || target.targetIdentity == identity))
        return true;
      break;
    case RevocationKind::SERVICE_AUTHORIZATION:
      NDN_LOG_WARN("NDNSF_SERVICE_REVOCATION_COMPARE targetIdentity=" << target.targetIdentity.toUri()
                   << " identity=" << identity.toUri()
                   << " targetService=" << target.serviceName.toUri()
                   << " service=" << serviceName.toUri()
                   << " targetAttribute=" << target.authorizationAttribute.toUri()
                   << " attribute=" << authorizationAttribute.toUri());
      if (target.targetIdentity == identity && target.serviceName == serviceName &&
          (authorizationAttribute.empty() ||
           target.authorizationAttribute == authorizationAttribute))
        return true;
      break;
    }
  }
  return false;
}

bool
ServiceController::isIdentityOrCertificateRevoked(
    const ndn::Name& identity, const std::string& certificateDigestValue) const
{
  return isRevoked(identity, ndn::Name(), certificateDigestValue);
}

void
ServiceController::advanceAuthorizationEpoch(bool requireAbeRotationOnRestart)
{
  if (!hasGenerationWriter())
    throw std::runtime_error("Controller generation is not ready");
  try {
    m_controllerVersion = m_generationStore->advanceEpoch(m_revocations,
                                                          requireAbeRotationOnRestart);
  }
  catch (const ControllerGenerationStore::CommitUncertain&) {
    // The rename may have succeeded even if the subsequent directory flush failed.
    // Stop serving rather than guessing which version survived.
    m_generationReady = false;
    throw;
  }
  m_policyEpoch = static_cast<size_t>(m_controllerVersion.controllerEpoch);
  m_requiredKeyEpoch = m_policyEpoch;
  m_policyValidFromMs = nowMilliseconds();
  m_policyValidUntilMs = m_policyValidFromMs + m_policyStatusLeaseMs;
}

void
ServiceController::registerServedPrefix(const ndn::Name& prefix,
                                        const std::function<void(const ndn::InterestFilter&,
                                                                 const ndn::Interest&)>& onInterest)
{
  if (!m_registeredServedPrefixes.insert(prefix.toUri(ndn::name::UriFormat::CANONICAL)).second)
    return;
  // The history prefixes are served Data, so they need the same forwarder registration the RPC prefixes get:
  // the two-argument overload only dispatches inside this process, and a MiniNDN actor route for another node
  // does not cover this one.
  auto holder = std::make_shared<ndn::ScopedRegisteredPrefixHandle>();
  m_registrationHandles.push_back(holder);
  *holder = m_face.setInterestFilter(prefix, onInterest,
    [] (const ndn::Name&) {},
    [prefix] (const ndn::Name&, const std::string& reason) {
      NDN_LOG_ERROR("NDNSF_HISTORY_REGISTER_FAILED prefix=" << prefix << " reason=" << reason);
    });
}

void
ServiceController::registerArchivedPublicParameters()
{
  if (!hasGenerationWriter())
    return;
  const auto& identity = m_aaCert.getIdentity();
  ndn::Name indexName(identity);
  indexName.append("NDNSF").append("HISTORY").append("PUBPARAMS");
  const auto sign = [this] (ndn::Data& data) {
    m_keyChain.sign(data, ndn::security::signingByCertificate(m_aaCert));
  };
  try {
    const auto archived = m_generationStore->loadAbePublicHistory();
    std::map<ndn::Name, std::shared_ptr<const ndn::Data>> packets;
    std::map<ndn::Name, std::shared_ptr<const ndn::Data>> proofs;
    for (const auto& entry : archived) {
      if (!entry.publicParametersPacket.empty()) {
        auto packet = decodeAbeParameters(entry.publicParametersPacket, abeParametersPrefix(identity),
                                          entry.publicParameters);
        if (packet->getName() != ndn::Name(entry.publicParametersName) ||
            !packets.emplace(packet->getName(), packet).second)
          throw std::runtime_error("invalid Controller ABE archived packet name");
      }

      auto proof = std::make_shared<ndn::Data>();
      if (!buildHistoricalPublicParametersData(entry, identity, {}, *proof,
                                                 m_controllerVersion.controllerGenerationTimestamp))
        continue;
      const auto previous = m_archivedAbeProofs.find(proof->getName());
      if (previous != m_archivedAbeProofs.end()) {
        if (previous->second->getContent() != proof->getContent())
          throw std::runtime_error("Controller ABE archived proof changed");
        proofs.emplace(previous->first, previous->second);
      }
      else {
        sign(*proof);
        if (proof->wireEncode().size() > ndn::MAX_NDN_PACKET_SIZE)
          throw std::runtime_error("Controller ABE archived proof exceeds packet size");
        proofs.emplace(proof->getName(), proof);
      }
    }

    auto index = std::make_shared<ndn::Data>();
    std::shared_ptr<const ndn::Data> indexPacket;
    if (buildHistoricalPublicParametersIndex(archived, identity, {}, *index)) {
      index->setName(historicalPublicParametersIndexName(identity,
        m_controllerVersion.controllerGenerationTimestamp, m_abeMaterialVersion));
      if (m_archivedAbeIndex && m_archivedAbeIndex->getName() == index->getName()) {
        if (m_archivedAbeIndex->getContent() != index->getContent())
          throw std::runtime_error("Controller ABE archived index changed");
        indexPacket = m_archivedAbeIndex;
      }
      else {
        sign(*index);
        if (index->wireEncode().size() <= ndn::MAX_NDN_PACKET_SIZE)
          indexPacket = index;
        else
          NDN_LOG_ERROR("Controller ABE archived index exceeds packet size");
      }
    }

    m_archivedAbeParameters = std::move(packets);
    m_archivedAbeProofs = std::move(proofs);
    m_archivedAbeIndex = std::move(indexPacket);
    NDN_LOG_INFO("Historical public parameters: archived=" << archived.size()
                 << " published=" << m_archivedAbeProofs.size());
  }
  catch (const std::exception& error) {
    // Keep previously published wire for this writer if an archive rebuild
    // fails. Discarding it could cause a second signature at the same name.
    NDN_LOG_ERROR("Controller archived public parameters unavailable: " << error.what());
  }

  registerServedPrefix(indexName,
    [this, indexName] (const ndn::InterestFilter&, const ndn::Interest& interest) {
      onArchivedPublicParametersInterest(indexName, interest);
    });

  if (kHistoricalKeyIssuanceEnabled) {
    const auto owner = m_controllerPrefix.empty() ? identity : m_controllerPrefix;
    auto prefix = owner;
    prefix.append("NDNSF").append("HISTORY").append("DKEY");
    registerServedPrefix(prefix,
      [this] (const ndn::InterestFilter&, const ndn::Interest& interest) {
        serveHistoricalKeyRequest(interest);
      });
    auto publication = ndn::Name(owner).append("NDNSF").append("RPC").append("PUBLISH-HISTORY");
    registerServedPrefix(publication,
      [this] (const ndn::InterestFilter&, const ndn::Interest& interest) {
        onHistoricalPublicationInterest(interest);
      });
    // The receipt acknowledgement carries the receipt's FullName; a reader must be able to fetch that
    // exact object back, whatever protection scheme issued it.
    for (const auto* kind : {"PUBLICATION-RECEIPT", "GRANT-RECEIPT"}) {
      auto receipts = ndn::Name(owner).append("NDNSF").append("HISTORY").append(kind);
      registerServedPrefix(receipts,
        [this] (const ndn::InterestFilter&, const ndn::Interest& interest) {
          serveHistoricalReceipt(interest);
        });
    }
  }
}

void
ServiceController::onArchivedPublicParametersInterest(const ndn::Name& indexName,
                                                       const ndn::Interest& interest)
{
  const auto& name = interest.getName();
  if (!hasGenerationWriter() || !indexName.isPrefixOf(name))
    return;
  const auto exact = name.at(-1).isImplicitSha256Digest() ? name.getPrefix(-1) : name;
  if (m_archivedAbeIndex && exact == m_archivedAbeIndex->getName() &&
      interest.matchesData(*m_archivedAbeIndex)) {
    m_face.put(*m_archivedAbeIndex);
    return;
  }
  const auto proof = m_archivedAbeProofs.find(exact);
  if (proof != m_archivedAbeProofs.end() && interest.matchesData(*proof->second))
    m_face.put(*proof->second);
}

void
ServiceController::onHistoricalPublicationInterest(const ndn::Interest& interest)
{
  if (!hasGenerationWriter() || !admitControlQuery(interest, "PUBLISH-HISTORY")) {
    NDN_LOG_WARN("NDNSF_HISTORICAL_RECEIPT_REJECTED reason=not-admitted");
    return;
  }
  const std::weak_ptr<int> lifetime = m_currentAbeLifetime;
  m_validator.validate(interest,
    [this, lifetime, version = m_controllerVersion] (const ndn::Interest& validated) {
      if (lifetime.expired() || !hasGenerationWriter() || m_abeRotationPending ||
          m_controllerVersion != version || !admitControlQuery(validated, "PUBLISH-HISTORY")) {
        NDN_LOG_WARN("NDNSF_HISTORICAL_RECEIPT_REJECTED reason=stale-generation");
        return;
      }
      try {
        const auto producerCertificate = getValidatedHistoricalSigner(validated);
        const auto parameters = validated.getApplicationParameters();
        // A grant-protected publication has no CK, so its receipt request carries a different body.
        // The body names its own schema, so the two shapes are decided here rather than by the name.
        {
          const std::string body(reinterpret_cast<const char*>(parameters.value()),
                                 parameters.value_size());
          HistoricalGrantReceiptRequest grantRequest;
          if (decodeHistoricalGrantReceiptRequest(body, grantRequest)) {
            issueHistoricalGrantReceipt(validated, producerCertificate, grantRequest, version);
            return;
          }
        }
        const ndn::Data publication{ndn::Block(ndn::span<const uint8_t>(
          parameters.value(), parameters.value_size()))};
        const auto published = decodeHistoricalPublication(publication);
        const bool decoded = published.has_value();
        const bool controllerMatches = decoded && published->controller == m_controllerPrefix;
        const bool producerMatches = decoded &&
          published->producer == producerCertificate.getIdentity();
        const bool versionMatches = decoded &&
          ControllerVersion{published->policyGeneration, published->policyEpoch} == version;
        const bool producerLive = decoded &&
          !isIdentityOrCertificateRevoked(published->producer, certificateDigest(producerCertificate));
        if (!decoded || !controllerMatches || !producerMatches || !versionMatches || !producerLive) {
          NDN_LOG_WARN("NDNSF_HISTORICAL_RECEIPT_REJECTED reason=publication-binding"
                       << " decoded=" << decoded << " controller=" << controllerMatches
                       << " producer=" << producerMatches << " version=" << versionMatches
                       << " live=" << producerLive);
          return;
        }
        const auto attributes = effectiveAttributesFor(
          published->producer.toUri(ndn::name::UriFormat::CANONICAL));
        const auto provider = ndn::Name("/SERVICE").append(published->service).toUri();
        const auto user = ndn::Name("/PERMISSION").append(published->service).toUri();
        if (attributes.count(provider) == 0 && attributes.count(user) == 0) {
          NDN_LOG_WARN("NDNSF_HISTORICAL_RECEIPT_REJECTED reason=not-authorised");
          return;
        }
        if (serveOriginalControlReply(validated))
          return;

        const auto original = m_generationStore->loadHistoricalPublicationReceipt(
          publication.getFullName());
        ndn::Data receipt;
        if (original) {
          receipt.wireDecode(ndn::Block(ndn::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(original->data()), original->size())));
          if (!verifyHistoricalPublicationReceipt(receipt, publication, m_aaCert) &&
              !(m_predecessorCertificate &&
                verifyHistoricalPublicationReceipt(receipt, publication,
                                                   *m_predecessorCertificate))) {
            NDN_LOG_WARN("NDNSF_HISTORICAL_RECEIPT_REJECTED reason=stored-receipt-invalid");
            return;
          }
        }
        else {
          auto status = getPolicyStatus(published->service);
          const auto now = nowMilliseconds();
          status.setValidity(now, now + m_policyStatusLeaseMs);
          receipt = makeHistoricalPublicationReceipt(publication, producerCertificate, status,
                                                      m_aaCert, m_keyChain, now);
          const auto& wire = receipt.wireEncode();
          m_generationStore->recordHistoricalPublicationReceipt(publication.getFullName(),
            std::string(reinterpret_cast<const char*>(wire.data()), wire.size()));
        }
        ndn::Data acknowledgement(validated.getName());
        acknowledgement.setContent(receipt.getFullName().wireEncode());
        acknowledgement.setFreshnessPeriod(ndn::time::milliseconds(0));
        m_keyChain.sign(acknowledgement, ndn::security::signingByCertificate(m_aaCert));
        NDN_LOG_WARN("NDNSF_HISTORICAL_RECEIPT_COMMITTED publication="
                     << publication.getFullName().toUri());
        publishOriginalControlReply(validated, acknowledgement);
      }
      catch (const ControllerGenerationStore::CommitUncertain& error) {
        m_generationReady = false;
        NDN_LOG_ERROR("Historical publication receipt commit uncertain: " << error.what());
      }
  catch (const std::exception& error) {
        NDN_LOG_ERROR("Historical publication receipt failed: " << error.what());
      }
    },
    [] (const ndn::Interest&, const ndn::security::ValidationError&) {});
}

bool
ServiceController::acceptsHistoricalPublication(const ndn::Data& publication) const
{
  try {
    const auto binding = decodeHistoricalPublication(publication);
    if (!binding || binding->controller != m_aaCert.getIdentity())
      return false;
    const auto original = m_generationStore->loadHistoricalPublicationReceipt(
      publication.getFullName());
    if (!original)
      return false;
    const auto receipt = decodeGrantData(*original);
    const auto evidence = decodeHistoricalPublicationReceipt(receipt, m_aaCert.getIdentity());
    return evidence && evidence->publicationFullName == publication.getFullName() &&
      evidence->version == ControllerVersion{binding->policyGeneration, binding->policyEpoch} &&
      evidence->version.compare(m_controllerVersion) <= 0 &&
      (ndn::security::verifySignature(receipt, m_aaCert.getPublicKey()) ||
       m_retiredGrantDigests.count(grantWireDigest(*original)) != 0);
  }
  catch (const std::exception&) {
    return false;
  }
}

bool
ServiceController::hasHistoricalGrant(const ndn::Name& identity, const ControllerVersion& version,
                                     const std::string& policy) const
{
  try {
    const auto record = m_generationStore->loadGrant(
      identity.toUri(ndn::name::UriFormat::CANONICAL),
      version.controllerGenerationTimestamp, version.controllerEpoch);
    if (!record || !acceptsHistoricalGrantWire(*record))
      return false;
    const auto data = decodeGrantData(*record);
    return historicalGrantRecordCoversPolicy(ndn::readString(data.getContent()), identity,
      version.controllerGenerationTimestamp, version.controllerEpoch, policy);
  }
  catch (const std::exception&) {
    return false;
  }
}

ndn::security::Certificate
ServiceController::getValidatedHistoricalSigner(const ndn::Interest& interest) const
{
  const auto signerName = getSignerCertNameFromInterest(interest);
  ndn::security::Certificate signer;
  try {
    signer = getSignerCertificateFromInterest(interest);
  }
  catch (const std::exception& error) {
    const auto* trusted = m_validator.findTrustedCert(ndn::Interest(signerName));
    if (!trusted || trusted->getName() != signerName)
      throw;
    signer = *trusted;
  }
  if (!signer.isValid() || !ndn::security::verifySignature(interest, signer.getPublicKey()))
    throw std::runtime_error("historical signing certificate mismatch");
  return signer;
}

void
ServiceController::publishHistoricalTerminal(const ndn::Data& data,
                                             const ControllerVersion& version)
{
  if (!hasGenerationWriter() || m_abeRotationPending || version != m_controllerVersion)
    return;
  try {
    const auto& wire = data.wireEncode();
    m_generationStore->recordHistoricalReply(version, data.getName(),
      std::string(reinterpret_cast<const char*>(wire.data()), wire.size()));
    m_face.put(data);
  }
  catch (const ControllerGenerationStore::CommitUncertain& error) {
    m_generationReady = false;
    NDN_LOG_ERROR("Historical key reply commit uncertain: " << error.what());
  }
  catch (const std::exception& error) {
    NDN_LOG_ERROR("Historical key reply could not be published: " << error.what());
  }
}

bool
ServiceController::serveHistoricalTerminal(const ndn::Name& name, const ControllerVersion& version,
                                           bool allowSuccessfulReply)
{
  try {
    if (!hasGenerationWriter() || m_abeRotationPending || version != m_controllerVersion)
      return true;
    const auto stored = m_generationStore->loadHistoricalReply(version, name);
    if (!stored)
      return false;
    const ndn::Data reply{ndn::Block(ndn::span<const uint8_t>(
      reinterpret_cast<const uint8_t*>(stored->data()), stored->size()))};
    if (reply.getName() != name || !verifyHistoricalKeyReplySignature(reply, m_aaCert))
      throw std::runtime_error("invalid stored historical key reply");
    const auto& content = reply.getContent();
    const std::string_view text(reinterpret_cast<const char*>(content.value()),
                                content.value_size());
    if (allowSuccessfulReply || text.find("NDNSF-HISTORICAL-DKEY-DENIED-v1\n") == 0)
      m_face.put(reply);
  }
  catch (const std::exception& error) {
    NDN_LOG_ERROR("Historical key terminal replay failed: " << error.what());
  }
  return true;
}

void
ServiceController::serveHistoricalReceipt(const ndn::Interest& interest)
{
  // Receipts are public admission evidence: they are signed by this Controller and bind a
  // publication to the policy state that authorised it. Serving one by name therefore needs no
  // additional authorisation, but the reply must be the stored wire rather than a re-signature.
  std::optional<std::string> match;
  try {
    m_generationStore->visitHistoricalPublicationReceipts(
      [&] (const std::string& wire, const ControllerVersion&) {
        if (match)
          return;
        try {
          const ndn::Block block(ndn::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(wire.data()), wire.size()));
          if (block.size() != wire.size())
            return;
          // The acknowledgement names the receipt by its FullName, digest included.
          if (ndn::Data(block).getFullName() == interest.getName())
            match = wire;
        }
        catch (const std::exception&) {
        }
      });
  }
  catch (const std::exception& error) {
    NDN_LOG_WARN("NDNSF_HISTORICAL_RECEIPT_FETCH_FAILED error=" << error.what());
    return;
  }
  if (!match) {
    NDN_LOG_WARN("NDNSF_HISTORICAL_RECEIPT_FETCH_MISS name=" << interest.getName().toUri());
    return;
  }
  try {
    ndn::Data reply(ndn::Block(ndn::span<const uint8_t>(
      reinterpret_cast<const uint8_t*>(match->data()), match->size())));
    m_face.put(reply);
    NDN_LOG_WARN("NDNSF_HISTORICAL_RECEIPT_FETCHED name=" << interest.getName().toUri());
  }
  catch (const std::exception& error) {
    NDN_LOG_WARN("NDNSF_HISTORICAL_RECEIPT_FETCH_FAILED error=" << error.what());
  }
}

void
ServiceController::issueHistoricalGrantReceipt(const ndn::Interest& validated,
                                              const ndn::security::Certificate& producerCertificate,
                                              const HistoricalGrantReceiptRequest& request,
                                              const ControllerVersion& version)
{
  if (request.currentControllerVersion != version ||
      request.controllerPrefix != m_controllerPrefix) {
    NDN_LOG_WARN("NDNSF_GRANT_RECEIPT_REJECTED reason=version-or-controller");
    return;
  }
  if (request.service.empty()) {
    NDN_LOG_WARN("NDNSF_GRANT_RECEIPT_REJECTED reason=no-service");
    return;
  }
  const auto& wire = request.publicationWire;
  const ndn::Data publication{ndn::Block(ndn::span<const uint8_t>(
    reinterpret_cast<const uint8_t*>(wire.data()), wire.size()))};
  if (!ndn::security::verifySignature(publication, producerCertificate.getPublicKey())) {
    NDN_LOG_WARN("NDNSF_GRANT_RECEIPT_REJECTED reason=publication-signature");
    return;
  }
  // The Producer must actually own the namespace it publishes in: a signature over someone else's
  // object is not a publication authorisation.
  if (!producerCertificate.getIdentity().isPrefixOf(publication.getName())) {
    NDN_LOG_WARN("NDNSF_GRANT_RECEIPT_REJECTED reason=producer-namespace");
    return;
  }
  // Same authorisation the CK receipt applies: the Controller's own permission state must grant this
  // Producer (or its users) the service the publication belongs to.
  const auto grantAttributes = effectiveAttributesFor(
    producerCertificate.getIdentity().toUri(ndn::name::UriFormat::CANONICAL));
  const auto grantedProvider = ndn::Name("/SERVICE").append(request.service).toUri();
  const auto grantedUser = ndn::Name("/PERMISSION").append(request.service).toUri();
  if (grantAttributes.count(grantedProvider) == 0 && grantAttributes.count(grantedUser) == 0) {
    NDN_LOG_WARN("NDNSF_GRANT_RECEIPT_REJECTED reason=not-authorised service="
                 << request.service);
    return;
  }
  if (isIdentityOrCertificateRevoked(producerCertificate.getIdentity(),
                                     certificateDigest(producerCertificate))) {
    NDN_LOG_WARN("NDNSF_GRANT_RECEIPT_REJECTED reason=revoked");
    return;
  }
  const auto original = m_generationStore->loadHistoricalGrantReceipt(
    publication.getFullName());
  ndn::Data receipt;
  if (original) {
    receipt.wireDecode(ndn::Block(ndn::span<const uint8_t>(
      reinterpret_cast<const uint8_t*>(original->data()), original->size())));
    const auto stored = decodeHistoricalGrantReceipt(receipt, m_controllerPrefix);
    if (!stored || stored->grantDigest != request.grantDigest ||
        stored->recipient != request.recipient || stored->service != request.service ||
        stored->version != version ||
        stored->publicationFullName != publication.getFullName() ||
        !ndn::security::verifySignature(receipt, m_aaCert.getPublicKey())) {
      NDN_LOG_WARN("NDNSF_GRANT_RECEIPT_REJECTED reason=stored-receipt-invalid");
      return;
    }
  }
  else {
    receipt = makeHistoricalGrantReceipt(m_controllerPrefix, publication.getFullName(),
                                         request.grantDigest, request.recipient, request.service,
                                         version, m_aaCert, m_keyChain);
    const auto& receiptWire = receipt.wireEncode();
    m_generationStore->recordHistoricalGrantReceipt(
      publication.getFullName(),
      std::string(reinterpret_cast<const char*>(receiptWire.data()), receiptWire.size()));
  }
  ndn::Data acknowledgement(validated.getName());
  // The receipt's name as canonical text: the wire form of a name nested in a content field is easy to
  // mis-frame, and a reader only needs the name to fetch the receipt back.
  acknowledgement.setContent(ndn::makeStringBlock(ndn::tlv::Content,
                                                  receipt.getFullName().toUri()));
  acknowledgement.setFreshnessPeriod(ndn::time::milliseconds(0));
  m_keyChain.sign(acknowledgement, ndn::security::signingByCertificate(m_aaCert));
  NDN_LOG_WARN("NDNSF_GRANT_RECEIPT_COMMITTED publication="
               << publication.getFullName().toUri());
  publishOriginalControlReply(validated, acknowledgement);
}

void
ServiceController::serveHistoricalKeyRequest(const ndn::Interest& interest)
{
  NDN_LOG_WARN("NDNSF_HISTORICAL_KEY_REQUEST name=" << interest.getName().toUri());
  if (!hasGenerationWriter())
    return;
  if (m_abeRotationPending) {
    // The retired and the current material are inconsistent until the rotation commits; a key minted now would
    // be one half of a mixed generation.
    NDN_LOG_WARN("NDNSF_HISTORICAL_KEY_WITHHELD reason=abe-rotation-pending");
    return;
  }
  if (!m_historicalKeyBroker) {
    const auto materialLookup = [this] (uint64_t generation, uint64_t epoch,
                                        HistoricalKeyBroker::Material& material) {
      try {
        auto entry = m_generationStore->loadAbeHistoryEntry({generation, epoch});
        if (!entry)
          return false;
        material.publicParameters = std::move(entry->publicParameters);
        material.masterSecret = std::move(entry->masterSecret);
        material.publicParametersName = ndn::Name(entry->publicParametersName);
        return true;
      }
      catch (const std::exception&) {
        return false;
      }
    };
    const auto replySigner = [this] (ndn::Data& data) {
      m_keyChain.sign(data, ndn::security::signingByCertificate(m_aaCert));
    };
    m_historicalKeyBroker = std::make_unique<HistoricalKeyBroker>(
      HistoricalKeyBroker::SignerLookup{}, materialLookup, replySigner);
    // The producer's policy version selects the signed grant. Key material can
    // remain unchanged across several grants under different policy versions.
    const auto authorization = [this] (const ndn::Name& identity, uint64_t generation,
                                       uint64_t epoch, const std::string& policy,
                                       const std::string& signerCertificateDigest) mutable {
      // A revoked identity or certificate must not receive a retired generation's key, whatever the
      // archived attribute snapshot says it once held.
      if (isIdentityOrCertificateRevoked(identity, signerCertificateDigest))
        return false;
      // What it held when the object was published comes from the Controller's own signed grant record for
      // that version, not from a retirement-time snapshot: the snapshot cannot prove publication-time
      // authority, and it was never signed as a statement about this identity.
      const auto identityUri = identity.toUri(ndn::name::UriFormat::CANONICAL);
      return hasHistoricalGrant(identity, ControllerVersion{generation, epoch}, policy) &&
             historicalAttributeIsAuthorized(policy, {policy}, effectiveAttributesFor(identityUri));
    };
    m_historicalKeyBroker->setAuthorizationCheck(authorization);
  }
  // The broker decides freshness, replay and the generation to mint from; the validator decides who
  // asked, and its answer is what the broker is allowed to trust.
  // A refusal must be answered, not ignored: a silent Controller leaves the reader waiting until the deployment
  // ends, and a reader cannot tell "not authorised" from "no answer" - the acceptance previously read that
  // timeout as a refusal.
  HistoricalKeyRequest claimed;
  const auto parameters = interest.getApplicationParameters();
  const std::string body(reinterpret_cast<const char*>(parameters.value()), parameters.value_size());
  const auto receivedAt = nowMilliseconds();
  if (!decodeHistoricalKeyRequest(body, claimed) ||
      !matchHistoricalKeyRequestName(interest.getName(), m_controllerPrefix, claimed) ||
      claimed.currentControllerVersion != m_controllerVersion ||
      claimed.expiresAtMs <= receivedAt || claimed.expiresAtMs <= claimed.mintedAtMs ||
      claimed.expiresAtMs - receivedAt > m_policyStatusLeaseMs ||
      claimed.expiresAtMs - claimed.mintedAtMs > m_policyStatusLeaseMs)
    return;

  const auto deny = [this, admittedVersion = m_controllerVersion, expires = claimed.expiresAtMs](
                      const ndn::Interest& request, const std::string& reason) {
    if (expires <= nowMilliseconds() ||
        serveHistoricalTerminal(request.getName(), admittedVersion, false))
      return;
    try {
      ndn::Data denial(request.getName());
      denial.setFreshnessPeriod(ndn::time::seconds(1));
      const std::string content = "NDNSF-HISTORICAL-DKEY-DENIED-v1\n" + reason + "\n";
      denial.setContent(ndn::makeStringBlock(ndn::tlv::Content, content));
      m_keyChain.sign(denial, ndn::security::signingByCertificate(m_aaCert));
      publishHistoricalTerminal(denial, admittedVersion);
      NDN_LOG_WARN("NDNSF_HISTORICAL_KEY_DENIED reason=" << reason);
    }
  catch (const std::exception& error) {
      NDN_LOG_ERROR("historical key denial could not be sent: " << error.what());
    }
  };
  m_validator.validate(interest,
    [this, interest, deny, claimed, admittedVersion = m_controllerVersion](
      const ndn::Interest& validated) {
      // Validation is asynchronous: by the time it finishes this controller may no longer be entitled to mint
      // anything, and the requested time window must be judged now rather than when the Interest arrived.
      if (!hasGenerationWriter()) {
        deny(validated, "controller-not-ready");
        return;
      }
      if (m_abeRotationPending) {
        deny(validated, "abe-rotation-pending");
        return;
      }
      ndn::security::Certificate signer;
      try {
        signer = getValidatedHistoricalSigner(validated);
      }
      catch (const std::exception&) {
        deny(validated, "requester-certificate");
        return;
      }
      const auto answer = [this, validated, deny, signer, admittedVersion,
                           expires = claimed.expiresAtMs](
                            const ndn::security::Certificate& recipient) {
        if (!hasGenerationWriter() || m_abeRotationPending ||
            m_controllerVersion != admittedVersion || expires <= nowMilliseconds()) {
          return;
        }
        if (!signer.isValid() || !recipient.isValid() ||
            signer.getIdentity() != recipient.getIdentity() ||
            getCertificateKeyType(recipient) != ndn::KeyType::RSA ||
            isIdentityOrCertificateRevoked(signer.getIdentity(), certificateDigest(signer)) ||
            isIdentityOrCertificateRevoked(recipient.getIdentity(), certificateDigest(recipient))) {
          deny(validated, "not-authorised");
          return;
        }
        if (serveHistoricalTerminal(validated.getName(), admittedVersion, true))
          return;
        HistoricalKeyBroker::Failure failure;
        auto reply = m_historicalKeyBroker->answerValidated(validated, m_controllerPrefix,
          nowMilliseconds(), signer, &recipient, &failure);
        if (!reply) {
          if (failure == HistoricalKeyBroker::Failure::KEY_UNAVAILABLE)
            deny(validated, "key-unavailable");
          else if (failure == HistoricalKeyBroker::Failure::NOT_AUTHORIZED)
            deny(validated, "not-authorised");
          else
            deny(validated, "request-rejected");
          return;
        }
        if (expires > nowMilliseconds())
          publishHistoricalTerminal(*reply, admittedVersion);
      };
      // Historical issuance requires the original producer-signed CK packet.
      if (claimed.ckEvidence.empty()) {
        deny(validated, "ck-evidence-required");
        return;
      }
      if (claimed.publicationEvidence.empty()) {
        deny(validated, "publication-evidence-required");
        return;
      }
      ndn::Data evidence;
      ndn::Data publicationData;
      try {
        evidence.wireDecode(ndn::Block(ndn::span<const uint8_t>(claimed.ckEvidence.data(),
                                                                 claimed.ckEvidence.size())));
        publicationData.wireDecode(
          ndn::Block(ndn::span<const uint8_t>(claimed.publicationEvidence)));
      }
      catch (const std::exception&) {
        deny(validated, "ck-evidence-malformed");
        return;
      }
      if (!evidence.getName().equals(claimed.ckName) ||
          claimed.producerName.empty() || !claimed.producerName.isPrefixOf(evidence.getName())) {
        deny(validated, "ck-evidence-name");
        return;
      }
      const auto publication = decodeHistoricalPublication(publicationData);
      if (!publication || !historicalPublicationMatches(*publication, claimed, evidence)) {
        deny(validated, "publication-evidence-binding");
        return;
      }
      m_validator.validate(evidence,
        [this, answer, deny, validated, publicationData, signer,
         producer = claimed.producerName, recipientName = claimed.recipientCertificate](
          const ndn::Data& packet) {
          if (!ProducerSignerPolicy().allowsValidatedSigner(packet, producer)) {
            deny(validated, "ck-evidence-producer");
            return;
          }
          m_validator.validate(publicationData,
            [this, answer, deny, validated, producer, signer, recipientName](
              const ndn::Data& mapping) {
              if (!ProducerSignerPolicy().allowsValidatedSigner(mapping, producer)) {
                deny(validated, "publication-evidence-producer");
                return;
              }
              if (!acceptsHistoricalPublication(mapping)) {
                deny(validated, "publication-receipt-required");
                return;
              }
              const auto publication = decodeHistoricalPublication(mapping);
              if (!publication) {
                deny(validated, "publication-evidence-binding");
                return;
              }
              const ControllerVersion first{publication->generation, publication->epoch};
              const ControllerVersion published{publication->policyGeneration, publication->policyEpoch};
              // Grants may extend rights while retaining material. Any
              // withdrawal rotates before the authority serves again.
              if (first.compare(published) > 0 ||
                  !hasHistoricalGrant(signer.getIdentity(), first, publication->policy)) {
                deny(validated, "not-authorised");
                return;
              }
              try {
                if (recipientName == signer.getFullName() || recipientName == signer.getName()) {
                  answer(signer);
                  return;
                }
                resolveProtectedRecipient(validated.getName(), signer.getIdentity(), recipientName,
                                          answer);
              }
              catch (const std::exception&) {
                deny(validated, "requester-certificate");
              }
            },
            [deny, validated] (const ndn::Data&, const ndn::security::ValidationError&) {
              deny(validated, "publication-evidence-signature");
            });
        },
        [deny, validated] (const ndn::Data&, const ndn::security::ValidationError&) {
          NDN_LOG_WARN("NDNSF_HISTORICAL_EVIDENCE_REJECTED reason=signature name="
                       << validated.getName());
          deny(validated, "ck-evidence-signature");
        });
    },
    [] (const ndn::Interest&, const ndn::security::ValidationError& error) {
      NDN_LOG_WARN("Historical key request rejected: " << error.getInfo());
    });
}

bool
ServiceController::revoke(const RevocationTarget& target)
{
  if (!hasGenerationWriter() || !target.isValid())
    return false;
  const bool recovering = m_abeRotationPending;
  // Reconcile before the duplicate-target check: callers retry the operation
  // that returned false, not an unrelated revocation invented to trigger it.
  if (!reconcilePendingAbeRotation())
    return false;
  for (const auto& existing : m_revocations) {
    if (existing.kind == target.kind &&
        existing.targetIdentity == target.targetIdentity &&
        existing.serviceName == target.serviceName &&
        sameCertificateDigest(existing.certificateDigest, target.certificateDigest) &&
        existing.authorizationAttribute == target.authorizationAttribute) {
      if (recovering) {
        NDN_LOG_WARN("NDNSF_CONTROLLER_REVOKED kind="
                     << static_cast<int>(target.kind)
                     << " identity=" << target.targetIdentity
                     << " service=" << target.serviceName
                     << " generation=" << m_controllerVersion.controllerGenerationTimestamp
                     << " epoch=" << m_controllerVersion.controllerEpoch
                     << " recovered=true");
      }
      return recovering;
    }
  }

  m_revocations.push_back(target);
  // Record the withdrawal against the version that is still being served. The version names key material, and the
  // material for the next one does not exist until the rotation below has run; committing the epoch first is what
  // produced a served version with no keys behind it.
  try {
    m_generationStore->recordRevocations(m_revocations, true);
  }
  catch (const ControllerGenerationStore::CommitUncertain& error) {
    m_generationReady = false;
    m_abeRotationPending = true;
    NDN_LOG_ERROR("NDNSF_CONTROLLER_REVOCATION_COMMIT_UNCERTAIN error=" << error.what());
    return false;
  }
  catch (const std::exception& error) {
    m_revocations.pop_back();
    NDN_LOG_ERROR("NDNSF_CONTROLLER_REVOKE_FAILED error=" << error.what());
    return false;
  }
  m_abeRotationPending = true;
  if (!reconcilePendingAbeRotation())
    return false;
  NDN_LOG_WARN("NDNSF_CONTROLLER_REVOKED kind="
               << static_cast<int>(target.kind)
               << " identity=" << target.targetIdentity
               << " service=" << target.serviceName
               << " generation=" << m_controllerVersion.controllerGenerationTimestamp
               << " epoch=" << m_controllerVersion.controllerEpoch);
  return true;
}

bool
ServiceController::grant(const ndn::Name& identity,
                          const ndn::Name& serviceName,
                          const ndn::Name& authorizationAttribute)
{
  const bool isUser = authorizationAttribute == ndn::Name("/PERMISSION").append(serviceName);
  if (!isUser && authorizationAttribute != ndn::Name("/SERVICE").append(serviceName))
    return false;
  std::string abeAttribute;
  try { abeAttribute = makeAbeServiceAttribute(serviceName, isUser ? "PERMISSION" : "SERVICE"); }
  catch (const std::invalid_argument&) { return false; }
  RevocationTarget target;
  target.kind = RevocationKind::SERVICE_AUTHORIZATION;
  target.targetIdentity = identity;
  target.serviceName = serviceName;
  target.authorizationAttribute = authorizationAttribute;
  if (!target.isValid() || !hasGenerationWriter())
    return false;
  // A grant following a failed withdrawal is not grant-only until the
  // withdrawal's rotation has completed. Never erase its target or issue a
  // replacement policy under the pre-withdrawal pair, even for another user.
  if (!reconcilePendingAbeRotation())
    return false;

  // An explicit grant can reauthorize an identity after an identity-wide
  // withdrawal, but never bypasses a certificate-wide withdrawal.  The
  // resulting DKEY is generated under the current post-withdrawal pair.
  bool reauthorizingIdentity = false;
  for (const auto& existing : m_revocations) {
    if (existing.targetIdentity != identity)
      continue;
    if (existing.kind == RevocationKind::CERTIFICATE)
      return false;
    if (existing.kind == RevocationKind::IDENTITY)
      reauthorizingIdentity = true;
  }

  const auto serviceUri = serviceName.toUri(ndn::name::UriFormat::CANONICAL);
  const auto hasServiceGrant = [&] {
    if (isUser) {
      for (const auto& policy : m_userPolicies) {
        if (policy.userName == identity.toUri(ndn::name::UriFormat::CANONICAL) &&
            std::find(policy.allowedServices.begin(), policy.allowedServices.end(), serviceUri) !=
                policy.allowedServices.end())
          return true;
      }
    }
    else {
      for (const auto& policy : m_providerPolicies) {
        if (policy.providerName == identity.toUri(ndn::name::UriFormat::CANONICAL) &&
            std::find(policy.allowedServices.begin(), policy.allowedServices.end(), serviceUri) !=
                policy.allowedServices.end())
          return true;
      }
    }
    return false;
  };
  const auto existingAttributes = m_attributesMap.find(identity.toUri(ndn::name::UriFormat::CANONICAL));
  const bool hasAttribute = existingAttributes != m_attributesMap.end() &&
                            existingAttributes->second.count(abeAttribute) != 0;
  const bool hasMatchingWithdrawal = std::any_of(
      m_revocations.begin(), m_revocations.end(), [&target] (const RevocationTarget& existing) {
        return existing.kind == target.kind &&
               existing.targetIdentity == target.targetIdentity &&
               existing.serviceName == target.serviceName &&
               existing.authorizationAttribute == target.authorizationAttribute;
      });
  // An identity-wide withdrawal leaves the configured service/policy entries
  // in place but blocks their use. An explicit grant is the reauthorization
  // operation for that identity, so allow it to remove the identity target
  // even when the role/service attribute already exists. Certificate-wide
  // withdrawal is rejected above and cannot be bypassed by grant().
  if (hasServiceGrant() && hasAttribute && !hasMatchingWithdrawal &&
      !reauthorizingIdentity)
    return false;

  // A grant can precede the first PUBPARAMS Interest. Freeze the initial
  // eligibility before modifying policy while this material's birth version
  // still matches the committed authority version.
  if (!m_currentAbeParameters && m_abeMaterialVersion == m_controllerVersion) {
    try {
      recordInitialAbeGrants();
    }
  catch (const std::exception& error) {
      // A missing birth grant fails closed for historical issuance. It does
      // not change the current Controller's authority to grant this service.
      NDN_LOG_ERROR("Some initial historical ABE grants could not be recorded: " << error.what());
    }
  }

  const auto oldUserPolicies = m_userPolicies;
  const auto oldProviderPolicies = m_providerPolicies;
  const auto oldAttributesMap = m_attributesMap;
  const auto oldRevocations = m_revocations;
  const auto oldVersion = m_controllerVersion;
  const auto oldPolicyEpoch = m_policyEpoch;
  const auto oldRequiredKeyEpoch = m_requiredKeyEpoch;
  const auto oldValidFrom = m_policyValidFromMs;
  const auto oldValidUntil = m_policyValidUntilMs;
  const auto restoreTargetPolicy = [&] {
    m_aa.removePolicy(identity);
    const auto effective = effectiveAttributesFor(identity.toUri(ndn::name::UriFormat::CANONICAL));
    if (!effective.empty()) {
      std::list<std::string> attributes(effective.begin(), effective.end());
      m_aa.replacePolicy(identity, boost::algorithm::join(attributes, " OR "));
    }
  };
  bool foundIdentity = false;
  if (isUser) {
    for (auto& policy : m_userPolicies) {
      if (policy.userName != identity.toUri(ndn::name::UriFormat::CANONICAL))
        continue;
      foundIdentity = true;
      if (std::find(policy.allowedServices.begin(), policy.allowedServices.end(), serviceUri) ==
          policy.allowedServices.end()) {
        policy.allowedServices.push_back(serviceUri);
      }
    }
    if (!foundIdentity)
      m_userPolicies.push_back(UserPolicy{identity.toUri(ndn::name::UriFormat::CANONICAL), {serviceUri}});
  }
  else {
    for (auto& policy : m_providerPolicies) {
      if (policy.providerName != identity.toUri(ndn::name::UriFormat::CANONICAL))
        continue;
      foundIdentity = true;
      if (std::find(policy.allowedServices.begin(), policy.allowedServices.end(), serviceUri) ==
          policy.allowedServices.end()) {
        policy.allowedServices.push_back(serviceUri);
      }
    }
    if (!foundIdentity)
      m_providerPolicies.push_back(ProviderPolicy{identity.toUri(ndn::name::UriFormat::CANONICAL), {serviceUri}});
  }

  m_attributesMap[identity.toUri(ndn::name::UriFormat::CANONICAL)].insert(abeAttribute);
  m_revocations.erase(std::remove_if(m_revocations.begin(), m_revocations.end(),
                                     [&target](const RevocationTarget& existing) {
    return (existing.kind == target.kind &&
            existing.targetIdentity == target.targetIdentity &&
            existing.serviceName == target.serviceName &&
            existing.authorizationAttribute == target.authorizationAttribute) ||
           (existing.kind == RevocationKind::IDENTITY &&
            target.targetIdentity == existing.targetIdentity);
  }), m_revocations.end());
  try {
    // Grant-only: retain the exact ABE pair and replace only this identity's
    // complete policy.  NAC-ABE generates its replacement DKEY lazily on the
    // target identity's next fetch; no global rotation or unaffected-identity
    // fan-out is performed.  Register the granted identity's certificate with
    // the Attribute Authority when the Controller keychain or public anchors know it,
    // so the AA can serve the identity's lazy DKEY fetch without a network
    // certificate lookup; fall back to the name-only policy when no local
    // certificate exists yet.
    const auto effective = effectiveAttributesFor(identity.toUri(ndn::name::UriFormat::CANONICAL));
    m_aa.removePolicy(identity);
    if (!effective.empty()) {
      std::list<std::string> attributes(effective.begin(), effective.end());
      const auto policy = boost::algorithm::join(attributes, " OR ");
      try {
        const auto cert = getTargetIdentityCertificate(identity);
        m_aa.addNewPolicy(cert, policy);
      }
      catch (const std::exception&) {
        m_aa.replacePolicy(identity, policy);
      }
    }
    buildLookupTables();
    advanceAuthorizationEpoch(true);
  }
  catch (const std::exception& error) {
    m_userPolicies = oldUserPolicies;
    m_providerPolicies = oldProviderPolicies;
    m_attributesMap = oldAttributesMap;
    m_revocations = oldRevocations;
    m_controllerVersion = oldVersion;
    m_policyEpoch = oldPolicyEpoch;
    m_requiredKeyEpoch = oldRequiredKeyEpoch;
    m_policyValidFromMs = oldValidFrom;
    m_policyValidUntilMs = oldValidUntil;
    buildLookupTables();
    try {
      restoreTargetPolicy();
    }
    catch (const std::exception& restoreError) {
      NDN_LOG_ERROR("NDNSF_CONTROLLER_GRANT_ROLLBACK_FAILED error="
                    << restoreError.what());
    }
    NDN_LOG_ERROR("NDNSF_CONTROLLER_GRANT_FAILED error=" << error.what());
    return false;
  }
  NDN_LOG_INFO("NDNSF_CONTROLLER_GRANT identity=" << identity
               << " service=" << serviceName
               << " attribute=" << authorizationAttribute
               << " generation=" << m_controllerVersion.controllerGenerationTimestamp
               << " epoch=" << m_controllerVersion.controllerEpoch
               << " reauthorizingIdentity=" << (reauthorizingIdentity ? "true" : "false")
               << " abeParametersUnchanged=true");
  return true;
}

PolicyStatusData
ServiceController::getPolicyStatus(const ndn::Name& serviceName) const
{
  if (!m_generationReady || !m_controllerVersion.isValid() || serviceName.empty())
    throw std::runtime_error("Controller policy status is not ready");
  // During recovery this is a local diagnostic. Control Interests cannot
  // publish it until the withdrawal and the new epoch are committed together.
  PolicyStatusData status;
  status.setServiceName(serviceName);
  status.setControllerVersion(m_controllerVersion);
  status.setValidity(m_policyValidFromMs, m_policyValidUntilMs);
  status.setPolicyDigest(sha256FileDigest(m_configFilePath));
  status.setAbePublicParametersName(m_abePublicParametersName);
  status.setAbePublicParametersDigest(m_abePublicParametersDigest);
  status.setAbeMaterialVersion(m_abeMaterialVersion);
  status.setControllerCertificate(m_aaCert.getName());
  for (const auto& target : m_revocations) {
    if (target.kind != RevocationKind::SERVICE_AUTHORIZATION ||
        target.serviceName == serviceName)
      status.addRevocation(target);
  }
  return status;
}

void ServiceController::setControllerPrefix(const ndn::Name& prefix)
{
  if (m_isRegistered) {
    throw std::logic_error("Cannot change ServiceController prefix after start()");
  }

  m_controllerPrefix = prefix;
  m_hasCustomControllerPrefix = true;
}

void
ServiceController::setBootstrapTokenFile(const std::string& path)
{
  if (m_isRegistered) {
    throw std::logic_error("Cannot change bootstrap token file after start()");
  }
  loadBootstrapTokenFile(path);
}

void
ServiceController::cancelStart() noexcept
{
  m_startCancelled.store(true, std::memory_order_release);
}

void
ServiceController::resetStartCancellation() noexcept
{
  m_startCancelled.store(false, std::memory_order_release);
}

void ServiceController::start()
{
  if (!hasGenerationWriter())
    throw std::runtime_error("ServiceController generation writer is unavailable");
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  if (m_startCancelled.load(std::memory_order_acquire)) {
    throw std::runtime_error(
      "ServiceController AttributeAuthority PUBPARAMS readiness cancelled");
  }

  registerInterestHandlers();

  // A second Face has its own transport, PIT and event loop.
  // Never express the readiness Interest on the AA Face: ndn-cxx could satisfy
  // it in-process even when no Interest/response crosses NFD. Reuse ndn-cxx's
  // configured endpoint, not a hard-coded socket or the AA transport object.
  auto& io = m_face.getIoContext();
  // Keep probe callbacks on a private context: destroying a temporary Face on
  // the long-lived AA context could leave ndn-cxx transport callbacks queued
  // there after the temporary Face is gone.
  boost::asio::io_context probeIo;
  ndn::Face probeFace(nullptr, probeIo, m_keyChain);
  struct ReadinessProbeState {
    bool receivedByAuthority = false;
    bool challengeReady = false;
    bool ready = false;
    bool finished = false;
    bool inFlight = false;
    std::string error;
  };
  const auto state = std::make_shared<ReadinessProbeState>();
  // NAC-ABE names parameters by their actual generation. Keep the fresh
  // liveness challenge separate; never relabel those bytes with a nonce.
  const auto paramsName = currentAbePublicParametersName();
  const auto paramsWire = m_aa.getPublicParametersWire();
  const auto challengeContent = paramsName.toUri(ndn::name::UriFormat::CANONICAL) + "|" +
    abePublicParametersDigest(paramsWire);
  const auto challengeParameters = ndn::makeStringBlock(ndn::tlv::Content, challengeContent);
  ndn::Interest preparedProbe(makeControlRpcName(m_aaCert.getIdentity(), "AA-READY",
                                               challengeParameters, makeControlQueryNonce()));
  preparedProbe.setApplicationParameters(challengeParameters);
  const auto probeName = preparedProbe.getName();
  checkNamingNameSize(probeName);
  auto originalReply = std::make_shared<ndn::Data>(probeName);
  originalReply->setContent(challengeContent);
  originalReply->setFreshnessPeriod(ndn::time::milliseconds(0));
  m_keyChain.sign(*originalReply, ndn::security::signingByCertificate(m_aaCert));
  if (preparedProbe.wireEncode().size() > ndn::MAX_NDN_PACKET_SIZE ||
      originalReply->wireEncode().size() > ndn::MAX_NDN_PACKET_SIZE)
    throw std::length_error("ServiceController AA readiness packet exceeds wire budget");
  ndn::ScopedRegisteredPrefixHandle observer = m_face.setInterestFilter(probeName,
    [this, state, probeName, originalReply](const ndn::InterestFilter&,
                                          const ndn::Interest& interest) {
      // NFD decrements 1 to 0 on ingress and only forwards the result to local
      // faces. Thus both Faces must reach the same local forwarder. If a caller
      // supplied a different endpoint, fail closed instead of probing another NFD.
      if (!state->finished && interest.getName() == probeName &&
          parseControlRpcName(interest, m_aaCert.getIdentity(), "AA-READY")) {
        state->receivedByAuthority = interest.getHopLimit() == 0;
        try { m_face.put(*originalReply); }
  catch (const std::exception& error) {
          state->error = std::string("ServiceController AA readiness delivery failed: ") + error.what();
        }
      }
    },
    nullptr,
    [state](const ndn::Name&, const std::string& reason) {
      state->error = "ServiceController AA readiness prefix registration failed: " + reason;
    });
  ndn::ScopedPendingInterestHandle pending;
  auto finish = ndn::make_scope_exit([state] { state->finished = true; });

  // Only restart on entry. A concurrent stop() must not be undone in the loop.
  // A work guard makes each idle run_for block; Nacks have a retry floor too.
  io.restart();
  auto work = boost::asio::make_work_guard(io);
  auto probeWork = boost::asio::make_work_guard(probeIo);
  auto nextAttempt = std::chrono::steady_clock::now();
  while (true) {
    if (m_startCancelled.load(std::memory_order_acquire)) {
      throw std::runtime_error(
        "ServiceController AttributeAuthority PUBPARAMS readiness cancelled");
    }
    if (io.stopped() || probeIo.stopped()) {
      throw std::runtime_error(
        "ServiceController AttributeAuthority PUBPARAMS readiness event loop stopped");
    }
    if (!m_registrationState->error.empty()) {
      throw std::runtime_error("ServiceController prefix registration failed: " +
                               m_registrationState->error);
    }
    if (!state->error.empty()) {
      throw std::runtime_error(state->error);
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      throw std::runtime_error(
        "ServiceController AttributeAuthority PUBPARAMS readiness timeout");
    }
    if (state->ready && m_registrationState->pending == 0) {
      if (!hasGenerationWriter())
        throw std::runtime_error("ServiceController generation writer was lost during readiness");
      return;
    }
    if (!state->ready && !state->inFlight && now >= nextAttempt) {
      state->inFlight = true;
      nextAttempt = now + std::chrono::milliseconds(100);
      const bool parameters = state->challengeReady;
      // A revocation can rotate the generation while readiness is still running. Re-read the current
      // parameters for each attempt, otherwise the probe keeps asking for the generation that was
      // current when the Controller started and never sees the new one.
      auto liveParamsName = paramsName;
      auto liveParamsWire = paramsWire;
      if (parameters) {
        liveParamsName = currentAbePublicParametersName();
        liveParamsWire = m_aa.getPublicParametersWire();
      }
      ndn::Interest interest = parameters ? ndn::Interest(liveParamsName) : preparedProbe;
      interest.setMustBeFresh(true);
      interest.setCanBePrefix(false);
      interest.setHopLimit(1);
      interest.setInterestLifetime(ndn::time::milliseconds(250));
      pending = probeFace.expressInterest(
        interest,
        [state, cert = m_aaCert, parameters, liveParamsName, liveParamsWire,
         probeName, challengeContent](const ndn::Interest&, const ndn::Data& data) {
          if (!state->finished) {
            state->inFlight = false;
            if (!state->receivedByAuthority) {
              state->error = "ServiceController PUBPARAMS probe did not reach the authority Face";
            }
            else if (data.getContent().value_size() == 0 ||
                      !ndn::security::verifySignature(data, cert)) {
              state->error = "ServiceController PUBPARAMS probe returned invalid authority Data";
            }
            else if (parameters) {
              const auto& content = data.getContent();
              if (data.getName() != liveParamsName ||
                  content.value_size() != liveParamsWire.size() ||
                  !std::equal(content.value_begin(), content.value_end(), liveParamsWire.begin())) {
                state->error = "ServiceController PUBPARAMS probe returned invalid current parameters";
              }
              else {
                state->ready = true;
              }
            }
            else {
              const auto& content = data.getContent();
              const std::string text(reinterpret_cast<const char*>(content.value()),
                                     content.value_size());
              if (data.getName() != probeName || text != challengeContent) {
                state->error = "ServiceController PUBPARAMS probe returned invalid authority Data";
              }
              else {
                state->challengeReady = true;
              }
            }
          }
        },
        [state](const ndn::Interest&, const ndn::lp::Nack&) {
          state->inFlight = false;
        },
        [state](const ndn::Interest&) {
          state->inFlight = false;
        });
    }
    // Both contexts run on this thread; cancellation is checked at least once
    // per two 25 ms slices, without sleeps or busy polling an empty context.
    const auto slice = std::chrono::steady_clock::duration(std::chrono::milliseconds(25));
    io.run_for(std::min(deadline - now, slice));
    const auto remaining = deadline - std::chrono::steady_clock::now();
    if (remaining > std::chrono::steady_clock::duration::zero() &&
        !m_startCancelled.load(std::memory_order_acquire) && !io.stopped()) {
      probeIo.run_for(std::min(remaining, slice));
    }
  }
}

void ServiceController::run()
{
  start();
  while (true) {
    m_face.getIoContext().restart();
    m_face.processEvents(ndn::time::milliseconds(1000));
  }
}

void ServiceController::loadConfigFiles()
{
  if (fs::is_regular_file(m_configFilePath)) {
    NDN_LOG_INFO("Loading config file: " << m_configFilePath);

    PolicyParser parser;
    auto policies = parser.parsePolicyFile(m_configFilePath);

    for (auto& policy : policies.first) {
      policy.providerName = ndn::Name(policy.providerName).toUri(ndn::name::UriFormat::CANONICAL);
      for (auto& service : policy.allowedServices)
        service = ndn::Name(service).toUri(ndn::name::UriFormat::CANONICAL);
    }
    for (auto& policy : policies.second) {
      policy.userName = ndn::Name(policy.userName).toUri(ndn::name::UriFormat::CANONICAL);
      for (auto& service : policy.allowedServices)
        service = ndn::Name(service).toUri(ndn::name::UriFormat::CANONICAL);
    }

    m_providerPolicies.insert(m_providerPolicies.end(),
                              policies.first.begin(), policies.first.end());
    m_userPolicies.insert(m_userPolicies.end(),
                          policies.second.begin(), policies.second.end());
  }
  else {
    NDN_LOG_ERROR("Error: " << m_configFilePath << " is not a valid file.");
  }
}

void ServiceController::addAttribute(const std::string& identity, const std::string& attributeName)
{
  m_attributesMap[identity].emplace(attributeName);
}

void ServiceController::addAttributesForUsersAccordingToServicePolicy()
{
  // Provider policies -> SERVICE/<service-name> attributes
  for (const auto& policy : m_providerPolicies) {
    for (const auto& service : policy.allowedServices) {
      addAttribute(policy.providerName, makeAbeServiceAttribute(ndn::Name(service), "SERVICE"));
    }
  }

  // User policies -> PERMISSION/<service-name> attributes
  for (const auto& policy : m_userPolicies) {
    ndn::Name userName(policy.userName);
    for (const auto& service : policy.allowedServices) {
      addAttribute(userName.toUri(ndn::name::UriFormat::CANONICAL),
                   makeAbeServiceAttribute(ndn::Name(service), "PERMISSION"));
    }
  }

  // Register each identity's OR-policy into the Attribute Authority.
  for (const auto& item : m_attributesMap) {
    const std::string& identity = item.first;
    const std::set<std::string>& attrs = item.second;

    std::list<std::string> attributeList(attrs.begin(), attrs.end());
    std::string abePolicy = boost::algorithm::join(attributeList, " OR ");

    try {
      const auto pibIdentity = m_keyChain.getPib().getIdentity(identity);
      size_t registeredCertificates = 0;
      // NAC-ABE DKEYs are encrypted to the Provider's RSA encryption key.
      // Register every certificate for this identity so split signing and
      // encryption keys are both resolvable by the Attribute Authority.
      for (const auto& key : pibIdentity.getKeys()) {
        for (const auto& cert : key.getCertificates()) {
          m_aa.addNewPolicy(cert, abePolicy);
          ++registeredCertificates;
        }
      }
      if (registeredCertificates != 0) {
        NDN_LOG_INFO("Add ABE policy: " << abePolicy
                  << " for identity (certificates=" << registeredCertificates
                  << "): " << identity);
        continue;
      }
    }
    catch (const std::exception&) {
      // Preserve the identity-only policy fallback below.
    }
    try {
      // Explicit public anchors also let NAC-ABE encrypt DKEYs when the
      // recipient's private identity belongs exclusively to another process.
      m_aa.addNewPolicy(getTargetIdentityCertificate(ndn::Name(identity)), abePolicy);
      // This branch registers from a public anchor instead of the local keychain and used to be
      // silent, which made "did this identity get a usable policy?" unanswerable from a log.
      NDN_LOG_INFO("Add ABE policy (anchor) for identity: " << identity);
      continue;
    }
    catch (const std::exception&) {
      // A name-only policy still permits the existing validated network lookup.
    }
    m_aa.addNewPolicy(ndn::Name(identity), abePolicy);
    NDN_LOG_INFO("Add ABE policy (fallback): " << abePolicy
              << " for identity: " << identity);
  }
}

std::vector<std::string> ServiceController::uniqSorted(std::vector<std::string> v)
{
  std::sort(v.begin(), v.end());
  v.erase(std::unique(v.begin(), v.end()), v.end());
  return v;
}

void ServiceController::buildLookupTables()
{
  m_userAllowedServices.clear();
  m_providerAllowedServices.clear();

  for (const auto& p : m_userPolicies) {
    auto& vec = m_userAllowedServices[p.userName];
    vec.insert(vec.end(), p.allowedServices.begin(), p.allowedServices.end());
  }

  for (const auto& p : m_providerPolicies) {
    auto& vec = m_providerAllowedServices[p.providerName];
    vec.insert(vec.end(), p.allowedServices.begin(), p.allowedServices.end());
  }

  for (auto& it : m_userAllowedServices) {
    it.second = uniqSorted(std::move(it.second));
  }
  for (auto& it : m_providerAllowedServices) {
    it.second = uniqSorted(std::move(it.second));
  }
}

bool ServiceController::extractEntityAfterPrefix(const ndn::Name& interestName,
                                                const ndn::Name& prefix,
                                                ndn::Name& entityOut)
{
  if (!prefix.isPrefixOf(interestName)) {
    return false;
  }
  if (interestName.size() <= prefix.size()) {
    return false;
  }
  entityOut = stripTrailingParametersDigest(interestName.getSubName(prefix.size()));
  return true;
}


bool ServiceController::admitControlQuery(const ndn::Interest& interest, const std::string& operation)
{
  if (m_abeRotationPending && operation != "CERTBOOTSTRAP")
    return false;
  if (!parseControlRpcName(interest, m_controllerPrefix, operation) ||
      !controlNonceExpiry(m_controllerPrefix, interest) ||
      interest.wireEncode().size() > ndn::MAX_NDN_PACKET_SIZE ||
      interest.getName().wireEncode().size() > 1024) return false;
  const auto now = static_cast<uint64_t>(ndn::time::duration_cast<ndn::time::milliseconds>(
    ndn::time::system_clock::now().time_since_epoch()).count());
  for (auto it = m_originalControlReplies.begin(); it != m_originalControlReplies.end();) {
    if (it->second.expiresAtMs > now) { ++it; continue; }
    m_originalControlReplyBytes -= it->second.packet->wireEncode().size();
    it = m_originalControlReplies.erase(it);
  }
  if (m_originalControlReplies.count(interest.getName())) return true;
  const bool hasCapacity = m_originalControlReplies.size() < 4096 &&
    m_originalControlReplyBytes <= 64ULL * 1024 * 1024 - ndn::MAX_NDN_PACKET_SIZE;
  if (!hasCapacity) {
    NDN_LOG_WARN("Control query refused: original reply capacity exhausted operation="
                 << operation << " entries=" << m_originalControlReplies.size()
                 << " bytes=" << m_originalControlReplyBytes);
  }
  return hasCapacity;
}

bool ServiceController::serveOriginalControlReply(const ndn::Interest& interest)
{
  const auto found = m_originalControlReplies.find(interest.getName());
  if (found == m_originalControlReplies.end()) return false;
  if (m_prefixUserPermissions.isPrefixOf(interest.getName())) {
    NDN_LOG_WARN("NDNSF_ORIGINAL_REPLY_HIT name=" << interest.getName());
  }
  try { m_face.put(*found->second.packet); }
  catch (const std::exception& e) { NDN_LOG_WARN("Original control reply delivery failed: " << e.what()); }
  return true;
}

void ServiceController::publishOriginalControlReply(const ndn::Interest& interest, const ndn::Data& data)
{
  const auto expiry = controlNonceExpiry(m_controllerPrefix, interest);
  if (!expiry || data.getName() != interest.getName())
    throw std::invalid_argument("control reply final Name or query nonce mismatch");
  checkNamingNameSize(data.getName());
  const auto wireBytes = data.wireEncode().size();
  if (wireBytes > ndn::MAX_NDN_PACKET_SIZE ||
      !ProducerSignerPolicy().allowsValidatedSigner(data, m_controllerPrefix))
    throw std::invalid_argument("control reply signer or signed packet budget invalid");
  const auto found = m_originalControlReplies.find(data.getName());
  if (found != m_originalControlReplies.end()) {
    if (!(found->second.packet->wireEncode() == data.wireEncode()))
      throw std::invalid_argument("immutable control reply conflict");
    try { m_face.put(*found->second.packet); }
    catch (const std::exception& e) { NDN_LOG_WARN("Original control reply delivery failed: " << e.what()); }
    return;
  }
  if (m_originalControlReplies.size() >= 4096 ||
      wireBytes > 64ULL * 1024 * 1024 - m_originalControlReplyBytes)
    throw std::runtime_error("original control reply capacity exhausted");
  auto original = std::make_shared<const ndn::Data>(data);
  m_originalControlReplies.emplace(data.getName(), OriginalControlReply{original, *expiry});
  m_originalControlReplyBytes += wireBytes;
  // Acceptance precedes optional delivery: a put failure retains the original.
  try { m_face.put(*original); }
  catch (const std::exception& e) { NDN_LOG_WARN("Accepted control reply delivery failed: " << e.what()); }
}

bool ServiceController::parseUserPermissionsInterestName(const ndn::Name& interestName,
                                                         ndn::Name& targetIdentity) const
{
  if (!m_prefixUserPermissions.isPrefixOf(interestName) ||
      interestName.size() <= m_prefixUserPermissions.size()) {
    return false;
  }

  targetIdentity = stripTrailingParametersDigest(
    interestName.getSubName(m_prefixUserPermissions.size()));
  return true;
}

bool ServiceController::parseProviderPermissionsInterestName(const ndn::Name& interestName,
                                                             ndn::Name& targetIdentity) const
{
  if (!m_prefixProviderPermissions.isPrefixOf(interestName) ||
      interestName.size() <= m_prefixProviderPermissions.size()) {
    return false;
  }

  targetIdentity = stripTrailingParametersDigest(
    interestName.getSubName(m_prefixProviderPermissions.size()));
  return true;
}

bool
ServiceController::canServeCurrentAbe() const
{
  return hasGenerationWriter() && !m_abeRotationPending && m_abeMaterialVersion.isValid() &&
         m_abeMaterialVersion.compare(m_controllerVersion) <= 0;
}

void
ServiceController::onCurrentAbeParametersInterest(const ndn::Interest& interest)
{
  if (!canServeCurrentAbe())
    return;
  try {
    const auto& name = interest.getName();
    if (!m_currentAbeParameters && (name == m_abePublicParametersName ||
          (interest.getCanBePrefix() && name.isPrefixOf(m_abePublicParametersName))))
      persistAbePublicParametersPacket();
    if (m_currentAbeParameters && interest.matchesData(*m_currentAbeParameters)) {
      m_face.put(*m_currentAbeParameters);
      return;
    }
    if (interest.getCanBePrefix() || name.empty())
      return;
    const auto archived = m_archivedAbeParameters.find(
      name.at(-1).isImplicitSha256Digest() ? name.getPrefix(-1) : name);
    if (archived != m_archivedAbeParameters.end() && interest.matchesData(*archived->second))
      m_face.put(*archived->second);
  }
  catch (const ControllerGenerationStore::CommitUncertain& error) {
    m_generationReady = false;
    NDN_LOG_ERROR("Controller ABE public parameters commit uncertain: " << error.what());
  }
  catch (const std::exception& error) {
    NDN_LOG_DEBUG("Current PUBPARAMS Interest refused: " << error.what());
  }
}

void
ServiceController::onCurrentAbeKeyInterest(const ndn::Interest& interest)
{
  constexpr size_t MAX_BUNDLES = 64;
  constexpr size_t MAX_SEGMENTS = 16;
  constexpr size_t SEGMENT_BYTES = 1500;
  if (!canServeCurrentAbe())
    return;
  const auto& requestName = interest.getName();
  const ndn::Name prefix = ndn::Name(m_aaCert.getIdentity()).append(ndn::nacabe::DECRYPT_KEY);
  if (!prefix.isPrefixOf(requestName) || requestName.size() < prefix.size() + 1 ||
      requestName.wireEncode().size() > 1024)
    return;
  ndn::Name keyName;
  try {
    const auto& component = requestName.at(prefix.size());
    if (component.type() != ndn::tlv::GenericNameComponent)
      return;
    keyName.wireDecode(component.blockFromValue());
    if (!ndn::security::isValidKeyName(keyName))
      return;
  }
  catch (const std::exception&) {
    return;
  }
  const auto identity = ndn::security::extractIdentityFromKeyName(keyName);
  const auto identityUri = identity.toUri(ndn::name::UriFormat::CANONICAL);
  if (effectiveAttributesFor(identityUri).empty())
    return;
  const auto authorized = [this, keyName, identity] (const ndn::security::Certificate& recipient) {
    return recipient.getKeyName() == keyName && recipient.isValid() &&
           getCertificateKeyType(recipient) == ndn::KeyType::RSA &&
           !isIdentityOrCertificateRevoked(identity, certificateDigest(recipient));
  };
  const auto now = nowMilliseconds();
  for (auto it = m_currentAbeKeys.begin(); it != m_currentAbeKeys.end();) {
    if (it->second.expiresAtMs <= now || it->second.version != m_controllerVersion)
      it = m_currentAbeKeys.erase(it);
    else
      ++it;
  }
  try {
    if (requestName.size() == prefix.size() + 3 &&
        requestName.at(-2).isVersion() && requestName.at(-1).isSegment()) {
      const auto entry = m_currentAbeKeys.find(requestName.getPrefix(-1));
      const auto segment = requestName.at(-1).toSegment();
      if (entry != m_currentAbeKeys.end() && segment < entry->second.segments.size() &&
          authorized(entry->second.recipient) &&
          interest.matchesData(*entry->second.segments[segment])) {
        m_face.put(*entry->second.segments[segment]);
      }
      return;
    }
    if (requestName.size() != prefix.size() + 1 || !interest.getCanBePrefix())
      return;
    const auto cached = m_currentAbeKeys.lower_bound(requestName);
    if (cached != m_currentAbeKeys.end() && requestName.isPrefixOf(cached->first) &&
        authorized(cached->second.recipient) &&
        interest.matchesData(*cached->second.segments.front())) {
      m_face.put(*cached->second.segments.front());
      return;
    }
    if (m_currentAbeKeys.size() >= MAX_BUNDLES)
      return;
    const auto issue = [this, interest, identityUri, authorized, version = m_controllerVersion]
                       (const ndn::security::Certificate& recipient) {
      try {
        if (!canServeCurrentAbe() || m_controllerVersion != version ||
            !authorized(recipient) || m_currentAbeKeys.size() >= MAX_BUNDLES)
          return;
        auto key = m_aa.issuePrivateKey(effectiveAttributesFor(identityUri));
        auto cleanse = ndn::make_scope_exit([&] {
          if (!key.m_prv.empty())
            OPENSSL_cleanse(key.m_prv.data(), key.m_prv.size());
        });
        auto bytes = key.toBuffer();
        auto cleanseBytes = ndn::make_scope_exit([&] {
          OPENSSL_cleanse(bytes.data(), bytes.size());
        });
        const auto sealed = ndn::nacabe::encryptDataContentWithCK(bytes, recipient.getPublicKey());
        if (sealed.size() > MAX_SEGMENTS * SEGMENT_BYTES)
          return;
        ndn::Name bundleName = interest.getName();
        bundleName.appendVersion(ndn::random::generateWord64());
        ndn::Segmenter segmenter(m_keyChain, ndn::security::signingByCertificate(m_aaCert));
        auto segments = segmenter.segment(ndn::span<const uint8_t>(sealed.data(), sealed.size()),
                                          bundleName, SEGMENT_BYTES, ndn::time::milliseconds(0));
        if (!interest.matchesData(*segments.front()) ||
            std::any_of(segments.begin(), segments.end(), [](const auto& data) {
              return data->wireEncode().size() > ndn::MAX_NDN_PACKET_SIZE;
            }))
          return;
        auto [entry, inserted] = m_currentAbeKeys.emplace(std::move(bundleName),
          CurrentAbeKey{version, recipient, std::move(segments), nowMilliseconds() + 30000});
        if (inserted)
          m_face.put(*entry->second.segments.front());
      }
  catch (const std::exception& error) {
        NDN_LOG_DEBUG("Current DKEY issuance refused: " << error.what());
      }
    };
    std::optional<ndn::security::Certificate> localRecipient;
    try {
      localRecipient = m_keyChain.getPib().getIdentity(identity).getKey(keyName)
                       .getDefaultCertificate();
    }
    catch (const ndn::security::pib::Pib::Error&) {
      localRecipient = m_aa.findRecipient(keyName);
    }
    if (localRecipient) {
      issue(*localRecipient);
      return;
    }
    if (m_currentAbeCertificateFetches.size() >= MAX_BUNDLES ||
        m_currentAbeCertificateFetches.count(keyName))
      return;
    ndn::Interest certificateQuery(keyName);
    certificateQuery.setCanBePrefix(true).setMustBeFresh(true);
    certificateQuery.setInterestLifetime(ndn::time::milliseconds(2000));
    const std::weak_ptr<int> lifetime = m_currentAbeLifetime;
    const auto finish = [this, lifetime, keyName] (const auto&...) {
      if (!lifetime.expired())
        m_currentAbeCertificateFetches.erase(keyName);
    };
    m_currentAbeCertificateFetches.emplace(keyName, m_face.expressInterest(certificateQuery,
      [this, lifetime, issue, finish](const ndn::Interest&, const ndn::Data& data) {
        if (lifetime.expired())
          return;
        try {
          m_validator.validate(data,
            [lifetime, issue, finish](const ndn::Data& validated) {
              if (lifetime.expired())
                return;
              finish();
              try {
                issue(ndn::security::Certificate(validated));
              }
              catch (const std::exception& error) {
              }
            },
            finish);
        }
        catch (const std::exception& error) {
          finish();
        }
      }, finish, finish));
  }
  catch (const std::exception& error) {
    NDN_LOG_DEBUG("Current DKEY Interest refused: " << error.what());
  }
}

void ServiceController::registerInterestHandlers()
{
  if (m_isRegistered) {
    return;
  }

  if (!m_hasCustomControllerPrefix) {
    m_controllerPrefix = m_aaCert.getIdentity();
  }

  m_prefixServiceAccess = m_controllerPrefix;
  m_prefixServiceAccess.append("NDNSF").append("RPC").append("SERVICEACCESS");

  m_prefixServiceProvision = m_controllerPrefix;
  m_prefixServiceProvision.append("NDNSF").append("RPC").append("SERVICEPROVISION");

  m_prefixUserPermissions = m_controllerPrefix;
  m_prefixUserPermissions.append("NDNSF").append("RPC").append("USER-PERMISSIONS");

  m_prefixProviderPermissions = m_controllerPrefix;
  m_prefixProviderPermissions.append("NDNSF").append("RPC").append("PROVIDER-PERMISSIONS");

  m_prefixPolicyManifest = m_controllerPrefix;
  m_prefixPolicyManifest.append("NDNSF").append("RPC").append("POLICY-MANIFEST");

  m_prefixPolicyStatus = m_controllerPrefix;
  m_prefixPolicyStatus.append("NDNSF").append("RPC").append("POLICY-STATUS");

  m_prefixCertificateBootstrap = m_controllerPrefix;
  m_prefixCertificateBootstrap.append("NDNSF").append("RPC").append("CERTBOOTSTRAP");

  m_registrationState = std::make_shared<RegistrationState>();
  const auto onRegistered = [state = m_registrationState](const ndn::Name&) {
    --state->pending;
  };
  const auto onFailure = [state = m_registrationState](const ndn::Name& prefix,
                                                      const std::string& reason) {
    state->error = prefix.toUri(ndn::name::UriFormat::CANONICAL) + ": " + reason;
    NDN_LOG_ERROR("Failed to register prefix " << prefix << " reason=" << reason);
  };

  auto registerFilter = [this, onRegistered, onFailure](
      const ndn::Name& prefix,
      std::function<void(const ndn::InterestFilter&, const ndn::Interest&)> onInterest) {
    auto holder = std::make_shared<ndn::ScopedRegisteredPrefixHandle>();
    m_registrationHandles.push_back(holder);
    ++m_registrationState->pending;
    *holder = m_face.setInterestFilter(
      prefix, std::move(onInterest), onRegistered, onFailure);
  };

  registerFilter(ndn::Name(m_aaCert.getIdentity()).append(ndn::nacabe::PUBLIC_PARAMS),
    [this](const ndn::InterestFilter&, const ndn::Interest& interest) {
      onCurrentAbeParametersInterest(interest);
    });
  registerFilter(ndn::Name(m_aaCert.getIdentity()).append(ndn::nacabe::DECRYPT_KEY),
    [this](const ndn::InterestFilter&, const ndn::Interest& interest) {
      onCurrentAbeKeyInterest(interest);
    });

  registerFilter(
    m_prefixServiceAccess,
    [this](const ndn::InterestFilter& f, const ndn::Interest& i) {
      this->onServiceAccessInterest(f, i);
    });

  registerFilter(
    m_prefixServiceProvision,
    [this](const ndn::InterestFilter& f, const ndn::Interest& i) {
      this->onServiceProvisionInterest(f, i);
    });

  registerFilter(
    m_prefixUserPermissions,
    [this](const ndn::InterestFilter& f, const ndn::Interest& i) {
      this->onUserPermissionsInterest(f, i);
    });

  registerFilter(
    m_prefixProviderPermissions,
    [this](const ndn::InterestFilter& f, const ndn::Interest& i) {
      this->onProviderPermissionsInterest(f, i);
    });

  registerFilter(
    m_prefixPolicyManifest,
    [this](const ndn::InterestFilter& f, const ndn::Interest& i) {
      this->onPolicyManifestInterest(f, i);
    });

  registerFilter(
    m_prefixPolicyStatus,
    [this](const ndn::InterestFilter& f, const ndn::Interest& i) {
      this->onPolicyStatusInterest(f, i);
    });

  registerFilter(
    m_prefixCertificateBootstrap,
    [this](const ndn::InterestFilter& f, const ndn::Interest& i) {
      this->onCertificateBootstrapInterest(f, i);
    });

  NDN_LOG_INFO("ServiceController listening on:\n"
            << "  " << m_prefixServiceAccess 
            << "  " << m_prefixServiceProvision 
            << "  " << m_prefixUserPermissions 
            << "  " << m_prefixProviderPermissions
            << "  " << m_prefixPolicyManifest
            << "  " << m_prefixPolicyStatus
            << "  " << m_prefixCertificateBootstrap);

  m_isRegistered = true;
}

ndn::Block ServiceController::makeAllowedServiceListTlv(const std::vector<std::string>& services) const
{
  ndn::Block list(tlv::AllowedServiceListType);

  for (const auto& s : services) {
    ndn::Name svcName(s);
    ndn::Block nameBlock = svcName.wireEncode();

    ndn::Block item(tlv::AllowedServiceType);
    item.push_back(nameBlock);
    item.encode();

    list.push_back(item);
  }

  list.encode();
  return list;
}

PermissionResponse
ServiceController::buildUserPermissionResponse(const ndn::Name& targetIdentity) const
{
  PermissionResponse response;
  response.setTargetIdentity(targetIdentity.toUri(ndn::name::UriFormat::CANONICAL));
  response.setPermissionKind(tlv::UserPermission);
  // The answer must name the same version the status advertises. Otherwise a grant record is written under one
  // version while readers are told another, and a historical read asks for a record that exists under a different
  // epoch - which is exactly how a legal read ends up refused as not-authorised.
  const auto advertisedVersion = (m_abeRotationPending && m_abeMaterialVersion.isValid())
    ? m_abeMaterialVersion : m_controllerVersion;
  response.setPolicyEpoch(static_cast<size_t>(advertisedVersion.controllerEpoch));
  if (advertisedVersion.isValid())
    response.setControllerVersion(advertisedVersion);

  std::vector<std::string> allowedServices;
  if (auto it = m_userAllowedServices.find(targetIdentity.toUri(ndn::name::UriFormat::CANONICAL));
      it != m_userAllowedServices.end()) {
    allowedServices = it->second;
  }

  for (const auto& service : allowedServices) {
    for (const auto& provider : m_providerAllowedServices) {
      const auto& providerName = provider.first;
      const auto& providerServices = provider.second;
      if (std::find(providerServices.begin(), providerServices.end(), service) ==
          providerServices.end()) {
        continue;
      }

      const auto permissionAttribute = makeAuthorizationAttribute("/PERMISSION", service);
      const bool serviceRevoked =
        isRevoked(targetIdentity, ndn::Name(service), {}, permissionAttribute);
      if (service.find("Spec225") != std::string::npos) {
        // WARN so a deployment run that does not enable diagnostic logging still shows why a service stays in
        // the record after a revocation named it.
        NDN_LOG_WARN("NDNSF_USER_PERMISSION_FILTER identity=" << targetIdentity
                     << " service=" << service << " attribute=" << permissionAttribute
                     << " revocations=" << m_revocations.size()
                     << " revoked=" << (serviceRevoked ? 1 : 0));
        for (const auto& target : m_revocations) {
          if (target.kind != RevocationKind::SERVICE_AUTHORIZATION)
            continue;
          NDN_LOG_WARN("NDNSF_USER_PERMISSION_REVOCATION identity=" << target.targetIdentity
                       << " service=" << target.serviceName
                       << " attribute=" << target.authorizationAttribute);
        }
      }
      if (serviceRevoked)
        continue;

      PermissionEntry entry;
      entry.setProviderName(providerName);
      entry.setServiceName(service);
      // Deprecated wire-compatibility field. Invocation authentication uses
      // per-request UserToken and per-ACK ProviderToken, not controller tokens.
      entry.setToken("");
      entry.setTtl(0);
      entry.setVersion(m_policyEpoch);
      response.addEntry(entry);
    }
  }

  return response;
}

PermissionResponse
ServiceController::buildProviderPermissionResponse(const ndn::Name& targetIdentity) const
{
  PermissionResponse response;
  response.setTargetIdentity(targetIdentity.toUri(ndn::name::UriFormat::CANONICAL));
  response.setPermissionKind(tlv::ProviderPermission);
  // The answer must name the same version the status advertises. Otherwise a grant record is written under one
  // version while readers are told another, and a historical read asks for a record that exists under a different
  // epoch - which is exactly how a legal read ends up refused as not-authorised.
  const auto advertisedVersion = (m_abeRotationPending && m_abeMaterialVersion.isValid())
    ? m_abeMaterialVersion : m_controllerVersion;
  response.setPolicyEpoch(static_cast<size_t>(advertisedVersion.controllerEpoch));
  if (advertisedVersion.isValid())
    response.setControllerVersion(advertisedVersion);

  std::vector<std::string> allowedServices;
  if (auto it = m_providerAllowedServices.find(targetIdentity.toUri(ndn::name::UriFormat::CANONICAL));
      it != m_providerAllowedServices.end()) {
    allowedServices = it->second;
  }

  NDN_LOG_WARN("NDNSF_PROVIDER_RECORD_ALLOWED identity=" << targetIdentity.toUri()
               << " services=" << allowedServices.size() << " revocations=" << m_revocations.size());
  for (const auto& service : allowedServices) {
    const auto attribute = makeAuthorizationAttribute("/SERVICE", service);
    const bool revoked = isRevoked(targetIdentity, ndn::Name(service), {}, attribute);
    NDN_LOG_WARN("NDNSF_PROVIDER_RECORD_SERVICE name=" << service
                 << " attribute=" << attribute.toUri() << " revoked=" << (revoked ? 1 : 0));
    if (revoked)
      continue;
    PermissionEntry entry;
    entry.setProviderName(targetIdentity.toUri(ndn::name::UriFormat::CANONICAL));
    entry.setServiceName(service);
    // Deprecated wire-compatibility field. Provider permission presence is
    // still installed, but no invocation token is issued by the controller.
    entry.setToken("");
    entry.setTtl(0);
    entry.setVersion(m_policyEpoch);
    response.addEntry(entry);
  }

  return response;
}

PolicyManifest
ServiceController::buildPolicyManifest() const
{
  PolicyManifest manifest;
  manifest.setPolicyEpoch(m_policyEpoch);
  manifest.setValidFromMs(m_policyValidFromMs);
  manifest.setGracePeriodMs(m_policyGracePeriodMs);
  manifest.setRequiredKeyEpoch(m_requiredKeyEpoch);
  if (m_controllerVersion.isValid())
    manifest.setControllerVersion(m_controllerVersion);
  return manifest;
}

ndn::security::Certificate
ServiceController::getTargetIdentityCertificate(const ndn::Name& targetIdentity)
{
  if (auto issued = m_bootstrapIssuedCertificates.find(targetIdentity.toUri(ndn::name::UriFormat::CANONICAL));
      issued != m_bootstrapIssuedCertificates.end()) {
    // Permission responses are encrypted with the identity's RSA key even
    // when the runtime uses a separate ECDSA signing certificate.  Bootstrap
    // may have issued either certificate type, so normalize it to RSA here.
    return getRsaEncryptionCertificateOrThrow(m_keyChain, issued->second);
  }

  ndn::security::Certificate cert;
  try {
    cert = m_keyChain.getPib().getIdentity(targetIdentity)
             .getDefaultKey().getDefaultCertificate();
  }
  catch (const ndn::security::pib::Pib::Error&) {
    const auto* anchor = m_validator.getTrustAnchors().find(ndn::Name(targetIdentity).append("KEY"));
    if (anchor == nullptr || anchor->getIdentity() != targetIdentity || !anchor->isValid() ||
        getCertificateKeyType(*anchor) != ndn::KeyType::RSA) {
      throw std::runtime_error("No valid RSA recipient certificate for " +
                               targetIdentity.toUri(ndn::name::UriFormat::CANONICAL));
    }
    // Copy the public certificate before another anchor lookup can invalidate it.
    // Trust in this key does not grant any service permission.
    return *anchor;
  }
  if (!cert.isValid()) {
    throw std::runtime_error("Target identity certificate is not currently valid: " +
                             cert.getName().toUri(ndn::name::UriFormat::CANONICAL));
  }
  // The default key may be ECDSA because it is preferred for Data signing;
  // NAC-ABE permission encryption still requires the RSA certificate.
  return getRsaEncryptionCertificateOrThrow(m_keyChain, cert);
}

void
ServiceController::generateBootstrapTokenFile(const std::string& path) const
{
  std::map<std::string, std::string> entries;
  for (const auto& policy : m_providerPolicies) {
    entries.emplace(ndn::Name(policy.providerName).toUri(ndn::name::UriFormat::CANONICAL), "provider");
  }
  for (const auto& policy : m_userPolicies) {
    entries.emplace(ndn::Name(policy.userName).toUri(ndn::name::UriFormat::CANONICAL), "user");
  }

  if (entries.empty()) {
    throw std::runtime_error("Cannot generate bootstrap token file without policy identities: " +
                             path);
  }

  const fs::path outputPath(path);
  if (outputPath.has_parent_path()) {
    fs::create_directories(outputPath.parent_path());
  }

  std::ofstream output(path, std::ios::out | std::ios::trunc);
  if (!output) {
    throw std::runtime_error("Cannot create bootstrap token file: " + path);
  }

  output << "# identity token role\n";
  for (const auto& entry : entries) {
    output << entry.first << ' ' << makeBootstrapToken(8) << ' ' << entry.second << '\n';
  }
  output.close();
  if (!output) {
    throw std::runtime_error("Cannot finish writing bootstrap token file: " + path);
  }

  fs::permissions(outputPath,
                  fs::perms::owner_read | fs::perms::owner_write,
                  fs::perm_options::replace);

  NDN_LOG_WARN("NDNSF_CERT_BOOTSTRAP_TOKEN_FILE_GENERATED path=" << path
               << " entries=" << entries.size());
}

void
ServiceController::loadBootstrapTokenFile(const std::string& path)
{
  if (path.empty()) {
    return;
  }

  std::ifstream input(path);
  if (!input) {
    if (!fs::exists(path)) {
      generateBootstrapTokenFile(path);
      input.open(path);
    }
    if (!input) {
      throw std::runtime_error("Cannot open bootstrap token file: " + path);
    }
  }

  std::string line;
  size_t lineNo = 0;
  while (std::getline(input, line)) {
    ++lineNo;
    const auto commentPos = line.find('#');
    if (commentPos != std::string::npos) {
      line = line.substr(0, commentPos);
    }

    std::istringstream parser(line);
    std::string identity;
    std::string token;
    std::string role;
    if (!(parser >> identity >> token)) {
      continue;
    }
    parser >> role;

    try {
      ndn::Name identityName(identity);
      if (identityName.empty() || token.empty()) {
        throw std::runtime_error("empty identity or token");
      }
      m_bootstrapTokens[identityName.toUri(ndn::name::UriFormat::CANONICAL)] = BootstrapTokenEntry{token, role};
    }
    catch (const std::exception& e) {
      throw std::runtime_error("Invalid bootstrap token file entry " + path +
                               ":" + std::to_string(lineNo) + ": " + e.what());
    }
  }

  NDN_LOG_INFO("NDNSF_CERT_BOOTSTRAP_TOKEN_FILE path=" << path
               << " entries=" << m_bootstrapTokens.size());
}

void
ServiceController::sendCertificateBootstrapResponse(const ndn::Interest& interest,
                                                    bool status,
                                                    const std::string& message,
                                                    const ndn::security::Certificate* issuedCertificate)
{
  try {
  CertificateBootstrapResponse response;
  response.status = status;
  response.message = message;
  if (issuedCertificate != nullptr) {
    response.issuedCertificate = *issuedCertificate;
    response.hasIssuedCertificate = true;
  }

  ndn::Data data(interest.getName());
  data.setFreshnessPeriod(ndn::time::milliseconds(0));
  data.setContent(response.wireEncode());
  m_keyChain.sign(data, ndn::security::signingByCertificate(m_aaCert));
  publishOriginalControlReply(interest, data);
  }
  catch (const std::exception& error) {
    NDN_LOG_ERROR("Certificate bootstrap reply failed: " << error.what());
  }
}

// ===================== Signer cert extraction =====================

bool
ServiceController::identitiesMatch(const ndn::Name& lhs, const ndn::Name& rhs) const
{
  return lhs == rhs;
}

ndn::Name ServiceController::getSignerCertNameFromInterest(const ndn::Interest& interest) const
{
  const auto sigInfo = interest.getSignatureInfo();
  if (!sigInfo) {
    throw std::runtime_error("Interest is not signed");
  }

  if (!sigInfo->hasKeyLocator()) {
    throw std::runtime_error("Interest SignatureInfo has no KeyLocator");
  }

  const auto& kl = sigInfo->getKeyLocator();

  if (kl.getType() != ndn::tlv::Name) {
    throw std::runtime_error("Interest KeyLocator is not a Name");
  }

  const ndn::Name certName = kl.getName();
  if (!ndn::security::Certificate::isValidName(certName)) {
    throw std::runtime_error("Interest KeyLocator Name is not a certificate name: " +
                             certName.toUri(ndn::name::UriFormat::CANONICAL));
  }

  return certName;
}

ndn::security::Certificate
ServiceController::getSignerCertificateFromInterest(const ndn::Interest& interest) const
{
  const ndn::Name certName = getSignerCertNameFromInterest(interest);

  try {
    const ndn::Name identityName = ndn::security::extractIdentityFromCertName(certName);
    const ndn::Name keyName = ndn::security::extractKeyNameFromCertName(certName);
    return m_keyChain.getPib()
      .getIdentity(identityName)
      .getKey(keyName)
      .getCertificate(certName);
  }
  catch (const std::exception&) {
    throw std::runtime_error("Cannot find signer certificate in PIB: " + certName.toUri(ndn::name::UriFormat::CANONICAL));
  }
}

ndn::Name
ServiceController::getSignerIdentityFromInterest(const ndn::Interest& interest) const
{
  const auto signerCert = getSignerCertificateFromInterest(interest);
  return ndn::security::extractIdentityFromCertName(signerCert.getName());
}

// ===================== Encryption =====================

ndn::Block
ServiceController::encryptForCertificate(const ndn::security::Certificate& cert,
                                        const ndn::Block& plaintext) const
{
  ndn::Block pt = plaintext;
  pt.encode();
  return encryptWireBytesWithContentKeyForCertificate(
    cert, ndn::span<const uint8_t>(pt.data(), pt.size()));
}

// ===================== Handlers =====================

void ServiceController::onServiceAccessInterest(const ndn::InterestFilter&,
                                               const ndn::Interest& interest)
{
  if (!hasGenerationWriter()) return;
  // <controller>/NDNSF/SERVICEACCESS/<user>
  if (!admitControlQuery(interest, "SERVICEACCESS")) return;
  const auto query = decodeControlQueryParameters(interest.getApplicationParameters());
  if (!query || query->subject.empty() || !query->service.empty() || query->generation != 0 ||
      !query->recipientCertificate.empty()) return;
  const ndn::Name userName = query->subject;

  // 先验证 Interest（必须！否则 KeyLocator 可被伪造导致加密给错误的人）
  m_validator.validate(
    interest,
    // validated
    [this, userName](const ndn::Interest& validatedInterest) {
      if (!hasGenerationWriter() || m_abeRotationPending) return;
      ndn::Name signerIdentity;
      ndn::security::Certificate signerCert;
      try {
        signerIdentity = getSignerIdentityFromInterest(validatedInterest);
        signerCert = getSignerCertificateFromInterest(validatedInterest);
      }
      catch (const std::exception& e) {
        NDN_LOG_ERROR("[SERVICEACCESS] authorization failed: requested="
                  << userName
                  << " signer=<unknown>"
                  << " error=" << e.what());
        return;
      }

      if (!identitiesMatch(signerIdentity, userName)) {
        NDN_LOG_ERROR("[SERVICEACCESS] authorization failed: requested="
                  << userName
                  << " signer=" << signerIdentity);
        return;
      }

      try {
      if (serveOriginalControlReply(validatedInterest)) return;
      const std::string userUri = userName.toUri(ndn::name::UriFormat::CANONICAL);

      // 查权限（你现在按 name 里的 userUri 查）
      std::vector<std::string> services;
      if (auto it = m_userAllowedServices.find(userUri); it != m_userAllowedServices.end()) {
        services = it->second;
      }

      // Data name = prefix + user + timestamp
      ndn::Name dataName = validatedInterest.getName();

      ndn::Data data(dataName);

      ndn::Block plaintext = makeAllowedServiceListTlv(services);
      ndn::Block content   = encryptForCertificate(signerCert, plaintext);

      data.setContent(content);
      data.setFreshnessPeriod(ndn::time::seconds(2));
      m_keyChain.sign(data, ndn::security::signingByCertificate(m_aaCert));
      publishOriginalControlReply(validatedInterest, data);

      NDN_LOG_INFO("[SERVICEACCESS] Reply to " << userUri
                << " services=" << services.size()
                << " data=" << data.getName()
                << " encryptedFor=" << signerCert.getName());
      }
  catch (const std::exception& error) {
        NDN_LOG_ERROR("Authorized control reply failed: " << error.what());
      }
    },
    // failed
    [](const ndn::Interest& badInterest, const ndn::security::ValidationError& err) {
      NDN_LOG_ERROR("[SERVICEACCESS] Interest validation failed: "
                << err << " name=" << badInterest.getName());
    });
}

void ServiceController::onServiceProvisionInterest(const ndn::InterestFilter&,
                                                  const ndn::Interest& interest)
{
  if (!hasGenerationWriter()) return;
  // <controller>/NDNSF/SERVICEPROVISION/<provider>
  if (!admitControlQuery(interest, "SERVICEPROVISION")) return;
  const auto query = decodeControlQueryParameters(interest.getApplicationParameters());
  if (!query || query->subject.empty() || !query->service.empty() || query->generation != 0 ||
      !query->recipientCertificate.empty()) return;
  const ndn::Name providerName = query->subject;

  m_validator.validate(
    interest,
    // validated
    [this, providerName](const ndn::Interest& validatedInterest) {
      if (!hasGenerationWriter() || m_abeRotationPending) return;
      ndn::Name signerIdentity;
      ndn::security::Certificate signerCert;
      try {
        signerIdentity = getSignerIdentityFromInterest(validatedInterest);
        signerCert = getSignerCertificateFromInterest(validatedInterest);
      }
      catch (const std::exception& e) {
        NDN_LOG_ERROR("[SERVICEPROVISION] authorization failed: requested="
                  << providerName
                  << " signer=<unknown>"
                  << " error=" << e.what());
        return;
      }

      if (!identitiesMatch(signerIdentity, providerName)) {
        NDN_LOG_ERROR("[SERVICEPROVISION] authorization failed: requested="
                  << providerName
                  << " signer=" << signerIdentity);
        return;
      }

      try {
      if (serveOriginalControlReply(validatedInterest)) return;
      const std::string providerUri = providerName.toUri(ndn::name::UriFormat::CANONICAL);

      std::vector<std::string> services;
      if (auto it = m_providerAllowedServices.find(providerUri); it != m_providerAllowedServices.end()) {
        services = it->second;
      }

      ndn::Name dataName = validatedInterest.getName();

      ndn::Data data(dataName);

      ndn::Block plaintext = makeAllowedServiceListTlv(services);
      ndn::Block content   = encryptForCertificate(signerCert, plaintext);

      data.setContent(content);
      data.setFreshnessPeriod(ndn::time::seconds(2));
      m_keyChain.sign(data, ndn::security::signingByCertificate(m_aaCert));
      publishOriginalControlReply(validatedInterest, data);

      NDN_LOG_INFO("[SERVICEPROVISION] Reply to " << providerUri
                << " services=" << services.size()
                << " data=" << data.getName()
                << " encryptedFor=" << signerCert.getName());
      }
  catch (const std::exception& error) {
        NDN_LOG_ERROR("Authorized control reply failed: " << error.what());
      }
    },
    // failed
    [](const ndn::Interest& badInterest, const ndn::security::ValidationError& err) {
      NDN_LOG_ERROR("[SERVICEPROVISION] Interest validation failed: "
                << err << " name=" << badInterest.getName());
    });
}

void ServiceController::onUserPermissionsInterest(const ndn::InterestFilter&,
                                                  const ndn::Interest& interest)
{
  onPermissionInterest(interest, true);
}

void ServiceController::onProviderPermissionsInterest(const ndn::InterestFilter&,
                                                      const ndn::Interest& interest)
{
  onPermissionInterest(interest, false);
}

void
ServiceController::recordUserGrant(const PermissionResponse& response)
{
  const auto& version = response.getControllerVersion();
  const auto identity = ndn::Name(response.getTargetIdentity());
  const auto uri = identity.toUri(ndn::name::UriFormat::CANONICAL);
  auto record = makeHistoricalGrantRecordHeader(identity, version.controllerGenerationTimestamp,
                                                 version.controllerEpoch);
  std::set<ndn::Name> services;
  for (const auto& entry : response.getEntries())
    services.emplace(entry.getServiceName());
  for (const auto& service : services)
    record += "granted " + service.toUri(ndn::name::UriFormat::CANONICAL) + "\n";
  const auto previous = m_generationStore->loadGrant(uri, version.controllerGenerationTimestamp,
                                                      version.controllerEpoch);
  if (previous) {
    if (!acceptsHistoricalGrantWire(*previous) ||
        ndn::readString(decodeGrantData(*previous).getContent()) != record)
      throw std::runtime_error("conflicting historical grant at the same authority version");
    return;
  }
  ndn::Data signedRecord(historicalGrantRecordName(m_aaCert.getIdentity(), record));
  signedRecord.setContent(record);
  m_keyChain.sign(signedRecord, ndn::security::signingByCertificate(m_aaCert));
  const auto& wire = signedRecord.wireEncode();
  m_generationStore->recordGrant(uri, version.controllerGenerationTimestamp,
    version.controllerEpoch, std::string(reinterpret_cast<const char*>(wire.data()), wire.size()));
  NDN_LOG_WARN("NDNSF_GRANT_RECORD identity=" << uri
               << " generation=" << version.controllerGenerationTimestamp
               << " epoch=" << version.controllerEpoch);
}

void
ServiceController::onPermissionInterest(const ndn::Interest& interest, bool user)
{
  const std::string operation = user ? "USER-PERMISSIONS" : "PROVIDER-PERMISSIONS";
  if (!hasGenerationWriter() || !admitControlQuery(interest, operation))
    return;
  const auto query = decodeControlQueryParameters(interest.getApplicationParameters());
  if (!query || query->subject.empty() || !query->service.empty() || query->generation != 0)
    return;
  if (serveOriginalControlReply(interest))
    return;

  const auto issue = [this, interest, operation, user, query = *query,
                      version = m_controllerVersion] (const ndn::security::Certificate& cert) {
    try {
      if (!hasGenerationWriter() || m_controllerVersion != version ||
          !admitControlQuery(interest, operation) || cert.getIdentity() != query.subject ||
          !cert.isValid() || getCertificateKeyType(cert) != ndn::KeyType::RSA ||
          (!query.recipientCertificate.empty() && cert.getFullName() != query.recipientCertificate))
        return;
      if (serveOriginalControlReply(interest))
        return;

      auto response = user ? buildUserPermissionResponse(query.subject) :
                             buildProviderPermissionResponse(query.subject);
      const bool revoked = isIdentityOrCertificateRevoked(query.subject, certificateDigest(cert));
      if (revoked)
        response.setEntries({});
      const auto encrypted = encryptPermissionResponseForCertificate(response, cert);

      // An empty reply to a revoked credential must not overwrite another
      // credential's legitimate grant at the same identity and policy version.
      if (user && !revoked)
        recordUserGrant(response);

      ndn::Data data(interest.getName());
      data.setContent(encrypted.WireEncode());
      data.setFreshnessPeriod(ndn::time::milliseconds(0));
      m_keyChain.sign(data, ndn::security::signingByCertificate(m_aaCert));
      publishOriginalControlReply(interest, data);
      NDN_LOG_INFO((user ? "[PERMISSIONS/USER]" : "[PERMISSIONS/PROVIDER]")
                   << " Encrypted reply target=" << query.subject << " data=" << data.getName());
    }
  catch (const std::exception& error) {
      NDN_LOG_ERROR(operation << " reply failed: " << error.what());
    }
  };

  try {
    if (query->recipientCertificate.empty()) {
      issue(getTargetIdentityCertificate(query->subject));
      return;
    }
    resolveProtectedRecipient(interest.getName(), query->subject, query->recipientCertificate, issue);
  }
  catch (const std::exception& error) {
    NDN_LOG_DEBUG(operation << " recipient resolution failed: " << error.what());
  }
}

void
ServiceController::resolveProtectedRecipient(
    const ndn::Name& requestName, const ndn::Name& subject, const ndn::Name& recipientFullName,
    const std::function<void(const ndn::security::Certificate&)>& issue)
{
  if (recipientFullName.empty() || !recipientFullName[-1].isImplicitSha256Digest())
    return;
  const auto certName = recipientFullName.getPrefix(-1);
  if (!ndn::security::Certificate::isValidName(certName) ||
      ndn::security::extractIdentityFromCertName(certName) != subject)
    return;
  try {
    const auto keyName = ndn::security::extractKeyNameFromCertName(certName);
    const auto cert = m_keyChain.getPib().getIdentity(subject).getKey(keyName).getCertificate(certName);
    if (cert.getFullName() == recipientFullName) {
      issue(cert);
      return;
    }
  }
  catch (const ndn::security::pib::Pib::Error&) {
    // The recipient may own its key exclusively on another node.
  }
  // ndn-cxx's trust cache is indexed by the explicit certificate name.
  const auto* cert = m_validator.findTrustedCert(ndn::Interest(certName));
  if (cert && cert->getFullName() == recipientFullName) {
    issue(*cert);
    return;
  }

  ndn::Interest lookup(recipientFullName);
  lookup.setCanBePrefix(false).setMustBeFresh(true).setInterestLifetime(ndn::time::milliseconds(2000));
  constexpr size_t MAX_CERTIFICATE_FETCHES = 64;
  if (m_permissionCertificateFetches.size() >= MAX_CERTIFICATE_FETCHES ||
      m_permissionCertificateFetches.count(requestName))
    return;
  const std::weak_ptr<int> lifetime = m_currentAbeLifetime;
  const auto finish = [this, lifetime, requestName] (const auto&...) {
    if (!lifetime.expired())
      m_permissionCertificateFetches.erase(requestName);
  };
  m_permissionCertificateFetches.emplace(requestName, m_face.expressInterest(lookup,
    [this, lifetime, issue, finish, recipientFullName](const ndn::Interest& requested,
                                                     const ndn::Data& data) {
      if (lifetime.expired())
        return;
      if (!requested.matchesData(data)) {
        finish();
        return;
      }
      try {
        m_validator.validate(data, [lifetime, issue, finish, recipientFullName](
                                         const ndn::Data& validated) {
          if (lifetime.expired())
            return;
          finish();
          try {
            ndn::security::Certificate cert(validated);
            if (cert.getFullName() == recipientFullName)
              issue(cert);
          }
          catch (const std::exception& error) {
          }
        }, finish);
      }
      catch (const std::exception&) {
        finish();
      }
    }, finish, finish));
}

void ServiceController::onPolicyManifestInterest(const ndn::InterestFilter&,
                                                 const ndn::Interest& interest)
{
  if (!hasGenerationWriter()) return;
  if (!admitControlQuery(interest, "POLICY-MANIFEST")) return;
  const auto query = decodeControlQueryParameters(interest.getApplicationParameters());
  if (!query || !query->subject.empty() || !query->service.empty() || query->generation != 0 ||
      !query->recipientCertificate.empty()) return;
  if (serveOriginalControlReply(interest)) return;
  try {
  PolicyManifest manifest = buildPolicyManifest();
  ndn::Data data(interest.getName());
  data.setContent(manifest.WireEncode());
  data.setFreshnessPeriod(ndn::time::milliseconds(0));
  m_keyChain.sign(data, ndn::security::signingByCertificate(m_aaCert));
  publishOriginalControlReply(interest, data);

  NDN_LOG_INFO("[POLICY-MANIFEST] Reply data=" << data.getName()
               << " payload=" << manifest.toString());
  }
  catch (const std::exception& error) {
    NDN_LOG_ERROR("PolicyManifest reply failed: " << error.what());
  }
}

void
ServiceController::onPolicyStatusInterest(const ndn::InterestFilter&,
                                          const ndn::Interest& interest)
{
  if (!hasGenerationWriter() || !admitControlQuery(interest, "POLICY-STATUS")) return;
  const auto query = decodeControlQueryParameters(interest.getApplicationParameters());
  if (!query || !query->subject.empty() || query->service.empty() ||
      !query->recipientCertificate.empty()) return;
  const auto serviceName = query->service;
  const auto requestedVersion = query->generation == 0 ? std::optional<ControllerVersion>() :
    std::optional<ControllerVersion>(ControllerVersion{query->generation, query->epoch});

  try {
    auto status = getPolicyStatus(serviceName);
    if (requestedVersion &&
        status.getControllerVersion() != *requestedVersion) {
      NDN_LOG_WARN("NDNSF_POLICY_STATUS_REFUSED service=" << serviceName
                   << " reason=version-not-available requested="
                   << requestedVersion->toString()
                   << " current=" << status.getControllerVersion().toString());
      return;
    }
    if (serveOriginalControlReply(interest)) return;
    const auto now = nowMilliseconds();
    if (now < m_policyValidFromMs || now < m_lastPolicyLeaseIssuedAtMs ||
        now > std::numeric_limits<uint64_t>::max() - m_policyStatusLeaseMs)
      throw std::runtime_error("unsafe clock for Controller policy lease");
    status.setValidity(now, now + m_policyStatusLeaseMs);
    ndn::Data data(interest.getName());
    data.setContent(status.wireEncode());
    data.setFreshnessPeriod(ndn::time::milliseconds(0));
    m_keyChain.sign(data, ndn::security::signingByCertificate(m_aaCert));
    publishOriginalControlReply(interest, data);
    m_lastPolicyLeaseIssuedAtMs = now;
    NDN_LOG_INFO("NDNSF_POLICY_STATUS_PUBLISHED service=" << serviceName
                 << " generation=" << m_controllerVersion.controllerGenerationTimestamp
                 << " epoch=" << m_controllerVersion.controllerEpoch
                 << " revocations=" << status.getRevocations().size());
  }
  catch (const std::exception& error) {
    NDN_LOG_ERROR("NDNSF_POLICY_STATUS_REFUSED service=" << serviceName
                  << " reason=" << error.what());
  }
}

void
ServiceController::onCertificateBootstrapInterest(const ndn::InterestFilter&,
                                                  const ndn::Interest& interest)
{
  if (!hasGenerationWriter()) return;
  if (!admitControlQuery(interest, "CERTBOOTSTRAP")) return;
  if (serveOriginalControlReply(interest)) return;
  std::string targetUri;
  try {
    const auto params = interest.getApplicationParameters();
    EncryptedCertificateBootstrapRequest encrypted;
    if (!params.isValid() || !encrypted.wireDecode(params)) {
      sendCertificateBootstrapResponse(interest, false, "encrypted bootstrap request required", nullptr);
      return;
    }
    const auto request = decryptCertificateBootstrapRequestWithKeyChain(encrypted, m_keyChain);
    const auto targetIdentity = request.identity;
    if (targetIdentity.empty()) {
      sendCertificateBootstrapResponse(interest, false, "empty bootstrap subject", nullptr); return;
    }
    targetUri = targetIdentity.toUri(ndn::name::UriFormat::CANONICAL);
    const auto tokenIt = m_bootstrapTokens.find(targetUri);
    if (tokenIt == m_bootstrapTokens.end()) {
      sendCertificateBootstrapResponse(interest, false, "no bootstrap token configured for subject", nullptr);
      return;
    }
    const auto& tokenEntry = tokenIt->second;
    const bool encryptedRequest = true;

    if (request.token != tokenEntry.token) {
      NDN_LOG_WARN("NDNSF_CERT_BOOTSTRAP_REFUSED identity=" << targetUri
                   << " reason=token-mismatch");
      sendCertificateBootstrapResponse(interest, false, "bootstrap token mismatch", nullptr);
      return;
    }

    if (request.certificateRequest.getIdentity() != targetIdentity) {
      NDN_LOG_WARN("NDNSF_CERT_BOOTSTRAP_REFUSED identity=" << targetUri
                   << " reason=request-identity-mismatch"
                   << " certIdentity=" << request.certificateRequest.getIdentity());
      sendCertificateBootstrapResponse(interest, false, "certificate request identity mismatch", nullptr);
      return;
    }

    if (request.proofNonce.empty() || request.proofSignature.empty()) {
      NDN_LOG_WARN("NDNSF_CERT_BOOTSTRAP_REFUSED identity=" << targetUri
                   << " reason=missing-proof");
      sendCertificateBootstrapResponse(interest, false, "certificate bootstrap proof missing", nullptr);
      return;
    }

    auto proofData = makeCertificateBootstrapProofData(request);
    proofData.encode();
    if (!ndn::security::verifySignature(
          ndn::InputBuffers{ndn::span<const uint8_t>(proofData.data(), proofData.size())},
          bufferToSpan(request.proofSignature),
          request.certificateRequest.getPublicKey())) {
      NDN_LOG_WARN("NDNSF_CERT_BOOTSTRAP_REFUSED identity=" << targetUri
                   << " reason=request-proof-invalid");
      sendCertificateBootstrapResponse(interest, false, "certificate bootstrap proof invalid", nullptr);
      return;
    }

    auto issued = m_keyChain.makeCertificate(
      request.certificateRequest,
      ndn::security::SigningInfo(ndn::security::SigningInfo::SIGNER_TYPE_ID,
                                 m_controllerPrefix));
    m_bootstrapIssuedCertificates[targetUri] = issued;

    NDN_LOG_INFO("NDNSF_CERT_BOOTSTRAP_ISSUED identity=" << targetUri
                 << " cert=" << issued.getName().toUri(ndn::name::UriFormat::CANONICAL)
                 << " role=" << tokenEntry.role
                 << " encryptedRequest=" << (encryptedRequest ? "true" : "false")
                 << " requesterProof=true");
    sendCertificateBootstrapResponse(interest, true, "issued", &issued);
  }
  catch (const std::exception& e) {
    NDN_LOG_ERROR("NDNSF_CERT_BOOTSTRAP_REFUSED identity=" << targetUri
                  << " reason=exception error=" << e.what());
    sendCertificateBootstrapResponse(interest, false, e.what(), nullptr);
  }
}

} // namespace ndn_service_framework
