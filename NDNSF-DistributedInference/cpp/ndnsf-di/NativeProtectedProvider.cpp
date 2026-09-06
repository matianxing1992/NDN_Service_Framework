#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProtectedProvider.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.hpp"
#include <ndn-cxx/face.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <openssl/evp.h>
#include <openssl/ec.h>
#include <openssl/pem.h>
#include <openssl/sha.h>
#include <openssl/crypto.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <memory>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace ndnsf::di {
namespace {
std::runtime_error reject(const std::string& message)
{
  return std::runtime_error("DI_PROTECTED_GRANT_REJECTED: " + message);
}
std::uint64_t nowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
}
std::string env(const char* name)
{
  const char* value = std::getenv(name);
  if (!value || !*value) throw reject(std::string("missing operator setting ") + name);
  return value;
}
std::string digest(const std::string& bytes)
{
  unsigned char hash[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), hash);
  std::string out = "sha256:";
  for (const auto c : hash) {
    out += "0123456789abcdef"[c >> 4]; out += "0123456789abcdef"[c & 15];
  }
  return out;
}
std::string readBounded(const std::filesystem::path& path, bool privateKey = false)
{
  const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) throw reject("operator file cannot be opened");
  struct stat st{};
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
      st.st_size > 1024 * 1024 || (privateKey && (st.st_mode & 0777) != 0600)) {
    ::close(fd); throw reject("operator file type, size or permissions are invalid");
  }
  std::string value(st.st_size, '\0');
  std::size_t offset = 0;
  while (offset < value.size()) {
    const auto count = ::read(fd, value.data() + offset, value.size() - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) {
      ::close(fd); OPENSSL_cleanse(value.data(), value.size());
      throw reject("operator file is truncated");
    }
    offset += count;
  }
  ::close(fd);
  return value;
}
boost::property_tree::ptree json(const std::string& text)
{
  boost::property_tree::ptree value;
  std::istringstream input(text);
  boost::property_tree::read_json(input, value);
  return value;
}
bool contains(const boost::property_tree::ptree& policy,
              const std::string& field, const std::string& value)
{
  const auto array = policy.get_child_optional(field);
  return array && std::any_of(array->begin(), array->end(), [&] (const auto& entry) {
    return entry.first.empty() && entry.second.template get_value<std::string>() == value;
  });
}
std::string rawPublicKey(const std::string& pem)
{
  BIO* bio = BIO_new_mem_buf(pem.data(), pem.size());
  EVP_PKEY* key = bio ? PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr) : nullptr;
  if (bio) BIO_free(bio);
  std::string raw(32, '\0');
  std::size_t size = raw.size();
  const bool ok = key && EVP_PKEY_id(key) == EVP_PKEY_ED25519 &&
    EVP_PKEY_get_raw_public_key(key, reinterpret_cast<unsigned char*>(raw.data()), &size) == 1 &&
    size == raw.size();
  if (key) EVP_PKEY_free(key);
  if (!ok) throw reject("registry authority key is not Ed25519");
  return raw;
}
NativeProtectedGrantConfig credentials(const std::string& provider,
                                      const std::string& modelFamily,
                                      const std::string& epoch)
{
  const auto publicHint = std::filesystem::path(env("SPEC181_GRANT_AUTHORITY_PUBLIC_KEY"));
  const auto registry = publicHint.parent_path() / "trust-root-registry-v1.json";
  const auto document = json(readBounded(registry));
  if (document.get<int>("schemaVersion", 0) != 1 ||
      document.get<std::string>("status", "") != "CONFIGURED") {
    throw reject("authority registry is not configured");
  }
  const auto& policy = document.get_child("artifactPolicyAuthority");
  if (policy.get<std::string>("publicKeyAlgorithm", "") != "ed25519" ||
      policy.get<std::string>("signatureAlgorithm", "") != "ed25519" ||
      policy.get<std::string>("grantSchema", "") != "ndnsf-di-key-grant-v1" ||
      !contains(policy, "acceptedModelFamilies", modelFamily) ||
      !contains(policy, "protectionEpochs", epoch)) {
    throw reject("registry algorithm or model/epoch policy rejected");
  }
  const std::filesystem::path relative(policy.get<std::string>("publicKeyPath", ""));
  if (relative.empty() || relative.is_absolute() ||
      std::find(relative.begin(), relative.end(), "..") != relative.end()) {
    throw reject("registry public key path is unsafe");
  }
  const auto pem = readBounded(registry.parent_path().parent_path() / relative);
  if (digest(pem) != policy.get<std::string>("publicKeySha256", "")) {
    throw reject("registry authority public key digest mismatch");
  }
  NativeProtectedGrantConfig out;
  out.authorityIdentity = policy.get<std::string>("authorityId", "");
  if (out.authorityIdentity.empty() || policy.get<std::string>("keyId", "").empty()) {
    throw reject("registry issuer identity is missing");
  }
  out.authorityPublicKeyRaw = rawPublicKey(pem);
  const auto mapping = json(readBounded(env("SPEC181_PROVIDER_RECIPIENT_KEY_MAP")));
  std::string recipientPath;
  for (const auto& entry : mapping) {
    if (entry.first == provider) recipientPath = entry.second.get_value<std::string>();
  }
  if (recipientPath.empty()) throw reject("provider recipient identity has no configured key");
  auto secret = readBounded(recipientPath, true);
  struct CleansePem {
    std::string& value;
    ~CleansePem() { OPENSSL_cleanse(value.data(), value.size()); }
  } cleanse{secret};
  std::unique_ptr<BIO, decltype(&BIO_free)> bio(
    BIO_new_mem_buf(secret.data(), secret.size()), BIO_free);
  std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(
    bio ? PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr) : nullptr,
    EVP_PKEY_free);
  if (!key) throw reject("provider recipient private key cannot be parsed");
  if (EVP_PKEY_id(key.get()) == EVP_PKEY_ED25519) {
    out.recipientKey.kind = NativeRecipientKey::Kind::Ed25519Seed;
    out.recipientKey.material.resize(32);
    std::size_t size = 32;
    if (EVP_PKEY_get_raw_private_key(key.get(),
          reinterpret_cast<unsigned char*>(out.recipientKey.material.data()), &size) != 1 ||
        size != 32) {
      OPENSSL_cleanse(out.recipientKey.material.data(), out.recipientKey.material.size());
      throw reject("provider Ed25519 private seed cannot be extracted");
    }
  }
  else if (EVP_PKEY_id(key.get()) == EVP_PKEY_EC) {
    std::unique_ptr<EC_KEY, decltype(&EC_KEY_free)> ec(
      EVP_PKEY_get1_EC_KEY(key.get()), EC_KEY_free);
    const auto* group = ec ? EC_KEY_get0_group(ec.get()) : nullptr;
    if (!group || EC_GROUP_get_curve_name(group) != NID_X9_62_prime256v1 ||
        EC_KEY_check_key(ec.get()) != 1) {
      throw reject("provider EC recipient private key must use P-256");
    }
    out.recipientKey.kind = NativeRecipientKey::Kind::EcP256Pem;
    out.recipientKey.material = std::move(secret);
  }
  else {
    throw reject("provider recipient private key must be Ed25519 or EC P-256");
  }
  return out;
}

