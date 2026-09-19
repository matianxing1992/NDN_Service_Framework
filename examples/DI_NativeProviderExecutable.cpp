#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeArtifactMaterializer.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalOnnxAssembler.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRunnerPreparation.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProtectedProvider.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProtectedArtifactStore.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderOfferV3.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeStandaloneTokenizer.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeYoloMergeRunner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ExecutionLeaseService.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceProvider.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderReadiness.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderSession.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeServiceManifest.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/OnnxRuntimeModelRunner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"

#include "ndn-service-framework/CertificateBootstrap.hpp"
#include "ndn-service-framework/CertificatePublisher.hpp"
#include "ndn-service-framework/HybridMessageCrypto.hpp"
#include "ndn-service-framework/ServiceProvider.hpp"
#include "ndn-service-framework/ServiceUser.hpp"

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>
#include <ndn-cxx/security/validator-null.hpp>
#include <ndn-cxx/security/transform/base64-decode.hpp>
#include <ndn-cxx/security/transform/buffer-source.hpp>
#include <ndn-cxx/security/transform/stream-sink.hpp>
#include <ndn-cxx/util/segment-fetcher.hpp>
#include <ndn-cxx/util/sha256.hpp>
#include <ndn-cxx/util/io.hpp>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <chrono>
#include <csignal>
#include <cstring>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>
#include <unistd.h>

namespace {

using namespace ndnsf::di;

volatile std::sig_atomic_t g_shutdownRequested = 0;

void
requestShutdown(int)
{
  g_shutdownRequested = 1;
}

class PlaceholderDependencyIo final : public DependencyIo
{
public:
  std::future<TensorBundle>
  prefetchInput(const std::string&, const DependencyEdge& edge) final
  {
    throw std::logic_error("native provider check-only mode cannot fetch input: " +
                           edge.scope);
  }

