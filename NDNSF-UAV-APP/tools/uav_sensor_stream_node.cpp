#include "../shared/UavSensorStreams.hpp"

#include "ndn-service-framework/CertificatePublisher.hpp"
#include "ndn-service-framework/ServiceProvider.hpp"
#include "ndn-service-framework/ServiceUser.hpp"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace nsf = ndn_service_framework;
namespace uav = ndnsf::examples::uav;
using Clock = std::chrono::steady_clock;

namespace {

struct Options
{
  std::string role;
  std::string workload;
  std::string descriptor;
  std::string publicationLog;
  std::string admissionLog;
  std::string status;
  uint64_t sessionEpoch = 144001;
  uint64_t mappingVersion = 144001;
  uint64_t warmupSeconds = 5;
  uint64_t measurementSeconds = 60;
  uint64_t timeoutSeconds = 75;
  uint64_t postMeasurementHoldSeconds = 5;
};

uint64_t
nowNs()
{
  return static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::nanoseconds>(
      Clock::now().time_since_epoch()).count());
}

std::string
jsonEscape(const std::string& input)
{
  std::ostringstream output;
  for (const auto ch : input) {
    switch (ch) {
      case '\\': output << "\\\\"; break;
      case '"': output << "\\\""; break;
      case '\n': output << "\\n"; break;
      case '\r': output << "\\r"; break;
      case '\t': output << "\\t"; break;
      default: output << ch; break;
    }
  }
  return output.str();
}

Options
parseOptions(int argc, char** argv)
{
  Options options;
  const auto value = [&] (const std::string& name) -> std::optional<std::string> {
    for (int i = 1; i + 1 < argc; ++i) {
      if (argv[i] == name) return argv[i + 1];
    }
    return std::nullopt;
  };
  options.role = value("--role").value_or("");
  options.workload = value("--workload").value_or("");
  options.descriptor = value("--descriptor").value_or("");
  options.publicationLog = value("--publication-log").value_or("");
  options.admissionLog = value("--admission-log").value_or("");
  options.status = value("--status").value_or("");
  if (const auto input = value("--session-epoch")) {
    options.sessionEpoch = std::stoull(*input);
  }
  if (const auto input = value("--mapping-version")) {
    options.mappingVersion = std::stoull(*input);
  }
  if (const auto input = value("--warmup-seconds")) {
    options.warmupSeconds = std::stoull(*input);
  }
  if (const auto input = value("--measurement-seconds")) {
    options.measurementSeconds = std::stoull(*input);
  }
  if (const auto input = value("--timeout-seconds")) {
    options.timeoutSeconds = std::stoull(*input);
  }
  if (const auto input = value("--post-measurement-hold-seconds")) {
    options.postMeasurementHoldSeconds = std::stoull(*input);
  }
  if ((options.role != "provider" && options.role != "consumer") ||
      (options.workload != "telemetry" && options.workload != "acoustic") ||
      options.descriptor.empty() || options.status.empty() ||
      options.warmupSeconds == 0 || options.measurementSeconds == 0) {
    throw std::invalid_argument(
      "usage: --role provider|consumer --workload telemetry|acoustic "
      "--descriptor PATH --status PATH [--publication-log PATH]");
  }
  return options;
}

ndn::security::Certificate
identity(ndn::KeyChain& keyChain, const ndn::Name& name)
{
  try {
    auto existing = keyChain.getPib().getIdentity(name);
    for (const auto& key : existing.getKeys()) {
      if (key.getKeyType() == ndn::KeyType::RSA) {
        return key.getDefaultCertificate();
      }
    }
    return keyChain.createKey(existing, ndn::RsaKeyParams(2048))
      .getDefaultCertificate();
  }
  catch (const std::exception&) {
    return keyChain.createIdentity(name, ndn::RsaKeyParams(2048))
      .getDefaultKey().getDefaultCertificate();
  }
}

std::string
hex(const nsf::StreamContentDigest& digest)
{
  static constexpr char DIGITS[] = "0123456789abcdef";
  std::string result;
  result.reserve(digest.size() * 2);
  for (const auto byte : digest) {
    result.push_back(DIGITS[byte >> 4]);
    result.push_back(DIGITS[byte & 0x0f]);
  }
  return result;
}