std::string fetchGrant(const std::string& name, int timeoutMs,
                       const std::function<bool()>& cancelled)
{
  ndn::Face face;
  const ndn::Name exact(name);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  std::string payload;
  bool received = false;
  while (!received && std::chrono::steady_clock::now() < deadline) {
    if (cancelled && cancelled()) throw reject("grant fetch cancelled");
    ndn::Interest interest(exact);
    interest.setCanBePrefix(false);
    interest.setMustBeFresh(false);
    interest.setInterestLifetime(ndn::time::milliseconds(500));
    if (const char* hints = std::getenv("SPEC181_GRANT_FORWARDING_HINT")) {
      std::istringstream input(hints);
      std::string hint;
      std::vector<ndn::Name> delegations;
      while (std::getline(input, hint, ',')) {
        if (!hint.empty()) delegations.emplace_back(hint);
      }
      interest.setForwardingHint(delegations);
    }
    bool done = false;
    face.expressInterest(interest,
      [&] (const ndn::Interest&, const ndn::Data& data) {
        if (data.getName() != exact || data.getContent().value_size() > 65536) {
          done = true; return;
        }
        payload.assign(reinterpret_cast<const char*>(data.getContent().value()),
                       data.getContent().value_size());
        received = true; done = true;
      },
      [&] (const ndn::Interest&, const ndn::lp::Nack&) { done = true; },
      [&] (const ndn::Interest&) { done = true; });
    while (!done && std::chrono::steady_clock::now() < deadline) {
      face.processEvents(ndn::time::milliseconds(20));
      if (cancelled && cancelled()) throw reject("grant fetch cancelled");
    }
  }
  if (!received) throw reject("exact grant Data fetch timed out");
  // This transport exposes bytes only to ProtectedRuntime's pinned authority
  // verifier. No permissive Data validator grants authorization here.
  return payload;
}
} // namespace

