#include "ndn-service-framework/CertificatePublisher.hpp"
#include "ndn-service-framework/ServiceProvider.hpp"

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

const ndn::Name GROUP_PREFIX("/example/hello/group");
const ndn::Name CONTROLLER_PREFIX("/example/hello/controller");
const ndn::Name PROVIDER_IDENTITY("/example/hello/provider");
const ndn::Name SERVICE_NAME("/AI/FNN/Inference");

std::string
getOption(int argc, char** argv, const std::string& option, const std::string& fallback)
{
  for (int i = 1; i + 1 < argc; ++i) {
    if (argv[i] == option) {
      return argv[i + 1];
    }
  }
  return fallback;
}

bool
hasFlag(int argc, char** argv, const std::string& option)
{
  for (int i = 1; i < argc; ++i) {
    if (argv[i] == option) {
      return true;
    }
  }
  return false;
}

ndn::security::Certificate
getOrCreateIdentity(ndn::security::KeyChain& keyChain, const ndn::Name& identity)
{
  try {
    return keyChain.getPib().getIdentity(identity).getDefaultKey().getDefaultCertificate();
  }
  catch (const std::exception&) {
    return keyChain.createIdentity(identity, ndn::RsaKeyParams(2048))
      .getDefaultKey().getDefaultCertificate();
  }
}

std::vector<double>
parseVector(const ndn::Buffer& payload)
{
  const std::string text(reinterpret_cast<const char*>(payload.data()), payload.size());
  std::vector<double> values;
  std::stringstream ss(text);
  std::string item;
  while (std::getline(ss, item, ',')) {
    if (!item.empty()) {
      values.push_back(std::stod(item));
    }
  }
  return values;
}

ndn::Buffer
toBuffer(const std::string& text)
{
  return ndn::Buffer(reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

std::string
serializeVector(const std::vector<double>& values)
{
  std::ostringstream os;
  for (size_t i = 0; i < values.size(); ++i) {
    if (i > 0) {
      os << ',';
    }
    os << values[i];
  }
  return os.str();
}

std::string
field(const std::string& text, const std::string& key, const std::string& fallback = "")
{
  const auto marker = key + "=";
  const auto begin = text.find(marker);
  if (begin == std::string::npos) {
    return fallback;
  }
  const auto valueBegin = begin + marker.size();
  const auto valueEnd = text.find(';', valueBegin);
  return text.substr(valueBegin, valueEnd - valueBegin);
}

std::string
assignmentRole(const ndn_service_framework::ServiceProvider::CollaborationContext& ctx,
               const std::string& fallback)
{
  if (!ctx.assignment().role.empty()) {
    return ctx.assignment().role;
  }
  const auto& payload = ctx.assignment().assignmentPayload;
  const std::string text(reinterpret_cast<const char*>(payload.data()), payload.size());
  return field(text, "role", fallback);
}

int
roleStage(const std::string& role)
{
  if (role.size() != 3 || role[0] != 'p') {
    return -1;
  }
  return role[1] - '0';
}

int
roleShard(const std::string& role)
{
  if (role.size() != 3 || role[0] != 'p') {
    return -1;
  }
  return role[2] - '0';
}

std::string
roleFor(int stage, int shard)
{
  return "p" + std::to_string(stage) + std::to_string(shard);
}

bool
hasStageInputs(const std::map<std::string, std::vector<double>>& byRole,
               int stage)
{
  for (int shard = 0; shard < 3; ++shard) {
    if (byRole.count(roleFor(stage, shard)) == 0) {
      return false;
    }
  }
  return true;
}

std::vector<double>
joinStageInputs(const std::map<std::string, std::vector<double>>& byRole,
                int stage)
{
  std::vector<double> input;
  for (int shard = 0; shard < 3; ++shard) {
    const auto it = byRole.find(roleFor(stage, shard));
    if (it == byRole.end() || it->second.size() != 1) {
      throw std::runtime_error("missing stage input shard");
    }
    input.push_back(it->second.front());
  }
  return input;
}

std::vector<double>
parseBinaryArtifactValues(const ndn::Buffer& artifact,
                          size_t& offset,
                          size_t count)
{
  std::vector<double> values;
  values.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    if (offset + sizeof(double) > artifact.size()) {
      throw std::runtime_error("truncated binary artifact");
    }
    double value = 0.0;
    std::memcpy(&value, artifact.data() + offset, sizeof(double));
    offset += sizeof(double);
    values.push_back(value);
  }
  return values;
}

uint32_t
readU32(const ndn::Buffer& artifact, size_t& offset)
{
  if (offset + 4 > artifact.size()) {
    throw std::runtime_error("truncated binary artifact");
  }
  uint32_t value = 0;
  for (size_t i = 0; i < 4; ++i) {
    value = (value << 8) | artifact[offset++];
  }
  return value;
}

std::vector<double>
runLinearReluArtifact(const ndn::Buffer& artifact, const std::vector<double>& input)
{
  if (artifact.size() < 9) {
    throw std::runtime_error("invalid binary artifact");
  }
  size_t offset = 0;
  const char kind = static_cast<char>(artifact[offset++]);
  const uint32_t rows = readU32(artifact, offset);
  const uint32_t cols = readU32(artifact, offset);
  if (kind != 'H' && kind != 'O') {
    throw std::runtime_error("unknown artifact operator kind");
  }
  if (rows == 0 || cols == 0 || rows > 1024 || cols > 1024 ||
      rows > (std::numeric_limits<uint32_t>::max() / cols)) {
    throw std::runtime_error("invalid artifact shape bounds");
  }
  const size_t weightCount = static_cast<size_t>(rows) * static_cast<size_t>(cols);
  const size_t expectedSize = 1 + 4 + 4 +
                              (weightCount + static_cast<size_t>(rows)) *
                                sizeof(double);
  if (artifact.size() != expectedSize) {
    throw std::runtime_error("unexpected binary artifact size");
  }
  const auto weights = parseBinaryArtifactValues(artifact, offset, weightCount);
  const auto bias = parseBinaryArtifactValues(artifact, offset, rows);
  if (weights.size() != weightCount ||
      bias.size() != static_cast<size_t>(rows) ||
      input.size() != static_cast<size_t>(cols)) {
    throw std::runtime_error("invalid artifact shape");
  }
  std::vector<double> output;
  output.reserve(rows);
  const bool relu = kind == 'H';
  for (uint32_t r = 0; r < rows; ++r) {
    double y = bias[r];
    for (uint32_t c = 0; c < cols; ++c) {
      y += weights[static_cast<size_t>(r * cols + c)] * input[static_cast<size_t>(c)];
    }
    output.push_back(relu ? std::max(0.0, y) : y);
  }
  return output;
}

ndn::Buffer
loadAssignedArtifact(ndn_service_framework::ServiceProvider::CollaborationContext& ctx)
{
  const auto artifactName = ctx.assignment().assignedArtifact;
  if (artifactName.empty()) {
    throw std::runtime_error("missing assigned artifact name");
  }
  if (!ctx.fetchArtifact(artifactName, ctx.assignment().provisioningTimeoutMs)) {
    throw std::runtime_error("failed to fetch assigned artifact");
  }
  auto artifact = ctx.getArtifact(artifactName);
  if (!artifact) {
    throw std::runtime_error("artifact fetch produced no local data");
  }
  return *artifact;
}

} // namespace