nsf::StreamContentDigest
unhex(const std::string& text)
{
  nsf::StreamContentDigest result{};
  if (text.size() != result.size() * 2) {
    throw std::invalid_argument("invalid descriptor digest");
  }
  for (size_t i = 0; i < result.size(); ++i) {
    result[i] = static_cast<uint8_t>(
      std::stoul(text.substr(i * 2, 2), nullptr, 16));
  }
  return result;
}

void
writeDescriptor(const std::string& path, const std::string& workload,
                const nsf::LiveStreamDescriptor& descriptor)
{
  boost::property_tree::ptree value;
  const auto& definition = descriptor.definition;
  value.put("schemaVersion", "spec144-uav-sensor-descriptor-v1");
  value.put("workload", workload);
  value.put("streamId", definition.streamId);
  value.put("contractVersion", definition.contractVersion);
  value.put("provider", definition.provider.toUri());
  value.put("semanticDataPrefix", definition.semanticDataPrefix.toUri());
  value.put("sessionEpoch", definition.sessionEpoch);
  value.put("mappingVersion", definition.mappingVersion);
  value.put("mappingBlockCapacity", definition.mappingBlockCapacity);
  value.put("mappingAheadBlocks", definition.mappingAheadBlocks);
  value.put("retainedItems", definition.retainedItems);
  value.put("maxNameReservations", definition.maxNameReservations);
  value.put("maxPendingInterests", definition.maxPendingInterests);
  value.put("signedWireCap", definition.signedWireCap);
  value.put("samplePeriodMs", definition.samplePeriodMs);
  value.put("measuredSamplePeriodMs", descriptor.measuredSamplePeriodMs);
  value.put("safeJoinCursor", descriptor.safeJoinCursor);
  value.put("checkpoint.blockNumber", descriptor.checkpoint.blockNumber);
  value.put("checkpoint.digest", hex(descriptor.checkpoint.contentDigest));
  value.put("checkpoint.oldestRetained",
            descriptor.checkpoint.frontiers.oldestRetained);
  value.put("checkpoint.latestJoin",
            descriptor.checkpoint.frontiers.latestJoin);
  value.put("checkpoint.latestProduced",
            descriptor.checkpoint.frontiers.latestProduced);
  value.put("checkpoint.mappingCommittedThrough",
            descriptor.checkpoint.frontiers.mappingCommittedThrough);
  value.put("checkpoint.nextReserved",
            descriptor.checkpoint.frontiers.nextReserved);
  boost::property_tree::write_json(path + ".tmp", value);
  std::rename((path + ".tmp").c_str(), path.c_str());
}

