#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "DI_NetworkProbeCalibration.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceProvider.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "ndn-service-framework/CertificateBootstrap.hpp"
#include "ndn-service-framework/CertificatePublisher.hpp"
#include "ndn-service-framework/NetworkProbeRate.hpp"
#include "ndn-service-framework/ServiceProvider.hpp"
#include "ndn-service-framework/ServiceUser.hpp"

#include <ndn-cxx/security/key-params.hpp>
#include <ndn-cxx/util/io.hpp>
#include <boost/asio/signal_set.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#ifndef NDNSF_PROBE_CALIBRATION_NO_MAIN
namespace {
using ndnsf::di::NativeJson;
using ndnsf::di::nativeCanonicalJson;
using ndnsf::di::nativePlanningDigest;
using namespace ndn_service_framework;
using Clock = std::chrono::steady_clock;

std::uint64_t wallMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
}

template<typename Predicate>
bool pumpUntil(ndn::Face& face, Predicate ready, Clock::time_point deadline)
{
  while (!ready()) {
    if (Clock::now() >= deadline)
      return false;
    face.processEvents(ndn::time::milliseconds(10));
  }
  return true;
}

NativeJson invoke(ndn::Face& face, ServiceUser& user, const std::string& target,
                  const NativeJson& payload, Clock::time_point deadline,
                  bool expectLocalDenial = false)
{
  struct Completion {
    std::mutex mutex;
    bool done = false;
    std::optional<ResponseMessage> response;
  };
  auto state = std::make_shared<Completion>();
  const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
    deadline - Clock::now()).count();
  if (remaining <= 0)
    throw std::runtime_error("targeted invocation deadline exhausted");
  const auto wire = nativeCanonicalJson(payload);
  if (wire.empty() || wire.size() > 4096)
    throw std::runtime_error("probe request exceeds wire budget");
  ndn::Buffer bytes(reinterpret_cast<const std::uint8_t*>(wire.data()), wire.size());
  RequestMessage request;
  request.setTargetProvider(ndn::Name(target));
  request.setPayload(bytes, bytes.size());
  if (!expectLocalDenial)
    request.setTokens({{"targeted.refill", "1"}});
  const auto name = user.RequestServiceTargeted(
    ndn::Name(target), ndn::Name(NETWORK_PROBE_SERVICE), std::move(request),
    static_cast<int>(std::min<std::int64_t>(remaining, 5000)),
    [state] (const ndn::Name&) {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (!state->done)
        state->done = true;
    },
    [state] (const ResponseMessage& response) {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (!state->done) {
        state->response = response;
        state->done = true;
      }
    });
  if (expectLocalDenial) {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->done = true;
    if (!name.empty())
      throw std::runtime_error("unauthorized targeted invocation was admitted");
    return NativeJson{{"rejected_locally", true}, {"provider", target}};
  }
  if (name.empty())
    throw std::runtime_error("targeted invocation rejected locally");
  pumpUntil(face, [&] {
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->done;
  }, deadline);
  std::optional<ResponseMessage> response;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->done = true;
    response = std::move(state->response);
  }
  if (!response)
    throw std::runtime_error("targeted invocation timed out");
  if (!ndnsf::di::probe_calibration::hasAuthenticatedSigner(*response, target))
    throw std::runtime_error("targeted response signer mismatch");
  if (!response->getStatus())
    throw std::runtime_error("targeted invocation rejected remotely: " + response->getErrorInfo());
  const auto result = response->getPayload();
  if (result.empty() || result.size() > ndnsf::di::NATIVE_PLACEMENT_PROBE_RESPONSE_MAX_BYTES)
    throw std::runtime_error("probe response exceeds wire budget");
  return ndnsf::di::nativeParseJson(std::string(
    reinterpret_cast<const char*>(result.data()), result.size()));
}

