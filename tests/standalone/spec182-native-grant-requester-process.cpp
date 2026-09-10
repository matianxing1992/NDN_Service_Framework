#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeArtifactPolicyAuthority.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeGrantVerifier.hpp"
#include "ndn-service-framework/ServiceUser.hpp"

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unistd.h>

namespace {

using namespace ndnsf::di;
using namespace ndn_service_framework;

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
  if (!input)
    throw std::runtime_error("requester file is unavailable: " + path.string());
  const auto size = input.tellg();
  if (size < 0 || static_cast<std::uint64_t>(size) > limit)
    throw std::runtime_error("requester file exceeds configured limit: " + path.string());
  std::string value(static_cast<std::size_t>(size), '\0');
  input.seekg(0);
  if (!value.empty() && !input.read(value.data(), value.size()))
    throw std::runtime_error("requester file read failed: " + path.string());
  return value;
}

std::shared_ptr<EVP_PKEY>
loadPrivateKey(const std::filesystem::path& path)
{
  auto bytes = readText(path, 65536);
  std::unique_ptr<BIO, decltype(&BIO_free)> bio(
    BIO_new_mem_buf(bytes.data(), static_cast<int>(bytes.size())), BIO_free);
  auto* raw = bio == nullptr ? nullptr :
    PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr);
  if (!bytes.empty())
    OPENSSL_cleanse(bytes.data(), bytes.size());
  if (raw == nullptr || EVP_PKEY_id(raw) != EVP_PKEY_ED25519) {
    if (raw != nullptr)
      EVP_PKEY_free(raw);
    throw std::runtime_error("requester key is not an Ed25519 private key");
  }
  return {raw, EVP_PKEY_free};
}

std::shared_ptr<EVP_PKEY>
loadPublicKey(const std::filesystem::path& path)
{
  auto bytes = readText(path, 65536);
  std::unique_ptr<BIO, decltype(&BIO_free)> bio(
    BIO_new_mem_buf(bytes.data(), static_cast<int>(bytes.size())), BIO_free);
  auto* raw = bio == nullptr ? nullptr :
    PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr);
  if (!bytes.empty())
    OPENSSL_cleanse(bytes.data(), bytes.size());
  if (raw == nullptr || EVP_PKEY_id(raw) != EVP_PKEY_ED25519) {
    if (raw != nullptr)
      EVP_PKEY_free(raw);
    throw std::runtime_error("authority key is not an Ed25519 public key");
  }
  return {raw, EVP_PKEY_free};
}

std::string
publicBytes(EVP_PKEY& key)
{
  std::string value(32, '\0');
  std::size_t size = value.size();
  if (EVP_PKEY_get_raw_public_key(
        &key, reinterpret_cast<unsigned char*>(value.data()), &size) != 1 || size != 32)
    throw std::runtime_error("authority public key extraction failed");
  return value;
}

std::uint64_t
nowMs()
{
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count());
}

bool
hasPermission(const ServiceUser& user, const ndn::Name& provider, const ndn::Name& service)
{
  auto providerServiceName = provider;
  providerServiceName.append(service);
  const auto providerService = providerServiceName.toUri();
  for (const auto& item : user.getAllowedServices()) {
    if (std::get<0>(item) == providerService &&
        std::get<1>(item) == service.toUri() &&
        std::get<2>(item) > 0)
      return true;
  }
  return false;
}

