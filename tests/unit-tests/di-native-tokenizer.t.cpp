#include "NDNSF-DistributedInference/cpp/adapters/qwen/NativeTokenizer.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeStandaloneTokenizer.hpp"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/test/unit_test.hpp>
#include <openssl/sha.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
const auto FIXTURE = std::filesystem::path(
  "tests/fixtures/spec175/tiny-causal-lm-v1/standalone/tokenizer.json");
const auto VECTORS = std::filesystem::path(
  "tests/fixtures/spec182/dependency-probes/tokenizer/vectors.json");

std::string
sha256Hex(const std::string& bytes)
{
  std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
  SHA256(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(),
         digest.data());
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (const auto value : digest) {
    output << std::setw(2) << static_cast<unsigned>(value);
  }
  return output.str();
}

struct TokenizerFixture
{
  std::string name;
  std::string tokenizerJson;
  std::string sha256;
  boost::property_tree::ptree cases;
};

std::vector<TokenizerFixture>
loadFixtures()
{
  boost::property_tree::ptree root;
  boost::property_tree::read_json(VECTORS.string(), root);
  std::vector<TokenizerFixture> fixtures;
  for (const auto& entry : root.get_child("fixtures")) {
    const auto& node = entry.second;
    TokenizerFixture fixture;
    fixture.name = node.get<std::string>("name");
    fixture.tokenizerJson = node.get<std::string>("tokenizerJson");
    fixture.sha256 = node.get<std::string>("sha256");
    fixture.cases = node.get_child("cases");
    fixtures.push_back(std::move(fixture));
  }
  return fixtures;
}

/** Write the frozen tokenizer bytes to a fresh temp file and verify them. */
std::string
materializeFixture(const TokenizerFixture& fixture)
{
  const auto directory =
    std::filesystem::temp_directory_path() /
    ("spec182-tokenizer-full-" + std::to_string(::getpid()));
  std::filesystem::create_directories(directory);
  const auto file = directory / (fixture.name + ".json");
  {
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    output.write(fixture.tokenizerJson.data(),
                 static_cast<std::streamsize>(fixture.tokenizerJson.size()));
  }
  // The recorded digest is part of the frozen vectors: writing the embedded
  // bytes must reproduce it exactly.
  BOOST_REQUIRE_EQUAL(sha256Hex(fixture.tokenizerJson), fixture.sha256);
  return file.string();
}

std::vector<std::int64_t>
toI64Vector(const boost::property_tree::ptree& node)
{
  std::vector<std::int64_t> result;
  for (const auto& entry : node) {
    result.push_back(entry.second.get_value<std::int64_t>());
  }
  return result;
}

ndnsf::di::qwen::NativeTokenizer
openFixture(const TokenizerFixture& fixture)
{
  return ndnsf::di::qwen::NativeTokenizer(
    materializeFixture(fixture), "sha256:" + fixture.sha256);
}

void
compareVectorCase(const TokenizerFixture& fixture,
                  const ndnsf::di::qwen::NativeTokenizer& tokenizer,
                  const boost::property_tree::ptree& row)
{
  const auto input = row.get<std::string>("input");
  const auto addSpecial = row.get<bool>("addSpecial");
  const auto skipSpecial = row.get<bool>("skipSpecial");
  const auto ids = toI64Vector(row.get_child("ids"));
  const auto decoded = row.get<std::string>("decoded");
  // Boost 1.71 cannot print std::vector via operator<<, so compare the
  // encoded ID list elementwise instead of with REQUIRE_EQUAL.
  const auto encoded = tokenizer.encode(input, addSpecial);
  BOOST_REQUIRE_EQUAL_COLLECTIONS(ids.begin(), ids.end(),
                                  encoded.begin(), encoded.end());
  BOOST_REQUIRE_EQUAL(tokenizer.decode(ids, skipSpecial), decoded);
}
} // namespace

BOOST_AUTO_TEST_SUITE(Spec182NativeTokenizer)