NativeJson measure(ndn::Face& face, ServiceUser& user, const NativeJson& config,
                   std::size_t round, bool expectLocalDenial = false)
{
  const auto source = config.at("source").get<std::string>();
  const auto observer = config.at("observer").get<std::string>();
  const auto requester = user.getName().toUri();
  std::vector<std::string> roster{source, observer};
  std::sort(roster.begin(), roster.end());
  const auto isStandardIdentity = [](const std::string& value) {
    const ndn::Name name(value);
    return !name.empty() &&
      (name.toUri(ndn::name::UriFormat::CANONICAL) == value ||
       name.toUri(ndn::name::UriFormat::ALTERNATE) == value);
  };
  if (!isStandardIdentity(source) || !isStandardIdentity(observer) ||
      ndn::Name(source) == ndn::Name(observer))
    throw std::runtime_error("calibration requires two canonical Provider identities");
  const auto deadline = Clock::now() + std::chrono::seconds(5);
  const auto deadlineMs = wallMs() + 5000;
  std::random_device random;
  const auto nonce = nativePlanningDigest(std::to_string(random()) + ":" +
    std::to_string(random()) + ":" + std::to_string(wallMs()) + ":" + std::to_string(round));
  auto requestName = user.getName();
  requestName.append("calibration").appendNumber(wallMs()).appendNumber(round);
  const auto requestId = requestName.toUri();
  const auto service = config.value("context_service", "/NDNSF/Calibration");
  const NativeJson rttPairs = NativeJson::array({{
    {"src_provider", observer}, {"dst_provider", source}}});
  const NativeJson edges = NativeJson::array({{
    {"src_provider", source}, {"dst_provider", observer}}});
  NativeJson scope{
    {"schema", "ndnsf-di-placement-probe-scope-v1"},
    {"request_id", requestId}, {"attempt", 1}, {"requester_identity", requester},
    {"observer_provider", observer}, {"service", service},
    {"provider_roster", roster},
    {"roster_digest", nativePlanningDigest(nativeCanonicalJson(roster))},
    {"rtt_pairs", rttPairs}, {"goodput_edges", edges},
    {"deadline_ms", deadlineMs}, {"intent_nonce", nonce}};
  const auto digest = nativePlanningDigest(nativeCanonicalJson(scope));
  auto request = scope;
  request["schema"] = "ndnsf-di-placement-probe-request-v1";
  request["action"] = "authorize";
  request["scope_digest"] = digest;
  request["measurement_kinds"] = {"active_probe_goodput", "active_probe_rtt"};
  if (expectLocalDenial) {
    const auto sourceDenial = invoke(face, user, source, request, deadline, true);
    request["action"] = "measure";
    const auto observerDenial = invoke(face, user, observer, request, deadline, true);
    return NativeJson{{"scope_digest", digest}, {"context_service", service},
      {"source_denial", sourceDenial}, {"observer_denial", observerDenial}};
  }
  const auto authorized = invoke(face, user, source, request, deadline);
  if (authorized.at("schema") != "ndnsf-di-placement-probe-scope-ack-v1" ||
      authorized.at("provider") != source || authorized.at("observer_provider") != observer ||
      authorized.at("scope_digest") != digest || authorized.at("status") != "authorized")
    throw std::runtime_error("source authorization binding mismatch");
  request["action"] = "measure";
  const auto result = invoke(face, user, observer, request, deadline);
  // Preserve the authenticated public measurement before its oracle runs,
  // so malformed or incomplete samples retain their first failure evidence.
  std::cout << "NDNSF_PROBE_CALIBRATION_RESPONSE " << nativeCanonicalJson(result) << std::endl;
  return ndnsf::di::probe_calibration::recordFromResponse(result, request, round);
}
} // namespace
#endif

