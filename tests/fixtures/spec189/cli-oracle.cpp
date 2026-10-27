#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeGenerationLimits.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ExecutionEvidence.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeConversationWire.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"

#include <filesystem>
#include <array>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <algorithm>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
std::string provider(unsigned index, bool materials, bool terminal, unsigned begin, unsigned end,
                     bool cacheHit = false, bool legacyCacheMarker = true)
{
  const auto name = "/example/ndnsf-qwen06b/provider-" + std::to_string(index);
  const auto identity = " requestId=request attemptEpoch=1 provider=" + name +
    " role=role" + std::to_string(index) + " planDigest=plan";
  std::string text = "NDNSF_DI_NATIVE_SELECTION_ACCEPTED" + identity +
    " manifestDigest=root graphDigest=graph initializerDigest=initializer artifactDigest=artifact" +
    " layerBegin=" + std::to_string(begin) + " layerEnd=" + std::to_string(end) + "\n";
  text += "NDNSF_DI_GRANT_VERIFIED" + identity + "\n";
  const auto stage = [&](const std::string& stage, const std::string& status = "observed") {
    return "NDNSF_DI_PROVIDER_STAGE stage=" + stage + " status=" + status + identity +
      " preparationId=1\n";
  };
  text += stage("GRANT_VERIFIED") + stage("EXECUTION_ENTERED") + stage("ASSEMBLY_STARTED");
  if (materials) {
    for (const auto& kind : {"root", "material-manifest", "material-payload"}) {
      const auto digest = std::string(kind) == "root" ? "root" : "digest";
      for (const auto& status : {"begin", "returned", "verified"}) {
        text += "NDNSF_DI_PROVIDER_MATERIAL_FETCH" + identity + " kind=" + kind +
          " name=/" + kind + " status=" + status + " expectedDigest=" + digest +
          " bytes=12 digest=" + digest + "\n";
      }
    }
  }
  if (cacheHit) {
    if (legacyCacheMarker) {
      text += "NDNSF_DI_PROVIDER_PREPARATION phase=CACHE_LOOKUP_HIT" + identity +
        " detail=recipe-addressed\n";
    }
    text += "NDNSF_DI_PROVIDER_PREPARATION phase=CACHE_ACQUIRE_DONE" + identity +
      " detail=template-hit\n";
    text += "NDNSF_DI_PROVIDER_PREPARATION phase=CACHE_TEMPLATE_REBIND" + identity +
      " detail=current-grant-ciphertext\n";
  }
  text += stage("RUNNER_READY");
  if (begin != 0) text += stage("DEPENDENCY_FETCH", "begin") + stage("DEPENDENCY_FETCH", "complete");
  text += stage("EXECUTION_COMPLETED");
  if (terminal) text += stage("TERMINAL");
  return text;
}

void write(const std::filesystem::path& path, const std::string& text)
{
  std::ofstream out(path); out << text;
  if (!out) throw std::runtime_error("fixture write failed");
}
} // namespace