NativeProtectedGrantConfig loadNativeProtectedGrantConfig(
  const std::string& provider, const std::string& modelFamily,
  const std::string& protectionEpoch)
{
  return credentials(provider, modelFamily, protectionEpoch);
}

std::string nativeProtectedFencingToken(
  const NativeSelectionProjectionV3& projection, const std::string& providerBootId,
  const std::map<std::string, std::string>& fields)
{
  const auto leaseFence = nativeProviderFieldValue(fields,
    {"executionFencingToken", "fencingToken", "admissionFencingToken"});
  if (!leaseFence.empty()) return leaseFence;
  // Non-leased V3 requests have no lease token. Their local attempt fence is
  // bound to the authenticated Selection and this Provider incarnation.
  return digest("NDNSF-DI/protected-attempt-fence/v1\n" + providerBootId + "\n" +
    projection.provider + "\n" + projection.requestId + "\n" +
    std::to_string(projection.attempt) + "\n" + projection.planDigest);
}

void installNativeProtectedGrantFactory(NativeProviderHandlerConfig& config)
{
  const auto boot = config.providerBootId;
  const auto modelFamily = nativeProtectedModelFamily(config.plan.modelName);
  config.protectedRuntimeFactory = [boot, modelFamily] (
      ndn_service_framework::ServiceProvider::CollaborationContext& ctx,
      const NativeSelectionProjectionV3& projection,
      const std::shared_ptr<ProviderGroupCoordinator>& group) {
    auto keys = loadNativeProtectedGrantConfig(ctx.localProvider().toUri(), modelFamily,
                                              projection.selectedRole.protectionEpoch);
    keys.modelManifestDigest = projection.selectedRole.modelManifestDigest;
    if (keys.modelManifestDigest.empty()) {
      const auto offset = projection.grantName.find("/MODEL/");
      if (offset == std::string::npos) throw reject("sealed grant lacks model commitment");
      keys.modelManifestDigest = "sha256:" + projection.grantName.substr(offset + 7, 64);
    }
    const auto fields = parseNativeProviderAssignmentFields(
      ctx.assignment().assignmentPayload, projection.executionRole.roleId);
    ProtectedRuntimeBindingV1 binding;
    binding.provider = projection.provider;
    binding.role = projection.executionRole.roleId;
    binding.requestId = projection.requestId;
    binding.attempt = projection.attempt;
    binding.planCoreDigest = projection.planCoreDigest;
    binding.planDigest = projection.planDigest;
    binding.securityPolicySnapshotDigest = projection.securityPolicySnapshotDigest;
    binding.protectionEpoch = projection.selectedRole.protectionEpoch;
    binding.grantName = projection.grantName;
    binding.grantDigest = projection.grantDigest;
    binding.providerBootId = boot;
    binding.fencingToken = nativeProtectedFencingToken(projection, boot, fields);
    binding.expiresAtMs = projection.deadlineMs;
    for (const auto& endpoint : projection.dataflow.mayPublish) {
      binding.mayPublishEndpointDigests.insert(endpoint.endpointDigest);
      binding.mayPublishConsumerByEndpoint[endpoint.endpointDigest] = endpoint.consumerRole;
    }
    for (const auto& endpoint : projection.dataflow.mustFetch) {
      if (endpoint.sourceKind == "APPLICATION_INPUT" || endpoint.operation == "APPLICATION_INPUT") continue;
      binding.mustFetchEndpointDigests.insert(endpoint.endpointDigest);
      binding.mustFetchProducerByEndpoint[endpoint.endpointDigest] = endpoint.producerRole;
    }
    if (group && group->hasCapability()) {
      const auto& capability = group->capability();
      binding.capabilityDigest = capability.capabilityDigest;
      binding.groupId = capability.groupId;
      binding.groupEpoch = capability.epoch;
      binding.epochKeyId = capability.epochKeyId;
    }
    const auto now = nowMs();
    if (now >= projection.deadlineMs) throw reject("request expired before grant acquisition");
    keys.shouldCancel = [&ctx] { return ctx.isStreamed() && ctx.streamCancelled(); };
    keys.fetchGrant = [limit = static_cast<int>(std::min<std::uint64_t>(
                        30000, projection.deadlineMs - now)),
                       cancelled = keys.shouldCancel] (const std::string& name) {
      return fetchGrant(name, limit, cancelled);
    };
    auto runtime = std::make_shared<ProtectedRuntime>(binding, std::move(keys));
    runtime->verifyGrant(binding, now);
    return runtime;
  };
}
} // namespace ndnsf::di