nsf::LiveStreamDescriptor
readDescriptor(const std::string& path, const std::string& workload)
{
  boost::property_tree::ptree input;
  boost::property_tree::read_json(path, input);
  if (input.get<std::string>("schemaVersion") !=
        "spec144-uav-sensor-descriptor-v1" ||
      input.get<std::string>("workload") != workload) {
    throw std::invalid_argument("descriptor identity mismatch");
  }
  const ndn::Name provider(input.get<std::string>("provider"));
  const auto sessionEpoch = input.get<uint64_t>("sessionEpoch");
  const auto mappingVersion = input.get<uint64_t>("mappingVersion");
  nsf::LiveStreamDescriptor descriptor;
  descriptor.definition = workload == "telemetry"
    ? uav::makeUavTelemetryStreamDefinition(provider, sessionEpoch, mappingVersion)
    : uav::makeUavAcousticStreamDefinition(provider, sessionEpoch, mappingVersion);
  auto& definition = descriptor.definition;
  definition.streamId = input.get<std::string>("streamId");
  definition.contractVersion = input.get<uint64_t>("contractVersion");
  definition.semanticDataPrefix = input.get<std::string>("semanticDataPrefix");
  definition.mappingBlockCapacity = input.get<size_t>("mappingBlockCapacity");
  definition.mappingAheadBlocks = input.get<size_t>("mappingAheadBlocks");
  definition.retainedItems = input.get<size_t>("retainedItems");
  definition.maxNameReservations = input.get<size_t>("maxNameReservations");
  definition.maxPendingInterests = input.get<size_t>("maxPendingInterests");
  definition.signedWireCap = input.get<size_t>("signedWireCap");
  definition.samplePeriodMs = input.get<double>("samplePeriodMs");
  descriptor.measuredSamplePeriodMs =
    input.get<double>("measuredSamplePeriodMs");
  descriptor.safeJoinCursor = input.get<uint64_t>("safeJoinCursor");
  descriptor.checkpoint.blockNumber =
    input.get<uint64_t>("checkpoint.blockNumber");
  descriptor.checkpoint.contentDigest =
    unhex(input.get<std::string>("checkpoint.digest"));
  auto& frontiers = descriptor.checkpoint.frontiers;
  frontiers.oldestRetained =
    input.get<uint64_t>("checkpoint.oldestRetained");
  frontiers.latestJoin = input.get<uint64_t>("checkpoint.latestJoin");
  frontiers.latestProduced =
    input.get<uint64_t>("checkpoint.latestProduced");
  frontiers.mappingCommittedThrough =
    input.get<uint64_t>("checkpoint.mappingCommittedThrough");
  frontiers.nextReserved =
    input.get<uint64_t>("checkpoint.nextReserved");
  if (const auto error = descriptor.validate()) {
    throw std::invalid_argument("invalid descriptor: " + *error);
  }
  return descriptor;
}

void
writeJsonArray(std::ostream& output, const std::vector<double>& values)
{
  output << '[';
  for (size_t i = 0; i < values.size(); ++i) {
    if (i != 0) output << ',';
    output << std::fixed << std::setprecision(6) << values[i];
  }
  output << ']';
}

