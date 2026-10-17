#include "dynamic-common.hpp"
#include <filesystem>
#include <iostream>
#include <limits>
#include <openssl/sha.h>
#include <set>

namespace {
using namespace dynamic204;

void
check(bool condition, const std::string& reason)
{
  if (!condition) throw std::runtime_error(reason);
}

std::vector<Json>
readRecords(const std::filesystem::path& path)
{
  check(std::filesystem::file_size(path) <= 16 * 1024 * 1024,"record file too large");
  std::ifstream input(path);
  check(static_cast<bool>(input),"missing record file");
  std::vector<Json> rows;
  std::string line;
  while (std::getline(input,line)) {
    check(line.size() <= 16384 && rows.size() < 100000,"record bounds");
    rows.push_back(Json::parse(line));
  }
  check(input.eof(),"record read failure");
  return rows;
}

uint64_t
bindingNumber(const Json& row, const char* field)
{
  const auto value = row.at("response_binding").at(field).get<std::string>();
  check(!value.empty() && value.find_first_not_of("0123456789") == std::string::npos,
        "invalid response binding number");
  size_t consumed = 0;
  const auto result = std::stoull(value, &consumed);
  check(consumed == value.size() && result != 0, "empty response binding identity");
  return result;
}

void
checkRecoveryRecords(const Json& manifest, const Json& run,
                     const std::vector<PlannedTask>& tasks,
                     const std::vector<Json>& users, const std::vector<Json>& providers)
{
  const auto condition = manifest.at("recovery_condition").get<std::string>();
  check(condition == "prepared" || condition == "reregister", "invalid recovery condition");
  const auto origin = unsignedField(run, "origin_ns");
  const auto& anchor = manifest.at("clock_anchor");
  const auto anchorMono = unsignedField(anchor, "mono_ns");
  check(origin >= anchorMono, "recovery clock origin");
  std::map<std::string, Json> warm, preparation, warmExecutions;
  for (const auto& row : users) {
    if (row.at("event") == "warmup_response")
      check(warm.emplace(row.at("provider").get<std::string>(), row).second,
            "duplicate warmup response");
  }
  for (const auto& row : providers) {
    if (row.at("event") == "recovery_prepared")
      check(preparation.emplace(row.at("provider").get<std::string>(), row).second,
            "duplicate recovery preparation");
    if (row.at("event") == "execution"
        && decodeTaskInput(row.at("command").get<std::string>()).warmup)
      check(warmExecutions.emplace(row.at("provider").get<std::string>(), row).second,
            "duplicate recovery warmup execution");
  }
  check(warm.size() == 2 && preparation.size() == 2 && warmExecutions.size() == 2,
        "missing recovery prerequisites");
  for (const auto& name : {"/example/hello/provider/A", "/example/hello/provider/B"}) {
    const auto& response = warm.at(name);
    const auto& prepared = preparation.at(name);
    const auto before = unsignedField(prepared, "before_generation");
    const auto after = unsignedField(prepared, "after_generation");
    check(prepared.at("condition") == condition && before > 0
          && (condition == "prepared" ? after == before : after > before),
          "recovery generation transition");
    check(unsignedField(warmExecutions.at(name), "registration_generation") == before
          && bindingNumber(response, "targeted.registrationGeneration") == before,
          "warmup registration mismatch");
    check(unsignedField(warmExecutions.at(name), "end_ns") <= unsignedField(response, "mono_ns")
          && unsignedField(response, "mono_ns") < unsignedField(prepared, "mono_ns")
          && unsignedField(prepared, "mono_ns") < origin, "recovery preparation order");
    const auto first = std::find_if(tasks.begin(), tasks.end(), [&](const PlannedTask& task) {
      return task.description.memory == (std::string(name).back() == 'B');
    });
    check(first != tasks.end() || unsignedField(manifest, "window_ms") == 3000,
          "missing recovery-sensitive planned visit");
    if (first != tasks.end()) {
      const auto firstWallMs = (unsignedField(anchor, "wall_ns") + origin - anchorMono) / 1000000
        + first->arrivalMs;
      check(bindingNumber(response, "targeted.expiresAtMs") > firstWallMs + 1000,
            "warmup material expires before planned recovery visit");
    }
    const auto boot = response.at("response_binding").at("targeted.providerBootEpoch").get<std::string>();
    check(boot.rfind(std::string(name) + ":", 0) == 0, "warmup boot identity mismatch");
    for (const auto& row : providers) {
      if (row.at("event") != "execution" || row.at("provider") != name) continue;
      if (decodeTaskInput(row.at("command").get<std::string>()).warmup) continue;
      check(unsignedField(row, "registration_generation") == after,
            "execution used wrong recovery generation");
      const auto mode = unsignedField(row, "request_mode");
      check((run.at("arm") == "SOLICIT" || run.at("arm") == "FEEDBACK"
             || run.at("arm") == "ACK_ALL") ? mode == nsf::tlv::NormalRequest
            : mode == nsf::tlv::TargetedRequest || mode == nsf::tlv::TargetedBootstrapRequest,
            "invalid actual request mode");
    }
    for (const auto& row : users) {
      if (row.at("event") != "attempt_response" || row.at("provider") != name
          || row.at("status") != true) continue;
      const auto execution = std::find_if(providers.begin(), providers.end(), [&](const Json& entry) {
        return entry.at("event") == "execution" && entry.at("provider") == name
          && entry.at("request_id") == row.at("request_id");
      });
      check(execution != providers.end(), "response lacks bound recovery execution");
      const auto& binding = row.at("response_binding");
      if (unsignedField(*execution, "request_mode") == nsf::tlv::NormalRequest
          && binding.at("targeted.registrationGeneration").is_null()
          && binding.at("targeted.providerBootEpoch").is_null()
          && binding.at("targeted.expiresAtMs").is_null()) continue;
      check(bindingNumber(row, "targeted.registrationGeneration") == after
            && row.at("response_binding").at("targeted.providerBootEpoch") == boot,
            "response recovery identity mismatch");
    }
  }
}

Json
checkRecords(const Json& plan, const Json& manifest, const Json& run,
             const std::vector<Json>& users, const std::vector<Json>& providers,
             const std::vector<Json>& traces = {})
{
  const auto tasks = decodeTaskPlan(plan,unsignedField(manifest,"window_ms"),
                                   unsignedField(manifest,"period_ms"));
  const bool comparison = manifest.value("schema", "") == "ndnsf-spec206-comparison-v1"
    || manifest.value("schema", "") == "ndnsf-spec207-coarse-v1"
    || manifest.value("schema", "") == "ndnsf-feedback-ablation-v1";
  const bool participationStudy = manifest.value("schema", "") == "ndnsf-feedback-ablation-v1";
  const bool coarse = manifest.value("schema", "") == "ndnsf-spec207-coarse-v1"
    && run.at("arm") == "ADS_COARSE";
  if (manifest.value("schema", "") == "ndnsf-spec207-coarse-v1")
    check(manifest.at("advertisement_ttl_ms") == 120000
          && manifest.at("status_ttl_ms") == 2500,
          "coarse comparison lifetime contract");
  const bool taskBusy = comparison && manifest.at("condition") == "task-busy";
  const bool postSelectionWait = manifest.value("post_selection_wait",false)
    && (run.at("arm") == "SOLICIT" || run.at("arm") == "FEEDBACK"
        || run.at("arm") == "ACK_ALL");
  const bool irregular = manifest.value("schema", "") == "ndnsf-dynamic205-e3-v1"
    || (comparison && manifest.at("condition") != "offline");
  if (manifest.value("schema", "") == "ndnsf-dynamic205-e2-v1" || irregular || comparison)
    checkRecoveryRecords(manifest, run, tasks, users, providers);
  const auto origin = unsignedField(run,"origin_ns");
  check(origin <= std::numeric_limits<uint64_t>::max() - 63000000000ULL,"origin overflow");
  check(manifest.at("dwell_ms") == 0 || manifest.at("dwell_ms") == 500
        || manifest.at("dwell_ms") == 5000,"unsupported availability trace");
  const auto end = origin + unsignedField(manifest,"window_ms") * 1000000;
  const auto dwell = unsignedField(manifest,"dwell_ms") * 1000000;
  const bool forced = manifest.at("force_unavailable").get<bool>();
  const std::vector<std::string> names{"/example/hello/provider/A","/example/hello/provider/B"};
  if (irregular) {
    check(traces.size() == 2 && dwell == 0 && !forced
          && manifest.at("recovery_condition") == "prepared", "E3 trace/mode binding");
    for (size_t index = 0; index < traces.size(); ++index) {
      const auto& trace = traces[index];
      check(trace.is_object() && trace.size() == 5 && trace.at("version").is_number_integer()
            && trace.at("version") == 1 && trace.at("producer") == names[index]
            && unsignedField(trace, "window_ms") == unsignedField(manifest, "window_ms")
            && trace.at("tail") == "hold" && trace.at("changes").is_array()
            && !trace.at("changes").empty() && trace.at("changes").size() <= 256,
            "archive trace shape/identity");
      uint64_t previous = 0;
      bool previousState = false;
      size_t count = 0;
      for (const auto& change : trace.at("changes")) {
        check(change.is_object() && change.size() == 2 && change.at("available").is_boolean(),
              "archive trace row shape");
        const auto offset = unsignedField(change, "offset_ms");
        const bool state = change.at("available").get<bool>();
        check(offset < unsignedField(manifest, "window_ms")
              && (count == 0 ? offset == 0 : offset > previous && state != previousState),
              "archive trace order/state");
        previous = offset;
        previousState = state;
        ++count;
      }
    }
  }
  else {
    check(traces.empty(), "trace supplied to legacy archive");
  }
  using Key = std::pair<uint64_t,uint64_t>;
  std::map<uint64_t,Json> arrivals, terminals;
  std::map<Key,Json> attempts, selections;
  std::map<std::string,std::vector<Json>> executions;
  std::vector<Json> responses, acknowledgments;
  std::set<int> warmed;
  size_t windows = 0, closes = 0, shutdowns = 0;
  auto taskFor = [&](uint64_t id) -> const PlannedTask& {
    check(id > 0 && id <= tasks.size(),"task outside frozen plan");
    return tasks.at(id - 1);
  };
  auto providerIndex = [&](const std::string& name) {
    const auto found = std::find(names.begin(),names.end(),name);
    check(found != names.end(),"unknown Producer");
    return static_cast<int>(found - names.begin());
  };
  auto available = [&](int index, uint64_t time) {
    if (irregular && time >= origin) {
      bool state = false;
      // Independent linear replay; do not use the actor's binary-search state lookup.
      for (const auto& change : traces.at(index).at("changes")) {
        if (unsignedField(change, "offset_ms") * 1000000 > time - origin) break;
        state = change.at("available").get<bool>();
      }
      return state;
    }
    return time < origin || (!forced && (!dwell || (time-origin)/dwell%2 == uint64_t(index)));
  };
  // Independent finite truth table, not the actor's taskScore implementation.
  auto expectedScore = [&](const TaskDescription& task, const std::string& name,
                           const Json& state, uint64_t time) {
    const int index = providerIndex(name);
    const auto published = state.contains("published_ns")
      ? unsignedField(state,"published_ns") : uint64_t{0};
    if (coarse && published && unsignedField(state,"expires_ns") > published
        && unsignedField(state,"expires_ns") - published
        == COARSE_AD_TTL_NS) {
      check(state.at("available") == true
            && state.at("compute_units") == (index == 0 ? 8 : 4)
            && state.at("memory_units") == (index == 0 ? 4 : 8),
            "coarse advertisement is not static service capability");
      return task.memory == (index == 1) ? 2000 : 1000;
    }
    const bool full = available(index,time);
    const int compute = index == 0 ? (taskBusy && !full ? 3 : 8) : 4;
    const int memory = index == 1 ? (taskBusy && !full ? 3 : 8) : 4;
    check(state.at("compute_units") == compute && state.at("memory_units") == memory,
          "resource identity mismatch");
    check(state.at("available") == (taskBusy || full),"availability trace mismatch");
    if (!taskBusy && !full) return 0;
    if (taskBusy && !full && task.memory == (index == 1)) return 0;
    return task.memory == (index == 1) ? 2000 : 1000;
  };
  for (const auto& row : users) {
    const auto event = row.at("event").get<std::string>();
    if (event == "window") {
      ++windows;
      check(row.at("t0_ns") == origin && row.at("t1_ns") == end
            && row.at("period_ms") == manifest.at("period_ms"),"window mismatch");
    }
    else if (event == "cohort_closed") {
      ++closes;
      check(row.at("tasks") == tasks.size() && unsignedField(row,"mono_ns") >= end,
            "cohort closed before complete arrival plan");
    }
    else if (event == "shutdown") {
      ++shutdowns;
      check(row.at("started") == true && row.at("invalid") == 0
            && row.at("tasks") == tasks.size() && row.at("terminals") == tasks.size()
            && row.at("planned_tasks") == tasks.size(),"incomplete native lifecycle");
    }
    else if (event == "arrival" || event == "terminal") {
      const auto id = unsignedField(row,"sequence");
      taskFor(id);
      auto& target = event == "arrival" ? arrivals : terminals;
      check(target.emplace(id,row).second,"duplicate arrival/terminal");
    }
    else if (event == "attempt_submitted" || event == "ack_selection"
             || event == "advertisement_selection") {
      const Key key{unsignedField(row,"sequence"),unsignedField(row,"attempt")};
      taskFor(key.first);
      check(key.second >= 1 && key.second <= 3,"attempt outside retry budget");
      auto& target = event == "attempt_submitted" ? attempts : selections;
      check(target.emplace(key,row).second,"duplicate attempt/selection");
    }
    else if (event == "attempt_response") responses.push_back(row);
  }
  check(windows == 1 && closes == 1 && shutdowns == 1
        && arrivals.size() == tasks.size() && terminals.size() == tasks.size(),
        "missing lifecycle/task records");
  std::map<std::string,Key> requestKeys;
  for (const auto& entry : attempts) {
    const auto& key = entry.first;
    const auto& row = entry.second;
    const auto arrival = origin + taskFor(key.first).arrivalMs * 1000000;
    const auto begin = unsignedField(row,"begin_ns"), deadline = unsignedField(row,"deadline_ns");
    check(begin >= arrival && begin < deadline && deadline <= arrival + 2000000000ULL
          && deadline - begin <= 600000000
          && begin >= unsignedField(arrivals.at(key.first),"dispatch_ns")
          && begin <= unsignedField(terminals.at(key.first),"terminal_ns"),"attempt time budget");
    const auto id = row.at("request_id").get<std::string>();
    if (id != "/") check(requestKeys.emplace(id,key).second,"duplicate request identity");
  }
  std::set<Key> responseKeys;
  if (postSelectionWait) {
    for (const auto& entry : selections) {
      if (entry.second.at("event") != "ack_selection"
          || entry.second.at("selected").get<std::string>().empty()) continue;
      check(std::none_of(attempts.begin(),attempts.end(),[&](const auto& attempt) {
        if (attempt.first.first != entry.first.first
            || attempt.first.second <= entry.first.second) return false;
        return std::none_of(responses.begin(),responses.end(),[&](const Json& response) {
          return unsignedField(response,"sequence") == entry.first.first
            && unsignedField(response,"attempt") == entry.first.second
            && response.at("identity_verified") == true
            && response.at("echo_verified") == true
            && response.at("status") == false
            && response.at("error") == "task ineligible at admission"
            && unsignedField(response,"mono_ns") <= unsignedField(attempt.second,"begin_ns");
        });
      }),"new invocation after execution-capable selection");
    }
  }
  for (const auto& response : responses) {
    const Key key{unsignedField(response,"sequence"),unsignedField(response,"attempt")};
    check(attempts.count(key) && responseKeys.insert(key).second,"unbound/duplicate response callback");
    check(response.at("request_id") == attempts.at(key).at("request_id"),"response request mismatch");
    check(response.at("status") == false
          || (response.at("identity_verified") == true && response.at("echo_verified") == true),
          "successful response failed native admission");
  }
  for (const auto& row : providers) {
    if (row.at("event") == "ack_handler") acknowledgments.push_back(row);
    if (row.at("event") != "execution") continue;
    const auto name = row.at("provider").get<std::string>();
    const int index = providerIndex(name);
    const auto task = decodeTaskInput(row.at("command").get<std::string>());
    check(row.at("payload_valid") == true && row.at("task") == taskJson(task),"execution task payload");
    const auto begin = unsignedField(row,"begin_ns"), finish = unsignedField(row,"end_ns");
    check(begin <= finish,"execution clock order");
    const auto score = expectedScore(task,name,row,begin);
    check(row.at("eligible") == (score > 0),"execution eligibility flag contradicts trace");
    if (task.warmup) {
      check(begin < origin && finish < origin && score > 0 && !task.memory
            && task.id == uint64_t(index + 1),"warmup bypass/mismatch");
      check(warmed.insert(index).second,"duplicate warmup execution");
      continue;
    }
    const auto& planned = taskFor(task.id);
    check(taskJson(task) == taskJson(planned.description),"wrong execution task class");
    const auto id = row.at("request_id").get<std::string>();
    check(requestKeys.count(id) && requestKeys.at(id).first == task.id,"unbound execution request");
    const auto key = requestKeys.at(id);
    check(begin >= unsignedField(attempts.at(key),"begin_ns"),"execution before submission");
    check(selections.count(key),"execution without recorded selection");
    const auto& selection = selections.at(key);
    const auto chosen = selection.at("event") == "ack_selection"
      ? selection.at("selected").get<std::string>()
      : selection.at("snapshot").at("producer").get<std::string>();
    check(chosen == name,"wrong Provider executed");
    executions[id].push_back(row);
  }
  check(warmed.size() == 2,"missing real warmup execution");
  if (coarse) {
    std::set<std::string> advertised;
    std::set<std::string> receivedBeforeOrigin;
    for (const auto& row : providers) {
      if (row.at("event") != "advertisement") continue;
      const auto name = row.at("snapshot").at("producer").get<std::string>();
      check(advertised.insert(name).second && unsignedField(row,"mono_ns") < origin,
            "duplicate or late coarse advertisement");
      const TaskDescription probe{1,false,false};
      expectedScore(probe,name,row.at("snapshot"),unsignedField(row,"mono_ns"));
      check(unsignedField(row.at("snapshot"),"expires_ns")
              - unsignedField(row.at("snapshot"),"published_ns") == COARSE_AD_TTL_NS,
            "coarse advertisement lifetime");
    }
    check(advertised.size() == 2,"missing coarse Provider advertisement");
    for (const auto& accepted : users) {
      if (accepted.at("event") != "snapshot_accepted") continue;
      const auto& snapshot = accepted.at("snapshot");
      const auto source = accepted.at("source").get<std::string>();
      check(source == "SVS-advertisement" || source == "fresh-status",
            "unknown coarse snapshot source");
      const auto lifetime = unsignedField(snapshot,"expires_ns")
        - unsignedField(snapshot,"published_ns");
      check(lifetime == (source == "SVS-advertisement" ? COARSE_AD_TTL_NS : TTL_NS),
            "coarse snapshot source/lifetime mismatch");
      check(std::any_of(providers.begin(),providers.end(),[&](const Json& provider) {
        return provider.at("event") == (source == "SVS-advertisement"
                                     ? "advertisement" : "fresh_status")
          && provider.at("snapshot") == snapshot;
      }),"snapshot has no Producer publication");
      if (source == "SVS-advertisement" && unsignedField(accepted,"mono_ns") < origin)
        receivedBeforeOrigin.insert(snapshot.at("producer").get<std::string>());
    }
    check(receivedBeforeOrigin == advertised,
          "User did not admit both initial Provider advertisements before origin");
  }
  for (const auto& entry : selections) {
    const auto& key = entry.first;
    const auto& row = entry.second;
    check(attempts.count(key),"selection without submission");
    const auto& task = taskFor(key.first).description;
    const bool solicitation = row.at("event") == "ack_selection";
    check(solicitation == (run.at("arm") == "SOLICIT" || participationStudy),
          "wrong selection mechanism");
    const auto decisionTime = solicitation ? unsignedField(row,"mono_ns")
      : unsignedField(attempts.at(key),"begin_ns");
    int best = 0;
    std::string chosen;
    int untriedBest = 0;
    std::string untriedChosen;
    std::set<std::string> unansweredTargets;
    if (coarse) {
      for (const auto& previous : selections) {
        if (previous.first.first != key.first || previous.first.second >= key.second
            || previous.second.at("event") != "advertisement_selection") continue;
        const bool answered = std::any_of(responses.begin(),responses.end(),[&](const Json& reply) {
          return unsignedField(reply,"sequence") == previous.first.first
            && unsignedField(reply,"attempt") == previous.first.second
            && unsignedField(reply,"mono_ns") <= decisionTime
            && reply.at("late") == false;
        });
        if (!answered)
          unansweredTargets.insert(previous.second.at("snapshot").at("producer"));
      }
    }
    std::set<std::string> seen;
    std::map<std::string,Json> admitted;
    if (!solicitation) {
      std::map<std::string,Json> staticAds, shortStatus;
      for (const auto& accepted : users) {
        if (accepted.at("event") == "snapshot_accepted"
            && unsignedField(accepted,"mono_ns") <= decisionTime) {
          const auto& snapshot = accepted.at("snapshot");
          const auto name = snapshot.at("producer").get<std::string>();
          if (coarse && accepted.at("source") == "fresh-status")
            shortStatus.insert_or_assign(name,snapshot);
          else staticAds.insert_or_assign(name,snapshot);
        }
      }
      admitted = std::move(staticAds);
      if (coarse) {
        for (const auto& entry : shortStatus) {
          if (unsignedField(entry.second,"expires_ns") > decisionTime)
            admitted.insert_or_assign(entry.first,entry.second);
        }
      }
      check(row.at("candidates").size() == admitted.size(),"incomplete cache candidate set");
    }
    for (const auto& candidate : row.at("candidates")) {
      const auto name = candidate.at(solicitation ? "provider" : "producer").get<std::string>();
      check(seen.insert(name).second,"duplicate selection candidate");
      Json state = solicitation ? Json::parse(candidate.at("message").get<std::string>()) : candidate;
      const auto observed = unsignedField(state,solicitation ? "observed_ns" : "published_ns");
      check(observed <= decisionTime,"future candidate observation");
      const int score = expectedScore(task,name,state,observed);
      bool valid = solicitation ? decisionTime - observed < TTL_NS
        : unsignedField(state,"expires_ns") > decisionTime;
      if (solicitation) {
        check(state.at("task") == taskJson(task),"ACK task mismatch");
        check(candidate.at("authenticated") == true && candidate.at("signer") == name,
              "ACK without native trust evidence");
        check(candidate.at("status") == (participationStudy && run.at("arm") == "ACK_ALL"
              ? true : score > 0),"ACK eligibility mismatch");
        check(std::any_of(acknowledgments.begin(),acknowledgments.end(),[&](const Json& ack) {
          return ack.at("provider") == name && ack.at("state") == state;
        }),"ACK not observed in real Provider callback");
      }
      else {
        check(admitted.count(name) && admitted.at(name) == state,
              "candidate is not latest admitted snapshot");
        check(unsignedField(state,"expires_ns") >= observed,"advertisement time reversal");
        const auto lifetime = unsignedField(state,"expires_ns") - observed;
        check(state.at("session") == run.at("session") && state.at("service") == "/HELLO"
              && (coarse ? lifetime == COARSE_AD_TTL_NS || lifetime == TTL_NS
                         : lifetime == TTL_NS),
              "advertisement scope/expiry");
        check(std::any_of(users.begin(),users.end(),[&](const Json& accepted) {
          return accepted.at("event") == "snapshot_accepted"
            && accepted.at("snapshot") == state
            && unsignedField(accepted,"mono_ns") <= decisionTime;
        }),"advertisement without admitted cache record");
      }
      if (valid && (participationStudy ? candidate.at("status") == true : score > 0)
          && (participationStudy ? chosen.empty() || name < chosen
                                 : score > best || (score == best && name < chosen))) {
        best = score; chosen = name;
      }
      if (valid && score > 0 && !unansweredTargets.count(name)
          && (score > untriedBest || (score == untriedBest && name < untriedChosen))) {
        untriedBest = score; untriedChosen = name;
      }
    }
    if (coarse && untriedBest > 0) chosen = untriedChosen;
    const auto actual = solicitation ? row.at("selected").get<std::string>()
      : row.at("snapshot").at("producer").get<std::string>();
    check(chosen == actual,"selection differs from the declared ACK policy");
    if (!solicitation) {
      check(std::any_of(row.at("candidates").begin(),row.at("candidates").end(),
        [&](const Json& candidate) { return candidate == row.at("snapshot"); }),
        "selected advertisement differs from observed candidate");
    }
  }
  size_t successes = 0, clientSuccesses = 0, duplicateExecutions = 0, lateExecutions = 0;
  std::vector<uint64_t> incorrectTasks;
  uint64_t occupiedUntil = 0;
  for (const auto& planned : tasks) {
    const auto id = planned.description.id;
    const auto arrival = origin + planned.arrivalMs * 1000000;
    const auto& received = arrivals.at(id);
    const auto& terminal = terminals.at(id);
    check(received.at("planned_ns") == arrival && unsignedField(received,"dispatch_ns") >= arrival
          && received.at("task") == taskJson(planned.description),"arrival plan mismatch");
    check(terminal.at("begin_ns") == arrival && terminal.at("deadline_ns") == arrival + 2000000000ULL
          && terminal.at("command") == taskInput(planned.description),"terminal task/deadline mismatch");
    const auto count = static_cast<size_t>(std::count_if(attempts.begin(),attempts.end(),[&](const auto& pair) {
      return pair.first.first == id;
    }));
    check(terminal.at("attempts") == count,"terminal attempt count");
    for (size_t attempt = 1; attempt <= count; ++attempt)
      check(attempts.count({id,attempt}),"missing attempt sequence");
    const auto outcome = terminal.at("terminal").get<std::string>();
    check(outcome == "success" || outcome == "timeout" || outcome == "rejected"
          || outcome == "attempts_exhausted" || outcome == "uncertain_execution",
          "unknown task outcome");
    check(unsignedField(terminal,"terminal_ns") >= arrival,"terminal before arrival");
    const auto dispatch = unsignedField(received,"dispatch_ns");
    if (dispatch >= arrival + 2000000000ULL) {
      check(outcome == "timeout" && count == 0,"expired arrival admitted");
    }
    else if (dispatch < occupiedUntil) {
      check(outcome == "rejected" && count == 0,"overlapping task admitted");
    }
    else {
      check(outcome != "rejected","idle capacity rejection");
      occupiedUntil = std::min<uint64_t>(unsignedField(terminal,"terminal_ns"),
                                        arrival + 2000000000ULL);
    }
    if (outcome == "timeout") {
      check(unsignedField(terminal,"terminal_ns") >= arrival + 2000000000ULL,
            "timeout before logical deadline");
    }
    if (outcome == "attempts_exhausted") check(count == 3,"retry exhaustion before budget");
    if (outcome == "rejected") check(count == 0,"capacity rejection submitted work");
    size_t effects = 0, late = 0;
    for (const auto& pair : executions) {
      if (requestKeys.at(pair.first).first != id) continue;
      for (const auto& execution : pair.second) {
        if (execution.at("eligible") != true) continue;
        ++effects;
        if (unsignedField(execution,"end_ns") >= arrival + 2000000000ULL) ++late;
      }
    }
    duplicateExecutions += effects > 1 ? effects - 1 : 0;
    lateExecutions += late;
    if (effects > 1 || late > 0) incorrectTasks.push_back(id);
    if (outcome != "success") continue;
    ++clientSuccesses;
    const Key key{id,unsignedField(terminal,"success_attempt")};
    const auto request = terminal.at("success_request_id").get<std::string>();
    check(attempts.count(key) && requestKeys.count(request) && requestKeys.at(request) == key
          && executions.count(request),"success without bound actual execution");
    const auto& matching = executions.at(request);
    const auto completed = unsignedField(terminal,"success_response_ns");
    check(std::any_of(matching.begin(),matching.end(),[&](const Json& execution) {
      return execution.at("eligible") == true
          && execution.at("provider") == terminal.at("success_provider")
          && unsignedField(execution,"end_ns") <= completed;
    })
          && completed < (postSelectionWait ? arrival + 2000000000ULL
                                           : unsignedField(attempts.at(key),"deadline_ns"))
          && completed <= unsignedField(terminal,"terminal_ns")
          && unsignedField(terminal,"terminal_ns") < arrival + 2000000000ULL,"late/invalid success");
    check(std::any_of(responses.begin(),responses.end(),[&](const Json& response) {
      return response.at("sequence") == id && response.at("attempt") == key.second
        && response.at("request_id") == request
        && response.at("provider") == terminal.at("success_provider")
        && response.at("status") == true && response.at("identity_verified") == true
        && response.at("echo_verified") == true && response.at("late") == false
        && unsignedField(response,"mono_ns") <= completed;
    }),"success without native response verification");
    if (effects == 1 && late == 0) ++successes;
  }
  size_t executionCount = 0;
  for (const auto& pair : executions) executionCount += pair.second.size();
  return {{"event","dynamic_record_oracle_complete"},{"qualified",true},
          {"run_id",run.at("id")},{"tasks",tasks.size()},{"successes",successes},
          {"client_successes",clientSuccesses},{"duplicate_executions",duplicateExecutions},
          {"late_executions",lateExecutions},{"incorrect_tasks",incorrectTasks},
          {"executions",executionCount},
          {"scope","native record consistency; not independent signature verification"}};
}

size_t
checkRecordFixtures()
{
  // Literal trace: one compute task, one selected execution, two prior warmups.
  const uint64_t origin = 10000000000ULL;
  const std::string a = "/example/hello/provider/A", b = "/example/hello/provider/B";
  const TaskDescription task{1,false,false};
  const Json plan = Json::parse(R"({"version":1,"window_ms":1000,"period_ms":4700,
    "tasks":[{"task":1,"arrival_ms":0,"class":"compute"}]})");
  const Json manifest = Json::parse(R"({"window_ms":1000,"period_ms":4700,
    "dwell_ms":0,"force_unavailable":false})");
  const Json run{{"id","fixture"},{"arm","SOLICIT"},{"session","fixture"},{"origin_ns",origin}};
  const Json state{{"available",true},{"compute_units",8},{"memory_units",4},
                   {"observed_ns",origin+10},{"task",taskJson(task)}};
  std::vector<Json> users{
    {{"event","window"},{"t0_ns",origin},{"t1_ns",origin+1000000000}, {"period_ms",4700}},
    {{"event","arrival"},{"sequence",1},{"planned_ns",origin},{"dispatch_ns",origin},
     {"task",taskJson(task)}},
    {{"event","attempt_submitted"},{"sequence",1},{"attempt",1},{"request_id","/request/1"},
     {"begin_ns",origin},{"deadline_ns",origin+600000000}},
    {{"event","ack_selection"},{"sequence",1},{"attempt",1},{"selected",a},
     {"mono_ns",origin+20},{"candidates",Json::array({Json{{"provider",a},
       {"authenticated",true},{"signer",a},{"status",true},{"message",state.dump()}}})}},
    {{"event","attempt_response"},{"sequence",1},{"attempt",1},{"request_id","/request/1"},
     {"provider",a},{"status",true},{"identity_verified",true},{"echo_verified",true},
     {"late",false},{"mono_ns",origin+40}},
    {{"event","terminal"},{"sequence",1},{"command",taskInput(task)},{"begin_ns",origin},
     {"deadline_ns",origin+2000000000},{"terminal_ns",origin+50},{"terminal","success"},
     {"attempts",1},{"success_attempt",1},{"success_request_id","/request/1"},
     {"success_provider",a},{"success_response_ns",origin+40}},
    {{"event","cohort_closed"},{"tasks",1},{"mono_ns",origin+1000000000}},
    {{"event","shutdown"},{"started",true},{"invalid",0},{"tasks",1},
     {"terminals",1},{"planned_tasks",1}}
  };
  auto execution = [&](const TaskDescription& description, const std::string& producer,
                       uint64_t begin, const std::string& request) {
    return Json{{"event","execution"},{"provider",producer},{"command",taskInput(description)},
      {"task",taskJson(description)},{"payload_valid",true},{"available",true},{"eligible",true},
      {"compute_units",producer == a ? 8 : 4},{"memory_units",producer == a ? 4 : 8},
      {"begin_ns",begin},{"end_ns",begin+1},{"request_id",request}};
  };
  std::vector<Json> providers{
    execution({1,false,true},a,origin-100,"/warm/1"),
    execution({2,false,true},b,origin-100,"/warm/2"),
    {{"event","ack_handler"},{"provider",a},{"state",state}},
    execution(task,a,origin+30,"/request/1")
  };
  // Match JSONL parsing: positive integer literals become unsigned values.
  for (auto& row : users) row = Json::parse(row.dump());
  for (auto& row : providers) row = Json::parse(row.dump());
  check(checkRecords(plan,manifest,run,users,providers).at("qualified") == true,
        "valid archive fixture rejected");
  auto repeated = providers;
  repeated.push_back(execution(task,a,origin+35,"/request/1"));
  auto repeatedResult = checkRecords(plan,manifest,run,users,repeated);
  check(repeatedResult.at("successes") == 0 && repeatedResult.at("client_successes") == 1
        && repeatedResult.at("duplicate_executions") == 1
        && repeatedResult.at("incorrect_tasks") == Json::array({1}),
        "duplicate execution counted as correct completion");
  auto lateExecution = providers;
  lateExecution.push_back(execution(task,a,origin+2000000000ULL,"/request/1"));
  auto lateResult = checkRecords(plan,manifest,run,users,lateExecution);
  check(lateResult.at("successes") == 0 && lateResult.at("late_executions") == 1,
        "late execution counted as correct completion");
  size_t rejected = 0;
  auto waitManifest = manifest;
  waitManifest["post_selection_wait"] = true;
  auto delayedUsers = users;
  delayedUsers.at(4)["mono_ns"] = origin + 700000000ULL;
  delayedUsers.at(5)["success_response_ns"] = origin + 700000000ULL;
  delayedUsers.at(5)["terminal_ns"] = origin + 710000000ULL;
  auto delayedProviders = providers;
  delayedProviders.at(3)["begin_ns"] = origin + 650000000ULL;
  delayedProviders.at(3)["end_ns"] = origin + 650000001ULL;
  check(checkRecords(plan,waitManifest,run,delayedUsers,delayedProviders).at("successes") == 1,
        "post-selection response within task deadline rejected");
  auto unsafeRetry = delayedUsers;
  unsafeRetry.at(5)["terminal_ns"] = origin + 900000000ULL;
  unsafeRetry.at(5)["attempts"] = uint64_t{2};
  unsafeRetry.insert(unsafeRetry.begin() + 5,
    Json{{"event","attempt_submitted"},{"sequence",uint64_t{1}},
         {"attempt",uint64_t{2}},
         {"request_id","/request/2"},{"begin_ns",origin+800000000ULL},
         {"deadline_ns",origin+1400000000ULL}});
  bool retryRejected = false;
  std::string rejectionReason;
  try { checkRecords(plan,waitManifest,run,unsafeRetry,delayedProviders); }
  catch (const std::exception& error) {
    rejectionReason = error.what();
    retryRejected = rejectionReason.find("new invocation after execution-capable selection")
      != std::string::npos;
  }
  check(retryRejected,"unsafe retry reached wrong boundary: " + rejectionReason);
  ++rejected;
  // Recovery checks use two predetermined visits, independently of response outcomes.
  const std::vector<PlannedTask> recoveryTasks{{{1, false, false}, 0}, {{2, true, false}, 4700}};
  Json recoveryManifest{{"recovery_condition", "reregister"},
    {"clock_anchor", {{"mono_ns", uint64_t{0}}, {"wall_ns", uint64_t{1000000000000ULL}}}}};
  auto recoveryProviders = providers;
  std::vector<Json> recoveryUsers;
  for (const auto& name : {a, b}) {
    Json binding{{"targeted.registrationGeneration", "1"},
      {"targeted.providerBootEpoch", name + ":123"}, {"targeted.expiresAtMs", "1100000"}};
    recoveryUsers.push_back({{"event", "warmup_response"}, {"provider", name},
      {"mono_ns", origin - 50}, {"response_binding", binding}});
    recoveryProviders.push_back({{"event", "recovery_prepared"}, {"provider", name},
      {"condition", "reregister"}, {"before_generation", uint64_t{1}},
      {"after_generation", uint64_t{2}}, {"mono_ns", origin - 10}});
  }
  for (auto& row : recoveryProviders) {
    if (row.at("event") != "execution") continue;
    row["registration_generation"] = uint64_t{
      decodeTaskInput(row.at("command").get<std::string>()).warmup ? 1U : 2U};
    row["request_mode"] = uint64_t{nsf::tlv::NormalRequest};
  }
  recoveryUsers.push_back({{"event", "attempt_response"}, {"provider", a},
    {"request_id", "/request/1"}, {"status", true},
    {"response_binding", {{"targeted.registrationGeneration", nullptr},
                          {"targeted.providerBootEpoch", nullptr}, {"targeted.expiresAtMs", nullptr}}}});
  checkRecoveryRecords(recoveryManifest, run, recoveryTasks, recoveryUsers, recoveryProviders);
  auto bootstrapRun = run;
  bootstrapRun["arm"] = "ADS100";
  auto bootstrapProviders = recoveryProviders;
  bootstrapProviders.at(3)["request_mode"] = uint64_t{nsf::tlv::TargetedBootstrapRequest};
  auto bootstrapUsers = recoveryUsers;
  bootstrapUsers.back()["response_binding"] = {{"targeted.registrationGeneration", "2"},
    {"targeted.providerBootEpoch", a + ":123"}, {"targeted.expiresAtMs", "1100000"}};
  checkRecoveryRecords(recoveryManifest, bootstrapRun, recoveryTasks, bootstrapUsers, bootstrapProviders);
  auto preparedManifest = recoveryManifest;
  preparedManifest["recovery_condition"] = "prepared";
  auto preparedProviders = recoveryProviders;
  for (auto& row : preparedProviders) {
    if (row.at("event") == "recovery_prepared") {
      row["condition"] = "prepared";
      row["after_generation"] = uint64_t{1};
    }
    if (row.at("event") == "execution") row["registration_generation"] = uint64_t{1};
  }
  checkRecoveryRecords(preparedManifest, run, recoveryTasks, recoveryUsers, preparedProviders);
  auto traceManifest = manifest;
  traceManifest.update(preparedManifest);
  traceManifest["schema"] = "ndnsf-dynamic205-e3-v1";
  traceManifest["window_ms"] = uint64_t{5000};
  auto tracePlan = plan;
  tracePlan["window_ms"] = uint64_t{5000};
  tracePlan["tasks"].push_back({{"task", uint64_t{2}}, {"arrival_ms", uint64_t{4700}},
                               {"class", "memory"}});
  auto traceUsers = users;
  traceUsers.at(0)["t1_ns"] = origin + 5000000000ULL;
  traceUsers.at(4)["response_binding"] = recoveryUsers.back().at("response_binding");
  traceUsers.at(6)["tasks"] = uint64_t{2};
  traceUsers.at(6)["mono_ns"] = origin + 5000000000ULL;
  for (const auto* field : {"tasks", "terminals", "planned_tasks"})
    traceUsers.at(7)[field] = uint64_t{2};
  traceUsers.insert(traceUsers.end(), recoveryUsers.begin(), recoveryUsers.begin() + 2);
  const TaskDescription secondTask{2, true, false};
  traceUsers.push_back({{"event", "arrival"}, {"sequence", uint64_t{2}},
    {"planned_ns", origin + 4700000000ULL}, {"dispatch_ns", origin + 4700000000ULL},
    {"task", taskJson(secondTask)}});
  traceUsers.push_back({{"event", "terminal"}, {"sequence", uint64_t{2}},
    {"begin_ns", origin + 4700000000ULL}, {"deadline_ns", origin + 6700000000ULL},
    {"terminal_ns", origin + 6700000000ULL}, {"terminal", "timeout"},
    {"attempts", uint64_t{0}}, {"command", taskInput(secondTask)}});
  std::vector<Json> traces;
  for (const auto& name : {a, b}) {
    traces.push_back(Json{{"version", 1}, {"producer", name}, {"window_ms", uint64_t{5000}},
      {"tail", "hold"}, {"changes", Json::array({
        Json{{"offset_ms", uint64_t{0}}, {"available", true}},
        Json{{"offset_ms", uint64_t{731}}, {"available", false}}})}});
  }
  check(checkRecords(tracePlan, traceManifest, run, traceUsers, preparedProviders, traces)
        .at("qualified") == true, "valid irregular archive rejected");
  auto denyTrace = [&](std::vector<Json> broken) {
    bool failed = false;
    try { checkRecords(tracePlan, traceManifest, run, traceUsers, preparedProviders, broken); }
    catch (const std::exception&) { failed = true; }
    check(failed, "corrupt irregular archive accepted");
    ++rejected;
  };
  auto badTraces = traces; badTraces.pop_back(); denyTrace(badTraces);
  badTraces = traces; badTraces[0]["producer"] = b; denyTrace(badTraces);
  badTraces = traces; badTraces[0]["changes"][1]["offset_ms"] = -1; denyTrace(badTraces);
  badTraces = traces; badTraces[0]["tail"] = "repeat"; denyTrace(badTraces);
  badTraces = traces; badTraces[0]["changes"][0]["available"] = false;
  badTraces[0]["changes"][1]["available"] = true; denyTrace(badTraces);
  auto crossedTrace = traces;
  crossedTrace[0]["changes"][1]["offset_ms"] = uint64_t{1};
  auto crossedProviders = preparedProviders;
  crossedProviders.at(3)["begin_ns"] = origin + 1000000;
  crossedProviders.at(3)["end_ns"] = origin + 1000001;
  crossedProviders.at(3)["available"] = false;
  crossedProviders.at(3)["eligible"] = false;
  auto crossedUsers = traceUsers;
  crossedUsers.at(4)["status"] = false;
  crossedUsers.at(4)["late"] = true;
  crossedUsers.at(4)["mono_ns"] = origin + 2000000001ULL;
  crossedUsers.at(5)["terminal"] = "timeout";
  crossedUsers.at(5)["terminal_ns"] = origin + 2000000000ULL;
  check(checkRecords(tracePlan, traceManifest, run, crossedUsers, crossedProviders, crossedTrace)
        .at("qualified") == true, "selection-before-transition failure rejected");
  crossedProviders.at(3)["eligible"] = true;
  bool badEligibility = false;
  try { checkRecords(tracePlan, traceManifest, run, crossedUsers, crossedProviders, crossedTrace); }
  catch (const std::exception&) { badEligibility = true; }
  check(badEligibility, "execution eligibility contradicting trace accepted");
  ++rejected;
  for (const Json badGeneration : {Json("1"), Json(nullptr)}) {
    auto broken = bootstrapUsers;
    broken.back()["response_binding"]["targeted.registrationGeneration"] = badGeneration;
    bool failed = false;
    try { checkRecoveryRecords(recoveryManifest, bootstrapRun, recoveryTasks, broken, bootstrapProviders); }
    catch (const std::exception&) { failed = true; }
    check(failed, "invalid bootstrap response generation accepted");
    ++rejected;
  }
  auto denyRecovery = [&](const std::vector<Json>& userRows, const std::vector<Json>& providerRows) {
    bool failed = false;
    try { checkRecoveryRecords(recoveryManifest, run, recoveryTasks, userRows, providerRows); }
    catch (const std::exception&) { failed = true; }
    check(failed, "corrupt recovery fixture accepted");
    ++rejected;
  };
  auto brokenRecovery = recoveryProviders;
  brokenRecovery.pop_back(); denyRecovery(recoveryUsers, brokenRecovery);
  brokenRecovery = recoveryProviders;
  brokenRecovery.push_back(brokenRecovery.back()); denyRecovery(recoveryUsers, brokenRecovery);
  brokenRecovery = recoveryProviders;
  brokenRecovery.back()["after_generation"] = uint64_t{1}; denyRecovery(recoveryUsers, brokenRecovery);
  brokenRecovery = recoveryProviders;
  brokenRecovery.back()["mono_ns"] = origin + 1; denyRecovery(recoveryUsers, brokenRecovery);
  brokenRecovery = recoveryProviders;
  brokenRecovery.at(3)["registration_generation"] = uint64_t{1};
  denyRecovery(recoveryUsers, brokenRecovery);
  auto brokenWarmup = recoveryUsers;
  brokenWarmup.front()["response_binding"]["targeted.expiresAtMs"] = "1";
  denyRecovery(brokenWarmup, recoveryProviders);
  brokenWarmup = recoveryUsers;
  brokenWarmup.push_back(brokenWarmup.front()); denyRecovery(brokenWarmup, recoveryProviders);
  auto deny = [&](std::vector<Json> changedUsers, std::vector<Json> changedProviders) {
    bool failed = false;
    try { checkRecords(plan,manifest,run,changedUsers,changedProviders); }
    catch (const std::exception&) { failed = true; }
    check(failed,"corrupt archive fixture accepted");
    ++rejected;
  };
  auto changed = users; changed.erase(changed.begin()+1); deny(changed,providers);
  changed = users; changed.push_back(users.at(5)); deny(changed,providers);
  changed = users; changed.at(3)["selected"] = b; deny(changed,providers);
  auto wrong = providers; wrong.back()["request_id"] = "/wrong"; deny(users,wrong);
  wrong = providers; wrong.pop_back(); deny(users,wrong);
  changed = users; changed.at(4)["echo_verified"] = false; deny(changed,providers);
  changed = users; changed.at(5)["terminal"] = "timeout"; deny(changed,providers);
  changed = users; changed.erase(changed.begin()+2,changed.begin()+5);
  changed.at(2)["terminal"] = "rejected"; changed.at(2)["attempts"] = 0;
  wrong = providers; wrong.pop_back(); deny(changed,wrong);
  auto adsRun = run; adsRun["arm"] = "ADS100";
  auto ads = users;
  auto snapshot = [&](const std::string& producer) {
    return Json{{"producer",producer},{"service","/HELLO"},{"session","fixture"},
      {"sequence",uint64_t{1}},{"published_ns",origin-100},{"expires_ns",origin-100+TTL_NS},
      {"available",true},{"compute_units",producer == a ? 8 : 4},
      {"memory_units",producer == a ? 4 : 8}};
  };
  const auto first = snapshot(a), second = snapshot(b);
  ads.at(3) = {{"event","advertisement_selection"},{"sequence",uint64_t{1}},
    {"attempt",uint64_t{1}},{"snapshot",first},{"candidates",Json::array({first,second})}};
  ads.insert(ads.begin(),Json{{"event","snapshot_accepted"},{"snapshot",first},{"mono_ns",origin-50}});
  ads.insert(ads.begin(),Json{{"event","snapshot_accepted"},{"snapshot",second},{"mono_ns",origin-50}});
  check(checkRecords(plan,manifest,adsRun,ads,providers).at("qualified") == true,
        "valid advertisement archive rejected");
  auto denyAds = [&](std::vector<Json> records) {
    bool failed = false;
    try { checkRecords(plan,manifest,adsRun,records,providers); }
    catch (const std::exception&) { failed = true; }
    check(failed,"corrupt advertisement archive accepted");
    ++rejected;
  };
  changed = ads; changed.at(5)["candidates"] = Json::array({second}); denyAds(changed);
  changed = ads;
  auto newer = first; newer["sequence"] = uint64_t{2};
  changed.insert(changed.begin()+2,Json{{"event","snapshot_accepted"},
    {"snapshot",newer},{"mono_ns",origin-25}});
  denyAds(changed);
  auto checkOccupancy = [&](uint64_t windowMs, uint64_t periodMs) {
    const uint64_t count = (windowMs + periodMs - 1) / periodMs;
    Json planned = Json::array();
    std::vector<Json> records{
      {{"event","window"},{"t0_ns",origin},{"t1_ns",origin+windowMs*1000000},
       {"period_ms",periodMs}}
    };
    uint64_t busyUntil = 0;
    for (uint64_t index = 0; index < count; ++index) {
      const TaskDescription item{index+1,index%2 == 1,false};
      const auto arrival = origin + index*periodMs*1000000;
      const bool busy = arrival < busyUntil;
      planned.push_back({{"task",item.id},{"arrival_ms",index*periodMs},
                         {"class",item.memory ? "memory" : "compute"}});
      records.push_back({{"event","arrival"},{"sequence",item.id},{"planned_ns",arrival},
        {"dispatch_ns",arrival},{"task",taskJson(item)}});
      records.push_back({{"event","terminal"},{"sequence",item.id},{"begin_ns",arrival},
        {"deadline_ns",arrival+2000000000ULL},{"command",taskInput(item)},{"attempts",uint64_t{0}},
        {"terminal",busy ? "rejected" : "timeout"},
        {"terminal_ns",busy ? arrival : arrival+2000000000ULL}});
      if (!busy) busyUntil = arrival+2000000000ULL;
    }
    records.push_back({{"event","cohort_closed"},{"tasks",count},
                       {"mono_ns",origin+windowMs*1000000}});
    records.push_back({{"event","shutdown"},{"started",true},{"invalid",0},
                       {"tasks",count},{"terminals",count},{"planned_tasks",count}});
    Json planMany{{"version",1},{"window_ms",windowMs},{"period_ms",periodMs},{"tasks",planned}};
    auto manifestMany = manifest;
    manifestMany["window_ms"] = windowMs; manifestMany["period_ms"] = periodMs;
    std::vector<Json> warmups{providers.at(0),providers.at(1)};
    check(checkRecords(planMany,manifestMany,adsRun,records,warmups).at("qualified") == true,
          "valid occupied/released capacity trace rejected");
    // Change the second arrival to the opposite admission decision.
    records.at(4)["terminal"] = periodMs == 100 ? "timeout" : "rejected";
    records.at(4)["terminal_ns"] = origin+periodMs*1000000+2000000000ULL;
    bool failed = false;
    try { checkRecords(planMany,manifestMany,adsRun,records,warmups); }
    catch (const std::exception&) { failed = true; }
    check(failed,"incorrect occupancy decision accepted");
    ++rejected;
  };
  checkOccupancy(1000,100);
  checkOccupancy(5000,4700);
  return rejected;
}

