#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPreparation.hpp"

#include <boost/test/unit_test.hpp>
#include <openssl/sha.h>

namespace ndnsf::di {
namespace {
std::string digest(const std::string& value)
{
  unsigned char hash[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), hash);
  std::string result = "sha256:";
  for (const auto byte : hash) {
    result += "0123456789abcdef"[byte >> 4];
    result += "0123456789abcdef"[byte & 15];
  }
  return result;
}

class Adapter final : public NativeModelAdapter
{
public:
  std::string adapterId() const override { return "fixture"; }
  std::string adapterVersion() const override { return "1"; }
  NativeModelDescriptor inspect(const std::string& name,
                               const std::string& content) const override
  {
    NativeModelDescriptor model;
    model.modelName = name;
    model.contentDigest = content;
    model.semanticsDigest = digest("semantics");
    model.graphDigest = digest("graph");
    model.modelFormat = "fixture";
    model.precision = "float32";
    model.adapterId = adapterId();
    model.adapterVersion = adapterVersion();
    return model;
  }
  std::vector<std::uint8_t> encodeInput(
    const std::vector<std::uint8_t>& input) const override { return input; }
  std::vector<std::uint8_t> decodeResult(
    const std::vector<std::uint8_t>& result) const override { return result; }
};
} // namespace

BOOST_AUTO_TEST_CASE(NativePreparationBindsAdapterAndGraphPort)
{
  const auto modelDigest = digest("model");
  const auto schemaDigest = digest("schema");
  auto registry = std::make_shared<NativeAdapterRegistry>();
  registry->registerAdapter(std::make_shared<Adapter>());
  registry->freeze();
  NativeRequestPreparation preparation(
    registry,
    [] (const NativePreparedInput&, const NativeModelDescriptor& model) {
      NativeGraphSnapshot graph;
      graph.graphDigest = model.graphDigest;
      graph.nodes = {{"node", "Identity", 0}};
      graph.topologicalOrder = {"node"};
      return graph;
    },
    [] (const NativeInspectedModel&, const NativePlacementProposal&,
        const NativeRequestControl&) {
      return NativeArtifactBinding{{{"role", "/artifact"}},
                                   {{"role", digest("artifact")}},
                                   digest("manifest"), digest("recipe")};
    });
  const auto input = preparation.prepareInput(
    NativeModelDescriptor{"model", modelDigest, digest("semantics"), digest("graph"),
                          "fixture", "float32", "fixture", "1"},
    "task", schemaDigest, schemaDigest, {1, 2, 3}, {},
    std::chrono::steady_clock::now() + std::chrono::seconds(1));
  BOOST_CHECK(input.encoded);
  const auto inspected = preparation.inspectModel(input);
  BOOST_CHECK_EQUAL(inspected.graph.nodes.size(), 1);
  NativeRequestControl control{"/request/1", 1,
    std::chrono::steady_clock::now() + std::chrono::seconds(1), {}};
  BOOST_CHECK_NO_THROW(preparation.ensureArtifacts(
    inspected, NativePlacementProposal{}, control));
}

BOOST_AUTO_TEST_CASE(NativeOfferAdmissionRejectsUnauthenticatedAck)
{
  NativeOfferAdmission admission;
  NativeOfferPolicySnapshot policy;
  policy.policyDigest = digest("policy");
  policy.acceptedProviders = {"/provider"};
  policy.acceptedServices = {"/service"};
  policy.acceptedSignerIdentities = {"/controller"};
  policy.acceptedRoles = {"role"};
  policy.backends = {"onnxruntime"};
  policy.resourceSequence = 1;
  policy.expiresAtMs = 100;
  NativeOfferBindingContext context{"/request", 1, "/service", digest("model"), digest("graph")};
  NativeAckEvidence ack;
  ack.requestId = context.requestId;
  ack.attempt = context.attempt;
  ack.serviceName = context.serviceName;
  ack.modelDigest = context.modelDigest;
  ack.graphDigest = context.graphDigest;
  BOOST_CHECK_THROW(admission.verify(ack, policy, context, 1), std::runtime_error);
}

} // namespace ndnsf::di