void
writeNativeStatus(std::ostream& output, const nsf::LiveStreamStatus& value)
{
  output << "\"state\":\"" << nsf::toString(value.state) << "\","
         << "\"reason\":\"" << jsonEscape(value.reason) << "\","
         << "\"delivered\":" << value.delivered << ','
         << "\"rejected\":" << value.rejected << ','
         << "\"recovered\":" << value.recovered << ','
         << "\"timeouts\":" << value.timeouts << ','
         << "\"nacks\":" << value.nacks << ','
         << "\"retryAttempts\":" << value.retryAttempts << ','
         << "\"lateArrivals\":" << value.lateArrivals << ','
         << "\"deadlineSkips\":" << value.deadlineSkips << ','
         << "\"retryExhaustions\":" << value.retryExhaustions << ','
         << "\"mappingInterests\":" << value.mappingInterests << ','
         << "\"mappingDataResponses\":" << value.mappingDataResponses << ','
         << "\"mappingNewDataResponses\":" << value.mappingNewDataResponses << ','
         << "\"payloadInterests\":" << value.payloadInterests << ','
         << "\"initialPayloadInterests\":" << value.initialPayloadInterests << ','
         << "\"retryPayloadInterests\":" << value.retryPayloadInterests << ','
         << "\"payloadSourceInterests\":" << value.payloadSourceInterests << ','
         << "\"initialPayloadSourceInterests\":"
         << value.initialPayloadSourceInterests << ','
         << "\"retryPayloadSourceInterests\":"
         << value.retryPayloadSourceInterests << ','
         << "\"payloadRepairInterests\":" << value.payloadRepairInterests << ','
         << "\"initialPayloadRepairInterests\":"
         << value.initialPayloadRepairInterests << ','
         << "\"retryPayloadRepairInterests\":"
         << value.retryPayloadRepairInterests << ','
         << "\"payloadUnclassifiedInterests\":"
         << value.payloadUnclassifiedInterests << ','
         << "\"futurePayloadInterests\":" << value.futurePayloadInterests << ','
         << "\"initialFuturePayloadInterests\":"
         << value.initialFuturePayloadInterests << ','
         << "\"retryFuturePayloadInterests\":"
         << value.retryFuturePayloadInterests << ','
         << "\"retrySuccesses\":" << value.retrySuccesses << ','
         << "\"retrySuppressions\":" << value.retrySuppressions << ','
         << "\"declaredRecoveryCapacity\":"
         << value.declaredRecoveryCapacity << ','
         << "\"recoveryEligibleSources\":"
         << value.recoveryEligibleSources << ','
         << "\"terminalMissingSources\":"
         << value.terminalMissingSources << ','
         << "\"recoverableGroups\":" << value.recoverableGroups << ','
         << "\"recoveredGroups\":" << value.recoveredGroups << ','
         << "\"recoveryAttempts\":" << value.recoveryAttempts << ','
         << "\"recoveryExhaustions\":" << value.recoveryExhaustions << ','
         << "\"mappingBytes\":" << value.mappingBytes << ','
         << "\"providerFutureInterests\":" << value.providerFutureInterests << ','
         << "\"providerFutureHits\":" << value.providerFutureHits << ','
         << "\"providerInitialFutureInterests\":"
         << value.providerInitialFutureInterests << ','
         << "\"providerInitialFutureHits\":"
         << value.providerInitialFutureHits << ','
         << "\"providerRetryFutureInterests\":"
         << value.providerRetryFutureInterests << ','
         << "\"providerRetryFutureHits\":"
         << value.providerRetryFutureHits << ','
         << "\"payloadSourceDataAdmissions\":"
         << value.payloadSourceDataAdmissions << ','
         << "\"payloadRepairDataResponses\":"
         << value.payloadRepairDataResponses << ','
         << "\"payloadRepairDataConsumed\":"
         << value.payloadRepairDataConsumed << ','
         << "\"payloadApplicationUsefulInterests\":"
         << value.payloadApplicationUsefulInterests << ','
         << "\"payloadProtectionOnlyInterests\":"
         << value.payloadProtectionOnlyInterests << ','
         << "\"payloadNonproductiveInterests\":"
         << value.payloadNonproductiveInterests << ','
         << "\"payloadUnresolvedInterests\":"
         << value.payloadUnresolvedInterests << ','
         << "\"retrySuppressionReasons\":{";
  bool firstReason = true;
  for (const auto& [reason, count] : value.retrySuppressionReasons) {
    if (!firstReason) output << ',';
    firstReason = false;
    output << '"' << jsonEscape(reason) << "\":" << count;
  }
  output << "},\"fetchDecision\":";
  if (!value.fetchDecision) {
    output << "null";
  }
  else {
    const auto& decision = *value.fetchDecision;
    output << "{\"phase\":\"" << nsf::toString(decision.phase) << "\","
           << "\"policyMode\":\"" << jsonEscape(decision.policyMode) << "\","
           << "\"window\":" << decision.window << ','
           << "\"lookahead\":" << decision.lookahead << ','
           << "\"interestLifetimeMs\":" << decision.interestLifetimeMs << ','
           << "\"missingTimeoutMs\":" << decision.missingTimeoutMs << ','
           << "\"mappingBudget\":" << decision.mappingBudget << ','
           << "\"payloadBudget\":" << decision.payloadBudget << ','
           << "\"pressure\":" << decision.pressure << ','
           << "\"reason\":\"" << jsonEscape(decision.reason) << "\"}";
  }
}

void
writeFailure(const Options& options, const std::exception& error)
{
  std::ofstream output(options.status + ".tmp");
  output << "{\"schemaVersion\":\"spec144-uav-sensor-node-v1\","
         << "\"role\":\"" << options.role << "\","
         << "\"workload\":\"" << options.workload << "\","
         << "\"passed\":false,\"error\":\""
         << jsonEscape(std::string(typeid(error).name()) + ": " + error.what())
         << "\"}\n";
  output.close();
  std::rename((options.status + ".tmp").c_str(), options.status.c_str());
}

