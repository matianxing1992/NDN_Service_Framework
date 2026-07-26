/* -*- Mode: C++; c-file-style: "gnu"; indent-tabs-mode:nil -*- */
#include "svsync.hpp"
#include "svspubsub.hpp"

#include <ndn-cxx/security/verification-helpers.hpp>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <openssl/sha.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

namespace {

using namespace ndn;
using namespace ndn::svs;

std::string
argument(int argc, char** argv, const std::string& name, const std::string& fallback = "")
{
  for (int i = 1; i + 1 < argc; ++i) {
    if (argv[i] == name) {
      return argv[i + 1];
    }
  }
  return fallback;
}

uint64_t
nowNs()
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string
escapeJson(const std::string& input)
{
  std::ostringstream output;
  for (unsigned char c : input) {
    switch (c) {
      case '\\': output << "\\\\"; break;
      case '"': output << "\\\""; break;
      case '\b': output << "\\b"; break;
      case '\f': output << "\\f"; break;
      case '\n': output << "\\n"; break;
      case '\r': output << "\\r"; break;
      case '\t': output << "\\t"; break;
      default:
        if (c < 0x20) {
          output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                 << static_cast<unsigned>(c) << std::dec;
        }
        else {
          output << c;
        }
    }
  }
  return output.str();
}

void
writeEvent(std::ofstream& output, const std::string& event, const std::string& version,
           const std::string& node, BootstrapTime boot = 0, SeqNo low = 0, SeqNo high = 0,
           const std::string& reason = "")
{
  output << "{\"event\":\"" << event << "\",\"implementation\":\"cpp\""
         << ",\"protocolVersion\":" << (version == "v3" ? 3 : 2)
         << ",\"nodeName\":\"" << escapeJson(node) << "\",\"bootstrapTime\":" << boot
         << ",\"low\":" << low << ",\"high\":" << high
         << ",\"reason\":\"" << escapeJson(reason) << "\",\"timestampNs\":" << nowNs()
         << "}\n";
  output.flush();
}

void
writePayloadEvent(std::ofstream& output, const std::string& event,
                  const std::string& direction, const std::string& caseId,
                  const std::string& name, SeqNo sequence, size_t length,
                  const std::string& digest, size_t segments,
                  const std::string& stage, const std::string& reason = "")
{
  output << "{\"event\":\"" << escapeJson(event) << "\",\"implementation\":\"cpp\""
         << ",\"protocolVersion\":3,\"direction\":\"" << escapeJson(direction) << "\""
         << ",\"caseId\":\"" << escapeJson(caseId) << "\",\"name\":\""
         << escapeJson(name) << "\",\"sequence\":" << sequence
         << ",\"length\":" << length << ",\"sha256\":\"" << digest << "\""
         << ",\"segments\":" << segments << ",\"stage\":\"" << escapeJson(stage) << "\""
         << ",\"reason\":\"" << escapeJson(reason) << "\",\"timestampNs\":" << nowNs()
         << "}\n";
  output.flush();
}

class HmacValidator final : public BaseValidator
{
public:
  HmacValidator(KeyChain& keyChain, const security::SigningInfo& info)
    : m_keyChain(keyChain)
    , m_keyName(info.getSignerName())
  {
    Data probe("/svs-v3-interop/hmac/probe");
    probe.setContent("probe");
    m_keyChain.sign(probe, info);
  }

  void
  validate(const Data& data, const security::DataValidationSuccessCallback& success,
           const security::DataValidationFailureCallback& failure) override
  {
    if (security::verifySignature(data, m_keyChain.getTpm(), m_keyName,
                                  DigestAlgorithm::SHA256)) {
      success(data);
    }
    else {
      failure(data, security::ValidationError(security::ValidationError::INVALID_SIGNATURE,
                                              "SVS V3 HMAC mismatch"));
    }
  }

private:
  KeyChain& m_keyChain;
  Name m_keyName;
};

struct PayloadCase
{
  std::string id;
  Name cppName;
  Name ndntsName;
  std::vector<uint8_t> payload;
  std::string digest;
  bool requiresSegmentation = false;
  size_t segmentHint = 4096;
};

std::vector<uint8_t>
readBytes(const std::filesystem::path& path)
{
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot read payload file: " + path.string());
  }
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string
sha256(span<const uint8_t> input)
{
  unsigned char value[SHA256_DIGEST_LENGTH];
  SHA256(input.data(), input.size(), value);
  std::ostringstream output;
  for (unsigned char byte : value) {
    output << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte);
  }
  return output.str();
}