  void
  publishOutput(const std::string&, const DependencyEdge&, const TensorBundle&) final
  {
    throw std::logic_error("native provider check-only mode cannot publish output");
  }
};

struct Options
{
  std::string planPath;
  std::string manifestPath;
  std::string serviceName = "/AI/YOLO/2x2Inference";
  std::string providerName = "/example/native-provider";
  std::string groupName = "/NDNSF-DistributeInference/example/group";
  std::string controllerName = "/NDNSF-DistributeInference/example/controller";
  std::string trustSchema = "examples/trust-schema.conf";
  std::string bootstrapToken;
  std::string roles = "all";
  std::string artifactReferencesPath;
  std::string artifactCacheDir = "/tmp/ndnsf-di-native-artifacts";
  // Deployment-local standalone tokenizer.  It is deliberately configured
  // outside the Selection wire contract and bound to the authenticated
  // tokenizer digest only after Selection.
  std::string tokenizerJson;
  std::string repoServiceName = "/NDNSF/DistributedRepo";
  int repoFetchTimeoutMs = 30000;
  int repoAckTimeoutMs = 500;
  int repoPermissionWaitMs = 3000;
  int permissionWaitMs = 30000;
  std::size_t workers = 1;
  std::size_t handlerThreads = 4;
  std::size_t ackThreads = 2;
  std::optional<int> runForMs;
  bool checkOnly = false;
  bool serve = false;
  bool noServeCertificates = false;
  bool disableTokens = false;
  bool wiringCheckOnly = false;
  bool tracerDeterministicRunner = false;
  // Rollback-only check-mode switch.  A serving Provider must always use
  // request-scoped canonical assembly after Selection.
  bool allowPreassembledDiagnostic = false;
  bool enableAdmissionLease = false;
  bool requireExecutionLease = false;
  std::string executionPolicy;
  int admissionLeaseTtlMs = 60000;
  std::string selectionOfferKeyFile;
  std::string offerBackend = "onnxruntime-cpu";
  std::vector<std::string> offerDevices;
  bool offerCanProvision = false;
  bool offerHasModel = false;
};

std::size_t
parseWorkers(const std::string& value)
{
  const auto workers = static_cast<std::size_t>(std::stoul(value));
  if (workers == 0) {
    throw std::invalid_argument("--workers must be greater than zero");
  }
  return workers;
}

int
parsePositiveInt(const std::string& value, const std::string& optionName)
{
  const auto parsed = std::stoi(value);
  if (parsed <= 0) {
    throw std::invalid_argument(optionName + " must be greater than zero");
  }
  return parsed;
}

long long
epochMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string
sha256File(const std::string& path)
{
  std::ifstream input(path, std::ios::binary);
  if (!input.good()) {
    throw std::runtime_error("cannot hash evidence file: " + path);
  }
  ndn::util::Sha256 digest;
  std::array<char, 65536> buffer{};
  while (input.good()) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = input.gcount();
    if (count > 0) {
      digest.update(ndn::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(buffer.data()), static_cast<std::size_t>(count)));
    }
  }
  auto hex = digest.toString();
  std::transform(hex.begin(), hex.end(), hex.begin(), [] (unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return "sha256:" + hex;
}

// OA02 worker location for post-Selection ONNX assembly.  The provider pins
// path AND content hash: every spawn re-probes the file and a mismatch fails
// the request before any fetch.  Resolution honors NDNSF_DI_WORKER_BINARY,
// then the invocation-directory siblings that a staged install produces.
// Returns an empty path when no worker is present (the request then fails
// deterministically with DI_PROVIDER_ASSEMBLY_WORKER_LOCATION_MISSING).
NativeOnnxWorkerLocation
resolveWorkerLocation()
{
  std::vector<std::string> candidates;
  const char* pinned = std::getenv("NDNSF_DI_WORKER_BINARY");
  if (pinned != nullptr && *pinned != '\0') {
    candidates.push_back(pinned);
  }
  std::array<char, 4096> selfPath{};
  const auto linkCount = ::readlink("/proc/self/exe", selfPath.data(),
                                    selfPath.size() - 1);
  if (linkCount > 0) {
    selfPath[linkCount] = '\0';
    const std::string invocationDir =
      std::filesystem::path(selfPath.data()).parent_path().string();
    candidates.push_back(invocationDir + "/DI_NativeOnnxAssemblyWorker");
    candidates.push_back(
      (std::filesystem::path(invocationDir).parent_path() /
       "DI_NativeOnnxAssemblyWorker").string());
  }
  candidates.push_back("build-nac182/DI_NativeOnnxAssemblyWorker");
  candidates.push_back("build/DI_NativeOnnxAssemblyWorker");
  for (const auto& path : candidates) {
    std::error_code error;
    if (std::filesystem::is_regular_file(path, error) && !error) {
      try {
        return NativeOnnxWorkerLocation{path, sha256File(path)};
      }
      catch (const std::exception&) {
        return NativeOnnxWorkerLocation{path, ""};
      }
    }
  }
  std::cerr << "DI_NativeProviderExecutable: no DI_NativeOnnxAssemblyWorker "
               "found; onnx post-Selection requests will fail with "
               "DI_PROVIDER_ASSEMBLY_WORKER_LOCATION_MISSING\n";
  return {};
}

ndn::Buffer
textBuffer(const std::string& text)
{
  return ndn::Buffer(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

std::string
bufferText(const ndn::Buffer& payload)
{
  return std::string(reinterpret_cast<const char*>(payload.data()), payload.size());
}

struct NativeOfferSigner
{
  std::string keyId;
  std::function<std::string(const std::string&)> sign;
};

std::string
base64Encode(const std::vector<unsigned char>& value)
{
  if (value.empty()) {
    return {};
  }
  std::string output(4 * ((value.size() + 2) / 3), '\0');
  const auto size = EVP_EncodeBlock(
    reinterpret_cast<unsigned char*>(&output[0]), value.data(),
    static_cast<int>(value.size()));
  if (size < 0) {
    throw std::runtime_error("failed to base64-encode V3 Provider offer signature");
  }
  output.resize(static_cast<std::size_t>(size));
  return output;
}

NativeOfferSigner
loadNativeOfferSigner(const std::string& path)
{
  auto* rawBio = BIO_new_file(path.c_str(), "rb");
  if (rawBio == nullptr) {
    throw std::runtime_error("cannot open selection offer signing key: " + path);
  }
  std::unique_ptr<BIO, decltype(&BIO_free)> bio(rawBio, &BIO_free);
  auto* rawKey = PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr);
  if (rawKey == nullptr) {
    throw std::runtime_error("invalid selection offer signing key: " + path);
  }
  std::shared_ptr<EVP_PKEY> key(rawKey, &EVP_PKEY_free);
  if (EVP_PKEY_base_id(key.get()) != EVP_PKEY_ED25519) {
    throw std::runtime_error("selection offer signing key must be Ed25519");
  }

  std::size_t publicSize = 0;
  if (EVP_PKEY_get_raw_public_key(key.get(), nullptr, &publicSize) != 1 ||
      publicSize == 0) {
    throw std::runtime_error("failed to derive selection offer public key");
  }
  std::vector<std::uint8_t> publicKey(publicSize);
  if (EVP_PKEY_get_raw_public_key(key.get(), publicKey.data(), &publicSize) != 1) {
    throw std::runtime_error("failed to read selection offer public key");
  }
  publicKey.resize(publicSize);
  ndn::util::Sha256 publicDigest;
  publicDigest.update(ndn::span<const std::uint8_t>(publicKey.data(), publicKey.size()));
  auto keyId = publicDigest.toString();
  std::transform(keyId.begin(), keyId.end(), keyId.begin(), [] (unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });

  NativeOfferSigner result;
  result.keyId = "sha256:" + keyId;
  result.sign = [key = std::move(key)] (const std::string& digest) {
    auto* rawContext = EVP_MD_CTX_new();
    if (rawContext == nullptr) {
      throw std::runtime_error("failed to allocate V3 Provider offer signer");
    }
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>
      context(rawContext, &EVP_MD_CTX_free);
    if (EVP_DigestSignInit(context.get(), nullptr, nullptr, nullptr, key.get()) != 1) {
      throw std::runtime_error("failed to initialize V3 Provider offer signer");
    }
    std::size_t signatureSize = 0;
    if (EVP_DigestSign(
          context.get(), nullptr, &signatureSize,
          reinterpret_cast<const unsigned char*>(digest.data()), digest.size()) != 1 ||
        signatureSize == 0) {
      throw std::runtime_error("failed to size V3 Provider offer signature");
    }
    std::vector<unsigned char> signature(signatureSize);
    if (EVP_DigestSign(
          context.get(), signature.data(), &signatureSize,
          reinterpret_cast<const unsigned char*>(digest.data()), digest.size()) != 1) {
      throw std::runtime_error("failed to sign V3 Provider offer");
    }
    signature.resize(signatureSize);
    return base64Encode(signature);
  };
  return result;
}

std::string
signNativeAssemblyManifest(ndn::KeyChain& keyChain,
                           const ndn::security::Certificate& providerCert,
                           const std::string& manifestBytes)
{
  ndn::Data signedManifest(ndn::Name("/NDNSF-DI/ASSEMBLY-MANIFEST"));
  signedManifest.setContent(ndn::span<const std::uint8_t>(
    reinterpret_cast<const std::uint8_t*>(manifestBytes.data()),
    manifestBytes.size()));
  keyChain.sign(
    signedManifest,
    ndn::security::signingByCertificate(providerCert));
  const auto signature = signedManifest.getSignatureValue();
  if (signature.value_size() == 0) {
    return {};
  }
  return ndn_service_framework::selectionGatedHex(
    ndn::span<const std::uint8_t>(
      signature.value_begin(), signature.value_size()));
}

std::string
nativeTracerLeaseId(const std::string& providerName)
{
  static std::atomic<std::uint64_t> counter{0};
  return "native-tracer-lease-" + std::to_string(epochMs()) + "-" +
         std::to_string(++counter) + "-" + std::to_string(std::hash<std::string>{}(providerName));
}

std::string
nativeTracerLeaseProof(const std::vector<std::string>& allowedRoles)
{
  if (allowedRoles.size() == 1) {
    return "role=" + allowedRoles.front();
  }
  return "";
}

std::vector<std::string>
splitCsv(const std::string& value)
{
  std::vector<std::string> items;
  std::stringstream input(value);
  std::string item;
  while (std::getline(input, item, ',')) {
    item.erase(item.begin(),
               std::find_if(item.begin(), item.end(), [] (unsigned char ch) {
                 return !std::isspace(ch);
               }));
    item.erase(std::find_if(item.rbegin(), item.rend(), [] (unsigned char ch) {
                 return !std::isspace(ch);
               }).base(),
               item.end());
    if (!item.empty()) {
      items.push_back(item);
    }
  }
  return items;
}

std::vector<std::string>
splitNames(const std::string& value)
{
  std::vector<std::string> names;
  std::stringstream input(value);
  std::string current;
  while (std::getline(input, current, ',')) {
    if (!current.empty()) {
      names.push_back(current);
    }
  }
  return names;
}

std::vector<std::string>
outputScopesFromMetadata(const NativeModelRunnerSpec& spec)
{
  std::vector<std::string> scopes;
  auto direct = spec.metadata.find("outputScope");
  if (direct != spec.metadata.end() && !direct->second.empty()) {
    scopes.push_back(direct->second);
  }
  for (std::size_t index = 0;; ++index) {
    auto found = spec.metadata.find("outputScope." + std::to_string(index));
    if (found == spec.metadata.end()) {
      break;
    }
    if (!found->second.empty()) {
      scopes.push_back(found->second);
    }
  }
  if (scopes.empty()) {
    scopes.push_back("final-response");
  }
  return scopes;
}

std::vector<std::uint8_t>
float32Payload(const std::vector<float>& values)
{
  std::vector<std::uint8_t> payload(values.size() * sizeof(float));
  std::memcpy(payload.data(), values.data(), payload.size());
  return payload;
}

double
metadataDoubleValue(const NativeModelRunnerSpec& spec,
                    const std::string& key,
                    double fallback = 0.0)
{
  const auto found = spec.metadata.find(key);
  if (found == spec.metadata.end() || found->second.empty()) {
    return fallback;
  }
  return std::stod(found->second);
}

std::shared_ptr<NativeModelRunner>
makeTracerDeterministicRunner(const NativeModelRunnerSpec& spec)
{
  return makeNativeModelRunner(
    [spec] (const RoleExecutionContext&) {
      const auto executionDelayMs = metadataDoubleValue(spec, "executionDelayMs");
      if (executionDelayMs > 0.0) {
        std::this_thread::sleep_for(
          std::chrono::duration<double, std::milli>(executionDelayMs));
      }
      auto outputNames = splitNames(spec.metadata.count("output_tensors") ?
                                    spec.metadata.at("output_tensors") : "");
      if (outputNames.empty()) {
        outputNames.push_back("output");
      }
      std::vector<NamedTensor> outputs;
      outputs.reserve(outputNames.size());
      std::vector<std::int64_t> outputShape{1, 1};
      if (spec.role == "/Backbone") {
        outputShape = {1, 16};
      }
      else if (spec.role.find("/Head/") != std::string::npos) {
        outputShape = {1, 8};
      }
      else if (spec.role == "/Merge") {
        outputShape = {1, 4};
      }
      float value = 1.0f;
      for (const auto& name : outputNames) {
        const auto elementCount = static_cast<std::size_t>(
          std::accumulate(outputShape.begin(), outputShape.end(), std::int64_t{1},
                          [] (std::int64_t total, std::int64_t dimension) {
                            return total * dimension;
                          }));
        std::vector<float> values(elementCount, value);
        outputs.push_back(makeFloat32Tensor(name, outputShape, float32Payload(values)));
        value += 1.0f;
      }
      const bool forceEncodedOutput =
        spec.metadata.find("forceOutputBundle") != spec.metadata.end();
      std::map<std::string, TensorBundle> byScope;
      for (const auto& scope : outputScopesFromMetadata(spec)) {
        if (outputs.size() == 1 && !forceEncodedOutput) {
          TensorBundle bundle;
          bundle.name = outputs.front().name;
          bundle.payload = outputs.front().payload;
          bundle.expectedBytes = bundle.payload.size();
          byScope.emplace(scope, std::move(bundle));
        }
        else {
          byScope.emplace(scope, makeEncodedTensorBundle(scope, outputs));
        }
      }
      return byScope;
    });
}

std::string
jsonEscape(const std::string& text)
{
  std::ostringstream output;
  for (const auto ch : text) {
    switch (ch) {
      case '\\':
        output << "\\\\";
        break;
      case '"':
        output << "\\\"";
        break;
      case '\n':
        output << "\\n";
        break;
      case '\r':
        output << "\\r";
        break;
      case '\t':
        output << "\\t";
        break;
      default:
        output << ch;
        break;
    }
  }
  return output.str();
}

std::vector<std::uint8_t>
decodeBase64Payload(const std::string& encoded)
{
  namespace transform = ndn::security::transform;
  std::stringstream output;
  transform::bufferSource(std::string_view(encoded)) >>
    transform::base64Decode() >>
    transform::streamSink(output);
  const auto decoded = output.str();
  return std::vector<std::uint8_t>(decoded.begin(), decoded.end());
}

std::vector<std::uint8_t>
decodeRepoFetchResponse(const ndn_service_framework::ResponseMessage& response)
{
  const auto buffer = response.getPayload();
  const std::string text(buffer.begin(), buffer.end());
  std::istringstream input(text);
  boost::property_tree::ptree root;
  boost::property_tree::read_json(input, root);
  return decodeBase64Payload(root.get<std::string>("payloadB64"));
}

struct RepoSegmentFetchPlan
{
  std::string dataName;
  std::vector<std::string> forwardingHints;
  std::size_t segmentCount = 0;
};

std::vector<std::string>
parseStringArray(const boost::property_tree::ptree& node, const std::string& key)
{
  std::vector<std::string> values;
  const auto child = node.get_child_optional(key);
  if (!child) {
    return values;
  }
  for (const auto& item : child.get()) {
    values.push_back(item.second.get_value<std::string>());
  }
  return values;
}

std::optional<RepoSegmentFetchPlan>
repoSegmentFetchPlanFromManifestJson(const std::string& manifestJson)
{
  if (manifestJson.empty()) {
    return std::nullopt;
  }
  std::istringstream input(manifestJson);
  boost::property_tree::ptree manifest;
  boost::property_tree::read_json(input, manifest);

  RepoSegmentFetchPlan plan;
  plan.segmentCount = manifest.get<std::size_t>("segmentCount", 0);
  const auto locations = manifest.get_child_optional("segmentLocations");
  if (locations) {
    for (const auto& item : locations.get()) {
      const auto& location = item.second;
      const auto dataName = location.get<std::string>("dataName", "");
      if (dataName.empty()) {
        continue;
      }
      const auto start = location.get<std::size_t>("start", 0);
      const auto end = location.get<std::size_t>("end", start);
      if (plan.segmentCount > 0 && start != 0 && end + 1 < plan.segmentCount) {
        continue;
      }
      plan.dataName = dataName;
      plan.forwardingHints = parseStringArray(location, "hints");
      const auto repoNode = location.get<std::string>("repoNode", "");
      if (plan.forwardingHints.empty() && !repoNode.empty() &&
          dataName.rfind(repoNode, 0) != 0) {
        plan.forwardingHints.push_back(repoNode);
      }
      return plan;
    }
  }

  const auto dataNames = parseStringArray(manifest, "replicaDataNames");
  if (!dataNames.empty()) {
    plan.dataName = dataNames.front();
    const auto replicaNodes = parseStringArray(manifest, "replicaNodes");
    if (!replicaNodes.empty() && plan.dataName.rfind(replicaNodes.front(), 0) != 0) {
      plan.forwardingHints.push_back(replicaNodes.front());
    }
    return plan;
  }
  return std::nullopt;
}

std::vector<std::uint8_t>
fetchSegmentedRepoObjectSync(ndn::Face& face,
                             const RepoSegmentFetchPlan& plan,
                             int timeoutMs)
{
  bool done = false;
  std::optional<std::string> error;
  std::vector<std::uint8_t> payload;

  ndn::Interest interest(ndn::Name(plan.dataName));
  interest.setCanBePrefix(true);
  interest.setMustBeFresh(false);
  interest.setInterestLifetime(ndn::time::milliseconds(4000));
  if (!plan.forwardingHints.empty()) {
    std::vector<ndn::Name> hints;
    hints.reserve(plan.forwardingHints.size());
    for (const auto& hint : plan.forwardingHints) {
      if (!hint.empty()) {
        hints.emplace_back(hint);
      }
    }
    interest.setForwardingHint(std::move(hints));
  }

  ndn::SegmentFetcher::Options fetchOptions;
  fetchOptions.probeLatestVersion = false;
  fetchOptions.useConstantCwnd = true;
  fetchOptions.initCwnd = 8.0;
  fetchOptions.maxTimeout = ndn::time::milliseconds(timeoutMs);
  fetchOptions.interestLifetime = ndn::time::milliseconds(4000);
  auto validator = std::make_shared<ndn::security::ValidatorNull>();
  auto fetcher = ndn::SegmentFetcher::start(face, interest, *validator, fetchOptions);
  fetcher->onComplete.connect(
    [&] (ndn::ConstBufferPtr buffer) {
      payload.assign(buffer->begin(), buffer->end());
      done = true;
    });
  fetcher->onError.connect(
    [&] (uint32_t code, const std::string& message) {
      error = "repo segmented fetch error " + std::to_string(code) + ": " + message;
      done = true;
    });

  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeoutMs + 1000);
  while (!done && std::chrono::steady_clock::now() < deadline) {
    face.processEvents(ndn::time::milliseconds(10));
  }
  if (!done) {
    throw std::runtime_error("repo segmented fetch did not complete before local deadline for " +
                             plan.dataName);
  }
  if (error) {
    throw std::runtime_error(*error);
  }
  return payload;
}

ndn::security::Certificate
getOrCreateIdentity(ndn::KeyChain& keyChain, const ndn::Name& identity)
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
loadControllerCertificate(const ndn::Name& controller,
                          ndn::KeyChain& keyChain)
{
  if (const char* certPath = std::getenv("NDNSF_CONTROLLER_CERT_FILE");
      certPath != nullptr && *certPath != '\0') {
    auto cert = ndn::io::load<ndn::security::Certificate>(certPath);
    if (cert == nullptr || !cert->isValid()) {
      throw std::runtime_error(
        "NDNSF_CONTROLLER_CERT_FILE is not a valid certificate: " +
        std::string(certPath));
    }
    if (cert->getIdentity() != controller) {
      throw std::runtime_error(
        "NDNSF_CONTROLLER_CERT_FILE identity " + cert->getIdentity().toUri() +
        " does not match controller " + controller.toUri());
    }
    return *cert;
  }
  return getOrCreateIdentity(keyChain, controller);
}

Options
parseArgs(int argc, char** argv)
{
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto readValue = [&] {
      if (i + 1 >= argc) {
        throw std::invalid_argument("missing value for " + arg);
      }
      return std::string(argv[++i]);
    };

    if (arg == "--plan") {
      options.planPath = readValue();
    }
    else if (arg == "--manifest") {
      options.manifestPath = readValue();
    }
    else if (arg == "--service") {
      options.serviceName = readValue();
    }
    else if (arg == "--provider") {
      options.providerName = readValue();
    }
    else if (arg == "--group") {
      options.groupName = readValue();
    }
    else if (arg == "--controller") {
      options.controllerName = readValue();
    }
    else if (arg == "--trust-schema") {
      options.trustSchema = readValue();
    }
    else if (arg == "--bootstrap-token") {
      options.bootstrapToken = readValue();
    }
    else if (arg == "--roles") {
      options.roles = readValue();
    }
    else if (arg == "--artifact-references") {
      options.artifactReferencesPath = readValue();
    }
    else if (arg == "--artifact-cache-dir") {
      options.artifactCacheDir = readValue();
    }
    else if (arg == "--tokenizer-json") {
      options.tokenizerJson = readValue();
    }
    else if (arg == "--repo-service") {
      options.repoServiceName = readValue();
    }
    else if (arg == "--repo-fetch-timeout-ms") {
      options.repoFetchTimeoutMs = parsePositiveInt(readValue(), "--repo-fetch-timeout-ms");
    }
    else if (arg == "--repo-ack-timeout-ms") {
      options.repoAckTimeoutMs = parsePositiveInt(readValue(), "--repo-ack-timeout-ms");
    }
    else if (arg == "--repo-permission-wait-ms") {
      options.repoPermissionWaitMs = parsePositiveInt(readValue(), "--repo-permission-wait-ms");
    }
    else if (arg == "--permission-wait-ms") {
      options.permissionWaitMs = parsePositiveInt(readValue(), "--permission-wait-ms");
    }
    else if (arg == "--workers") {
      options.workers = parseWorkers(readValue());
    }
    else if (arg == "--handler-threads") {
      options.handlerThreads = parseWorkers(readValue());
    }
    else if (arg == "--ack-threads") {
      options.ackThreads = parseWorkers(readValue());
    }
    else if (arg == "--check-only") {
      options.checkOnly = true;
    }
    else if (arg == "--serve") {
      options.serve = true;
    }
    else if (arg == "--run-for-ms") {
      options.runForMs = parsePositiveInt(readValue(), "--run-for-ms");
    }
    else if (arg == "--no-serve-certificates") {
      options.noServeCertificates = true;
    }
    else if (arg == "--disable-tokens") {
      options.disableTokens = true;
    }
    else if (arg == "--wiring-check-only") {
      options.wiringCheckOnly = true;
    }
    else if (arg == "--tracer-deterministic-runner") {
      options.tracerDeterministicRunner = true;
    }
    else if (arg == "--allow-preassembled-diagnostic") {
      options.allowPreassembledDiagnostic = true;
    }
    else if (arg == "--enable-admission-lease") {
      options.enableAdmissionLease = true;
    }
    else if (arg == "--require-execution-lease") {
      options.requireExecutionLease = true;
    }
    else if (arg == "--execution-policy") {
      options.executionPolicy = readValue();
    }
    else if (arg == "--admission-lease-ttl-ms") {
      options.admissionLeaseTtlMs = parsePositiveInt(readValue(), "--admission-lease-ttl-ms");
    }
    else if (arg == "--selection-offer-key-file") {
      options.selectionOfferKeyFile = readValue();
    }
    else if (arg == "--offer-backend") {
      options.offerBackend = readValue();
    }
    else if (arg == "--offer-device") {
      options.offerDevices.push_back(readValue());
    }
    else if (arg == "--offer-can-provision") {
      options.offerCanProvision = true;
    }
    else if (arg == "--offer-has-model") {
      options.offerHasModel = true;
    }
    else {
      throw std::invalid_argument("unknown argument: " + arg);
    }
  }

  if (options.planPath.empty()) {
    throw std::invalid_argument("--plan is required");
  }
  if (options.manifestPath.empty()) {
    throw std::invalid_argument("--manifest is required");
  }
  if (options.tokenizerJson.empty()) {
    if (const char* tokenizer = std::getenv("NDNSF_DI_TOKENIZER_JSON");
        tokenizer != nullptr && *tokenizer != '\0') {
      options.tokenizerJson = tokenizer;
    }
  }
  if (options.wiringCheckOnly && !options.checkOnly) {
    throw std::invalid_argument("--wiring-check-only requires --check-only");
  }
  if (options.allowPreassembledDiagnostic && !options.checkOnly) {
    throw std::invalid_argument(
      "--allow-preassembled-diagnostic is restricted to --check-only");
  }
  if (!options.executionPolicy.empty() &&
      options.executionPolicy != "DATA_DRIVEN_V2" &&
      options.executionPolicy != "LEGACY_READY_SET_V1") {
    throw std::invalid_argument("unsupported --execution-policy");
  }
  if (options.executionPolicy == "LEGACY_READY_SET_V1" &&
      !options.requireExecutionLease) {
    throw std::invalid_argument(
      "LEGACY_READY_SET_V1 requires --require-execution-lease");
  }
  if (!options.selectionOfferKeyFile.empty() && options.offerBackend.empty()) {
    throw std::invalid_argument("--offer-backend must not be empty");
  }
  return options;
}