BOOST_AUTO_TEST_CASE(MissingArtifactIsRejectedBeforeEngineCreate)
{
  BOOST_CHECK_THROW(
    ndnsf::di::qwen::NativeTokenizer(
      "/tmp/spec182-tokenizer-does-not-exist.json",
      "sha256:" + std::string(64, '0')),
    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(DigestMismatchIsRejectedBeforeEngineCreate)
{
  BOOST_REQUIRE(std::filesystem::is_regular_file(FIXTURE));
  BOOST_CHECK_THROW(
    ndnsf::di::qwen::NativeTokenizer(
      FIXTURE.string(), "sha256:" + std::string(64, '0')),
    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(StandaloneFactoryRequiresIdentity)
{
  BOOST_CHECK_THROW(
    ndnsf::di::makeNativeStandaloneTokenizerDecoders(
      ndnsf::di::NativeStandaloneTokenizerOptions{}, {}),
    std::invalid_argument);
}

BOOST_AUTO_TEST_SUITE_END()

// T007-A: the real native ABI (statically linked pinned Rust tokenizers) is
// the object under test.  The frozen dependency vectors (84 cases over three
// pinned tokenizer profiles: null decoder, WordPiece decoder, ByteFallback
// decoder) are compared byte-for-byte through encode()/decode() on the
// native owner, then the mandated digest / out-of-range / UTF-8 / owner
// reuse / serialized-call checks are exercised on the same ABI.
BOOST_AUTO_TEST_SUITE(Spec182TokenizerFull)

BOOST_AUTO_TEST_CASE(LegacyAsciiFullVectorsMatchFrozenReference)
{
  const auto fixtures = loadFixtures();
  BOOST_REQUIRE_EQUAL(fixtures.size(), std::size_t(3));
  const auto& fixture = fixtures.at(0);
  BOOST_REQUIRE_EQUAL(fixture.name, "legacy-ascii");
  const auto tokenizer = openFixture(fixture);
  BOOST_REQUIRE_EQUAL(tokenizer.digest(), "sha256:" + fixture.sha256);
  BOOST_REQUIRE_EQUAL(fixture.cases.size(), std::size_t(28));
  for (const auto& row : fixture.cases) {
    compareVectorCase(fixture, tokenizer, row.second);
  }
}

BOOST_AUTO_TEST_CASE(LegacyUnicodeFullVectorsMatchFrozenReference)
{
  const auto fixtures = loadFixtures();
  BOOST_REQUIRE_EQUAL(fixtures.size(), std::size_t(3));
  const auto& fixture = fixtures.at(1);
  BOOST_REQUIRE_EQUAL(fixture.name, "legacy-unicode");
  const auto tokenizer = openFixture(fixture);
  BOOST_REQUIRE_EQUAL(tokenizer.digest(), "sha256:" + fixture.sha256);
  BOOST_REQUIRE_EQUAL(fixture.cases.size(), std::size_t(28));
  for (const auto& row : fixture.cases) {
    compareVectorCase(fixture, tokenizer, row.second);
  }
}

BOOST_AUTO_TEST_CASE(ByteFallbackSpecialFullVectorsMatchFrozenReference)
{
  const auto fixtures = loadFixtures();
  BOOST_REQUIRE_EQUAL(fixtures.size(), std::size_t(3));
  const auto& fixture = fixtures.at(2);
  BOOST_REQUIRE_EQUAL(fixture.name, "byte-fallback-special");
  const auto tokenizer = openFixture(fixture);
  BOOST_REQUIRE_EQUAL(tokenizer.digest(), "sha256:" + fixture.sha256);
  BOOST_REQUIRE_EQUAL(fixture.cases.size(), std::size_t(28));
  for (const auto& row : fixture.cases) {
    compareVectorCase(fixture, tokenizer, row.second);
  }
}

BOOST_AUTO_TEST_CASE(DecodeRejectsOutOfRangeAndUnknownIds)
{
  const auto fixtures = loadFixtures();
  const auto& fixture = fixtures.at(2);
  const auto tokenizer = openFixture(fixture);
  // Negative and beyond-u32 ids are rejected before the ABI sees them.
  BOOST_CHECK_THROW(tokenizer.decode({std::int64_t(-1)}), std::invalid_argument);
  BOOST_CHECK_THROW(tokenizer.decode({std::int64_t(4294967296LL)}),
                    std::invalid_argument);
  // An in-range id that the frozen vocabulary does not contain is an ABI
  // error, not a successful text result.
  BOOST_CHECK_THROW(tokenizer.decode({std::int64_t(4000000000LL)}),
                    std::runtime_error);
}

BOOST_AUTO_TEST_CASE(EncodeRejectsInvalidUtf8AndOversizedText)
{
  const auto fixtures = loadFixtures();
  const auto& fixture = fixtures.at(0);
  const auto tokenizer = openFixture(fixture);
  const std::string invalid("\xff\xfe", 2);
  BOOST_CHECK_THROW(tokenizer.encode(invalid), std::runtime_error);
  BOOST_CHECK_THROW(
    tokenizer.encode(std::string(1024 * 1024 + 1, 'a')),
    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(OwnerReuseAcrossRepeatedCallsIsDeterministic)
{
  const auto fixtures = loadFixtures();
  const auto& fixture = fixtures.at(1);
  const auto tokenizer = openFixture(fixture);
  const auto ids = tokenizer.encode("\xe4\xbd\xa0\xe5\xa5\xbd world", false);
  BOOST_REQUIRE(!ids.empty());
  const auto skipped = tokenizer.decode(ids, true);
  const auto kept = tokenizer.decode(ids, false);
  for (int round = 0; round != 5; ++round) {
    const auto roundIds =
      tokenizer.encode("\xe4\xbd\xa0\xe5\xa5\xbd world", false);
    BOOST_REQUIRE_EQUAL_COLLECTIONS(ids.begin(), ids.end(),
                                    roundIds.begin(), roundIds.end());
    BOOST_REQUIRE_EQUAL(tokenizer.decode(ids, true), skipped);
    BOOST_REQUIRE_EQUAL(tokenizer.decode(ids, false), kept);
    BOOST_REQUIRE_EQUAL(tokenizer.digest(), "sha256:" + fixture.sha256);
  }
}

BOOST_AUTO_TEST_CASE(ConcurrentCallsSerializeOnSharedOwner)
{
  const auto fixtures = loadFixtures();
  const auto& fixture = fixtures.at(2);
  const auto tokenizer = openFixture(fixture);
  const std::vector<std::string> inputs = {"hello", "\xe4\xbd\xa0\xe5\xa5\xbd",
                                           "\xf0\x9f\x99\x82 \xe2\x82\xac"};
  std::vector<std::vector<std::int64_t>> baselineIds;
  std::vector<std::string> baselineSkipped;
  for (const auto& input : inputs) {
    const auto ids = tokenizer.encode(input, true);
    baselineIds.push_back(ids);
    baselineSkipped.push_back(tokenizer.decode(ids, true));
  }
  std::vector<std::thread> workers;
  std::atomic<bool> failed{false};
  for (int worker = 0; worker != 8; ++worker) {
    workers.emplace_back([&, worker]() {
      for (int iteration = 0; iteration != 40; ++iteration) {
        try {
          const std::size_t index = static_cast<std::size_t>(
            (worker + iteration) % static_cast<int>(inputs.size()));
          const auto ids = tokenizer.encode(inputs.at(index), true);
          if (ids != baselineIds.at(index) ||
              tokenizer.decode(ids, true) != baselineSkipped.at(index)) {
            failed.store(true);
          }
        } catch (...) {
          failed.store(true);
        }
      }
    });
  }
  for (auto& worker : workers) {
    worker.join();
  }
  BOOST_REQUIRE(!failed.load());
  BOOST_REQUIRE_EQUAL(tokenizer.decode(baselineIds.at(0), true),
                      baselineSkipped.at(0));
}

BOOST_AUTO_TEST_SUITE_END()