Json
checkNscRecords(const Json& plan, const Json& manifest, const Json& run,
                const std::filesystem::path& directory,
                const std::vector<Json>& traces)
{
  const auto tasks = decodeTaskPlan(plan, unsignedField(manifest,"window_ms"),
                                   unsignedField(manifest,"period_ms"));
  const auto origin = unsignedField(run,"origin_ns");
  const auto end = origin + unsignedField(manifest,"window_ms") * 1000000;
  const auto id = run.at("id").get<std::string>();
  const bool taskBusy = manifest.at("condition") == "task-busy";
  auto constrained = [&](int index, uint64_t time) {
    if (!taskBusy || time < origin) return false;
    check(traces.size() == 2, "task-busy NSC trace missing");
    bool full = false;
    for (const auto& change : traces.at(index).at("changes")) {
      if (unsignedField(change, "offset_ms") * 1000000 > time - origin) break;
      full = change.at("available").get<bool>();
    }
    return !full;
  };
  const auto users = readRecords(directory / (id + "-user.jsonl"));
  std::map<uint64_t,Json> terminals;
  size_t ready = 0, shutdown = 0, busy = 0;
  for (const auto& row : users) {
    const auto event = row.at("event").get<std::string>();
    if (event == "ready") {
      ++ready;
      check(unsignedField(row,"t0_ns") == origin
            && unsignedField(row,"t1_ns") == end
            && unsignedField(row,"ready_ns") < origin, "NSC window mismatch");
    }
    else if (event == "shutdown") ++shutdown;
    else if (event == "busy_reject") {
      ++busy;
      check(unsignedField(row,"task") > 0
            && unsignedField(row,"task") <= tasks.size()
            && (row.at("provider") == "/muas/drone1"
                || row.at("provider") == "/muas/drone2"), "NSC busy identity");
    }
    else if (event == "terminal") {
      const auto description = decodeTaskInput(row.at("command").get<std::string>());
      check(!description.warmup && description.id > 0 && description.id <= tasks.size()
            && taskJson(description) == taskJson(tasks.at(description.id - 1).description)
            && terminals.emplace(description.id,row).second, "NSC terminal task identity");
    }
  }
  check(ready == 1 && shutdown == 1 && terminals.size() == tasks.size(),
        "NSC incomplete task cohort");
  std::map<uint64_t,std::vector<Json>> executions;
  size_t providerReady = 0, providerShutdown = 0, providerBusy = 0;
  for (const auto& node : {"drone1","drone2"}) {
    const auto expected = std::string("/muas/") + node;
    for (const auto& row : readRecords(directory / (id + "-" + node + ".jsonl"))) {
      const auto event = row.at("event").get<std::string>();
      if (event == "ready") {
        ++providerReady;
        check(row.at("provider") == expected, "NSC ready Producer mismatch");
      }
      else if (event == "shutdown") {
        ++providerShutdown;
        check(row.at("provider") == expected, "NSC shutdown Producer mismatch");
      }
      else if (event == "busy_reject") {
        ++providerBusy;
        check(row.at("provider") == expected
              && unsignedField(row,"mono_ns") >= origin,
              "NSC busy Producer mismatch");
      }
      else if (event == "task_busy_reject") {
        ++providerBusy;
        const auto task = decodeTaskInput(row.at("command").get<std::string>());
        const int index = expected == "/muas/drone1" ? 0 : 1;
        check(taskBusy && !task.warmup && row.at("provider") == expected
              && constrained(index, unsignedField(row,"mono_ns"))
              && task.memory == (index == 1),
              "NSC task-specific busy truth mismatch");
      }
      else if (event == "execution") {
        const auto description = decodeTaskInput(row.at("command").get<std::string>());
        const int index = expected == "/muas/drone1" ? 0 : 1;
        check(!description.warmup && description.id > 0 && description.id <= tasks.size()
              && taskJson(description) == taskJson(tasks.at(description.id - 1).description)
              && row.at("provider") == expected && row.at("payload_valid") == true
              && !(constrained(index, unsignedField(row,"begin_ns"))
                   && description.memory == (index == 1))
              && unsignedField(row,"begin_ns") >= origin
                + tasks.at(description.id - 1).arrivalMs * 1000000
              && unsignedField(row,"end_ns") >= unsignedField(row,"begin_ns"),
              "NSC unbound execution");
        executions[description.id].push_back(row);
      }
    }
  }
  check(providerReady == 2 && providerShutdown == 2
        && providerBusy >= busy, "NSC Provider lifecycle/busy records");
  size_t successes = 0, clientSuccesses = 0, duplicateExecutions = 0, lateExecutions = 0;
  std::vector<uint64_t> incorrectTasks;
  for (const auto& task : tasks) {
    const auto id = task.description.id;
    const auto& row = terminals.at(id);
    const auto planned = origin + task.arrivalMs * 1000000;
    const auto deadline = planned + 2000000000ULL;
    const auto completed = unsignedField(row,"terminal_ns");
    check(row.at("measured") == true && row.at("planned_ns") == planned
          && row.at("deadline_ns") == deadline && row.at("command") == taskInput(task.description)
          && unsignedField(row,"begin_ns") >= planned
          && completed >= unsignedField(row,"begin_ns")
          && (row.at("target") == "/muas/drone1" || row.at("target") == "/muas/drone2"),
          "NSC planned task/deadline mismatch");
    const auto status = row.at("terminal").get<std::string>();
    check(status == "success" || status == "timeout" || status == "response_error",
          "NSC invalid terminal state");
    const auto found = executions.find(id);
    const auto count = found == executions.end() ? 0 : found->second.size();
    duplicateExecutions += count > 1 ? count - 1 : 0;
    if (found != executions.end()) {
      for (const auto& execution : found->second)
        if (unsignedField(execution,"end_ns") >= deadline) ++lateExecutions;
    }
    if (count > 1 || (found != executions.end() && std::any_of(
        found->second.begin(),found->second.end(),[&](const Json& execution) {
          return unsignedField(execution,"end_ns") >= deadline;
        }))) incorrectTasks.push_back(id);
    if (status == "success") {
      ++clientSuccesses;
      check(row.at("payload_verified") == true && completed < deadline
            && found != executions.end()
            && std::any_of(found->second.begin(),found->second.end(),[&](const Json& execution) {
              return execution.at("provider") == row.at("target")
                  && unsignedField(execution,"end_ns") <= completed;
            }), "NSC success without bound execution");
      if (count == 1 && unsignedField(found->second.front(),"end_ns") < deadline)
        ++successes;
    }
    else {
      check(row.at("payload_verified") == false
            && (status != "timeout" || completed >= deadline),
            "NSC invalid failed terminal");
    }
  }
  size_t executionCount = 0;
  for (const auto& pair : executions) executionCount += pair.second.size();
  return {{"event","spec206_nsc_oracle_complete"},{"qualified",true},
          {"run_id",run.at("id")},{"tasks",tasks.size()},{"successes",successes},
          {"client_successes",clientSuccesses},{"duplicate_executions",duplicateExecutions},
          {"late_executions",lateExecutions},{"incorrect_tasks",incorrectTasks},
          {"executions",executionCount},{"busy_rejects",busy}};
}