std::vector<PayloadCase>
loadPayloadManifest(const std::filesystem::path& path)
{
  if (path.empty()) {
    throw std::runtime_error("--manifest is required in payload mode");
  }
  boost::property_tree::ptree manifest;
  boost::property_tree::read_json(path.string(), manifest);
  if (manifest.get<std::string>("schemaVersion") != "spec117-payload-corpus-v1") {
    throw std::runtime_error("invalid Spec 117 payload manifest");
  }
  std::vector<PayloadCase> cases;
  for (const auto& entry : manifest.get_child("cases")) {
    const auto& item = entry.second;
    PayloadCase value;
    value.id = item.get<std::string>("caseId");
    value.cppName = Name(item.get<std::string>("names.cpp"));
    value.ndntsName = Name(item.get<std::string>("names.ndnts"));
    value.payload = readBytes(path.parent_path() / item.get<std::string>("path"));
    value.digest = item.get<std::string>("sha256");
    value.requiresSegmentation = item.get<bool>("requiresSegmentation");
    value.segmentHint = item.get<size_t>("segmentHint");
    if (value.payload.size() != item.get<size_t>("length") ||
        sha256(make_span(value.payload.data(), value.payload.size())) != value.digest) {
      throw std::runtime_error("manifest payload mismatch: " + value.id);
    }
    cases.push_back(std::move(value));
  }
  if (cases.size() != 4) {
    throw std::runtime_error("Spec 117 payload manifest must contain four cases");
  }
  return cases;
}

size_t
segmentLowerBound(size_t length)
{
  return std::max<size_t>(1, (length + 8799) / 8800);
}

SyncProtocolOptions
makeProtocol(const std::string& version)
{
  SyncProtocolOptions protocol;
  protocol.version = version == "v2" ? SvsProtocolVersion::V2 : SvsProtocolVersion::V3;
  protocol.syncInterestLifetime = version == "v2" ? 1_ms : 1_s;
  protocol.periodicTimeout = 30_s;
  protocol.suppressionPeriod = version == "v2" ? 500_ms : 200_ms;
  protocol.periodicJitter = 0.1;
  return protocol;
}

void
runSync(const std::string& version, const Name& syncPrefix, const Name& nodePrefix,
        size_t publishCount, size_t intervalMs, size_t startDelayMs, size_t settleMs,
        Face& face, const SecurityOptions& securityOptions, std::ofstream& events)
{
  const auto protocol = makeProtocol(version);
  writeEvent(events, "startup", version, nodePrefix.toUri());
  SVSync sync(syncPrefix, nodePrefix, face,
              [&] (const std::vector<MissingDataInfo>& updates) {
                for (const auto& update : updates) {
                  writeEvent(events, "update", version, update.nodeId.toUri(),
                             update.bootstrapTime, update.low, update.high);
                }
              }, securityOptions, SVSync::DEFAULT_DATASTORE, protocol);

  auto& core = sync.getCore();
  core.sendInitialInterest();
  const auto deadline = std::chrono::steady_clock::now() +
    std::chrono::milliseconds(startDelayMs + intervalMs * publishCount + settleMs);
  SeqNo published = 0;
  auto nextPublish = std::chrono::steady_clock::now() + std::chrono::milliseconds(startDelayMs);
  while (std::chrono::steady_clock::now() < deadline) {
    face.processEvents(10_ms);
    if (published < publishCount && std::chrono::steady_clock::now() >= nextPublish) {
      ++published;
      core.updateSeqNo(published, nodePrefix);
      writeEvent(events, "publish", version, nodePrefix.toUri(),
                 core.getBootstrapTime(), published, published);
      nextPublish += std::chrono::milliseconds(intervalMs);
    }
  }

  writeEvent(events, "final", version, nodePrefix.toUri(), core.getBootstrapTime(),
             1, core.getSeqNo(nodePrefix), core.getStateStr());
  for (const auto& [node, epochs] : core.getState().getAllEntries()) {
    for (const auto& [boot, seq] : epochs) {
      writeEvent(events, "state", version, node.toUri(), boot, seq, seq);
    }
  }
  writeEvent(events, "shutdown", version, nodePrefix.toUri());
}

