#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeArtifactPolicyAuthority.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "ndn-service-framework/CertificatePublisher.hpp"
#include "ndn-service-framework/ServiceProvider.hpp"

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>
#include <ndn-cxx/util/io.hpp>

#include <openssl/evp.h>
#include <openssl/pem.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace ndnsf::di;
volatile std::sig_atomic_t interrupted = 0;

void
onSignal(int)
{
  interrupted = 1;
}

std::string
readText(const std::filesystem::path& path, std::uint64_t limit)
{
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) throw std::runtime_error("authority file is unavailable: " + path.string());
  const auto size = input.tellg();
  if (size < 0 || static_cast<std::uint64_t>(size) > limit)
    throw std::runtime_error("authority file exceeds configured limit: " + path.string());
  std::string value(static_cast<std::size_t>(size), '\0');
  input.seekg(0);
  if (!value.empty() && !input.read(value.data(), value.size()))
    throw std::runtime_error("authority file read failed: " + path.string());
  return value;
}

std::shared_ptr<EVP_PKEY>
loadKey(const std::filesystem::path& path, bool privateKey)
{
  auto bytes = readText(path, 65536);
  std::unique_ptr<BIO, decltype(&BIO_free)> bio(
    BIO_new_mem_buf(bytes.data(), static_cast<int>(bytes.size())), BIO_free);
  EVP_PKEY* raw = bio == nullptr ? nullptr :
    (privateKey ? PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr) :
                  PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr));
  if (!bytes.empty()) OPENSSL_cleanse(bytes.data(), bytes.size());
  if (raw == nullptr) throw std::runtime_error("authority key could not be loaded: " + path.string());
  return {raw, EVP_PKEY_free};
}

ndn::security::Certificate
getOrCreateIdentity(ndn::security::KeyChain& keyChain, const ndn::Name& identity)
{
  try {
    return keyChain.getPib().getIdentity(identity).getDefaultKey().getDefaultCertificate();
  }
  catch (const std::exception&) {
    return keyChain.createIdentity(identity, ndn::RsaKeyParams(2048))
      .getDefaultKey().getDefaultCertificate();
  }
}

ndn::security::Certificate
loadControllerCertificate(const ndn::Name& controller, ndn::security::KeyChain& keyChain)
{
  if (const char* path = std::getenv("NDNSF_CONTROLLER_CERT_FILE"); path != nullptr && *path != '\0') {
    auto cert = ndn::io::load<ndn::security::Certificate>(path);
    if (cert == nullptr || !cert->isValid() || cert->getIdentity() != controller)
      throw std::runtime_error("authority controller certificate is invalid");
    return *cert;
  }
  return getOrCreateIdentity(keyChain, controller);
}

std::uint64_t
nowMs()
{
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count());
}

ndn::Buffer
buffer(const std::string& value)
{
  return ndn::Buffer(reinterpret_cast<const std::uint8_t*>(value.data()), value.size());
}

std::string
bufferText(const ndn::Buffer& value)
{
  return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

void
usage(const char* program)
{
  std::cerr << "usage: " << program << " --config FILE [--run-for-ms MS]\n"
            << "config schema: ndnsf-di-native-authority-v1\n";
}

} // namespace

