#include "local-pipeline-dependency-io.hpp"

#include "NdnsfCollaborationDependencyIo.hpp"
#include "TensorBundleCodec.hpp"
#include "request-work-statistics.hpp"

#include <algorithm>
#include <stdexcept>

namespace ndnsf::di {
namespace {

std::size_t
bundleBytes(const TensorBundle& bundle)
{
  return bundle.localTensors ? tensorBundleWireSize(*bundle.localTensors) : bundle.payload.size();
}

void
observeDelivery(const std::shared_ptr<NativeWorkStatistics>& statistics,
                const char* count, const char* bytes, std::size_t size) noexcept
{
  if (!statistics) return;
  try {
    statistics->addCount(count, 1);
    statistics->addCount(bytes, size);
  }
  catch (...) {
    // Measurement is independent of the committed delivery.
  }
}

} // namespace

LocalDependencyAttempt::LocalDependencyAttempt(std::string identity, std::size_t maxSlots,
                                               std::size_t maxBytes)
  : m_identity(std::move(identity))
  , m_maxSlots(maxSlots)
  , m_maxBytes(maxBytes)
{
  if (m_identity.empty() || m_maxSlots == 0 || m_maxBytes == 0)
    throw std::invalid_argument("DI_LOCAL_DEPENDENCY_INCOMPLETE_BUDGET");
}

void
LocalDependencyAttempt::attach(const std::string& role, const std::string& identity, bool cpu,
                               Clock::time_point deadline)
{
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    requireActive();
    if (identity != m_identity || m_roles.size() >= 2 || !m_roles.insert(role).second) {
      m_failed = true;
      m_changed.notify_all();
      throw std::invalid_argument("DI_LOCAL_DEPENDENCY_DISPATCH_MISMATCH");
    }
    m_allCpu = m_allCpu && cpu;
    m_deadline = std::min(m_deadline, deadline);
  }
  m_changed.notify_all();
}

bool
LocalDependencyAttempt::awaitRoute(const std::function<bool()>& current)
{
  await([this] { return m_roles.size() == 2; }, current);
  std::lock_guard<std::mutex> lock(m_mutex);
  requireActive();
  return m_allCpu;
}

std::uint64_t
LocalDependencyAttempt::leaseDeadline(std::uint64_t proposedDeadline)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  requireActive();
  if (m_leaseDeadline == 0) m_leaseDeadline = proposedDeadline;
  return m_leaseDeadline;
}

LocalDependencyAttempt::ConversationUse
LocalDependencyAttempt::acquireConversationUse(const std::function<ConversationUse()>& admit)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  requireActive();
  if (!m_conversationUse) m_conversationUse = admit();
  return m_conversationUse;
}

void
LocalDependencyAttempt::markComputed(const std::string& role)
{
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    requireActive();
    if (m_roles.count(role) == 0 || !m_computed.insert(role).second)
      throw std::runtime_error("DI_LOCAL_DEPENDENCY_COMPUTE_ROLE_MISMATCH");
  }
  m_changed.notify_all();
}

void
LocalDependencyAttempt::awaitComputed(const std::function<bool()>& current)
{
  await([this] { return m_computed.size() == 2; }, current);
}

void
LocalDependencyAttempt::await(const std::function<bool()>& predicate,
                              const std::function<bool()>& current)
{
  std::unique_lock<std::mutex> lock(m_mutex);
  while (true) {
    requireActive();
    lock.unlock();
    const bool authorized = current();
    lock.lock();
    requireActive();
    if (!authorized)
      throw std::runtime_error("DI_LOCAL_DEPENDENCY_WORK_EXPIRED");
    if (predicate()) return;
    m_changed.wait_until(lock, std::min(m_deadline, Clock::now() + std::chrono::milliseconds(10)));
  }
}

void
LocalDependencyAttempt::finishRole(const std::string& role, bool succeeded)
{
  std::map<std::string, Slot> discarded;
  ConversationUse conversation;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_finished.insert(role);
    m_failed = m_failed || !succeeded;
    if (!m_failed && m_finished.size() != 2) return;
    m_slots.swap(discarded);
    conversation.swap(m_conversationUse);
    m_bytes = 0;
  }
  m_changed.notify_all();
  const auto error = std::make_exception_ptr(std::runtime_error("DI_LOCAL_DEPENDENCY_ATTEMPT_ENDED"));
  for (auto& entry : discarded) {
    if (entry.second.reader) entry.second.reader->promise.set_exception(error);
  }
}

