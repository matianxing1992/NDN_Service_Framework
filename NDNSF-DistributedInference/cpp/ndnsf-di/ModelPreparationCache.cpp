#include "NDNSF-DistributedInference/cpp/ndnsf-di/ModelPreparationCache.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestCatalog.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeV3Placement.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/Runtime.hpp"

#include <algorithm>
#include <atomic>
#include <deque>
#include <limits>
#include <stdexcept>
#include <utility>

namespace ndnsf::di {
namespace {

bool digest(const std::string& value)
{
  return value.size() == 71 && value.compare(0, 7, "sha256:") == 0 &&
    std::all_of(value.begin() + 7, value.end(), [] (unsigned char c) {
      return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

std::string requiredString(const NativeJson& object, const char* field)
{
  if (!object.is_object() || !object.contains(field) || !object.at(field).is_string() ||
      object.at(field).get<std::string>().empty())
    throw std::invalid_argument(std::string("preparation configuration requires ") + field);
  return object.at(field).get<std::string>();
}

void requireActive(std::chrono::steady_clock::time_point deadline,
                   const std::function<bool()>& cancelled = {})
{
  if (cancelled && cancelled())
    throw std::runtime_error("DI_NATIVE_PREPARATION_CANCELLED");
  if (std::chrono::steady_clock::now() >= deadline)
    throw std::runtime_error("DI_NATIVE_PREPARATION_TIMEOUT");
}

void validateSpecIdentity(const PreparationSpec& spec)
{
  if (spec.configurationJson.empty() || spec.catalogConfigurationJson.empty() ||
      spec.taskName.empty() || !digest(spec.taskContractDigest) ||
      !digest(spec.inputLayoutDigest) || !digest(spec.configurationDigest))
    throw std::invalid_argument("preparation identity is incomplete");

  const auto runtime = nativeParseJson(spec.configurationJson);
  const auto catalog = nativeParseJson(spec.catalogConfigurationJson);
  if (nativeCanonicalJson(runtime) != spec.configurationJson ||
      nativeCanonicalJson(catalog) != spec.catalogConfigurationJson)
    throw std::invalid_argument("preparation configuration is not canonical JSON");
  if (!runtime.is_object() || runtime.value("schema", std::string{}) !=
        "ndnsf-di-native-requester-v1" || !runtime.contains("catalog") ||
      !runtime.at("catalog").is_object())
    throw std::invalid_argument("preparation requester configuration is invalid");
  if (nativeCanonicalJson(runtime.at("catalog")) != spec.catalogConfigurationJson)
    throw std::invalid_argument("preparation catalog is detached from requester configuration");
  if (nativePlanningDigest(spec.configurationJson) != spec.configurationDigest)
    throw std::invalid_argument("preparation configuration digest differs from pinned identity");
  if (!runtime.contains("request") || !runtime.at("request").is_object())
    throw std::invalid_argument("preparation requester configuration is invalid");
  const auto& request = runtime.at("request");
  if (requiredString(request, "task") != spec.taskName ||
      requiredString(request, "task_descriptor_digest") != spec.taskContractDigest ||
      requiredString(request, "input_layout_digest") != spec.inputLayoutDigest)
    throw std::invalid_argument("preparation task contract differs from pinned configuration");
  if (!runtime.contains("limits") || !runtime.at("limits").is_object())
    throw std::invalid_argument("preparation requester limits are invalid");
  const auto& limits = runtime.at("limits");
  if (!limits.contains("max_source_bytes") || !limits.contains("max_assembled_bytes") ||
      !limits.at("max_source_bytes").is_number_unsigned() ||
      !limits.at("max_assembled_bytes").is_number_unsigned() ||
      limits.at("max_source_bytes").get<std::uint64_t>() != spec.maxSourceBytes ||
      limits.at("max_assembled_bytes").get<std::uint64_t>() != spec.maxAssembledBytes)
    throw std::invalid_argument("preparation limits differ from pinned configuration");
}

void addSize(std::size_t& total, std::size_t amount)
{
  if (amount > std::numeric_limits<std::size_t>::max() - total)
    throw std::runtime_error("DI_NATIVE_PREPARATION_SIZE_OVERFLOW");
  total += amount;
}

void addTensorSize(std::size_t& total, const NativeTensorContract& tensor)
{
  addSize(total, tensor.name.size());
  addSize(total, tensor.dtype.size());
  for (const auto& value : tensor.shape) {
    if (std::holds_alternative<std::string>(value))
      addSize(total, std::get<std::string>(value).size());
  }
}

void addGraphSize(std::size_t& total, const NativeGraphSnapshot& graph)
{
  addSize(total, graph.graphDigest.size());
  for (const auto& node : graph.nodes) {
    addSize(total, node.id.size());
    addSize(total, node.opType.size());
  }
  for (const auto& name : graph.topologicalOrder)
    addSize(total, name.size());
  for (const auto& name : graph.legalCutEdges)
    addSize(total, name.size());
  for (const auto& tensor : graph.modelInputs)
    addTensorSize(total, tensor);
  for (const auto& tensor : graph.modelOutputs)
    addTensorSize(total, tensor);
  for (const auto& edge : graph.edges) {
    addSize(total, edge.id.size());
    addSize(total, edge.producer.size());
    for (const auto& consumer : edge.consumers)
      addSize(total, consumer.size());
    addTensorSize(total, edge.tensor);
  }
}

std::string preparationIdentityDigest(const PreparationSpec& spec)
{
  auto runtime = nativeParseJson(spec.configurationJson);
  auto catalog = nativeParseJson(spec.catalogConfigurationJson);
  if (catalog.is_object() && catalog.contains("source") && catalog.at("source").is_object()) {
    // Local files are fetch locators, not protocol/model identity.  The
    // source and initializer content digests remain in this identity object.
    catalog["source"].erase("file");
    catalog["source"].erase("initializer_file");
  }
  if (runtime.is_object())
    runtime["catalog"] = catalog;
  const NativeJson identity{
    {"schema", "ndnsf-di-preparation-identity-v1"},
    {"runtime", runtime},
    {"catalog", catalog},
    {"task_name", spec.taskName},
    {"task_contract_digest", spec.taskContractDigest},
    {"input_layout_digest", spec.inputLayoutDigest},
    {"max_source_bytes", spec.maxSourceBytes},
    {"max_assembled_bytes", spec.maxAssembledBytes}};
  return nativePlanningDigest(nativeCanonicalJson(identity));
}

/** Owns a Core timer cancellation closure without holding the cache mutex. */
struct TimerCancellation
{
  std::mutex mutex;
  std::function<void()> cancelFn;
  bool cancelled = false;

  void set(std::function<void()> fn)
  {
    bool runNow = false;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (cancelled)
        runNow = true;
      else
        cancelFn = std::move(fn);
    }
    if (runNow && fn) {
      try { fn(); }
      catch (...) {}
    }
  }

  void cancel() noexcept
  {
    std::function<void()> fn;
    {
      std::lock_guard<std::mutex> lock(mutex);
      cancelled = true;
      fn = std::move(cancelFn);
    }
    if (fn) {
      try { fn(); }
      catch (...) {}
    }
  }

  ~TimerCancellation() noexcept { cancel(); }
};

/** Joins cache workers released by their own callback without detaching. */
class CacheJoinReaper
{
public:
  static CacheJoinReaper& instance()
  {
    // A cache worker may be the last owner of RuntimeState and therefore
    // destroy the cache while the process is already tearing down shared
    // libraries.  A function-local object would then be destroyed before a
    // late self-join handoff reaches enqueue(), leaving that worker with a
    // dangling reaper pointer.  Keep the reaper process-lived; its worker
    // thread and queue are reclaimed by process termination after all cache
    // workers have either joined or been handed off.
    static CacheJoinReaper* reaper = new CacheJoinReaper;
    return *reaper;
  }

  void enqueue(std::thread worker) noexcept
  {
    if (!worker.joinable())
      return;
    try {
      {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_workers.push_back(std::move(worker));
      }
      m_condition.notify_one();
    }
    catch (...) {
      std::terminate();
    }
  }

private:
  CacheJoinReaper()
    : m_thread([this] { run(); })
  {
  }

  ~CacheJoinReaper() noexcept
  {
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_stopping = true;
    }
    m_condition.notify_all();
    if (m_thread.joinable())
      m_thread.join();
  }

  void run() noexcept
  {
    for (;;) {
      std::thread worker;
      {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_condition.wait(lock, [this] { return m_stopping || !m_workers.empty(); });
        if (m_workers.empty() && m_stopping)
          return;
        worker = std::move(m_workers.front());
        m_workers.pop_front();
      }
      if (worker.joinable())
        worker.join();
    }
  }

  std::mutex m_mutex;
  std::condition_variable m_condition;
  std::deque<std::thread> m_workers;
  bool m_stopping = false;
  std::thread m_thread;
};

} // namespace

struct ModelPreparationCache::LeaseBook
{
  mutable std::mutex mutex;
  std::size_t retiredBytes = 0;
};

struct ModelPreparationCache::LeaseRecord
{
  std::shared_ptr<LeaseBook> book;
  std::size_t bytes = 0;
  std::size_t holders = 0;
  bool retired = false;
};

struct ModelPreparationCache::PreparationJob
  : std::enable_shared_from_this<ModelPreparationCache::PreparationJob>
{
  struct Completion
  {
    std::shared_ptr<ndn_service_framework::detail::SubscriptionControl> control;
    PreparationCompletion callback;
    std::shared_ptr<std::atomic<bool>> waiterCancelled;
    std::shared_ptr<std::atomic<bool>> deliveryGate;
    std::shared_ptr<std::atomic<bool>> deliveryOnce;
    std::shared_ptr<TimerCancellation> timer;
    std::function<void(std::function<void()>)> dispatch;
    bool joinedInFlight = false;
  };

  ModelPreparationCache* cache = nullptr;
  PreparationSpec spec;
  CachePolicy policy = CachePolicy::UseOrFetch;
  std::string key;
  std::chrono::milliseconds timeout{0};
  std::chrono::steady_clock::time_point deadline{};
  std::uint64_t generation = 0;
  mutable std::mutex mutex;
  std::condition_variable condition;
  PreparationStatus state = PreparationStatus::Pending;
  std::atomic<bool> pending{true};
  std::optional<PreparedModel> result;
  std::exception_ptr error;
  std::atomic<bool> cancelRequested{false};
  std::atomic<std::size_t> activeTimerCallbacks{0};
  std::size_t waiterCount = 0;
  std::vector<Completion> completions;
  std::vector<std::function<void()>> terminalHooks;
  std::mutex commitMutex;
};

ModelPreparationCache::ModelPreparationCache(std::size_t maxBytes,
                                             std::size_t maxEntries,
                                             std::chrono::milliseconds jobTimeout)
  : m_maxBytes(maxBytes), m_maxEntries(maxEntries), m_jobTimeout(jobTimeout),
    m_leaseBook(std::make_shared<LeaseBook>())
{
  if (!m_maxBytes || !m_maxEntries || m_jobTimeout.count() <= 0)
    throw std::invalid_argument("preparation cache limits must be positive");
}

ModelPreparationCache::~ModelPreparationCache() noexcept
{
  std::vector<WorkerRecord> workers;
  {
    std::lock_guard<std::mutex> lock(m_workerMutex);
    workers.swap(m_workers);
  }
  for (auto& worker : workers) {
    if (!worker.thread.joinable())
      continue;
    if (worker.thread.get_id() == std::this_thread::get_id())
      CacheJoinReaper::instance().enqueue(std::move(worker.thread));
    else
      worker.thread.join();
  }
}

std::shared_ptr<void> ModelPreparationCache::acquireLease(
  const std::shared_ptr<LeaseRecord>& lease)
{
  if (!lease)
    return {};
  // Allocate the owning token before publishing a holder.  Allocation
  // failure must not leave a lease book with an owner that does not exist.
  auto token = std::make_unique<std::uint8_t>(0);
  auto result = std::shared_ptr<void>(token.release(), [lease](void* value) {
    delete static_cast<std::uint8_t*>(value);
    std::lock_guard<std::mutex> lock(lease->book->mutex);
    if (lease->holders != 0)
      --lease->holders;
    if (lease->holders == 0 && lease->retired) {
      if (lease->book->retiredBytes >= lease->bytes)
        lease->book->retiredBytes -= lease->bytes;
      lease->retired = false;
    }
  });
  {
    std::lock_guard<std::mutex> lock(lease->book->mutex);
    ++lease->holders;
  }
  return result;
}

void ModelPreparationCache::retireLease(const std::shared_ptr<LeaseRecord>& lease)
{
  if (!lease)
    return;
  std::lock_guard<std::mutex> lock(lease->book->mutex);
  if (!lease->retired && lease->holders != 0) {
    lease->retired = true;
    addSize(lease->book->retiredBytes, lease->bytes);
  }
}

std::size_t ModelPreparationCache::retiredBytes() const noexcept
{
  std::lock_guard<std::mutex> lock(m_leaseBook->mutex);
  return m_leaseBook->retiredBytes;
}

std::string ModelPreparationCache::makePreparationKey(const PreparationSpec& spec)
{
  if (spec.taskName.empty() || !digest(spec.taskContractDigest) ||
      !digest(spec.inputLayoutDigest) || !digest(spec.configurationDigest))
    throw std::invalid_argument("preparation identity is incomplete");
  const NativeJson key{
    {"schema", "ndnsf-di-preparation-v1"},
    {"configuration_identity_digest", preparationIdentityDigest(spec)},
    {"task_name", spec.taskName},
    {"task_contract_digest", spec.taskContractDigest},
    {"input_layout_digest", spec.inputLayoutDigest}};
  return nativePlanningDigest(nativeCanonicalJson(key));
}

std::shared_ptr<const PreparedModelPackage> ModelPreparationCache::buildPackage(
  const PreparationSpec& spec, std::chrono::steady_clock::time_point deadline,
  PreparationSpec::MemorySnapshot* memorySnapshot) const
{
  if (!memorySnapshot)
    throw std::invalid_argument("preparation memory snapshot is missing");
  requireActive(deadline, spec.cancelled);
  if (spec.configurationJson.empty() || spec.catalogConfigurationJson.empty() ||
      spec.maxSourceBytes == 0 || spec.maxAssembledBytes == 0 || !spec.loadSource)
    throw std::invalid_argument("preparation source/configuration is incomplete");
  validateSpecIdentity(spec);

  auto& memory = *memorySnapshot;
  memory.ortPreparationBudgetBytes = spec.maxAssembledBytes;
  const auto updatePeak = [&] {
    std::size_t total = 0;
    const auto addSaturating = [&] (std::size_t value) {
      total = value > std::numeric_limits<std::size_t>::max() - total
        ? std::numeric_limits<std::size_t>::max() : total + value;
    };
    addSaturating(memory.sourceBytes);
    addSaturating(memory.initializerBytes);
    addSaturating(memory.materialBytes);
    addSaturating(memory.encryptedPublicationBytes);
    addSaturating(memory.ortPreparationBudgetBytes);
    memory.peakBytes = std::max(memory.peakBytes, total);
  };
  const auto runtime = nativeParseJson(spec.configurationJson);
  const auto& request = runtime.at("request");

  std::optional<NativePreparedCanonicalPublication> preparedPublication;
  if (spec.lookupPrepared) {
    preparedPublication = spec.lookupPrepared(spec, deadline);
    if (preparedPublication)
      preparedPublication->validate();
  }
  const bool publicationRepairRequired = preparedPublication &&
    !preparedPublication->missingDataNames.empty();
  NativeCanonicalSource source = spec.loadSource(spec, deadline);
  // A Repo hit normally carries a reference-only material index so a
  // complete hit can avoid material payload reads.  A partial publication is
  // different: the publisher must receive a complete manifest in order to
  // validate and repair the missing objects.  Let NativeRequestCatalog derive
  // that complete manifest from the bounded canonical source in this case.
  if (preparedPublication && preparedPublication->materialManifest &&
      (!publicationRepairRequired || preparedPublication->materialManifest->payloadsComplete))
    source.materialManifest = preparedPublication->materialManifest;
  requireActive(deadline, spec.cancelled);
  if (source.modelBytes.empty() || source.modelBytes.size() > spec.maxSourceBytes)
    throw std::invalid_argument("canonical model source is empty or exceeds its bound");
  if (source.initializerBytes && source.initializerBytes->size() > spec.maxSourceBytes)
    throw std::invalid_argument("canonical initializer exceeds its bound");
  memory.sourceBytes = source.modelBytes.size();
  memory.initializerBytes = source.initializerBytes ? source.initializerBytes->size() : 0;
  updatePeak();

  NativeAssemblyControl control{
    deadline,
    [deadline, cancelled = spec.cancelled] { requireActive(deadline, cancelled); },
    spec.maxSourceBytes,
    spec.maxAssembledBytes};
  auto catalog = NativeRequestCatalog::load(spec.catalogConfigurationJson,
                                            std::move(source), control);
  requireActive(deadline, spec.cancelled);
  if (!catalog.preparation || !catalog.splitter || !catalog.cooperativeSplitter)
    throw std::runtime_error("DI_NATIVE_PREPARATION_UNSUPPORTED_CAPABILITY");
  const auto& descriptor = catalog.model.descriptor;
  if (descriptor.modelName.empty() || catalog.model.canonicalSourceName.empty() ||
      !digest(catalog.model.canonicalSourceDigest) || !digest(catalog.model.modelManifestDigest) ||
      !digest(catalog.model.canonicalGraphDigest))
    throw std::invalid_argument("prepared model source identity is incomplete");

  // Keep the owning source view only for the pre-publication identity check.
  // Its scope must end before preparePublication() so the old full source is
  // not kept alive by a local shared_ptr while material bundles are published.
  {
    const auto preparedSource = catalog.preparation->sourceRefFor(descriptor);
    if (preparedSource->materialManifest) {
      for (const auto& payload : preparedSource->materialManifest->payloads)
        addSize(memory.materialBytes, payload.byteSize());
    }
    updatePeak();

    // Validate the canonical ONNX graph separately from the adapter's planning
    // graph.  This catches a semantic graph accidentally being used as source
    // identity and also validates any pinned initializer object.
    const auto sourceIdentity = inspectNativeOnnxSourceGraph(
      *preparedSource, descriptor, control);
    if (sourceIdentity.canonicalIdentity.graphDigest != catalog.model.canonicalGraphDigest)
      throw std::invalid_argument("prepared model canonical graph identity differs");
    if (catalog.model.canonicalInitializerBytes != 0 &&
        sourceIdentity.canonicalIdentity.initializerDigest != catalog.model.canonicalInitializerDigest)
      throw std::invalid_argument("prepared model initializer identity differs");
  }
  const auto adapter = catalog.preparation->adapters()->find(descriptor.adapterId);
  if (!adapter || adapter->adapterVersion() != descriptor.adapterVersion)
    throw std::runtime_error("DI_NATIVE_PREPARATION_ADAPTER_UNAVAILABLE");
  if (std::find(descriptor.adapter.tasks.begin(), descriptor.adapter.tasks.end(), spec.taskName) ==
      descriptor.adapter.tasks.end())
    throw std::runtime_error("DI_NATIVE_PREPARATION_UNSUPPORTED_CAPABILITY");

  const auto catalogDigest = nativePlanningDigest(spec.catalogConfigurationJson);
  const NativeJson catalogRoot = nativeParseJson(spec.catalogConfigurationJson);
  const NativeJson runtimeRoot = nativeParseJson(spec.configurationJson);
  const auto& catalogRecipe = catalogRoot.at("recipe");
  const auto artifactRoot = catalogRoot.at("publication").at("artifact_root").get<std::string>();
  auto serviceName = runtimeRoot.at("request").value("service", std::string{});
  if (serviceName.empty()) {
    const auto separator = artifactRoot.find('/', 1);
    serviceName = artifactRoot.substr(0, separator == std::string::npos ? artifactRoot.size() : separator);
  }
  NativeModelArtifactReference modelReference{
    artifactRoot,
    catalog.model.canonicalSourceName,
    catalog.model.modelManifestDigest,
    catalog.model.canonicalSourceDigest,
    catalog.model.canonicalSourceBytes,
    catalog.model.canonicalGraphDigest,
    catalogRecipe.at("protection_epoch").get<std::string>(),
    "/SERVICE" + serviceName,
    nativePlanningDigest(nativeCanonicalJson(catalogRecipe)),
    1};
  modelReference.validate();
  const NativeJson capabilityInput{{"schema", "native-input-v1"},
                                   {"digest", descriptor.adapter.inputSchemaDigest}};
  const NativeJson capabilityOutput{{"schema", "native-result-v1"},
                                    {"digest", descriptor.adapter.resultSchemaDigest}};
  const auto keyDigest = makePreparationKey(spec);
  const ModelManifest manifest{
    descriptor.modelName, descriptor.sourceRevision, descriptor.contentDigest,
    spec.taskName, catalog.model.canonicalGraphDigest, catalog.model.graph.graphDigest,
    catalogDigest, spec.taskContractDigest, keyDigest};
  ModelCapabilities capabilities;
  capabilities.inputSchemaJson = nativeCanonicalJson(capabilityInput);
  capabilities.outputSchemaJson = nativeCanonicalJson(capabilityOutput);
  capabilities.inputKinds = {"BYTES"};
  capabilities.outputModes = {"FULL"};
  capabilities.streaming = request.value("generation_mode", std::string{}) == "TOKEN_STREAMING";
  if (capabilities.streaming)
    capabilities.outputModes.push_back("TOKEN_STREAMING");
  // Runtime::open validates the operator-owned conversation section and binds
  // one coordinator.  The prepared package advertises the capability only
  // when that same frozen configuration carries the section; openConversation
  // still obtains the coordinator from the Runtime client before use.
  try {
    const auto root = nativeParseJson(spec.configurationJson);
    capabilities.conversations = root.contains("conversation") &&
      root.at("conversation").is_object();
  }
  catch (const std::exception& error) {
    throw std::runtime_error(std::string("DI_NATIVE_PREPARATION_CONFIGURATION_INVALID: ") + error.what());
  }

  std::size_t retained = 0;
  addSize(retained, catalog.model.canonicalSourceBytes);
  addSize(retained, catalog.model.canonicalInitializerBytes);
  addSize(retained, spec.configurationJson.size());
  addSize(retained, spec.catalogConfigurationJson.size());
  addSize(retained, catalog.model.descriptor.canonicalJson().size());
  addGraphSize(retained, catalog.model.graph);
  addSize(retained, catalog.model.canonicalSourceName.size());
  addSize(retained, catalog.model.canonicalSourceDigest.size());
  addSize(retained, catalog.model.modelManifestDigest.size());
  addSize(retained, catalog.model.canonicalGraphDigest.size());
  addSize(retained, catalog.model.canonicalInitializerObjectDigest.size());
  addSize(retained, catalog.model.canonicalInitializerDigest.size());
  addSize(retained, manifest.modelName.size() + manifest.modelRevision.size() +
                    manifest.modelDigest.size() + manifest.taskName.size() +
                    manifest.canonicalGraphDigest.size() + manifest.planningGraphDigest.size() +
                    manifest.catalogConfigurationDigest.size() + manifest.taskContractDigest.size() +
                    manifest.preparationKeyDigest.size());
  addSize(retained, capabilities.inputSchemaJson.size() + capabilities.outputSchemaJson.size());
  for (const auto& value : capabilities.inputKinds)
    addSize(retained, value.size());
  for (const auto& value : capabilities.outputModes)
    addSize(retained, value.size());
  addSize(retained, spec.baseDirectory.string().size() + spec.key.size() +
                    spec.configurationJson.size() + spec.configurationDigest.size() +
                    spec.taskName.size() + spec.taskContractDigest.size() + spec.inputLayoutDigest.size());
  // The package retains strategy/adapter state whose exact allocator footprint
  // is private to those implementations.  Charge the configured source and
  // assembly bounds plus parser/container overhead as a conservative budget
  // charge; this is deliberately larger than the serialized payload.
  if (spec.maxSourceBytes > std::numeric_limits<std::size_t>::max() ||
      spec.maxAssembledBytes > std::numeric_limits<std::size_t>::max())
    throw std::runtime_error("DI_NATIVE_PREPARATION_SIZE_OVERFLOW");
  // Publication is an external side effect.  Reject a package that cannot
  // fit before invoking the publisher, so a budget failure cannot leave
  // source/root objects orphaned after an otherwise successful commit.
  auto projectedRetained = retained;
  addSize(projectedRetained, static_cast<std::size_t>(spec.maxSourceBytes));
  addSize(projectedRetained, static_cast<std::size_t>(spec.maxAssembledBytes));
  addSize(projectedRetained, spec.configurationJson.size());
  addSize(projectedRetained, spec.catalogConfigurationJson.size());
  addSize(projectedRetained, std::size_t{256} * 1024);
  if (projectedRetained > m_maxBytes)
    throw std::runtime_error("DI_NATIVE_PREPARATION_CACHE_BUDGET_EXCEEDED");
  if (spec.cancelled && spec.cancelled())
    throw std::runtime_error("DI_NATIVE_PREPARATION_CANCELLED");
  const auto rollbackPublication = [&] {
    if (preparedPublication && spec.rollbackPublication) {
      try { spec.rollbackPublication(*preparedPublication); }
      catch (...) {}
    }
  };
  struct PublicationRollbackGuard
  {
    std::function<void()> rollback;
    bool committed = false;
    ~PublicationRollbackGuard() { if (!committed && rollback) rollback(); }
  } publicationGuard{rollbackPublication};
  if (publicationRepairRequired && !spec.preparePublication)
    throw std::runtime_error("DI_NATIVE_PREPARATION_PARTIAL_PUBLICATION_OWNER_MISSING");
  if ((!preparedPublication || publicationRepairRequired) && spec.preparePublication) {
    NativeRequestControl publicationControl{
      "prepare/" + spec.key, 1, deadline, spec.cancelled};
    preparedPublication = spec.preparePublication(*catalog.preparation,
                                                  catalog.model, publicationControl);
    publicationControl.requireActive();
    preparedPublication->validate();
    if (!preparedPublication->missingDataNames.empty())
      throw std::runtime_error("DI_NATIVE_PREPARATION_PARTIAL_PUBLICATION_UNRESOLVED");
    memory.encryptedPublicationBytes = preparedPublication->publishedBytes;
    updatePeak();
  }
  addSize(retained, static_cast<std::size_t>(spec.maxSourceBytes));
  addSize(retained, static_cast<std::size_t>(spec.maxAssembledBytes));
  addSize(retained, spec.configurationJson.size());
  addSize(retained, spec.catalogConfigurationJson.size());
  addSize(retained, std::size_t{256} * 1024);
  if (retained > m_maxBytes)
    throw std::runtime_error("DI_NATIVE_PREPARATION_CACHE_BUDGET_EXCEEDED");
  auto registration = std::make_shared<const FrozenPreparationRegistration>(
    FrozenPreparationRegistration{spec.key, spec.baseDirectory, spec.configurationJson,
      spec.configurationDigest, spec.taskName, spec.taskContractDigest, spec.inputLayoutDigest});
  const auto preparation = catalog.preparation;
  const bool releaseTransientSource = preparedPublication.has_value();
  auto package = std::make_shared<PreparedModelPackage>(
    PreparedModelPackage{std::move(catalog), std::move(registration), manifest,
                         std::move(capabilities), keyDigest, retained,
                         std::make_shared<NativePreSplitFirstPlacement>(),
                         spec.runtimeBinding, std::move(modelReference),
                         preparedPublication});
  // Catalog construction and prepare-time publication may transiently need
  // the canonical bytes. The published package carries only identity,
  // receipt and lease state; release the source owner before returning it so
  // eviction accounting cannot be bypassed by a hidden catalog reference.
  if (releaseTransientSource)
    preparation->releaseTransientSource();
  if (preparedPublication)
    memory.encryptedPublicationBytes = preparedPublication->publishedBytes;
  updatePeak();
  memory.sourceOwnerReleased = releaseTransientSource;
  publicationGuard.committed = true;
  return package;
}

PreparedModel ModelPreparationCache::prepareSingle(const PreparationSpec& spec,
                                             CachePolicy policy,
                                             std::chrono::steady_clock::time_point deadline)
{
  validateSpecIdentity(spec);
  const auto key = makePreparationKey(spec);
  if (deadline == std::chrono::steady_clock::time_point{})
    deadline = std::chrono::steady_clock::now() + m_jobTimeout;
  if (deadline <= std::chrono::steady_clock::now())
    throw std::runtime_error("DI_NATIVE_PREPARATION_TIMEOUT");

  {
    requireActive(deadline, spec.cancelled);
    std::shared_ptr<void> commit;
    if (spec.acquireCommit) {
      commit = spec.acquireCommit(deadline);
      if (!commit)
        throw std::runtime_error(std::chrono::steady_clock::now() >= deadline
          ? "DI_NATIVE_PREPARATION_TIMEOUT" : "DI_NATIVE_PREPARATION_CANCELLED");
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    auto found = m_entries.find(key);
    if (found != m_entries.end() && policy != CachePolicy::Refresh) {
      found->second.lastUse = m_nextUse++;
      auto lease = found->second.lease;
      PreparationReceipt receipt{PreparationReceipt::Origin::CacheHit,
        found->second.package->preparationKeyDigest,
        found->second.package->catalog.model.modelManifestDigest,
        std::chrono::milliseconds(0)};
      return PreparedModel(found->second.package, std::move(receipt), acquireLease(lease),
                           spec.clientFactory);
    }
    if (found == m_entries.end() && (policy == CachePolicy::RequireReady ||
                                     policy == CachePolicy::UseOrWait))
      throw std::runtime_error(policy == CachePolicy::UseOrWait
        ? "DI_NATIVE_PREPARATION_NOT_IN_FLIGHT" : "DI_NATIVE_MODEL_NOT_READY");
  }

  // The commit guard above may own a caller's Runtime mutex.  Release it
  // before invoking the cancellation probe, whose implementation may take
  // that same mutex (the Runtime User path does exactly this).
  requireActive(deadline, spec.cancelled);
  std::size_t reservation = 0;
  if (spec.maxSourceBytes > std::numeric_limits<std::size_t>::max() ||
      spec.maxAssembledBytes > std::numeric_limits<std::size_t>::max())
    throw std::runtime_error("DI_NATIVE_PREPARATION_SIZE_OVERFLOW");
  addSize(reservation, static_cast<std::size_t>(spec.maxSourceBytes));
  // The source contract permits a separate initializer file of the same
  // bounded size; reserve both inputs before staging the assembled package.
  addSize(reservation, static_cast<std::size_t>(spec.maxSourceBytes));
  addSize(reservation, static_cast<std::size_t>(spec.maxAssembledBytes));
  addSize(reservation, spec.configurationJson.size());
  addSize(reservation, spec.catalogConfigurationJson.size());
  // Parser and container allocations are not represented by serialized input
  // sizes.  Keep a conservative scratch allowance in the admission charge.
  addSize(reservation, 256u * 1024u);
  bool reservationHeld = false;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto retired = retiredBytes();
    if (retired > m_maxBytes || reservation > m_maxBytes - retired ||
        m_reservedBytes > m_maxBytes - retired - reservation)
      throw std::runtime_error("DI_NATIVE_PREPARATION_CACHE_BUDGET_EXCEEDED");
    const auto available = m_maxBytes - retired;
    // Evict candidates while holding the reservation mutex.  A separate
    // evictable-byte estimate is racy because another builder can pin an
    // entry before this reservation is recorded.
    for (;;) {
      const bool overBudget = m_chargedBytes > available ||
        m_reservedBytes > available - m_chargedBytes ||
        reservation > available - m_chargedBytes - m_reservedBytes;
      if (!overBudget)
        break;
      auto victim = m_entries.end();
      for (auto candidate = m_entries.begin(); candidate != m_entries.end(); ++candidate) {
        // A refresh retains the old package until its replacement commits.
        if (candidate->first == key)
          continue;
        bool pinned = false;
        if (candidate->second.lease) {
          std::lock_guard<std::mutex> leaseLock(candidate->second.lease->book->mutex);
          pinned = candidate->second.lease->holders != 0;
        }
        if (!pinned && (victim == m_entries.end() ||
                        candidate->second.lastUse < victim->second.lastUse))
          victim = candidate;
      }
      if (victim == m_entries.end() ||
          m_chargedBytes < victim->second.package->retainedBytes)
        throw std::runtime_error("DI_NATIVE_PREPARATION_CACHE_BUDGET_EXCEEDED");
      m_chargedBytes -= victim->second.package->retainedBytes;
      m_entries.erase(victim);
    }
    m_reservedBytes += reservation;
    reservationHeld = true;
    ++m_parseCount;
  }
  const auto started = std::chrono::steady_clock::now();
  PreparationSpec::MemorySnapshot memory;
  bool memoryCommitted = false;
  bool memoryObserved = false;
  const auto observeMemory = [&] () noexcept {
    if (memoryObserved)
      return;
    memoryObserved = true;
    memory.terminalCleanup = true;
    if (!memoryCommitted)
      memory.sourceOwnerReleased = true;
    if (spec.memoryObserver) {
      try { spec.memoryObserver(memory); }
      catch (...) {
        // Evidence callbacks cannot change preparation ownership or outcome.
      }
    }
  };
  struct MemoryObservationGuard
  {
    std::function<void()> observe;
    ~MemoryObservationGuard() noexcept { observe(); }
  } memoryObservationGuard{observeMemory};
  std::shared_ptr<const PreparedModelPackage> package;
  bool publicationCommitted = false;
  const auto rollbackPackagePublication = [&] {
    if (publicationCommitted || !package || !package->preparedPublication ||
        !spec.rollbackPublication)
      return;
    try { spec.rollbackPublication(*package->preparedPublication); }
    catch (...) {}
    memory.publicationRollbackAttempted = true;
  };
  try {
    package = buildPackage(spec, deadline, &memory);
    requireActive(deadline, spec.cancelled);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);
    std::shared_ptr<void> commit;
    if (spec.acquireCommit) {
      commit = spec.acquireCommit(deadline);
      if (!commit)
        throw std::runtime_error(std::chrono::steady_clock::now() >= deadline
          ? "DI_NATIVE_PREPARATION_TIMEOUT" : "DI_NATIVE_PREPARATION_CANCELLED");
    }
    std::unique_lock<std::mutex> lock(m_mutex);
    auto found = m_entries.find(key);
    if (spec.jobGeneration != 0 && found != m_entries.end() &&
        found->second.generation > spec.jobGeneration) {
      found->second.lastUse = m_nextUse++;
      auto lease = found->second.lease;
      m_reservedBytes -= reservation;
      reservationHeld = false;
      auto result = PreparedModel(found->second.package,
        PreparationReceipt{PreparationReceipt::Origin::CacheHit,
          found->second.package->preparationKeyDigest,
          found->second.package->catalog.model.modelManifestDigest,
          std::chrono::milliseconds(0)}, acquireLease(lease), spec.clientFactory);
      lock.unlock();
      rollbackPackagePublication();
      return result;
    }
    if (spec.jobGeneration != 0 && policy != CachePolicy::Refresh) {
      const auto jobs = m_jobs.find(key);
      if (jobs != m_jobs.end() && jobs->second.refresh &&
          jobs->second.refresh->generation > spec.jobGeneration) {
        m_reservedBytes -= reservation;
        reservationHeld = false;
        // This package is the valid result of the caller's own preparation.
        // A newer refresh may supersede the cache entry, but returning this
        // package after rolling back its publication would leave its receipt
        // unusable.  Keep the publication committed for this live result.
        const auto digest = package->catalog.model.modelManifestDigest;
        auto result = PreparedModel(package,
          PreparationReceipt{PreparationReceipt::Origin::Fetched, key, digest, elapsed}, {},
          spec.clientFactory);
        publicationCommitted = true;
        memoryCommitted = true;
        lock.unlock();
        return result;
      }
    }
    const auto prior = found == m_entries.end() ? std::size_t{0} : found->second.package->retainedBytes;
    if (!commit)
      requireActive(deadline, spec.cancelled);
    const auto otherReserved = m_reservedBytes - reservation;
    if (m_chargedBytes < prior || otherReserved > m_maxBytes)
      throw std::runtime_error("DI_NATIVE_PREPARATION_CACHE_BUDGET_EXCEEDED");
    auto futureRetired = retiredBytes();
    if (found != m_entries.end() && found->second.lease) {
      std::lock_guard<std::mutex> leaseLock(found->second.lease->book->mutex);
      if (found->second.lease->holders != 0) {
        if (prior > std::numeric_limits<std::size_t>::max() - futureRetired)
          throw std::runtime_error("DI_NATIVE_PREPARATION_SIZE_OVERFLOW");
        futureRetired += prior;
      }
    }
    if (futureRetired > m_maxBytes - otherReserved)
      throw std::runtime_error("DI_NATIVE_PREPARATION_CACHE_BUDGET_EXCEEDED");
    auto baseCharged = m_chargedBytes - prior;
    std::vector<std::string> victimKeys;
    const auto available = m_maxBytes - otherReserved - futureRetired;
    const auto needsEviction = [&] {
      const bool noSlot = found == m_entries.end() &&
        m_entries.size() - victimKeys.size() >= m_maxEntries;
      const bool noBytes = baseCharged > available ||
        package->retainedBytes > available - baseCharged;
      return noSlot || noBytes;
    };
    while (needsEviction()) {
      auto victim = m_entries.end();
      for (auto candidate = m_entries.begin(); candidate != m_entries.end(); ++candidate) {
        if (candidate->first == key ||
            std::find(victimKeys.begin(), victimKeys.end(), candidate->first) != victimKeys.end())
          continue;
        bool pinned = false;
        if (candidate->second.lease) {
          std::lock_guard<std::mutex> leaseLock(candidate->second.lease->book->mutex);
          pinned = candidate->second.lease->holders != 0;
        }
        if (!pinned && (victim == m_entries.end() ||
                        candidate->second.lastUse < victim->second.lastUse))
          victim = candidate;
      }
      if (victim == m_entries.end() || baseCharged < victim->second.package->retainedBytes)
        throw std::runtime_error("DI_NATIVE_PREPARATION_CACHE_BUDGET_EXCEEDED");
      victimKeys.push_back(victim->first);
      baseCharged -= victim->second.package->retainedBytes;
    }
    if (package->retainedBytes > available - baseCharged)
      throw std::runtime_error("DI_NATIVE_PREPARATION_CACHE_BUDGET_EXCEEDED");
    if (package->retainedBytes > std::numeric_limits<std::size_t>::max() - baseCharged)
      throw std::runtime_error("DI_NATIVE_PREPARATION_SIZE_OVERFLOW");
    const auto newCharged = baseCharged + package->retainedBytes;
    const auto generation = spec.jobGeneration != 0 ? spec.jobGeneration : m_nextGeneration;
    auto lease = std::make_shared<LeaseRecord>(LeaseRecord{m_leaseBook, package->retainedBytes});
    if (found != m_entries.end())
      retireLease(found->second.lease);
    if (found != m_entries.end()) {
      found->second.package = package;
      found->second.generation = generation;
      found->second.lease = lease;
      found->second.lastUse = m_nextUse++;
    }
    else {
      m_entries.emplace(key, Entry{package, generation, lease, m_nextUse++});
    }
    for (const auto& victimKey : victimKeys)
      m_entries.erase(victimKey);
    if (m_nextGeneration <= generation)
      m_nextGeneration = generation + 1;
    m_chargedBytes = newCharged;
    m_reservedBytes -= reservation;
    reservationHeld = false;
    // The package is now owned by the cache.  Any later allocation failure
    // while constructing the return receipt must not roll back its durable
    // publication and leave the cached package unusable.
    publicationCommitted = true;
    memoryCommitted = true;
    memory.cacheEntryCommitted = true;
    const auto origin = policy == CachePolicy::Refresh
      ? PreparationReceipt::Origin::Refreshed : PreparationReceipt::Origin::Fetched;
    const auto manifestDigest = package->catalog.model.modelManifestDigest;
    return PreparedModel(std::move(package), PreparationReceipt{origin, key,
      manifestDigest, elapsed}, acquireLease(lease), spec.clientFactory);
  }
  catch (...) {
    rollbackPackagePublication();
    if (reservationHeld) {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_reservedBytes -= reservation;
      reservationHeld = false;
    }
    throw;
  }
}

void ModelPreparationCache::finishJob(const std::shared_ptr<PreparationJob>& job,
                                      std::optional<PreparedModel> result,
                                      std::exception_ptr error)
{
  struct Delivery
  {
    // The timer/finish gate arbitrates timeout against terminal completion;
    // this separate gate arbitrates the callback when a dispatcher both queues
    // work and reports an exception.
    std::shared_ptr<std::atomic<bool>> once;
    std::shared_ptr<ndn_service_framework::detail::SubscriptionControl> control;
    std::shared_ptr<PreparationCompletion> callback;
    std::exception_ptr error;
    std::optional<PreparedModel> value;
    std::function<void()> releaseTerminal;

    void run() noexcept
    {
      if (once->exchange(true, std::memory_order_acq_rel))
        return;
      try { (*callback)(error, std::move(value)); }
      catch (...) {}
      control->end(true);
      releaseTerminal();
    }
  };
  std::vector<PreparationJob::Completion> completions;
  std::vector<std::function<void()>> terminalHooks;
  // Allocate the terminal barrier before publishing READY/FAILED.  Once the
  // state is visible, no allocation failure may strand the ticket or hooks.
  auto hooks = std::make_shared<std::vector<std::function<void()>>>();
  auto remaining = std::make_shared<std::atomic<std::size_t>>(1);
  std::unique_lock<std::mutex> serial(job->commitMutex);
  {
    std::lock_guard<std::mutex> lock(job->mutex);
    if (job->state != PreparationStatus::Pending)
      return;
    if (error) {
      job->error = error;
      job->state = PreparationStatus::Failed;
    }
    else {
      job->result = std::move(result);
      job->state = PreparationStatus::Ready;
    }
    job->pending.store(false, std::memory_order_release);
    completions.swap(job->completions);
    terminalHooks.swap(job->terminalHooks);
    hooks->swap(terminalHooks);
  }
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto found = m_jobs.find(job->key);
    if (found != m_jobs.end()) {
      auto& slot = found->second;
      if (job->policy == CachePolicy::Refresh && slot.refresh == job)
        slot.refresh.reset();
      if (job->policy != CachePolicy::Refresh && slot.normal == job)
        slot.normal.reset();
      if (!slot.normal && !slot.refresh)
        m_jobs.erase(found);
    }
  }
  job->condition.notify_all();
  const auto releaseTerminal = [hooks, remaining] {
    if (remaining->fetch_sub(1, std::memory_order_acq_rel) != 1)
      return;
    for (auto& hook : *hooks) {
      if (hook) {
        try { hook(); }
        catch (...) {}
      }
    }
    hooks->clear();
  };
  // The completion callbacks are part of the Runtime drain barrier.  Do not
  // release the preparation ticket until every queued callback has returned.
  serial.unlock();
  for (auto& completion : completions) {
    if (completion.timer)
      completion.timer->cancel();
  }
  // A timer may have claimed its gate while still executing user code.  Its
  // callback is part of the completion barrier even though it is no longer in
  // the pending-completion vector.
  {
    std::unique_lock<std::mutex> lock(job->mutex);
    job->condition.wait(lock, [&] {
      return job->activeTimerCallbacks.load(std::memory_order_acquire) == 0;
    });
  }
  for (auto& completion : completions) {
    if ((completion.deliveryGate &&
         completion.deliveryGate->exchange(true, std::memory_order_acq_rel)) ||
        (completion.waiterCancelled &&
         completion.waiterCancelled->load(std::memory_order_acquire)) ||
        !completion.callback || !completion.control->begin())
      continue;
    std::optional<PreparedModel> value;
    auto deliveryError = error;
    try {
      std::lock_guard<std::mutex> lock(job->mutex);
      if (job->result)
        value = *job->result;
      if (value && completion.joinedInFlight)
        value->m_receipt.origin = PreparationReceipt::Origin::JoinedInFlight;
    }
    catch (...) {
      // A value copy is part of delivery and may allocate.  Keep finishJob
      // terminal and continue notifying all remaining waiters if it fails.
      value.reset();
      deliveryError = std::current_exception();
    }
    auto deliveryValue = std::move(value);
    std::shared_ptr<PreparationCompletion> callbackHolder;
    std::shared_ptr<Delivery> delivery;
    try {
      // Copy the callback into an immutable shared payload.  The same payload
      // can be used by a failure delivery if dispatch both queues and throws.
      callbackHolder = std::make_shared<PreparationCompletion>(completion.callback);
      delivery = std::make_shared<Delivery>(
        Delivery{completion.deliveryOnce, completion.control, callbackHolder,
                 deliveryError, std::move(deliveryValue), releaseTerminal});
    }
    catch (...) {
      // A completion allocation failure must still finish the one-shot
      // subscription and report an error; it must not leak the terminal hook.
      try {
        const auto failure = std::current_exception();
        if (callbackHolder)
          (*callbackHolder)(failure, std::nullopt);
        else
          completion.callback(failure, std::nullopt);
      }
      catch (...) {}
      completion.control->end(true);
      continue;
    }
    remaining->fetch_add(1, std::memory_order_acq_rel);
    auto task = [delivery] { delivery->run(); };
    try {
      if (completion.dispatch)
        completion.dispatch(std::move(task));
      else
        task();
    }
    catch (...) {
      // Dispatch failure gets an immutable failure payload.  It shares the
      // callback once-gate with the original delivery, so a dispatcher that
      // queued before throwing cannot race a mutation of its payload or invoke
      // the callback twice.
      std::shared_ptr<Delivery> failure;
      try {
        failure = std::make_shared<Delivery>(
          Delivery{completion.deliveryOnce, completion.control, callbackHolder,
                   std::current_exception(), std::nullopt, releaseTerminal});
      }
      catch (...) {
        // If even the fallback object cannot be allocated, deliver the
        // original immutable payload; this still closes the subscription and
        // releases the terminal barrier exactly once.
        delivery->run();
        continue;
      }
      failure->run();
    }
  }
  releaseTerminal();
}

