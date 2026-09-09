#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceProvider.hpp"

#include <ndn-cxx/name.hpp>

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace ndnsf::di {

using ServiceProvider = ndn_service_framework::ServiceProvider;

// One served native service of the host. host->targets keeps at most one
// entry per serviceName: a closed target survives as the draining record
// until a same-name re-serve replaces it (closing-service semantics of the
// native-provider lifecycle design).  The record lives as long as the map
// entry, in-flight router copies, or a registration handle hold it.
struct Target
{
  std::shared_ptr<ServiceProvider::ServiceRegistration> core;
  std::shared_ptr<ExecutionLeaseService> lease;
  std::shared_ptr<std::atomic<bool>> draining;
};

// Host singleton state (see NativeInferenceProvider.hpp). The fixed lease
// entry, every collaboration handler, and every registration closure capture
// the shared_ptr chain rooted at NativeInferenceProvider::m_host, so the
// shared lease table and the router stay alive until Core detaches the last
// entry even if the provider object is destroyed first.
struct HostState
{
  std::string providerName;
  std::string providerBootId;
  std::size_t workerSlots = 1;
  std::shared_ptr<SharedExecutionLeaseState> sharedLease;
  std::shared_ptr<ServiceProvider::ServiceRegistration> fixedLease;
  std::mutex routerMutex;
  std::map<std::string, std::shared_ptr<Target>> targets;
};

struct NativeServiceRegistration::State
{
  std::string serviceName;
  std::shared_ptr<ServiceProvider::ServiceRegistration> core;
  std::shared_ptr<std::atomic<bool>> draining;
};

namespace {

uint64_t
hostEpochMs()
{
  const auto elapsed = std::chrono::system_clock::now().time_since_epoch();
  const auto ms =
    std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
  return static_cast<uint64_t>(std::max<long long>(0, ms));
}

// Host-wide compute-slot resolver: scan the host's boot-time slot range
// (host resources, never grown by later serves) and return the first slot
// without an active conflict key. An empty result means the host is
// saturated.  All targets of the same host serialize through the shared
// prepare mutex inside ExecutionLeaseService.
ExecutionLeaseService::ConflictKeyResolver
makeHostSlotResolver(const std::shared_ptr<HostState>& host)
{
  return [host](const LeaseOperationRequest&,
                const ExecutionLeaseRequestContext&) {
    const auto now = hostEpochMs();
    for (std::size_t slot = 0; slot < host->workerSlots; ++slot) {
      const auto key =
        host->providerName + ":compute-slot:" + std::to_string(slot);
      if (!host->sharedLease->table.hasActiveConflictKey(key, now)) {
        return std::vector<std::string>{key};
      }
    }
    return std::vector<std::string>{};
  };
}

ServiceProvider::AckStrategyHandler
acceptingAckHandler(std::string message)
{
  return [message = std::move(message)](
           const ndn_service_framework::RequestMessage&) {
    ServiceProvider::AckDecision decision;
    decision.status = true;
    decision.message = message;
    return decision;
  };
}

// Fixed lease entry handler: route one decoded lease operation to the target
// named by request.targetServiceName. An unknown target and a draining
// target are answered here without reaching any ExecutionLeaseService; the
// target's own instance still re-checks the binding for every operation it
// accepts. No exception ever crosses the Core callback.
ServiceProvider::RequestHandler
makeLeaseRouter(const std::shared_ptr<HostState>& host)
{
  return [host](const ndn::Name& requesterIdentity,
                const ndn::Name& providerName,
                const ndn::Name& serviceName,
                const ndn::Name& requestId,
                const ndn_service_framework::RequestMessage& request) {
    const auto wirePayload = request.getPayload();
    const std::string payload(
      reinterpret_cast<const char*>(wirePayload.data()), wirePayload.size());
    const ExecutionLeaseRequestContext context{
      requesterIdentity.toUri(), providerName.toUri(), serviceName.toUri(),
      requestId.toUri()};
    const auto now = hostEpochMs();
    // Errors travel inside the encoded LeaseOperationResponse; the wire
    // Response stays status=true so a lease client always decodes one.
    ndn_service_framework::ResponseMessage response;
    response.setStatus(true);
    auto complete = [&response](const std::string& responsePayload) {
      ndn::Buffer bytes(
        reinterpret_cast<const uint8_t*>(responsePayload.data()),
        responsePayload.size());
      response.setPayload(bytes, bytes.size());
      return response;
    };
    try {
      const auto operation = decodeLeaseOperationRequest(payload);
      std::shared_ptr<Target> target;
      {
        std::lock_guard<std::mutex> lock(host->routerMutex);
        const auto it = host->targets.find(operation.targetServiceName);
        if (it != host->targets.end()) {
          target = it->second;
        }
      }
      if (!target) {
        LeaseOperationResponse failure;
        failure.status = false;
        failure.operation = operation.operation;
        failure.reasonCode = "LEASE_SERVICE_MISMATCH";
        return complete(encodeLeaseOperationResponse(failure));
      }
      // Draining fences stop new Prepare/Commit/Renew; Abort/Release keep
      // routing so in-flight records finish their cleanup.
      if (target->draining->load(std::memory_order_relaxed) &&
          operation.operation != LeaseOperation::Abort &&
          operation.operation != LeaseOperation::Release) {
        LeaseOperationResponse failure;
        failure.status = false;
        failure.operation = operation.operation;
        failure.reasonCode = "LEASE_TARGET_DRAINING";
        return complete(encodeLeaseOperationResponse(failure));
      }
      // All Core identity/state/replay validation happens inside the
      // target's ExecutionLeaseService over the shared host table.
      return complete(target->lease->handle(context, payload, now));
    }
    catch (const std::exception&) {
      // A malformed payload or an unexpected failure is answered in-band.
      LeaseOperationResponse failure;
      failure.status = false;
      failure.operation = LeaseOperation::Prepare;
      failure.reasonCode = "LEASE_INTERNAL_ERROR";
      return complete(encodeLeaseOperationResponse(failure));
    }
  };
}

} // namespace

NativeServiceRegistration::NativeServiceRegistration(std::shared_ptr<State> state)
  : m_state(std::move(state))
{
}

NativeServiceRegistration::~NativeServiceRegistration() noexcept
{
  close();
}

void
NativeServiceRegistration::close() noexcept
{
  if (!m_state) {
    return;
  }
  if (m_state->draining) {
    m_state->draining->store(true, std::memory_order_relaxed);
  }
  if (m_state->core) {
    m_state->core->close();
  }
}

bool
NativeServiceRegistration::closed() const noexcept
{
  return !m_state || !m_state->core || m_state->core->closed();
}

bool
NativeServiceRegistration::valid() const noexcept
{
  return m_state != nullptr;
}

uint64_t
NativeServiceRegistration::generation() const noexcept
{
  return (m_state && m_state->core) ? m_state->core->generation() : 0;
}

const std::string&
NativeServiceRegistration::serviceName() const noexcept
{
  static const std::string empty;
  return m_state ? m_state->serviceName : empty;
}

NativeInferenceProvider::NativeInferenceProvider(
  std::shared_ptr<ndn_service_framework::ServiceProvider> provider,
  std::shared_ptr<const NativeAdapterRegistry> adapters)
  : m_provider(std::move(provider)), m_adapters(std::move(adapters))
{
  if (!m_provider) {
    throw std::invalid_argument(
      "NativeInferenceProvider requires a Core ServiceProvider");
  }
  if (!m_adapters) {
    throw std::invalid_argument(
      "NativeInferenceProvider requires the NativeAdapterRegistry");
  }
}

NativeInferenceProvider::~NativeInferenceProvider() noexcept
{
  stop();
}

NativeServiceRegistration
NativeInferenceProvider::serve(const NativeServiceDefinition& service,
                               const NativeProviderHandlerConfig& config)
{
  if (service.serviceName.empty()) {
    throw std::invalid_argument(
      "native service definition requires a serviceName");
  }
  if (service.allowedRoles.empty()) {
    throw std::invalid_argument(
      "native service definition requires allowedRoles");
  }

  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_stopped) {
    throw std::runtime_error("NativeInferenceProvider host is stopped");
  }

  // The first serve builds the host singleton and its fixed lease entry in a
  // local transaction.  Do not publish m_host until the target runtime and
  // scoped Core registration have both succeeded; otherwise a failed first
  // serve would pin an identity/lease table that has no usable target.
  auto host = m_host;
  bool hostCreated = false;
  if (!host) {
    host = std::make_shared<HostState>();
    host->providerName = config.localProviderName;
    host->providerBootId = config.providerBootId;
    host->workerSlots = std::max<std::size_t>(1, config.workerCount);
    host->sharedLease =
      std::make_shared<SharedExecutionLeaseState>(host->providerBootId);
    try {
      host->fixedLease = std::make_shared<ServiceProvider::ServiceRegistration>(
        m_provider->addScopedService(
          ndn::Name(EXECUTION_LEASE_SERVICE_NAME),
          acceptingAckHandler("execution lease service ready"),
          makeLeaseRouter(host),
          ServiceProvider::ServiceInvocationMode::NormalAndTargeted));
    }
    catch (...) {
      throw;
    }
    hostCreated = true;
  }

  std::shared_ptr<std::atomic<bool>> draining;
  std::shared_ptr<ServiceProvider::ServiceRegistration> core;
  try {
    // A serve must not silently reshape the host's shared boot identity or its
    // compute-slot range: the slot range is host resources configured at boot,
    // not a sum over serves.
    if (!config.localProviderName.empty() &&
        config.localProviderName != host->providerName) {
      throw std::invalid_argument("native service config providerName '"
        + config.localProviderName + "' conflicts with the host's '"
        + host->providerName + "'");
    }
    if (!config.providerBootId.empty() &&
        config.providerBootId != host->providerBootId) {
      throw std::invalid_argument(
        "native service config providerBootId conflicts with the host's");
    }
    if (std::max<std::size_t>(1, config.workerCount) != host->workerSlots) {
      throw std::invalid_argument(
        "native service config workerCount conflicts with the host's "
        "compute-slot range");
    }

    // Same-name re-serve: an active target is refused; a draining (closed)
    // record may be replaced below by a fresh registration that never reuses
    // the old target's lease instance, draining fence, or bindings.
    {
      std::lock_guard<std::mutex> routerLock(host->routerMutex);
      const auto existing = host->targets.find(service.serviceName);
      if (existing != host->targets.end() &&
          !existing->second->draining->load(std::memory_order_relaxed)) {
        throw std::logic_error("native service is already registered: "
                               + service.serviceName);
      }
    }

    // The host owns the shared lease table: when the configuration opts into
    // execution leases, inject the host table and bind the lease target to
    // this service. The table outlives every handler state because each
    // collaboration closure below captures the host (and therefore the shared
    // state) and is detached by Core only after the handler itself is gone.
    NativeProviderHandlerConfig effectiveConfig = config;
    if (!config.executionLeaseTargetService.empty()) {
      if (config.executionLeaseTargetService != service.serviceName) {
        throw std::invalid_argument("executionLeaseTargetService '"
          + config.executionLeaseTargetService
          + "' must equal the served serviceName '" + service.serviceName
          + "'");
      }
      if (config.executionLeaseTable != nullptr) {
        throw std::invalid_argument(
          "executionLeaseTable is owned by the host; pass a null table");
      }
      effectiveConfig.executionLeaseTable = &host->sharedLease->table;
    }
    else if (config.executionLeaseTable != nullptr) {
      throw std::invalid_argument(
        "executionLeaseTable requires a non-empty executionLeaseTargetService");
    }

    // Assemble the native runtime first; nothing is registered until the
    // runtime and the observation seam both succeed.
    auto runtime = makeNativeProviderCollaborationRuntime(
      std::move(effectiveConfig));
    if (service.runtimeObserver) {
      service.runtimeObserver(runtime);
    }

    draining = std::make_shared<std::atomic<bool>>(false);
    auto guardedHandler =
      [host, draining, handler = runtime.handler](
        ServiceProvider::CollaborationContext& ctx,
        const ndn_service_framework::RequestMessage& request) {
        // The Core registration gate is authoritative; this fence only covers
        // the tiny window between draining and the Core entry detaching.
        if (draining->load(std::memory_order_relaxed)) {
          ctx.fail("native service is closing");
          return;
        }
        handler(ctx, request);
      };
    const auto ackHandler = service.ackHandler
      ? service.ackHandler
      : acceptingAckHandler("native provider ready");

    core = std::make_shared<ServiceProvider::ServiceRegistration>(
      m_provider->addScopedCollaborationHandler(
        ndn::Name(service.serviceName), service.allowedRoles,
        ackHandler, guardedHandler));
    auto target = std::make_shared<Target>();
    target->core = core;
    target->draining = draining;
    target->lease = std::make_shared<ExecutionLeaseService>(
      host->providerName, service.serviceName,
      makeHostSlotResolver(host), host->sharedLease);
    {
      std::lock_guard<std::mutex> routerLock(host->routerMutex);
      host->targets[service.serviceName] = std::move(target);
    }
    // Publish only after the full first target is installed.  m_mutex is held
    // for the entire serve call, so a concurrent serve cannot observe a half
    // initialized host.
    if (hostCreated) {
      m_host = host;
      hostCreated = false;
    }
  }
  catch (...) {
    if (draining) {
      draining->store(true, std::memory_order_relaxed);
    }
    if (core) {
      core->close();
    }
    if (hostCreated) {
      if (host->fixedLease) {
        host->fixedLease->close();
      }
      m_host.reset();
    }
    throw;
  }

  auto state = std::make_shared<NativeServiceRegistration::State>();
  state->serviceName = service.serviceName;
  state->core = std::move(core);
  state->draining = std::move(draining);
  return NativeServiceRegistration(std::move(state));
}

void
NativeInferenceProvider::stop() noexcept
{
  std::shared_ptr<HostState> host;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_stopped) {
      return;
    }
    m_stopped = true;
    host = m_host;
  }
  if (!host) {
    return;
  }
  if (host->fixedLease) {
    host->fixedLease->close();
  }
  std::vector<std::shared_ptr<Target>> targets;
  {
    std::lock_guard<std::mutex> routerLock(host->routerMutex);
    targets.reserve(host->targets.size());
    for (const auto& entry : host->targets) {
      targets.push_back(entry.second);
    }
  }
  for (const auto& target : targets) {
    target->draining->store(true, std::memory_order_relaxed);
    if (target->core) {
      target->core->close();
    }
  }
}

} // namespace ndnsf::di
