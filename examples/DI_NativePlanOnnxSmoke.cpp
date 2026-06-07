#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderSession.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeServiceManifest.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/OnnxRuntimeModelRunner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"

#include <cstring>
#include <fstream>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace ndnsf::di;

class InMemoryDependencyIo final : public DependencyIo
{
public:
  std::future<TensorBundle>
  prefetchInput(const std::string& sessionId, const DependencyEdge& edge) final
  {
    auto promise = std::make_shared<std::promise<TensorBundle>>();
    auto future = promise->get_future();
    const auto itemKey = key(sessionId, edge);
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      const auto found = m_available.find(itemKey);
      if (found != m_available.end()) {
        promise->set_value(found->second);
        return future;
      }
      m_waiters[itemKey].push_back(std::move(promise));
    }
    return future;
  }

  void
  publishOutput(const std::string& sessionId,
                const DependencyEdge& edge,
                const TensorBundle& value) final
  {
    std::vector<std::shared_ptr<std::promise<TensorBundle>>> ready;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      const auto itemKey = key(sessionId, edge);
      m_available[itemKey] = value;
      m_publishedNames.push_back(edge.plannedDataName);
      m_publishedScopes.push_back(edge.scope);
      const auto found = m_waiters.find(itemKey);
      if (found != m_waiters.end()) {
        ready = std::move(found->second);
        m_waiters.erase(found);
      }
    }
    for (auto& promise : ready) {
      promise->set_value(value);
    }
  }

  std::size_t
  publishedCount() const
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_publishedNames.size();
  }

private:
  static std::string
  key(const std::string& sessionId, const DependencyEdge& edge)
  {
    return sessionId + "|" + edge.plannedDataName;
  }

private:
  mutable std::mutex m_mutex;
  std::map<std::string, TensorBundle> m_available;
  std::map<std::string, std::vector<std::shared_ptr<std::promise<TensorBundle>>>> m_waiters;
  std::vector<std::string> m_publishedNames;
  std::vector<std::string> m_publishedScopes;
};

std::vector<uint8_t>
floatPayload(std::size_t count)
{
  std::vector<float> values(count);
  for (std::size_t i = 0; i < values.size(); ++i) {
    values[i] = static_cast<float>((i % 255) + 1) / 255.0f;
  }
  std::vector<uint8_t> payload(values.size() * sizeof(float));
  if (!payload.empty()) {
    std::memcpy(payload.data(), values.data(), payload.size());
  }
  return payload;
}

NativeExecutionPlan
loadPlan(const std::string& path, const std::string& serviceName)
{
  std::ifstream input(path);
  if (!input.good()) {
    throw std::runtime_error("cannot open native execution plan: " + path);
  }
  return nativeExecutionPlanForServiceFromJson(input, serviceName);
}

std::map<std::string, NativeModelRunnerSpec>
loadManifestSpecs(const std::string& path, const std::string& serviceName)
{
  std::ifstream input(path);
  if (!input.good()) {
    throw std::runtime_error("cannot open service manifest: " + path);
  }
  return nativeModelRunnerSpecsByRoleForServiceManifestFromJson(input, serviceName);
}

NativeProviderAssignment
makeSingleProcessAssignment(const NativeExecutionPlan& plan)
{
  NativeProviderAssignment assignment;
  for (const auto& role : plan.roles) {
    assignment.providerByRole[role] = "/example/native-provider/" + trimSlashes(role);
  }
  return assignment;
}

std::map<std::string, TensorBundle>
initialInputsFor(const RoleSpec& role)
{
  if (!role.inputs.empty()) {
    return {};
  }

  TensorBundle input;
  input.name = "images";
  input.payload = floatPayload(1 * 3 * 32 * 32);
  input.expectedBytes = input.payload.size();
  return {{"images", std::move(input)}};
}

void
printUsage(const char* program)
{
  std::cerr << "usage: " << program
            << " <native-execution-plan.json> <service-manifest.json> [service-name]\n";
}

} // namespace

int
main(int argc, char** argv)
{
  try {
    if (argc < 3 || argc > 4) {
      printUsage(argv[0]);
      return 2;
    }
    const std::string serviceName = argc == 4 ? argv[3] : "/AI/YOLO/2x2Inference";
    auto plan = loadPlan(argv[1], serviceName);
    auto specs = loadManifestSpecs(argv[2], serviceName);

    auto factory = std::make_shared<RegistryNativeModelRunnerFactory>();
    registerOnnxRuntimeBackend(*factory);

    auto io = std::make_shared<InMemoryDependencyIo>();
    NativeProviderSession session(plan,
                                  makeSingleProcessAssignment(plan),
                                  io,
                                  factory,
                                  plan.roles.size());
    for (const auto& role : plan.roles) {
      const auto found = specs.find(role);
      if (found == specs.end()) {
        throw std::runtime_error("service manifest missing artifact for role: " + role);
      }
      session.registerRunner(found->second);
    }

    const std::string sessionId = "native-plan-onnx-smoke";
    std::vector<std::future<ProviderRoleResult>> futures;
    futures.reserve(plan.roles.size());
    for (const auto& role : plan.roles) {
      const auto roleSpec = session.roleSpec(role, sessionId);
      futures.push_back(session.executeRoleAsync(sessionId, role, initialInputsFor(roleSpec)));
    }

    std::size_t outputBytes = 0;
    std::size_t encodedBundleOutputs = 0;
    bool sawFinalOutput = false;
    for (auto& future : futures) {
      auto result = future.get();
      for (const auto& item : result.outputsByScope) {
        outputBytes += item.second.payload.size();
        if (isEncodedTensorBundle(item.second.payload)) {
          ++encodedBundleOutputs;
        }
        if (item.first == "predictions" || item.first == "output" ||
            item.first == "onnx-output-bundle") {
          sawFinalOutput = true;
        }
      }
    }

    if (io->publishedCount() == 0) {
      throw std::logic_error("native ONNX plan smoke published no dependency data");
    }
    if (!sawFinalOutput || outputBytes == 0) {
      throw std::logic_error("native ONNX plan smoke produced no final output");
    }

    std::cout << "NDNSF_DI_NATIVE_PLAN_ONNX_SMOKE_OK roles="
              << plan.roles.size()
              << " artifacts=" << specs.size()
              << " dependencyObjects=" << io->publishedCount()
              << " encodedBundleOutputs=" << encodedBundleOutputs
              << " outputBytes=" << outputBytes
              << std::endl;
    return 0;
  }
  catch (const std::exception& exc) {
    printUsage(argv[0]);
    std::cerr << "error: " << exc.what() << "\n";
    return 2;
  }
}
