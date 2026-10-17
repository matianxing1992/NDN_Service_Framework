#include "common.hpp"

#include <algorithm>
#include <iostream>
#include <set>

namespace {
struct ResponseVerdict { bool identity, payload, invalid; };
ResponseVerdict checkResponse(const sim203::nsf::ResponseMessage& response,
                             const ndn::Name& target, const std::string& command) {
  const bool producer = !response.getDataName().empty() && target.isPrefixOf(ndn::Name(response.getDataName()));
  const bool identity = response.getProviderIdentity() == target.toUri() && producer &&
                        !response.getWireDigest().empty();
  const bool payload = response.getStatus() && identity && sim203::stringPayload(response.getPayload()) == command;
  const bool wrongProducer =
    (!response.getProviderIdentity().empty() && response.getProviderIdentity() != target.toUri()) ||
    (!response.getDataName().empty() && !producer);
  return {identity, payload, wrongProducer || (response.getStatus() && !payload)};
}
}

int main(int argc, char** argv) try {
  using namespace sim203;
  Options options(argc, argv);
  const std::string mode = options.get("--mode", "basic");
  if (mode != "basic" && mode != "targeted" && mode != "all" &&
      mode != "all-raw" && mode != "single")
    throw std::invalid_argument("unknown mode");
  const ndn::Name identity(options.get("--identity", "/example/hello/user"));
  const ndn::Name target(options.get("--target", "/example/hello/provider/A"));
  const ndn::Name other(options.get("--other", "/example/hello/provider/B"));
  const ndn::Name service(options.get("--service", "/HELLO"));
  const ndn::Name controller(options.get("--controller", "/example/hello/controller"));
  const auto run = options.get("--run-id");
  if (run.empty() || run.size() > 32 || target.empty()) throw std::invalid_argument("invalid run/target");
  const int warmup = options.number("--warmup-ms", 10000);
  const int window = options.number("--window-ms", 60000);
  const int count = options.number("--count", 0);
  const int timeout = options.number("--timeout-ms", 5000);
  const int interval = options.number("--interval-ms", 0);
  const int rateRps = options.number("--rate-rps", 0);
  const int maxOutstanding = options.number("--max-outstanding", 512);
  const bool fixedRate = rateRps != 0;
  if (rateRps > 1000000 || maxOutstanding < 1 ||
      (fixedRate && (count != 0 || interval != 0 || window == 0))) {
    throw std::invalid_argument("fixed rate requires a window, no count/interval, and a pending bound");
  }
  const int payloadBytes = options.number("--payload-bytes", 64);
  const bool hasDiscoveryPayload = options.values.count("--discovery-payload") != 0;
  const auto discoveryPayload = options.get("--discovery-payload");
  if (discoveryPayload.size() > nsf::RequestMessage::MAX_DISCOVERY_PAYLOAD_SIZE) {
    throw std::invalid_argument("discovery payload exceeds 4096 bytes");
  }
  if (payloadBytes < 1 || payloadBytes > 64) throw std::invalid_argument("payload bytes must be 1..64");
  if (timeout == 0 || (window == 0 && count == 0)) throw std::invalid_argument("empty workload");
  Records records(options.get("--records"));
  ndn::Face face; ndn::KeyChain keys; ndn::Scheduler scheduler(face.getIoContext());
  const auto cert = certificate(keys, identity);
  keys.setDefaultIdentity(keys.getPib().getIdentity(identity));
  nsf::CertificatePublisher publisher(face, keys, identity);
  struct Attempt {
    uint64_t sequence, begin, scheduled = 0;
    std::string command, requestId;
    bool measured, prime = false, terminal = false, nativeSettled = false;
    std::set<std::string> verifiedProviders;
    ndn::scheduler::EventId deadline;
  };
  uint64_t sequence = 0, terminals = 0, measured = 0, invalid = 0;
  uint64_t primeAttempts = 0, primeTerminals = 0;
  size_t pendingNative = 0, peakPending = 0;
  uint64_t rateTick = 0, scheduleStart = 0, scheduledNs = 0;
  std::map<std::string, std::shared_ptr<Attempt>> pendingRequests;
  bool started = false, stopped = false, draining = false, drainTimedOut = false;
  bool capacityExceeded = false;
  uint64_t t0 = 0, t1 = 0;
  std::function<void()> next;
  std::function<void()> check;
  std::function<void()> tick;
  std::function<void()> startWorkload;
  std::function<void(bool)> submit;
  const auto finish = [&](const std::shared_ptr<Attempt>& attempt, const std::string& terminal,
                          const nsf::ResponseMessage* response) {
    if (attempt->terminal) {
      records.write({{"event", attempt->prime ? "prime_duplicate_observed" : "duplicate_observed"},
                     {"command", attempt->command}}); return;
    }
    attempt->terminal = true;
    if (attempt->prime) ++primeTerminals;
    else ++terminals;
    attempt->deadline.cancel();
    Json row{{"event", attempt->prime ? "prime_terminal" : "terminal"},
             {"run_id", run}, {"sequence", attempt->sequence},
             {"command", attempt->command}, {"request_id", attempt->requestId},
             {"mode", mode}, {"target", target.toUri()}, {"measured", attempt->measured},
             {"begin_ns", attempt->begin},
             {"deadline_ns", attempt->begin + static_cast<uint64_t>(timeout) * 1000000},
             {"terminal", terminal}};
    if (attempt->prime) row.erase("sequence");
    else if (fixedRate) row["scheduled_ns"] = attempt->scheduled;
    if (response) {
      // Failed responses may legitimately have no echo or remote Data (e.g.
      // retrieval failure). Preserve them in the failure denominator. A
      // contradictory claimed producer or malformed success invalidates the run.
      const auto verdict = checkResponse(*response, target, attempt->command);
      row["response_status"] = response->getStatus(); row["payload_verified"] = verdict.payload;
      row["response_identity_verified"] = verdict.identity;
      row["provider"] = response->getProviderIdentity(); row["data_name"] = response->getDataName();
      row["wire_digest"] = response->getWireDigest(); row["error"] = response->getErrorInfo();
      const auto boot = response->getTokens().find("targeted.providerBootEpoch");
      if (boot != response->getTokens().end()) row["provider_boot"] = boot->second;
      if (response->hasProviderSecurityProfile())
        row["provider_security_profile"] = response->getProviderSecurityProfile().toUri();
      row["terminal"] = verdict.invalid ? "invalid_response" : verdict.payload ? "success" : "response_error";
      if (verdict.invalid) ++invalid;
    }
    row["terminal_ns"] = nowNs();
    if (row["terminal"] == "success" && nowNs() >= attempt->begin + static_cast<uint64_t>(timeout) * 1000000)
      row["terminal"] = "timeout";
    const bool success = row["terminal"] == "success";
    if (attempt->prime) row["latency_ms"] = (nowNs() - attempt->begin) / 1000000.0;
    records.write(std::move(row));
    if (attempt->prime) {
      // Prime uses the real invocation and oracle, but never enters the offered
      // workload or its counters. Only a verified completion starts the clock.
      if (success && attempt->nativeSettled && invalid == 0) startWorkload();
      else { stopped = true; shutdownFace(face); }
      return;
    }
    if (attempt->nativeSettled && (!fixedRate || stopped))
      scheduler.schedule(ndn::time::milliseconds(interval), [&] { next(); });
  };
  const auto settle = [&](const std::shared_ptr<Attempt>& attempt, const std::string& terminal,
                          const nsf::ResponseMessage* response) {
    const bool overDeadline = nowNs() >= attempt->begin + static_cast<uint64_t>(timeout) * 1000000;
    // Late/duplicate data cannot replace a timeout but must still satisfy the
    // same response oracle; provenance violations invalidate the run.
    if (response && (attempt->terminal || attempt->nativeSettled || overDeadline)) {
      const auto verdict = checkResponse(*response, target, attempt->command);
      if (verdict.invalid) ++invalid;
      records.write({{"event", attempt->prime ? "prime_late_response_checked" : "late_response_checked"},
                     {"command", attempt->command},
                     {"provider", response->getProviderIdentity()}, {"invalid", verdict.invalid},
                     {"payload_verified", verdict.payload}, {"identity_verified", verdict.identity},
                     {"mono_ns", nowNs()}});
    }
    if (attempt->nativeSettled) {
      records.write({{"event", attempt->prime ? "prime_duplicate_native_terminal" : "duplicate_native_terminal"},
                     {"command", attempt->command}}); return;
    }
    attempt->nativeSettled = true;
    if (!attempt->prime) {
      --pendingNative;
      const auto pending = pendingRequests.find(attempt->requestId);
      if (pending != pendingRequests.end() && pending->second == attempt)
        pendingRequests.erase(pending);
    }
    if (attempt->terminal) {
      records.write({{"event", attempt->prime ? "prime_late_native_terminal" : "late_native_terminal"},
                     {"command", attempt->command},
                     {"native_terminal", terminal}, {"mono_ns", nowNs()}});
      if (!attempt->prime && (!fixedRate || stopped))
        scheduler.schedule(ndn::time::milliseconds(interval), [&] { next(); });
    } else if (overDeadline) {
      finish(attempt, "timeout", nullptr);
    } else finish(attempt, terminal, response);
  };
  // Destroy/join ServiceUser workers before any captured state above is released.
  // Workers only copy results and post; all experiment state runs on this Face.
  {
  nsf::ServiceUser user(face, ndn::Name(options.get("--group", "/example/hello/group")), cert,
                        certificate(keys, controller), options.get("--trust"));
  user.init(); user.setUseTokens(true);
  user.setTimelineTrace(options.number("--timeline-trace", 1) != 0);
  if (fixedRate) {
    // Match the other fixed-rate generators: the requester owns the pending
    // bound, and each offered call enters the native publication path directly.
    auto admission = user.getAdaptiveAdmissionOptions();
    admission.enabled = false;
    user.setAdaptiveAdmissionControl(admission);
  }
  user.fetchPermissionsFromController(controller);
  next = [&] {
    const auto now = nowNs();
    if ((count && measured >= static_cast<uint64_t>(count)) || (!count && now >= t1)) {
      if (!stopped) {
        stopped = true;
        records.write({{"event", "cohort_closed"}, {"run_id", run}, {"t0_ns", t0}, {"t1_ns", t1},
                     {"attempts", sequence}, {"terminals", terminals}, {"measured", measured}});
        if (fixedRate) {
          // A missing native callback retains its slot, preventing an unbounded
          // queue of expired requests. Stop this actor if the final drain fails.
          scheduler.schedule(ndn::time::milliseconds(timeout) + ndn::time::seconds(2), [&] {
            if (pendingNative != 0 || terminals != sequence) {
              drainTimedOut = true;
              records.write({{"event", "drain_timeout"}, {"pending_native", pendingNative},
                             {"attempts", sequence}, {"terminals", terminals}});
            }
            shutdownFace(face);
          });
        }
      }
      // Allow queued publication/refill work to drain before actor destruction.
      if (terminals == sequence && (!fixedRate || pendingNative == 0) && !draining) {
        draining = true;
        scheduler.schedule(ndn::time::seconds(2), [&] { shutdownFace(face); });
      }
      return;
    }
    if (stopped || (!fixedRate && pendingNative != 0)) return;
    if (fixedRate && pendingNative >= static_cast<size_t>(maxOutstanding)) {
      capacityExceeded = true;
      stopped = true;
      records.write({{"event", "rate_capacity_exceeded"}, {"scheduled_ns", scheduledNs},
                     {"mono_ns", now}, {"pending_native", pendingNative}});
      shutdownFace(face);
      return;
    }
    submit(false);
  };
  submit = [&](bool prime) {
    auto attempt = std::make_shared<Attempt>();
    attempt->prime = prime;
    attempt->sequence = prime ? 0 : sequence + 1;
    attempt->scheduled = prime ? 0 : scheduledNs;
    attempt->command = run + ":" + (prime ? std::string("prime") : std::to_string(attempt->sequence));
    attempt->command.resize(payloadBytes, '_');
    nsf::RequestMessage request;
    ndn::Buffer payload(reinterpret_cast<const uint8_t*>(attempt->command.data()), payloadBytes);
    request.setPayload(payload, payload.size());
    if (hasDiscoveryPayload) {
      request.setDiscoveryPayload(ndn::Buffer(discoveryPayload.begin(), discoveryPayload.end()));
    }
    const auto onTimeout = [&, attempt](const ndn::Name& id) {
      boost::asio::post(face.getIoContext(), [&, attempt, id] {
        if (attempt->requestId.empty()) attempt->requestId = id.toUri();
        else if (attempt->requestId != id.toUri()) {
          ++invalid;
          records.write({{"event", attempt->prime ? "prime_request_id_mismatch" : "request_id_mismatch"},
                         {"request_id", attempt->requestId},
                         {"callback_request_id", id.toUri()}});
        }
        settle(attempt, "timeout", nullptr);
      });
    };
    const auto onResponse = [&, attempt](const nsf::ResponseMessage& response) {
      boost::asio::post(face.getIoContext(), [&, attempt, response] {
        if (fixedRate && !response.getDataName().empty()) {
          // The native callback selects an Attempt. Independently check its
          // wire name so concurrent requests cannot exchange valid echoes.
          const auto producer = ndn::Name(response.getProviderIdentity());
          const auto expected = nsf::makeResponseNameV2(
            producer, identity, service, ndn::Name(attempt->requestId));
          const auto actual = nsf::stripTrailingResponseSegments(ndn::Name(response.getDataName()));
          if (producer.empty() || actual != expected) {
            ++invalid;
            records.write({{"event", attempt->prime ? "prime_response_binding_mismatch"
                                                  : "response_binding_mismatch"},
                           {"request_id", attempt->requestId}, {"data_name", response.getDataName()}});
            settle(attempt, "invalid_response", nullptr);
            return;
          }
        }
        if (mode == "all" || mode == "all-raw") {
          const auto& producer = response.getProviderIdentity();
          const bool candidate = producer == target.toUri() || producer == other.toUri();
          const auto verdict = candidate ? checkResponse(response, ndn::Name(producer), attempt->command)
                                         : ResponseVerdict{false, false, true};
          records.write({{"event", attempt->prime ? "prime_all_response" : "all_response"},
                         {"request_id", attempt->requestId},
                         {"provider", producer}, {"data_name", response.getDataName()},
                         {"payload_verified", verdict.payload}, {"identity_verified", verdict.identity},
                         {"mono_ns", nowNs()}});
          if (!verdict.payload || !attempt->verifiedProviders.insert(producer).second) {
            ++invalid;
            settle(attempt, "invalid_response", nullptr);
          }
          else if (attempt->verifiedProviders.size() == 2) settle(attempt, "success", nullptr);
          return;
        }
        settle(attempt, "response", &response);
      });
    };
    attempt->begin = nowNs();
    if (!prime && !count && attempt->begin >= t1) { next(); return; }
    attempt->measured = !prime && attempt->begin >= t0;
    if (prime) {
      ++primeAttempts;
      records.write({{"event", "prime_started"}, {"run_id", run}, {"mode", mode},
                     {"target", target.toUri()}, {"begin_ns", attempt->begin},
                     {"deadline_ns", attempt->begin + static_cast<uint64_t>(timeout) * 1000000}});
    } else {
      ++sequence;
      if (attempt->measured) ++measured;
      ++pendingNative;
      peakPending = std::max(peakPending, pendingNative);
    }
    attempt->deadline = scheduler.schedule(ndn::time::milliseconds(timeout), [&, attempt] {
      if (!attempt->terminal) finish(attempt, "timeout", nullptr);
    });
    try {
      ndn::Name id;
      if (mode == "targeted")
        id = user.RequestServiceTargeted(target, service, request, timeout, onTimeout, onResponse);
      else if (mode == "all")
        id = user.RequestService(std::vector<ndn::Name>{target, other}, service, request,
                                 1000, nsf::ServiceUser::AckSelectionStrategy::AllSelected,
                                 timeout, onTimeout, onResponse);
      else if (mode == "all-raw")
        id = user.RequestService(std::vector<ndn::Name>{target, other}, service, request,
                                 timeout, onTimeout, onResponse, nsf::tlv::AllSelected);
      else if (mode == "single")
        id = user.RequestService(std::vector<ndn::Name>{target, other}, service, request, 1000,
          nsf::ServiceUser::AckCandidatesHandler([&target](const auto& candidates) {
            std::vector<nsf::AckSelectionCandidate> selected;
            for (const auto& candidate : candidates) {
              if (candidate.providerName == target && candidate.ack.getStatus()) {
                selected.push_back(candidate);
                break;
              }
            }
            return selected;
          }), timeout, onTimeout, onResponse);
      else
        id = user.RequestService(std::vector<ndn::Name>{target}, service, request, timeout,
                                 onTimeout, onResponse, nsf::tlv::FirstResponding);
      attempt->requestId = id.toUri();
      if (id.empty() && !attempt->terminal) settle(attempt, "submit_error", nullptr);
      else if (!id.empty()) {
        if (!prime && !pendingRequests.emplace(attempt->requestId, attempt).second) {
          ++invalid;
          records.write({{"event", "duplicate_request_id"}, {"request_id", attempt->requestId}});
        }
        if (prime) {
          records.write({{"event", "prime_submitted"}, {"request_id", attempt->requestId},
                         {"begin_ns", attempt->begin}});
        } else if (fixedRate) {
          records.write({{"event", "submitted"}, {"sequence", attempt->sequence},
                         {"request_id", attempt->requestId}, {"measured", attempt->measured},
                         {"scheduled_ns", attempt->scheduled}, {"begin_ns", attempt->begin}});
        }
      }
    } catch (const std::exception& error) {
      records.write({{"event", prime ? "prime_submit_exception" : "submit_exception"},
                     {"error", error.what()}});
      settle(attempt, "submit_error", nullptr);
    }
  };
  tick = [&] {
    const auto now = nowNs();
    if (stopped || now >= t1) {
      next();
      return;
    }
    const auto dueAt = [&](uint64_t index) {
      return scheduleStart + (index / rateRps) * 1000000000ULL +
             (index % rateRps) * 1000000000ULL / rateRps;
    };
    // Keep each absolute slot, including late slots, as the matched baseline
    // generators do. Actual submission times expose any pacing delay.
    scheduledNs = dueAt(rateTick++);
    next();
    const auto after = nowNs();
    const auto due = std::min<uint64_t>(dueAt(rateTick), t1);
    scheduler.schedule(ndn::time::nanoseconds(due > after ? due - after : 0), tick);
  };
  startWorkload = [&] {
    started = true;
    const auto readyNs = nowNs();
    scheduleStart = readyNs;
    t0 = readyNs + static_cast<uint64_t>(warmup) * 1000000;
    t1 = t0 + static_cast<uint64_t>(window) * 1000000;
    records.write({{"event", "ready"}, {"run_id", run}, {"mode", mode}, {"target", target.toUri()},
                   {"interval_ms", interval},
                   {"rate_rps", rateRps}, {"max_outstanding", maxOutstanding},
                   {"admission_control_enabled", user.getAdaptiveAdmissionOptions().enabled},
                   {"tokens_enabled", true}, {"prime_completed", fixedRate},
                   {"ready_ns", readyNs}, {"t0_ns", t0}, {"t1_ns", t1}});
    std::cout << "SPEC203_REQUESTER_READY " << run << std::endl;
    if (!count) scheduler.schedule(ndn::time::milliseconds(warmup) +
                                   ndn::time::milliseconds(window), [&] { next(); });
    if (fixedRate) tick();
    else next();
  };
  const auto readinessStart = Clock::now();
  check = [&] {
    if (user.isRequestBootstrapReady(service, mode == "all" || mode == "all-raw" ||
        mode == "single" ?
        std::vector<ndn::Name>{target, other} : std::vector<ndn::Name>{target})) {
      if (fixedRate) submit(true);
      else startWorkload();
    } else if (Clock::now() - readinessStart > std::chrono::seconds(45)) {
      records.write({{"event", "readiness_timeout"}}); shutdownFace(face);
    } else scheduler.schedule(ndn::time::milliseconds(100), check);
  };
  scheduler.schedule(ndn::time::milliseconds(100), check);
  boost::asio::signal_set signals(face.getIoContext(), SIGINT, SIGTERM);
  signals.async_wait([&](const auto&, int) { shutdownFace(face); });
  face.processEvents();
  }
  // Fixed-rate slots begin at zero and end strictly before the window closes.
  const uint64_t plannedAttempts = fixedRate ?
    ((static_cast<uint64_t>(warmup) + window) * rateRps + 999) / 1000 : 0;
  records.write({{"event", "shutdown"}, {"started", started}, {"stopped", stopped},
                 {"invalid_responses", invalid}, {"attempts", sequence}, {"terminals", terminals},
                 {"planned_attempts", plannedAttempts},
                 {"prime_attempts", primeAttempts}, {"prime_terminals", primeTerminals},
                 {"pending_native", pendingNative}, {"peak_pending", peakPending},
                 {"capacity_exceeded", capacityExceeded}, {"drain_timeout", drainTimedOut}});
  const bool rateComplete = !fixedRate ||
    (primeAttempts == 1 && primeTerminals == 1 && pendingNative == 0 && measured != 0 &&
     sequence == plannedAttempts);
  return started && stopped && sequence == terminals && invalid == 0 &&
         !capacityExceeded && !drainTimedOut && rateComplete ? 0 : 2;
} catch (const std::exception& error) {
  std::cerr << "SPEC203_REQUESTER_ERROR " << error.what() << std::endl; return 2;
}
