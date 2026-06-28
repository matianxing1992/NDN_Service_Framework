#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeArtifactMaterializer.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderReadiness.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderSession.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeServiceManifest.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/OnnxRuntimeModelRunner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"

#include "ndn-service-framework/CertificatePublisher.hpp"
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

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <chrono>
#include <cstring>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace {

using namespace ndnsf::di;

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
  std::string roles = "all";
  std::string artifactReferencesPath;
  std::string artifactCacheDir = "/tmp/ndnsf-di-native-artifacts";
  std::string repoServiceName = "/NDNSF/DistributedRepo";
  int repoFetchTimeoutMs = 30000;
  int repoAckTimeoutMs = 500;
  int repoPermissionWaitMs = 3000;
  std::size_t workers = 1;
  std::size_t handlerThreads = 4;
  std::size_t ackThreads = 2;
  bool checkOnly = false;
  bool serve = false;
  bool noServeCertificates = false;
  bool disableTokens = false;
  bool wiringCheckOnly = false;
  bool tracerDeterministicRunner = false;
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

std::shared_ptr<NativeModelRunner>
makeTracerDeterministicRunner(const NativeModelRunnerSpec& spec)
{
  return makeNativeModelRunner(
    [spec] (const RoleExecutionContext&) {
      auto outputNames = splitNames(spec.metadata.count("output_tensors") ?
                                    spec.metadata.at("output_tensors") : "");
      if (outputNames.empty()) {
        outputNames.push_back("output");
      }
      std::vector<NamedTensor> outputs;
      outputs.reserve(outputNames.size());
      float value = 1.0f;
      for (const auto& name : outputNames) {
        outputs.push_back(makeFloat32Tensor(name, {1, 1}, float32Payload({value})));
        value += 1.0f;
      }
      std::map<std::string, TensorBundle> byScope;
      for (const auto& scope : outputScopesFromMetadata(spec)) {
        byScope.emplace(scope, makeEncodedTensorBundle(scope, outputs));
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
    else if (arg == "--roles") {
      options.roles = readValue();
    }
    else if (arg == "--artifact-references") {
      options.artifactReferencesPath = readValue();
    }
    else if (arg == "--artifact-cache-dir") {
      options.artifactCacheDir = readValue();
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
  if (options.wiringCheckOnly && !options.checkOnly) {
    throw std::invalid_argument("--wiring-check-only requires --check-only");
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
defaultAssignment(const NativeExecutionPlan& plan, const std::string& providerName)
{
  NativeProviderAssignment assignment;
  for (const auto& role : plan.roles) {
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
    << "[--roles all|role,...] [--group <prefix>] [--controller <prefix>] "
    << "[--trust-schema <path>] [--artifact-references <json>] "
    << "[--artifact-cache-dir <dir>] [--repo-service <service>] "
    << "[--repo-fetch-timeout-ms <ms>] [--repo-ack-timeout-ms <ms>] "
    << "[--repo-permission-wait-ms <ms>] [--wiring-check-only] "
    << "[--tracer-deterministic-runner]\n";
}

} // namespace

int
main(int argc, char** argv)
{
  try {
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

    auto plan = loadPlan(options);
    auto specs = loadManifestSpecs(options);
    const auto allowedRoles = allowedRolesForOptions(plan, options);

    auto factory = std::make_shared<RegistryNativeModelRunnerFactory>();
    registerOnnxRuntimeBackend(*factory);
    if (options.wiringCheckOnly || options.tracerDeterministicRunner) {
      factory->registerBackend(
        "onnxruntime",
        [tracerDeterministicRunner = options.tracerDeterministicRunner]
        (const NativeModelRunnerSpec& spec) {
          if (tracerDeterministicRunner) {
            return makeTracerDeterministicRunner(spec);
          }
          return makeNativeModelRunner(
            [] (const RoleExecutionContext&) {
              return std::map<std::string, TensorBundle>{};
            });
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
      auto controllerCert = getOrCreateIdentity(keyChain, controllerIdentity);
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
      ndn_service_framework::ServiceProvider provider(face,
                                                      ndn::Name(options.groupName),
                                                      providerCert,
                                                      controllerCert,
                                                      options.trustSchema);
      std::cout << "NDNSF_DI_NATIVE_PROVIDER_SERVICE_PROVIDER_READY"
                << std::endl;
      provider.setUseTokens(!options.disableTokens);
      provider.setHandlerThreads(options.handlerThreads);
      provider.setAckThreads(options.ackThreads);
      std::cout << "NDNSF_DI_NATIVE_PROVIDER_THREADS_READY handlerThreads="
                << options.handlerThreads
                << " ackThreads=" << options.ackThreads
                << std::endl;

      using CollaborationHandler =
        ndn_service_framework::ServiceProvider::CollaborationHandler;
      auto provisioningState = std::make_shared<NativeProviderReadinessState>();
      auto readyHandler = std::make_shared<std::optional<CollaborationHandler>>();
      auto readyHandlerMutex = std::make_shared<std::mutex>();

      provider.addCollaborationHandler(
        ndn::Name(options.serviceName),
        allowedRoles,
        [rolesText = joinRoles(allowedRoles), provisioningState](
          const ndn_service_framework::RequestMessage&) {
          auto decision = provisioningState->makeAckDecision(rolesText);
          std::cout << "NDNSF_DI_NATIVE_PROVIDER_ACK_DECISION"
                    << " roles=" << rolesText
                    << " status=" << (decision.status ? 1 : 0)
                    << " message=\"" << decision.message << "\""
                    << std::endl;
          return decision;
        },
        [provisioningState, readyHandler, readyHandlerMutex](
          ndn_service_framework::ServiceProvider::CollaborationContext& ctx,
          const ndn_service_framework::RequestMessage& request) {
          CollaborationHandler handler;
          {
            std::lock_guard<std::mutex> lock(*readyHandlerMutex);
            if (!readyHandler->has_value()) {
              ctx.fail("native DI provider " + provisioningState->statusText() +
                       ": " + provisioningState->message());
              return;
            }
            handler = **readyHandler;
          }
          handler(ctx, request);
        });

      auto installTask =
        [options,
         plan,
         specs,
         allowedRoles,
         factory,
         providerCert,
         controllerCert,
         controllerIdentity,
         provisioningState,
         readyHandler,
         readyHandlerMutex] () mutable {
          try {
            provisioningState->markInstalling(
              "fetching and materializing native model/runtime artifacts");
            std::cout << "NDNSF_DI_NATIVE_PROVIDER_PROVISION_INSTALLING"
                      << " artifactReferences=" << options.artifactReferencesPath
                      << " cacheDir=" << options.artifactCacheDir
                      << std::endl;

            std::map<std::string, NativeModelRunnerSpec> materializedSpecs;
            if (options.artifactReferencesPath.empty()) {
              materializedSpecs = specs;
            }
            else {
              ndn::Face installFace;
              std::cout << "NDNSF_DI_NATIVE_PROVIDER_REPO_USER_CREATING"
                        << std::endl;
              ndn_service_framework::ServiceUser repoUser(
                installFace,
                ndn::Name(options.groupName),
                providerCert,
                controllerCert,
                options.trustSchema);
              repoUser.setUseTokens(!options.disableTokens);
              repoUser.fetchPermissionsFromController(controllerIdentity);
              std::cout << "NDNSF_DI_NATIVE_PROVIDER_REPO_PERMISSION_FETCH_ISSUED controller="
                        << controllerIdentity
                        << " repoService=" << options.repoServiceName
                        << std::endl;
              if (!waitForUserPermission(repoUser,
                                         installFace,
                                         ndn::Name(options.repoServiceName),
                                         options.repoPermissionWaitMs)) {
                throw std::runtime_error(
                  "native provider repo user permission not installed for " +
                  options.repoServiceName);
              }
              std::cout << "NDNSF_DI_NATIVE_PROVIDER_REPO_PERMISSION_READY service="
                        << options.repoServiceName
                        << std::endl;
              materializedSpecs = materializeManifestSpecs(
                options,
                specs,
                [&repoUser, &installFace,
                 repoService = ndn::Name(options.repoServiceName),
                 ackTimeoutMs = options.repoAckTimeoutMs,
                 timeoutMs = options.repoFetchTimeoutMs]
                (const std::string& objectName, const std::string& repoManifestJson) {
                  std::cout << "NDNSF_DI_NATIVE_PROVIDER_REPO_ARTIFACT_FETCH"
                            << " objectName=" << objectName
                            << " repoService=" << repoService
                            << std::endl;
                  const auto segmentPlan =
                    repoSegmentFetchPlanFromManifestJson(repoManifestJson);
                  if (segmentPlan) {
                    std::cout << "NDNSF_DI_NATIVE_PROVIDER_REPO_SEGMENT_FETCH"
                              << " objectName=" << objectName
                              << " dataName=" << segmentPlan->dataName
                              << " segmentCount=" << segmentPlan->segmentCount
                              << " hints=" << segmentPlan->forwardingHints.size()
                              << std::endl;
                    return fetchSegmentedRepoObjectSync(installFace,
                                                        *segmentPlan,
                                                        timeoutMs);
                  }
                  return fetchRepoObjectSync(repoUser,
                                            installFace,
                                            repoService,
                                            objectName,
                                            ackTimeoutMs,
                                            timeoutMs);
                });
            }

            auto runners = orderedSpecs(plan, materializedSpecs, allowedRoles);
            std::cout << "NDNSF_DI_NATIVE_PROVIDER_PLAN_READY roles="
                      << plan.roles.size()
                      << " artifacts=" << materializedSpecs.size()
                      << " activeRoles=" << allowedRoles.size()
                      << " runners=" << runners.size()
                      << std::endl;

            NativeProviderHandlerConfig config;
            config.plan = plan;
            config.assignment = defaultAssignment(plan, options.providerName);
            config.runnerFactory = factory;
            config.runnerSpecs = std::move(runners);
            config.workerCount = options.workers;

            auto handler = makeNativeProviderCollaborationHandler(std::move(config));
            {
              std::lock_guard<std::mutex> lock(*readyHandlerMutex);
              *readyHandler = std::move(handler);
            }
            provisioningState->markReady("native model/runtime artifacts ready");
            std::cout << "NDNSF_DI_NATIVE_PROVIDER_PROVISION_READY"
                      << " activeRoles=" << allowedRoles.size()
                      << " workers=" << options.workers
                      << std::endl;
          }
          catch (const std::exception& exc) {
            provisioningState->markFailed(exc.what());
            std::cerr << "NDNSF_DI_NATIVE_PROVIDER_PROVISION_FAILED"
                      << " error=\"" << exc.what() << "\""
                      << std::endl;
          }
        };

      provider.fetchPermissionsFromController(controllerIdentity);
      std::cout << "NDNSF_DI_NATIVE_PROVIDER_PERMISSION_FETCH_ISSUED controller="
                << controllerIdentity
                << std::endl;
      provider.init();
      std::cout << "NDNSF_DI_NATIVE_PROVIDER_INIT_DONE" << std::endl;
      std::thread(std::move(installTask)).detach();

      std::cout << "NDNSF_DI_NATIVE_PROVIDER_SERVE_READY service="
                << options.serviceName
                << " identity=" << options.providerName
                << " roles=" << joinRoles(allowedRoles)
                << " workers=" << options.workers
                << " handlerThreads=" << options.handlerThreads
                << " ackThreads=" << options.ackThreads
                << " runtimeStatus=installing"
                << std::endl;
      while (true) {
        try {
          face.processEvents();
        }
        catch (const std::exception& exc) {
          std::cerr << "NDNSF_DI_NATIVE_PROVIDER_EVENT_LOOP_EXCEPTION"
                    << " provider=" << options.providerName
                    << " service=" << options.serviceName
                    << " error=\"" << exc.what() << "\""
                    << std::endl;
        }
      }
    }

    specs = materializeManifestSpecs(options, specs);
    auto runners = orderedSpecs(plan, specs, allowedRoles);
    std::cout << "NDNSF_DI_NATIVE_PROVIDER_PLAN_READY roles="
              << plan.roles.size()
              << " artifacts=" << specs.size()
              << " activeRoles=" << allowedRoles.size()
              << " runners=" << runners.size()
              << std::endl;
    auto io = std::make_shared<PlaceholderDependencyIo>();
    NativeProviderSession session(plan,
                                  defaultAssignment(plan, options.providerName),
                                  io,
                                  factory,
                                  options.workers);

    std::size_t registered = 0;
    for (const auto& spec : runners) {
      session.registerRunner(spec);
      ++registered;
    }

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