void
usage(const char* program)
{
  std::cerr << "usage: " << program << " --config FILE\n"
            << "config schema: ndnsf-di-native-grant-process-probe-v1\n";
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
    const auto root = nativeParseJson(readText(configPath, 1024 * 1024));
    if (root.value("schema", std::string{}) !=
        "ndnsf-di-native-grant-process-probe-v1")
      throw std::invalid_argument("unsupported native grant process probe configuration");

    const auto group = ndn::Name(root.at("group").get<std::string>());
    const auto controller = ndn::Name(root.at("controller").get<std::string>());
    const auto requester = ndn::Name(root.at("requester_identity").get<std::string>());
    const auto authority = ndn::Name(root.at("authority_identity").get<std::string>());
    const auto service = ndn::Name(root.at("authority_service").get<std::string>());
    const auto targetProvider = ndn::Name(
      root.value("target_provider", authority.toUri()));
    const auto targetService = ndn::Name(
      root.value("target_service", service.toUri()));
    const auto mode = root.value("mode", std::string("positive"));
    const bool expectRejected = root.value("expect_rejection", mode != "positive");
    const auto bootstrapMs = root.value("bootstrap_ms", static_cast<std::uint64_t>(15000));
    const auto requestTimeoutMs = root.value("request_timeout_ms", 5000);
    if (bootstrapMs == 0 || bootstrapMs > 120000 || requestTimeoutMs <= 0 ||
        requestTimeoutMs > 120000)
      throw std::invalid_argument("invalid process probe timeout");

    const auto requesterKey = loadPrivateKey(
      base / root.at("requester_private_key_file").get<std::string>());
    const auto authorityKey = loadPublicKey(
      base / root.at("authority_public_key_file").get<std::string>());
    const auto trustSchema = (base / root.at("trust_schema_file").get<std::string>()).string();
    const auto epoch = root.value("protection_epoch", std::string("epoch-1"));
    const auto manifest = root.value(
      "model_manifest_digest", "sha256:1111111111111111111111111111111111111111111111111111111111111111");
    const auto plan = root.value(
      "plan_core_digest", "sha256:2222222222222222222222222222222222222222222222222222222222222222");
    const auto view = root.value(
      "grant_view_digest", "sha256:3333333333333333333333333333333333333333333333333333333333333333");

    ndn::Face face;
    ndn::security::KeyChain keyChain;
    const auto userCert = keyChain.getPib().getIdentity(requester)
      .getDefaultKey().getDefaultCertificate();
    const auto controllerCert = keyChain.getPib().getIdentity(controller)
      .getDefaultKey().getDefaultCertificate();
    ServiceUser user(face, group, userCert, controllerCert, trustSchema);
    user.init();
    user.fetchPermissionsFromController(controller);

    std::cout << "NATIVE_GRANT_REQUESTER_STARTED pid=" << ::getpid()
              << " requester=" << requester
              << " authority=" << authority
              << " target=" << targetProvider << targetService << std::endl;

    const auto bootstrapDeadline = std::chrono::steady_clock::now() +
      std::chrono::milliseconds(bootstrapMs);
    while (!interrupted && std::chrono::steady_clock::now() < bootstrapDeadline &&
           (!hasPermission(user, authority, service) ||
            user.getCurrentPolicyEpoch(service) == 0)) {
      face.processEvents(ndn::time::milliseconds(20));
    }
    const bool permissionReady = hasPermission(user, authority, service) &&
      user.getCurrentPolicyEpoch(service) != 0;
    std::cout << "NATIVE_GRANT_REQUESTER_PERMISSION_STATE allowed="
              << (hasPermission(user, authority, service) ? 1 : 0)
              << " policyEpoch=" << user.getCurrentPolicyEpoch(service)
              << std::endl;
    if (!permissionReady) {
      if (expectRejected) {
        std::cout << "NATIVE_GRANT_PROCESS_REJECTED boundary=permission-bootstrap mode="
                  << mode << std::endl;
        return 0;
      }
      throw std::runtime_error("requester permission bootstrap timed out");
    }

    NativeSignedGrantRequest request;
    request.providerIdentity = root.value("provider_identity", authority.toUri());
    request.requestId = "/NDNSF/DI/GRANT-PROCESS/" + std::to_string(::getpid());
    request.attempt = root.value("attempt", static_cast<std::uint64_t>(1));
    request.planCoreDigest = plan;
    request.grantViewDigest = view;
    request.modelManifestDigest = manifest;
    request.protectionEpoch = epoch;
    request.requesterIdentity = requester.toUri();
    request.issuedAtMs = nowMs();

    if (mode == "wrong-epoch")
      request.protectionEpoch = "epoch-invalid";
    if (mode == "unknown-recipient")
      request.providerIdentity = "/example/hello/unknown-provider";
    request = request.sign(*requesterKey);
    if (mode == "bad-signature") {
      request.requesterSignature.back() = request.requesterSignature.back() == '0' ? '1' : '0';
    }

    const auto now = nowMs();
    const auto expiresAtMs = mode == "expired" ? now - 1 : now + 60000;
    NativeGrantAuthorityRequest envelope{request, expiresAtMs, {}};
    auto wire = nativeGrantAuthorityRequestJson(envelope);
    if (mode == "malformed")
      wire.push_back(' ');

    RequestMessage message;
    auto requestPayload = ndn::Buffer(
      reinterpret_cast<const std::uint8_t*>(wire.data()), wire.size());
    message.setPayload(requestPayload, requestPayload.size());
    bool completed = false;
    bool timedOut = false;
    ResponseMessage response;
    std::string timeoutName;
    const auto requestId = user.RequestServiceTargeted(
      targetProvider, targetService, std::move(message), requestTimeoutMs,
      [&](const ndn::Name& name) {
        timedOut = true;
        timeoutName = name.toUri();
        completed = true;
      },
      [&](const ResponseMessage& value) {
        response = value;
        completed = true;
      });
    if (requestId.empty()) {
      if (expectRejected) {
        std::cout << "NATIVE_GRANT_PROCESS_REJECTED boundary=admission mode=" << mode
                  << std::endl;
        return 0;
      }
      throw std::runtime_error("targeted authority request was not admitted");
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    const auto requestDeadline = std::chrono::steady_clock::now() +
      std::chrono::milliseconds(requestTimeoutMs + 1000);
    while (!interrupted && !completed && std::chrono::steady_clock::now() < requestDeadline)
      face.processEvents(ndn::time::milliseconds(20));
    if (!completed)
      timedOut = true;

    if (expectRejected) {
      if (timedOut || !response.getStatus()) {
        std::cout << "NATIVE_GRANT_PROCESS_REJECTED boundary="
                  << (timedOut ? "transport-timeout" : "authority-handler")
                  << " mode=" << mode;
        if (!timedOut)
          std::cout << " error=" << response.getErrorInfo();
        if (timedOut && !timeoutName.empty())
          std::cout << " request=" << timeoutName;
        std::cout << std::endl;
        return 0;
      }
      throw std::runtime_error("negative process case unexpectedly returned a grant");
    }
    if (timedOut)
      throw std::runtime_error("authority request timed out");
    if (!response.getStatus())
      throw std::runtime_error("authority rejected positive request: " + response.getErrorInfo());

    const auto payload = response.getPayload();
    const auto grant = nativeKeyGrantFromJson(std::string(
      reinterpret_cast<const char*>(payload.data()), payload.size()));
    ndnsf::di::detail::verifyNativeIssuedGrant(
      grant, request, authority.toUri(), publicBytes(*authorityKey), nowMs(), expiresAtMs);
    std::cout << "NATIVE_GRANT_PROCESS_POSITIVE grant=" << grant.grantName
              << " recipient=" << grant.recipient
              << " authority=" << authority << std::endl;
    return 0;
  }
  catch (const std::exception& error) {
    std::cerr << "NATIVE_GRANT_REQUESTER_FAILED: " << error.what() << std::endl;
    return 1;
  }
}