int main(int argc, char** argv)
{
  if (argc != 2) return 2;
  char directory[] = "/tmp/spec189-cli-oracle-XXXXXX";
  if (!mkdtemp(directory)) return 2;
  const std::filesystem::path root(directory);
  struct Cleanup {
    std::filesystem::path root;
    ~Cleanup() { std::error_code error; std::filesystem::remove_all(root, error); }
  } cleanup{root};
  try {
    const std::string requester =
      "NDNSF_DI_NATIVE_SELECTION_COMMITTED requestId=request attemptEpoch=1 planDigest=plan\n"
      "NDNSF_COLLAB_ASSIGNMENT_SELECTED requestId=request providerName=/example/ndnsf-qwen06b/provider-0 role=role0\n"
      "NDNSF_COLLAB_ASSIGNMENT_SELECTED requestId=request providerName=/example/ndnsf-qwen06b/provider-1 role=role1\n"
      "NATIVE_REQUEST_SUCCEEDED request=request plan=plan\n";
    write(root / "requester-0.log", requester);
    unsigned cases = 0;
    const auto check = [&](const std::string& first, const std::string& second,
                           const std::string& reason,
                           const std::vector<std::string>& options = {}, int expectedFailure = 1) {
      write(root / "provider-0.log", first);
      write(root / "provider-1.log", second);
      const auto output = (root / "output.log").string();
      const auto child = fork();
      if (child < 0) throw std::runtime_error("fixture fork failed");
      if (child == 0) {
        const int fd = open(output.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0 || dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0) _exit(126);
        close(fd);
        std::vector<std::string> arguments{argv[1]};
        if (std::find(options.begin(), options.end(), "--deployment-state") == options.end())
          arguments.insert(arguments.end(), {"--run-root", directory});
        arguments.insert(arguments.end(), options.begin(), options.end());
        std::vector<char*> pointers;
        for (auto& argument : arguments) pointers.push_back(argument.data());
        pointers.push_back(nullptr);
        execv(argv[1], pointers.data());
        _exit(127);
      }
      int status = 0;
      if (waitpid(child, &status, 0) != child)
        throw std::runtime_error("oracle did not exit normally");
      std::ifstream in(output); std::ostringstream content; content << in.rdbuf();
      const auto text = content.str();
      if (!WIFEXITED(status) || text.find("AddressSanitizer") != std::string::npos ||
          text.find("LeakSanitizer") != std::string::npos ||
          text.find("UndefinedBehaviorSanitizer") != std::string::npos ||
          text.find("runtime error:") != std::string::npos)
        throw std::runtime_error("oracle crash/sanitizer report case=" +
                                 std::to_string(cases + 1) + ": " + text);
      const bool cache = std::find(options.begin(), options.end(), "--cache-compatibility") != options.end();
      const bool quiet = std::find(options.begin(), options.end(), "--quiet-performance") != options.end();
      const bool deployment = std::find(options.begin(), options.end(), "--deployment-state") != options.end();
      if ((cache || quiet || deployment) && (text.find("SPEC189_CPP_ORACLE_PASS") != std::string::npos ||
                    text.find("SPEC189_CPP_PLACEMENT_PASS") != std::string::npos))
        throw std::runtime_error("cache diagnostic emitted qualification PASS: " + text);
      if (reason.empty()) {
        const auto marker = deployment ? "SPEC217_CPP_DEPLOYMENT_EXECUTION_PASS"
          : quiet ? "SPEC189_CPP_QUIET_PERFORMANCE_PASS"
          : cache ? "SPEC189_CPP_CACHE_DIAGNOSTIC_PASS" : "SPEC189_CPP_ORACLE_PASS";
        if (WEXITSTATUS(status) != 0 || text.find(marker) == std::string::npos)
          throw std::runtime_error("positive CLI fixture rejected case=" +
                                   std::to_string(cases + 1) + ": " + text);
        if (cache && (text.find("\"scope\":\"cache-compatible-execution\"") == std::string::npos ||
                      text.find("\"materialFetch\":\"NOT_EVALUATED\"") == std::string::npos ||
                      text.find("\"fullPathQualification\":\"NOT_RUN\"") == std::string::npos))
          throw std::runtime_error("cache diagnostic scope missing: " + text);
        if (quiet && (text.find("\"scope\":\"quiet-performance\"") == std::string::npos ||
                      text.find("\"fullPathQualification\":\"NOT_RUN\"") == std::string::npos ||
                      text.find("\"grantAudit\":\"NOT_EVALUATED\"") == std::string::npos ||
                      text.find("\"placementAudit\":\"NOT_EVALUATED\"") == std::string::npos ||
                      text.find("\"restoreAudit\":\"NOT_EVALUATED\"") == std::string::npos ||
                      text.find("\"materialFetch\":\"NOT_EVALUATED\"") == std::string::npos))
          throw std::runtime_error("quiet performance scope missing: " + text);
        if (deployment && (text.find("\"scope\":\"deployment-quiet-execution\"") == std::string::npos ||
                           text.find("\"fullPathQualification\":\"NOT_RUN\"") == std::string::npos ||
                           text.find("\"checkpointAuthentication\":\"NOT_EVALUATED\"") == std::string::npos))
          throw std::runtime_error("deployment execution scope missing: " + text);
      }
      else if (WEXITSTATUS(status) != expectedFailure ||
               text.find(expectedFailure == 2 ? "usage:" : "reason=" + reason) == std::string::npos ||
               text.find("SPEC189_CPP_ORACLE_PASS") != std::string::npos ||
               text.find("SPEC189_CPP_CACHE_DIAGNOSTIC_PASS") != std::string::npos ||
               text.find("SPEC189_CPP_QUIET_PERFORMANCE_PASS") != std::string::npos ||
               text.find("SPEC217_CPP_DEPLOYMENT_EXECUTION_PASS") != std::string::npos) {
        throw std::runtime_error("negative CLI fixture mismatch case=" +
                                 std::to_string(cases + 1) + " expected=" + reason + ": " + text);
      }
      ++cases;
    };
    const auto first = provider(0, true, false, 0, 14);
    const auto second = provider(1, true, true, 14, 28);
    check(first, second, "");
    check(first, provider(1, false, true, 14, 28), "incomplete-material-sequence");
    check(first, provider(1, true, false, 14, 28), "tail-terminal-missing");
    check(provider(0, true, true, 0, 14), provider(1, true, false, 14, 28), "non-tail-terminal");
    check(first, provider(1, true, true, 15, 28), "range-coverage-mismatch");
    const std::vector<std::string> cacheOption{"--cache-compatibility"};
    const std::string requesterMode =
      "NDNSF_DI_CACHE_COMPATIBILITY_REQUESTER enabled=true protectedPublication=skipped\n";
    const std::string providerMode =
      "1790044168.187586 WARN: [ndnsf.di.RuntimeEvidence] "
      "NDNSF_DI_CACHE_COMPATIBILITY_CONFIG sourceDir=/verified/cache repoFetch=skipped-after-selection\n";
    const auto cacheFirst = providerMode + provider(0, false, false, 0, 14);
    const auto cacheSecond = providerMode + provider(1, false, true, 14, 28);
    write(root / "requester-0.log", requesterMode + requester);
    check(cacheFirst, cacheSecond, "", cacheOption);
    check(cacheFirst, cacheSecond, "incomplete-material-sequence");
    check(cacheFirst, cacheSecond, "incomplete-material-sequence", {"--placement-only"});
    check(provider(0, false, false, 0, 14), cacheSecond, "provider-mode-missing-or-mixed", cacheOption);
    check(cacheFirst, provider(1, false, true, 14, 28), "provider-mode-missing-or-mixed", cacheOption);
    check(providerMode + first, cacheSecond, "material-events-in-cache-mode", cacheOption);
    check(cacheFirst, providerMode + second, "material-events-in-cache-mode", cacheOption);
    check(providerMode + cacheFirst, cacheSecond, "provider-mode-missing-or-mixed", cacheOption);
    check("NDNSF_DI_CACHE_COMPATIBILITY_CONFIG sourceDir= repoFetch=skipped-after-selection\n" +
          provider(0, false, false, 0, 14), cacheSecond, "provider-mode-missing-or-mixed", cacheOption);
    check("NDNSF_DI_CACHE_COMPATIBILITY_CONFIG sourceDir=/cache repoFetch=enabled\n" +
          provider(0, false, false, 0, 14), cacheSecond, "provider-mode-missing-or-mixed", cacheOption);
    check(cacheFirst, providerMode + provider(1, false, false, 14, 28), "tail-terminal-missing", cacheOption);
    auto wrongIdentity = cacheSecond;
    const auto identityAt = wrongIdentity.find("requestId=request");
    wrongIdentity.replace(identityAt, std::string("requestId=request").size(), "requestId=wrong");
    check(cacheFirst, wrongIdentity, "missing-or-before-selection", cacheOption);
    check(cacheFirst, cacheSecond, "options", {"--placement-only", "--cache-compatibility"}, 2);
    check(cacheFirst, cacheSecond, "options", {"--cache-compatibility", "--placement-only"}, 2);
    write(root / "requester-0.log", requester);
    check(cacheFirst, cacheSecond, "requester-mode-missing-or-mixed", cacheOption);
    write(root / "requester-0.log", requesterMode + requesterMode + requester);
    check(cacheFirst, cacheSecond, "requester-mode-missing-or-mixed", cacheOption);
    write(root / "requester-0.log",
          "NDNSF_DI_CACHE_COMPATIBILITY_REQUESTER enabled=true enabled=false protectedPublication=skipped\n" + requester);
    check(cacheFirst, cacheSecond, "ambiguous-mode-fields", cacheOption);
    auto wrongRequester = requester;
    wrongRequester.replace(wrongRequester.find("plan=plan"), std::string("plan=plan").size(), "plan=wrong");
    write(root / "requester-0.log", requesterMode + wrongRequester);
    check(cacheFirst, cacheSecond, "identity-mismatch", cacheOption);
    write(root / "requester-0.log", requesterMode + requester.substr(0, requester.find("NATIVE_REQUEST_SUCCEEDED")));
    check(cacheFirst, cacheSecond, "requester-not-success", cacheOption);
    std::filesystem::create_directory(root / "requester");
    write(root / "requester/config.json", "{\"request\":{\"options_file\":\"options.json\"}}");
    const std::vector<std::string> multi{"--cache-compatibility", "--require-multi-token"};
    const auto generation = [&](const std::string& tokens, const std::string& reason,
                                const std::string& hint, unsigned budget, unsigned events,
                                bool checkpoint = true) {
      write(root / "requester/options.json", "{\"maxNewTokens\":" + std::to_string(budget) +
            ",\"eosTokenIds\":[0,4]}");
      write(root / "requester/output-0.bin", "{\"schema\":\"NDNSF-DI-FINAL-V1\",\"text\":\"你好\","
            "\"tokenIds\":" + tokens + ",\"finishReason\":\"" + reason + "\",\"finishHint\":\"" + hint + "\"}");
      write(root / "requester-0.log", requesterMode + requester +
            "NATIVE_STREAM_EVENTS=" + std::to_string(events) + "\n" +
            (checkpoint ? "NATIVE_CONVERSATION_CHECKPOINT_WRITTEN path=checkpoint\n" : ""));
    };
    generation("[1,2,0]", "eos", "EOS", 1024, 3);
    check(cacheFirst, cacheSecond, "", multi);
    generation("[1,2,4]", "eos", "EOS", 1024, 3);
    check(cacheFirst, cacheSecond, "", multi);
    generation("[1,2,3]", "max_tokens", "MAX_TOKENS", 3, 3);
    check(cacheFirst, cacheSecond, "", multi);
    generation("[1]", "max_tokens", "MAX_TOKENS", 3, 1);
    check(cacheFirst, cacheSecond, "not-bounded-multi-token", multi);
    generation("[1,2,3]", "max_tokens", "MAX_TOKENS", 2, 3);
    check(cacheFirst, cacheSecond, "not-bounded-multi-token", multi);
    generation("[1,2,3]", "eos", "EOS", 1024, 3);
    check(cacheFirst, cacheSecond, "invalid-stop-reason", multi);
    generation("[1,2,0]", "max_tokens", "MAX_TOKENS", 3, 3);
    check(cacheFirst, cacheSecond, "invalid-stop-reason", multi);
    generation("[1,0,2]", "max_tokens", "MAX_TOKENS", 3, 3);
    check(cacheFirst, cacheSecond, "tokens-after-eos", multi);
    generation("[1,2,0]", "eos", "EOS", 1024, 2);
    check(cacheFirst, cacheSecond, "stream-count-mismatch", multi);
    generation("[1,2,0]", "eos", "EOS", 1024, 3, false);
    check(cacheFirst, cacheSecond, "checkpoint-missing-or-duplicate", multi);
    generation("[1,-2,0]", "eos", "EOS", 1024, 3);
    check(cacheFirst, cacheSecond, "invalid-final-token", multi);
    generation("[1,2,0]", "eos", "EOS",
               static_cast<unsigned>(ndnsf::di::MAX_NATIVE_GENERATED_TOKENS), 3);
    check(cacheFirst, cacheSecond, "", multi);
    generation("[1,2,0]", "eos", "EOS",
               static_cast<unsigned>(ndnsf::di::MAX_NATIVE_GENERATED_TOKENS + 1), 3);
    check(cacheFirst, cacheSecond, "invalid-generation-options", multi);
    check(cacheFirst, cacheSecond, "options", {"--placement-only", "--require-multi-token"}, 2);
    generation("[1,2,0]", "eos", "EOS", 1024, 3);
    write(root / "requester/config.json", "{\"request\":{\"options_file\":\"other.json\"}}");
    check(cacheFirst, cacheSecond, "unsupported-request-config", multi);
    write(root / "requester/config.json", "{\"request\":{\"options_file\":\"options.json\",\"allow_replacement\":true}}");
    check(cacheFirst, cacheSecond, "unsupported-request-config", multi);
    write(root / "requester/config.json", "{\"request\":{\"options_file\":\"options.json\",\"max_new_tokens\":3}}");
    check(cacheFirst, cacheSecond, "generation-budget-override-mismatch", multi);
    write(root / "requester/config.json", "{\"request\":{\"options_file\":\"options.json\",\"max_new_tokens\":1024}}");
    check(cacheFirst, cacheSecond, "", multi);

    // Multi-round evidence follows the real requester filenames and checkpoint
    // fields. These are parser fixtures, not authenticated KV/model executions.
    using ndnsf::di::NativeJson;
    const auto name = [](const std::string& stem, unsigned round) {
      return stem + (round ? "-" + std::to_string(round) : "") + ".json";
    };
    const auto replaceAll = [](std::string text, const std::string& from, const std::string& to) {
      size_t at = 0;
      while ((at = text.find(from, at)) != std::string::npos) {
        text.replace(at, from.size(), to);
        at += to.size();
      }
      return text;
    };
    const auto roundText = [&](std::string text, unsigned round) {
      text = replaceAll(text, "requestId=request", "requestId=request-" + std::to_string(round));
      text = replaceAll(text, "request=request", "request=request-" + std::to_string(round));
      text = replaceAll(text, "planDigest=plan", "planDigest=plan-" + std::to_string(round));
      return replaceAll(text, "plan=plan", "plan=plan-" + std::to_string(round));
    };
    const auto chain = [&](unsigned rounds = 3, bool materials = false, unsigned budget = 1024,
                           bool residentCache = false, bool legacyCacheMarker = true) {
      std::pair<std::string, std::string> logs{materials ? "" : providerMode, materials ? "" : providerMode};
      unsigned prefix = 0;
      for (unsigned round = 0; round < rounds; ++round) {
        const unsigned generated = budget == 1 ? 1 : 3;
        const auto parentPrefix = prefix;
        prefix += (round ? 1 : 2) + generated;
        const auto hash = [](char c) { return "sha256:" + std::string(64, c); };
        NativeJson turn = round ? NativeJson{{"mode", "APPEND_DELTA"},
          {"parent_state_file", name("conversation-state", round - 1)}, {"delta_token_ids", {5}}}
          : NativeJson{{"mode", "FULL_CONTEXT"}, {"parent_context_epoch", 0},
              {"conversation_id", "conversation"}, {"canonical_token_ids", {8, 9}},
              {"expected_roles", {"role0", "role1"}}};
        NativeJson config{{"request", {{"options_file", name("options", round)},
            {"service", "/LLM/Qwen"}, {"security_policy_digest", hash('2')}}},
          {"conversation", {{"turn", turn}, {"checkpoint_output_file", name("conversation-state", round)},
            {"owner", {{"requester_identity", "/user"}, {"service_name", "/LLM/Qwen"},
              {"security_domain_digest", hash('2')}}}}}};
        NativeJson checkpoint{{"schema", "ndnsf-di-conversation-checkpoint-v1"}, {"version", 1},
          {"conversationId", "conversation"}, {"contextEpoch", round + 1}, {"parentContextEpoch", round},
          {"serviceName", "/LLM/Qwen"}, {"requesterIdentity", "/user"},
          {"modelContractDigest", hash('1')}, {"securityDomainDigest", hash('2')},
          {"planRoleMapDigest", hash('3')}, {"prefixTokenCount", prefix},
          {"logicalPrefixDigest", hash('4')}, {"checkpointDigest", hash(static_cast<char>('5' + round % 4))},
          {"roleReceiptDigests", {{"role0", hash('a')}, {"role1", hash('b')}}},
          {"issuedAtMs", 1000 + round}, {"expiresAtMs", 9000000}, {"signature", std::string(64, 'c')}};
        write(root / "requester" / name("config", round), config.dump());
        write(root / "requester" / name("options", round),
          NativeJson{{"maxNewTokens", budget}, {"eosTokenIds", {0, 4}}}.dump());
        write(root / "requester" / name("conversation-state", round), checkpoint.dump());
        write(root / "requester" / ("output-" + std::to_string(round) + ".bin"),
          NativeJson{{"schema", "NDNSF-DI-FINAL-V1"}, {"text", "你好"},
            {"tokenIds", budget == 1 ? NativeJson::array({1}) : NativeJson::array({1, 2, 0})},
            {"finishReason", budget == 1 ? "max_tokens" : "eos"},
            {"finishHint", budget == 1 ? "MAX_TOKENS" : "EOS"}}.dump());
        write(root / ("requester-" + std::to_string(round) + ".log"),
          (materials ? "" : requesterMode) + roundText(requester, round) +
          "NATIVE_STREAM_EVENTS=" + std::to_string(generated) + "\nNATIVE_CONVERSATION_CHECKPOINT_WRITTEN\n");
        for (unsigned p = 0; p < 2; ++p) {
          const bool roundMaterials = materials && (!residentCache || round == 0);
          auto events = roundText(provider(p, roundMaterials, p == 1, p * 14, (p + 1) * 14,
                                           residentCache && round != 0, legacyCacheMarker), round);
          if (round) {
            const auto restore = "1790044168 WARN: [ndnsf.di.RuntimeEvidence] "
              "NDNSF_DI_CONVERSATION_KV_RESTORED requestId=request-" + std::to_string(round) +
              " role=role" + std::to_string(p) + " parentContextEpoch=" + std::to_string(round) +
              " prefixTokenCount=" + std::to_string(parentPrefix) + "\n";
            events.insert(events.find("NDNSF_DI_PROVIDER_STAGE stage=EXECUTION_COMPLETED"), restore);
          }
          (p ? logs.second : logs.first) += events;
        }
      }
      return logs;
    };
    const auto mutate = [&](const std::string& file, const std::function<void(NativeJson&)>& change) {
      std::ifstream in(root / "requester" / file);
      NativeJson value; in >> value; change(value);
      write(root / "requester" / file, value.dump());
    };
    const std::vector<std::string> three{"--cache-compatibility", "--require-multi-token", "--rounds", "3"};
    auto logs = chain();
    check(logs.first, logs.second, "", three);
    logs = chain(3, true);
    check(logs.first, logs.second, "", {"--rounds", "3", "--require-multi-token"});
    logs = chain(3, true, 1024, true);
    check(logs.first, logs.second, "", {"--rounds", "3", "--require-multi-token"});
    logs = chain(3, true, 1024, true, false);
    check(logs.first, logs.second, "", {"--rounds", "3", "--require-multi-token"});
    logs = chain(3, true, 1024, true);
    logs = {replaceAll(
              replaceAll(logs.first, "phase=CACHE_LOOKUP_HIT", "phase=CACHE_LOOKUP_MISSING"),
              "phase=CACHE_ACQUIRE_DONE", "phase=CACHE_ACQUIRE_MISSING"),
            logs.second};
    check(logs.first, logs.second, "incomplete-material-sequence", {"--rounds", "3"});
    logs = chain(3, true, 1024, true);
    const auto cachedMaterial = logs.first.find("NDNSF_DI_PROVIDER_PREPARATION phase=CACHE_LOOKUP_HIT");
    if (cachedMaterial == std::string::npos) throw std::runtime_error("cache fixture marker missing");
    logs.first.insert(cachedMaterial, "NDNSF_DI_PROVIDER_MATERIAL_FETCH requestId=request-1 "
      "attemptEpoch=1 provider=/example/ndnsf-qwen06b/provider-0 role=role0 planDigest=plan-1 "
      "kind=root name=/root status=empty expectedDigest=root bytes=0 digest=\n");
    check(logs.first, logs.second, "material-events-on-cache-hit", {"--rounds", "3"});
    logs = chain(2, false, 1);
    check(logs.first, logs.second, "", {"--cache-compatibility", "--rounds", "2"});
    check(logs.first, logs.second, "invalid-generation-options",
      {"--cache-compatibility", "--rounds", "2", "--require-multi-token"});
    logs = chain(8);
    check(logs.first, logs.second, "", {"--cache-compatibility", "--rounds", "8"});
    for (const auto* bad : {"0", "9", "-1", "1.5", "abc", "999999999999999999999999"})
      check(logs.first, logs.second, "options", {"--rounds", bad}, 2);
    check(logs.first, logs.second, "options", {"--rounds"}, 2);
    check(logs.first, logs.second, "options", {"--rounds", "2", "--rounds", "3"}, 2);
    check(logs.first, logs.second, "options", {"--placement-only", "--rounds", "2"}, 2);

    // Each mutation starts from fresh three-round evidence, not a previous
    // failed mutation; cleanup owns every generated input and captured output.
    for (const auto& file : {"output-2.bin", "config-2.json", "options-2.json", "conversation-state-2.json"}) {
      logs = chain();
      std::filesystem::remove(root / "requester" / file);
      check(logs.first, logs.second, "missing-round-file", three);
    }
    logs = chain();
    std::filesystem::remove(root / "requester-2.log");
    check(logs.first, logs.second, "missing-round-file", three);
    const auto badJson = [&](const std::string& file, const std::string& reason,
                             const std::function<void(NativeJson&)>& change) {
      const auto fresh = chain();
      mutate(file, change);
      check(fresh.first, fresh.second, reason, three);
    };
    badJson("config-2.json", "parent-config-mismatch", [](NativeJson& j) {
      j["conversation"]["turn"]["parent_state_file"] = "conversation-state.json";
    });
    badJson("config-2.json", "parent-config-mismatch", [](NativeJson& j) {
      j["conversation"]["turn"]["parent_context_epoch"] = 1;
    });
    badJson("config-2.json", "checkpoint-output-path-mismatch", [](NativeJson& j) {
      j["conversation"]["checkpoint_output_file"] = "conversation-state.json";
    });
    for (const auto* key : {"contextEpoch", "parentContextEpoch"})
      badJson("conversation-state-2.json", "checkpoint-epoch-mismatch", [=](NativeJson& j) { j[key] = 1; });
    badJson("conversation-state-2.json", "invalid-integer", [](NativeJson& j) { j["contextEpoch"] = 3.0; });
    badJson("conversation-state-2.json", "checkpoint-prefix-mismatch", [](NativeJson& j) { j["prefixTokenCount"] = 12; });
    badJson("config.json", "checkpoint-prefix-mismatch", [](NativeJson& j) { j["conversation"]["turn"]["canonical_token_ids"] = {8}; });
    badJson("config-2.json", "checkpoint-prefix-mismatch", [](NativeJson& j) { j["conversation"]["turn"]["delta_token_ids"] = {5, 6}; });
    for (const auto* key : {"conversationId", "modelContractDigest", "securityDomainDigest", "planRoleMapDigest"})
      badJson("conversation-state-2.json", "checkpoint-binding-mismatch", [=](NativeJson& j) { j[key] = "wrong"; });
    badJson("conversation-state-2.json", "checkpoint-roles-mismatch", [](NativeJson& j) { j["roleReceiptDigests"].erase("role1"); });
    badJson("options-2.json", "invalid-generation-options", [](NativeJson& j) {
      j["maxNewTokens"] = ndnsf::di::MAX_NATIVE_GENERATED_TOKENS + 1;
    });
    badJson("output-2.bin", "invalid-stop-reason", [](NativeJson& j) { j["finishHint"] = "MAX_TOKENS"; });
    logs = chain();
    write(root / "requester-2.log", requesterMode + roundText(requester, 1) +
      "NATIVE_STREAM_EVENTS=3\nNATIVE_CONVERSATION_CHECKPOINT_WRITTEN\n");
    check(logs.first, logs.second, "duplicate-request-id", three);
    logs = chain();
    write(root / "requester-2.log", requesterMode);
    check(logs.first, logs.second, "requester-not-success", three);
    logs = chain();
    check(logs.first, replaceAll(logs.second, "requestId=request-2", "requestId=absent"),
      "round-selection-missing-or-duplicate", three);
    logs = chain();
    check(logs.first, replaceAll(logs.second, "planDigest=plan-2", "planDigest=wrong"), "identity-mismatch", three);
    const std::string restore = "NDNSF_DI_CONVERSATION_KV_RESTORED requestId=request-2 role=role1 parentContextEpoch=2 prefixTokenCount=9";
    logs = chain();
    check(logs.first, replaceAll(logs.second, restore, "REMOVED_RESTORE"), "restore-missing-or-duplicate", three);
    check(logs.first, replaceAll(logs.second, restore, restore + "\n" + restore), "restore-missing-or-duplicate", three);
    for (const auto& wrong : {replaceAll(restore, "role=role1", "role=role0"),
                             replaceAll(restore, "parentContextEpoch=2", "parentContextEpoch=1"),
                             replaceAll(restore, "prefixTokenCount=9", "prefixTokenCount=5")})
      check(logs.first, replaceAll(logs.second, restore, wrong), "restore-binding-mismatch", three);
    check(logs.first, replaceAll(logs.second, restore, restore + " role=role1"), "ambiguous-marker-fields", three);
    check(logs.first, replaceAll(logs.second, restore, replaceAll(restore, "request-2", "stale-request")),
      "restore-missing-or-duplicate", three);
    check(logs.first, replaceAll(logs.second,
      "stage=TERMINAL status=observed requestId=request-2", "stage=TERMINAL status=failed requestId=request-2"),
      "unsuccessful-stage", three);
    // A valid old round's material evidence cannot qualify a later round.
    logs = chain(3, true);
    check(logs.first, replaceAll(logs.second,
      "NDNSF_DI_PROVIDER_MATERIAL_FETCH requestId=request-2", "OLD_MATERIAL requestId=request-2"),
      "incomplete-material-sequence", {"--rounds", "3"});
    // These fixed counterexamples use the production provider serializer;
    // they prove measurement parsing, not authentication or inference.
    const auto quietFixture = [&](unsigned rounds = 3, bool missingCpu = false) {
      chain(rounds, true);
      std::string requesterRows;
      std::array<std::string, 2> providerRows;
      for (unsigned p = 0; p < 2; ++p) {
        providerRows[p] = NativeJson{
          {"schema", "ndnsf-di-native-request-statistics-v1"}, {"type", "serve-ready"},
          {"provider", "/example/ndnsf-qwen06b/provider-" + std::to_string(p)},
          {"providerBootId", "boot-" + std::to_string(p)}}.dump() + '\n';
      }
      for (unsigned round = 0; round < rounds; ++round) {
        const std::string request = "request-" + std::to_string(round);
        const std::string plan = "plan-" + std::to_string(round);
        const std::int64_t offset = round * 10000;
        requesterRows += NativeJson{
          {"schema", "ndnsf-di-native-requester-statistics-v1"}, {"type", "request"},
          {"turn", round}, {"requestId", request}, {"planDigest", plan},
          {"completed", true}, {"streaming", true}, {"observedEvents", 3},
          {"terminalSeen", true}, {"checkpointWritten", true},
          {"startedSteadyNs", offset + 100}, {"finishedSteadyNs", offset + 9000},
          {"startedProcessCpuNs", nullptr}, {"finishedProcessCpuNs", nullptr}}.dump() + '\n';
        write(root / ("requester-" + std::to_string(round) + ".log"), "");
        for (unsigned p = 0; p < 2; ++p) {
          ndnsf::di::NativeRequestStatistics statistics;
          statistics.requestId = request;
          statistics.planDigest = plan;
          statistics.provider = "/example/ndnsf-qwen06b/provider-" + std::to_string(p);
          statistics.providerBootId = "boot-" + std::to_string(p);
          statistics.role = "role" + std::to_string(p);
          statistics.attemptEpoch = 1;
          statistics.startedSteadyNs = offset + 200;
          statistics.finishedSteadyNs = offset + 8000;
          statistics.startedProcessCpuNs = missingCpu ? -1 : 100;
          statistics.finishedProcessCpuNs = missingCpu ? -1 : 7000;
          statistics.completionAdmitted = true;
          statistics.hasEpochStatistics = true;
          statistics.epochsExecuted = 4;
          statistics.eventsPublished = p == 1 ? 3 : 0;
          auto identity = std::make_shared<ndnsf::di::NativeBackendExecutionIdentity>();
          identity->requestId = request;
          identity->sessionId = request;
          identity->role = statistics.role;
          identity->providerBootId = statistics.providerBootId;
          identity->attemptEpoch = 1;
          for (unsigned epoch = 0; epoch < 4; ++epoch) {
            ndnsf::di::NativeBackendExecutionTiming timing;
            timing.identity = identity;
            timing.inferenceEpoch = epoch;
            timing.startedSteadyNs = offset + 1000 + epoch * 1000;
            timing.finishedSteadyNs = timing.startedSteadyNs + 500;
            timing.startedProcessCpuNs = missingCpu ? std::int64_t{-1} : 400 + epoch * 200;
            timing.finishedProcessCpuNs = missingCpu ? -1 : timing.startedProcessCpuNs + 100;
            timing.startedThreadCpuNs = missingCpu ? -1 : 30;
            timing.finishedThreadCpuNs = missingCpu ? -1 : 60;
            timing.completed = true;
            ndnsf::di::NativeRuntimeMetrics metrics;
            metrics.lastBackendExecution = timing;
            statistics.runtimeMetrics.push_back({statistics.role, epoch, metrics});
            ndnsf::di::NativeEpochCoordinatorResult::CacheObservation cache;
            cache.inferenceEpoch = epoch;
            cache.actualNewInputExtent = 1;
            cache.representedPrefixTokenCount = epoch + 1;
            cache.prefixWorkAvoided = epoch;
            statistics.cacheObservations.push_back(cache);
          }
          providerRows[p] += ndnsf::di::nativeRequestStatisticsToJson(statistics) + '\n';
        }
      }
      write(root / "requester-statistics.jsonl", requesterRows);
      for (unsigned p = 0; p < 2; ++p)
        write(root / ("provider-" + std::to_string(p) + "-statistics.jsonl"), providerRows[p]);
    };
    const std::vector<std::string> quiet{"--quiet-performance", "--rounds", "3",
                                        "--require-multi-token"};
    const auto badStatistics = [&](const std::string& file, unsigned row,
                                  const std::string& reason,
                                  const std::function<void(NativeJson&)>& change) {
      quietFixture();
      std::ifstream input(root / file);
      std::string line, result;
      unsigned index = 0;
      while (std::getline(input, line)) {
        auto value = NativeJson::parse(line);
        if (index++ == row) change(value);
        result += value.dump() + '\n';
      }
      input.close();
      write(root / file, result);
      check("", "", reason, quiet);
    };
    quietFixture();
    check("", "", "", quiet);
    quietFixture(3, true);
    check("", "", "", quiet);
    quietFixture(1);
    check("", "", "", {"--quiet-performance", "--require-multi-token"});
    badStatistics("requester-statistics.jsonl", 2, "statistics-incomplete-request",
      [](NativeJson& j) { j["completed"] = false; });
    for (const auto* field : {"terminalSeen", "checkpointWritten", "streaming"}) {
      badStatistics("requester-statistics.jsonl", 1, "stream-statistics-mismatch",
        [=](NativeJson& j) { j[field] = false; });
    }
    badStatistics("requester-statistics.jsonl", 1, "stream-statistics-mismatch",
      [](NativeJson& j) { j["observedEvents"] = 2; });
    for (const auto* field : {"requestId", "providerBootId", "planDigest"}) {
      badStatistics("provider-0-statistics.jsonl", 2, "statistics-binding",
        [=](NativeJson& j) { j[field] = "stale"; });
    }
    badStatistics("provider-0-statistics.jsonl", 2, "statistics-incomplete-request",
      [](NativeJson& j) { j["failureReason"] = "failed"; });
    badStatistics("provider-0-statistics.jsonl", 2, "statistics-epoch-count",
      [](NativeJson& j) { j["runtimeMetrics"].erase(0); });
    badStatistics("provider-0-statistics.jsonl", 2, "statistics-generation-coverage",
      [](NativeJson& j) {
        j["runtimeMetrics"].erase(j["runtimeMetrics"].size() - 1);
        j["cacheObservations"].erase(j["cacheObservations"].size() - 1);
        j["epochsExecuted"] = 3;
      });
    badStatistics("provider-0-statistics.jsonl", 2, "statistics-backend-window",
      [](NativeJson& j) {
        for (auto& observation : j["runtimeMetrics"]) {
          observation["lastBackendExecution"]["startedProcessCpuNs"] = 400;
          observation["lastBackendExecution"]["finishedProcessCpuNs"] = 6000;
        }
      });
    badStatistics("provider-0-statistics.jsonl", 2, "statistics-missing-backend",
      [](NativeJson& j) { j["runtimeMetrics"][0]["lastBackendExecution"] = nullptr; });
    for (const auto* field : {"requestId", "role", "providerBootId", "sessionId"}) {
      badStatistics("provider-0-statistics.jsonl", 2, "statistics-backend-binding",
        [=](NativeJson& j) { j["runtimeMetrics"][1]["lastBackendExecution"][field] = "stale"; });
    }
    badStatistics("provider-0-statistics.jsonl", 2, "statistics-backend-binding",
      [](NativeJson& j) { j["runtimeMetrics"][0]["lastBackendExecution"]["completed"] = false; });
    badStatistics("provider-0-statistics.jsonl", 2, "statistics-backend-binding",
      [](NativeJson& j) { j["runtimeMetrics"][0]["lastBackendExecution"]["attemptEpoch"] = 2; });
    badStatistics("provider-0-statistics.jsonl", 2, "statistics-clock-order",
      [](NativeJson& j) { j["finishedSteadyNs"] = 0; });
    badStatistics("provider-0-statistics.jsonl", 2, "invalid-integer",
      [](NativeJson& j) { j["finishedProcessCpuNs"] = nullptr; });
    badStatistics("provider-0-statistics.jsonl", 2, "statistics-backend-window",
      [](NativeJson& j) { j["runtimeMetrics"][0]["lastBackendExecution"]["finishedSteadyNs"] = 999999; });
    badStatistics("provider-0-statistics.jsonl", 2, "invalid-integer",
      [](NativeJson& j) { j["startedSteadyNs"] = 123.5; });
    badStatistics("requester-statistics.jsonl", 1, "statistics-request-window",
      [](NativeJson& j) { j["startedSteadyNs"] = 16000; });
    badStatistics("provider-1-statistics.jsonl", 2, "statistics-event-count",
      [](NativeJson& j) { j["eventsPublished"] = 2; });
    for (const auto& item : std::vector<std::pair<std::string, std::string>>{
           {"{}", "statistics-truncated-row"}, {"{broken}\n", "statistics-json"},
           {"{\"schema\":1,\"schema\":2}\n", "statistics-json"}}) {
      quietFixture();
      write(root / "provider-0-statistics.jsonl", item.first);
      check("", "", item.second, quiet);
    }
    quietFixture();
    mutate("conversation-state-2.json", [](NativeJson& j) { j["prefixTokenCount"] = 12; });
    check("", "", "checkpoint-prefix-mismatch", quiet);
    quietFixture();
    mutate("output-2.bin", [](NativeJson& j) { j["finishReason"] = "wrong"; });
    check("", "", "invalid-stop-reason", quiet);
    quietFixture();
    for (const auto* file : {"requester-statistics.jsonl", "provider-0-statistics.jsonl",
                              "provider-1-statistics.jsonl"}) {
      std::ifstream input(root / file);
      std::ostringstream text;
      text << input.rdbuf();
      input.close();
      write(root / file, replaceAll(text.str(), "request-1", "request-0"));
    }
    check("", "", "duplicate-request-id", quiet);
    quietFixture();
    check("", "", "usage", {"--quiet-performance", "--cache-compatibility"}, 2);
    const auto readJsonFile = [&](const std::filesystem::path& path) {
      std::ifstream input(path);
      NativeJson value;
      input >> value;
      return value;
    };
    const auto deploymentFixture = [&](bool explicitStatistics = false,
                                        const std::string& requesterId = "requester") {
      quietFixture(2);
      NativeJson turns = NativeJson::array();
      std::vector<std::int64_t> prefix;
      NativeJson requesterConfig;
      for (unsigned round = 0; round < 2; ++round) {
        const auto suffix = round ? "-1" : "";
        const auto legacy = readJsonFile(root / "requester" /
          (std::string("config") + suffix + ".json"));
        if (round == 0) requesterConfig = legacy;
        NativeJson turn;
        for (const auto& item : std::vector<std::pair<std::string, std::string>>{
               {"options_file", std::string("options") + suffix + ".json"},
               {"output_file", "output-" + std::to_string(round) + ".bin"},
               {"checkpoint_output_file", std::string("conversation-state") + suffix + ".json"}})
          turn[item.first] = (root / "requester" / item.second).string();
        turn["input_file"] = (root / ("input-" + std::to_string(round) + ".bin")).string();
        ndnsf::di::NamedTensor input;
        input.name = "input_ids";
        input.elementType = ndnsf::di::TensorElementType::Int64;
        input.shape = {1, 1};
        input.payload.assign(8, 0);
        input.payload[0] = 10 + round;
        const auto wire = ndnsf::di::encodeTensorBundle({input});
        write(turn.at("input_file").get<std::string>(), std::string(wire.begin(), wire.end()));
        prefix.push_back(10 + round);
        const auto output = readJsonFile(turn.at("output_file").get<std::string>());
        for (const auto& token : output.at("tokenIds")) prefix.push_back(token.get<std::int64_t>());
        auto checkpoint = readJsonFile(turn.at("checkpoint_output_file").get<std::string>());
        checkpoint["logicalPrefixDigest"] = ndnsf::di::nativeConversationPrefixDigest(prefix);
        checkpoint["prefixTokenCount"] = prefix.size();
        checkpoint["planRoleMapDigest"] = ndnsf::di::nativePlanningDigest(
          ndnsf::di::nativeCanonicalJson(NativeJson::array({
            NativeJson::array({"role0", "/example/ndnsf-qwen06b/provider-0"}),
            NativeJson::array({"role1", "/example/ndnsf-qwen06b/provider-1"})})));
        checkpoint["issuedAtMs"] = 1;
        checkpoint["expiresAtMs"] = 100000;
        checkpoint.erase("signature");
        checkpoint.erase("checkpointDigest");
        checkpoint["checkpointDigest"] = ndnsf::di::nativePlanningDigest(
          ndnsf::di::nativeConversationCanonicalJson(checkpoint));
        checkpoint["signature"] = std::string(64, 'a');
        write(turn.at("checkpoint_output_file").get<std::string>(), checkpoint.dump());
        turns.push_back(turn);
      }
      requesterConfig["turns"] = turns;
      requesterConfig["request"]["generation_mode"] = "TOKEN_STREAMING";
      requesterConfig["fixed_placement"]["role_providers"] = {
        {"role0", "/example/ndnsf-qwen06b/provider-0"},
        {"role1", "/example/ndnsf-qwen06b/provider-1"}};
      const auto requesterStatisticsPath = root /
        (explicitStatistics ? "custom-requester-statistics.jsonl" : requesterId + "-statistics.jsonl");
      if (explicitStatistics)
        requesterConfig["statistics_file"] = requesterStatisticsPath.string();
      const auto requesterIdentity = requesterConfig.at("conversation").at("owner").at("requester_identity");
      NativeJson config{{"deployment_id", "fixture"}, {"requester", requesterConfig},
        {"platform", {{"kind", "host"}}}, {"nodes", NativeJson::array({{{"id", "local"}}})},
        {"actors", NativeJson::array({
          {{"id", requesterId}, {"kind", "requester"}, {"identity", requesterIdentity}},
          {{"id", "provider-0"}, {"kind", "provider"},
            {"identity", "/example/ndnsf-qwen06b/provider-0"}},
          {{"id", "provider-1"}, {"kind", "provider"},
            {"identity", "/example/ndnsf-qwen06b/provider-1"}}})}};
      if (explicitStatistics) {
        std::filesystem::copy_file(root / "requester-statistics.jsonl", requesterStatisticsPath,
          std::filesystem::copy_options::overwrite_existing);
        for (unsigned p = 0; p < 2; ++p) {
          const auto name = "provider-" + std::to_string(p) + "-statistics.jsonl";
          const auto custom = root / ("custom-" + name);
          std::filesystem::copy_file(root / name, custom,
            std::filesystem::copy_options::overwrite_existing);
          config["actors"][p + 1]["parameters"]["statistics-file"] = custom.string();
        }
      }
      else if (requesterId != "requester") {
        std::filesystem::copy_file(root / "requester-statistics.jsonl", requesterStatisticsPath,
          std::filesystem::copy_options::overwrite_existing);
      }
      const auto digest = ndnsf::di::nativePlanningDigest(
        ndnsf::di::nativeCanonicalJson(config)).substr(7);
      write(root / "configuration.json", config.dump());
      requesterConfig["statistics_file"] = requesterStatisticsPath.string();
      write(root / (requesterId + ".json"), requesterConfig.dump());
      NativeJson state{{"schema", "ndnsf-di-host-operation-v1"}, {"deployment_id", "fixture"},
        {"boot_id", "fixture-boot"}, {"configuration_sha256", digest},
        {"evidence_root", root.string()}, {"status", "STOPPED"}, {"needs_closeout", false},
        {"external_references", NativeJson::array()},
        {"requester", {{"completion", "EXITED"}, {"exit_code", 0}}},
        {"cleanup", NativeJson::array()}, {"launches", NativeJson::array()}};
      for (const auto& actor : config.at("actors")) {
        const auto id = actor.at("id");
        NativeJson bindings{{"NDNSF_DEPLOYMENT_ACTOR_ID", id},
          {"NDNSF_DEPLOYMENT_BOOT_ID", "fixture-boot"},
          {"NDNSF_DEPLOYMENT_CONFIG_SHA256", digest}};
        state["launches"].push_back({{"id", id}, {"bindings", bindings},
          {"proof", {{"bindings", bindings}}}});
        state["cleanup"].push_back({{"id", id}, {"status", "ZERO"}, {"forced", false}});
      }
      write(root / "deployment-state.json", state.dump());
    };
    const std::vector<std::string> deployment{
      "--deployment-state", (root / "deployment-state.json").string(), "--require-multi-token"};
    deploymentFixture();
    check("", "", "", deployment);
    deploymentFixture(true);
    check("", "", "", deployment);
    deploymentFixture(false, "client");
    check("", "", "", deployment);
    const auto mutateDeployment = [&](const std::string& name, const std::string& reason,
                                      const std::function<void(NativeJson&)>& change) {
      deploymentFixture();
      auto value = readJsonFile(root / name);
      change(value);
      write(root / name, value.dump());
      check("", "", reason, deployment);
    };
    mutateDeployment("deployment-state.json", "deployment-cleanup", [](NativeJson& j) {
      j["cleanup"][0]["status"] = "UNKNOWN";
    });
    mutateDeployment("deployment-state.json", "deployment-cleanup", [](NativeJson& j) {
      j["cleanup"][0]["forced"] = true;
    });
    mutateDeployment("deployment-state.json", "deployment-requester-exit", [](NativeJson& j) {
      j["requester"]["exit_code"] = 1;
    });
    mutateDeployment("deployment-state.json", "deployment-process-binding", [](NativeJson& j) {
      j["launches"][0]["proof"]["bindings"]["NDNSF_DEPLOYMENT_BOOT_ID"] = "old";
    });
    mutateDeployment("configuration.json", "deployment-state-or-configuration", [](NativeJson& j) {
      j["deployment_id"] = "other";
    });
    mutateDeployment("requester/conversation-state-1.json", "deployment-checkpoint-binding", [](NativeJson& j) {
      j["contextEpoch"] = 1;
    });
    mutateDeployment("requester/conversation-state-1.json", "deployment-checkpoint-binding", [](NativeJson& j) {
      j["logicalPrefixDigest"] = "wrong";
    });
    mutateDeployment("requester/conversation-state-1.json", "deployment-checkpoint-digest", [](NativeJson& j) {
      j["checkpointDigest"] = "wrong";
    });
    deploymentFixture();
    for (const auto* name : {"conversation-state.json", "conversation-state-1.json"}) {
      auto checkpoint = readJsonFile(root / "requester" / name);
      checkpoint["planRoleMapDigest"] = "sha256:" + std::string(64, 'd');
      checkpoint.erase("checkpointDigest");
      checkpoint.erase("signature");
      checkpoint["checkpointDigest"] = ndnsf::di::nativePlanningDigest(
        ndnsf::di::nativeConversationCanonicalJson(checkpoint));
      checkpoint["signature"] = std::string(64, 'a');
      write(root / "requester" / name, checkpoint.dump());
    }
    check("", "", "deployment-checkpoint-binding", deployment);
    deploymentFixture();
    write(root / "requester-statistics.jsonl", "");
    check("", "", "statistics-request-count", deployment);
    deploymentFixture();
    std::ifstream statistics(root / "requester-statistics.jsonl");
    std::string firstRow, secondRow;
    std::getline(statistics, firstRow);
    std::getline(statistics, secondRow);
    auto incomplete = NativeJson::parse(secondRow);
    incomplete["observedEvents"] = 2;
    statistics.close();
    write(root / "requester-statistics.jsonl", firstRow + '\n' + incomplete.dump() + '\n');
    check("", "", "stream-statistics-mismatch", deployment);

    // Original receipt/parser fixtures only. These records do not claim that
    // this host has Slurm, a container, or a CUDA device.
    const auto privateJson = [&](const std::filesystem::path& path, const NativeJson& value) {
      std::filesystem::create_directories(path.parent_path());
      write(path, value.dump());
      std::filesystem::permissions(path, std::filesystem::perms::owner_read |
                                       std::filesystem::perms::owner_write);
    };
    const auto slurmFixture = [&](bool gpu = true,
                                  NativeJson selectors = NativeJson::array({"3"})) {
      deploymentFixture();
      auto config = readJsonFile(root / "configuration.json");
      config["platform"] = {{"kind", "slurm-apptainer"},
        {"build_record", (root / "build-record.json").string()},
        {"sif_sha256", "sha256:" + std::string(64, 'a')},
        {"slurm", {{"gpu", {{"count", gpu ? 1 : 0}}}}}};
      config["release"] = {{"prefix", (root / "installed").string()},
        {"manifest", (root / "release.json").string()}};
      privateJson(root / "release.json", {{"fixture", "native-ledger"}});
      std::ifstream manifestInput(root / "release.json");
      std::ostringstream manifestBytes;
      manifestBytes << manifestInput.rdbuf();
      const auto release = ndnsf::di::nativePlanningDigest(manifestBytes.str()).substr(7);
      NativeJson record{{"schemaVersion", "ndnsf-local-sif-build-v3"},
        {"status", "BUILT_UNQUALIFIED"},
        {"sif", {{"sha256", config["platform"]["sif_sha256"]}, {"bytes", 1}}}};
      record["recordDigest"] = ndnsf::di::nativePlanningDigest(ndnsf::di::nativeCanonicalJson(record));
      privateJson(root / "build-record.json", record);
      config["nodes"] = NativeJson::array({
        {{"id", "a"}, {"host", "node-a"}, {"nfd", {{"mode", "managed"}}}},
        {{"id", "b"}, {"host", "node-b"}, {"nfd", {{"mode", "managed"}}}}});
      for (auto& actor : config["actors"]) {
        actor["external"] = false;
        actor["node"] = actor["id"] == "provider-1" ? "b" : "a";
        actor["resources"] = {{"gpu_devices", gpu && actor["kind"] == "provider"
          ? selectors : NativeJson::array()}};
      }
      const auto digest = ndnsf::di::nativePlanningDigest(ndnsf::di::nativeCanonicalJson(config)).substr(7);
      privateJson(root / "configuration.json", config);
      const auto binding = NativeJson{{"boot_id", "fixture-boot"}, {"configuration_sha256", digest},
        {"release_sha256", release}, {"job_id", "12345"}};
      unsigned serial = 100;
      const auto groups = [&](const std::string& id) {
        const auto pid = ++serial;
        NativeJson env{{"NDNSF_DEPLOYMENT_ACTOR_ID", id},
          {"NDNSF_DEPLOYMENT_BOOT_ID", "fixture-boot"},
          {"NDNSF_DEPLOYMENT_CONFIG_SHA256", digest}};
        return NativeJson{{"launches", NativeJson::array({{
          {"id", id}, {"bindings", env}, {"proof", {{"pid", pid}, {"pgid", pid},
            {"uid", ::getuid()}, {"start_ticks", pid + 1000}, {"exe", "/controlled/native"},
            {"bindings", env}}}}})},
          {"cleanup", NativeJson::array({{{"id", id}, {"status", "ZERO"}, {"forced", false}}})}};
      };
      auto allocation = binding;
      allocation.update(groups("supervisor-step"));
      allocation.update({{"schema", "ndnsf-di-host-operation-v1"}, {"deployment_id", "fixture"},
        {"status", "STOPPED"}, {"execution", "COMPLETE"}, {"failure", nullptr},
        {"evidence_root", (root / "supervisor/fixture").string()},
        {"external_references", NativeJson::array()}, {"roles", NativeJson::array()},
        {"gpu_allocations", NativeJson::object()}});
      std::map<std::string, NativeJson> pools;
      for (const auto& node : config["nodes"]) {
        const auto id = node["id"].get<std::string>();
        for (const auto* kind : {"nfd", "nfdc"})
          allocation["roles"].push_back({{"id", std::string(kind) + "." + id},
            {"kind", kind}, {"node", id}});
        auto audit = binding;
        audit.update({{"schema", "ndnsf-di-host-node-audit-v1"}, {"node_id", id},
          {"host", node["host"]}, {"status", "ZERO"}, {"cleanup_scope", "HOST_UID_JOB"},
          {"processes", NativeJson::array()}, {"errors", NativeJson::array()},
          {"auditor_cgroup", "0::/slurm/job_12345/step_99"}, {"auditor_step_id", "99"}});
        privateJson(root / "nodes" / id / "host-audit-final.json", audit);
        if (!gpu) continue;
        const auto uuid = "GPU-" + std::string(8, id == "a" ? '1' : '2') + "-1111-1111-1111-111111111111";
        auto pool = binding;
        pool.update({{"schema", "ndnsf-di-gpu-allocation-v1"}, {"node_id", id},
          {"host", node["host"]}, {"visibility", "0"},
          {"devices", NativeJson::array({{{"physical_id", "3"}, {"uuid", uuid}}})}});
        auto probe = binding;
        probe.update({{"schema", "ndnsf-di-gpu-probe-v1"}, {"event", "NDNSF_DI_GPU_PROBE"},
          {"role_id", "gpu-probe." + id}, {"step_id", "3"}, {"step_gpus", "9"},
          {"host", node["host"]}, {"visibility", "0"}, {"device_order", "PCI_BUS_ID"},
          {"visible_uuids", NativeJson::array({uuid})}, {"inventory", pool["devices"]}});
        pool["native_probe"] = probe;
        pools[id] = pool;
        allocation["gpu_allocations"][id] = pool;
        privateJson(root / "nodes" / id / "gpu-allocation.json", pool);
      }
      for (const auto& actor : config["actors"]) allocation["roles"].push_back(actor);
      std::map<std::string, NativeJson> receipts;
      for (const auto& role : allocation["roles"]) {
        const auto id = role["id"].get<std::string>();
        const auto node = role["node"].get<std::string>();
        const auto kind = role["kind"].get<std::string>();
        auto receipt = binding;
        receipt.update(groups(id));
        const auto proof = receipt["launches"][0]["proof"];
        receipt.update({{"schema", "ndnsf-di-node-role-v1"}, {"role_id", id},
          {"role_kind", kind}, {"node_id", node}, {"host", "node-" + node},
          {"status", "STOPPED"}, {"cleanup_scope", "PID_NAMESPACE"}, {"failure", nullptr},
          {"execution", kind == "requester" || kind == "nfdc" ? "COMPLETE" : "SERVICE_STOPPED"},
          {"exit_code", 0}, {"step_id", "3"}, {"pid_namespace", "pid:[" + id + "]"}});
        if (gpu && kind == "provider") {
          auto admission = pools.at(node);
          admission["native_probe"]["role_id"] = "management-0";
          receipt["gpu_admission"] = admission;
          receipt["gpu_visibility"] = "0";
          auto query = groups("management-0");
          query["launches"][0]["argv"] = NativeJson::array({
            (root / "installed/libexec/ndnsf-di/ndnsf-di-device-query").string(), "--count", "1"});
          query["launches"][0]["exit_code"] = 0;
          receipt["launches"].push_back(query["launches"][0]);
          receipt["cleanup"].push_back(query["cleanup"][0]);
        }
        receipts[id] = receipt;
        const auto path = kind == "nfd" || kind == "nfdc"
          ? root / "nodes" / node / kind / "receipt.json" : root / "actors" / id / "receipt.json";
        privateJson(path, receipt);
        auto observed = binding;
        auto hostProof = proof;
        hostProof["pid"] = proof["pid"].get<unsigned>() + 10000;
        hostProof["nspid"] = NativeJson::array({hostProof["pid"], proof["pid"]});
        hostProof["pid_namespace"] = receipt["pid_namespace"];
        hostProof["cgroup"] = "0::/slurm/job_12345/step_3";
        observed.update({{"schema", "ndnsf-di-host-node-audit-v1"}, {"node_id", node},
          {"role_id", id}, {"host", "node-" + node}, {"status", "OBSERVED"},
          {"native_process", hostProof}});
        privateJson(root / "nodes" / node / ("host-audit-" + id + ".json"), observed);
      }
      const auto requesterRoot = root / "actors/requester/runtime/fixture";
      std::filesystem::create_directories(requesterRoot);
      auto requesterConfig = readJsonFile(root / "requester.json");
      requesterConfig["statistics_file"] = (requesterRoot / "requester-statistics.jsonl").string();
      privateJson(requesterRoot / "requester.json", requesterConfig);
      std::filesystem::copy_file(root / "requester-statistics.jsonl", requesterRoot / "requester-statistics.jsonl",
        std::filesystem::copy_options::overwrite_existing);
      for (unsigned providerIndex = 0; providerIndex != 2; ++providerIndex) {
        const auto id = "provider-" + std::to_string(providerIndex);
        const auto output = root / "actors" / id / "runtime/fixture" / (id + "-statistics.jsonl");
        std::filesystem::create_directories(output.parent_path());
        std::ifstream input(root / (id + "-statistics.jsonl"));
        std::string line, records, firstRequest;
        std::uint64_t firstAttempt = 0;
        while (std::getline(input, line)) {
          auto row = NativeJson::parse(line);
          if (row["type"] == "request") {
            if (firstRequest.empty()) {
              firstRequest = row["requestId"].get<std::string>();
              firstAttempt = row["attemptEpoch"].get<std::uint64_t>();
            }
            // The second host deliberately has a different monotonic origin.
            if (providerIndex == 1) {
              for (const auto* key : {"startedSteadyNs", "finishedSteadyNs"})
                row[key] = row[key].get<std::int64_t>() + 1000000000;
              for (auto& item : row["runtimeMetrics"])
                for (const auto* key : {"startedSteadyNs", "finishedSteadyNs"})
                  item["lastBackendExecution"][key] =
                    item["lastBackendExecution"][key].get<std::int64_t>() + 1000000000;
            }
            if (gpu) {
              row["admittedModelDigest"] = "sha256:model";
              row["admittedArtifactDigest"] = "sha256:artifact";
              ndnsf::di::ExecutionEvidence evidence;
              evidence.providerName = row["provider"];
              evidence.providerBootId = row["providerBootId"];
              evidence.runnerKind = ndnsf::di::RunnerKind::OnnxRuntimeCuda;
              evidence.realCompute = true; evidence.deviceKind = "cuda"; evidence.deviceId = "0";
              evidence.runtimeVersion = "fixture-ort"; evidence.createdAtMs = 1;
              evidence.modelDigest = "sha256:model"; evidence.planDigest = row["planDigest"];
              evidence.roles = {row["role"].get<std::string>()};
              evidence.artifactDigests[evidence.roles.front()] = "sha256:artifact";
              evidence.loadCompleted = true; evidence.warmupCompleted = true;
              evidence.gpuUuid = receipts.at(id)["gpu_admission"]["devices"][0]["uuid"];
              evidence.gpuUuids = {evidence.gpuUuid}; evidence.cudaVisibleDevices = "0";
              evidence.gpuIdentitySource = "cuda-runtime-pci+driver-uuid";
              evidence.processId = receipts.at(id)["launches"][0]["proof"]["pid"];
              evidence.requestId = row["requestId"]; evidence.attemptEpoch = row["attemptEpoch"];
              evidence.executionCompleted = true; evidence.providerProfilePath = "/controlled/profile";
              evidence.profileRequestId = firstRequest; evidence.profileAttemptEpoch = firstAttempt;
              evidence.setNodeProviderAssignments({{evidence.roles.front(), "projection_kernel_time",
                                                    "CUDAExecutionProvider", true}});
              row["finalExecutionEvidence"] = NativeJson::parse(ndnsf::di::executionEvidenceToJson(evidence));
            }
          }
          records += row.dump() + '\n';
        }
        write(output, records);
      }
      NativeJson reference{{"boot_id", "fixture-boot"}, {"configuration_sha256", digest},
        {"release_sha256", release}, {"job_id", "12345"}, {"owner_uid", ::getuid()}};
      auto operation = binding;
      operation.update({{"schema", "ndnsf-di-slurm-operation-v1"}, {"deployment_id", "fixture"},
        {"evidence_root", root.string()}, {"job_reference", reference}, {"scheduler", {
          {"reference", reference}, {"terminal", true}, {"status", "TERMINAL"},
          {"state", "COMPLETED"}, {"exit_code", "0:0"}}}});
      privateJson(root / "operation.json", operation);
      privateJson(root / "allocation.json", allocation);
    };
    const std::vector<std::string> slurmGpu{
      "--deployment-state", (root / "allocation.json").string(),
      "--require-multi-token", "--require-distributed-gpu"};
    slurmFixture();
    check("", "", "", slurmGpu);
    slurmFixture(true, NativeJson::array({"allocated:0"}));
    check("", "", "", slurmGpu);
    for (const auto* selector : {"allocated:1", "allocated:64", "allocated:00",
                                  "allocated:-1", "allocated:", "allocated:0x0"}) {
      slurmFixture(true, NativeJson::array({selector}));
      check("", "", "deployment-gpu-selector", slurmGpu);
    }
    slurmFixture(true, NativeJson::array({"allocated:0", "3"}));
    check("", "", "deployment-gpu-selector", slurmGpu);
    slurmFixture(false);
    check("", "", "", {"--deployment-state", (root / "allocation.json").string(), "--require-multi-token"});
    check("", "", "deployment-distributed-gpu-required", slurmGpu);
    const auto badSlurm = [&](const std::string& path, const std::string& reason,
                              const std::function<void(NativeJson&)>& mutate) {
      slurmFixture();
      auto value = readJsonFile(root / path);
      mutate(value); privateJson(root / path, value);
      check("", "", reason, slurmGpu);
    };
    badSlurm("operation.json", "deployment-scheduler-binding", [](auto& j) { j["job_reference"]["job_id"] = "999"; });
    badSlurm("build-record.json", "deployment-image-binding", [](auto& j) { j["sif"]["sha256"] = "wrong"; });
    badSlurm("allocation.json", "deployment-role-coverage", [](auto& j) { j["roles"].erase(0); });
    badSlurm("nodes/b/host-audit-final.json", "deployment-node-cleanup", [](auto& j) { j["processes"] = {1}; });
    badSlurm("nodes/b/host-audit-final.json", "deployment-node-binding", [](auto& j) { j["boot_id"] = "wrong"; });
    badSlurm("actors/provider-1/receipt.json", "deployment-cleanup", [](auto& j) { j["cleanup"][0]["forced"] = true; });
    badSlurm("actors/provider-1/receipt.json", "deployment-role-terminal", [](auto& j) { j["execution"] = "FAILED"; });
    badSlurm("nodes/b/host-audit-provider-1.json", "deployment-native-host-proof", [](auto& j) {
      j["native_process"]["pid_namespace"] = "wrong";
    });
    badSlurm("actors/provider-1/receipt.json", "deployment-gpu-probe-binding", [](auto& j) {
      j["gpu_admission"]["native_probe"]["job_id"] = "wrong";
    });
    const auto badGpuStatistics = [&](const std::string& reason, const std::function<void(NativeJson&)>& mutate) {
      slurmFixture();
      const auto path = root / "actors/provider-1/runtime/fixture/provider-1-statistics.jsonl";
      std::ifstream input(path); std::string line, records;
      bool changed = false;
      while (std::getline(input, line)) {
        auto value = NativeJson::parse(line);
        if (!changed && value["type"] == "request") { mutate(value); changed = true; }
        records += value.dump() + '\n';
      }
      input.close(); write(path, records);
      check("", "", reason, slurmGpu);
    };
    badGpuStatistics("deployment-gpu-execution-missing", [](auto& j) { j.erase("finalExecutionEvidence"); });
    for (const auto* key : {"gpuUuid", "requestId", "providerBootId", "planDigest"})
      badGpuStatistics("deployment-gpu-execution-binding", [=](auto& j) { j["finalExecutionEvidence"][key] = "wrong"; });
    badGpuStatistics("deployment-gpu-execution-binding", [](auto& j) {
      j["finalExecutionEvidence"]["cpuFallbackUsed"] = "true";
    });
    badGpuStatistics("deployment-gpu-kernel-proof", [](auto& j) {
      j["finalExecutionEvidence"]["profileRequestId"] = "unbound-profile";
    });
    badGpuStatistics("deployment-gpu-kernel-provider", [](auto& j) {
      j["finalExecutionEvidence"]["nodeProviderAssignments"][0]["provider"] = "CPUExecutionProvider";
    });
    for (bool wrongModel : {false, true}) {
      slurmFixture();
      const auto path = root / "actors/provider-1/runtime/fixture/provider-1-statistics.jsonl";
      std::ifstream input(path); std::string line, records;
      while (std::getline(input, line)) {
        auto value = NativeJson::parse(line);
        if (value["type"] == "request") {
          if (wrongModel) value["finalExecutionEvidence"]["modelDigest"] = "sha256:consistently-wrong";
          else value["finalExecutionEvidence"]["artifactDigests"]["role1"] = "sha256:consistently-wrong";
        }
        records += value.dump() + '\n';
      }
      input.close(); write(path, records);
      check("", "", "deployment-gpu-model-binding", slurmGpu);
    }
    slurmFixture();
    std::filesystem::remove(root / "nodes/b/host-audit-final.json");
    check("", "", "deployment-private-evidence", slurmGpu);
    std::cout << "Spec189 CLI oracle: " << cases << " cases passed\n";
    return 0;
  }
  catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