NativeExecutionPlan
loadPlan(const Options& options)
{
  std::ifstream input(options.planPath);
  if (!input.good()) {
    throw std::runtime_error("cannot open native execution plan: " + options.planPath);
  }
  return nativeExecutionPlanForServiceFromJson(input, options.serviceName);
}

std::map<std::string, NativeModelRunnerSpec>
loadManifestSpecs(const Options& options)
{
  std::ifstream input(options.manifestPath);
  if (!input.good()) {
    throw std::runtime_error("cannot open service manifest: " + options.manifestPath);
  }
  return nativeModelRunnerSpecsByRoleForServiceManifestFromJson(input, options.serviceName);
}

std::map<std::string, NativeModelRunnerSpec>
withExecutionEvidenceContext(std::map<std::string, NativeModelRunnerSpec> specs,
                             const Options& options,
                             const std::string& providerBootId,
                             std::uint64_t createdAtMs)
{
  const auto planDigest = sha256File(options.planPath);
  const auto manifestDigest = sha256File(options.manifestPath);
  const auto* profileRoot = std::getenv("NDNSF_DI_ORT_PROFILE_PREFIX");
  for (auto& item : specs) {
    auto& spec = item.second;
    spec.metadata["evidence.providerName"] = options.providerName;
    spec.metadata["evidence.providerBootId"] = providerBootId;
    spec.metadata["evidence.epoch"] = "1";
    spec.metadata["evidence.createdAtMs"] = std::to_string(createdAtMs);
    spec.metadata["evidence.planDigest"] = planDigest;
    spec.metadata["evidence.modelDigest"] = manifestDigest;
    std::ifstream artifact(spec.path, std::ios::binary);
    spec.metadata["evidence.artifactDigest"] =
      spec.path.empty() || !artifact.good() ? manifestDigest : sha256File(spec.path);
    if (profileRoot != nullptr && *profileRoot != '\0') {
      auto provider = options.providerName;
      std::replace_if(provider.begin(), provider.end(), [] (unsigned char ch) {
        return !(std::isalnum(ch) || ch == '-' || ch == '_');
      }, '_');
      auto role = spec.role;
      std::replace_if(role.begin(), role.end(), [] (unsigned char ch) {
        return !(std::isalnum(ch) || ch == '-' || ch == '_');
      }, '_');
      spec.metadata["providerProfilePrefix"] = std::string(profileRoot) + "-" + provider + "-" + role;
    }
  }
  return specs;
}

