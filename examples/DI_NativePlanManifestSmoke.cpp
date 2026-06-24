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

double
prefetchMs(const ProviderRoleResult& result)
{
  double total = 0.0;
  for (const auto& timing : result.inputTimings) {
    total += durationMs(timing.prefetchStartedAt, timing.fetchCompletedAt);
  }
  return total;
}

double
publishMs(const ProviderRoleResult& result)
{
  double total = 0.0;
  for (const auto& timing : result.outputTimings) {
    total += durationMs(timing.outputReadyAt, timing.publishDoneAt);
  }
  return total;
}

double
executeMs(const ProviderRoleResult& result)
{
  if (result.outputTimings.empty()) {
    return durationMs(result.timing.startedAt, result.timing.finishedAt);
  }
  auto outputReadyAt = result.outputTimings.front().outputReadyAt;
  for (const auto& timing : result.outputTimings) {
    if (timing.outputReadyAt > outputReadyAt) {
      outputReadyAt = timing.outputReadyAt;
    }
  }
  return durationMs(result.timing.startedAt, outputReadyAt);
}

std::size_t
inputBytes(const ProviderRoleResult& result)
{
  std::size_t total = 0;
  for (const auto& timing : result.inputTimings) {
    total += timing.bytes;
  }
  return total;
}

std::size_t
outputBytes(const ProviderRoleResult& result)
{
  std::size_t total = 0;
  for (const auto& timing : result.outputTimings) {
    total += timing.bytes;
  }
  if (total == 0) {
    for (const auto& item : result.outputsByScope) {
      total += item.second.payload.size();
    }
  }
  return total;
}

std::string
tracerProviderForRole(const std::string& assignmentName, const std::string& role)
{
  const std::string prefix = assignmentName == "alternate" ?
    "/NDNSF-DI/Tracer/alt-provider" : "/NDNSF-DI/Tracer/provider";

  if (role == "/Backbone") {
    return prefix + "/backbone";
  }
  if (role == "/Head/Shard/0") {
    return prefix + "/head0";
  }
  if (role == "/Head/Shard/1") {
    return prefix + "/head1";
  }
  if (role == "/Merge") {
    return prefix + "/merge";
  }
  throw std::runtime_error("unknown tracer role for assignment: " + role);
}

std::string
csvEscape(const std::string& value)
{
  if (value.find_first_of(",\"\n") == std::string::npos) {
    return value;
  }
  std::string escaped = "\"";
  for (const auto ch : value) {
    if (ch == '"') {
      escaped += "\"\"";
    }
    else {
      escaped += ch;
    }
  }
  escaped += "\"";
  return escaped;
}

void
writeTimingCsv(const std::string& path,
               const std::string& sessionId,
               const NativeProviderAssignment& assignment,
               const std::map<std::string, ProviderRoleResult>& resultsByRole)
{
  std::ofstream output(path);
  if (!output.good()) {
    throw std::runtime_error("cannot open timing csv: " + path);
  }
  output << "sessionId,provider,role,inputBytes,outputBytes,prefetchMs,executeMs,publishMs,endToEndMs,status\n";
  output.setf(std::ios::fixed);
  output.precision(3);
  for (const auto& item : resultsByRole) {
    const auto& result = item.second;
    const auto provider = providerForRole(assignment, item.first, "/example/provider");
    output << csvEscape(sessionId) << ","
           << csvEscape(provider) << ","
           << csvEscape(item.first) << ","
           << inputBytes(result) << ","
           << outputBytes(result) << ","
           << prefetchMs(result) << ","
           << executeMs(result) << ","
           << publishMs(result) << ","
           << durationMs(result.timing.queuedAt, result.timing.finishedAt) << ","
           << "ok\n";
  }
}

int
main(int argc, char** argv)
{
  if (argc < 3) {
    std::cerr << "usage: " << argv[0]
              << " <native-execution-plan.json> <service-manifest.json>"
              << " [service-name] [--timing-csv <path>] [--assignment default|alternate]\n";
    return 2;
  }

  std::string serviceName = "/AI/YOLO/2x2Inference";
  std::string timingCsv;
  std::string assignmentName = "default";
  for (int i = 3; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--timing-csv") {
      if (i + 1 >= argc) {
        std::cerr << "--timing-csv requires a path\n";
        return 2;
      }
      timingCsv = argv[++i];
    }
    else if (arg == "--assignment") {
      if (i + 1 >= argc) {
        std::cerr << "--assignment requires default or alternate\n";
        return 2;
      }
      assignmentName = argv[++i];
      if (assignmentName != "default" && assignmentName != "alternate") {
        std::cerr << "unknown assignment: " << assignmentName << "\n";
        return 2;
      }
    }
    else {
      serviceName = arg;
    }
  }

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
    assignment.providerByRole[role] = tracerProviderForRole(assignmentName, role);
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

  const std::string sessionId = "native-plan-manifest-smoke";
  std::vector<std::future<ProviderRoleResult>> futures;
  for (const auto& role : plan.roles) {
    futures.push_back(session.executeRoleAsync(sessionId, role));
  }

  std::size_t finalOutputCount = 0;
  std::map<std::string, ProviderRoleResult> resultsByRole;
  for (auto& future : futures) {
    auto result = future.get();
    for (const auto& item : result.outputsByScope) {
      if (isEncodedTensorBundle(item.second.payload)) {
        finalOutputCount += decodeTensorBundle(item.second.payload).size();
      }
    }
    resultsByRole.emplace(result.timing.role, std::move(result));
  }

  if (io->publishedScopes().empty() || finalOutputCount == 0) {
    throw std::logic_error("native plan/manifest smoke produced no dependency output");
  }
  if (!timingCsv.empty()) {
    writeTimingCsv(timingCsv, sessionId, assignment, resultsByRole);
  }

  std::cout << "NDNSF_DI_NATIVE_PLAN_MANIFEST_SMOKE_OK roles="
            << plan.roles.size()
            << " artifacts=" << specs.size()
            << " outputTensors=" << finalOutputCount
            << std::endl;
  return 0;
}