Json
checkArchive(const std::string& manifestPath, const std::string& runId)
{
  const std::filesystem::path path(manifestPath);
  const auto directory = path.parent_path();
  std::ifstream manifestFile(path), planFile(directory / "task-plan.json");
  check(static_cast<bool>(manifestFile) && static_cast<bool>(planFile),"missing archive inputs");
  check(std::filesystem::file_size(path) <= 16 * 1024 * 1024
        && std::filesystem::file_size(directory / "task-plan.json") <= 131072,"archive metadata bounds");
  const std::string planText((std::istreambuf_iterator<char>(planFile)),{});
  const auto manifest = Json::parse(manifestFile), plan = Json::parse(planText);
  check(manifest.at("schema") == "ndnsf-dynamic205-v2"
        || manifest.at("schema") == "ndnsf-dynamic205-e2-v1"
        || manifest.at("schema") == "ndnsf-dynamic205-e3-v1"
        || manifest.at("schema") == "ndnsf-spec206-comparison-v1"
        || manifest.at("schema") == "ndnsf-spec207-coarse-v1"
        || manifest.at("schema") == "ndnsf-feedback-ablation-v1", "wrong archive schema");
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(planText.data()),planText.size(),digest);
  std::ostringstream hash;
  for (auto value : digest) hash << std::hex << std::setw(2) << std::setfill('0') << unsigned(value);
  check(manifest.at("task_plan").at("file") == "task-plan.json"
        && manifest.at("task_plan").at("sha256") == hash.str(),"task plan hash mismatch");
  std::vector<Json> runs;
  for (const auto& run : manifest.at("runs")) if (run.at("id") == runId) runs.push_back(run);
  check(runs.size() == 1 && !runId.empty()
        && runId.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_")
        == std::string::npos,"missing/ambiguous run identity");
  auto providers = readRecords(directory / (runId + "-drone1.jsonl"));
  const auto second = readRecords(directory / (runId + "-drone2.jsonl"));
  providers.insert(providers.end(),second.begin(),second.end());
  std::vector<Json> traces;
  const bool comparison = manifest.at("schema") == "ndnsf-spec206-comparison-v1"
    || manifest.at("schema") == "ndnsf-spec207-coarse-v1"
    || manifest.at("schema") == "ndnsf-feedback-ablation-v1";
  if (comparison) {
    check(manifest.at("condition") == "offline" || manifest.at("condition") == "busy"
          || manifest.at("condition") == "stable"
          || manifest.at("condition") == "task-busy", "invalid comparison condition");
    check(runs.front().at("arm") == "SOLICIT" || runs.front().at("arm") == "NSC"
          || runs.front().at("arm") == "FEEDBACK" || runs.front().at("arm") == "ACK_ALL"
          || (manifest.at("schema") == "ndnsf-spec207-coarse-v1"
              && runs.front().at("arm") == "ADS_COARSE"),
          "native comparison arm mismatch");
    const auto& capture = manifest.at("capture").at(runId + "-relay-any");
    check(capture.at("interface") == "any" && capture.at("exit") == 0
          && capture.at("drops") == 0
          && unsignedField(capture,"started_ns") < unsignedField(runs.front(),"origin_ns")
          && unsignedField(capture,"stopped_ns") >= unsignedField(runs.front(),"origin_ns")
            + unsignedField(manifest,"window_ms") * 1000000
          && capture.at("pcap") == runId + "-relay-any.pcap"
          && std::filesystem::file_size(directory / (runId + "-relay-any.pcap")) > 24,
          "comparison capture boundary");
  }
  if (manifest.at("schema") == "ndnsf-dynamic205-e3-v1" || comparison) {
    const auto& reference = manifest.at("availability_trace");
    check(reference.is_object() && reference.size() == 2
          && (reference.at("profile") == "stable" || reference.at("profile") == "slow"
              || reference.at("profile") == "fast")
          && reference.at("files").is_array() && reference.at("files").size() == 2,
          "archive trace references");
    for (size_t index = 0; index < 2; ++index) {
      const auto& entry = reference.at("files").at(index);
      const auto filename = "availability-" + std::to_string(index) + ".json";
      check(entry.is_object() && entry.size() == 3 && entry.at("file") == filename,
            "archive trace filename");
      const auto tracePath = directory / filename;
      check(std::filesystem::file_size(tracePath) <= 65536, "archive trace size");
      std::ifstream input(tracePath);
      check(static_cast<bool>(input), "archive trace missing");
      const std::string bytes((std::istreambuf_iterator<char>(input)), {});
      SHA256(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), digest);
      std::ostringstream traceHash;
      for (auto value : digest)
        traceHash << std::hex << std::setw(2) << std::setfill('0') << unsigned(value);
      check(entry.at("sha256") == traceHash.str(), "archive trace hash mismatch");
      std::map<int, std::set<std::string>> objectKeys;
      const auto trace = Json::parse(bytes, [&](int depth, Json::parse_event_t event, Json& value) {
        check(depth <= 8, "archive trace nesting");
        if (event == Json::parse_event_t::object_start) objectKeys[depth + 1].clear();
        if (event == Json::parse_event_t::key)
          check(objectKeys[depth].insert(value.get<std::string>()).second,
                "archive duplicate trace key");
        if (event == Json::parse_event_t::object_end) objectKeys.erase(depth + 1);
        return true;
      });
      check(entry.at("producer") == trace.at("producer"), "archive trace reference Producer");
      traces.push_back(trace);
    }
  }
  if (comparison && manifest.at("condition") == "offline") {
    const auto linkRows = readRecords(directory / (runId + "-link-state.jsonl"));
    size_t expected = 0;
    for (size_t index = 0; index < traces.size(); ++index) {
      const auto name = index == 0 ? "drone1" : "drone2";
      for (const auto& change : traces[index].at("changes")) {
        const auto offset = unsignedField(change,"offset_ms");
        const auto found = std::find_if(linkRows.begin(),linkRows.end(),[&](const Json& row) {
          return row.at("node") == name && unsignedField(row,"offset_ms") == offset;
        });
        check(found != linkRows.end()
              && found->at("mechanism") == "bidirectional-netem-loss"
              && found->at("expected_up") == change.at("available")
              && found->at("observed_up") == change.at("available"),
              "offline link state mismatch");
        const auto planned = unsignedField(runs.front(),"origin_ns") + offset * 1000000;
        const auto applied = unsignedField(*found,"applied_ns");
        check(applied >= planned && applied - planned <= 250000000,
              "offline link transition late");
        ++expected;
      }
    }
    check(linkRows.size() == expected,"extra/missing offline link event");
    traces.clear(); // The Provider's local capacity remains available during link outage.
  }
  if (comparison && runs.front().at("arm") == "NSC")
    return checkNscRecords(plan, manifest, runs.front(), directory, traces);
  return checkRecords(plan, manifest, runs.front(),
                      readRecords(directory / (runId + "-user.jsonl")), providers, traces);
}
} // namespace

