#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderSession.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeServiceManifest.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/OnnxRuntimeModelRunner.hpp"

#include "ndn-service-framework/CertificatePublisher.hpp"
#include "ndn-service-framework/ServiceProvider.hpp"

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
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
  std::size_t workers = 1;
  std::size_t handlerThreads = 4;
  std::size_t ackThreads = 2;
  bool checkOnly = false;
  bool serve = false;
  bool noServeCertificates = false;
  bool disableTokens = false;
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

ndn::Buffer
toBuffer(const std::string& text)
{
  return ndn::Buffer(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
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

std::vector<NativeModelRunnerSpec>
orderedSpecs(const NativeExecutionPlan& plan,
             const std::map<std::string, NativeModelRunnerSpec>& specs)
{
  std::vector<NativeModelRunnerSpec> ordered;
  ordered.reserve(plan.roles.size());
  for (const auto& role : plan.roles) {
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
    << "[--trust-schema <path>]\n";
}

} // namespace

int
main(int argc, char** argv)
{
  try {
    auto options = parseArgs(argc, argv);
    if (options.checkOnly == options.serve) {
      throw std::invalid_argument(
        "exactly one of --check-only or --serve is required");
    }

    auto plan = loadPlan(options);
    auto specs = loadManifestSpecs(options);
    auto runners = orderedSpecs(plan, specs);

    auto factory = std::make_shared<RegistryNativeModelRunnerFactory>();
    registerOnnxRuntimeBackend(*factory);

    if (options.serve) {
      ndn::Face face;
      ndn::KeyChain keyChain;

      const ndn::Name providerIdentity(options.providerName);
      const ndn::Name controllerIdentity(options.controllerName);
      auto providerCert = getOrCreateIdentity(keyChain, providerIdentity);
      auto controllerCert = getOrCreateIdentity(keyChain, controllerIdentity);
      keyChain.setDefaultIdentity(keyChain.getPib().getIdentity(providerIdentity));

      std::unique_ptr<ndn_service_framework::CertificatePublisher> certPublisher;
      if (!options.noServeCertificates) {
        certPublisher = std::make_unique<ndn_service_framework::CertificatePublisher>(
          face,
          keyChain,
          providerCert.getName());
      }

      ndn_service_framework::ServiceProvider provider(face,
                                                      ndn::Name(options.groupName),
                                                      providerCert,
                                                      controllerCert,
                                                      options.trustSchema);
      provider.setUseTokens(!options.disableTokens);
      provider.setHandlerThreads(options.handlerThreads);
      provider.setAckThreads(options.ackThreads);

      NativeProviderHandlerConfig config;
      config.plan = plan;
      config.assignment = defaultAssignment(plan, options.providerName);
      config.runnerFactory = factory;
      config.runnerSpecs = runners;
      config.workerCount = options.workers;

      const auto allowedRoles = allowedRolesForOptions(plan, options);
      provider.addCollaborationHandler(
        ndn::Name(options.serviceName),
        allowedRoles,
        [rolesText = joinRoles(allowedRoles)](
          const ndn_service_framework::RequestMessage&) {
          ndn_service_framework::ServiceProvider::AckDecision decision;
          decision.status = true;
          decision.message = "native DI provider ready";
          decision.payload = toBuffer(
            "roles=" + rolesText +
            ";queue=0;hasModel=1;canProvision=0;backends=onnxruntime;");
          return decision;
        },
        makeNativeProviderCollaborationHandler(std::move(config)));

      provider.fetchPermissionsFromController(controllerIdentity);
      provider.init();

      std::cout << "NDNSF_DI_NATIVE_PROVIDER_SERVE_READY service="
                << options.serviceName
                << " identity=" << options.providerName
                << " roles=" << joinRoles(allowedRoles)
                << " workers=" << options.workers
                << " handlerThreads=" << options.handlerThreads
                << " ackThreads=" << options.ackThreads
                << std::endl;
      while (true) {
        face.processEvents();
      }
    }

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