void
LocalDependencyAttempt::cancelReads(const std::string& role) noexcept
{
  try {
    const auto error = std::make_exception_ptr(std::runtime_error("DI_LOCAL_DEPENDENCY_READ_CANCELLED"));
    std::vector<PendingRead> discarded;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      discarded.reserve(m_slots.size());
      m_cancelledReaders.insert(role);
      for (auto& entry : m_slots) {
        auto& slot = entry.second;
        if (slot.edge.consumerRole == role && slot.reader) {
          discarded.push_back(std::move(*slot.reader));
          slot.reader.reset();
        }
      }
    }
    for (auto& reader : discarded) reader.promise.set_exception(error);
  }
  catch (...) {
    // The role's terminal guard also retires the attempt and pending reads.
  }
}

void
LocalDependencyAttempt::requireActive() const
{
  if (m_failed || m_finished.size() >= 2 || Clock::now() >= m_deadline)
    throw std::runtime_error("DI_LOCAL_DEPENDENCY_ATTEMPT_ENDED");
}

LocalDependencyAttempt::Slot&
LocalDependencyAttempt::slotFor(const DependencyEdge& edge)
{
  requireActive();
  if (m_roles.size() != 2 || !m_allCpu || m_roles.count(edge.producerRole) == 0 ||
      m_roles.count(edge.consumerRole) == 0 || m_cancelledReaders.count(edge.consumerRole) != 0)
    throw std::runtime_error("DI_LOCAL_DEPENDENCY_ROLE_UNAVAILABLE");
  const auto it = m_slots.find(edge.plannedDataName);
  if (it == m_slots.end()) {
    if (m_slots.size() >= m_maxSlots)
      throw std::length_error("DI_LOCAL_DEPENDENCY_SLOT_LIMIT");
    return m_slots.emplace(edge.plannedDataName, Slot{edge, {}, {}, false}).first->second;
  }
  const auto& first = it->second.edge;
  if (first.requestId != edge.requestId || first.attemptEpoch != edge.attemptEpoch ||
      first.planDigest != edge.planDigest || first.executionObjectDigest != edge.executionObjectDigest ||
      first.endpointDigest != edge.endpointDigest || first.round != edge.round ||
      first.layoutDigest != edge.layoutDigest || first.producerRole != edge.producerRole ||
      first.consumerRole != edge.consumerRole || it->second.consumed)
    throw std::invalid_argument("DI_LOCAL_DEPENDENCY_SLOT_MISMATCH");
  return it->second;
}

std::future<TensorBundle>
LocalDependencyAttempt::read(const DependencyEdge& edge, PendingRead reader)
{
  auto future = reader.promise.get_future();
  std::optional<TensorBundle> value;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto& slot = slotFor(edge);
    if (slot.reader) throw std::runtime_error("DI_LOCAL_DEPENDENCY_DUPLICATE_READ");
    if (!slot.value) {
      slot.reader.emplace(std::move(reader));
      return future;
    }
    if (slot.value->expectedBytes > reader.maxBytes)
      throw std::length_error("DI_LOCAL_DEPENDENCY_CONSUMER_BYTE_LIMIT");
    value = std::move(slot.value);
    slot.value.reset();
    slot.consumed = true;
    m_bytes -= value->expectedBytes;
  }
  value->localDelivery = std::move(reader.delivery);
  observeDelivery(reader.statistics, "dependency.localTake", "dependency.localTakeBytes",
                  value->expectedBytes);
  reader.promise.set_value(std::move(*value));
  return future;
}

void
LocalDependencyAttempt::publish(const DependencyEdge& edge, const TensorBundle& bundle,
                               std::size_t bytes)
{
  std::optional<PendingRead> reader;
  TensorBundle value;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto& slot = slotFor(edge);
    if (slot.value) throw std::runtime_error("DI_LOCAL_DEPENDENCY_DUPLICATE_OUTPUT");
    if (slot.reader && bytes > slot.reader->maxBytes)
      throw std::length_error("DI_LOCAL_DEPENDENCY_CONSUMER_BYTE_LIMIT");
    if (bytes > m_maxBytes - m_bytes)
      throw std::length_error("DI_LOCAL_DEPENDENCY_BYTE_LIMIT");
    value = bundle;
    value.expectedBytes = bytes;
    value.transferObservation.reset();
    value.localDelivery.reset();
    if (!slot.reader) {
      m_bytes += bytes;
      slot.value.emplace(std::move(value));
      return;
    }
    reader = std::move(slot.reader);
    slot.reader.reset();
    slot.consumed = true;
  }
  value.localDelivery = std::move(reader->delivery);
  observeDelivery(reader->statistics, "dependency.localTake", "dependency.localTakeBytes", bytes);
  reader->promise.set_value(std::move(value));
}