ExecutionEvidence
aggregateExecutionEvidence(const std::vector<ExecutionEvidence>& items)
{
  if (items.empty()) {
    throw std::runtime_error("initialized provider runners emitted no execution evidence");
  }
  auto aggregate = items.front();
  auto appendUnique = [] (std::vector<std::string>& values, const std::string& value) {
    if (!value.empty() && std::find(values.begin(), values.end(), value) == values.end()) {
      values.push_back(value);
    }
  };
  auto seedDeviceIds = [&] (const ExecutionEvidence& evidence) {
    if (!evidence.deviceIds.empty()) {
      for (const auto& value : evidence.deviceIds) appendUnique(aggregate.deviceIds, value);
    }
    else {
      appendUnique(aggregate.deviceIds, evidence.deviceId);
    }
  };
  auto seedGpuUuids = [&] (const ExecutionEvidence& evidence) {
    if (!evidence.gpuUuids.empty()) {
      for (const auto& value : evidence.gpuUuids) appendUnique(aggregate.gpuUuids, value);
    }
    else {
      appendUnique(aggregate.gpuUuids, evidence.gpuUuid);
    }
  };
  aggregate.deviceIds.clear();
  aggregate.gpuUuids.clear();
  seedDeviceIds(items.front());
  seedGpuUuids(items.front());
  for (std::size_t i = 1; i < items.size(); ++i) {
    const auto& item = items[i];
    if (item.providerName != aggregate.providerName ||
        item.providerBootId != aggregate.providerBootId ||
        item.runnerKind != aggregate.runnerKind ||
        item.realCompute != aggregate.realCompute ||
        item.modelDigest != aggregate.modelDigest ||
        item.planDigest != aggregate.planDigest ||
        item.runtimeVersion != aggregate.runtimeVersion ||
        item.deviceKind != aggregate.deviceKind ||
        item.cpuFallbackUsed != aggregate.cpuFallbackUsed) {
      throw std::runtime_error("provider runner execution evidence is internally inconsistent");
    }
    seedDeviceIds(item);
    seedGpuUuids(item);
    aggregate.roles.insert(aggregate.roles.end(), item.roles.begin(), item.roles.end());
    aggregate.artifactDigests.insert(item.artifactDigests.begin(), item.artifactDigests.end());
    aggregate.nodeProviderAssignments.insert(
      aggregate.nodeProviderAssignments.end(),
      item.nodeProviderAssignments.begin(), item.nodeProviderAssignments.end());
    if (aggregate.providerProfilePath.empty()) {
      aggregate.providerProfilePath = item.providerProfilePath;
    }
  }
  if (aggregate.deviceIds.size() == 1) {
    aggregate.deviceId = aggregate.deviceIds.front();
  }
  else if (aggregate.deviceIds.size() > 1) {
    aggregate.deviceId = "multi";
  }
  if (aggregate.gpuUuids.size() == 1) {
    aggregate.gpuUuid = aggregate.gpuUuids.front();
  }
  else if (aggregate.gpuUuids.size() > 1) {
    aggregate.gpuUuid = "multi";
  }
  std::sort(aggregate.roles.begin(), aggregate.roles.end());
  aggregate.roles.erase(std::unique(aggregate.roles.begin(), aggregate.roles.end()),
                        aggregate.roles.end());
  if (items.size() > 1) {
    // Readiness aggregation may contain different requests or old role
    // snapshots. It is not one per-request/per-profile execution observation.
    aggregate.executionCompleted = false;
    aggregate.exactForwardCacheHit = false;
    aggregate.requestId.clear();
    aggregate.attemptEpoch = 0;
    aggregate.profileRequestId.clear();
    aggregate.profileAttemptEpoch = 0;
  }
  aggregate.validate();
  return aggregate;
}

std::map<std::string, NativeModelRunnerSpec>
materializeManifestSpecs(const Options& options,
                         const std::map<std::string, NativeModelRunnerSpec>& specs,
                         std::function<std::vector<std::uint8_t>(
                           const std::string&, const std::string&)> repoFetchFromManifest = {},
                         std::function<std::vector<std::uint8_t>(const std::string&)> repoFetch = {})
{
  if (options.artifactReferencesPath.empty()) {
    return specs;
  }
  std::ifstream input(options.artifactReferencesPath);
  if (!input.good()) {
    throw std::runtime_error("cannot open artifact references: " +
                             options.artifactReferencesPath);
  }
  NativeArtifactMaterializerOptions materializerOptions;
  materializerOptions.cacheDir = options.artifactCacheDir;
  materializerOptions.repoFetchFromManifest = std::move(repoFetchFromManifest);
  materializerOptions.repoFetch = std::move(repoFetch);
  auto materialized = materializeNativeModelArtifactsFromReferencesJson(
    specs,
    input,
    materializerOptions);
  std::cout << "NDNSF_DI_NATIVE_PROVIDER_ARTIFACTS_MATERIALIZED"
            << " references=" << options.artifactReferencesPath
            << " cacheDir=" << options.artifactCacheDir
            << " repoFetchFromManifest=" << (materializerOptions.repoFetchFromManifest ? 1 : 0)
            << " repoFetch=" << (materializerOptions.repoFetch ? 1 : 0)
            << std::endl;
  return materialized;
}

bool
waitForUserPermission(ndn_service_framework::ServiceUser& user,
                      ndn::Face& face,
                      const ndn::Name& serviceName,
                      int timeoutMs)
{
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeoutMs);
  while (std::chrono::steady_clock::now() < deadline) {
    for (const auto& entry : user.getAllowedServices()) {
      if (std::get<1>(entry) == serviceName.toUri()) {
        return true;
      }
    }
    face.processEvents(ndn::time::milliseconds(10));
  }
  return false;
}

std::vector<std::uint8_t>
fetchRepoObjectSync(ndn_service_framework::ServiceUser& user,
                    ndn::Face& face,
                    const ndn::Name& repoServiceName,
                    const std::string& objectName,
                    int ackTimeoutMs,
                    int timeoutMs)
{
  bool done = false;
  std::optional<std::string> error;
  std::vector<std::uint8_t> payload;
  const auto requestJson = std::string("{\"objectName\":\"") +
                           jsonEscape(objectName) +
                           "\",\"operation\":\"FETCH\"}";
  std::vector<std::uint8_t> requestPayload(requestJson.begin(), requestJson.end());
  auto request = ndn_service_framework::RequestMessage();
  auto buffer = ndn::Buffer(requestPayload.data(), requestPayload.size());
  request.setPayload(buffer, buffer.size());
  auto selector = ndn_service_framework::ServiceUser::makeAckSelectionHandler(
    ndn_service_framework::ServiceUser::AckSelectionStrategy::FirstRespondingSelection);
  user.RequestService(
    {},
    repoServiceName,
    request,
    ackTimeoutMs,
    std::move(selector),
    timeoutMs,
    [&] (const ndn::Name& requestId) {
      error = "repo fetch timeout for " + objectName + " requestId=" + requestId.toUri();
      done = true;
    },
    [&] (const ndn_service_framework::ResponseMessage& response) {
      payload = decodeRepoFetchResponse(response);
      done = true;
    },
    ndn_service_framework::tlv::FirstResponding);

  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeoutMs + 1000);
  while (!done && std::chrono::steady_clock::now() < deadline) {
    face.processEvents(ndn::time::milliseconds(10));
  }
  if (!done) {
    throw std::runtime_error("repo fetch did not complete before local deadline for " +
                             objectName);
  }
  if (error) {
    throw std::runtime_error(*error);
  }
  return payload;
}

std::vector<NativeModelRunnerSpec>
orderedSpecs(const NativeExecutionPlan& plan,
             const std::map<std::string, NativeModelRunnerSpec>& specs,
             const std::vector<std::string>& roles)
{
  std::vector<NativeModelRunnerSpec> ordered;
  ordered.reserve(roles.size());
  for (const auto& role : roles) {
    if (std::find(plan.roles.begin(), plan.roles.end(), role) == plan.roles.end()) {
      throw std::runtime_error("runner role is not in native plan: " + role);
    }
    const auto found = specs.find(role);
    if (found == specs.end()) {
      throw std::runtime_error("service manifest missing artifact for role: " + role);
    }
    ordered.push_back(found->second);
  }
  return ordered;
}

NativeProviderAssignment
defaultAssignment(const NativeExecutionPlan& plan,
                  const std::string& providerName,
                  const std::vector<std::string>& allowedRoles)
{
  NativeProviderAssignment assignment;
  for (const auto& role : allowedRoles) {
    if (std::find(plan.roles.begin(), plan.roles.end(), role) == plan.roles.end()) {
      throw std::invalid_argument("provider assignment role is not in native plan: " + role);
    }
    assignment.providerByRole[role] = providerName;
  }
  return assignment;
}

std::vector<std::string>
allowedRolesForOptions(const NativeExecutionPlan& plan, const Options& options)
{
  if (options.roles == "all") {
    return plan.roles;
  }
  auto roles = splitCsv(options.roles);
  if (roles.empty()) {
    throw std::invalid_argument("--roles must be all or a comma-separated role list");
  }
  for (const auto& role : roles) {
    if (std::find(plan.roles.begin(), plan.roles.end(), role) == plan.roles.end()) {
      throw std::invalid_argument("--roles contains role not in plan: " + role);
    }
  }
  return roles;
}