std::vector<std::vector<uint8_t>>
makeSources(const std::string& workload, uint64_t sampleId, uint64_t sourceNs)
{
  if (workload == "telemetry") {
    return {uav::CompactTelemetrySample::deterministic(
      sampleId, sourceNs, "A").encode()};
  }
  std::vector<std::vector<uint8_t>> result;
  const auto count = uav::OpaqueAcousticSource::sourceCountFor(sampleId);
  for (size_t index = 0; index < count; ++index) {
    result.push_back(uav::OpaqueAcousticSource::deterministic(
      sampleId, sourceNs, index).encode());
  }
  return result;
}

int
runProvider(const Options& options)
{
  ndn::Face face;
  ndn::KeyChain keyChain;
  const ndn::Name providerName("/example/uav/drone/A");
  const ndn::Name controllerName("/example/uav/controller");
  const auto providerCert = identity(keyChain, providerName);
  const auto controllerCert = identity(keyChain, controllerName);
  nsf::CertificatePublisher certificatePublisher(
    face, keyChain, providerCert.getName());
  nsf::ServiceProvider provider(
    face, "/example/uav", providerCert, controllerCert, "examples/trust-any.conf");
  const auto definition = options.workload == "telemetry"
    ? uav::makeUavTelemetryStreamDefinition(
        providerName, options.sessionEpoch, options.mappingVersion)
    : uav::makeUavAcousticStreamDefinition(
        providerName, options.sessionEpoch, options.mappingVersion);
  auto publisher = provider.createLiveStream(definition);
  face.processEvents(ndn::time::milliseconds(300));
  face.getIoContext().restart();

  const uint64_t periodMs = options.workload == "telemetry"
    ? uav::UAV_TELEMETRY_PERIOD_MS : uav::UAV_ACOUSTIC_BLOCK_PERIOD_MS;
  const uint64_t warmupCount = options.warmupSeconds * 1000 / periodMs;
  const uint64_t measuredCount =
    options.measurementSeconds * 1000 / periodMs;
  const uint64_t totalCount = warmupCount + measuredCount;
  // Keep a bounded 1.6 s name-production lead for both periodic workloads.
  // This exposes real future names without choosing the consumer's adaptive
  // fetch window or expressing any Interest in the APP.
  constexpr size_t ANNOUNCEMENT_AHEAD = 32;
  std::map<uint64_t, nsf::LiveStreamSampleReservation> reservations;
  const auto nameFactory = [&definition] (uint64_t sampleId) {
    return [&definition, sampleId] (size_t index, nsf::LiveStreamItemKind kind) {
      return ndn::Name(definition.semanticDataPrefix)
        .append("sample").appendSequenceNumber(sampleId)
        .append(kind == nsf::LiveStreamItemKind::Source ? "source" : "repair")
        .appendSegment(index);
    };
  };
  const auto sampleClassFor = [&options] (uint64_t sampleId) {
    return options.workload == "telemetry"
      ? std::string("compact-state")
      : uav::acousticSourceCountClass(
          uav::OpaqueAcousticSource::sourceCountFor(sampleId));
  };
  for (uint64_t id = 0; id < std::min<uint64_t>(ANNOUNCEMENT_AHEAD, totalCount);
       ++id) {
    reservations.emplace(id, publisher->announceSample(
      id, sampleClassFor(id), nameFactory(id)));
  }

  std::ofstream publications;
  if (!options.publicationLog.empty()) {
    publications.open(options.publicationLog, std::ios::trunc);
  }
  const auto started = Clock::now();
  bool descriptorWritten = false;
  uint64_t attemptedMeasured = 0;
  uint64_t producedMeasured = 0;
  for (uint64_t id = 0; id < totalCount; ++id) {
    const auto deadline = started + std::chrono::milliseconds(id * periodMs);
    while (Clock::now() < deadline) {
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - Clock::now());
      face.processEvents(std::max(ndn::time::milliseconds(1),
                                 ndn::time::milliseconds(remaining.count())));
      face.getIoContext().restart();
    }
    const auto sourceNs = nowNs();
    const auto sources = makeSources(options.workload, id, sourceNs);
    const auto reservation = reservations.at(id);
    const auto prepared = publisher->prepareSampleExtent(
      reservation, sources.size());
    if (prepared.size() != sources.size()) {
      throw std::logic_error("prepared extent mismatch");
    }
    if (publications) {
      publications << "{\"sampleId\":" << id
                   << ",\"phase\":\"" << (id < warmupCount ? "warmup" : "measured")
                   << "\",\"sourceTimestampNs\":" << sourceNs
                   << ",\"sourceItems\":" << sources.size()
                   << ",\"repairItems\":" << definition.fec.repairItemCount()
                   << "}\n";
      publications.flush();
    }
    if (id >= warmupCount) ++attemptedMeasured;
    publisher->publishSample(reservation, sources);
    if (!descriptorWritten) {
      const auto descriptor = publisher->activate(
        {static_cast<double>(periodMs), reservation.group.sources.front().cursor});
      writeDescriptor(options.descriptor, options.workload, descriptor);
      descriptorWritten = true;
    }
    if (id >= warmupCount) ++producedMeasured;
    const auto next = id + ANNOUNCEMENT_AHEAD;
    if (next < totalCount) {
      reservations.emplace(next, publisher->announceSample(
        next, sampleClassFor(next), nameFactory(next)));
    }
    reservations.erase(id);
  }
  const auto status = publisher->status();
  std::ofstream output(options.status + ".tmp");
  output << "{\"schemaVersion\":\"spec144-uav-sensor-provider-v1\","
         << "\"role\":\"provider\",\"workload\":\"" << options.workload << "\","
         << "\"passed\":" << (descriptorWritten ? "true" : "false") << ','
         << "\"descriptorWritten\":" << (descriptorWritten ? "true" : "false") << ','
         << "\"trafficCounterScope\":\"full-run-including-warmup\","
         << "\"measurementScope\":\"measured-window-only\","
         << "\"warmupCount\":" << warmupCount << ','
         << "\"expectedMeasured\":" << measuredCount << ','
         << "\"attemptedMeasured\":" << attemptedMeasured << ','
         << "\"producedMeasured\":" << producedMeasured << ','
         << "\"nativeStatus\":{";
  writeNativeStatus(output, status);
  output << "}}\n";
  output.close();
  std::rename((options.status + ".tmp").c_str(), options.status.c_str());

  // The measured publication window is already complete and the status has
  // been atomically written. Keep the producer's registered prefixes and
  // retained packets available while a later-started consumer drains its own
  // unchanged measurement window.
  const auto holdUntil = Clock::now() +
    std::chrono::seconds(options.postMeasurementHoldSeconds);
  while (Clock::now() < holdUntil) {
    face.processEvents(ndn::time::milliseconds(20));
    face.getIoContext().restart();
  }
  publisher->stop();
  return descriptorWritten ? 0 : 2;
}