int
main(int argc, char** argv)
{
  try {
    ndn::Face face;
    ndn::KeyChain keyChain;

    const std::string providerId = getOption(argc, argv, "--provider-id", "");
    const std::string configuredRole = getOption(argc, argv, "--role", providerId.empty() ? "p0" : providerId);
    const ndn::Name providerIdentity = providerId.empty()
      ? PROVIDER_IDENTITY
      : ndn::Name(PROVIDER_IDENTITY).append(providerId);

    auto providerCert = getOrCreateIdentity(keyChain, providerIdentity);
    auto controllerCert = getOrCreateIdentity(keyChain, CONTROLLER_PREFIX);
    keyChain.setDefaultIdentity(keyChain.getPib().getIdentity(providerIdentity));

    std::unique_ptr<ndn_service_framework::CertificatePublisher> certPublisher;
    if (!hasFlag(argc, argv, "--no-serve-certificates")) {
      certPublisher = std::make_unique<ndn_service_framework::CertificatePublisher>(
        face, keyChain, providerCert.getName());
    }

    ndn_service_framework::ServiceProvider provider(face,
                                                    GROUP_PREFIX,
                                                    providerCert,
                                                    controllerCert,
                                                    "examples/trust-schema.conf");
    provider.setUseTokens(!hasFlag(argc, argv, "--disable-tokens"));
    provider.setHandlerThreads(4);
    provider.setAckThreads(2);

    provider.addCollaborationHandler(
      SERVICE_NAME,
      {configuredRole},
      [configuredRole](const ndn_service_framework::RequestMessage&) {
        ndn_service_framework::ServiceProvider::AckDecision decision;
        decision.status = true;
        decision.message = "AI collaboration provider ready";
        const std::string payload =
          "role=" + configuredRole +
          ";artifact=/artifacts/random-fnn/" + configuredRole + ";queue=0;";
        decision.payload = toBuffer(payload);
        return decision;
      },
      [configuredRole](ndn_service_framework::ServiceProvider::CollaborationContext& ctx,
                       const ndn_service_framework::RequestMessage& request) {
        const auto role = assignmentRole(ctx, configuredRole);

        const int stage = roleStage(role);
        const int shard = roleShard(role);
        if (stage == 0 && shard >= 0 && shard < 3) {
          const auto x = parseVector(request.getPayload());
          const auto artifact = loadAssignedArtifact(ctx);
          const auto h = runLinearReluArtifact(artifact, x);
          ctx.publish("stage0-internal",
                      ndn::Name("/hidden").append(role),
                      toBuffer(serializeVector(h)));
          ctx.publish("stage0-to-stage1",
                      ndn::Name("/stage0").append(role),
                      toBuffer(serializeVector(h)));
          return;
        }

        if ((stage == 1 || stage == 2) && shard >= 0 && shard < 3) {
          const auto artifact = loadAssignedArtifact(ctx);
          struct StageState
          {
            std::mutex mutex;
            std::map<std::string, std::vector<double>> inputsByRole;
            bool computed = false;
          };
          auto state = std::make_shared<StageState>();
          const std::string inputScope = stage == 1 ? "stage0-to-stage1" : "stage1-to-stage2";
          const ndn::Name inputPrefix = stage == 1 ? ndn::Name("/stage0") : ndn::Name("/stage1");
          const std::string outputScope = stage == 1 ? "stage1-to-stage2" : "stage2-internal";
          const ndn::Name outputPrefix = stage == 1 ? ndn::Name("/stage1") : ndn::Name("/output");
          const int producerStage = stage - 1;
          ctx.subscribe(
            inputScope,
            inputPrefix,
            [state, artifact, role, stage, producerStage, outputScope, outputPrefix](
              ndn_service_framework::ServiceProvider::CollaborationContext& cbCtx,
              const ndn_service_framework::ServiceProvider::CollaborationData& part) {
            double y = 0.0;
            {
              std::lock_guard<std::mutex> lock(state->mutex);
              state->inputsByRole[part.producerRole] = parseVector(part.payload);
              if (state->computed ||
                  !hasStageInputs(state->inputsByRole, producerStage)) {
                return;
              }

              const auto input = joinStageInputs(state->inputsByRole, producerStage);
              const auto output = runLinearReluArtifact(artifact, input);
              if (output.empty()) {
                cbCtx.fail("empty output shard");
                return;
              }
              y = output.front();
              state->computed = true;
            }

            ndn::Name outputTopic(outputPrefix);
            outputTopic.append(role);
            cbCtx.publish(stage == 1 ? "stage1-internal" : "stage2-internal",
                          outputTopic,
                          toBuffer(std::to_string(y)));

            if (stage == 1) {
              cbCtx.publish(outputScope,
                            outputTopic,
                            toBuffer(std::to_string(y)));
            }

            if (role == "p22") {
              struct FinalAggregationState
              {
                std::mutex mutex;
                std::map<std::string, double> byRole;
                bool published = false;
              };
              auto finalState = std::make_shared<FinalAggregationState>();
              {
                std::lock_guard<std::mutex> lock(finalState->mutex);
                finalState->byRole[role] = y;
              }
              cbCtx.subscribe(
                "stage2-internal",
                ndn::Name("/output"),
                [finalState](
                  ndn_service_framework::ServiceProvider::CollaborationContext& finalCtx,
                  const ndn_service_framework::ServiceProvider::CollaborationData& peer) {
                const std::string peerText(
                  reinterpret_cast<const char*>(peer.payload.data()),
                  peer.payload.size());
                std::vector<double> out;
                {
                  std::lock_guard<std::mutex> lock(finalState->mutex);
                  finalState->byRole[peer.producerRole] = std::stod(peerText);
                  if (finalState->published ||
                      finalState->byRole.count("p20") == 0 ||
                      finalState->byRole.count("p21") == 0 ||
                      finalState->byRole.count("p22") == 0) {
                    return;
                  }
                  finalState->published = true;
                  out = {
                    finalState->byRole["p20"],
                    finalState->byRole["p21"],
                    finalState->byRole["p22"]
                  };
                }
                finalCtx.publishFinalResponse(toBuffer(serializeVector(out)));
              });
            }
          });
        }
      });

    provider.fetchPermissionsFromController(CONTROLLER_PREFIX);
    provider.init();

    std::cout << "[AI_Provider] identity=" << providerIdentity
              << " role=" << configuredRole << std::endl;
    while (true) {
      face.processEvents();
    }
  }
  catch (const std::exception& e) {
    std::cerr << "AI_DistributedCollaborationProvider error: "
              << e.what() << std::endl;
    return 1;
  }
}
