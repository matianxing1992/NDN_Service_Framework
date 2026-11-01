#include "tests/unit-tests/spec204-protected-kv-fixture.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <new>
#include <optional>
#include <random>

using namespace ndnsf::di;
namespace kvFixture = ndnsf::di::test::protectedKv;

namespace {
constexpr std::size_t MaxInputBytes = 600001;

std::filesystem::path corpusDirectory()
{
  const auto setting = std::getenv("NDNSF_SPEC212_FUZZ_CORPUS");
  BOOST_REQUIRE_MESSAGE(setting, "NDNSF_SPEC212_FUZZ_CORPUS is required");
  const std::filesystem::path directory(setting);
  BOOST_REQUIRE(directory.is_absolute());
  return directory;
}

std::string readBounded(const std::filesystem::path& file, std::uint64_t limit)
{
  const auto size = std::filesystem::file_size(file);
  BOOST_REQUIRE_LE(size, limit);
  std::ifstream input(file, std::ios::binary);
  BOOST_REQUIRE(input.good());
  std::string bytes(static_cast<std::size_t>(size), '\0');
  input.read(bytes.data(), static_cast<std::streamsize>(size));
  BOOST_REQUIRE_EQUAL(input.gcount(), static_cast<std::streamsize>(size));
  BOOST_REQUIRE_EQUAL(input.peek(), std::char_traits<char>::eof());
  return bytes;
}

struct Scratch
{
  Scratch()
  {
    auto pattern = (std::filesystem::temp_directory_path() / "spec212-kv-fuzz-XXXXXX").string();
    const auto result = ::mkdtemp(pattern.data());
    BOOST_REQUIRE(result);
    directory = result;
  }
  ~Scratch() { std::error_code error; std::filesystem::remove_all(directory, error); }
  std::filesystem::path directory;
};

// This manifest describes frozen parser inputs; it is never an authority token.
NativeJson describeReference(const NativeProtectedKvCheckpointReferenceV1& reference)
{
  const auto format = ndn::name::UriFormat::ALTERNATE;
  return {{"ownerIdentity", reference.ownerIdentity.toUri(format)},
    {"producerPrefix", reference.producerPrefix.toUri(format)},
    {"ownerCertificateName", reference.ownerCertificateName.toUri(format)},
    {"ownerCertificateDigest", reference.ownerCertificateDigest},
    {"checkpointIdentityDigest", reference.checkpointIdentityDigest},
    {"wireDigest", reference.wireDigest}, {"wireBytes", reference.wireBytes},
    {"keyEnvelope", reference.keyEnvelope}};
}

NativeProtectedKvCheckpointReferenceV1 readReference(const NativeJson& value)
{
  BOOST_REQUIRE_EQUAL(value.size(), 8U);
  return {ndn::Name(value.at("ownerIdentity").get<std::string>()),
    ndn::Name(value.at("producerPrefix").get<std::string>()),
    ndn::Name(value.at("ownerCertificateName").get<std::string>()),
    value.at("ownerCertificateDigest").get<std::string>(),
    value.at("checkpointIdentityDigest").get<std::string>(),
    value.at("wireDigest").get<std::string>(), value.at("wireBytes").get<std::uint64_t>(),
    value.at("keyEnvelope").get<std::string>()};
}

std::vector<std::string> malformedFraming(const std::string& seed, std::size_t byteLimit)
{
  std::vector<std::string> corpus{"", seed.substr(0, 15), seed.substr(0, seed.size() - 1),
    seed + "x", std::string(byteLimit + 1, 'x')};
  auto badMagic = seed;
  badMagic[0] ^= 1;
  corpus.push_back(std::move(badMagic));
  auto overflow = seed;
  std::fill(overflow.begin() + 8, overflow.begin() + 16, static_cast<char>(255));
  corpus.push_back(std::move(overflow));
  const auto metadata = seed.substr(16, kvFixture::metadataSize(seed));
  corpus.push_back(kvFixture::replaceMetadata(seed, " " + metadata));
  corpus.push_back(kvFixture::replaceMetadata(seed, "{\"version\":1," + metadata.substr(1)));
  auto fields = nativeParseJson(metadata);
  fields["unexpected"] = true;
  corpus.push_back(kvFixture::replaceMetadata(seed, nativeCanonicalJson(fields)));
  fields = nativeParseJson(metadata);
  fields["version"] = -1;
  corpus.push_back(kvFixture::replaceMetadata(seed, nativeCanonicalJson(fields)));
  return corpus;
}

std::string mutate(std::string bytes, std::mt19937_64& random)
{
  if (bytes.empty()) bytes.push_back(static_cast<char>(random()));
  const auto window = random() % 2 == 0 ? std::min<std::size_t>(bytes.size(), 8192) : bytes.size();
  const auto offset = static_cast<std::size_t>(random() % window);
  switch (random() % 6) {
    case 0: bytes[offset] ^= static_cast<char>(1U << (random() % 8)); break;
    case 1: bytes.resize(static_cast<std::size_t>(random() % (bytes.size() + 1))); break;
    case 2: bytes.erase(offset, static_cast<std::size_t>(1 + random() % 16)); break;
    case 3: bytes.insert(offset, static_cast<std::size_t>(1 + random() % 16), static_cast<char>(random())); break;
    case 4: bytes.append(static_cast<std::size_t>(1 + random() % 32), static_cast<char>(random())); break;
    case 5:
      if (bytes.size() > 8)
        std::fill(bytes.begin() + 8, bytes.begin() + std::min<std::size_t>(16, bytes.size()),
          static_cast<char>(255));
      break;
  }
  if (bytes.size() > MaxInputBytes) bytes.resize(MaxInputBytes);
  return bytes;
}
} // namespace

