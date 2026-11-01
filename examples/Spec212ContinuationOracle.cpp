// Native assertions over the installed-process continuation campaign.
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "ndnsf-distributed-repo/RepoProtocol.hpp"
#include "ndn-service-framework/ProtectedBlobManifest.hpp"

#include <ndn-cxx/data.hpp>
#include <ndn-cxx/security/certificate.hpp>
#include <ndn-cxx/security/verification-helpers.hpp>
#include <ndn-cxx/util/io.hpp>
#include <ndn-cxx/util/sha256.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace {
using ndnsf::di::NativeJson;

std::string read(const std::filesystem::path& path)
{
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("missing campaign artifact: " + path.string());
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

NativeJson json(const std::filesystem::path& path)
{
  return ndnsf::di::nativeParseJson(read(path));
}

void require(bool value, const std::string& reason)
{
  if (!value) throw std::runtime_error(reason);
}

std::map<std::string, std::string> fields(const std::string& line)
{
  std::map<std::string, std::string> result;
  std::istringstream stream(line);
  for (std::string word; stream >> word;) {
    const auto equal = word.find('=');
    if (equal != std::string::npos) result.emplace(word.substr(0, equal), word.substr(equal + 1));
  }
  return result;
}

NativeJson tokens(const std::filesystem::path& path)
{
  const auto value = json(path);
  require(value.at("schema") == "NDNSF-DI-FINAL-V1", "unexpected output schema");
  const auto result = value.at("tokenIds");
  require(result.is_array() && !result.empty(), "empty generated output");
  for (const auto& item : result)
    require(item.is_number_integer() && item.get<std::int64_t>() >= 0, "invalid generated token");
  return result;
}

std::size_t verifyStoredGraph(const std::filesystem::path& root, const NativeJson& config,
                              const NativeJson& state)
{
  require(state.at("repo_original_data_served_before_event") == 0,
          "Repo served original KV Data before the handoff event");
  const auto expectedRepo = config.at("kv_handoff").at("roles").begin().value()
    .at("repo_provider_identity").get<std::string>();
  const auto recovery = json(root / "provider-1/recovery.json");
  require(recovery.at("repoForwardingHints") == NativeJson::array({expectedRepo}),
          "destination KV recovery configuration has a different Repo route");
  for (const auto& route : state.at("kv_recovery_forwarding_hints").items())
    require(route.value() == recovery.at("repoForwardingHints"),
            "configured KV recovery routes differ between Providers");
  require(state.at("destination_runner_reuse_hit") == true,
          "event did not reuse the destination runner prepared before migration");
  const auto& servedEvents = state.at("repo_original_data_served");
  require(servedEvents.is_array() && !servedEvents.empty() &&
          state.at("repo_original_data_served_count") == servedEvents.size(),
          "Repo original-Data service evidence is missing");
  std::map<std::string, std::string> servedPackets;
  for (const auto& event : servedEvents) {
    const auto name = event.at("name").get<std::string>();
    const auto digest = event.at("sha256").get<std::string>();
    require(!name.empty() && digest.size() == 64 &&
            digest.find_first_not_of("0123456789abcdef") == std::string::npos,
            "Repo service event has an invalid name or wire digest");
    const auto inserted = servedPackets.emplace(name, digest);
    require(inserted.second || inserted.first->second == digest,
            "Repo service events conflict for one original Data name");
  }
  const auto& policy = config.at("kv_handoff").at("roles").begin().value();
  const ndn::Name producer(policy.at("producer_identity").get<std::string>());
  const auto certificate = ndn::io::load<ndn::security::Certificate>(
    policy.at("producer_certificate").get<std::string>());
  require(certificate && certificate->getValidityPeriod().isValid() &&
          certificate->getIdentity() == producer, "Producer certificate mismatch");
  const auto store = root / "repo/repo.sqlite.store";
  std::size_t graphs = 0;
  std::uint64_t charged = 0;
  bool servedVerifiedOriginalPacket = false;
  for (const auto& entry : std::filesystem::directory_iterator(store / "manifests")) {
    if (entry.path().extension() != ".json") continue;
    const auto encoded = read(entry.path());
    const auto manifest = ndnsf_distributed_repo::parseManifestJson(encoded);
    if (manifest.objectType != "original-packet-set") continue;
    require(manifest.lifecycleState == "COMMITTED" && manifest.size > 0 && manifest.size <= (64ULL << 20),
            "packet graph is not bounded and committed");
    require(manifest.sha256.size() == 64 && manifest.sha256.find_first_not_of("0123456789abcdef") == std::string::npos,
            "invalid persisted wire digest");
    const auto wire = read(store / "payloads/sha256" / manifest.sha256.substr(0, 2) / manifest.sha256);
    ndn::util::Sha256 digest;
    digest.update(ndn::span<const uint8_t>(reinterpret_cast<const uint8_t*>(wire.data()), wire.size()));
    auto measuredDigest = digest.toString();
    std::transform(measuredDigest.begin(), measuredDigest.end(), measuredDigest.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    require(wire.size() == manifest.size && measuredDigest == manifest.sha256, "stored original wire digest mismatch");
    require(!manifest.packetNames.empty() && manifest.packetNames.size() <= 4096 &&
            manifest.packetOffsets.size() == manifest.packetNames.size() + 1 &&
            manifest.packetOffsets.front() == 0 && manifest.packetOffsets.back() == wire.size() &&
            manifest.segmentCount == manifest.packetNames.size(), "packet graph index incomplete");
    std::map<ndn::Name, ndn::Data> packets;
    for (std::size_t i = 0; i < manifest.packetNames.size(); ++i) {
      const auto begin = manifest.packetOffsets[i], end = manifest.packetOffsets[i + 1];
      require(begin < end && end <= wire.size(), "invalid original packet offset");
      const ndn::Block block(ndn::span<const uint8_t>(
        reinterpret_cast<const uint8_t*>(wire.data()) + begin, end - begin));
      const ndn::Data packet(block);
      require(block.size() == end - begin && packet.getName() == ndn::Name(manifest.packetNames[i]) &&
              producer.isPrefixOf(packet.getName()) && ndn::security::verifySignature(packet, *certificate),
              "stored packet lost its original Producer name/signature/wire");
      const auto served = servedPackets.find(packet.getName().toUri());
      if (served != servedPackets.end()) {
        ndn::util::Sha256 packetDigest;
        packetDigest.update(ndn::span<const uint8_t>(
          reinterpret_cast<const uint8_t*>(wire.data()) + begin, end - begin));
        auto packetSha256 = packetDigest.toString();
        std::transform(packetSha256.begin(), packetSha256.end(), packetSha256.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        require(packetSha256 == served->second,
                "Repo-served wire digest differs from the verified original packet graph");
        servedVerifiedOriginalPacket = true;
      }
      require(packets.emplace(packet.getName(), packet).second, "duplicate original packet");
    }
    const ndn::Name object(manifest.objectName);
    require(object == ndn::Name(manifest.packetNames.front()), "original root missing");
    const auto& content = packets.at(object).getContent();
    const auto transport = ndn_service_framework::ProtectedBlobManifest::decode(
      {content.value_begin(), content.value_end()}, producer, object);
    std::uint64_t envelope = 0;
    for (std::uint64_t i = 0; i < transport.segmentCount; ++i) {
      const auto name = ndn::Name(transport.dataPrefix).appendSegment(i);
      const auto found = packets.find(name);
      require(found != packets.end(), "referenced ciphertext segment is absent");
      envelope += found->second.getContent().value_size();
    }
    require(envelope == transport.envelopeBytes && envelope == manifest.envelopeBytes &&
            transport.dataPrefix == ndn::Name(manifest.dataPrefix) &&
            packets.size() > transport.segmentCount + 1, "ciphertext/key graph closure missing");
    bool claimed = false;
    for (const auto& database : state.at("owned_after_baseline").items()) {
      for (const auto& claim : database.value().at("repo_user_storage_claims").at("values")) {
        if (claim.at("state") != "Committed" || ndn::Name(claim.at("object_name").get<std::string>()) != object) continue;
        require(!claimed && ndn::Name(claim.at("owner_identity").get<std::string>()) ==
                  ndn::Name(config.at("core").at("requester_identity").get<std::string>()) &&
                claim.at("wire_sha256") == manifest.sha256 && claim.at("generation") == manifest.generation &&
                claim.at("payload_wire_bytes") == wire.size() && claim.at("manifest_bytes") == encoded.size(),
                "actual Repo wire/metadata does not match the authenticated Owner charge");
        claimed = true;
      }
      for (const auto& write : database.value().at("repo_user_storage_writes").at("values"))
        require(write.at("reserved_bytes") == 0 && write.at("state") != "Reserved", "Repo reservation leaked");
    }
    require(claimed, "original packet graph has no durable Owner claim");
    charged += wire.size() + encoded.size();
    require(charged <= 1000000000ULL, "Owner quota exceeded");
    ++graphs;
  }
  require(graphs == config.at("kv_handoff").at("roles").size(),
          "stored packet graph count differs from the complete role set");
  require(servedVerifiedOriginalPacket,
          "Repo service event does not match a verified original Producer packet wire");
  return graphs;
}
} // namespace

int main(int argc, char** argv)
{
  try {
    require(argc == 2, "usage: spec212-continuation-oracle RUN_ROOT");
    const auto root = std::filesystem::path(argv[1]);
    const auto config = json(root / "requester/config.json");
    const auto state = json(root / "campaign-state.json");
    require(state.at("source_stopped_before_append") == true, "source was not stopped before APPEND");
    require(state.at("source_exit") == 0, "source did not close cleanly");
    require(state.at("destination_exit") == 0 && state.at("checkpoint_scratch_files").empty(),
            "native cleanup did not release checkpoint scratch");
    const auto& before = state.at("owned_before_baseline");
    require(before.is_object() && !before.empty(), "durable Owner journal evidence missing");
    std::uint64_t claims = 0;
    for (const auto& database : before.items())
      claims += database.value().at("repo_user_storage_claims").at("rows").get<std::uint64_t>();
    require(claims > 0, "handoff made no durable Owner claim");
    require(before == state.at("owned_after_baseline"),
            "ordinary uninterrupted conversation changed owned KV claims/writes");
    require(before == state.at("owned_after_close"), "ordinary Provider close changed owned KV claims/writes");
    const auto graphs = verifyStoredGraph(root, config, state);
    const auto requester = read(root / "logs/requester.log");
    require(requester.find("NATIVE_CONVERSATION_TURNS_SUCCEEDED count=2") != std::string::npos,
            "migration conversation did not finish");
    const bool departure = config.at("kv_handoff").at("wait_for_departure").get<bool>();
    if (departure) {
      const auto source = read(root / "logs/provider-0.log");
      const auto scope = state.at("withdrawal_scope").get<std::string>();
      require(source.find("scope=" + scope + " closed=1 forced=0") != std::string::npos,
              "voluntary withdrawal did not save and close its requested scope");
    }
    require(requester.find(departure ? "NATIVE_KV_DEPARTURE_SAVED" : "NATIVE_KV_HANDOFF_APPEND_SUCCEEDED")
            != std::string::npos, "event completion missing");
    std::string appendRequest;
    std::istringstream requestLines(requester);
    for (std::string line; std::getline(requestLines, line);) {
      if (line.find("NATIVE_CONVERSATION_TURN_START ") == std::string::npos) continue;
      const auto record = fields(line);
      if (record.at("index") == "1") appendRequest = record.at("request");
    }
    require(!appendRequest.empty(), "APPEND request identity missing");
    const auto delta = json(root / "campaign-state.json").at("delta_tokens").get<std::uint64_t>();
    const auto parentCheckpoint = json(root / "requester/checkpoint-0.json");
    const auto expectedRole = config.at("kv_handoff").at("roles").begin().key();
    bool parentRestored = false;
    std::uint64_t prefill = 0, decode = 0;
    std::istringstream providerLines(read(root / "logs/provider-1.log"));
    for (std::string line; std::getline(providerLines, line);) {
      if (line.find("NDNSF_DI_CONVERSATION_KV_RESTORED ") != std::string::npos) {
        const auto record = fields(line);
        if (record.at("requestId") == appendRequest) {
          require(record.at("role") == expectedRole &&
                  std::stoull(record.at("prefixTokenCount")) ==
                    parentCheckpoint.at("prefixTokenCount").get<std::uint64_t>() &&
                  std::stoull(record.at("parentContextEpoch")) ==
                    parentCheckpoint.at("contextEpoch").get<std::uint64_t>(),
                  "destination restored a different APPEND parent role or boundary");
          parentRestored = true;
        }
      }
      if (line.find("certifiedTokenWork") == std::string::npos) continue;
      const auto record = fields(line);
      if (record.at("requestId") != appendRequest) continue;
      require(record.at("executionRole") == config.at("kv_handoff").at("roles").begin().key(),
              "actual work role mismatch");
      const auto count = std::stoull(record.at("tokens"));
      require(count <= 1000000, "actual work count exceeds campaign bound");
      if (record.at("transition") == "PREFILL") prefill += count;
      else if (record.at("transition") == "DECODE") decode += count;
    }
    require(parentRestored && delta > 0 && prefill == delta && decode > 0,
            "destination did not restore its parent and execute exactly the APPEND delta and decode");
    std::string baselineRequest, baselineAppendRequest;
    const auto baselineLog = read(root / "logs/baseline-requester.log");
    require(baselineLog.find("NATIVE_CONVERSATION_TURNS_SUCCEEDED count=2") != std::string::npos,
            "independent no-event history conversation did not finish");
    std::istringstream baselineLines(baselineLog);
    for (std::string line; std::getline(baselineLines, line);) {
      if (line.find("NATIVE_CONVERSATION_TURN_START ") == std::string::npos) continue;
      const auto record = fields(line);
      if (record.at("index") == "0") {
        require(baselineRequest.empty(), "duplicate initial baseline request");
        baselineRequest = record.at("request");
      }
      else if (record.at("index") == "1") {
        require(baselineAppendRequest.empty(), "duplicate APPEND baseline request");
        baselineAppendRequest = record.at("request");
      }
    }
    require(!baselineRequest.empty() && !baselineAppendRequest.empty() &&
            baselineRequest != appendRequest && baselineAppendRequest != appendRequest &&
            baselineRequest != baselineAppendRequest, "independent reference request identities missing");
    require(state.at("reference_execution") == "independent-same-request-and-decode-boundaries",
            "reference execution boundaries differ from the frozen contract");
    std::uint64_t baselinePrefill = 0, baselineDelta = 0, baselineDecode = 0;
    std::istringstream baselineWork(read(root / "logs/provider-1.log"));
    for (std::string line; std::getline(baselineWork, line);) {
      if (line.find("certifiedTokenWork") == std::string::npos) continue;
      const auto record = fields(line);
      if (record.at("requestId") == baselineRequest && record.at("transition") == "PREFILL")
        baselinePrefill += std::stoull(record.at("tokens"));
      if (record.at("requestId") == baselineAppendRequest) {
        const auto count = std::stoull(record.at("tokens"));
        require(count <= 1000000, "reference work count exceeds campaign bound");
        if (record.at("transition") == "PREFILL") baselineDelta += count;
        else if (record.at("transition") == "DECODE") baselineDecode += count;
      }
    }
    require(baselinePrefill == state.at("reference_initial_tokens").get<std::uint64_t>() &&
            baselinePrefill > delta && baselineDelta == delta && baselineDecode > 0,
            "independent reference did not execute the original prompt and APPEND delta");
    const auto initialTokens = config.at("conversation").at("turn").at("canonical_token_ids").size();
    const auto parentTokens = initialTokens + tokens(root / "requester/output-0.json").size();
    require(initialTokens == state.at("reference_initial_tokens").get<std::uint64_t>() &&
            parentTokens == state.at("reference_context_tokens_before_append").get<std::uint64_t>() &&
            parentTokens + delta == state.at("reference_context_tokens_after_delta").get<std::uint64_t>(),
            "reference history lengths differ from the initial prompt and generated parent");
    for (unsigned turn = 0; turn < 2; ++turn) {
      const auto suffix = std::to_string(turn) + ".json";
      require(tokens(root / ("requester/output-" + suffix)) ==
                tokens(root / ("requester/baseline-" + suffix)),
              "generated tokens differ from independent no-event native history");
      const auto actual = json(root / ("requester/checkpoint-" + suffix));
      const auto reference = json(root / ("requester/baseline-checkpoint-" + suffix));
      const auto expectedTokens = turn == 0 ? parentTokens :
        parentTokens + delta + tokens(root / ("requester/output-" + suffix)).size();
      require(actual.at("conversationId") != reference.at("conversationId") &&
              actual.at("logicalPrefixDigest") == reference.at("logicalPrefixDigest") &&
              actual.at("prefixTokenCount") == reference.at("prefixTokenCount") &&
              actual.at("contextEpoch") == reference.at("contextEpoch") &&
              actual.at("contextEpoch") == turn + 1 &&
              actual.at("prefixTokenCount") == expectedTokens,
              "independent reference checkpoint history or boundary differs");
    }
    std::cout << "SPEC212_NATIVE_CONTINUATION_PASS event=" << (departure ? "departure" : "switch")
              << " prefillTokens=" << prefill << " decodeTokens=" << decode
              << " baselinePrefillTokens=" << baselinePrefill
              << " baselineAppendPrefillTokens=" << baselineDelta
              << " sourceStopped=1 baselineEqual=1 noEventUpload=0 repoGraphs=" << graphs << '\n';
    return 0;
  }
  catch (const std::exception& error) {
    std::cerr << "SPEC212_NATIVE_CONTINUATION_FAIL reason=" << error.what() << '\n';
    return 1;
  }
}