int
main(int argc, char** argv)
{
  if (argc == 2 && std::string(argv[1]) == "--help") {
    usage(argv[0]);
    return 0;
  }
  if (argc != 3 || std::string(argv[1]) != "--config") {
    usage(argv[0]);
    return 2;
  }

  try {
    const auto configPath = std::filesystem::absolute(argv[2]);
    const auto base = configPath.parent_path();
    const auto root = nativeParseJson(readText(configPath, 2 * 1024 * 1024));
    if (root.value("schema", std::string{}) != "ndnsf-di-native-authority-v1")
      throw std::invalid_argument("unsupported native authority configuration");
    const auto& authority = root.at("authority");
    const auto identity = authority.at("identity").get<std::string>();
    const auto service = authority.at("service").get<std::string>();
    const auto group = authority.at("group").get<std::string>();
    const auto controller = authority.at("controller_identity").get<std::string>();
    const auto trustSchema = (base / authority.at("trust_schema_file").get<std::string>()).string();

    NativeGrantIssuerConfig issuerConfig;
    issuerConfig.authorityIdentity = identity;
    issuerConfig.requesterIdentity = authority.at("requester_identity").get<std::string>();
    issuerConfig.protectionEpoch = authority.at("protection_epoch").get<std::string>();
    issuerConfig.keyId = authority.at("content_key_id").get<std::string>();
    issuerConfig.authorityPrivateKey = loadKey(
      base / authority.at("authority_private_key_file").get<std::string>(), true);
    issuerConfig.requesterPublicKey = loadKey(
      base / authority.at("requester_public_key_file").get<std::string>(), false);
    for (const auto& manifest : authority.at("allowed_model_manifests"))
      issuerConfig.allowedModelManifests.insert(manifest.get<std::string>());
    for (const auto& recipient : authority.at("recipient_public_key_files").items())
      issuerConfig.recipientPublicKeys.emplace(
        recipient.key(), loadKey(base / recipient.value().get<std::string>(), false));
    if (authority.contains("publication_sources")) {
      for (const auto& entry : authority.at("publication_sources").items()) {
        const auto& source = entry.value();
        issuerConfig.publicationSources.emplace(entry.key(), NativeGrantPublicationSource{
          source.at("model_name").get<std::string>(),
          source.at("model_content_digest").get<std::string>(),
          source.at("canonical_source_digest").get<std::string>(),
          source.value("initializer_object_digest", std::string{}),
          source.at("artifact_profile_digest").get<std::string>()});
      }
    }
    auto contentKey = std::shared_ptr<std::vector<std::uint8_t>>(
      new std::vector<std::uint8_t>, [](auto* bytes) {
        if (bytes != nullptr && !bytes->empty())
          OPENSSL_cleanse(bytes->data(), bytes->size());
        delete bytes;
      });
    const auto content = readText(base / authority.at("content_key_file").get<std::string>(), 256);
    contentKey->assign(content.begin(), content.end());
    if (contentKey->empty()) throw std::invalid_argument("authority content key is empty");
    issuerConfig.contentKey = [contentKey](const std::string&, const std::string&) {
      return *contentKey;
    };
    auto issuer = std::make_shared<const NativeArtifactGrantIssuer>(std::move(issuerConfig));

    const auto maxTtlMs = root.value("max_grant_ttl_ms", static_cast<std::uint64_t>(60000));
    if (maxTtlMs == 0 || maxTtlMs > 3600000) throw std::invalid_argument("invalid max_grant_ttl_ms");

    ndn::Face face;
    ndn::security::KeyChain keyChain;
    const auto authorityCert = getOrCreateIdentity(keyChain, ndn::Name(identity));
    const auto controllerCert = loadControllerCertificate(ndn::Name(controller), keyChain);
    keyChain.setDefaultIdentity(keyChain.getPib().getIdentity(ndn::Name(identity)));
    ndn_service_framework::ServiceProvider provider(
      face, ndn::Name(group), authorityCert, controllerCert, trustSchema);
    auto registration = provider.addScopedService(
      ndn::Name(service),
      [] (const ndn_service_framework::RequestMessage&) {
        ndn_service_framework::ServiceProvider::AckDecision decision;
        decision.status = true;
        decision.message = "NATIVE_GRANT_AUTHORITY_READY";
        return decision;
      },
      [issuer, identity, maxTtlMs](const ndn::Name& requesterIdentity,
                                   const ndn::Name&, const ndn::Name&, const ndn::Name&,
                                   const ndn_service_framework::RequestMessage& request) {
        ndn_service_framework::ResponseMessage response;
        try {
          const auto envelope = nativeGrantAuthorityRequestFromJson(bufferText(request.getPayload()));
          if (requesterIdentity.toUri() != envelope.request.requesterIdentity ||
              envelope.request.requesterIdentity != issuer->requesterIdentity())
            throw std::runtime_error("requester identity mismatch");
          const auto now = nowMs();
          if (envelope.expiresAtMs <= now || envelope.expiresAtMs - now > maxTtlMs)
            throw std::runtime_error("grant expiry exceeds authority policy");
          const auto grant = issuer->issue(envelope.request, now, envelope.expiresAtMs,
                                            envelope.publishedManifestJson);
          auto payload = buffer(nativeKeyGrantJson(grant));
          response.setStatus(true);
          response.setPayload(payload, payload.size());
        }
        catch (const std::exception& error) {
          response.setStatus(false);
          response.setErrorInfo(std::string("DI_PROTECTED_GRANT_REJECTED: ") + error.what());
        }
        return response;
      },
      ndn_service_framework::ServiceProvider::ServiceInvocationMode::TargetedOnly);
    provider.fetchPermissionsFromController(ndn::Name(controller));
    std::cout << "NATIVE_GRANT_AUTHORITY_PERMISSION_FETCH_ISSUED controller="
              << controller << std::endl;
    provider.init();
    const auto permissionBootstrapMs = root.value("permission_bootstrap_ms",
                                                  static_cast<std::uint64_t>(60000));
    if (permissionBootstrapMs == 0 || permissionBootstrapMs > 3600000)
      throw std::invalid_argument("invalid permission_bootstrap_ms");
    const auto permissionDeadline = std::chrono::steady_clock::now() +
      std::chrono::milliseconds(permissionBootstrapMs);
    while (!interrupted && !provider.hasProviderPermissionForService(ndn::Name(service)) &&
           std::chrono::steady_clock::now() < permissionDeadline)
      face.processEvents(ndn::time::milliseconds(20));
    if (!provider.hasProviderPermissionForService(ndn::Name(service)))
      throw std::runtime_error("authority ProviderPermission was not installed before deadline");
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    std::cout << "NATIVE_GRANT_AUTHORITY_PERMISSION_READY service=" << service << std::endl;
    std::cout << "NATIVE_GRANT_AUTHORITY_READY identity=" << identity
              << " service=" << service << std::endl;
    const auto runForMs = root.value("run_for_ms", static_cast<std::uint64_t>(0));
    const auto deadline = runForMs == 0 ? std::chrono::steady_clock::time_point::max() :
      std::chrono::steady_clock::now() + std::chrono::milliseconds(runForMs);
    while (!interrupted && std::chrono::steady_clock::now() < deadline)
      face.processEvents(ndn::time::milliseconds(20));
    registration.close();
    return 0;
  }
  catch (const std::exception& error) {
    std::cerr << "NATIVE_GRANT_AUTHORITY_FAILED: " << error.what() << std::endl;
    return 1;
  }
}