BOOST_AUTO_TEST_SUITE(Spec212KvParserFuzz)

BOOST_AUTO_TEST_CASE(GenerateFrozenCorpus)
{
  const auto directory = corpusDirectory();
  BOOST_REQUIRE_MESSAGE(!std::filesystem::exists(directory), "Refusing to replace an existing frozen corpus");
  kvFixture::Fixture fixture;
  const auto reference = fixture.protect();
  const auto plain = readBounded(fixture.plain, fixture.portableLimits.maxCheckpointBytes);
  const auto protectedWire = readBounded(fixture.cipher, fixture.limits.maxWireBytes);
  const auto manifest = nativeCanonicalJson(NativeJson{{"version", 1U},
    {"privateDigest", kvFixture::hash(plain)}, {"protectedDigest", kvFixture::hash(protectedWire)},
    {"reference", describeReference(reference)}});
  BOOST_REQUIRE(std::filesystem::create_directory(directory));
  BOOST_REQUIRE_EQUAL(::chmod(directory.c_str(), 0700), 0);
  kvFixture::write(directory / "private.ndkv", plain);
  kvFixture::write(directory / "protected.ndkve", protectedWire);
  kvFixture::write(directory / "manifest.json", manifest);
  BOOST_TEST_MESSAGE("Frozen parser corpus manifest=" << kvFixture::hash(manifest)
    << " privateBytes=" << plain.size() << " protectedBytes=" << protectedWire.size());
}