void
runPayload(const Name& syncPrefix, const Name& nodePrefix,
           size_t intervalMs, size_t startDelayMs, size_t settleMs,
           const std::filesystem::path& manifestPath, Face& face,
           const SecurityOptions& securityOptions, std::ofstream& events)
{
  const auto cases = loadPayloadManifest(manifestPath);
  std::map<Name, const PayloadCase*> remoteByName;
  for (const auto& item : cases) {
    remoteByName.emplace(item.ndntsName, &item);
  }
  std::set<std::string> received;
  bool sawRemoteSync = false;
  auto protocol = makeProtocol("v3");
  SVSPubSubOptions options;
  options.syncProtocol = protocol;
  options.useTimestamp = false;
  options.publicationFetchRetries = 2;
  options.publicationFetchInnerRetries = 2;
  options.publicationFetchInterestLifetime = 500_ms;

  SVSPubSub pubsub(syncPrefix, nodePrefix, face,
                   [&] (const std::vector<MissingDataInfo>& updates) {
                     for (const auto& update : updates) {
                       if (update.nodeId == Name("/ndnts")) {
                         sawRemoteSync = true;
                       }
                       writePayloadEvent(events, "sync-update", "ndnts-to-cpp", "",
                                         update.nodeId.toUri(), update.high, 0, "", 0,
                                         "sync", "");
                     }
                   }, options, securityOptions);

  pubsub.subscribe(Name("/ndnsf/svs-pubsub-interop/payload"),
                   [&] (const SVSPubSub::SubscriptionData& data) {
                     auto found = remoteByName.find(data.name);
                     if (found == remoteByName.end()) {
                       return; // SVSPubSub also reports this peer's own publications.
                     }
                     const auto& id = found->second->id;
                     received.insert(id);
                     const auto digest = sha256(data.data);
                     writePayloadEvent(events, "receive", "ndnts-to-cpp", id,
                                       data.name.toUri(), data.seqNo, data.data.size(), digest,
                                       segmentLowerBound(data.data.size()), "payload-check", "");
                   });

  writePayloadEvent(events, "startup", "cpp-to-ndnts", "", nodePrefix.toUri(),
                    0, 0, "", 0, "sync", "");
  const auto deadline = std::chrono::steady_clock::now() +
    std::chrono::milliseconds(startDelayMs + intervalMs * cases.size() + settleMs);
  auto nextPublish = std::chrono::steady_clock::now() + std::chrono::milliseconds(startDelayMs);
  size_t publishIndex = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    face.processEvents(10_ms);
    if (publishIndex < cases.size() && std::chrono::steady_clock::now() >= nextPublish) {
      const auto& item = cases[publishIndex++];
      const auto sequence = pubsub.publish(item.cppName,
        make_span(item.payload.data(), item.payload.size()), Name(), 60_s);
      writePayloadEvent(events, "publish", "cpp-to-ndnts", item.id,
                        item.cppName.toUri(), sequence, item.payload.size(), item.digest,
                        item.requiresSegmentation ? segmentLowerBound(item.payload.size()) : 1,
                        "publish", "");
      nextPublish += std::chrono::milliseconds(intervalMs);
    }
  }

  for (const auto& item : cases) {
    if (received.count(item.id) == 0) {
      writePayloadEvent(events, "error", "ndnts-to-cpp", item.id,
                        item.ndntsName.toUri(), 0, 0, "", 0,
                        sawRemoteSync ? "mapping" : "sync",
                        "payload not received before bounded deadline");
    }
  }
  writePayloadEvent(events, "shutdown", "cpp-to-ndnts", "", nodePrefix.toUri(),
                    0, 0, "", 0, "payload-check", "");
}

} // namespace

int
main(int argc, char** argv)
{
  using namespace ndn;
  using namespace ndn::svs;
  try {
    const auto mode = argument(argc, argv, "--mode", "sync");
    const auto version = argument(argc, argv, "--version", "v3");
    const Name syncPrefix(argument(argc, argv, "--sync-prefix", "/ndn/svs-v3-interop"));
    const Name nodePrefix(argument(argc, argv, "--node-prefix", "/cpp"));
    const auto publishCount = std::stoul(argument(argc, argv, "--publish-count", "5"));
    const auto intervalMs = std::stoul(argument(argc, argv, "--publish-interval-ms", "20"));
    const auto startDelayMs = std::stoul(argument(argc, argv, "--start-delay-ms", "500"));
    const auto settleMs = std::stoul(argument(argc, argv, "--settle-ms", "1500"));
    const auto hmac = argument(argc, argv, "--hmac-key-base64",
                               "c3BlYzExNC1wdWJsaWMtaG1hYy10ZXN0LWtleQ==");
    const auto eventsPath = argument(argc, argv, "--events", "/tmp/svs-v3-cpp.jsonl");
    const auto manifestPath = argument(argc, argv, "--manifest", "");
    std::ofstream events(eventsPath, std::ios::out | std::ios::trunc);
    if (!events) {
      throw std::runtime_error("cannot open events file");
    }

    Face face;
    KeyChain keyChain("pib-memory:svs-v3-peer", "tpm-memory:svs-v3-peer");
    SecurityOptions securityOptions(keyChain);
    securityOptions.interestSigner->signingInfo.setSigningHmacKey(hmac);
    securityOptions.dataSigner->signingInfo.setSigningHmacKey(hmac);
    securityOptions.pubSigner->signingInfo.setSigningHmacKey(hmac);
    auto validator =
      std::make_shared<HmacValidator>(keyChain, securityOptions.dataSigner->signingInfo);
    securityOptions.validator = validator;
    securityOptions.encapsulatedDataValidator = validator;

    if (mode == "payload") {
      if (version != "v3") {
        throw std::runtime_error("payload mode requires SVS V3");
      }
      runPayload(syncPrefix, nodePrefix, intervalMs, startDelayMs, settleMs,
                 manifestPath, face, securityOptions, events);
    }
    else if (mode == "sync") {
      runSync(version, syncPrefix, nodePrefix, publishCount, intervalMs,
              startDelayMs, settleMs, face, securityOptions, events);
    }
    else {
      throw std::runtime_error("unsupported --mode " + mode);
    }
    return 0;
  }
  catch (const std::exception& e) {
    std::cerr << "svs3-peer: " << e.what() << std::endl;
    return 2;
  }
}
