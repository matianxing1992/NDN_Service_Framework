#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeGenerationLimits.hpp"

#include <filesystem>
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
                     bool cacheHit = false)
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
    text += "NDNSF_DI_PROVIDER_PREPARATION phase=CACHE_LOOKUP_HIT" + identity +
      " detail=recipe-addressed\n";
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
        std::vector<std::string> arguments{argv[1], "--run-root", directory};
        arguments.insert(arguments.end(), options.begin(), options.end());
        std::vector<char*> pointers;
        for (auto& argument : arguments) pointers.push_back(argument.data());
        pointers.push_back(nullptr);
        execv(argv[1], pointers.data());
        _exit(127);
      }
      int status = 0;
      if (waitpid(child, &status, 0) != child || !WIFEXITED(status))
        throw std::runtime_error("oracle did not exit normally");
      std::ifstream in(output); std::ostringstream content; content << in.rdbuf();
      const auto text = content.str();
      const bool cache = std::find(options.begin(), options.end(), "--cache-compatibility") != options.end();
      if (cache && (text.find("SPEC189_CPP_ORACLE_PASS") != std::string::npos ||
                    text.find("SPEC189_CPP_PLACEMENT_PASS") != std::string::npos))
        throw std::runtime_error("cache diagnostic emitted qualification PASS: " + text);
      if (reason.empty()) {
        const auto marker = cache ? "SPEC189_CPP_CACHE_DIAGNOSTIC_PASS" : "SPEC189_CPP_ORACLE_PASS";
        if (WEXITSTATUS(status) != 0 || text.find(marker) == std::string::npos)
          throw std::runtime_error("positive CLI fixture rejected: " + text);
        if (cache && (text.find("\"scope\":\"cache-compatible-execution\"") == std::string::npos ||
                      text.find("\"materialFetch\":\"NOT_EVALUATED\"") == std::string::npos ||
                      text.find("\"fullPathQualification\":\"NOT_RUN\"") == std::string::npos))
          throw std::runtime_error("cache diagnostic scope missing: " + text);
      }
      else if (WEXITSTATUS(status) != expectedFailure ||
               text.find(expectedFailure == 2 ? "usage:" : "reason=" + reason) == std::string::npos ||
               text.find("SPEC189_CPP_ORACLE_PASS") != std::string::npos ||
               text.find("SPEC189_CPP_CACHE_DIAGNOSTIC_PASS") != std::string::npos) {
        throw std::runtime_error("negative CLI fixture mismatch: " + text);
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
                           bool residentCache = false) {
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
                                           residentCache && round != 0), round);
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
    logs = chain(3, true, 1024, true);
    logs = {replaceAll(logs.first, "phase=CACHE_LOOKUP_HIT", "phase=CACHE_LOOKUP_MISSING"), logs.second};
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
    std::cout << "Spec189 CLI oracle: " << cases << " cases passed\n";
    return 0;
  }
  catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