BOOST_AUTO_TEST_CASE(BoundedMutationCorpusUsesProductionParsers)
{
  const auto directory = corpusDirectory();
  const auto expectedDigest = std::getenv("NDNSF_SPEC212_FUZZ_MANIFEST_DIGEST");
  BOOST_REQUIRE_MESSAGE(expectedDigest, "NDNSF_SPEC212_FUZZ_MANIFEST_DIGEST is required");
  const auto manifestWire = readBounded(directory / "manifest.json", 65536);
  BOOST_REQUIRE_EQUAL(kvFixture::hash(manifestWire), "sha256:" + std::string(expectedDigest));
  const auto manifest = nativeParseJson(manifestWire);
  BOOST_REQUIRE_EQUAL(manifest.size(), 4U);
  BOOST_REQUIRE(manifest.at("version") == 1U);
  const auto reference = readReference(manifest.at("reference"));
  const NativePortableKvCheckpointLimitsV1 portableLimits{{400000, 450000, {}}, 8192, 500000};
  const NativeProtectedKvCheckpointLimitsV1 protectedLimits{500000, 600000, {}};
  const std::array<std::string, 3> seeds{
    readBounded(directory / "private.ndkv", portableLimits.maxCheckpointBytes),
    readBounded(directory / "protected.ndkve", protectedLimits.maxWireBytes), reference.keyEnvelope};
  BOOST_REQUIRE_EQUAL(kvFixture::hash(seeds[0]), manifest.at("privateDigest").get<std::string>());
  BOOST_REQUIRE_EQUAL(kvFixture::hash(seeds[1]), manifest.at("protectedDigest").get<std::string>());
  Scratch scratch;
  const auto input = scratch.directory / "input";
  const auto roundTrip = scratch.directory / "round-trip";
  const auto parse = [&](std::size_t lane, const std::string& bytes) {
    if (lane != 2) kvFixture::write(input, bytes);
    std::optional<NativePortableKvSnapshotV1> decoded;
    std::optional<ndn::Buffer> loaded;
    try {
      if (lane == 0) {
        decoded = nativeReadPrivatePortableKvCheckpoint(input, portableLimits);
      }
      else if (lane == 1) {
        auto expected = reference;
        // Reach the representation parser without claiming a storage/authentication proof.
        expected.wireBytes = bytes.size();
        expected.wireDigest = kvFixture::hash(bytes);
        loaded = nativeLoadProtectedPortableKvCheckpoint(input, expected, protectedLimits);
      }
      else {
        auto expected = reference;
        expected.keyEnvelope = bytes;
        nativeValidateProtectedKvKeyEnvelope(expected);
      }
    }
    catch (const std::bad_alloc&) { throw; }
    catch (const std::exception&) { return false; }
    if (decoded) {
      BOOST_REQUIRE_LE(decoded->logicalBytes, portableLimits.portable.maxPayloadBytes);
      BOOST_REQUIRE_LE(decoded->encodedBytes, portableLimits.portable.maxEncodedBytes);
      nativeExportPrivatePortableKvCheckpoint(roundTrip, *decoded, portableLimits);
      BOOST_REQUIRE_MESSAGE(readBounded(roundTrip, portableLimits.maxCheckpointBytes) == bytes,
        "Accepted private representation changed on canonical round-trip");
    }
    if (loaded) {
      BOOST_REQUIRE_EQUAL(loaded->size(), bytes.size());
      BOOST_REQUIRE_MESSAGE(std::equal(loaded->begin(), loaded->end(), bytes.begin(),
        [](std::uint8_t a, char b) { return a == static_cast<std::uint8_t>(b); }),
        "Protected parser changed the committed input wire");
    }
    return true;
  };
  for (std::size_t lane = 0; lane < seeds.size(); ++lane)
    BOOST_REQUIRE_MESSAGE(parse(lane, seeds[lane]), "Legal frozen parser seed rejected in lane " << lane);

  std::array<std::vector<std::string>, 3> malformed{
    malformedFraming(seeds[0], portableLimits.maxCheckpointBytes),
    malformedFraming(seeds[1], protectedLimits.maxWireBytes),
    std::vector<std::string>{"", " " + seeds[2], std::string(16385, 'x'),
      "{\"version\":1," + seeds[2].substr(1)}};
  auto envelopeFields = nativeParseJson(seeds[2]);
  envelopeFields["unexpected"] = true;
  malformed[2].push_back(nativeCanonicalJson(envelopeFields));
  envelopeFields = nativeParseJson(seeds[2]);
  envelopeFields["version"] = -1;
  malformed[2].push_back(nativeCanonicalJson(envelopeFields));
  for (std::size_t lane = 0; lane < seeds.size(); ++lane)
    for (const auto& bytes : malformed[lane])
      BOOST_REQUIRE_MESSAGE(!parse(lane, bytes), "Known malformed seed accepted in lane " << lane);

  std::mt19937_64 random(0x2124b564b564bULL);
  std::array<std::uint64_t, 3> attempts{}, accepted{};
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
  std::uint64_t iterations = 0;
  do {
    const auto lane = static_cast<std::size_t>(iterations % seeds.size());
    const auto& seed = random() % 2 == 0 ? seeds[lane] :
      malformed[lane][static_cast<std::size_t>(random() % malformed[lane].size())];
    const auto candidate = mutate(seed, random);
    BOOST_REQUIRE_LE(candidate.size(), MaxInputBytes);
    ++attempts[lane];
    if (parse(lane, candidate)) ++accepted[lane];
    ++iterations;
  } while (std::chrono::steady_clock::now() < deadline);
  BOOST_REQUIRE_GE(iterations, 1000U);
  for (std::size_t lane = 0; lane < attempts.size(); ++lane) {
    BOOST_CHECK_GT(attempts[lane], 0U);
    BOOST_TEST_MESSAGE("Parser mutation lane=" << lane << " attempts=" << attempts[lane]
      << " accepted=" << accepted[lane] << " rejected=" << attempts[lane] - accepted[lane]);
  }
  BOOST_TEST_MESSAGE("Parser mutation manifest=" << expectedDigest << " seed=0x2124b564b564b iterations=" << iterations);
}

BOOST_AUTO_TEST_SUITE_END()