namespace ndnsf::di::probe_calibration {
using namespace ndn_service_framework;

bool isValidRepetitionBudget(unsigned repeats, unsigned warmup, unsigned intervalMs) noexcept
{
  return repeats > 0 && repeats <= 100 && warmup <= 3 && intervalMs <= 15000 &&
    static_cast<std::uint64_t>(warmup + repeats) * (5000 + intervalMs) <= 570000;
}

bool hasAuthenticatedSigner(const ResponseMessage& response,
                            const std::string& expectedProvider) noexcept
{
  if (response.getDataName().empty() || response.getSignerCertificate().empty() ||
      response.getWireDigest().empty())
    return false;
  try {
    return ndn::security::extractIdentityFromCertName(
      ndn::Name(response.getSignerCertificate())) == ndn::Name(expectedProvider);
  }
  catch (const std::exception&) {
    return false;
  }
}

NativeJson recordFromResponse(const NativeJson& result, const NativeJson& request,
                              std::size_t round)
{
  const auto source = request.at("goodput_edges").at(0).at("src_provider").get<std::string>();
  const auto observer = request.at("observer_provider").get<std::string>();
  const auto requester = request.at("requester_identity").get<std::string>();
  const auto requestId = request.at("request_id").get<std::string>();
  const auto service = request.at("service").get<std::string>();
  const auto digest = request.at("scope_digest").get<std::string>();
  const auto nonce = request.at("intent_nonce").get<std::string>();
  const auto deadlineMs = request.at("deadline_ms").get<std::uint64_t>();
  if (result.at("schema") != "ndnsf-di-placement-probe-response-v1" ||
      result.at("request_id") != requestId || result.at("attempt") != 1 ||
      result.at("requester_identity") != requester || result.at("observer_provider") != observer ||
      result.at("service") != service || result.at("scope_digest") != digest ||
      result.at("intent_nonce") != nonce || result.at("deadline_ms") != deadlineMs ||
      result.at("observer_boot_epoch").get<std::string>().empty() ||
      result.at("completed_at_ms").get<std::uint64_t>() > deadlineMs ||
      result.at("requested_rtt_count") != 1 || result.at("requested_goodput_count") != 1 ||
      !result.at("observations").is_array() || result.at("observations").size() > 2)
    throw std::runtime_error("measurement response context mismatch");

  NativeJson record{{"round", round}, {"scope_digest", digest},
    {"status", "UNKNOWN"}, {"reason", "MISSING_TRAIN"}, {"rtt_ms", nullptr},
    {"goodput_mbps", nullptr}, {"wire_equivalent_mbps", nullptr},
    {"response", result}};
  bool sawRtt = false;
  bool sawTrain = false;
  for (const auto& row : result.at("observations")) {
    if (row.at("observer") != observer || row.at("target") != source ||
        row.at("target_signature_validated") != true ||
        row.at("peer_boot_epoch").get<std::string>().empty() ||
        row.at("data_name").get<std::string>().empty() ||
        row.at("observed_at_ms").get<std::uint64_t>() > result.at("completed_at_ms").get<std::uint64_t>() ||
        row.at("expires_at_ms").get<std::uint64_t>() < result.at("completed_at_ms").get<std::uint64_t>() ||
        row.at("expires_at_ms").get<std::uint64_t>() > deadlineMs)
      throw std::runtime_error("probe observation authentication or freshness mismatch");
    if (row.at("kind") == "active_probe_rtt") {
      const auto rtt = row.at("value").get<double>();
      if (sawRtt || !std::isfinite(rtt) || rtt <= 0)
        throw std::runtime_error("invalid RTT observation");
      sawRtt = true;
      record["rtt_ms"] = rtt;
      continue;
    }
    if (row.at("kind") != "active_probe_goodput" || sawTrain ||
        row.at("train_complete") != true || row.at("sent_segments") != NETWORK_PROBE_WARMED_SEGMENTS ||
        row.at("received_segments") != NETWORK_PROBE_WARMED_SEGMENTS ||
        row.at("profile_digest") != NETWORK_PROBE_TRAIN_PROFILE_DIGEST ||
        row.at("probe_data_arrival_gaps_ms").size() != NETWORK_PROBE_WARMED_SEGMENTS - 1 ||
        row.at("probe_source_send_gaps_ms").size() != NETWORK_PROBE_WARMED_SEGMENTS - 2 ||
        row.at("probe_data_wire_bytes").size() != NETWORK_PROBE_WARMED_SEGMENTS)
      throw std::runtime_error("invalid dedicated train observation");
    sawTrain = true;
    const auto arrival = row.at("probe_data_arrival_gaps_ms").get<NetworkProbeArrivalGaps>();
    const auto sent = row.at("probe_source_send_gaps_ms").get<NetworkProbeSourceGaps>();
    const auto wireSizes = row.at("probe_data_wire_bytes").get<NetworkProbeWireSizes>();
    std::size_t totalWireBytes = 0;
    for (const auto size : wireSizes) {
      if (size == 0 || size > 6144)
        throw std::runtime_error("invalid received Data wire size");
      totalWireBytes += size;
    }
    // Core counts encoded Content, including the probe's TLV fields. The
    // estimator separately uses the profile's dedicated payload size.
    const auto contentBytes = row.at("payload_bytes").get<std::size_t>();
    if (contentBytes <= NETWORK_PROBE_WARMED_SEGMENTS * NETWORK_PROBE_WARMED_PAYLOAD_BYTES ||
        contentBytes > NETWORK_PROBE_WARMED_SEGMENTS * NETWORK_PROBE_WARMED_MAX_CONTENT_BYTES ||
        contentBytes >= totalWireBytes)
      throw std::runtime_error("invalid dedicated train Content byte count");
    // A complete, authenticated but reordered train remains an UNKNOWN
    // sample. Match the production metadata helper's trusted conjunction.
    const bool trusted = row.at("probe_profile_validated").get<bool>() &&
      row.at("probe_source_timing_validated").get<bool>();
    const auto rate = estimateWarmedNetworkProbeTrainRate(arrival, sent,
      NETWORK_PROBE_WARMED_PAYLOAD_BYTES, trusted);
    const bool measured = rate.status == NetworkProbeRateStatus::MeasuredGoodput;
    if (row.at("probe_rate_status") != (measured ? "MEASURED_GOODPUT" : "UNKNOWN") ||
        row.at("probe_rate_reason") != rate.reason)
      throw std::runtime_error("provider rate quality does not match Core estimator");
    record["reason"] = rate.reason;
    if (!measured) {
      if (!row.at("probe_train_goodput_mbps").is_null())
        throw std::runtime_error("UNKNOWN observation carries a point estimate");
      continue;
    }
    const auto declared = row.at("probe_train_goodput_mbps").get<double>();
    if (!std::isfinite(declared) || std::abs(declared - rate.goodputMbps) >
        1e-9 + rate.goodputMbps * 1e-6)
      throw std::runtime_error("provider rate differs from Core recomputation");
    double referenceWireBytes = 0;
    for (std::size_t index = NETWORK_PROBE_WARMED_START + 1;
         index <= NETWORK_PROBE_WARMED_START + NETWORK_PROBE_WARMED_MEASURED_SEGMENTS; ++index)
      referenceWireBytes += wireSizes[index];
    record["status"] = "MEASURED_GOODPUT";
    record["goodput_mbps"] = rate.goodputMbps;
    record["reference_data_wire_bytes"] = referenceWireBytes;
    record["wire_equivalent_mbps"] = rate.goodputMbps * referenceWireBytes /
      static_cast<double>(NETWORK_PROBE_WARMED_MEASURED_SEGMENTS * NETWORK_PROBE_WARMED_PAYLOAD_BYTES);
  }
  return record;
}

double median(std::vector<double> values)
{
  std::sort(values.begin(), values.end());
  const auto mid = values.size() / 2;
  return values.size() % 2 ? values[mid] : (values[mid - 1] + values[mid]) / 2;
}

NativeJson summarize(NativeJson records, double bandwidth)
{
  if (!records.is_array() || records.empty() || records.size() > 100 ||
      !std::isfinite(bandwidth) || bandwidth < 0)
    throw std::runtime_error("invalid calibration summary input");
  const auto repeats = records.size();
  std::vector<double> rates;
  std::vector<double> errors;
  std::vector<double> rtts;
  for (auto& record : records) {
    if (record.contains("rtt_ms") && !record.at("rtt_ms").is_null()) {
      const auto rtt = record.at("rtt_ms").get<double>();
      if (!std::isfinite(rtt) || rtt <= 0)
        throw std::runtime_error("invalid calibration sample RTT");
      rtts.push_back(rtt);
    }
    if (record.at("status") == "UNKNOWN")
      continue;
    if (record.at("status") != "MEASURED_GOODPUT")
      throw std::runtime_error("invalid calibration sample status");
    const auto wireRate = record.at("wire_equivalent_mbps").get<double>();
    if (!std::isfinite(wireRate) || wireRate <= 0)
      throw std::runtime_error("invalid calibration sample rate");
    rates.push_back(wireRate);
    if (bandwidth > 0) {
      const auto error = std::abs(wireRate - bandwidth) / bandwidth;
      record["absolute_relative_error"] = error;
      errors.push_back(error);
    }
  }
  const double unknownFraction = 1.0 - static_cast<double>(rates.size()) / repeats;
  NativeJson summary{{"schema", "ndnsf-network-probe-calibration-v1"},
    {"probe_service", NETWORK_PROBE_SERVICE}, {"records", std::move(records)},
    {"expected_bandwidth_mbps", bandwidth}, {"unknown_fraction", unknownFraction},
    {"wire_reference", "NDN_DATA_ONLY"},
    {"rtt_samples", rtts.size()},
    {"median_rtt_ms", rtts.empty() ? NativeJson(nullptr) : NativeJson(median(rtts))},
    {"median_wire_equivalent_mbps", rates.empty() ? NativeJson(nullptr) : NativeJson(median(rates))},
    {"median_absolute_relative_error", errors.empty() ? NativeJson(nullptr) : NativeJson(median(errors))}};
  summary["accuracy_pass"] = bandwidth > 0 && !errors.empty() &&
    median(errors) <= 0.25 && unknownFraction <= 0.20;
  return summary;
}

NativeJson summarizeMatrix(const NativeJson& cells)
{
  const std::set<std::pair<unsigned, unsigned>> expected{
    {1, 10}, {4, 10}, {10, 10}, {4, 2}, {4, 30}, {4, 70}};
  if (!cells.is_array() || cells.size() != expected.size())
    throw std::runtime_error("calibration matrix must contain the six frozen cells");
  std::set<std::pair<unsigned, unsigned>> seen;
  std::map<std::pair<unsigned, unsigned>, double> rates;
  NativeJson summaries = NativeJson::array();
  bool cellPass = true;
  for (const auto& cell : cells) {
    if (!cell.at("bandwidth_mbps").is_number_unsigned() ||
        !cell.at("delay_ms").is_number_unsigned())
      throw std::runtime_error("calibration matrix link parameters must be unsigned integers");
    const auto key = std::make_pair(cell.at("bandwidth_mbps").get<unsigned>(),
                                    cell.at("delay_ms").get<unsigned>());
    if (cell.at("bandwidth_mbps") != key.first || cell.at("delay_ms") != key.second ||
        !expected.count(key) || !seen.insert(key).second ||
        !cell.value("control", NativeJson(nullptr)).is_null() ||
        !cell.value("authorization_case", NativeJson(nullptr)).is_null())
      throw std::runtime_error("calibration matrix contains an unexpected or duplicate cell");
    const auto& native = cell.at("native_result");
    if (native.at("schema") != "ndnsf-network-probe-calibration-v1" ||
        native.at("expected_bandwidth_mbps") != key.first)
      throw std::runtime_error("calibration matrix native result binding mismatch");
    auto summary = summarize(native.at("records"), key.first);
    const auto& records = native.at("records");
    bool spanPass = records.size() >= 5;
    std::uint64_t first = 0, previous = 0;
    for (const auto& record : records) {
      const auto response = record.value("response", NativeJson::object());
      const auto stamp = response.value("completed_at_ms", NativeJson(nullptr));
      if (!stamp.is_number_unsigned() || stamp.get<std::uint64_t>() == 0 ||
          stamp.get<std::uint64_t>() <= previous) {
        spanPass = false;
        continue;
      }
      previous = stamp.get<std::uint64_t>();
      if (first == 0) first = previous;
    }
    spanPass = spanPass && previous >= first && previous - first >= 60'000;
    summary["delay_ms"] = key.second;
    summary["repetition_span_pass"] = spanPass;
    cellPass = cellPass && spanPass && summary.at("accuracy_pass").get<bool>();
    if (!summary.at("median_wire_equivalent_mbps").is_null())
      rates.emplace(key, summary.at("median_wire_equivalent_mbps").get<double>());
    summaries.push_back(std::move(summary));
  }
  double minRate = std::numeric_limits<double>::infinity(), maxRate = 0;
  for (const auto delay : {2U, 10U, 30U, 70U}) {
    const auto found = rates.find({4, delay});
    if (found != rates.end()) {
      minRate = std::min(minRate, found->second);
      maxRate = std::max(maxRate, found->second);
    }
  }
  const bool complete = rates.size() == expected.size();
  const auto ratio = complete ? maxRate / minRate : 0.0;
  const bool rttPass = complete && std::isfinite(ratio) && ratio <= 1.25;
  const bool bandwidthPass = complete && rates.at({1, 10}) < rates.at({4, 10}) &&
    rates.at({4, 10}) < rates.at({10, 10});
  return NativeJson{{"schema", "ndnsf-network-probe-calibration-matrix-v1"},
    {"cells", std::move(summaries)}, {"cell_accuracy_pass", cellPass},
    {"fixed_bandwidth_rate_ratio", complete ? NativeJson(ratio) : NativeJson(nullptr)},
    {"rtt_stability_pass", rttPass}, {"bandwidth_order_pass", bandwidthPass},
    {"qualification_pass", cellPass && rttPass && bandwidthPass}};
}
} // namespace ndnsf::di::probe_calibration