std::string
joinRoles(const std::vector<std::string>& roles)
{
  std::ostringstream output;
  for (std::size_t i = 0; i < roles.size(); ++i) {
    if (i > 0) {
      output << ',';
    }
    output << roles[i];
  }
  return output.str();
}

void
printUsage(const char* program)
{
  std::cerr
    << "usage: " << program << " --plan <native-execution-plan.json> "
    << "--manifest <service-manifest.json> [--service <name>] "
    << "[--provider <identity>] [--workers <n>] (--check-only | --serve) "
    << "[--run-for-ms <ms>] "
    << "[--roles all|role,...] [--group <prefix>] [--controller <prefix>] "
    << "[--trust-schema <path>] [--bootstrap-token <token>] "
    << "[--artifact-references <json>] "
    << "[--artifact-cache-dir <dir>] [--repo-service <service>] "
    << "[--tokenizer-json <path>] "
    << "[--repo-fetch-timeout-ms <ms>] [--repo-ack-timeout-ms <ms>] "
    << "[--repo-permission-wait-ms <ms>] [--wiring-check-only] "
    << "[--permission-wait-ms <ms>] "
    << "[--tracer-deterministic-runner] [--enable-admission-lease] "
    << "[--allow-preassembled-diagnostic] "
    << "[--require-execution-lease] "
    << "[--execution-policy DATA_DRIVEN_V2|LEGACY_READY_SET_V1] "
    << "[--admission-lease-ttl-ms <ms>] "
    << "[--selection-offer-key-file <ed25519.pem>] "
    << "[--offer-backend <name>] [--offer-device <device>] "
    << "[--offer-can-provision] [--offer-has-model]\n";
}

} // namespace