void ModelPreparationCache::runJob(const std::shared_ptr<PreparationJob>& job)
{
  try {
    const auto originalCancelled = job->spec.cancelled;
    const auto originalCommit = job->spec.acquireCommit;
    const std::weak_ptr<PreparationJob> weakJob = job;
    job->spec.cancelled = [weakJob, originalCancelled] {
      const auto job = weakJob.lock();
      if (!job)
        return true;
      return job->cancelRequested.load(std::memory_order_acquire) ||
        (originalCancelled && originalCancelled());
    };
    job->spec.acquireCommit = [weakJob, originalCommit](std::chrono::steady_clock::time_point deadline) {
    const auto job = weakJob.lock();
    if (!job)
      return std::shared_ptr<void>{};
    using Guard = std::unique_lock<std::mutex>;
    auto serial = std::make_shared<Guard>(job->commitMutex, std::defer_lock);
    while (!serial->try_lock()) {
      if (std::chrono::steady_clock::now() >= deadline)
        return std::shared_ptr<void>{};
      std::this_thread::yield();
    }
    if (job->cancelRequested.load(std::memory_order_acquire))
      return std::shared_ptr<void>{};
    auto owner = originalCommit ? originalCommit(deadline) : std::shared_ptr<void>{};
    if (originalCommit && !owner)
      return std::shared_ptr<void>{};
    struct CommitHold
    {
      std::shared_ptr<Guard> serial;
      std::shared_ptr<void> owner;
    };
    return std::shared_ptr<void>(new CommitHold{std::move(serial), std::move(owner)},
                                 [] (void* value) { delete static_cast<CommitHold*>(value); });
    };
    auto prepared = prepareSingle(job->spec, job->policy, job->deadline);
    finishJob(job, std::move(prepared), {});
  }
  catch (...) {
    finishJob(job, std::nullopt, std::current_exception());
  }
}