#ifndef NDNSF_PROBE_CALIBRATION_NO_MAIN
namespace {
using namespace ndn_service_framework;

bool hasProbeGrant(const ServiceUser& user)
{
  const auto grants = user.getAllowedServices();
  return std::any_of(grants.begin(), grants.end(), [] (const auto& grant) {
    return std::get<1>(grant) == NETWORK_PROBE_SERVICE;
  });
}

void writeResult(const NativeJson& config, const NativeJson& result)
{
  std::ofstream output(config.at("output").get<std::string>());
  if (!output || !(output << nativeCanonicalJson(result) << '\n'))
    throw std::runtime_error("cannot write calibration result");
  output.close();
  if (!output)
    throw std::runtime_error("cannot finish calibration result");
}

int runUser(ndn::Face& face, ndn::KeyChain& keyChain,
            const ndn::security::Certificate& identityCert,
            const ndn::security::Certificate& controllerCert, const NativeJson& config)
{
  const auto authorizationCase = config.value("authorization_case", "measure");
  if (authorizationCase != "measure" && authorizationCase != "deny" && authorizationCase != "revoke")
    throw std::runtime_error("invalid authorization case");
  ServiceUser user(face, ndn::Name(config.at("group").get<std::string>()),
    identityCert, identityCert, controllerCert,
    config.at("trust_schema").get<std::string>(), keyChain);
  user.init();
  user.fetchPermissionsFromController(controllerCert.getIdentity());
  const std::vector<ndn::Name> providers{
    ndn::Name(config.at("source").get<std::string>()),
    ndn::Name(config.at("observer").get<std::string>())};
  const auto contextService = ndn::Name(config.value("context_service", "/NDNSF/Calibration"));
  const auto contextReady = [&] {
    return std::all_of(providers.begin(), providers.end(), [&] (const auto& provider) {
      return user.isRequestBootstrapReady(contextService, {provider});
    });
  };
  if (authorizationCase != "measure") {
    if (config.value("repeats", 1U) != 1 || config.value("warmup", 0U) != 0 ||
        config.value("interval_ms", 0U) != 0)
      throw std::runtime_error("authorization case requires one bounded round");
    if (!pumpUntil(face, contextReady, Clock::now() + std::chrono::seconds(15)))
      throw std::runtime_error("original context authorization bootstrap timed out");
    if (authorizationCase == "deny") {
      if (hasProbeGrant(user))
        throw std::runtime_error("inference-only User unexpectedly has probe permission");
      NativeJson result{{"schema", "ndnsf-network-probe-authorization-v1"},
        {"case", authorizationCase}, {"pass", true}, {"original_context_ready", true},
        {"probe_grant_present", false}, {"denial", measure(face, user, config, 1, true)}};
      writeResult(config, result);
      std::cout << "NDNSF_PROBE_AUTHORIZATION_PASS " << nativeCanonicalJson(result) << std::endl;
      face.shutdown();
      return 0;
    }
  }
  if (!pumpUntil(face, [&] {
    return user.isRequestBootstrapReady(ndn::Name(NETWORK_PROBE_SERVICE), providers);
  }, Clock::now() + std::chrono::seconds(15)))
    throw std::runtime_error("real Controller probe authorization bootstrap timed out");
  if (authorizationCase == "revoke") {
    auto positive = measure(face, user, config, 1);
    if (positive.at("rtt_ms").is_null())
      throw std::runtime_error("pre-revocation probe did not produce an authenticated RTT");
    const auto before = user.getControllerVersion();
    if (!before || !contextReady() || !hasProbeGrant(user))
      throw std::runtime_error("pre-revocation authority is incomplete");
    const std::filesystem::path trigger(config.at("revoke_trigger_file").get<std::string>());
    if (!trigger.is_absolute() || trigger.empty() || std::filesystem::exists(trigger))
      throw std::runtime_error("revocation trigger must be a fresh absolute path");
    std::ofstream triggerOutput(trigger);
    triggerOutput << "revoke\n";
    triggerOutput.close();
    if (!triggerOutput)
      throw std::runtime_error("cannot write revocation trigger");
    auto nextRefresh = Clock::now();
    if (!pumpUntil(face, [&] {
      if (Clock::now() >= nextRefresh) {
        user.fetchPermissionsFromController(controllerCert.getIdentity());
        nextRefresh = Clock::now() + std::chrono::milliseconds(500);
      }
      const auto current = user.getControllerVersion();
      return current && current->compare(*before) > 0 && !hasProbeGrant(user) && contextReady();
    }, Clock::now() + std::chrono::seconds(20)))
      throw std::runtime_error("Controller probe revocation did not converge");
    const auto after = user.getControllerVersion();
    if (!after || after->compare(*before) <= 0 || hasProbeGrant(user))
      throw std::runtime_error("post-revocation authority changed before denial");
    NativeJson result{{"schema", "ndnsf-network-probe-authorization-v1"},
      {"case", authorizationCase}, {"pass", true}, {"original_context_ready", true},
      {"probe_grant_present", false}, {"controller_before", before->toString()},
      {"controller_after", after->toString()}, {"positive", std::move(positive)},
      {"denial", measure(face, user, config, 2, true)}};
    writeResult(config, result);
    std::cout << "NDNSF_PROBE_AUTHORIZATION_PASS " << nativeCanonicalJson(result) << std::endl;
    face.shutdown();
    return 0;
  }
  const auto repeats = config.value("repeats", 5U);
  const auto warmup = config.value("warmup", 1U);
  const auto intervalMs = config.value("interval_ms", 0U);
  if (!ndnsf::di::probe_calibration::isValidRepetitionBudget(repeats, warmup, intervalMs))
    throw std::runtime_error("invalid calibration repetition budget");
  const auto bandwidth = config.value("expected_bandwidth_mbps", 0.0);
  if (!std::isfinite(bandwidth) || bandwidth < 0)
    throw std::runtime_error("invalid reference bandwidth");
  NativeJson records = NativeJson::array();
  for (std::size_t i = 0; i < warmup + repeats; ++i) {
    auto record = measure(face, user, config, i + 1);
    std::cout << "NDNSF_PROBE_CALIBRATION_ROUND " << nativeCanonicalJson(record) << std::endl;
    if (i >= warmup)
      records.push_back(std::move(record));
    if (intervalMs > 0 && i + 1 < warmup + repeats) {
      const auto next = Clock::now() + std::chrono::milliseconds(intervalMs);
      pumpUntil(face, [&] { return Clock::now() >= next; }, next + std::chrono::milliseconds(50));
    }
  }
  const auto summary = ndnsf::di::probe_calibration::summarize(std::move(records), bandwidth);
  writeResult(config, summary);
  face.shutdown();
  return config.value("require_accuracy", false) && !summary.at("accuracy_pass").get<bool>() ? 1 : 0;
}
} // namespace