int main(int argc, char** argv) try {
  using namespace dynamic204;
  if (argc > 1) {
    const Options options(argc,argv);
    check(options.values.size() == 2,"archive mode requires manifest and run-id");
    std::cout << checkArchive(options.get("--manifest"),options.get("--run-id")).dump() << std::endl;
    return 0;
  }
  auto require=[](bool condition,const char* why){if(!condition)throw std::runtime_error(why);};
  ndn::KeyChain keys("pib-memory:dynamic-oracle","tpm-memory:dynamic-oracle");
  const auto cert=keys.createIdentity(ndn::Name("/oracle/provider/A"),ndn::RsaKeyParams(2048))
    .getDefaultKey().getDefaultCertificate();
  const auto wrong=keys.createIdentity(ndn::Name("/oracle/provider/B"),ndn::RsaKeyParams(2048))
    .getDefaultKey().getDefaultCertificate();
  const uint64_t now=10000000000;
  Snapshot s{cert.getIdentity(),"session",1,now-1000000,now-1000000+TTL_NS,true,8,4};
  auto make=[&](const Snapshot& value,const ndn::Name& name,const ndn::security::Certificate& signer) {
    ndn::Data data(name);data.setContent(std::string_view(snapshotJson(value).dump()));
    keys.sign(data,ndn::security::signingByCertificate(signer));return data;
  };
  auto name=ndn::Name(statusPrefix(cert.getIdentity())).append("session").appendVersion(1);
  const auto good=make(s,name,cert);
  require(verifySnapshot(good,cert,"session",now).available,"valid snapshot rejected");
  int denials=0;
  auto denied=[&](const ndn::Data& data,const char* why) {
    bool rejected=false;try{verifySnapshot(data,cert,"session",now);}catch(const std::exception&){rejected=true;}
    require(rejected,why);++denials;
  };
  denied(make(s,name,wrong),"wrong signer accepted");
  denied(make(s,ndn::Name("/other/NDNSF/service-status/session").appendVersion(1),cert),"wrong namespace accepted");
  auto altered=s;altered.session="old-session";denied(make(altered,name,cert),"old session accepted");
  altered=s;altered.published=now-TTL_NS;altered.expires=now;denied(make(altered,name,cert),"expired snapshot accepted");
  altered=s;altered.published=now+1;altered.expires=altered.published+TTL_NS;
  denied(make(altered,name,cert),"future snapshot accepted");
  denied(make(s,ndn::Name(statusPrefix(cert.getIdentity())).append("session").appendVersion(2),cert),"version mismatch accepted");
  auto tampered=good;tampered.setContent(std::string_view("{}"));denied(tampered,"tampered Data accepted");
  auto coarse=s;
  coarse.expires=coarse.published+COARSE_AD_TTL_NS;
  const auto coarseData=make(coarse,name,cert);
  require(verifySnapshot(coarseData,cert,"session",now,COARSE_AD_TTL_NS).expires
            == coarse.expires,"valid coarse advertisement rejected");
  auto coarseDenied=[&](const ndn::Data& data,const char* why) {
    bool rejected=false;
    try { verifySnapshot(data,cert,"session",now,COARSE_AD_TTL_NS); }
    catch(const std::exception&) { rejected=true; }
    require(rejected,why);++denials;
  };
  coarseDenied(good,"short status accepted as coarse advertisement");
  coarseDenied(make(coarse,name,wrong),"wrong coarse signer accepted");
  coarseDenied(make(coarse,ndn::Name(statusPrefix(cert.getIdentity())).append("session")
                    .appendNumber(1).appendVersion(1),cert),
               "status name accepted as coarse advertisement");
  altered=s;altered.computeUnits=-1;
  denied(make(altered,name,cert),"negative resources accepted");
  altered=s;altered.memoryUnits=1025;
  denied(make(altered,name,cert),"unbounded resources accepted");
  for (const auto& field : {"compute_units", "memory_units"}) {
    for (const auto& value : {Json(8.5), Json(uint64_t{4294967304}), Json(-1)}) {
      auto invalid = snapshotJson(s);
      invalid[field] = value;
      auto packet = good;
      packet.setContent(std::string_view(invalid.dump()));
      keys.sign(packet,ndn::security::signingByCertificate(cert));
      denied(packet,"invalid numeric resource accepted");
      bool rejected = false;
      try { resourceUnits(invalid,field); }
      catch (const std::exception&) { rejected = true; }
      require(rejected,"ACK resource reader accepted invalid number");
    }
    require(resourceUnits(Json{{field,0}},field)==0
            && resourceUnits(Json{{field,1024}},field)==1024,"resource boundary rejected");
  }
  for (const auto& value : {Json(-1), Json(1.5), Json(1e100)}) {
    auto invalid = snapshotJson(s);
    invalid["published_ns"] = value;
    auto packet = good;
    packet.setContent(std::string_view(invalid.dump()));
    keys.sign(packet,ndn::security::signingByCertificate(cert));
    denied(packet,"invalid timestamp accepted");
    bool rejected = false;
    try { unsignedField(Json{{"observed_ns",value}},"observed_ns"); }
    catch (const std::exception&) { rejected = true; }
    require(rejected,"ACK timestamp reader accepted invalid number");
  }
  const TaskDescription compute{1, false, false}, memory{2, true, false};
  require(taskScore(compute,true,8,4)==2000 && taskScore(compute,true,4,8)==1000,
          "compute task preference/overlap");
  require(taskScore(memory,true,8,4)==1000 && taskScore(memory,true,4,8)==2000,
          "memory task preference/overlap");
  require(taskScore(compute,false,8,4)==0 && taskScore(memory,true,8,3)==0
          && taskScore(compute,true,3,8)==0,"ineligible task accepted");
  for (const auto& task : {compute, memory, TaskDescription{1, false, true},
                           TaskDescription{1000000000, true, false}}) {
    const auto descriptor = taskJson(task).dump();
    const auto parsed = decodeTask(descriptor);
    const auto input = taskInput(task);
    const auto restored = decodeTaskInput(input);
    require(parsed.id == task.id && parsed.memory == task.memory && parsed.warmup == task.warmup,
            "descriptor roundtrip");
    require(restored.id == task.id && restored.memory == task.memory
            && restored.warmup == task.warmup && input.size() == 64,"private input roundtrip");
    require(descriptor.find("private-input") == std::string::npos,"descriptor contains private input");
  }
  std::string expected = "T205|M|C|0000000001|private-input";
  expected.resize(64, '_');
  require(taskInput(compute) == expected,"independent private wire vector");
  auto invalidTask = [&](const std::string& text, bool input) {
    bool rejected = false;
    try {
      if (input) decodeTaskInput(text); else decodeTask(text);
    }
    catch (const std::exception&) { rejected = true; }
    require(rejected,"malformed task accepted");
    ++denials;
  };
  invalidTask("",false);
  invalidTask(std::string(257,'x'),false);
  invalidTask("{}",false);
  invalidTask(taskJson(compute).dump()+" ",false);
  for (const auto& field : {"task", "class", "phase", "version"}) {
    auto invalid = taskJson(compute);
    invalid[field] = -1;
    invalidTask(invalid.dump(),false);
  }
  auto zero = taskJson(compute);zero["task"]=0;invalidTask(zero.dump(),false);
  invalidTask(std::string(64,'w'),true);
  invalidTask(expected.substr(1),true);
  auto damaged=expected;damaged[9]='x';invalidTask(damaged,true);
  damaged=expected;damaged.back()='x';invalidTask(damaged,true);
  auto makePlan = [](uint64_t window, uint64_t period) {
    Json tasks = Json::array();
    for (uint64_t arrival = 0, id = 1; arrival < window; arrival += period, ++id) {
      tasks.push_back({{"task",id},{"arrival_ms",arrival},
                       {"class",id % 2 ? "compute" : "memory"}});
    }
    return Json{{"version",1},{"window_ms",window},{"period_ms",period},{"tasks",tasks}};
  };
  require(decodeTaskPlan(makePlan(60000,100),60000,100).size()==600,"maximum plan count");
  require(decodeTaskPlan(makePlan(60000,1000),60000,1000).size()==60,"1 Hz plan count");
  const auto sparse = decodeTaskPlan(makePlan(60000,4700),60000,4700);
  require(sparse.size()==13 && sparse.back().arrivalMs==56400,"sparse arrival grid");
  auto invalidPlan = [&](Json plan) {
    bool rejected = false;
    try { decodeTaskPlan(plan,3000,100); }
    catch (const std::exception&) { rejected = true; }
    require(rejected,"malformed arrival plan accepted");
    ++denials;
  };
  auto plan = makePlan(3000,100);plan["tasks"][1]["task"]=uint64_t{1};invalidPlan(plan);
  plan = makePlan(3000,100);plan["tasks"][1]["arrival_ms"]=uint64_t{0};invalidPlan(plan);
  plan = makePlan(3000,100);plan["tasks"][0]["class"]="unknown";invalidPlan(plan);
  plan = makePlan(3000,100);plan["period_ms"]=0;invalidPlan(plan);
  plan = makePlan(3000,100);plan["tasks"][0]["arrival_ms"]=0.5;invalidPlan(plan);
  plan = makePlan(3000,100);plan["tasks"].erase(plan["tasks"].begin());invalidPlan(plan);
  LocalState a(0,500),b(1,500);a.setOrigin(now);b.setOrigin(now);
  require(a.available(now)&&!b.available(now),"initial trace phase");
  require(!a.available(now+500000000)&&b.available(now+500000000),"transition admission state");
  require(a.available(now+1000000000)&&!b.available(now+1000000000),"trace cycle");
  const auto trace = Json::parse(R"({"version":1,"producer":"/example/hello/provider/A",
    "window_ms":3000,"tail":"hold","changes":[
    {"offset_ms":0,"available":false},{"offset_ms":731,"available":true},
    {"offset_ms":1789,"available":false}]})");
  const ndn::Name traceProducer("/example/hello/provider/A");
  const auto changes = decodeAvailabilityTrace(trace, traceProducer, 3000);
  LocalState irregular(0, 0, false, changes);
  require(irregular.available(now), "trace warmup before origin publication");
  irregular.setOrigin(now);
  require(irregular.available(now - 1) && !irregular.available(now), "trace origin boundary");
  require(!irregular.available(now + 731000000 - 1)
          && irregular.available(now + 731000000), "trace exact first transition");
  require(irregular.available(now + 1789000000 - 1)
          && !irregular.available(now + 1789000000)
          && !irregular.available(now + 65000000000ULL), "trace transition and tail hold");
  auto invalidTrace = [&](Json value) {
    bool rejected = false;
    try { decodeAvailabilityTrace(value, traceProducer, 3000); }
    catch (const std::exception&) { rejected = true; }
    require(rejected, "malformed availability trace accepted");
    ++denials;
  };
  auto badTrace = trace; badTrace["producer"] = "/example/hello/provider/B"; invalidTrace(badTrace);
  badTrace = trace; badTrace["tail"] = "repeat"; invalidTrace(badTrace);
  badTrace = trace; badTrace["window_ms"] = 3001; invalidTrace(badTrace);
  badTrace = trace; badTrace["extra"] = true; invalidTrace(badTrace);
  badTrace = trace; badTrace["changes"] = Json::array(); invalidTrace(badTrace);
  badTrace = trace; badTrace["changes"][0]["offset_ms"] = 1; invalidTrace(badTrace);
  badTrace = trace; badTrace["changes"][1]["offset_ms"] = -1; invalidTrace(badTrace);
  badTrace = trace; badTrace["changes"][1]["offset_ms"] = 0.5; invalidTrace(badTrace);
  badTrace = trace; badTrace["changes"][1]["offset_ms"] = 0; invalidTrace(badTrace);
  badTrace = trace; badTrace["changes"][1]["offset_ms"] = 3000; invalidTrace(badTrace);
  badTrace = trace; badTrace["changes"][1]["offset_ms"] = UINT64_MAX; invalidTrace(badTrace);
  badTrace = trace; badTrace["changes"][1]["available"] = false; invalidTrace(badTrace);
  badTrace = trace; badTrace["changes"][1]["available"] = 1; invalidTrace(badTrace);
  badTrace = trace; badTrace["changes"][1]["extra"] = 0; invalidTrace(badTrace);
  badTrace = trace; badTrace.erase("tail"); invalidTrace(badTrace);
  badTrace = trace; badTrace["version"] = 1.0; invalidTrace(badTrace);
  badTrace = trace; badTrace["version"] = true; invalidTrace(badTrace);
  badTrace = trace; badTrace["producer"] = "/unknown"; invalidTrace(badTrace);
  badTrace = trace; badTrace["changes"] = Json::array();
  for (uint64_t index = 0; index < 256; ++index) {
    badTrace["changes"].push_back({{"offset_ms", index}, {"available", index % 2 == 0}});
  }
  require(decodeAvailabilityTrace(badTrace, traceProducer, 3000).size() == 256,
          "maximum availability trace rejected");
  badTrace["changes"].push_back({{"offset_ms", uint64_t{256}}, {"available", true}});
  invalidTrace(badTrace);
  auto traceBytes = trace.dump();
  traceBytes.resize(65536, ' ');
  require(parseAvailabilityTrace(traceBytes) == trace, "64KiB trace rejected");
  for (const auto& bytes : {traceBytes + " ",
       std::string(R"({"producer":"A","producer":"B"})"),
       std::string(R"({"changes":[{"available":true,"available":false}]})")}) {
    bool rejected = false;
    try { parseAvailabilityTrace(bytes); }
    catch (const std::exception&) { rejected = true; }
    require(rejected, "oversized or duplicate-key trace accepted");
    ++denials;
  }
  for (const auto& mode : {std::make_pair(500, false), std::make_pair(0, true)}) {
    bool rejected = false;
    try { LocalState mixed(0, mode.first, mode.second, changes); }
    catch (const std::exception&) { rejected = true; }
    require(rejected, "mixed trace mode accepted");
    ++denials;
  }
  const auto& wire=good.wireEncode();
  ndn::Data decoded(ndn::Block(ndn::span<const uint8_t>{wire.data(),wire.size()}));
  require(decoded.wireEncode()==wire,"signed inner wire roundtrip");
  const auto archiveDenials = checkRecordFixtures();
  std::cout<<Json{{"event","dynamic_oracle_complete"},{"negative_cases",denials},
    {"signed_wire_roundtrip",true},{"state_transition",true},{"task_codec",true},
    {"task_preference_overlap",true},{"arrival_plan",true},{"irregular_trace",true},
    {"archive_negative_cases",archiveDenials},{"oracle",true}}.dump()<<std::endl;return 0;
}catch(const std::exception& e){std::cerr<<"DYNAMIC_ORACLE_FAILURE "<<e.what()<<std::endl;return 2;}
