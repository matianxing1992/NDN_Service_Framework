#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderSession.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeServiceManifest.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"

#include <fstream>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace ndnsf::di;

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

std::vector<NamedTensor>
decodeAllInputs(const RoleExecutionContext& ctx)
{
  std::vector<NamedTensor> tensors;
  for (const auto& item : ctx.inputsByScope) {
    if (!isEncodedTensorBundle(item.second.payload)) {
      continue;
    }
    auto decoded = decodeTensorBundle(item.second.payload);
    tensors.insert(tensors.end(), decoded.begin(), decoded.end());
  }
  return tensors;
}

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
      m_published.push_back(edge.scope);
      const auto itemKey = key(sessionId, edge);
      m_available[itemKey] = value;
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

  std::vector<std::string>
  publishedScopes() const
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_published;
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
  std::vector<std::string> m_published;
};

} // namespace

int
main(int argc, char** argv)
{
  if (argc < 3 || argc > 4) {
    std::cerr << "usage: " << argv[0]
              << " <native-execution-plan.json> <service-manifest.json> [service-name]\n";
    return 2;
  }

  const std::string serviceName = argc == 4 ? argv[3] : "/AI/YOLO/2x2Inference";

  std::ifstream planInput(argv[1]);
  if (!planInput.good()) {
    std::cerr << "cannot open native execution plan: " << argv[1] << "\n";
    return 2;
  }
  auto plan = nativeExecutionPlanForServiceFromJson(planInput, serviceName);

  std::ifstream manifestInput(argv[2]);
  if (!manifestInput.good()) {
    std::cerr << "cannot open service manifest: " << argv[2] << "\n";
    return 2;
  }
  auto specs = nativeModelRunnerSpecsByRoleForServiceManifestFromJson(
    manifestInput, serviceName);

  NativeProviderAssignment assignment;
  for (const auto& role : plan.roles) {
    assignment.providerByRole[role] = "/example/provider/" + trimSlashes(role);
  }

  auto io = std::make_shared<InMemoryDependencyIo>();
  auto factory = std::make_shared<RegistryNativeModelRunnerFactory>();
  factory->registerBackend(
    "onnxruntime",
    [] (const NativeModelRunnerSpec& spec) {
      return makeNativeModelRunner(
        [spec] (const RoleExecutionContext& ctx) {
          const auto inputNames = splitNames(spec.metadata.count("input_tensors") ?
                                            spec.metadata.at("input_tensors") : "");
          const auto availableInputs = decodeAllInputs(ctx);
          if (!ctx.inputsByScope.empty()) {
            for (const auto& name : inputNames) {
              if (name.empty()) {
                continue;
              }
              (void)findTensor(availableInputs, name);
            }
          }

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
          return std::map<std::string, TensorBundle>{
            {"onnx-output-bundle", makeEncodedTensorBundle("onnx-output-bundle", outputs)},
          };
        });
    });

  NativeProviderSession session(plan, assignment, io, factory, plan.roles.size());
  for (auto& item : specs) {
    session.registerRunner(std::move(item.second));
  }

  std::vector<std::future<ProviderRoleResult>> futures;
  for (const auto& role : plan.roles) {
    futures.push_back(session.executeRoleAsync("native-plan-manifest-smoke", role));
  }

  std::size_t finalOutputCount = 0;
  for (auto& future : futures) {
    auto result = future.get();
    for (const auto& item : result.outputsByScope) {
      if (isEncodedTensorBundle(item.second.payload)) {
        finalOutputCount += decodeTensorBundle(item.second.payload).size();
      }
    }
  }

  if (io->publishedScopes().empty() || finalOutputCount == 0) {
    throw std::logic_error("native plan/manifest smoke produced no dependency output");
  }

  std::cout << "NDNSF_DI_NATIVE_PLAN_MANIFEST_SMOKE_OK roles="
            << plan.roles.size()
            << " artifacts=" << specs.size()
            << " outputTensors=" << finalOutputCount
            << std::endl;
  return 0;
}
