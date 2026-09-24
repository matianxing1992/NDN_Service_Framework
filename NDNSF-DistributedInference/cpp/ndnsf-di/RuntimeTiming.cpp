#include "NDNSF-DistributedInference/cpp/ndnsf-di/RuntimeTiming.hpp"

#include <ndn-cxx/util/logger.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>

namespace ndnsf::di {

NDN_LOG_INIT(ndnsf.di.RuntimeEvidence);

namespace {
std::string singleLine(std::string record)
{
  // A field or JSON writer must not create a second apparent evidence line.
  for (char& ch : record)
    if (ch == '\n' || ch == '\r') ch = ' ';
  return record;
}
} // namespace

void
logRuntimeEvidence(const std::string& record)
{
  // WARN is intentional: qualification runs normally use *=WARN, so required
  // evidence remains available while verbose component logs stay filtered.
  std::lock_guard<std::mutex> lock(runtimeTimingOutputMutex());
  NDN_LOG_WARN(singleLine(record));
}

void
logRuntimeTrace(const std::string& record)
{
  std::lock_guard<std::mutex> lock(runtimeTimingOutputMutex());
  NDN_LOG_TRACE(singleLine(record));
}

void
logRuntimeInfo(const std::string& record)
{
  std::lock_guard<std::mutex> lock(runtimeTimingOutputMutex());
  NDN_LOG_INFO(singleLine(record));
}

void
logRuntimeWarn(const std::string& record)
{
  std::lock_guard<std::mutex> lock(runtimeTimingOutputMutex());
  NDN_LOG_WARN(singleLine(record));
}

void
logRuntimeError(const std::string& record)
{
  std::lock_guard<std::mutex> lock(runtimeTimingOutputMutex());
  NDN_LOG_ERROR(singleLine(record));
}

namespace {

bool
phaseTimingEnabled()
{
  const char* value = std::getenv("NDNSF_PHASE_TIMING");
  if (value == nullptr) return false;
  const std::string text(value);
  return !(text.empty() || text == "0" || text == "false" ||
           text == "FALSE" || text == "no" || text == "NO");
}

bool
parseUnsigned(const std::string& text, std::uint64_t& value)
{
  if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
    return false;
  try {
    std::size_t consumed = 0;
    value = std::stoull(text, &consumed);
    return consumed == text.size();
  }
  catch (...) {
    return false;
  }
}

} // namespace

std::optional<RuntimePhaseObservation>
parseRuntimePhaseObservation(std::string_view input)
{
  std::string record(input);
  while (!record.empty() && (record.back() == '\n' || record.back() == '\r'))
    record.pop_back();
  if (record.find_first_of("\r\n") != std::string::npos)
    return std::nullopt;
  const auto marker = record.find("NDNSF_PHASE_TIMING");
  if (marker == std::string::npos)
    return std::nullopt;
  if (marker != 0 &&
      !std::isspace(static_cast<unsigned char>(record[marker - 1])))
    return std::nullopt;
  const auto afterMarker = marker + std::string("NDNSF_PHASE_TIMING").size();
  if (afterMarker < record.size() &&
      !std::isspace(static_cast<unsigned char>(record[afterMarker])))
    return std::nullopt;

  std::istringstream stream{record.substr(marker)};
  std::string token;
  std::unordered_map<std::string, std::string> fields;
  bool first = true;
  while (stream >> token) {
    if (first) {
      if (token != "NDNSF_PHASE_TIMING") return std::nullopt;
      first = false;
      continue;
    }
    const auto separator = token.find('=');
    if (separator == std::string::npos || separator == 0 ||
        separator + 1 == token.size())
      return std::nullopt;
    const auto key = token.substr(0, separator);
    const auto value = token.substr(separator + 1);
    if (key.find_first_of("\r\n= \t") != std::string::npos ||
        value.find_first_of("\r\n \t") != std::string::npos ||
        !fields.emplace(key, value).second)
      return std::nullopt;
  }
  if (first)
    return std::nullopt;
  const auto required = [&fields](const char* name) -> const std::string* {
    const auto found = fields.find(name);
    return found == fields.end() || found->second.empty() ? nullptr : &found->second;
  };
  const auto role = required("component");
  const auto executionRole = required("executionRole");
  const auto phase = required("phase");
  const auto requestId = required("requestId");
  const auto attempt = required("attempt");
  const auto steady = required("steady_us");
  const auto wall = required("timestamp_us");
  if (!role || !executionRole || !phase || !requestId || !attempt || !steady || !wall)
    return std::nullopt;
  RuntimePhaseObservation result;
  result.role = *role;
  result.executionRole = *executionRole;
  result.phase = *phase;
  result.requestId = *requestId;
  result.attempt = *attempt;
  for (const auto* name : {"providerBootId", "providerName", "sessionId",
                           "conversationId", "inferenceEpoch", "contextEpoch",
                           "scope"}) {
    if (const auto found = fields.find(name); found != fields.end()) {
      if (found->second.empty()) return std::nullopt;
      if (std::string(name) == "providerBootId") result.providerBootId = found->second;
      if (std::string(name) == "providerName") result.providerName = found->second;
      if (std::string(name) == "sessionId") result.sessionId = found->second;
      if (std::string(name) == "conversationId") result.conversationId = found->second;
      if (std::string(name) == "inferenceEpoch") result.inferenceEpoch = found->second;
      if (std::string(name) == "contextEpoch") result.contextEpoch = found->second;
      if (std::string(name) == "scope") result.scope = found->second;
    }
  }
  if (const auto found = fields.find("tokenIndex"); found != fields.end() &&
      (!parseUnsigned(found->second, result.tokenIndex) || result.tokenIndex == 0))
    return std::nullopt;
  if (!parseUnsigned(*steady, result.steadyUs) ||
      !parseUnsigned(*wall, result.wallUs) ||
      result.steadyUs == 0 || result.wallUs == 0)
    return std::nullopt;
  return result;
}

bool
validateRuntimePhaseSequence(const std::vector<RuntimePhaseObservation>& observations,
                             std::string* error)
{
  const auto fail = [error](const char* message) {
    if (error) *error = message;
    return false;
  };
  if (observations.empty()) return fail("phase sequence is empty");
  const auto& first = observations.front();
  std::uint64_t previous = 0;
  const std::map<std::string, std::string> paired = {
    {"roleValidationBegin", "roleValidationEnd"},
    {"runnerPreparationBegin", "runnerPreparationEnd"},
    {"stageInputFetchBegin", "stageInputFetchEnd"},
    {"stageOutputPublishBegin", "stageOutputPublishEnd"},
    {"roleExecutionBegin", "roleExecutionEnd"},
    {"ortRunBegin", "ortRunEnd"},
  };
  const std::map<std::string, std::size_t> order = {
    {"submit", 10}, {"requestPublished", 20}, {"ackReceived", 30},
    {"ackVerified", 40}, {"ackClosed", 50}, {"planBegin", 60},
    {"planEnd", 70}, {"selectionCommitted", 80},
    {"roleValidationBegin", 90}, {"roleValidationEnd", 100},
    {"stageInputFetchBegin", 110}, {"stageInputFetchEnd", 120},
    {"runnerPreparationBegin", 130}, {"runnerPreparationEnd", 140},
    {"roleExecutionBegin", 150}, {"ortRunBegin", 160},
    {"ortRunEnd", 170}, {"roleExecutionEnd", 180},
    {"stageOutputPublishBegin", 190}, {"stageOutputPublishEnd", 200},
    {"tokenReceived", 210}, {"tokenDelivered", 215}, {"tokenEmitted", 220},
    {"checkpointCommitted", 230}, {"turnReady", 240}, {"terminal", 245},
    {"closeBegin", 250}, {"drainEnd", 260},
  };
  std::map<std::string, std::size_t> open;
  std::map<std::string, std::size_t> beginSeen;
  std::map<std::string, std::size_t> seen;
  std::size_t previousOrder = 0;
  std::size_t previousScopedGroup = 0;
  std::uint64_t previousTokenIndex = 0;
  std::uint64_t previousCliTokenIndex = 0;
  std::set<std::uint64_t> pendingTokenDeliveries;
  std::set<std::string> receivedAckProviders;
  std::set<std::string> verifiedAckProviders;
  std::string contextEpoch = first.contextEpoch;
  bool checkpointSeen = false;
  std::string successorContextEpoch;
  std::set<std::string> selectionProviders;
  bool selectionAggregateSeen = false;
  const auto phaseKey = [](const std::string& phase,
                           const RuntimePhaseObservation& observation) {
    const bool scoped = phase == "stageInputFetchBegin" ||
                        phase == "stageInputFetchEnd" ||
                        phase == "stageOutputPublishBegin" ||
                        phase == "stageOutputPublishEnd";
    const bool providerScoped = phase == "ackReceived" ||
                                phase == "ackVerified" ||
                                phase == "selectionCommitted";
    if (scoped) return phase + "\x1f" + observation.scope;
    if (providerScoped) return phase + "\x1f" + observation.providerName;
    return phase;
  };
  for (const auto& observation : observations) {
    if (observation.requestId != first.requestId || observation.role != first.role ||
        observation.executionRole != first.executionRole ||
        observation.attempt != first.attempt ||
        observation.providerBootId != first.providerBootId ||
        observation.sessionId != first.sessionId ||
        observation.conversationId != first.conversationId ||
        observation.inferenceEpoch != first.inferenceEpoch)
      return fail("phase sequence merges request, role, attempt, session, or epoch identities");
    if (observation.phase == "checkpointCommitted") {
      if (checkpointSeen) return fail("checkpoint is duplicated");
      checkpointSeen = true;
      successorContextEpoch = observation.contextEpoch;
    }
    else if (observation.phase == "turnReady" ||
             observation.phase == "terminal" ||
             observation.phase == "closeBegin" ||
             observation.phase == "drainEnd") {
      if (checkpointSeen && observation.contextEpoch != successorContextEpoch)
        return fail("post-checkpoint phase changed context identity");
    }
    else if (checkpointSeen) {
      return fail("phase follows durable checkpoint before terminal lifecycle");
    }
    else if (observation.contextEpoch != contextEpoch) {
      return fail("pre-checkpoint phase changed context identity");
    }
    if (observation.steadyUs < previous)
      return fail("steady phase timestamps are not monotonic");
    const auto orderIt = order.find(observation.phase);
    const bool tokenPhase = observation.phase == "tokenReceived" ||
                            observation.phase == "tokenDelivered" ||
                            observation.phase == "tokenEmitted";
    const bool scopedPhase = observation.phase == "stageInputFetchBegin" ||
                             observation.phase == "stageInputFetchEnd" ||
                             observation.phase == "stageOutputPublishBegin" ||
                             observation.phase == "stageOutputPublishEnd";
    const bool ackScopedPhase = observation.phase == "ackReceived" ||
                                observation.phase == "ackVerified";
    if (tokenPhase && previousOrder > order.at("tokenEmitted"))
      return fail("token phase occurs after terminal checkpoint lifecycle");
    if (!tokenPhase && scopedPhase) {
      const auto group = observation.phase.find("stageInputFetch") == 0 ?
        order.at("stageInputFetchBegin") : order.at("stageOutputPublishBegin");
      if (group < previousScopedGroup || group < previousOrder)
        return fail("scoped phase group is not monotonic");
      previousScopedGroup = group;
      previousOrder = std::max(previousOrder, group);
    }
    else if (!tokenPhase && ackScopedPhase) {
      if (previousOrder > order.at("ackVerified"))
        return fail("ACK phase occurs after its admission window");
      previousOrder = std::max(previousOrder, order.at("ackReceived"));
    }
    else if (!tokenPhase && orderIt != order.end()) {
      if (orderIt->second < previousOrder)
        return fail("phase order is not monotonic");
      previousOrder = orderIt->second;
    }
    const auto observationKey = phaseKey(observation.phase, observation);
    const auto pairIt = paired.find(observation.phase);
    if (pairIt != paired.end()) {
      const auto key = phaseKey(observation.phase, observation);
      if (open.count(key) != 0 || beginSeen.count(key) != 0)
        return fail("phase begin is duplicated");
      open[key] = 1;
      beginSeen[key] = 1;
      previous = observation.steadyUs;
      continue;
    }
    for (const auto& pair : paired) {
      if (pair.second != observation.phase) continue;
      const auto beginKey = phaseKey(pair.first, observation);
      const auto endKey = phaseKey(pair.second, observation);
      if (open.erase(beginKey) == 0 || seen.count(endKey) != 0)
        return fail("phase end is missing its begin or is duplicated");
      seen[endKey] = 1;
      goto phase_processed;
    }
    if (observation.phase == "ackVerified") {
      if (observation.providerName.empty() ||
          receivedAckProviders.count(observation.providerName) == 0)
        return fail("ackVerified has no matching provider ACK");
      if (!verifiedAckProviders.insert(observation.providerName).second)
        return fail("ackVerified is duplicated for a provider");
    }
    else if (observation.phase == "ackReceived") {
      if (observation.providerName.empty() ||
          !receivedAckProviders.insert(observation.providerName).second)
        return fail("ackReceived is duplicated or missing provider identity");
    }
    if (observation.phase != "tokenReceived" && observation.phase != "tokenDelivered" &&
        observation.phase != "tokenEmitted" &&
        observation.phase != "selectionCommitted" &&
        orderIt != order.end() && seen.count(observationKey) != 0)
      return fail("phase is duplicated");
    if (observation.phase == "tokenReceived") {
      if (first.executionRole == "cli-output" || observation.tokenIndex == 0 ||
          observation.tokenIndex <= previousTokenIndex)
        return fail("tokenReceived index is missing or not increasing");
      previousTokenIndex = observation.tokenIndex;
      pendingTokenDeliveries.insert(observation.tokenIndex);
    }
    else if (observation.phase == "tokenDelivered") {
      if (first.executionRole == "cli-output" ||
          pendingTokenDeliveries.erase(observation.tokenIndex) == 0)
        return fail("tokenDelivered does not match the received token");
    }
    else if (observation.phase == "tokenEmitted") {
      if (first.executionRole != "cli-output" || observation.tokenIndex == 0 ||
          observation.tokenIndex <= previousCliTokenIndex)
        return fail("CLI token emission identity or index is invalid");
      previousCliTokenIndex = observation.tokenIndex;
    }
    else if (observation.phase == "selectionCommitted") {
      if (observation.providerName.empty()) {
        if (selectionAggregateSeen) return fail("aggregate Selection is duplicated");
        selectionAggregateSeen = true;
      }
      else if (!selectionProviders.insert(observation.providerName).second) {
        return fail("Selection provider is duplicated");
      }
    }
    if (orderIt != order.end()) seen[observationKey] = 1;
phase_processed:
    previous = observation.steadyUs;
  }
  if (!open.empty()) return fail("phase sequence has an unclosed begin event");
  if (!pendingTokenDeliveries.empty()) return fail("tokenReceived has no tokenDelivered");
  return true;
}

void
logRuntimePhase(const std::string& role,
                const std::string& phase,
                const std::string& requestId,
                const std::string& attempt,
                const std::vector<std::pair<std::string, std::string>>& fields)
{
  if (!phaseTimingEnabled() || role.empty() || phase.empty() || requestId.empty()) return;
  const auto steadyUs = std::chrono::duration_cast<std::chrono::microseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count();
  const auto wallUs = std::chrono::duration_cast<std::chrono::microseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
  std::ostringstream record;
  std::string executionRole = role;
  std::string canonicalAttempt = attempt.empty() ? "request" : attempt;
  if (canonicalAttempt == "0") canonicalAttempt = "request";
  std::map<std::string, std::string> identityFields;
  for (const auto& field : fields) {
    if (field.first == "role" || field.first == "executionRole") executionRole = field.second;
    else if (field.first == "attempt" || field.first == "attemptEpoch") canonicalAttempt = field.second;
    else if (field.first == "providerBootId" || field.first == "providerName" ||
             field.first == "sessionId" || field.first == "conversationId" ||
             field.first == "inferenceEpoch" || field.first == "contextEpoch") {
      identityFields[field.first] = field.second;
    }
  }
  record << "NDNSF_PHASE_TIMING"
         << " component=" << role
         << " executionRole=" << (executionRole.empty() ? "unknown" : executionRole)
         << " phase=" << phase
         << " steady_us=" << steadyUs
         << " timestamp_us=" << wallUs
         << " requestId=" << requestId;
  record << " attempt=" << canonicalAttempt
         << " providerBootId=" << (identityFields.count("providerBootId") ?
                                      identityFields["providerBootId"] : "none")
         << (identityFields.count("providerName") ?
             " providerName=" + identityFields["providerName"] : "")
         << " sessionId=" << (identityFields.count("sessionId") ?
                                  identityFields["sessionId"] : "none")
         << " conversationId=" << (identityFields.count("conversationId") ?
                                      identityFields["conversationId"] : "none")
         << " inferenceEpoch=" << (identityFields.count("inferenceEpoch") ?
                                      identityFields["inferenceEpoch"] : "none")
         << " contextEpoch=" << (identityFields.count("contextEpoch") ?
                                    identityFields["contextEpoch"] : "none");
  for (const auto& field : fields) {
    if (field.first.empty() || field.first == "role" || field.first == "executionRole" ||
        field.first == "attempt" || field.first == "attemptEpoch" ||
        field.first == "providerBootId" || field.first == "providerName" ||
        field.first == "sessionId" || field.first == "conversationId" ||
        field.first == "inferenceEpoch" || field.first == "contextEpoch" ||
        field.second.find_first_of("\r\n \t") != std::string::npos)
      continue;
    record << " " << field.first << "=" << field.second;
  }
  logRuntimeEvidence(record.str());
}

} // namespace ndnsf::di