int
runConsumer(const Options& options)
{
  const auto descriptorDeadline =
    Clock::now() + std::chrono::seconds(options.timeoutSeconds);
  while (Clock::now() < descriptorDeadline) {
    std::ifstream input(options.descriptor);
    if (input.good()) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  const auto descriptor = readDescriptor(options.descriptor, options.workload);
  const uint64_t periodMs = options.workload == "telemetry"
    ? uav::UAV_TELEMETRY_PERIOD_MS : uav::UAV_ACOUSTIC_BLOCK_PERIOD_MS;
  const uint64_t warmupCount = options.warmupSeconds * 1000 / periodMs;
  const uint64_t expectedMeasured =
    options.measurementSeconds * 1000 / periodMs;

  ndn::Face face;
  ndn::KeyChain keyChain;
  // Reuse the deployed UAV APP identity and policy membership used by the
  // video reference path; this is not a synthetic stream-only principal.
  const ndn::Name userName("/example/uav/gs");
  const ndn::Name controllerName("/example/uav/controller");
  const auto userCert = identity(keyChain, userName);
  const auto controllerCert = identity(keyChain, controllerName);
  nsf::ServiceUser user(
    face, "/example/uav", userCert, controllerCert, "examples/trust-any.conf");
  uav::LatestTelemetryAdmission telemetry("A");
  uav::CompleteAcousticBlockAdmission acoustic(descriptor.definition.streamId);
  std::vector<double> measuredLatenciesMs;
  std::vector<uint64_t> measuredArrivalNs;
  std::ofstream admissions;
  if (!options.admissionLog.empty()) {
    admissions.open(options.admissionLog, std::ios::trunc);
  }
  uint64_t measuredComplete = 0;
  uint64_t invalid = 0;
  uint64_t duplicate = 0;
  uint64_t outOfOrder = 0;
  uint64_t monotonicStateViolations = 0;
  uint64_t recoveredBlocks = 0;
  uint64_t recoveredSources = 0;
  std::shared_ptr<nsf::LiveStreamConsumerHandle> handle;

  nsf::LiveStreamOpenOptions streamOptions;
  streamOptions.start = nsf::LiveStreamStart::Latest;
  streamOptions.prefetchPolicy =
    nsf::LiveStreamPrefetchPolicy::AdaptiveSampleAtomic;
  streamOptions.aggregateInterestLimit = 64;
  streamOptions.enableFecRecovery = options.workload == "acoustic";
  streamOptions.interestLifetimeMs = 500;
  streamOptions.onItem = [&] (const nsf::VerifiedLiveStreamItem& item) {
    const auto receivedNs = nowNs();
    if (options.workload == "telemetry") {
      const auto before = telemetry.latest();
      const auto result = telemetry.admit(item.content, receivedNs);
      if (!result.valid) {
        ++invalid;
        return nsf::LiveStreamItemAdmission::rejectItem(result.reason);
      }
      if (result.duplicate) ++duplicate;
      if (result.outOfOrder) ++outOfOrder;
      const auto current = telemetry.latest();
      if (before && current && current->sampleId < before->sampleId) {
        ++monotonicStateViolations;
      }
      if (result.newSample && result.sampleId >= warmupCount) {
          ++measuredComplete;
          measuredLatenciesMs.push_back(result.ageNs / 1'000'000.0);
          measuredArrivalNs.push_back(receivedNs);
          if (handle) {
            handle->observeAcceptedSample(
              {result.sampleId, item.receivedMs,
               result.ageNs / 1'000'000.0, 1});
          }
      }
      if (admissions) {
        admissions << "{\"workload\":\"telemetry\",\"sampleId\":"
                   << result.sampleId << ",\"receivedTimestampNs\":" << receivedNs
                   << ",\"ageNs\":" << result.ageNs
                   << ",\"valid\":" << (result.valid ? "true" : "false")
                   << ",\"newSample\":" << (result.newSample ? "true" : "false")
                   << ",\"stateAdvanced\":"
                   << (result.stateAdvanced ? "true" : "false")
                   << ",\"duplicate\":" << (result.duplicate ? "true" : "false")
                   << ",\"outOfOrder\":"
                   << (result.outOfOrder ? "true" : "false")
                   << ",\"measured\":"
                   << (result.newSample && result.sampleId >= warmupCount ?
                         "true" : "false")
                   << "}\n";
      }
      return nsf::LiveStreamItemAdmission::acceptItem();
    }

    const auto result = acoustic.admit(item.content, item.provenance, receivedNs);
    if (!result.valid) {
      ++invalid;
      return nsf::LiveStreamItemAdmission::rejectItem(result.reason);
    }
    if (result.duplicate) ++duplicate;
    if (admissions) {
      admissions << "{\"workload\":\"acoustic\",\"receivedTimestampNs\":"
                 << receivedNs << ",\"valid\":"
                 << (result.valid ? "true" : "false")
                 << ",\"duplicate\":" << (result.duplicate ? "true" : "false")
                 << ",\"completed\":" << (result.completed ? "true" : "false");
      if (result.completed) {
        admissions << ",\"blockId\":" << result.completed->blockId
                   << ",\"captureTimestampNs\":"
                   << result.completed->captureTimestampNs
                   << ",\"recoveredSources\":"
                   << result.completed->recoveredSources
                   << ",\"measured\":"
                   << (result.completed->blockId >= warmupCount ? "true" : "false");
      }
      admissions << "}\n";
    }
    if (result.completed && result.completed->blockId >= warmupCount) {
      ++measuredComplete;
      recoveredSources += result.completed->recoveredSources;
      if (result.completed->recoveredSources > 0) ++recoveredBlocks;
      measuredLatenciesMs.push_back(
        (result.completed->completedTimestampNs -
         result.completed->captureTimestampNs) / 1'000'000.0);
      measuredArrivalNs.push_back(result.completed->completedTimestampNs);
      if (handle) {
        handle->observeAcceptedSample(
          {result.completed->blockId, item.receivedMs,
           measuredLatenciesMs.back(),
           static_cast<uint64_t>(result.completed->orderedSources.size())});
      }
    }
    return nsf::LiveStreamItemAdmission::acceptItem();
  };
  handle = user.openLiveStream(descriptor, std::move(streamOptions));
  handle->start();
  const auto deadline = Clock::now() + std::chrono::seconds(options.timeoutSeconds);
  while (measuredComplete < expectedMeasured && Clock::now() < deadline) {
    face.processEvents(ndn::time::milliseconds(20));
    face.getIoContext().restart();
  }
  // Measurement ends when the final application block is admitted, but a
  // bounded number of future/retry Interests can still own network slots.
  // Drain those terminal callbacks before taking traffic-counter evidence;
  // measured latency and delivery samples above remain unchanged.
  if (measuredComplete >= expectedMeasured) {
    const auto drainUntil = Clock::now() +
      std::chrono::seconds(options.postMeasurementHoldSeconds);
    while (Clock::now() < drainUntil) {
      face.processEvents(ndn::time::milliseconds(20));
      face.getIoContext().restart();
    }
  }
  const auto nativeStatus = handle->status();
  uint64_t longestGapNs = 0;
  for (size_t i = 1; i < measuredArrivalNs.size(); ++i) {
    longestGapNs = std::max(longestGapNs,
                           measuredArrivalNs[i] - measuredArrivalNs[i - 1]);
  }
  std::ofstream output(options.status + ".tmp");
  output << "{\"schemaVersion\":\"spec144-uav-sensor-consumer-v1\","
         << "\"role\":\"consumer\",\"workload\":\"" << options.workload << "\","
         << "\"passed\":"
         << (nativeStatus.state != nsf::LiveStreamLifecycleState::Failed ? "true" : "false")
         << ",\"trafficCounterScope\":\"full-run-including-warmup\","
         << "\"measurementScope\":\"measured-window-only\","
         << "\"clockDomain\":\"shared-host-steady-clock\","
         << "\"latencyOrigin\":\"source-or-capture-ready\","
         << "\"latencyTerminal\":\"complete-application-admission\","
         << "\"warmupCount\":" << warmupCount << ','
         << "\"expectedMeasured\":" << expectedMeasured << ','
         << "\"completeMeasured\":" << measuredComplete << ','
         << "\"invalid\":" << invalid << ','
         << "\"duplicates\":" << duplicate << ','
         << "\"outOfOrder\":" << outOfOrder << ','
         << "\"monotonicStateViolations\":" << monotonicStateViolations << ','
         << "\"recoveredBlocks\":" << recoveredBlocks << ','
         << "\"recoveredSources\":" << recoveredSources << ','
         << "\"longestGapMs\":" << longestGapNs / 1'000'000.0 << ','
         << "\"latencyMs\":";
  writeJsonArray(output, measuredLatenciesMs);
  output << ",\"nativeStatus\":{";
  writeNativeStatus(output, nativeStatus);
  output << "}}\n";
  output.close();
  admissions.close();
  std::rename((options.status + ".tmp").c_str(), options.status.c_str());
  handle->stop();
  return nativeStatus.state == nsf::LiveStreamLifecycleState::Failed ? 2 : 0;
}

} // namespace

int
main(int argc, char** argv)
{
  Options options;
  try {
    options = parseOptions(argc, argv);
    return options.role == "provider" ? runProvider(options) : runConsumer(options);
  }
  catch (const std::exception& error) {
    if (!options.status.empty()) writeFailure(options, error);
    std::cerr << "uav-sensor-stream-node: " << error.what() << std::endl;
    return 2;
  }
}