int
main(int argc, char** argv)
{
  try {
    std::signal(SIGINT, requestShutdown);
    std::signal(SIGTERM, requestShutdown);
    auto options = parseArgs(argc, argv);
    std::cout << "NDNSF_DI_NATIVE_PROVIDER_START mode="
              << (options.serve ? "serve" : "check")
              << " service=" << options.serviceName
              << " identity=" << options.providerName
              << " roles=" << options.roles
              << std::endl;
    if (options.checkOnly == options.serve) {
      throw std::invalid_argument(
        "exactly one of --check-only or --serve is required");
    }
    if (options.runForMs && !options.serve) {
      throw std::invalid_argument("--run-for-ms requires --serve");
    }

    auto plan = loadPlan(options);
    if (!options.executionPolicy.empty() &&
        options.executionPolicy != plan.executionPolicy) {
      throw std::invalid_argument(
        "--execution-policy does not match the sealed native plan");
    }
    if (plan.executionPolicy == "LEGACY_READY_SET_V1" &&
        !options.requireExecutionLease) {
      throw std::invalid_argument(
        "LEGACY_READY_SET_V1 plan requires --require-execution-lease");
    }
    const auto providerStartedAtMs = static_cast<std::uint64_t>(std::max<long long>(0, epochMs()));
    auto providerBootId = options.providerName + "@" + std::to_string(providerStartedAtMs);
    auto specs = withExecutionEvidenceContext(loadManifestSpecs(options), options,
                                              providerBootId, providerStartedAtMs);
    const auto allowedRoles = allowedRolesForOptions(plan, options);
    if (options.serve) {
      // A request-scoped serving manifest intentionally carries role names but
      // no preassembled artifact paths. Keep one metadata-only runner slot per
      // advertised role so ordered registration can complete; the authenticated
      // Selection projection replaces each slot with its certified ONNX or
      // native postprocess runner before execution.
      for (const auto& role : allowedRoles) {
        if (specs.find(role) != specs.end()) {
          continue;
        }
        NativeModelRunnerSpec spec;
        spec.role = role;
        spec.kind = role == "Merge" ? "native-yolo-postprocess" : "onnx-model";
        spec.backend = role == "Merge" ? "native-yolo-postprocess" : options.offerBackend;
        specs.emplace(role, std::move(spec));
      }
      specs = withExecutionEvidenceContext(
        std::move(specs), options, providerBootId, providerStartedAtMs);
    }
    std::optional<NativeProviderOfferV3Config> nativeOfferConfig;
    if (!options.selectionOfferKeyFile.empty()) {
      auto signer = loadNativeOfferSigner(options.selectionOfferKeyFile);
      NativeProviderOfferV3Config config;
      config.provider = options.providerName;
      config.service = options.serviceName;
      config.bootEpoch = providerBootId;
      config.signerKeyId = std::move(signer.keyId);
      config.acceptedRoles = allowedRoles;
      config.backends = {options.offerBackend};
      config.devices = options.offerDevices;
      config.canProvision = options.offerCanProvision;
      config.hasModel = options.offerHasModel;
      config.signDigest = std::move(signer.sign);
      nativeOfferConfig = std::move(config);
      std::cout << "NDNSF_DI_NATIVE_PROVIDER_V3_OFFER_SIGNER_READY"
                << " provider=" << options.providerName
                << " keyId=" << nativeOfferConfig->signerKeyId
                << " backend=" << options.offerBackend
                << " devices=" << joinRoles(options.offerDevices)
                << std::endl;
    }
    if (options.serve) {
      if (plan.executionPolicy != "DATA_DRIVEN_V2") {
        throw std::invalid_argument(
          "serving requires DATA_DRIVEN_V2 post-Selection assembly");
      }
      if (!options.artifactReferencesPath.empty()) {
        throw std::invalid_argument(
          "serving rejects preassembled --artifact-references; use canonical "
          "artifact assignment");
      }
      for (const auto& item : specs) {
        if (!item.second.path.empty()) {
          throw std::invalid_argument(
            "serving rejects ready-made role artifact for " + item.first);
        }
      }
    }

    auto factory = std::make_shared<RegistryNativeModelRunnerFactory>();
    registerOnnxRuntimeBackend(*factory);
    factory->registerBackend(
      "native-yolo-postprocess",
      [] (const NativeModelRunnerSpec& spec) {
        return makeNativeYoloMergeRunner(spec);
      });
    if (options.wiringCheckOnly || options.tracerDeterministicRunner) {
      factory->registerBackend(
        "onnxruntime",
        [tracerDeterministicRunner = options.tracerDeterministicRunner]
        (const NativeModelRunnerSpec& spec) {
          if (tracerDeterministicRunner) {
            auto runner = makeTracerDeterministicRunner(spec);
            auto evidence = executionEvidenceFromRunnerSpec(
              spec, RunnerKind::SyntheticDelay, "deterministic-runner-v1", "synthetic");
            return makeNativeModelRunner(
              [runner] (const RoleExecutionContext& ctx) { return runner->run(ctx); },
              std::move(evidence));
          }
          auto evidence = executionEvidenceFromRunnerSpec(
            spec, RunnerKind::WiringOnly, "wiring-only-v1", "wiring");
          return makeNativeModelRunner(
            [] (const RoleExecutionContext&) {
              return std::map<std::string, TensorBundle>{};
            }, std::move(evidence));
        });
    }
    std::cout << "NDNSF_DI_NATIVE_PROVIDER_BACKENDS_READY onnxruntime=1"
              << " wiringCheckOnly=" << (options.wiringCheckOnly ? 1 : 0)
              << " tracerDeterministicRunner="
              << (options.tracerDeterministicRunner ? 1 : 0)
              << std::endl;

    if (options.serve) {
      std::cout << "NDNSF_DI_NATIVE_PROVIDER_FACE_CREATING" << std::endl;
      ndn::Face face;
      ndn::KeyChain keyChain;
      std::cout << "NDNSF_DI_NATIVE_PROVIDER_FACE_READY" << std::endl;

      const ndn::Name providerIdentity(options.providerName);
      const ndn::Name controllerIdentity(options.controllerName);
      auto providerCert = getOrCreateIdentity(keyChain, providerIdentity);
      auto controllerCert = loadControllerCertificate(controllerIdentity, keyChain);
      if (!options.bootstrapToken.empty()) {
        providerCert = ndn_service_framework::ensureControllerSignedCertificate(
          face, keyChain, controllerIdentity, providerIdentity,
          providerIdentity, options.bootstrapToken);
        std::cout << "NDNSF_DI_NATIVE_PROVIDER_BOOTSTRAP_CERT_READY"
                  << " provider=" << options.providerName
                  << " certificate=" << providerCert.getName()
                  << std::endl;
      }
      keyChain.setDefaultIdentity(keyChain.getPib().getIdentity(providerIdentity));
      std::cout << "NDNSF_DI_NATIVE_PROVIDER_KEYCHAIN_READY providerCert="
                << providerCert.getName()
                << " controllerCert=" << controllerCert.getName()
                << std::endl;

      std::unique_ptr<ndn_service_framework::CertificatePublisher> certPublisher;
      if (!options.noServeCertificates) {
        std::cout << "NDNSF_DI_NATIVE_PROVIDER_CERT_PUBLISHER_CREATING"
                  << std::endl;
        certPublisher = std::make_unique<ndn_service_framework::CertificatePublisher>(
          face,
          keyChain,
          providerCert.getName());
        std::cout << "NDNSF_DI_NATIVE_PROVIDER_CERT_PUBLISHER_READY prefix="
                  << certPublisher->getRegisteredPrefix()
                  << std::endl;
      }

      std::cout << "NDNSF_DI_NATIVE_PROVIDER_SERVICE_PROVIDER_CREATING"
                << std::endl;
      auto provider = std::make_shared<ndn_service_framework::ServiceProvider>(
        face,
        ndn::Name(options.groupName),
        providerCert,
        controllerCert,
        options.trustSchema);
      // The framework owns the Provider boot epoch carried by the ACK's
      // encrypted key offer.  Bind every DI offer, assignment, and evidence
      // record to that same epoch instead of inventing a second executable-
      // local identifier that the User must reject.
      providerBootId = provider->getProviderBootEpoch();
      specs = withExecutionEvidenceContext(
        std::move(specs), options, providerBootId, providerStartedAtMs);
      if (nativeOfferConfig) {
        nativeOfferConfig->bootEpoch = providerBootId;
      }
      std::cout << "NDNSF_DI_NATIVE_PROVIDER_SERVICE_PROVIDER_READY"
                << std::endl;
      provider->setUseTokens(!options.disableTokens);
      provider->setHandlerThreads(options.handlerThreads);
      provider->setAckThreads(options.ackThreads);
      std::cout << "NDNSF_DI_NATIVE_PROVIDER_THREADS_READY handlerThreads="
                << options.handlerThreads
                << " ackThreads=" << options.ackThreads
                << std::endl;

      auto provisioningState = std::make_shared<NativeProviderReadinessState>();
      auto capacitySnapshot = std::make_shared<
        NativeProviderReadinessState::CapacitySnapshotProvider>();
      auto capacitySnapshotMutex = std::make_shared<std::mutex>();
      ProviderResourceProbeConfig resourceConfig;
      resourceConfig.providerName = options.providerName;
      resourceConfig.providerBootId = providerBootId;
      auto resourceProbe = std::make_shared<LinuxProviderResourceProbe>(resourceConfig);
      auto telemetryCollector = std::make_shared<NativeProviderTelemetryCollector>(
        resourceProbe,
        [capacitySnapshot, capacitySnapshotMutex] {
          std::lock_guard<std::mutex> lock(*capacitySnapshotMutex);
          return *capacitySnapshot ? (*capacitySnapshot)() : ProviderRoleWorkerSnapshot{};
        });
      auto stageServiceTimeObserver = std::make_shared<
        std::function<void(std::chrono::milliseconds)>>();
      auto executionEvidenceObserver = std::make_shared<
        std::function<void(const ExecutionEvidence&)>>();
      telemetryCollector->start();
      provisioningState->setTelemetrySnapshotProvider(
        [telemetryCollector] { return telemetryCollector->snapshot(); });
      if (options.enableAdmissionLease) {
        provider->setGenericAdmissionLeaseRequired(ndn::Name(options.serviceName), true);
        std::cout << "NDNSF_DI_NATIVE_PROVIDER_ADMISSION_LEASE_REQUIRED"
                  << " service=" << options.serviceName
                  << " ttlMs=" << options.admissionLeaseTtlMs
                  << std::endl;
      }

      // spec182 CD-014: the shared NativeInferenceProvider host owns the
      // single fixed execution-lease entry, the host-wide shared lease
      // table, and one routed ExecutionLeaseService per served target.  The
      // collaboration registration below is installed by host->serve inside
      // the installTask; the main thread waits for that registration before
      // starting the event loop, so serve always lands on the Face event
      // thread or before the event loop starts (the Core scoped-registration
      // constraint), and the executable keeps exactly one registration path.
      auto providerHost = std::make_shared<ndnsf::di::NativeInferenceProvider>(
        provider, std::make_shared<ndnsf::di::NativeAdapterRegistry>());
      ndnsf::di::NativeServiceRegistration nativeRegistration;
      ndnsf::di::NativeServiceDefinition nativeService;
      nativeService.serviceName = options.serviceName;
      nativeService.allowedRoles = allowedRoles;
      nativeService.ackHandler =
        [rolesText = joinRoles(allowedRoles),
         allowedRoles,
         provisioningState,
         provider,
         serviceName = ndn::Name(options.serviceName),
         providerName = ndn::Name(options.providerName),
         nativeOfferConfig,
         enableAdmissionLease = options.enableAdmissionLease,
         admissionLeaseTtlMs = options.admissionLeaseTtlMs](
          const ndn_service_framework::RequestMessage& request) {
          auto decision = provisioningState->makeAckDecision(rolesText,
                                                             providerName,
                                                             serviceName);
          bool issuedV3Offer = false;
          if (decision.status && nativeOfferConfig) {
            const auto requestPayload = request.getPayload();
            const auto offer = issueNativeProviderOfferV3(
              std::vector<std::uint8_t>(requestPayload.begin(), requestPayload.end()),
              *nativeOfferConfig,
              static_cast<std::uint64_t>(std::max<long long>(0, epochMs())));
            if (offer) {
              issuedV3Offer = true;
              decision.status = offer->status;
              decision.message = offer->message;
              decision.payload = textBuffer(offer->payload);
              decision.pendingStateTtlMs = offer->pendingStateTtlMs;
            }
          }
          // Update NDNSD meta with live capacity from this ACK decision
          if (decision.status) {
            auto payloadText = bufferText(decision.payload);
            provider->updateNdnsdMeta("roles", rolesText);
            provider->updateNdnsdMeta("runtimeStatus", "ready");
            // Parse semicolon-delimited key=value fields for capacity
            std::string current;
            for (char ch : payloadText) {
              if (ch == ';') {
                auto eq = current.find('=');
                if (eq != std::string::npos && eq > 0 && eq + 1 < current.size()) {
                  provider->updateNdnsdMeta(current.substr(0, eq), current.substr(eq + 1));
                }
                current.clear();
              } else {
                current.push_back(ch);
              }
            }
          }
          if (enableAdmissionLease && decision.status && !issuedV3Offer) {
            ndn_service_framework::ServiceProvider::GenericAdmissionLease lease;
            lease.leaseId = nativeTracerLeaseId(providerName.toUri());
            lease.providerName = providerName;
            lease.serviceName = serviceName;
            lease.expiresAtMs = static_cast<std::uint64_t>(
              std::max<long long>(0, epochMs() + admissionLeaseTtlMs));
            const auto proof = nativeTracerLeaseProof(allowedRoles);
            if (!proof.empty()) {
              lease.resourceBindingProof = textBuffer(proof);
            }
            provider->grantGenericAdmissionLease(lease);
            std::string payload = bufferText(decision.payload);
            if (!payload.empty() && payload.back() != ';') {
              payload.push_back(';');
            }
            payload += "leaseId=" + lease.leaseId + ";";
            payload += "leaseProvider=" + lease.providerName.toUri() + ";";
            payload += "leaseService=" + lease.serviceName.toUri() + ";";
            payload += "leaseExpiresAtMs=" + std::to_string(lease.expiresAtMs) + ";";
            if (!proof.empty()) {
              payload += "resourceBindingProof=" + proof + ";";
            }
            decision.payload = textBuffer(payload);
            std::cout << "NDNSF_DI_NATIVE_PROVIDER_ADMISSION_LEASE_GRANTED"
                      << " provider=" << providerName
                      << " service=" << serviceName
                      << " leaseId=" << lease.leaseId
                      << " proof=" << (proof.empty() ? "-" : proof)
                      << std::endl;
          }
          std::cout << "NDNSF_DI_NATIVE_PROVIDER_ACK_DECISION"
                    << " provider=" << providerName
                    << " roles=" << rolesText
                    << " status=" << (decision.status ? 1 : 0)
                    << " message=\"" << decision.message << "\""
                    << " payload=\"" << bufferText(decision.payload) << "\""
                    << std::endl;
          return decision;
        };
      // Observation seam invoked by host->serve once the native runtime is
      // assembled, before its collaboration registration is installed: bind
      // the runtime capacity/evidence snapshot to readiness and telemetry,
      // exactly the wiring the old readyHandler forwarder owned.
      nativeService.runtimeObserver =
        [provisioningState,
         allowedRoles,
         capacitySnapshot,
         capacitySnapshotMutex,
         telemetryCollector,
         stageServiceTimeObserver,
         executionEvidenceObserver](
          const ndnsf::di::NativeProviderCollaborationRuntime& runtime) {
          {
            std::lock_guard<std::mutex> lock(*capacitySnapshotMutex);
            *capacitySnapshot = runtime.capacitySnapshot;
          }
          *stageServiceTimeObserver = [telemetryCollector](
            std::chrono::milliseconds duration) {
            telemetryCollector->recordStageServiceTime(duration);
          };
          telemetryCollector->refresh();
          auto executionEvidenceByRole = std::make_shared<
            std::map<std::string, ExecutionEvidence>>();
          for (const auto& item : runtime.executionEvidence) {
            for (const auto& role : item.roles) {
              (*executionEvidenceByRole)[role] = item;
            }
          }
          if (!runtime.executionEvidence.empty()) {
            auto executionEvidence = aggregateExecutionEvidence(
              runtime.executionEvidence);
            provisioningState->setExecutionEvidence(executionEvidence);
            std::cout << "NDNSF_DI_EXECUTION_EVIDENCE "
                      << executionEvidenceToJson(executionEvidence)
                      << std::endl;
          }
          else {
            // Canonical role assembly is deliberately deferred until an
            // authenticated Selection.  Readiness is capability-only here;
            // per-role execution evidence is published after ORT loads.
            std::cout << "NDNSF_DI_EXECUTION_EVIDENCE_DEFERRED"
                      << " reason=post-selection-assembly"
                      << " roles=" << allowedRoles.size()
                      << std::endl;
          }
          provisioningState->setExecutionEvidenceByRole(*executionEvidenceByRole);
          auto executionEvidenceMutex = std::make_shared<std::mutex>();
          *executionEvidenceObserver =
            [executionEvidenceByRole, executionEvidenceMutex, provisioningState]
            (const ExecutionEvidence& observed) {
              std::lock_guard<std::mutex> lock(*executionEvidenceMutex);
              for (const auto& role : observed.roles) {
                (*executionEvidenceByRole)[role] = observed;
              }
              std::vector<ExecutionEvidence> current;
              current.reserve(executionEvidenceByRole->size());
              for (const auto& item : *executionEvidenceByRole) {
                current.push_back(item.second);
              }
              const auto aggregate = aggregateExecutionEvidence(current);
              provisioningState->setExecutionEvidence(aggregate);
              provisioningState->setExecutionEvidenceByRole(*executionEvidenceByRole);
              std::cout << "NDNSF_DI_EXECUTION_EVIDENCE_UPDATE "
                        << executionEvidenceToJson(aggregate)
                        << std::endl;
              std::cout << "NDNSF_DI_EXECUTION_EVIDENCE_OBSERVED "
                        << executionEvidenceToJson(observed)
                        << std::endl;
            };
        };

      // The installTask assembles the runtime and registers the service
      // through host->serve before the event loop starts (the main thread
      // waits on serveCompleted below), then polls the Controller permission
      // and marks readiness once the event loop is running.
      auto serveCompleted = std::make_shared<std::atomic<bool>>(false);
      auto provisionFailed = std::make_shared<std::atomic<bool>>(false);
      auto runLimitReached = std::make_shared<std::atomic<bool>>(false);
      auto provisioningDone = std::make_shared<std::atomic<bool>>(false);
      auto serveCompletedMutex = std::make_shared<std::mutex>();
      auto serveCompletedCv = std::make_shared<std::condition_variable>();
      auto provisioningDoneMutex = std::make_shared<std::mutex>();
      auto provisioningDoneCv = std::make_shared<std::condition_variable>();
      auto signalServeCompleted = [serveCompleted, serveCompletedMutex,
                                   serveCompletedCv] {
        {
          std::lock_guard<std::mutex> lock(*serveCompletedMutex);
          *serveCompleted = true;
        }
        serveCompletedCv->notify_all();
      };
      auto signalProvisioningDone = [provisioningDone, provisioningDoneMutex,
                                     provisioningDoneCv] {
        {
          std::lock_guard<std::mutex> lock(*provisioningDoneMutex);
          provisioningDone->store(true, std::memory_order_release);
        }
        provisioningDoneCv->notify_all();
      };
      auto installTask =
        [options,
         plan,
         specs,
         allowedRoles,
         providerBootId,
         providerStartedAtMs,
         factory,
         providerCert,
         controllerCert,
         controllerIdentity,
         provisioningState,
         stageServiceTimeObserver,
         executionEvidenceObserver,
         provider,
         providerHost,
         nativeService,
         registrationOut = &nativeRegistration,
         provisionFailed,
         runLimitReached,
         signalServeCompleted,
         signalProvisioningDone,
         &face,
         &keyChain] () mutable {
          try {
            provisioningState->markInstalling(
              "waiting for authenticated post-Selection role assembly");
            // Tell other users via negative-ACK what's happening
            provisioningState->setProvisioningContext(
              options.providerName,      // deploymentId placeholder
              joinRoles(allowedRoles),   // which roles
              30000);                    // estimated 30s to ready
            std::cout << "NDNSF_DI_NATIVE_PROVIDER_PROVISION_INSTALLING"
                      << " artifactReferences=" << options.artifactReferencesPath
                      << " cacheDir=" << options.artifactCacheDir
                      << std::endl;

            // Formal serving never reads a ready-made role file at startup.
            // The metadata-only specs are used only for the ordered role set;
            // the exact model path is produced after Selection by the factory.
            auto metadataSpecs = withExecutionEvidenceContext(
              specs, options, providerBootId, providerStartedAtMs);
            auto materializedSpecs = metadataSpecs;
            auto runners = orderedSpecs(plan, materializedSpecs, allowedRoles);
            std::cout << "NDNSF_DI_NATIVE_PROVIDER_PLAN_READY roles="
                      << plan.roles.size()
                      << " artifacts=" << materializedSpecs.size()
                      << " activeRoles=" << allowedRoles.size()
                      << " runners=" << runners.size()
                      << std::endl;

            NativeProviderHandlerConfig config;
            config.plan = plan;
            config.assignment = defaultAssignment(plan, options.providerName, allowedRoles);
            config.runnerFactory = factory;
            config.runnerSpecs = std::move(runners);
            config.localProviderName = options.providerName;
            config.providerBootId = providerBootId;
            installNativeProtectedGrantFactory(config);
            config.planDigest = sha256File(options.planPath);
            if (const auto* mutation = std::getenv("SPEC180_YN_MUTATION")) {
              config.spec180YnMutation = mutation;
            }
            config.executionPolicy = plan.executionPolicy;
            // The executable option is the deployment-owned budget for
            // authenticated inter-Provider dependency reads. It must not
            // extend the independent readiness/control deadlines.
            config.dependencyFetchTimeoutMs = options.repoFetchTimeoutMs;
            std::cout << "NDNSF_DI_DEPENDENCY_FETCH_TIMEOUT_MS "
                      << config.dependencyFetchTimeoutMs << std::endl;
            // Serving prepares a runner only after authenticated Selection.
            // Model adapters supply a spec; observations are bound once below.
            config.allowPreassembledV3Compatibility = false;
            config.requireGenerationTextOutput = true;
            if (!options.tokenizerJson.empty()) {
              const auto tokenizerPath = options.tokenizerJson;
              config.generationDecodersFactory =
                [tokenizerPath](const std::string& tokenizerDigest) {
                  NativeStandaloneTokenizerOptions tokenizerOptions;
                  tokenizerOptions.tokenizerPath = tokenizerPath;
                  return makeNativeStandaloneTokenizerDecoders(
                    std::move(tokenizerOptions), tokenizerDigest);
                };
            }
            const auto assemblyCacheDir = options.artifactCacheDir;
            const auto assemblyProviderIdentity = options.providerName;
            // Pinned OA02 worker (path + content hash): every post-Selection
            // assembly spawns this binary and re-probes its hash first.
            const auto assemblyWorkerLocation = resolveWorkerLocation();
            config.runnerPreparationFactory =
              [assemblyCacheDir,
               assemblyProviderIdentity,
               assemblyWorkerLocation,
               providerCert,
               providerBootId,
               providerStartedAtMs,
               &keyChain] (
                ndn_service_framework::ServiceProvider::CollaborationContext& ctx,
                const NativeSelectionProjectionV3& projection,
                const std::shared_ptr<ProtectedRuntime>& protectedRuntime) {
                NativeModelRunnerSpec spec;
                if (projection.assembly.mergeKind == "NATIVE_POSTPROCESS") {
                  spec = nativeYoloMergeRunnerSpecFromProjection(projection);
                }
                else {
                  NativeCanonicalOnnxAssemblerOptions assemblyOptions;
                  assemblyOptions.cacheDir = assemblyCacheDir;
                  assemblyOptions.providerIdentity = assemblyProviderIdentity;
                  assemblyOptions.protectedRuntime = protectedRuntime;
                  assemblyOptions.workerLocation = assemblyWorkerLocation;
                  assemblyOptions.reportProgress = makeNativeAssemblyProgressReporter(
                    ctx, projection, projection.assembly.backend.empty()
                      ? std::string("native") : projection.assembly.backend);
                  if (protectedRuntime) {
                    const auto& payload = ctx.assignment().assignmentPayload;
                    assemblyOptions.roleAssemblySpecDigest = nativeAssemblyDigestFromCanonicalProjection(
                      std::string(reinterpret_cast<const char*>(payload.data()), payload.size()));
                  }
                  assemblyOptions.signManifest =
                    [&keyChain, providerCert](const std::string& manifestBytes) {
                      return signNativeAssemblyManifest(
                        keyChain, providerCert, manifestBytes);
                    };
                  spec = prepareNativeCanonicalOnnxRole(ctx, projection, assemblyOptions);
                }
                bindNativeRunnerPreparationContext(spec, projection,
                  {assemblyProviderIdentity, providerBootId, providerStartedAtMs, assemblyCacheDir});
                return spec;
              };
            config.requireExecutionAttemptBinding = options.requireExecutionLease;
            // Execution leases bind the attempt and resources. They are not a
            // global ReadySet barrier: DATA_DRIVEN_V2 roles start after local
            // preparation and authenticated direct-predecessor data.
            config.requireExecutionActivation =
              plan.executionPolicy == "LEGACY_READY_SET_V1";
            config.allowLegacyPeerReadinessBarrier =
              plan.executionPolicy == "LEGACY_READY_SET_V1";
            config.workerCount = options.workers;
            config.kvStateStore = std::make_shared<KvStateStore>(
              64ULL * 1024ULL * 1024ULL, 128);
            config.kvStateStore->setProviderBootId(providerBootId);
            config.stageServiceTimeObserver = stageServiceTimeObserver;
            config.executionEvidenceObserver = executionEvidenceObserver;
            const bool requiresGroupCapability = std::any_of(
              plan.dependencies.begin(), plan.dependencies.end(),
              [] (const NativeDependencySpec& dependency) {
                return dependency.useNdnsfDataV1;
              });
            config.groupCoordinatorFactory =
              [requiresGroupCapability,
               localProvider = options.providerName,
               providerCertName = providerCert.getName(),
               expectedPlanDigest = config.planDigest,
               &keyChain] (
                  ndn_service_framework::ServiceProvider::CollaborationContext& ctx,
                  const std::map<std::string, std::string>& fields) {
                const auto field = fields.find("groupCapabilityV1");
                if (field == fields.end()) {
                  if (requiresGroupCapability) {
                    throw std::runtime_error(
                      "NDNSF_DATA_V1 assignment is missing groupCapabilityV1");
                  }
                  return std::shared_ptr<ProviderGroupCoordinator>{};
                }
                const auto decodedWire =
                  ndn_service_framework::selectionGatedUnhex(field->second);
                auto capability = ProviderGroupCoordinator::decodeCapability(
                  ProviderGroupBytes(decodedWire.begin(), decodedWire.end()));
                const auto requestPlan = fields.find("executionPlanDigest");
                const auto authenticatedPlanDigest =
                  requestPlan == fields.end() ? expectedPlanDigest : requestPlan->second;
                if (capability.requestId != ctx.sessionId() ||
                    capability.planDigest != authenticatedPlanDigest) {
                  throw std::runtime_error(
                    "NDNSF_DATA_V1 capability request/plan binding mismatch");
                }
                const auto localMember = std::find_if(
                  capability.orderedMembers.begin(),
                  capability.orderedMembers.end(),
                  [&localProvider] (const GroupMemberV1& member) {
                    return member.provider == localProvider;
                  });
                if (localMember == capability.orderedMembers.end() ||
                    localMember->endpointPrefix.empty()) {
                  throw std::runtime_error(
                    "NDNSF_DATA_V1 capability omits the local Provider endpoint");
                }
                ProviderGroupCoordinatorOptions groupOptions;
                groupOptions.localProvider = localProvider;
                groupOptions.unwrapEpochKey =
                  [&keyChain, providerCertName, localProvider] (
                      const std::string& providerName,
                      const ProviderGroupBytes& wrapped) {
                    if (providerName != localProvider) {
                      throw std::runtime_error(
                        "NDNSF_DATA_V1 wrapped key targets another Provider");
                    }
                    const auto plaintext =
                      ndn_service_framework::unwrapSelectionGatedInputKey(
                        ndn::Buffer(wrapped.data(), wrapped.size()),
                        providerCertName,
                        keyChain);
                    return ProviderGroupBytes(plaintext.begin(), plaintext.end());
                  };
                auto coordinator = std::make_shared<ProviderGroupCoordinator>(
                  std::move(groupOptions));
                // Empty plaintext key forces RSA unwrap of exactly the local
                // Provider's wrapped epoch key.  The default inner
                // authenticator is request-scoped HMAC-SHA256.
                coordinator->installCapability(std::move(capability), {}, true);
                return coordinator;
              };
            // spec182 CD-014: the host injects its shared lease table; the
            // executable only declares that the service wants one.
            if (options.requireExecutionLease) {
              config.executionLeaseTargetService = options.serviceName;
            }
            config.executionLeaseHardDeadlineMs = static_cast<uint64_t>(
              std::max(1000, options.admissionLeaseTtlMs));

            // serve assembles the runtime -- invoking nativeService.runtimeObserver
            // for the capacity/evidence/telemetry wiring -- and installs the
            // scoped collaboration registration on this thread, before the
            // event loop starts, so the Core scoped-registration thread
            // constraint holds and the main thread owns the registration.
            *registrationOut = providerHost->serve(nativeService, config);
            signalServeCompleted();
            std::cout << "NDNSF_DI_EXECUTION_LEASE_SERVICE_READY"
                      << " provider=" << options.providerName
                      << " service=" << options.serviceName
                      << std::endl;
            const auto permissionDeadline =
              std::chrono::steady_clock::now() +
              std::chrono::milliseconds(options.permissionWaitMs);
            while (!provider->hasProviderPermissionForService(
                     ndn::Name(options.serviceName))) {
              if (runLimitReached->load(std::memory_order_acquire)) {
                std::cout << "NDNSF_DI_NATIVE_PROVIDER_PERMISSION_WAIT_CANCELLED"
                          << " reason=run-limit" << std::endl;
                signalProvisioningDone();
                return;
              }
              if (std::chrono::steady_clock::now() >= permissionDeadline) {
                throw std::runtime_error(
                  "provider permission not installed for " + options.serviceName);
              }
              std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            std::cout << "NDNSF_DI_NATIVE_PROVIDER_PERMISSION_READY"
                      << " provider=" << options.providerName
                      << " service=" << options.serviceName
                      << " policyEpoch=" << provider->getCurrentPolicyEpoch()
                      << std::endl;
            provider->updateNdnsdMeta("providerBootId", providerBootId);
            std::cout << "NDNSF_DI_PROVIDER_BOOT_READY"
                      << " provider=" << options.providerName
                      << " providerBootId=" << providerBootId
                      << " attemptAuthority=fresh"
                      << " kvState=fresh"
                      << std::endl;
            provisioningState->markReady(
              "native runtime ready; role assembly deferred until Selection");
            provider->updateNdnsdMeta("runtimeStatus", "ready");
            std::cout << "NDNSF_DI_NATIVE_PROVIDER_PROVISION_READY"
                      << " activeRoles=" << allowedRoles.size()
                      << " workers=" << options.workers
                      << std::endl;
            std::cout << "NDNSF_DI_NATIVE_PROVIDER_READY"
                      << " provider=" << options.providerName
                      << " activeRoles=" << allowedRoles.size()
                      << std::endl;
            signalProvisioningDone();
          }
          catch (const std::exception& exc) {
            provisioningState->markFailed(exc.what());
            provisionFailed->store(true, std::memory_order_release);
            std::cerr << "NDNSF_DI_NATIVE_PROVIDER_PROVISION_FAILED"
                      << " error=\"" << exc.what() << "\""
                      << std::endl;
            // A producer Face may keep its io_context alive indefinitely. Stop
            // it directly so processEvents() returns even when another
            // ServiceProvider scheduler has outstanding retry work. The main
            // thread performs scheduler cancellation after the event loop has
            // stopped, where the scheduler is no longer accessed concurrently.
            face.getIoContext().stop();
            // The main thread waits for serve below even when assembly
            // failed; otherwise it would never enter the event loop.
            signalServeCompleted();
            signalProvisioningDone();
          }
        };

      provider->fetchPermissionsFromController(controllerIdentity);
      std::cout << "NDNSF_DI_NATIVE_PROVIDER_PERMISSION_FETCH_ISSUED controller="
                << controllerIdentity
                << std::endl;
      provider->init();
      std::cout << "NDNSF_DI_NATIVE_PROVIDER_INIT_DONE" << std::endl;
      provider->setNdnsdMeta({{"runtimeStatus", "installing"}});
      provider->startNdnsdPeriodicPublish(10);
      // Keep the installation task joinable.  It captures the Face and
      // KeyChain by reference while waiting for permission, so detaching it
      // would let the main stack unwind before the callback has drained.
      std::thread installThread(std::move(installTask));

      // InstallTask runs serve() off the main thread; wait for the host
      // registration to land (or the assembly failure to be reported) before
      // processing events, so serve never races the Core Face dispatch.
      {
        std::unique_lock<std::mutex> lock(*serveCompletedMutex);
        serveCompletedCv->wait(lock,
                               [&serveCompleted] { return serveCompleted->load(); });
      }
      if (provisionFailed->load(std::memory_order_acquire)) {
        provider->stopNdnsdPeriodicPublish();
        face.shutdown();
        if (installThread.joinable())
          installThread.join();
        std::unique_lock<std::mutex> lock(*provisioningDoneMutex);
        provisioningDoneCv->wait(lock,
                                  [&provisioningDone] {
                                    return provisioningDone->load(
                                      std::memory_order_acquire);
                                  });
        return 2;
      }
      std::cout << "NDNSF_DI_NATIVE_PROVIDER_SERVE_READY service="
                << options.serviceName
                << " identity=" << options.providerName
                << " roles=" << joinRoles(allowedRoles)
                << " workers=" << options.workers
                << " handlerThreads=" << options.handlerThreads
                << " ackThreads=" << options.ackThreads
                << " runtimeStatus=installing"
                << std::endl;
      const auto serveStartedAt = std::chrono::steady_clock::now();
      while (!provisionFailed->load(std::memory_order_acquire) &&
             g_shutdownRequested == 0) {
        if (options.runForMs &&
            std::chrono::steady_clock::now() >=
              serveStartedAt + std::chrono::milliseconds(*options.runForMs)) {
          std::cout << "NDNSF_DI_NATIVE_PROVIDER_RUN_LIMIT_REACHED"
                    << " runForMs=" << *options.runForMs << std::endl;
          runLimitReached->store(true, std::memory_order_release);
          break;
        }
        try {
          // Keep the event loop responsive to an asynchronous provisioning
          // failure. An unbounded processEvents() would leave a failed Provider
          // looking alive until another network event arrived.
          face.processEvents(ndn::time::milliseconds(100));
        }
        catch (const std::exception& exc) {
          std::cerr << "NDNSF_DI_NATIVE_PROVIDER_EVENT_LOOP_EXCEPTION"
                    << " provider=" << options.providerName
                    << " service=" << options.serviceName
                    << " error=\"" << exc.what() << "\""
                    << std::endl;
        }
      }
      if (g_shutdownRequested != 0) {
        std::cout << "NDNSF_DI_NATIVE_PROVIDER_SHUTDOWN_REQUESTED" << std::endl;
      }
      provider->stopNdnsdPeriodicPublish();
      face.shutdown();
      if (installThread.joinable())
        installThread.join();
      {
        std::unique_lock<std::mutex> lock(*provisioningDoneMutex);
        provisioningDoneCv->wait(lock,
                                  [&provisioningDone] {
                                    return provisioningDone->load(
                                      std::memory_order_acquire);
                                  });
      }
      return provisionFailed->load(std::memory_order_acquire) ? 2 : 0;
    }

    specs = withExecutionEvidenceContext(
      materializeManifestSpecs(options, specs), options, providerBootId, providerStartedAtMs);
    auto runners = orderedSpecs(plan, specs, allowedRoles);
    std::cout << "NDNSF_DI_NATIVE_PROVIDER_PLAN_READY roles="
              << plan.roles.size()
              << " artifacts=" << specs.size()
              << " activeRoles=" << allowedRoles.size()
              << " runners=" << runners.size()
              << std::endl;
    auto io = std::make_shared<PlaceholderDependencyIo>();
    NativeProviderSession session(plan,
                                  defaultAssignment(plan, options.providerName, allowedRoles),
                                  io,
                                  factory,
                                  options.workers);

    std::size_t registered = 0;
    std::vector<ExecutionEvidence> checkEvidence;
    for (const auto& spec : runners) {
      auto observedRunner = factory->create(spec);
      if (!observedRunner->executionEvidence()) {
        throw std::runtime_error("check-only runner emitted no execution evidence: " + spec.role);
      }
      checkEvidence.push_back(*observedRunner->executionEvidence());
      session.registerRunner(spec);
      ++registered;
    }
    const auto aggregateEvidence = aggregateExecutionEvidence(checkEvidence);
    std::cout << "NDNSF_DI_EXECUTION_EVIDENCE "
              << executionEvidenceToJson(aggregateEvidence)
              << std::endl;

    std::cout << "NDNSF_DI_NATIVE_PROVIDER_CHECK_OK service="
              << options.serviceName
              << " roles=" << plan.roles.size()
              << " artifacts=" << specs.size()
              << " registered=" << registered
              << " workers=" << options.workers
              << std::endl;
    return 0;
  }
  catch (const std::exception& exc) {
    printUsage(argv[0]);
    std::cerr << "error: " << exc.what() << "\n";
    return 2;
  }
}
