#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderSession.hpp"

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

TensorBundle
bundle(std::string name, std::string text)
{
  return TensorBundle{
    std::move(name),
    std::vector<uint8_t>(text.begin(), text.end()),
    1,
    text.size(),
  };
}

std::string
payloadText(const TensorBundle& value)
{
  return std::string(value.payload.begin(), value.payload.end());
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

private:
  static std::string
  key(const std::string& sessionId, const DependencyEdge& edge)
  {
    return sessionId + "|" + edge.plannedDataName;
  }

private:
  std::mutex m_mutex;
  std::map<std::string, TensorBundle> m_available;
  std::map<std::string, std::vector<std::shared_ptr<std::promise<TensorBundle>>>> m_waiters;
};

NativeExecutionPlan
makePlan()
{
  NativeExecutionPlan plan;
  plan.roles = {"/Backbone", "/Head/Shard/0", "/Head/Shard/1", "/Merge"};
  plan.dependencies = {
    NativeDependencySpec{
      {"/Backbone"},
      {"/Head/Shard/0"},
      "backbone-to-head-shard0",
      "/activation",
      "{producerProvider}/NDNSF/DI/ACTIVATION/{sessionId}/{keyScope}/{producerRole}/bundle/{sequence}",
      1,
      1024,
    },
    NativeDependencySpec{
      {"/Backbone"},
      {"/Head/Shard/1"},
      "backbone-to-head-shard1",
      "/activation",
      "{producerProvider}/NDNSF/DI/ACTIVATION/{sessionId}/{keyScope}/{producerRole}/bundle/{sequence}",
      1,
      1024,
    },
    NativeDependencySpec{
      {"/Head/Shard/0"},
      {"/Merge"},
      "detect-head-shard0-to-merge",
      "/activation",
      "{producerProvider}/NDNSF/DI/ACTIVATION/{sessionId}/{keyScope}/{producerRole}/bundle/{sequence}",
      1,
      512,
    },
    NativeDependencySpec{
      {"/Head/Shard/1"},
      {"/Merge"},
      "detect-head-shard1-to-merge",
      "/activation",
      "{producerProvider}/NDNSF/DI/ACTIVATION/{sessionId}/{keyScope}/{producerRole}/bundle/{sequence}",
      1,
      512,
    },
  };
  return plan;
}

} // namespace

int
main()
{
  const auto plan = makePlan();
  NativeProviderAssignment assignment;
  for (const auto& role : plan.roles) {
    assignment.providerByRole[role] = "/example/provider/" + trimSlashes(role);
  }

  auto factory = std::make_shared<RegistryNativeModelRunnerFactory>();
  factory->registerBackend(
    "fake-backend",
    [] (const NativeModelRunnerSpec& spec) {
      return makeNativeModelRunner(
        [spec] (const RoleExecutionContext& ctx) {
          if (ctx.role == "/Backbone") {
            std::map<std::string, TensorBundle> outputs;
            for (const auto& item : spec.metadata) {
              if (item.first.find("outputScope.") == 0) {
                outputs.emplace(item.second, bundle(item.second, "features"));
              }
            }
            return outputs;
          }
          if (ctx.role.find("/Head/Shard/") == 0) {
            if (ctx.inputsByScope.size() != 1) {
              throw std::logic_error("head role expected one input");
            }
            std::map<std::string, TensorBundle> outputs;
            for (const auto& item : spec.metadata) {
              if (item.first.find("outputScope.") == 0) {
                outputs.emplace(item.second, bundle(item.second, "head:" + ctx.role));
              }
            }
            return outputs;
          }
          if (ctx.role == "/Merge") {
            if (ctx.inputsByScope.size() != 2) {
              throw std::logic_error("merge role expected two inputs");
            }
            std::string merged;
            for (const auto& input : ctx.inputsByScope) {
              if (!merged.empty()) {
                merged += "+";
              }
              merged += payloadText(input.second);
            }
            return std::map<std::string, TensorBundle>{
              {"final-response", bundle("final-response", merged)},
            };
          }
          throw std::logic_error("unexpected role: " + ctx.role);
        });
    });

  auto io = std::make_shared<InMemoryDependencyIo>();
  NativeProviderSession session(plan, assignment, io, factory, plan.roles.size());

  for (const auto& role : plan.roles) {
    const auto roleSpec = session.roleSpec(role, "smoke-session");
    NativeModelRunnerSpec spec;
    spec.role = role;
    spec.kind = "test-model";
    spec.backend = "fake-backend";
    spec.path = "/tmp/" + trimSlashes(role) + ".onnx";
    for (std::size_t i = 0; i < roleSpec.outputs.size(); ++i) {
      spec.metadata["outputScope." + std::to_string(i)] = roleSpec.outputs[i].scope;
    }
    session.registerRunner(spec);
  }

  std::vector<std::future<ProviderRoleResult>> futures;
  for (const auto& role : plan.roles) {
    futures.push_back(session.executeRoleAsync("smoke-session", role));
  }

  std::map<std::string, ProviderRoleResult> results;
  for (auto& future : futures) {
    auto result = future.get();
    results.emplace(result.timing.role, std::move(result));
  }

  const auto merge = results.find("/Merge");
  if (merge == results.end() ||
      merge->second.outputsByScope.count("final-response") == 0) {
    throw std::logic_error("merge final-response missing");
  }

  NativeProviderHandlerConfig handlerConfig;
  handlerConfig.plan = plan;
  handlerConfig.assignment = assignment;
  handlerConfig.runnerFactory = factory;
  (void)makeNativeProviderCollaborationHandler(std::move(handlerConfig));

  std::cout << "NDNSF_DI_NATIVE_PROVIDER_SMOKE_OK "
            << payloadText(merge->second.outputsByScope.at("final-response"))
            << std::endl;
  return 0;
}