int main(int argc, char** argv)
{
  if (argc == 2 && std::string(argv[1]) == "--help") {
    std::cout << "Usage: DI_NetworkProbeCalibration --config FILE\n";
    return 0;
  }
  try {
    if (argc != 3 || std::string(argv[1]) != "--config")
      throw std::runtime_error("Usage: DI_NetworkProbeCalibration --config FILE");
    std::ifstream input(argv[2]);
    NativeJson config;
    if (!input || !(input >> config) || !config.is_object())
      throw std::runtime_error("invalid calibration config");
    const auto role = config.at("role").get<std::string>();
    if (role == "matrix") {
      const auto result = ndnsf::di::probe_calibration::summarizeMatrix(config.at("cells"));
      writeResult(config, result);
      return result.at("qualification_pass").get<bool>() ? 0 : 1;
    }
    if (role != "provider" && role != "user")
      throw std::runtime_error("invalid calibration role");
    const ndn::Name identity(config.at("identity").get<std::string>());
    const ndn::Name controller(config.at("controller").get<std::string>());
    auto controllerCert = ndn::io::load<ndn::security::Certificate>(
      config.at("controller_certificate").get<std::string>());
    if (!controllerCert || !controllerCert->isValid() || controllerCert->getIdentity() != controller)
      throw std::runtime_error("invalid Controller certificate");
    ndn::Face face;
    ndn::KeyChain keyChain;
    try {
      keyChain.getPib().getIdentity(identity);
    }
    catch (const std::exception&) {
      keyChain.createIdentity(identity, ndn::RsaKeyParams(2048));
    }
    const auto identityCert = ensureControllerSignedCertificate(face, keyChain,
      controller, identity, identity, config.at("bootstrap_token").get<std::string>());
    keyChain.setDefaultIdentity(keyChain.getPib().getIdentity(identity));
    CertificatePublisher publisher(face, keyChain, identityCert.getName());
    if (role == "user")
      return runUser(face, keyChain, identityCert, *controllerCert, config);
    const auto duration = config.value("duration_ms", 600000U);
    if (duration == 0 || duration > 600000)
      throw std::runtime_error("invalid Provider runtime budget");
    ServiceProvider provider(face, ndn::Name(config.at("group").get<std::string>()),
      identityCert, *controllerCert, config.at("trust_schema").get<std::string>());
    auto registration = provider.addScopedTargetedService(ndn::Name(NETWORK_PROBE_SERVICE),
      ndnsf::di::makeNativePlacementMeasurementTargetedHandler(provider));
    provider.setHandlerThreads(1);
    provider.setAckThreads(1);
    provider.fetchPermissionsFromController(controller);
    provider.init();
    bool stopped = false;
    boost::asio::signal_set signals(face.getIoContext(), SIGINT, SIGTERM);
    signals.async_wait([&] (const boost::system::error_code& error, int) {
      if (!error) stopped = true;
    });
    const auto ready = [&] {
      return provider.hasProviderPermissionForService(ndn::Name(NETWORK_PROBE_SERVICE)) &&
        provider.isRequestDecryptionReady() &&
        provider.getControllerVersion(ndn::Name(NETWORK_PROBE_SERVICE)).has_value();
    };
    if (!pumpUntil(face, [&] { return stopped || ready(); },
                   Clock::now() + std::chrono::seconds(15)) || stopped || !ready())
      throw std::runtime_error("real Controller Provider authorization bootstrap failed: permission=" +
        std::to_string(provider.hasProviderPermissionForService(ndn::Name(NETWORK_PROBE_SERVICE))) +
        " decryption=" + std::to_string(provider.isRequestDecryptionReady()) +
        " version=" + std::to_string(provider.getControllerVersion(ndn::Name(NETWORK_PROBE_SERVICE)).has_value()));
    std::cout << "NDNSF_PROBE_CALIBRATION_PROVIDER_READY identity=" << identity
              << " boot=" << provider.getProviderBootEpoch() << std::endl;
    pumpUntil(face, [&] { return stopped; }, Clock::now() + std::chrono::milliseconds(duration));
    registration.close();
    face.shutdown();
    return 0;
  }
  catch (const std::exception& error) {
    std::cerr << "NDNSF_PROBE_CALIBRATION_FAILED " << error.what() << '\n';
    return 2;
  }
}
#endif