std::shared_ptr<PreparationHandle::State>
ModelPreparationCache::prepareAsync(const PreparationSpec& spec,
                                    CachePolicy policy,
                                    std::chrono::milliseconds timeout)
{
  validateSpecIdentity(spec);
  const auto key = makePreparationKey(spec);
  const auto effective = timeout.count() == 0 ? m_jobTimeout : timeout;
  if (effective.count() <= 0)
    throw std::invalid_argument("preparation timeout must be positive");

  auto job = std::make_shared<PreparationJob>();
  bool startJob = false;
  bool joinedInFlight = false;
  bool slotCreated = false;
  bool slotAssigned = false;
  std::function<void()> terminalNow;
  auto waiterTerminal = spec.onTerminal;
  const auto rollbackUnstartedJob = [this, job, key, policy,
                                     &slotCreated, &slotAssigned] {
    if (!slotCreated && !slotAssigned)
      return;
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto found = m_jobs.find(key);
    if (found == m_jobs.end())
      return;
    auto& slot = found->second;
    if (slotAssigned) {
      if (policy == CachePolicy::Refresh && slot.refresh == job)
        slot.refresh.reset();
      if (policy != CachePolicy::Refresh && slot.normal == job)
        slot.normal.reset();
    }
    if ((slotCreated || slotAssigned) && !slot.normal && !slot.refresh)
      m_jobs.erase(found);
  };
  try {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto found = m_entries.find(key);
    if (found != m_entries.end() && policy != CachePolicy::Refresh) {
      found->second.lastUse = m_nextUse++;
      job->key = key;
      job->policy = policy;
      job->state = PreparationStatus::Ready;
      job->pending.store(false, std::memory_order_release);
      job->result = PreparedModel(found->second.package,
        PreparationReceipt{PreparationReceipt::Origin::CacheHit,
          found->second.package->preparationKeyDigest,
          found->second.package->catalog.model.modelManifestDigest,
          std::chrono::milliseconds(0)}, acquireLease(found->second.lease), spec.clientFactory);
      terminalNow = std::move(waiterTerminal);
    }
    else if (found == m_entries.end() && policy == CachePolicy::RequireReady) {
      job->key = key;
      job->policy = policy;
      job->state = PreparationStatus::Failed;
      job->pending.store(false, std::memory_order_release);
      try { throw std::runtime_error("DI_NATIVE_MODEL_NOT_READY"); }
      catch (...) { job->error = std::current_exception(); }
      terminalNow = std::move(waiterTerminal);
    }
    else {
      auto slot = m_jobs.find(key);
      std::shared_ptr<PreparationJob> selected;
      if (slot != m_jobs.end())
        selected = policy == CachePolicy::Refresh ? slot->second.refresh : slot->second.normal;
      if (selected) {
        job = selected;
        joinedInFlight = true;
      }
      else if (found == m_entries.end() && policy == CachePolicy::UseOrWait) {
        job->key = key;
        job->policy = policy;
        job->state = PreparationStatus::Failed;
        job->pending.store(false, std::memory_order_release);
        try { throw std::runtime_error("DI_NATIVE_PREPARATION_NOT_IN_FLIGHT"); }
        catch (...) { job->error = std::current_exception(); }
        terminalNow = std::move(waiterTerminal);
      }
      else {
        auto slotResult = m_jobs.try_emplace(key);
        slotCreated = slotResult.second;
        auto& newSlot = slotResult.first->second;
        job->cache = this;
        job->spec = spec;
        job->policy = policy;
        job->key = key;
        // The job deadline is shared by all waiters.  Their requested timeout
        // remains a local result-wait timeout on the returned State.
        job->timeout = m_jobTimeout;
        job->deadline = std::chrono::steady_clock::now() + m_jobTimeout;
        job->generation = m_nextJobGeneration++;
        job->spec.jobGeneration = job->generation;
        if (waiterTerminal) {
          job->terminalHooks.push_back(waiterTerminal);
          waiterTerminal = {};
        }
        (policy == CachePolicy::Refresh ? newSlot.refresh : newSlot.normal) = job;
        slotAssigned = true;
        startJob = true;
      }
    }
  }
  catch (...) {
    rollbackUnstartedJob();
    throw;
  }

  try {
    if (waiterTerminal) {
      std::lock_guard<std::mutex> lock(job->mutex);
      if (job->state == PreparationStatus::Pending)
        job->terminalHooks.push_back(std::move(waiterTerminal));
      else
        terminalNow = std::move(waiterTerminal);
    }
  }
  catch (...) {
    rollbackUnstartedJob();
    throw;
  }
  if (terminalNow) {
    try { terminalNow(); }
    catch (...) {}
  }

  std::shared_ptr<std::atomic<bool>> cancelled;
  std::function<void(std::function<void()>)> waiterDispatch;
  std::shared_ptr<PreparationHandle::State> state;
  bool waiterRegistered = false;
  try {
    cancelled = std::make_shared<std::atomic<bool>>(false);
    waiterDispatch = spec.dispatch;
    state = std::make_shared<PreparationHandle::State>();
    {
      std::lock_guard<std::mutex> lock(job->mutex);
      ++job->waiterCount;
      waiterRegistered = true;
    }
  }
  catch (...) {
    if (waiterRegistered) {
      std::lock_guard<std::mutex> lock(job->mutex);
      if (job->state == PreparationStatus::Pending && job->waiterCount != 0) {
        --job->waiterCount;
        if (job->waiterCount == 0)
          job->cancelRequested.store(true, std::memory_order_release);
      }
    }
    rollbackUnstartedJob();
    job->condition.notify_all();
    throw;
  }
  try {
    state->defaultTimeout = effective;
    state->waiterDeadline = std::chrono::steady_clock::now() + effective;
    state->joinedInFlight = joinedInFlight;
    const auto cancelWaiter = [job, cancelled] {
    std::unique_lock<std::mutex> serial(job->commitMutex);
    if (cancelled->load(std::memory_order_acquire))
      return;
    bool removeJob = false;
    std::vector<std::shared_ptr<TimerCancellation>> timers;
    {
      std::lock_guard<std::mutex> lock(job->mutex);
      if (job->state != PreparationStatus::Pending)
        return;
      cancelled->store(true, std::memory_order_release);
      if (job->waiterCount != 0)
        --job->waiterCount;
      if (job->waiterCount == 0) {
        job->cancelRequested.store(true, std::memory_order_release);
        removeJob = true;
      }
      job->completions.erase(std::remove_if(job->completions.begin(), job->completions.end(),
        [&] (const auto& item) {
          if (item.waiterCancelled != cancelled)
            return false;
          if (item.timer)
            timers.push_back(item.timer);
          return true;
        }), job->completions.end());
    }
    serial.unlock();
    job->condition.notify_all();
    for (auto& timer : timers)
      timer->cancel();
    if (removeJob) {
      std::lock_guard<std::mutex> lock(job->cache->m_mutex);
      const auto found = job->cache->m_jobs.find(job->key);
      if (found != job->cache->m_jobs.end()) {
        auto& slot = found->second;
        if (job->policy == CachePolicy::Refresh && slot.refresh == job)
          slot.refresh.reset();
        if (job->policy != CachePolicy::Refresh && slot.normal == job)
          slot.normal.reset();
        if (!slot.normal && !slot.refresh)
          job->cache->m_jobs.erase(found);
      }
    }
    };
    state->cancel = cancelWaiter;
    state->onDestroy = cancelWaiter;
    waiterRegistered = false;
  }
  catch (...) {
    bool removeJob = false;
    if (waiterRegistered) {
      {
        std::lock_guard<std::mutex> lock(job->mutex);
        if (job->state == PreparationStatus::Pending && job->waiterCount != 0) {
          --job->waiterCount;
          if (job->waiterCount == 0) {
            job->cancelRequested.store(true, std::memory_order_release);
            removeJob = true;
          }
        }
      }
      if (removeJob)
        rollbackUnstartedJob();
    }
    job->condition.notify_all();
    throw;
  }
  state->status = [job, cancelled] {
    std::lock_guard<std::mutex> lock(job->mutex);
    if (cancelled->load(std::memory_order_acquire))
      return PreparationStatus::Cancelled;
    return job->state;
  };
  state->wouldBlock = [job] {
    return job->pending.load(std::memory_order_acquire);
  };
  const auto waitFor = [job, cancelled, joinedInFlight](
    std::chrono::steady_clock::time_point deadline) {
    std::unique_lock<std::mutex> lock(job->mutex);
    while (job->state == PreparationStatus::Pending) {
      if (cancelled->load(std::memory_order_acquire))
        throw std::runtime_error("DI_NATIVE_PREPARATION_CANCELLED");
      if (!job->condition.wait_until(lock, deadline, [&] {
        return job->state != PreparationStatus::Pending ||
          cancelled->load(std::memory_order_acquire);
      }))
        throw std::runtime_error("DI_NATIVE_PREPARATION_TIMEOUT");
    }
    if (cancelled->load(std::memory_order_acquire))
      throw std::runtime_error("DI_NATIVE_PREPARATION_CANCELLED");
    if (job->error) {
      // Do not rethrow the job-owned exception object directly.  A synchronous
      // caller may release the last PreparationHandle/Job reference while its
      // exception predicate is still reading what(); copy the diagnostic into
      // an independent exception before leaving this wait function.
      std::string message;
      try {
        std::rethrow_exception(job->error);
      }
      catch (const DiError& error) {
        throw DiError(error.code(), error.domain(), error.boundary(), error.what(),
                      error.requestId(), error.attempt());
      }
      catch (const ndn_service_framework::OperationError& error) {
        throw ndn_service_framework::OperationError(error.code(), error.what());
      }
      catch (const std::exception& error) {
        message = error.what();
      }
      catch (...) {
        message = "native preparation failed with a non-standard exception";
      }
      throw std::runtime_error(std::move(message));
    }
    if (!job->result)
      throw std::runtime_error("DI_NATIVE_PREPARATION_FAILED");
    auto value = *job->result;
    if (joinedInFlight)
      value.m_receipt.origin = PreparationReceipt::Origin::JoinedInFlight;
    return value;
  };
  state->result = [waitFor](std::chrono::milliseconds wait) {
    if (wait.count() < 0)
      throw std::invalid_argument("preparation result timeout must not be negative");
    return waitFor(std::chrono::steady_clock::now() + wait);
  };
  state->resultDefault = [waitFor, deadline = state->waiterDeadline] {
    return waitFor(deadline);
  };

  const auto addCompletion = [job, cancelled, joinedInFlight, waiterDispatch](PreparationCompletion callback) {
    if (!callback)
      throw std::invalid_argument("preparation completion callback is empty");
    auto control = std::make_shared<ndn_service_framework::detail::SubscriptionControl>();
    auto gate = std::make_shared<std::atomic<bool>>(false);
    bool immediate = false;
    std::exception_ptr error;
    std::optional<PreparedModel> value;
    {
      std::lock_guard<std::mutex> lock(job->mutex);
      if (cancelled->load(std::memory_order_acquire))
        throw std::runtime_error("DI_NATIVE_PREPARATION_CANCELLED");
      if (job->state == PreparationStatus::Pending) {
        if (job->completions.size() >= 64)
          throw std::runtime_error("DI_NATIVE_SUBSCRIPTION_LIMIT");
        const std::weak_ptr<ndn_service_framework::detail::SubscriptionControl> weakControl = control;
        const std::weak_ptr<PreparationJob> weakJob = job;
        control->cancelFn = [weakJob, weakControl] {
          const auto job = weakJob.lock();
          const auto control = weakControl.lock();
          if (!job || !control)
            return;
          std::lock_guard<std::mutex> lock(job->mutex);
          job->completions.erase(std::remove_if(job->completions.begin(), job->completions.end(),
            [&control] (const auto& item) { return item.control == control; }), job->completions.end());
        };
        job->completions.push_back({control, std::move(callback), cancelled, gate,
                                    std::make_shared<std::atomic<bool>>(false), {},
                                    waiterDispatch, joinedInFlight});
      }
      else {
        immediate = true;
        error = job->error;
        if (job->result)
          value = *job->result;
        if (value && joinedInFlight)
          value->m_receipt.origin = PreparationReceipt::Origin::JoinedInFlight;
      }
    }
    if (immediate) {
      auto deliver = [control, cancelled, error, value = std::move(value),
                      callback = std::move(callback)]() mutable {
        if (cancelled->load(std::memory_order_acquire) || !control->begin())
          return;
        try { callback(error, std::move(value)); }
        catch (...) {}
        control->end(true);
      };
      if (waiterDispatch)
        waiterDispatch(std::move(deliver));
      else
        deliver();
    }
    return ndn_service_framework::OperationSubscription::fromControl(std::move(control));
  };
  state->onCompletion = addCompletion;
  state->resultAsync = [job, cancelled, joinedInFlight, waiterDispatch](std::chrono::milliseconds wait,
                                                        PreparationCompletion callback) {
    if (wait.count() < 0)
      throw std::invalid_argument("preparation result timeout must not be negative");
    if (!callback)
      throw std::invalid_argument("preparation result callback is empty");
    auto gate = std::make_shared<std::atomic<bool>>(false);
    auto callbackHolder = std::make_shared<PreparationCompletion>(std::move(callback));
    auto control = std::make_shared<ndn_service_framework::detail::SubscriptionControl>();
    auto timer = std::make_shared<TimerCancellation>();
    const std::weak_ptr<ndn_service_framework::detail::SubscriptionControl> weakControl = control;
    const std::weak_ptr<PreparationJob> weakJob = job;
    control->cancelFn = [weakJob, weakControl] {
      const auto job = weakJob.lock();
      const auto control = weakControl.lock();
      if (!job || !control)
        return;
      std::lock_guard<std::mutex> lock(job->mutex);
      job->completions.erase(std::remove_if(job->completions.begin(), job->completions.end(),
        [&control] (const auto& item) { return item.control == control; }), job->completions.end());
    };
    auto completion = [callbackHolder](
      std::exception_ptr error, std::optional<PreparedModel> value) mutable {
      try { (*callbackHolder)(error, std::move(value)); }
      catch (...) {}
    };
    bool immediate = false;
    std::exception_ptr immediateError;
    std::optional<PreparedModel> immediateValue;
    {
      std::lock_guard<std::mutex> lock(job->mutex);
      if (cancelled->load(std::memory_order_acquire))
        throw std::runtime_error("DI_NATIVE_PREPARATION_CANCELLED");
      if (job->state == PreparationStatus::Pending) {
        if (wait.count() == 0) {
          immediate = true;
          immediateError = std::make_exception_ptr(
            std::runtime_error("DI_NATIVE_PREPARATION_TIMEOUT"));
        }
        else {
          if (job->completions.size() >= 64)
            throw std::runtime_error("DI_NATIVE_SUBSCRIPTION_LIMIT");
          job->completions.push_back({control, std::move(completion), cancelled,
                                      gate, std::make_shared<std::atomic<bool>>(false),
                                      timer, waiterDispatch, joinedInFlight});
        }
      }
      else {
        immediate = true;
        immediateError = job->error;
        if (job->result)
          immediateValue = *job->result;
        if (immediateValue && joinedInFlight)
          immediateValue->m_receipt.origin = PreparationReceipt::Origin::JoinedInFlight;
      }
    }
    if (immediate) {
      auto deliver = [control, cancelled, completion = std::move(completion),
                      gate, immediateError, immediateValue = std::move(immediateValue)]() mutable {
        if (cancelled->load(std::memory_order_acquire) || !control->begin())
          return;
        if (!gate->exchange(true, std::memory_order_acq_rel))
          completion(immediateError, std::move(immediateValue));
        control->end(true);
      };
      if (waiterDispatch)
        waiterDispatch(std::move(deliver));
      else
        deliver();
      return ndn_service_framework::OperationSubscription::fromControl(std::move(control));
    }
    if (wait.count() == 0)
      return ndn_service_framework::OperationSubscription::fromControl(std::move(control));
    if (job->spec.schedule) {
      const auto deadline = std::chrono::steady_clock::now() + wait;
      std::function<void()> cancelTimer;
      try {
        cancelTimer = job->spec.schedule(deadline,
          [job, cancelled, gate, control, callbackHolder] {
            job->activeTimerCallbacks.fetch_add(1, std::memory_order_acq_rel);
            struct ActiveTimerCallback
            {
              std::shared_ptr<PreparationJob> job;
              ~ActiveTimerCallback() noexcept
              {
                job->activeTimerCallbacks.fetch_sub(1, std::memory_order_acq_rel);
                job->condition.notify_all();
              }
            } active{job};
            bool invoke = false;
            bool cancelledBeforeDelivery = false;
            {
              // Claim the one-shot delivery under the commit mutex, then
              // release it before entering user code.  The callback may
              // legally re-enter cancel()/resultAsync().
              std::unique_lock<std::mutex> serial(job->commitMutex);
              if (!gate->exchange(true, std::memory_order_acq_rel)) {
                cancelledBeforeDelivery = cancelled->load(std::memory_order_acquire);
                if (!cancelledBeforeDelivery)
                  invoke = control->begin();
              }
            }
            if (cancelledBeforeDelivery) {
              control->cancel();
              return;
            }
            if (invoke) {
              try { throw std::runtime_error("DI_NATIVE_PREPARATION_RESULT_TIMEOUT"); }
              catch (...) {
                try { (*callbackHolder)(std::current_exception(), std::nullopt); }
                catch (...) {}
              }
              control->end(true);
            }
            {
              std::lock_guard<std::mutex> lock(job->mutex);
              job->completions.erase(std::remove_if(job->completions.begin(),
                job->completions.end(), [&control] (const auto& item) {
                  return item.control == control;
                }), job->completions.end());
            }
            control->cancel();
          });
      }
      catch (...) {
        control->cancel();
        timer->cancel();
        throw;
      }
      timer->set(std::move(cancelTimer));
    }
    return ndn_service_framework::OperationSubscription::fromControl(std::move(control));
  };

  if (startJob) {
    auto done = std::make_shared<std::atomic<bool>>(false);
    auto execute = [this, job, done] {
      try { runJob(job); }
      catch (...) {
        // runJob normally converts every failure to a terminal job.  Keep the
        // worker boundary noexcept if an allocation or unexpected callback
        // escapes that conversion so the reaper can still reclaim the thread.
      }
      done->store(true, std::memory_order_release);
    };
    // Core dispatch is reserved for lightweight callback delivery.  Source
    // reads, parsing and assembly run on a cache-owned worker so a Runtime
    // IO/notification executor is never blocked by preparation work.
    try {
      std::lock_guard<std::mutex> lock(m_workerMutex);
      for (auto it = m_workers.begin(); it != m_workers.end();) {
        if (!it->done->load(std::memory_order_acquire) ||
            !it->thread.joinable() ||
            it->thread.get_id() == std::this_thread::get_id()) {
          ++it;
          continue;
        }
        it->thread.join();
        it = m_workers.erase(it);
      }
      // Reserve while holding the worker mutex, before starting the thread.
      // A vector growth failure must never leave a temporary joinable thread
      // whose moved-from local is destroyed during stack unwinding.
      m_workers.reserve(m_workers.size() + 1);
      auto worker = std::thread(std::move(execute));
      WorkerRecord record{std::move(worker), std::move(done)};
      try {
        m_workers.emplace_back(std::move(record));
      }
      catch (...) {
        if (record.thread.joinable())
          record.thread.join();
        throw;
      }
    }
    catch (...) { finishJob(job, std::nullopt, std::current_exception()); }
  }
  return state;
}

PreparedModel ModelPreparationCache::prepare(const PreparationSpec& spec,
                                             CachePolicy policy,
                                             std::chrono::milliseconds timeout)
{
  auto operation = prepareAsync(spec, policy, timeout);
  return operation->result(timeout.count() == 0 ? m_jobTimeout : timeout);
}

std::size_t ModelPreparationCache::parseCount() const noexcept
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_parseCount;
}

std::size_t ModelPreparationCache::entryCount() const noexcept
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_entries.size();
}

std::size_t ModelPreparationCache::chargedBytes() const noexcept
{
  std::lock_guard<std::mutex> lock(m_mutex);
  const auto retired = retiredBytes();
  return retired > std::numeric_limits<std::size_t>::max() - m_chargedBytes
    ? std::numeric_limits<std::size_t>::max() : m_chargedBytes + retired;
}

} // namespace ndnsf::di