LocalPipelineDependencyIo::LocalPipelineDependencyIo(
  std::shared_ptr<LocalDependencyAttempt> local, std::shared_ptr<DependencyIo> network,
  std::shared_ptr<ProtectedRuntime> protection, std::shared_ptr<ProviderGroupCoordinator> group,
  std::string sessionId, std::string role, std::string provider,
  std::function<bool()> current, std::shared_ptr<NativeWorkStatistics> statistics)
  : m_local(std::move(local))
  , m_network(std::move(network))
  , m_cancellable(dynamic_cast<PromptCancellableDependencyIo*>(m_network.get()))
  , m_protection(std::move(protection))
  , m_group(std::move(group))
  , m_sessionId(std::move(sessionId))
  , m_role(std::move(role))
  , m_provider(std::move(provider))
  , m_current(std::move(current))
  , m_statistics(std::move(statistics))
{
}

bool
LocalPipelineDependencyIo::isLocal(const DependencyEdge& edge) const noexcept
{
  return edge.producerRole != edge.consumerRole &&
         (edge.operationKind == "ACTIVATION" || edge.operationKind == "PIPELINE" ||
          edge.operationKind == "TOKEN_FEEDBACK");
}

std::size_t
LocalPipelineDependencyIo::authorize(const std::string& sessionId, const DependencyEdge& edge,
                                    bool publish) const
{
  if (sessionId != m_sessionId || !m_current() || !m_protection || !m_group ||
      !m_group->hasCapability() || m_role != (publish ? edge.producerRole : edge.consumerRole))
    throw std::runtime_error("DI_LOCAL_DEPENDENCY_AUTHORITY_REJECTED");
  const auto now = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count());
  m_protection->authorizeDataflow(
    publish ? ProtectedDataflowDirection::Publish : ProtectedDataflowDirection::Fetch,
    edge.endpointDigest, edge.producerRole, edge.consumerRole, now, edge.round, m_statistics);
  const auto limit = validateLocalPipelineOperation(edge, m_provider, m_group->capability());
  if (!m_group->beginOperation(now))
    throw std::runtime_error("DI_LOCAL_DEPENDENCY_GROUP_CLOSED");
  return limit;
}

std::future<TensorBundle>
LocalPipelineDependencyIo::prefetchInput(const std::string& sessionId, const DependencyEdge& edge)
{
  if (!isLocal(edge)) return m_network->prefetchInput(sessionId, edge);
  return prefetchInputWithFirstReadBarrier(sessionId, edge).result;
}

void
LocalPipelineDependencyIo::publishOutput(const std::string& sessionId, const DependencyEdge& edge,
                                        const TensorBundle& bundle)
{
  if (!isLocal(edge)) {
    m_network->publishOutput(sessionId, edge, bundle);
    return;
  }
  const auto limit = authorize(sessionId, edge, true);
  validateTensorBundleForEdge(edge, bundle, false);
  const auto bytes = bundleBytes(bundle);
  if (bytes == 0 || bytes > limit || (bundle.localTensors && !bundle.payload.empty()))
    throw std::length_error("DI_LOCAL_DEPENDENCY_VALUE_SIZE");
  if (bundle.localTensors) decodeTensorBundleViews(bundle, m_statistics);
  m_local->publish(edge, bundle, bytes);
  markLocalDependencyPublication(edge, bundle);
  observeDelivery(m_statistics, "dependency.localPut", "dependency.localPutBytes", bytes);
}

bool
LocalPipelineDependencyIo::supportsPromptPrefetchCancellation(const DependencyEdge& edge) const noexcept
{
  return isLocal(edge) || (m_cancellable && m_cancellable->supportsPromptPrefetchCancellation(edge));
}

PromptCancellableDependencyIo::PrefetchOperation
LocalPipelineDependencyIo::prefetchInputWithFirstReadBarrier(const std::string& sessionId,
                                                            const DependencyEdge& edge)
{
  if (!isLocal(edge)) {
    if (!m_cancellable) throw std::invalid_argument("dependency has no cancellable prefetch");
    return m_cancellable->prefetchInputWithFirstReadBarrier(sessionId, edge);
  }
  const auto limit = authorize(sessionId, edge, false);
  auto result = m_local->read(edge, {{}, std::shared_ptr<const LocalDependencyDelivery>(
    new LocalDependencyDelivery(edge)), m_statistics, limit});
  std::promise<void> started;
  auto first = started.get_future().share();
  started.set_value();
  return {std::move(result), std::move(first)};
}

void
LocalPipelineDependencyIo::cancelPendingPrefetches() noexcept
{
  m_local->cancelReads(m_role);
  if (m_cancellable) m_cancellable->cancelPendingPrefetches();
}

void
LocalPipelineDependencyIo::drainPendingPrefetches() noexcept
{
  if (m_cancellable) m_cancellable->drainPendingPrefetches();
}

} // namespace ndnsf::di
