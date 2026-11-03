#ifndef NDNSF_TESTS_NATIVE_PROTECTED_KV_FIXTURE_HPP
#define NDNSF_TESTS_NATIVE_PROTECTED_KV_FIXTURE_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/protected-kv-checkpoint.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/portable-kv-checkpoint.hpp"

#include <boost/test/unit_test.hpp>
#include <openssl/evp.h>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

namespace ndnsf::di::test::protectedKv {
inline std::string hash(const std::string& bytes)
{
  unsigned char output[32]; unsigned size = 0;
  BOOST_REQUIRE_EQUAL(EVP_Digest(bytes.data(), bytes.size(), output, &size, EVP_sha256(), nullptr), 1);
  BOOST_REQUIRE_EQUAL(size, 32U); std::string result = "sha256:";
  for (auto byte : output) { result += "0123456789abcdef"[byte >> 4]; result += "0123456789abcdef"[byte & 15]; }
  return result;
}
inline std::string read(const std::filesystem::path& path)
{ std::ifstream input(path, std::ios::binary); return {std::istreambuf_iterator<char>(input), {}}; }
inline void write(const std::filesystem::path& path, const std::string& bytes)
{
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  BOOST_REQUIRE(output.write(bytes.data(), bytes.size())); output.close();
  BOOST_REQUIRE_EQUAL(::chmod(path.c_str(), 0600), 0);
}
inline NativePortableKvSnapshotV1 sample()
{
  const auto digest = hash("committed role binding"); NativePortableKvSnapshotV1 state;
  auto& id = state.originIdentity;
  id.modelDigest = id.graphSemanticDigest = id.artifactDigest = id.adapterDigest = digest;
  id.tokenizerDigest = id.runnerDigest = id.roleSplitDigest = id.prefixDigest = digest;
  id.positionDigest = id.layoutDigest = id.stateSchemaDigest = id.runtimeAbiDigest = id.securityDomainDigest = digest;
  id.roleName = "decoder"; id.layerBegin = 0; id.layerEnd = 2; id.prefixTokenCount = 9;
  id.precision = "uint8"; id.stateComponentDigests = {digest};
  id.providerIdentity = "/actual/producer/complete/a"; id.providerBootId = "private-boot-a";
  id.requestId = "/actual/owner/request/9"; id.generationId = "private-generation";
  id.stateInferenceEpoch = 9; id.predecessorInferenceEpoch = 8; id.cacheEpoch = 3; id.attemptEpoch = 1;
  NativePortableKvComponentV1 component; component.inputName = "past/key";
  component.elementType = TensorElementType::UInt8; component.shape = {3 * 65536 + 7};
  std::vector<std::uint8_t> payload(static_cast<std::size_t>(component.shape[0]), 0x5a);
  component.payloadBytes = payload.size();
  component.tensorWire = encodeTensorBundle({NamedTensor{component.inputName, component.elementType, component.shape, payload}});
  component.contentDigest = hash(std::string(component.tensorWire.begin(), component.tensorWire.end()));
  state.components = {component}; state.stateIoDigest = digest;
  state.logicalBytes = component.payloadBytes; state.encodedBytes = component.tensorWire.size(); return state;
}
struct Fixture
{
  ndn::security::KeyChain ownerKeys{"pib-memory:protected-kv-owner", "tpm-memory:protected-kv-owner"};
  ndn::security::Certificate certificate;
  std::filesystem::path directory, plain, cipher, restored;
  NativePortableKvSnapshotV1 state = sample();
  NativeProtectedKvCheckpointLimitsV1 limits{500000, 600000, {}};
  NativePortableKvCheckpointLimitsV1 portableLimits{{400000, 450000, {}}, 8192, 500000};
  Fixture() {
    auto pattern = (std::filesystem::temp_directory_path() / "spec221-protected-kv-XXXXXX").string();
    const auto result = ::mkdtemp(pattern.data());
    if (!result)
      throw std::runtime_error("protected KV fixture directory failed");
    directory = result;
    plain = directory / "plain"; cipher = directory / "cipher"; restored = directory / "restored";
    certificate = ownerKeys.createIdentity(ndn::Name("/actual/owner/complete"), ndn::RsaKeyParams(2048)).getDefaultKey().getDefaultCertificate();
    nativeExportPrivatePortableKvCheckpoint(plain, state, portableLimits);
  }
  ~Fixture() { std::error_code error; std::filesystem::remove_all(directory, error); }
  NativeProtectedKvCheckpointReferenceV1 protect(const NativeCheckpointExportOptions& options = {}) {
    return nativeProtectPortableKvCheckpoint(plain, cipher, certificate, ndn::Name(state.originIdentity.providerIdentity), hash("checkpoint-9"), limits, options);
  }
  void recover(const NativeProtectedKvCheckpointReferenceV1& reference, const NativeCheckpointExportOptions& options = {}) {
    nativeRecoverProtectedPortableKvCheckpoint(cipher, restored, reference, certificate, ownerKeys, limits, options);
  }
};
inline std::uint64_t metadataSize(const std::string& bytes)
{
  BOOST_REQUIRE(bytes.size() >= 32); std::uint64_t size = 0;
  for (unsigned i = 8; i < 16; ++i) size = (size << 8) | static_cast<unsigned char>(bytes[i]);
  BOOST_REQUIRE(size <= bytes.size() - 32); return size;
}
inline std::string replaceMetadata(const std::string& wire, const std::string& metadata)
{
  auto prefix = wire.substr(0, 16); auto size = static_cast<std::uint64_t>(metadata.size());
  for (int i = 15; i >= 8; --i) { prefix[i] = size & 255; size >>= 8; }
  return prefix + metadata + wire.substr(16 + metadataSize(wire));
}
inline void commitMutatedWire(Fixture& fixture, NativeProtectedKvCheckpointReferenceV1& reference, const std::string& wire)
{ write(fixture.cipher, wire); reference.wireBytes = wire.size(); reference.wireDigest = hash(wire); }
} // namespace ndnsf::di::test::protectedKv

#endif // NDNSF_TESTS_NATIVE_PROTECTED_KV_FIXTURE_HPP
