#include "dependency-transfer.hpp"
#include "detail/collaboration-input-budget.hpp"
#include "detail/collaboration-envelope.hpp"
#include "detail/finite-packet-recovery.hpp"
#include "detail/nac-consumer-transport.hpp"
#include "ProtectedBlobManifest.hpp"
#include <ndn-cxx/util/sha256.hpp>
#include <set>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <string_view>

NDN_LOG_INIT(ndn_service_framework.DependencyTransfer);

namespace ndn_service_framework {


namespace {
std::string admissionErrorCode(const std::exception& error, const std::string& fallback)
{
  static const std::set<std::string> codes{
    "INPUT_PROFILE_UNSUPPORTED", "INPUT_SPEC_INVALID", "INPUT_SCOPE_DENIED",
    "INPUT_PRODUCER_MISMATCH", "INPUT_ENCODING_UNSUPPORTED", "INPUT_SET_INCOMPLETE",
    "INPUT_PROVENANCE_MISMATCH", "INPUT_SIZE_EXCEEDED", "INPUT_DIGEST_MISMATCH",
    "INPUT_CANCELLED", "INPUT_AUTHORITY_EXPIRED", "INPUT_DEADLINE", "INPUT_TRANSFER_FAILED",
    "INPUT_PACKET_PLAN_INVALID", "INPUT_PACKET_MISMATCH", "INPUT_DECRYPTION_REJECTED",
    "INPUT_DOMAIN_REJECTED", "INPUT_SIGNATURE_REJECTED", "INPUT_OVERLOADED", "INPUT_ADMISSION_REQUIRES_WORKER"};
  if (std::string_view(error.what()) == "DEPENDENCY_PACKET_LAYOUT_INVALID" ||
      std::string_view(error.what()) == "DEPENDENCY_REPAIR_INVALID")
    return "INPUT_PACKET_PLAN_INVALID";
  NDN_LOG_WARN("input admission first cause: " << error.what());
  return codes.count(error.what()) ? error.what() : fallback;
}
std::string orderedDigest(const std::vector<std::shared_ptr<const ndn::Data>>& packets)
{
  ndn::util::Sha256 digest;
  for (const auto& packet : packets) {
    const auto& wire = packet->wireEncode();
    digest << std::string(reinterpret_cast<const char*>(wire.data()), wire.size());
  }
  auto hex = digest.toString();
  std::transform(hex.begin(), hex.end(), hex.begin(), [](unsigned char c) { return std::tolower(c); });
  return "sha256:" + hex;
}

std::string plaintextDigest(const ndn::Buffer& content)
{
  ndn::util::Sha256 digest;
  if (!content.empty()) digest << std::string(reinterpret_cast<const char*>(content.data()), content.size());
  auto hex = digest.toString();
  std::transform(hex.begin(), hex.end(), hex.begin(), [](unsigned char c) { return std::tolower(c); });
  return "sha256:" + hex;
}

// Parsed native projections are retained while reads suspend. Charge their
// capacity, not just the deduplicated wire set. The multiplier also reserves
// the bounded merge/name lookup working copies used by one acquisition stage.
size_t projectionStorageCharge(const DependencyPacketPlan& plan, size_t limit)
{
  size_t bytes = 0;
  const auto add = [&](size_t count, size_t width = 1) {
    if (width == 0 || count > (limit - bytes) / width)
      throw std::runtime_error("INPUT_OVERLOADED");
    bytes += count * width;
  };
  const auto name = [&](const ndn::Name& value) {
    add(value.wireEncode().size(), 2);
    add(value.size(), 2 * sizeof(ndn::name::Component));
  };
  add(sizeof(DependencyPacketPlan) + 8 * sizeof(void*));
  add(plan.packets.capacity(), sizeof(DependencyPacket));
  add(plan.indexDataNames.capacity(), sizeof(ndn::Name));
  add(plan.expectedAggregateWireDigest.capacity());
  name(plan.rootName);
  name(plan.producer);
  for (const auto& packet : plan.packets) {
    name(packet.exactName);
    if (packet.wireSha256) add(packet.wireSha256->capacity());
  }
  for (const auto& index : plan.indexDataNames) name(index);
  if (bytes > limit / 4) throw std::runtime_error("INPUT_OVERLOADED");
  return 4 * bytes;
}

void validateSpecifications(const std::vector<CollaborationInputSpec>& inputs,
                           const ServiceProvider::CollaborationAssignment& assignment)
{
  if (assignment.inputAdmissionProfile != "data-admission-v1" || !assignment.admissionLimits)
    throw std::invalid_argument("INPUT_PROFILE_UNSUPPORTED");
  assignment.admissionLimits->validate();
  if (inputs.empty() || inputs.size() > assignment.admissionLimits->maxRetainedObjects)
    throw std::invalid_argument("INPUT_SPEC_INVALID");
  std::set<std::string> slots;
  for (const auto& input : inputs) {
    checkNamingNameSize(input.object.name);
    if (input.slotId.empty() || input.slotId.size() > 128 || !slots.insert(input.slotId).second ||
        input.object.producerName.empty() || input.object.name.size() <= input.object.producerName.size() ||
        !input.object.producerName.isPrefixOf(input.object.name) ||
        input.object.serviceName != assignment.service || input.maxBytes == 0 || input.maxPackets == 0 ||
        input.maxPackets > 65536 || input.maxBytes > assignment.admissionLimits->maxRetainedBytes ||
        input.object.sizeBytes > input.maxBytes || input.object.segmentCount > input.maxPackets)
      throw std::invalid_argument("INPUT_SPEC_INVALID");
    if (!assignment.scopeKeys.count(input.keyScope))
      throw std::invalid_argument("INPUT_SCOPE_DENIED");
    if (input.provenanceBinding == InputProvenanceBinding::SelectedRole) {
      if (input.producerRole.empty())
        throw std::invalid_argument("INPUT_SPEC_INVALID");
      const auto role = assignment.selectedRoleProviders.find(input.producerRole);
      if (role == assignment.selectedRoleProviders.end() ||
          std::find(role->second.begin(), role->second.end(), input.object.producerName) == role->second.end())
        throw std::invalid_argument("INPUT_PRODUCER_MISMATCH");
    }
    else if (input.provenanceBinding == InputProvenanceBinding::AuthorizedObject) {
      if (!input.producerRole.empty())
        throw std::invalid_argument("INPUT_SPEC_INVALID");
    }
    else
      throw std::invalid_argument("INPUT_SPEC_INVALID");
    const bool exact = input.encoding == InputObjectEncoding::SignedExactPackets;
    const bool versioned = input.object.name.at(-1).isVersion() && input.object.name.at(-1).toVersion() != 0;
    const bool protectedName = input.encoding == InputObjectEncoding::ImmutableProtected &&
      (ProtectedBlobManifest::inferProducer(input.object.name) == std::optional<ndn::Name>(input.object.producerName) ||
       ProtectedBlobManifest::inferDataProducer(input.object.name) == std::optional<ndn::Name>(input.object.producerName));
    if ((!exact && input.encoding != InputObjectEncoding::CollaborationLarge &&
         input.encoding != InputObjectEncoding::ReusableCollaborationObject &&
         input.encoding != InputObjectEncoding::ImmutableProtected) ||
        (exact ? input.digestKind != InputDigestKind::OrderedWireSha256 :
                 input.digestKind != InputDigestKind::PlaintextSha256) ||
        (!exact && !versioned && !protectedName))
      throw std::invalid_argument("INPUT_ENCODING_UNSUPPORTED");
  }
}
} // namespace


struct InputAdmissionOperation::Impl
{
  using FetchKey = std::tuple<KeyScope, ndn::Name, std::vector<ndn::Name>, bool>;
  std::promise<InputAdmissionResult> promise;
  std::future<InputAdmissionResult> future = promise.get_future();
  std::shared_ptr<std::atomic<bool>> cancelled = std::make_shared<std::atomic<bool>>(false);
  std::atomic<bool> completed{false};
  std::function<bool()> onFace;
  std::chrono::steady_clock::time_point deadline;
  std::mutex schedulerMutex;
  bool running = false;
  bool queued = false;
  bool wake = false;
  std::function<void()> schedule;
  std::function<std::optional<InputAdmissionResult>()> step;
  std::mutex metricsMutex;
  CollaborationTransferMetrics metrics;
  std::optional<FetchKey> activeKey;
  std::optional<VerifiedExactDataFetchOperation> activeRead;
  std::shared_ptr<void> readCapacity;
  std::map<FetchKey, VerifiedInputObject> fetched;
  std::map<FetchKey, std::shared_ptr<void>> controlRetentions;
  struct ImmutableEnvelope {
    ndn::ConstBufferPtr wire;
    std::string epoch;
  };
  std::map<std::string, ImmutableEnvelope> immutableEnvelopes;
  struct CachedPlan {
    DependencyPacketPlan plan;
    std::shared_ptr<void> retention;
  };
  std::map<std::pair<std::string, ndn::Name>, CachedPlan> packetPlans;
  struct FiniteRead {
    std::shared_ptr<detail::FinitePacketRecovery> recovery;
    std::shared_ptr<void> retention;
    size_t wireLimit = 0;
  };
  std::map<std::string, FiniteRead> finiteReads;
  std::map<std::string, VerifiedInputObject> decoded;
};

InputAdmissionOperation& InputAdmissionOperation::operator=(InputAdmissionOperation&& other) noexcept
{
  if (this != &other) { cancel(); m_impl = std::move(other.m_impl); }
  return *this;
}
InputAdmissionOperation::~InputAdmissionOperation() { cancel(); }
void InputAdmissionOperation::cancel() noexcept
{
  if (!m_impl) return;
  {
    std::lock_guard<std::mutex> lock(m_impl->schedulerMutex);
    if (m_impl->completed.load()) return;
    m_impl->cancelled->store(true);
  }
  try { if (m_impl->schedule) m_impl->schedule(); }
  catch (...) { /* Destruction requests cancellation; the owner still drains. */ }
}
std::future_status InputAdmissionOperation::wait_for(std::chrono::milliseconds duration) const
{
  if (!m_impl) throw std::logic_error("empty input admission operation");
  if (duration.count() > 0 && m_impl->onFace && m_impl->onFace())
    throw std::logic_error("INPUT_ADMISSION_REQUIRES_WORKER");
  return m_impl->future.wait_for(duration);
}
InputAdmissionResult InputAdmissionOperation::get()
{
  if (!m_impl) throw std::logic_error("empty input admission operation");
  if (m_impl->onFace && m_impl->onFace())
    throw std::logic_error("INPUT_ADMISSION_REQUIRES_WORKER");
  if (m_impl->future.wait_until(m_impl->deadline + std::chrono::milliseconds(50)) != std::future_status::ready &&
      m_impl->schedule) m_impl->schedule();
  return m_impl->future.get();
}

InputAdmissionResult ServiceProvider::CollaborationContext::admitResolvedInputs(
    const std::vector<CollaborationInputSpec>& inputs, std::vector<VerifiedInputObject> objects,
    const InputSemanticValidator& validate)
{
  return admitResolvedInputsImpl(inputs, std::move(objects), validate, {});
}

AdmittedInputSet ServiceProvider::CollaborationContext::requireInputs(
    std::vector<CollaborationInputSpec> inputs, InputSemanticValidator validate,
    DependencyPlanResolver resolve)
{
  const auto executor = m_provider.m_face.getIoContext().get_executor();
  if (executor.running_in_this_thread() || m_provider.m_fetchPool.isCurrentThread() ||
      !m_provider.m_handlerPool.isCurrentThread()) {
    throw std::logic_error("INPUT_ADMISSION_REQUIRES_WORKER");
  }

  auto operation = startInputAdmission(std::move(inputs), std::move(validate),
                                       std::move(resolve));
  auto result = operation.get();
  if (result.status != InputAdmissionStatus::Ready) {
    throw std::runtime_error(result.reasonCode.empty() ?
      "INPUT_ADMISSION_REJECTED" : result.reasonCode);
  }
  if (!result.inputs || !result.inputs->isCurrent()) {
    throw std::runtime_error("INPUT_AUTHORITY_EXPIRED");
  }
  return std::move(*result.inputs);
}

InputAdmissionResult ServiceProvider::CollaborationContext::admitResolvedInputsImpl(
    const std::vector<CollaborationInputSpec>& inputs, std::vector<VerifiedInputObject> objects,
    const InputSemanticValidator& validate, std::shared_ptr<void> retention)
{
  try {
    if (m_provider.m_face.getIoContext().get_executor().running_in_this_thread())
      throw std::logic_error("INPUT_ADMISSION_REQUIRES_WORKER");
    validateSpecifications(inputs, m_assignment);
    const auto current = currentWorkPredicate(m_assignment.workDeadline);
    if (!current()) return {InputAdmissionStatus::Rejected, "INPUT_AUTHORITY_EXPIRED", {}};
    if (!validate || objects.size() != inputs.size())
      throw std::invalid_argument("INPUT_SET_INCOMPLETE");
    size_t total = 0;
    size_t unretainedTotal = 0;
    size_t unretainedObjects = 0;
    for (size_t i = 0; i < inputs.size(); ++i) {
      const auto& input = inputs[i];
      const auto& object = objects[i];
      const auto binding = ndn::Name(m_requestId).append(m_assignment.service).append(m_assignment.selectionDigest)
        .append(m_assignment.role).append(input.keyScope).toUri();
      if (object.m_dispatchBinding != binding || !object.m_current || !object.m_current() ||
          object.name() != input.object.name || object.producer() != input.object.producerName ||
          !object.m_packets || object.m_packets->empty() || object.m_packets->size() > input.maxPackets ||
          (input.encoding == InputObjectEncoding::SignedExactPackets ? bool(object.m_content) : !object.m_content))
        throw std::invalid_argument("INPUT_PROVENANCE_MISMATCH");
      size_t bytes = 0;
      for (const auto& packet : *object.m_packets) {
        if (!object.producer().isPrefixOf(packet->getName()) ||
            packet->wireEncode().size() > input.maxBytes - std::min<uint64_t>(bytes, input.maxBytes))
          throw std::invalid_argument("INPUT_SIZE_EXCEEDED");
        bytes += packet->wireEncode().size();
      }
      const auto measuredBytes = object.m_content ? object.m_content->size() : bytes;
      const auto measuredDigest = object.m_content ? plaintextDigest(*object.m_content) : orderedDigest(*object.m_packets);
      const auto segmentCount = object.m_content ? std::count_if(object.m_packets->begin(), object.m_packets->end(),
        [](const auto& packet) { return packet->getName().at(-1).isSegment(); }) : object.m_packets->size();
      if (measuredBytes > input.maxBytes ||
          (input.object.sizeBytes && measuredBytes != input.object.sizeBytes) ||
          (input.object.segmentCount && segmentCount != input.object.segmentCount) ||
          (!input.object.digest.empty() && measuredDigest != input.object.digest))
        throw std::invalid_argument("INPUT_DIGEST_MISMATCH");
      if (bytes > m_assignment.admissionLimits->maxRetainedBytes - total)
        throw std::invalid_argument("INPUT_SIZE_EXCEEDED");
      total += bytes;
      if (object.m_content) {
        if (object.m_content->size() > m_assignment.admissionLimits->maxRetainedBytes - total)
          throw std::invalid_argument("INPUT_SIZE_EXCEEDED");
        total += object.m_content->size();
      }
      if (!object.m_retention) {
        ++unretainedObjects;
        unretainedTotal += bytes + (object.m_content ? object.m_content->size() : 0);
      }
    }
    const bool callerRetentionProvided = bool(retention);
    if (!retention && unretainedObjects != 0) {
      std::lock_guard<std::mutex> lock(m_provider.m_collaborationMutex);
      if (!m_provider.m_inputBudget) m_provider.m_inputBudget = std::make_shared<detail::CollaborationInputBudget>();
      retention = m_provider.m_inputBudget->reserve(m_assignment.service.toUri(),
        {unretainedObjects, unretainedTotal, 0, 0}, *m_assignment.admissionLimits);
      if (!retention) return {InputAdmissionStatus::Overloaded, "INPUT_OVERLOADED", {}};
    }
    if (retention) {
      for (auto& object : objects) {
        if (callerRetentionProvided || !object.m_retention) object.m_retention = retention;
      }
    }
    try { validate(objects); }
    catch (const std::exception& error) {
      NDN_LOG_WARN("input domain first cause: " << error.what());
      return {InputAdmissionStatus::Rejected, "INPUT_DOMAIN_REJECTED", {}};
    }
    catch (...) { return {InputAdmissionStatus::Rejected, "INPUT_DOMAIN_REJECTED", {}}; }
    if (!current()) return {InputAdmissionStatus::Rejected, "INPUT_AUTHORITY_EXPIRED", {}};
    std::vector<std::string> slotIds;
    slotIds.reserve(inputs.size());
    for (const auto& input : inputs) slotIds.push_back(input.slotId);
    AdmittedInputSet admitted(std::move(objects), std::move(slotIds), current);
    admitted.m_retention = std::move(retention);
    return {InputAdmissionStatus::Ready, {}, std::move(admitted)};
  }
  catch (const std::exception& error) {
    return {InputAdmissionStatus::Rejected, admissionErrorCode(error, "INPUT_OBJECT_MISMATCH"), {}};
  }
}

InputAdmissionOperation ServiceProvider::CollaborationContext::startInputAdmission(
    std::vector<CollaborationInputSpec> inputs, InputSemanticValidator validate,
    DependencyPlanResolver resolve,
    std::function<void(std::exception_ptr)> onFirstReadStarted,
    std::function<bool()> onValidatedData)
{
  auto state = std::make_shared<InputAdmissionOperation::Impl>();
  const auto traceRequestId = m_requestId;
  const bool traceInputAdmission = std::getenv("NDNSF_SPEC226_INPUT_TRACE") != nullptr;
  state->metrics.recoveryMode = m_assignment.dependencyTransport.redundancy ==
      DependencyRedundancy::Xor ? "xor-packet-set-v1" : "none";
  state->metrics.recoveryGroupDataPackets =
      m_assignment.dependencyTransport.redundancy == DependencyRedundancy::Xor ? 8 : 0;
  auto executor = m_provider.m_face.getIoContext().get_executor();
  state->onFace = [executor] { return executor.running_in_this_thread(); };
  const auto settle = [](const std::shared_ptr<InputAdmissionOperation::Impl>& owner, InputAdmissionResult result) {
    {
      std::lock_guard<std::mutex> lock(owner->schedulerMutex);
      if (owner->completed.load()) return;
      if (owner->cancelled->load())
        result = {InputAdmissionStatus::Cancelled, "INPUT_CANCELLED", {}};
      if (result.status != InputAdmissionStatus::Ready) owner->cancelled->store(true);
      owner->completed.store(true);
      owner->queued = false;
    }
    {
      std::lock_guard<std::mutex> lock(owner->metricsMutex);
      result.metrics = owner->metrics;
    }
    // Notify only after execution leases and cache owners have retired. A
    // copied result keeps its own retention lease, independent of this future.
    owner->step = {};
    owner->activeRead.reset();
    owner->activeKey.reset();
    owner->readCapacity.reset();
    owner->fetched.clear();
    owner->controlRetentions.clear();
    owner->immutableEnvelopes.clear();
    owner->packetPlans.clear();
    owner->finiteReads.clear();
    owner->decoded.clear();
    owner->promise.set_value(std::move(result));
  };
  const auto finish = [state, settle](InputAdmissionResult result) { settle(state, std::move(result)); };
  try { validateSpecifications(inputs, m_assignment); }
  catch (const std::exception& error) {
    finish({InputAdmissionStatus::Rejected, admissionErrorCode(error, "INPUT_SPEC_INVALID"), {}});
    return InputAdmissionOperation(state);
  }
  if (!currentWorkPredicate(m_assignment.workDeadline)()) {
    finish({InputAdmissionStatus::Rejected, "INPUT_AUTHORITY_EXPIRED", {}});
    return InputAdmissionOperation(state);
  }
  const auto hasUnsupportedExactCodec = std::any_of(inputs.begin(), inputs.end(),
    [this, &resolve](const auto& input) {
      if (input.encoding != InputObjectEncoding::SignedExactPackets || resolve) return false;
      return input.object.segmentCount != 1 ||
        m_assignment.dependencyTransport.redundancy != DependencyRedundancy::None;
    });
  if (!validate || hasUnsupportedExactCodec) {
    finish({InputAdmissionStatus::Rejected, "INPUT_CODEC_REQUIRED", {}});
    return InputAdmissionOperation(state);
  }
  const auto limits = *m_assignment.admissionLimits;
  size_t bytes = 0;
  for (const auto& input : inputs) {
    const size_t multiplier = input.encoding == InputObjectEncoding::SignedExactPackets ? 1 : 3;
    if (input.maxBytes > (limits.maxRetainedBytes - bytes) / multiplier) {
      finish({InputAdmissionStatus::Overloaded, "INPUT_OVERLOADED", {}});
      return InputAdmissionOperation(state);
    }
    bytes += multiplier * input.maxBytes;
  }
  std::shared_ptr<void> retention;
  std::shared_ptr<void> byteRetention;
  std::shared_ptr<void> execution;
  std::shared_ptr<detail::CollaborationInputBudget> budget;
  {
    std::lock_guard<std::mutex> lock(m_provider.m_collaborationMutex);
    if (!m_provider.m_inputBudget) m_provider.m_inputBudget = std::make_shared<detail::CollaborationInputBudget>();
    auto objectRetention = m_provider.m_inputBudget->reserve(m_assignment.service.toUri(),
      {inputs.size(), 0, 0, 0}, limits);
    byteRetention = m_provider.m_inputBudget->reserve(m_assignment.service.toUri(),
      {0, bytes, 0, 0}, limits);
    if (objectRetention && byteRetention)
      retention = std::make_shared<std::pair<std::shared_ptr<void>, std::shared_ptr<void>>>(
        std::move(objectRetention), byteRetention);
    execution = m_provider.m_inputBudget->reserve(m_assignment.service.toUri(),
      {0, 0, 1, 0}, limits);
    budget = m_provider.m_inputBudget;
  }
  if (!retention || !execution) {
    finish({InputAdmissionStatus::Overloaded, "INPUT_OVERLOADED", {}});
    return InputAdmissionOperation(state);
  }
  // Snapshot only dispatch authority and codec inputs, not assignment artifacts.
  CollaborationAssignment assignment;
  assignment.role = m_assignment.role;
  assignment.service = m_assignment.service;
  assignment.scopeKeys = m_assignment.scopeKeys;
  assignment.selectionDigest = m_assignment.selectionDigest;
  assignment.selectedRoleProviders = m_assignment.selectedRoleProviders;
  assignment.inputAdmissionProfile = m_assignment.inputAdmissionProfile;
  assignment.dependencyTransport = m_assignment.dependencyTransport;
  assignment.admissionLimits = m_assignment.admissionLimits;
  assignment.workDeadline = m_assignment.workDeadline;
  assignment.workCurrent = currentWorkPredicate(m_assignment.workDeadline);
  auto owned = std::make_shared<CollaborationContext>(m_provider, m_requesterName, m_requestId,
                                                     RequestMessage{}, std::move(assignment));
  const auto deadline = std::min(m_assignment.workDeadline,
    std::chrono::steady_clock::now() + std::chrono::milliseconds(limits.maxInputAcquisitionMs));
  state->deadline = deadline;
  const std::weak_ptr<InputAdmissionOperation::Impl> weakState(state);
  auto firstReadReported = std::make_shared<std::atomic<bool>>(false);
  auto reportFirstRead = [firstReadReported,
                          onFirstReadStarted = std::move(onFirstReadStarted)]
                         (std::exception_ptr error) noexcept {
    if (!onFirstReadStarted) return;
    bool expected = false;
    if (!firstReadReported->compare_exchange_strong(
          expected, true, std::memory_order_acq_rel)) return;
    try { onFirstReadStarted(std::move(error)); }
    catch (...) { /* A readiness observer cannot alter Core admission. */ }
  };
  auto reportValidatedData = [onValidatedData = std::move(onValidatedData)] {
    if (!onValidatedData) return true;
    try { return onValidatedData(); }
    catch (...) { return false; }
  };
  state->schedule = [weakState, settle, control = m_provider.m_registrationControl,
                     stopping = m_provider.m_fetchStopping, traceInputAdmission,
                     traceRequestId] {
    const auto owner = weakState.lock();
    if (!owner) return;
    {
      std::lock_guard<std::mutex> lock(owner->schedulerMutex);
      if (owner->completed.load()) return;
      if (owner->running) {
        owner->wake = true;
        if (traceInputAdmission)
          NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=INPUT_ADMISSION_WAKE_COALESCED"
                        << " requestId=" << traceRequestId.toUri());
        return;
      }
      if (owner->queued) return;
      owner->queued = true;
    }
    if (traceInputAdmission)
      NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=INPUT_ADMISSION_STEP_QUEUED"
                    << " requestId=" << traceRequestId.toUri());
    auto* provider = control->acquireOwner();
    if (!provider || stopping->load()) {
      if (provider) control->releaseOwner();
      settle(owner, {InputAdmissionStatus::Cancelled, "INPUT_CANCELLED", {}});
      return;
    }
    const auto lease = std::shared_ptr<void>(provider, [control](void*) { control->releaseOwner(); });
    if (!provider->m_fetchPool.post([owner, settle, traceInputAdmission, traceRequestId, lease] {
          {
            std::lock_guard<std::mutex> lock(owner->schedulerMutex);
            owner->queued = false;
            if (owner->completed.load()) return;
            owner->running = true;
            owner->wake = false;
          }
          if (traceInputAdmission)
            NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=INPUT_ADMISSION_STEP_STARTED"
                          << " requestId=" << traceRequestId.toUri());
          std::optional<InputAdmissionResult> result;
          try {
            // Copy the callable before invoking it; terminal settlement may
            // release the stored callable and all its captured owners.
            auto step = owner->step;
            result = step();
          }
          catch (const std::exception& error) {
            result = InputAdmissionResult{InputAdmissionStatus::Rejected,
              admissionErrorCode(error, "INPUT_TRANSFER_FAILED"), {}};
          }
          catch (...) { result = InputAdmissionResult{InputAdmissionStatus::Rejected, "INPUT_TRANSFER_FAILED", {}}; }
          if (result) {
            if (traceInputAdmission)
              NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=INPUT_ADMISSION_STEP_SETTLED"
                            << " requestId=" << traceRequestId.toUri()
                            << " status=" << static_cast<int>(result->status)
                            << " reason=" << result->reasonCode);
            settle(owner, std::move(*result));
            std::lock_guard<std::mutex> lock(owner->schedulerMutex);
            owner->running = false;
          }
          else {
            bool wake;
            {
              std::lock_guard<std::mutex> lock(owner->schedulerMutex);
              owner->running = false;
              wake = owner->wake;
              owner->wake = false;
            }
            if (traceInputAdmission)
              NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=INPUT_ADMISSION_STEP_SUSPENDED"
                            << " requestId=" << traceRequestId.toUri()
                            << " wakePending=" << (wake ? "true" : "false"));
            if (wake) owner->schedule();
          }
        })) settle(owner, {InputAdmissionStatus::Overloaded, "INPUT_OVERLOADED", {}});
  };
  struct AwaitRead {};
  state->step = [owned, weakState, budget, traceInputAdmission, traceRequestId,
                 inputs = std::move(inputs), validate = std::move(validate),
                 resolve = std::move(resolve), reportFirstRead, reportValidatedData,
                 retention = std::move(retention), byteRetention = std::move(byteRetention),
                 execution = std::move(execution), deadline, limits]() mutable -> std::optional<InputAdmissionResult> {
    const auto state = weakState.lock();
    if (!state) return InputAdmissionResult{InputAdmissionStatus::Cancelled, "INPUT_CANCELLED", {}};
    try {
      auto check = [&] {
        if (state->cancelled->load()) throw std::runtime_error("INPUT_CANCELLED");
        if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("INPUT_DEADLINE");
        if (!owned->currentWorkPredicate(owned->m_assignment.workDeadline)())
          throw std::runtime_error("INPUT_AUTHORITY_EXPIRED");
      };
      auto addMetrics = [state](const CollaborationTransferMetrics& source,
                                bool rootControl) {
        std::lock_guard<std::mutex> lock(state->metricsMutex);
        auto& target = state->metrics;
        const auto addOptional = [](auto& into, const auto& value) {
          if (value) into = into.value_or(0) + *value;
        };
        if (target.actualDataName.empty()) target.actualDataName = source.actualDataName;
        if (rootControl) {
          target.rootControlInterests += source.originalInterests;
          target.rootControlRetries += source.originalRetries;
        }
        else {
          target.originalInterests += source.originalInterests;
          target.originalRetries += source.originalRetries;
        }
        target.repairInterests += source.repairInterests;
        target.repairRetries += source.repairRetries;
        target.repairFetches += source.repairFetches;
        target.repairWireBytes += source.repairWireBytes;
        target.fecRecoveryAttempts += source.fecRecoveryAttempts;
        target.fecRecoveredPackets += source.fecRecoveredPackets;
        target.fecRejectedRepairs += source.fecRejectedRepairs;
        target.arqFallbacks += source.arqFallbacks;
        target.validatedOriginalWireBytes += source.validatedOriginalWireBytes;
        target.deadlineExpired += source.deadlineExpired;
        target.peakRetainedBytes = std::max(target.peakRetainedBytes,
                                             source.peakRetainedBytes);
        if (source.recoveryMode != "none") target.recoveryMode = source.recoveryMode;
        target.recoveryGroupDataPackets = std::max(target.recoveryGroupDataPackets,
                                                    source.recoveryGroupDataPackets);
        addOptional(target.transportPayloadBytes, source.transportPayloadBytes);
        addOptional(target.metadataBytes, source.metadataBytes);
        addOptional(target.wireBytes, source.wireBytes);
        addOptional(target.interestCount, source.interestCount);
        addOptional(target.retryCount, source.retryCount);
        addOptional(target.localCopyBytes, source.localCopyBytes);
        for (const auto& record : source.recovery) {
          const auto existing = std::find_if(target.recovery.begin(), target.recovery.end(),
              [&record](const auto& candidate) {
                return candidate.objectName == record.objectName &&
                  (candidate.trigger == record.trigger || candidate.trigger.empty() ||
                   record.trigger.empty());
              });
          if (existing == target.recovery.end()) target.recovery.push_back(record);
          else {
            if (existing->groupName.empty()) existing->groupName = record.groupName;
            if (existing->trigger.empty()) existing->trigger = record.trigger;
            if (!record.terminalState.empty()) existing->terminalState = record.terminalState;
            existing->missingPackets = std::max(existing->missingPackets,
                                                 record.missingPackets);
            existing->originalRetriesBeforeRecovery = std::max(
                existing->originalRetriesBeforeRecovery,
                record.originalRetriesBeforeRecovery);
            existing->recoveredBeforeOriginalRetry =
                existing->recoveredBeforeOriginalRetry || record.recoveredBeforeOriginalRetry;
          }
        }
      };
      auto recordRetainedBytes = [weakState, limits](std::size_t additionalBytes = 0) {
        const auto state = weakState.lock();
        if (!state) return;
        std::size_t retained = additionalBytes;
        std::set<const void*> owners;
        const auto add = [&retained](std::size_t bytes) {
          if (bytes > std::numeric_limits<std::size_t>::max() - retained)
            retained = std::numeric_limits<std::size_t>::max();
          else retained += bytes;
        };
        const auto addObject = [&owners, &add](const VerifiedInputObject& object) {
          if (object.m_packets)
            for (const auto& packet : *object.m_packets)
              if (packet && owners.insert(packet.get()).second) add(packet->wireEncode().size());
          if (object.m_content && owners.insert(object.m_content.get()).second)
            add(object.m_content->size());
        };
        for (const auto& [key, object] : state->fetched) {
          (void)key;
          addObject(object);
        }
        for (const auto& [slot, object] : state->decoded) {
          (void)slot;
          addObject(object);
        }
        for (const auto& [key, cached] : state->packetPlans) {
          (void)key;
          add(projectionStorageCharge(cached.plan, limits.maxRetainedBytes));
        }
        for (const auto& [slot, envelope] : state->immutableEnvelopes) {
          (void)slot;
          if (envelope.wire && owners.insert(envelope.wire.get()).second) add(envelope.wire->size());
        }
        std::lock_guard<std::mutex> lock(state->metricsMutex);
        state->metrics.peakRetainedBytes = std::max(state->metrics.peakRetainedBytes, retained);
      };
      auto fetch = [&](const CollaborationInputSpec& input, const std::vector<ndn::Name>& names,
                       size_t byteLimit, ExactDataWireRecovery recovery = {},
                       bool rootControl = false, bool hybridKeyBootstrap = false,
                       std::vector<size_t> optionalSlots = {},
                       ExactDataRecoveryReady recoveryReady = {}) -> VerifiedInputObject {
        check();
        const InputAdmissionOperation::Impl::FetchKey key{
          input.keyScope, input.object.producerName, names, hybridKeyBootstrap};
        auto cached = state->fetched.find(key);
        if (cached == state->fetched.end()) {
          if (state->activeRead) {
            if (!state->activeKey || *state->activeKey != key)
              throw std::logic_error("inconsistent input read continuation");
            if (state->activeRead->wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
              throw AwaitRead{};
            auto result = state->activeRead->get();
            const auto readMetrics = result ? result->metrics :
              state->activeRead->m_operation.metricsSnapshot();
            const bool signatureRejected = state->activeRead->m_signatureRejected &&
              state->activeRead->m_signatureRejected->load();
            state->activeRead.reset();
            state->activeKey.reset();
            state->readCapacity.reset();
            addMetrics(readMetrics, rootControl);
            check();
            if (!result) throw std::runtime_error(signatureRejected ? "INPUT_SIGNATURE_REJECTED" : "INPUT_TRANSFER_FAILED");
            result->object.m_retention = retention;
            cached = state->fetched.emplace(key, std::move(result->object)).first;
          }
          else {
            size_t window = std::min(limits.maxInFlightReads, names.size());
            for (; window > 0; --window) {
              state->readCapacity = budget->reserve(owned->m_assignment.service.toUri(), {0, 0, 0, window}, limits);
              if (state->readCapacity) break;
            }
            if (!window) throw std::runtime_error("INPUT_OVERLOADED");
            const auto timeout = std::max<int64_t>(1, std::chrono::duration_cast<std::chrono::milliseconds>(
              deadline - std::chrono::steady_clock::now()).count());
            state->activeKey = key;
            if (traceInputAdmission)
              NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=INPUT_ADMISSION_READ_START"
                            << " requestId=" << traceRequestId.toUri()
                            << " firstName=" << names.front().toUri()
                            << " nameCount=" << names.size()
                            << " rootControl=" << (rootControl ? "true" : "false")
                            << " timeoutMs=" << timeout);
            state->activeRead.emplace(owned->startVerifiedSignedExactDataBatchImpl(
              input.keyScope, input.producerRole, names, input.object.producerName,
              static_cast<int>(timeout),
              [cancelled = state->cancelled] { return cancelled->load(); }, reportFirstRead,
              // A resolved finite dependency names an already-published
              // immutable packet. Use the strict published-data retry cadence
              // rather than holding its first Interest until the whole
              // operation deadline as a future stream item would.
              ExactDataFetchPhase::Published, reportValidatedData, window, byteLimit,
              // Keep the byte lease through retiring exact-read callbacks,
              // including a canceled operation with an XOR worker in flight.
              [weakState, traceInputAdmission, traceRequestId, byteRetention] {
                (void)byteRetention;
                if (traceInputAdmission)
                  NDN_LOG_TRACE("[NDNSF_TRACE] role=provider event=INPUT_ADMISSION_READ_READY"
                                << " requestId=" << traceRequestId.toUri());
                if (const auto owner = weakState.lock()) owner->schedule();
              },
              std::move(recovery), hybridKeyBootstrap,
              std::move(optionalSlots), std::move(recoveryReady)));
            throw AwaitRead{};
          }
        }
        size_t bytes = 0;
        for (const auto& packet : *cached->second.m_packets) {
          if (packet->wireEncode().size() > byteLimit - std::min(bytes, byteLimit))
            throw std::runtime_error("INPUT_SIZE_EXCEEDED");
          bytes += packet->wireEncode().size();
        }
        recordRetainedBytes();
        return cached->second;
      };
      auto fetchControl = [&](const CollaborationInputSpec& input,
                              const std::vector<ndn::Name>& names,
                              size_t wireLimit, bool bootstrap = false) {
        if (names.empty() || names.size() > 128)
          throw std::runtime_error("INPUT_PACKET_PLAN_INVALID");
        wireLimit = std::min(wireLimit, names.size() * ndn::MAX_NDN_PACKET_SIZE);
        const InputAdmissionOperation::Impl::FetchKey key{
          input.keyScope, input.object.producerName, names, bootstrap};
        auto found = state->controlRetentions.find(key);
        if (found == state->controlRetentions.end()) {
          auto bytes = budget->reserve(owned->m_assignment.service.toUri(), {0, wireLimit, 0, 0}, limits);
          if (!bytes) throw std::runtime_error("INPUT_OVERLOADED");
          auto owners = std::make_shared<std::pair<std::shared_ptr<void>, std::shared_ptr<void>>>(
            byteRetention, std::move(bytes));
          found = state->controlRetentions.emplace(key, std::move(owners)).first;
        }
        auto result = fetch(input, names, wireLimit, {}, true, bootstrap);
        result.m_retention = found->second;
        recordRetainedBytes();
        return result;
      };
      auto fetchFinite = [&](const CollaborationInputSpec& input,
                             const detail::FinitePacketLayout& layout,
                             const std::vector<ndn::Name>& sources,
                             const std::map<ndn::Name, std::shared_ptr<const ndn::Data>>& bootstrap,
                             size_t sourceWireLimit) {
        auto cached = state->finiteReads.find(input.slotId);
        if (cached == state->finiteReads.end()) {
          if (layout.sourceCount > input.maxPackets)
            throw std::runtime_error("INPUT_SIZE_EXCEEDED");
          const size_t workingBytes = 3 * 64 * 1024 +
            3 * detail::DEPENDENCY_XOR_GROUP_SIZE * ndn::MAX_NDN_PACKET_SIZE;
          DependencyPacketPlan projection;
          projection.rootName = sources.front();
          projection.producer = input.object.producerName;
          for (const auto& name : sources) projection.packets.push_back({name, {}, {}});
          const auto charge = projectionStorageCharge(projection, limits.maxRetainedBytes);
          const auto repairCount = layout.totalPacketCount() - layout.sourceCount;
          const size_t repairSlotBytes = 2 * ndn::MAX_NDN_PACKET_SIZE +
            2 * (sizeof(ndn::Name) + sizeof(ndn::Buffer) + 4 * sizeof(size_t) +
                 layout.objectName.wireEncode().size() + 64);
          if (charge > limits.maxRetainedBytes ||
              workingBytes > limits.maxRetainedBytes - charge ||
              repairCount > (limits.maxRetainedBytes - charge - workingBytes) / repairSlotBytes)
            throw std::runtime_error("INPUT_OVERLOADED");
          auto lease = budget->reserve(owned->m_assignment.service.toUri(),
            {0, charge + workingBytes + repairCount * repairSlotBytes, 0, 0}, limits);
          if (!lease) throw std::runtime_error("INPUT_OVERLOADED");
          auto finite = std::make_shared<detail::FinitePacketRecovery>(
            layout, sources, bootstrap, input.object.producerName);
          cached = state->finiteReads.emplace(input.slotId,
            InputAdmissionOperation::Impl::FiniteRead{std::move(finite), std::move(lease),
              sourceWireLimit + repairCount * ndn::MAX_NDN_PACKET_SIZE}).first;
        }
        const auto finite = cached->second.recovery;
        const auto lease = cached->second.retention;
        ExactDataWireRecovery recover = [finite, lease](const ndn::Name& missing,
            const std::vector<ndn::Name>& names, const std::vector<ndn::Buffer>& wires) {
          return finite->recover(missing, names, wires);
        };
        ExactDataRecoveryReady ready = [finite, lease](size_t completed,
            const std::vector<ndn::Buffer>& wires, std::vector<size_t>& snapshot) {
          return finite->ready(completed, wires, snapshot);
        };
        return fetch(input, finite->requestedNames(), cached->second.wireLimit,
          std::move(recover), false, false, finite->optionalSlots(), std::move(ready));
      };
      std::vector<VerifiedInputObject> objects;
      objects.reserve(inputs.size());
      for (const auto& input : inputs) {
        const auto decoded = state->decoded.find(input.slotId);
        if (decoded != state->decoded.end()) {
          objects.push_back(decoded->second);
          continue;
        }
        if (input.encoding != InputObjectEncoding::SignedExactPackets) {
          std::optional<ProtectedBlobManifest> manifest;
          std::optional<VerifiedInputObject> index;
          size_t indexBytes = 0;
          ndn::Name dataPrefix(input.object.name);
          if (input.encoding == InputObjectEncoding::ImmutableProtected &&
              ProtectedBlobManifest::inferProducer(input.object.name)) {
            index = fetch(input, {input.object.name}, input.maxBytes, {}, true);
            const auto& packet = *index->m_packets->front();
            indexBytes = packet.wireEncode().size();
            manifest = ProtectedBlobManifest::decode(ndn::Buffer(packet.getContent().value_begin(),
              packet.getContent().value_end()), input.object.producerName, input.object.name);
            if (manifest->segmentCount >= input.maxPackets || manifest->envelopeBytes > input.maxBytes ||
                manifest->plaintextBytes > input.maxBytes || indexBytes >= input.maxBytes)
              throw std::runtime_error("INPUT_PACKET_PLAN_INVALID");
            dataPrefix = manifest->dataPrefix;
          }
          auto physical = input;
          physical.object.name = ndn::Name(dataPrefix).appendSegment(0);
          auto root = fetch(physical, {physical.object.name}, input.maxBytes - indexBytes, {}, true);
          const auto& first = *root.m_packets->front();
          const auto layout = detail::FinitePacketLayout::readFrom(
            first, dataPrefix, input.object.producerName);
          if (!first.getFinalBlock() || !first.getFinalBlock()->isSegment() ||
              (layout && layout->framing != detail::FinitePacketLayout::Framing::SEGMENTED))
            throw std::runtime_error("INPUT_PACKET_PLAN_INVALID");
          if (input.encoding == InputObjectEncoding::ReusableCollaborationObject &&
              owned->m_assignment.dependencyTransport.redundancy == DependencyRedundancy::Xor &&
              !layout)
            throw std::runtime_error("INPUT_PACKET_PLAN_INVALID");
          const auto count = layout ? layout->sourceCount : first.getFinalBlock()->toSegment() + 1;
          if (count > input.maxPackets - (manifest ? 1 : 0) ||
              (manifest && count != manifest->segmentCount))
            throw std::runtime_error("INPUT_PACKET_PLAN_INVALID");
          std::vector<ndn::Name> names;
          names.reserve(count - 1);
          for (uint64_t i = 1; i < count; ++i) names.push_back(ndn::Name(dataPrefix).appendSegment(i));
          auto packets = std::make_shared<std::vector<std::shared_ptr<const ndn::Data>>>();
          packets->reserve(count);
          packets->push_back(root.m_packets->front());
          if (!names.empty()) {
            const auto firstBytes = first.wireEncode().size();
            if (firstBytes >= input.maxBytes - indexBytes) throw std::runtime_error("INPUT_SIZE_EXCEEDED");
            auto rest = [&] {
              if (layout &&
                  owned->m_assignment.dependencyTransport.redundancy == DependencyRedundancy::Xor) {
                auto sourceNames = names;
                sourceNames.insert(sourceNames.begin(), first.getName());
                return fetchFinite(physical, *layout, sourceNames,
                  {{first.getName(), root.m_packets->front()}},
                  input.maxBytes - indexBytes - firstBytes);
              }
              return fetch(physical, names, input.maxBytes - indexBytes - firstBytes);
            }();
            packets->insert(packets->end(), rest.m_packets->begin(), rest.m_packets->end());
          }
          auto savedEnvelope = state->immutableEnvelopes.find(input.slotId);
          ndn::ConstBufferPtr encoded;
          if (savedEnvelope != state->immutableEnvelopes.end()) encoded = savedEnvelope->second.wire;
          size_t encodedBytes = 0;
          for (size_t segment = 0; segment < packets->size(); ++segment) {
            const auto& packet = packets->at(segment);
            if (packet->getFinalBlock() != first.getFinalBlock() ||
                packet->getContent().value_size() > input.maxBytes - encodedBytes ||
                (manifest && packet->getContent().value_size() != std::min<uint64_t>(manifest->segmentBytes,
                  manifest->envelopeBytes - segment * manifest->segmentBytes)))
              throw std::runtime_error("INPUT_PACKET_PLAN_INVALID");
            encodedBytes += packet->getContent().value_size();
          }
          if (manifest && encodedBytes != manifest->envelopeBytes)
            throw std::runtime_error("INPUT_PACKET_PLAN_INVALID");
          if (!encoded) {
            auto buffer = std::make_shared<ndn::Buffer>();
            buffer->reserve(encodedBytes);
            for (const auto& packet : *packets)
              buffer->insert(buffer->end(), packet->getContent().value_begin(), packet->getContent().value_end());
            encoded = std::move(buffer);
            if (input.encoding == InputObjectEncoding::ImmutableProtected) {
              HybridMessageEnvelope envelope;
              if (!envelope.WireDecode(ndn::Block(encoded)) ||
                  envelope.getMessageType() != "REQUEST-LARGE" || envelope.getEpochId().empty())
                throw std::runtime_error("INPUT_DECRYPTION_REJECTED");
              savedEnvelope = state->immutableEnvelopes.emplace(input.slotId,
                InputAdmissionOperation::Impl::ImmutableEnvelope{encoded, envelope.getEpochId()}).first;
              recordRetainedBytes();
            }
          }
          check();
          std::optional<ndn::Buffer> plaintext;
          if (input.encoding == InputObjectEncoding::CollaborationLarge)
            plaintext = detail::decryptCollaborationEnvelope(input.object.name, owned->m_requestId,
              input.keyScope, owned->m_assignment.scopeKeys.at(input.keyScope), std::move(encoded));
          else if (input.encoding == InputObjectEncoding::ReusableCollaborationObject) {
            const auto grantName = detail::makeCollaborationObjectGrantName(
              input.object.producerName, owned->m_assignment.service, input.keyScope,
              input.object.name, owned->m_requestId);
            auto grantInput = input;
            grantInput.object.name = grantName;
            grantInput.encoding = InputObjectEncoding::CollaborationLarge;
            const auto grantPacketName = ndn::Name(grantName).appendSegment(0);
            const auto grantObject = fetch(grantInput, {grantPacketName},
              ndn::MAX_NDN_PACKET_SIZE, {}, true);
            if (!grantObject.m_packets || grantObject.m_packets->size() != 1 ||
                grantObject.m_packets->front()->getName() != grantPacketName ||
                !grantObject.m_packets->front()->getFinalBlock() ||
                !grantObject.m_packets->front()->getFinalBlock()->isSegment() ||
                grantObject.m_packets->front()->getFinalBlock()->toSegment() != 0) {
              throw std::runtime_error("INPUT_DECRYPTION_REJECTED");
            }
            const auto& grantContent = grantObject.m_packets->front()->getContent();
            auto grantEncoded = std::make_shared<const ndn::Buffer>(
              grantContent.value_begin(), grantContent.value_end());
            auto grant = detail::decryptCollaborationEnvelope(grantName, owned->m_requestId,
              input.keyScope, owned->m_assignment.scopeKeys.at(input.keyScope), grantEncoded);
            if (!grant || grant->size() != 103) {
              throw std::runtime_error("INPUT_DECRYPTION_REJECTED");
            }
            ndn::Buffer objectKey(grant->begin(), grant->begin() + 32);
            const std::string expectedDigest(grant->begin() + 32, grant->end());
            if (expectedDigest.compare(0, 7, "sha256:") != 0 ||
                !std::all_of(expectedDigest.begin() + 7, expectedDigest.end(),
                  [] (unsigned char value) {
                    return (value >= '0' && value <= '9') ||
                           (value >= 'a' && value <= 'f');
                  })) {
              throw std::runtime_error("INPUT_DECRYPTION_REJECTED");
            }
            plaintext = detail::decryptReusableCollaborationEnvelope(
              input.object.name, owned->m_assignment.service, input.keyScope,
              objectKey, std::move(encoded));
            if (!plaintext || plaintextDigest(*plaintext) != expectedDigest) {
              throw std::runtime_error("INPUT_DECRYPTION_REJECTED");
            }
          }
          else {
            auto keyInput = physical;
            const auto locator = makeHybridMessageKeyDataName(owned->m_assignment.service,
              input.object.producerName, makeAbeServiceAttribute(owned->m_assignment.service, "SERVICE"),
              savedEnvelope->second.epoch);
            keyInput.object.name = locator;
            auto wrapperRoot = fetchControl(keyInput, {locator}, ndn::MAX_NDN_PACKET_SIZE, true);
            const auto& firstWrapper = *wrapperRoot.m_packets->front();
            const auto wrapperPrefix = firstWrapper.getName().getPrefix(-1);
            const auto wrapperFinal = firstWrapper.getFinalBlock()->toSegment();
            std::vector<ndn::Name> wrapperNames;
            for (uint64_t i = 0; i <= wrapperFinal; ++i) {
              if (i != firstWrapper.getName()[-1].toSegment())
                wrapperNames.push_back(ndn::Name(wrapperPrefix).appendSegment(i));
            }
            auto keys = std::make_shared<HybridMessageKeyOriginals>();
            auto keyRetentions = std::make_shared<std::vector<std::shared_ptr<void>>>();
            keyRetentions->push_back(wrapperRoot.m_retention);
            keys->wrappedPackets = *wrapperRoot.m_packets;
            if (!wrapperNames.empty()) {
              auto remaining = fetchControl(keyInput, wrapperNames,
                128 * ndn::MAX_NDN_PACKET_SIZE);
              keys->wrappedPackets.insert(keys->wrappedPackets.end(),
                remaining.m_packets->begin(), remaining.m_packets->end());
              keyRetentions->push_back(remaining.m_retention);
            }
            const auto cipher = decodeVerifiedHybridMessageKeyPackets(locator, keys->wrappedPackets);
            const ndn::Name ckRoot(cipher.second.get(ndn::tlv::Name));
            if (!detail::isNacCkLocator(ckRoot, input.object.producerName))
              throw std::runtime_error("INPUT_PACKET_PLAN_INVALID");
            keyInput.object.name = ndn::Name(ckRoot).appendSegment(0);
            auto firstCk = fetchControl(keyInput, {keyInput.object.name}, ndn::MAX_NDN_PACKET_SIZE);
            const auto& packet = *firstCk.m_packets->front();
            if (!packet.getFinalBlock() || !packet.getFinalBlock()->isSegment() ||
                packet.getFinalBlock()->toSegment() >= 32 ||
                *packet.getFinalBlock() != ndn::name::Component::fromSegment(
                  packet.getFinalBlock()->toSegment()))
              throw std::runtime_error("INPUT_PACKET_PLAN_INVALID");
            keys->ckPackets = *firstCk.m_packets;
            keyRetentions->push_back(firstCk.m_retention);
            std::vector<ndn::Name> ckNames;
            for (uint64_t i = 1; i <= packet.getFinalBlock()->toSegment(); ++i)
              ckNames.push_back(ndn::Name(ckRoot).appendSegment(i));
            if (!ckNames.empty()) {
              auto remaining = fetchControl(keyInput, ckNames, 64 * 1024);
              keys->ckPackets.insert(keys->ckPackets.end(),
                remaining.m_packets->begin(), remaining.m_packets->end());
              keyRetentions->push_back(remaining.m_retention);
            }
            keys->retention = std::move(keyRetentions);
            auto current = owned->currentWorkPredicate(deadline);
            auto result = owned->m_provider.fetchAndDecryptLargeDataUntil(input.object.name,
              owned->m_assignment.service.toUri(), deadline,
              [current, cancelled = state->cancelled] { return !cancelled->load() && current(); },
              DataNameReadMode::StrictCanonical, input.object.producerName, nullptr,
              std::move(encoded),
              manifest ? &*manifest : nullptr, std::move(keys));
            if (result.success) plaintext = std::move(result.plaintext);
          }
          check();
          if (!plaintext || plaintext->size() > input.maxBytes)
            throw std::runtime_error("INPUT_DECRYPTION_REJECTED");
          root.m_name = input.object.name;
          if (index) packets->insert(packets->begin(), index->m_packets->front());
          root.m_packets = std::move(packets);
          root.m_content = std::make_shared<const ndn::Buffer>(std::move(*plaintext));
          root.m_current = [authority = root.m_current, cancelled = state->cancelled] {
            return !cancelled->load() && authority();
          };
          root.m_retention = retention;
          state->decoded.emplace(input.slotId, root);
          state->immutableEnvelopes.erase(input.slotId);
          recordRetainedBytes();
          objects.push_back(std::move(root));
          continue;
        }
         auto root = fetch(input, {input.object.name}, input.maxBytes, {}, true);
        DependencyPacketPlan plan;
        std::map<ndn::Name, size_t> positions;
        std::set<ndn::Name> ancestors, expanded, indexNames;
        std::map<ndn::Name, std::shared_ptr<const ndn::Data>> found;
        found.emplace(input.object.name, root.packets().front());
        size_t retainedBytes = root.packets().front()->wireEncode().size();
        size_t wireLimit = input.maxBytes;
        size_t packetLimit = input.maxPackets;
        auto append = [&](const DependencyPacket& packet) {
          checkNamingNameSize(packet.exactName);
          if (!input.object.producerName.isPrefixOf(packet.exactName) ||
              packet.exactName.size() <= input.object.producerName.size())
            throw std::runtime_error("INPUT_PACKET_PLAN_INVALID");
          const auto existing = positions.find(packet.exactName);
          if (existing != positions.end()) {
            auto& previous = plan.packets.at(existing->second);
            if ((previous.wireLength && packet.wireLength && previous.wireLength != packet.wireLength) ||
                (previous.wireSha256 && packet.wireSha256 && previous.wireSha256 != packet.wireSha256))
              throw std::runtime_error("INPUT_PACKET_PLAN_INVALID");
            if (packet.wireLength) previous.wireLength = packet.wireLength;
            if (packet.wireSha256) previous.wireSha256 = packet.wireSha256;
            return;
          }
          if (plan.packets.size() >= packetLimit) throw std::runtime_error("INPUT_PACKET_PLAN_INVALID");
          positions.emplace(packet.exactName, plan.packets.size());
          plan.packets.push_back(packet);
        };
        std::function<void(const ndn::Name&, size_t)> expand;
        expand = [&](const ndn::Name& name, size_t depth) {
          check();
          if (depth > 4 || ancestors.count(name)) throw std::runtime_error("INPUT_PACKET_PLAN_INVALID");
          if (expanded.count(name)) return;
          if (expanded.size() >= packetLimit) throw std::runtime_error("INPUT_PACKET_PLAN_INVALID");
          ancestors.insert(name);
          auto original = found.find(name);
          if (original == found.end()) {
            if (retainedBytes >= wireLimit) throw std::runtime_error("INPUT_SIZE_EXCEEDED");
             auto index = fetch(input, {name}, wireLimit - retainedBytes, {}, true);
            original = found.emplace(name, index.packets().front()).first;
            retainedBytes += original->second->wireEncode().size();
          }
          const auto key = std::make_pair(input.slotId, name);
          auto cached = state->packetPlans.find(key);
          if (cached == state->packetPlans.end()) {
            DependencyPacketPlan projection;
            if (resolve) {
              try { projection = resolve(input, *original->second); }
              catch (const std::exception& error) {
                NDN_LOG_WARN("input codec first cause: " << error.what());
                throw std::runtime_error("INPUT_DOMAIN_REJECTED");
              }
              catch (...) { throw std::runtime_error("INPUT_DOMAIN_REJECTED"); }
            }
            else if (input.encoding == InputObjectEncoding::SignedExactPackets &&
                     input.object.segmentCount == 1 &&
                     owned->m_assignment.dependencyTransport.redundancy ==
                       DependencyRedundancy::None) {
              projection.rootName = input.object.name;
              projection.producer = input.object.producerName;
              projection.packets.push_back({input.object.name, {}, {}});
              projection.maxWireBytes = input.maxBytes;
              projection.maxPackets = input.maxPackets;
            }
            else {
              throw std::runtime_error("INPUT_CODEC_REQUIRED");
            }
            if (projection.rootName != input.object.name || projection.producer != input.object.producerName ||
                projection.packets.size() > packetLimit || projection.indexDataNames.size() > packetLimit)
              throw std::runtime_error("INPUT_PACKET_PLAN_INVALID");
            const auto charge = projectionStorageCharge(projection, limits.maxRetainedBytes);
            auto lease = budget->reserve(owned->m_assignment.service.toUri(), {0, charge, 0, 0}, limits);
            NDN_LOG_TRACE("input projection reservation slot=" << input.slotId << " bytes=" << charge
                          << " limit=" << limits.maxRetainedBytes << " accepted=" << bool(lease));
            if (!lease) throw std::runtime_error("INPUT_OVERLOADED");
            cached = state->packetPlans.emplace(key, InputAdmissionOperation::Impl::CachedPlan{
              std::move(projection), std::move(lease)}).first;
            recordRetainedBytes();
          }
          check();
          const auto& node = cached->second.plan;
          if (node.rootName != input.object.name || node.producer != input.object.producerName ||
              node.packets.size() > packetLimit || node.indexDataNames.size() > packetLimit ||
              node.maxPackets > packetLimit || node.maxWireBytes > wireLimit ||
              node.maxPlaintextBytes > input.maxBytes)
            throw std::runtime_error("INPUT_PACKET_PLAN_INVALID");
          if (depth == 0) {
            plan.rootName = node.rootName;
            plan.producer = node.producer;
            plan.expectedAggregateWireDigest = node.expectedAggregateWireDigest;
          }
          else if (!node.expectedAggregateWireDigest.empty() &&
                   node.expectedAggregateWireDigest != plan.expectedAggregateWireDigest)
            throw std::runtime_error("INPUT_PACKET_PLAN_INVALID");
          if (node.maxWireBytes) wireLimit = std::min<uint64_t>(wireLimit, node.maxWireBytes);
          if (node.maxPackets) packetLimit = std::min<uint64_t>(packetLimit, node.maxPackets);
          if (retainedBytes > wireLimit || plan.packets.size() > packetLimit)
            throw std::runtime_error("INPUT_SIZE_EXCEEDED");
          for (const auto& packet : node.packets) append(packet);
          std::set<ndn::Name> children;
          for (const auto& child : node.indexDataNames) {
            if (!children.insert(child).second) throw std::runtime_error("INPUT_PACKET_PLAN_INVALID");
            if (indexNames.insert(child).second) plan.indexDataNames.push_back(child);
            append({child, {}, {}});
            expand(child, depth + 1);
          }
          ancestors.erase(name);
          expanded.insert(name);
        };
        expand(input.object.name, 0);
        if (plan.packets.empty()) throw std::runtime_error("INPUT_PACKET_PLAN_INVALID");
        std::set<ndn::Name> unique;
        std::vector<ndn::Name> remaining;
        for (const auto& packet : plan.packets) {
          if (!plan.producer.isPrefixOf(packet.exactName) || packet.exactName.size() <= plan.producer.size() ||
              !unique.insert(packet.exactName).second)
            throw std::runtime_error("INPUT_PACKET_PLAN_INVALID");
          if (!found.count(packet.exactName)) remaining.push_back(packet.exactName);
        }
        if (!unique.count(input.object.name)) throw std::runtime_error("INPUT_PACKET_PLAN_INVALID");
        const bool builtInSingleton = !resolve &&
          input.encoding == InputObjectEncoding::SignedExactPackets &&
          input.object.segmentCount == 1 &&
          owned->m_assignment.dependencyTransport.redundancy == DependencyRedundancy::None;
        const auto inBandLayout = builtInSingleton ? std::optional<detail::FinitePacketLayout>{} :
          detail::FinitePacketLayout::readFrom(
            *root.packets().front(), input.object.name, plan.producer);
        if (inBandLayout &&
            (inBandLayout->framing != detail::FinitePacketLayout::Framing::NAMED_PACKETS ||
             inBandLayout->sourceCount != plan.packets.size() ||
             plan.packets.front().exactName != plan.rootName))
          throw std::runtime_error("INPUT_PACKET_PLAN_INVALID");
        auto packets = std::make_shared<std::vector<std::shared_ptr<const ndn::Data>>>();
        if (!remaining.empty()) {
          if (retainedBytes >= wireLimit) throw std::runtime_error("INPUT_SIZE_EXCEEDED");
          auto rest = [&] {
            if (inBandLayout &&
                owned->m_assignment.dependencyTransport.redundancy == DependencyRedundancy::Xor) {
              std::vector<ndn::Name> sourceNames;
              for (const auto& packet : plan.packets) sourceNames.push_back(packet.exactName);
              return fetchFinite(input, *inBandLayout, sourceNames, found, wireLimit - retainedBytes);
            }
            return fetch(input, remaining, wireLimit - retainedBytes);
          }();
          for (const auto& packet : rest.packets()) found.emplace(packet->getName(), packet);
        }
        size_t bytes = 0;
        for (const auto& descriptor : plan.packets) {
          auto packet = found.at(descriptor.exactName);
          if (packet->wireEncode().size() > wireLimit - std::min<uint64_t>(bytes, wireLimit) ||
              (descriptor.wireLength && *descriptor.wireLength != packet->wireEncode().size()) ||
              (descriptor.wireSha256 && *descriptor.wireSha256 != orderedDigest({packet})))
            throw std::runtime_error("INPUT_PACKET_MISMATCH");
          bytes += packet->wireEncode().size();
          packets->push_back(std::move(packet));
        }
        if (!plan.expectedAggregateWireDigest.empty() && orderedDigest(*packets) != plan.expectedAggregateWireDigest)
          throw std::runtime_error("INPUT_DIGEST_MISMATCH");
        root.m_packets = std::move(packets);
        root.m_current = [authority = root.m_current, cancelled = state->cancelled] {
          return !cancelled->load() && authority();
        };
        root.m_retention = retention;
        state->decoded.emplace(input.slotId, root);
        recordRetainedBytes();
        objects.push_back(std::move(root));
      }
      check();
      auto result = owned->admitResolvedInputsImpl(inputs, std::move(objects), validate, std::move(retention));
      check();
      execution.reset();
      return result;
    }
    catch (const AwaitRead&) { return std::nullopt; }
    catch (const std::exception& error) {
      const auto reason = admissionErrorCode(error, "INPUT_TRANSFER_FAILED");
      if (reason == "INPUT_DEADLINE") {
        std::lock_guard<std::mutex> lock(state->metricsMutex);
        state->metrics.deadlineExpired = 1;
      }
      const auto status = reason == "INPUT_CANCELLED" ? InputAdmissionStatus::Cancelled :
        reason == "INPUT_DEADLINE" ? InputAdmissionStatus::DeadlineExpired :
        reason == "INPUT_OVERLOADED" ? InputAdmissionStatus::Overloaded : InputAdmissionStatus::Rejected;
      execution.reset();
      return InputAdmissionResult{status, reason, {}};
    }
    catch (...) {
      execution.reset();
      return InputAdmissionResult{InputAdmissionStatus::Rejected, "INPUT_DOMAIN_REJECTED", {}};
    }
  };
  state->schedule();
  return InputAdmissionOperation(state);
}
} // namespace ndn_service_framework
