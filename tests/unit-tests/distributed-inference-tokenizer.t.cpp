#include "NDNSF-DistributedInference/cpp/adapters/qwen/NativeTokenizer.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeStandaloneTokenizer.hpp"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/test/unit_test.hpp>
#include <openssl/sha.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace {
const auto STABLE_VECTORS = std::filesystem::path(
  "tests/fixtures/spec182/dependency-probes/tokenizer/stable-vectors.json");

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

struct StableFixture
{
  std::string name;
  std::string profile;
  std::string tokenizerJson;
  std::string sha256;
  boost::property_tree::ptree cases;
};

std::vector<StableFixture>
loadStableFixtures()
{
  boost::property_tree::ptree root;
  boost::property_tree::read_json(STABLE_VECTORS.string(), root);
  std::vector<StableFixture> fixtures;
  for (const auto& entry : root.get_child("fixtures")) {
    const auto& node = entry.second;
    StableFixture fixture;
    fixture.name = node.get<std::string>("name");
    fixture.profile = node.get<std::string>("profile");
    fixture.tokenizerJson = node.get<std::string>("tokenizerJson");
    fixture.sha256 = node.get<std::string>("sha256");
    fixture.cases = node.get_child("cases");
    fixtures.push_back(std::move(fixture));
  }
  return fixtures;
}

/** Write the frozen tokenizer bytes to a fresh temp file and verify them. */
std::string
materializeStableFixture(const StableFixture& fixture)
{
  const auto directory =
    std::filesystem::temp_directory_path() /
    ("spec182-tokenizer-stable-" + std::to_string(::getpid()));
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

std::vector<std::string>
toStringVector(const boost::property_tree::ptree& node)
{
  std::vector<std::string> result;
  for (const auto& entry : node) {
    result.push_back(entry.second.get_value<std::string>());
  }
  return result;
}

ndnsf::di::qwen::NativeTokenizer
openStableFixture(const StableFixture& fixture)
{
  return ndnsf::di::qwen::NativeTokenizer(
    materializeStableFixture(fixture), "sha256:" + fixture.sha256);
}

const StableFixture&
fixtureNamed(const std::vector<StableFixture>& fixtures,
             const std::string& name)
{
  for (const auto& fixture : fixtures) {
    if (fixture.name == name) {
      return fixture;
    }
  }
  BOOST_FAIL("frozen fixture " + name + " is missing");
}

/**
 * T007-B: every row freezes, for one id list, the per-prefix stable text of
 * the real native ABI (statically linked pinned Rust tokenizers):
 *   - decodeStable(ids[..cut], skip, final=false) == frozen prefix
 *   - decodeStable(ids, skip, final=true) == frozen finalText
 *   - decode(ids, skip) == frozen finalText          (final == full)
 *   - every frozen prefix is a text prefix of the final text (no revision
 *     ever rewrites committed output)
 */
void
compareStableRow(const StableFixture& fixture,
                 const ndnsf::di::qwen::NativeTokenizer& tokenizer,
                 const boost::property_tree::ptree& row)
{
  const auto skip = row.get<bool>("skip");
  const auto ids = toI64Vector(row.get_child("ids"));
  const auto prefixes = toStringVector(row.get_child("prefixes"));
  const auto finalText = row.get<std::string>("finalText");
  BOOST_REQUIRE_EQUAL(prefixes.size(), ids.size());
  for (std::size_t cut = 0; cut != ids.size(); ++cut) {
    const std::vector<std::int64_t> prefix(ids.begin(),
                                           ids.begin() + static_cast<long>(cut + 1));
    const auto stable =
      tokenizer.decodeStable(prefix, skip, false);
    BOOST_REQUIRE_EQUAL(stable, prefixes.at(cut));
  }
  BOOST_REQUIRE_EQUAL(tokenizer.decodeStable(ids, skip, true), finalText);
  BOOST_REQUIRE_EQUAL(tokenizer.decode(ids, skip), finalText);
  for (const auto& prefix : prefixes) {
    BOOST_REQUIRE_EQUAL(finalText.compare(
                          0, prefix.size(), prefix),
                        0);
  }
}
} // namespace

// T007-B: the stable-prefix contract of native-token-stream-design.md
// ("Stable Prefix Algorithms").  The frozen vectors were authored against
// HF tokenizers 0.20.3 by the dependency probe author-stable-vectors.py:
// ByteFallback expectations come from HF's own closed-part decode, ByteLevel
// non-final text from a byte-granular incremental UTF-8 stream (final=false)
// whose full flush was asserted equal to HF decode before freezing, and the
// null/WordPiece profiles from HF prefix decode.  Every row is re-asserted
// here through decodeStable() on the native ABI; the unsupported Fuse profile
// is asserted to fail closed on stable calls only.
BOOST_AUTO_TEST_SUITE(Spec182TokenizerStable)

BOOST_AUTO_TEST_CASE(FixtureListIsFrozen)
{
  const auto fixtures = loadStableFixtures();
  BOOST_REQUIRE_EQUAL(fixtures.size(), std::size_t(5));
  const std::vector<std::string> names = {
    "byte-fallback-special", "bytelevel", "reject-fuse",
    "legacy-ascii", "legacy-unicode"};
  const std::vector<std::string> profiles = {
    "bytefallback", "bytelevel", "unsupported", "null", "wordpiece"};
  const std::vector<std::size_t> counts = {11, 13, 0, 2, 2};
  for (std::size_t index = 0; index != fixtures.size(); ++index) {
    BOOST_REQUIRE_EQUAL(fixtures.at(index).name, names.at(index));
    BOOST_REQUIRE_EQUAL(fixtures.at(index).profile, profiles.at(index));
    BOOST_REQUIRE_EQUAL(fixtures.at(index).cases.size(), counts.at(index));
    BOOST_REQUIRE(!fixtures.at(index).sha256.empty());
  }
  // Frozen artifacts: the byte-fallback-special, legacy-ascii and
  // legacy-unicode tokenizer bodies are re-embedded verbatim from the T007-A
  // vectors.json container and must keep their T007-A digests; the synthetic
  // ByteLevel fixture and the Fuse reject fixture are frozen by the
  // authoring script's own recorded digests (asserted above by materialize).
  const auto& bytefallback = fixtureNamed(fixtures, "byte-fallback-special");
  BOOST_REQUIRE_EQUAL(bytefallback.sha256,
    "3aef42a9cf6eb91f711bd2fbc3c885e29a9b8b44945c5ab050216369f6629a5f");
  const auto& bytelevel = fixtureNamed(fixtures, "bytelevel");
  BOOST_REQUIRE_EQUAL(bytelevel.sha256,
    "bef4550bb44a0f02d2fcd0c02dc2aac71419081f25da43c1c0bfe4a847c001f4");
  const auto& reject = fixtureNamed(fixtures, "reject-fuse");
  BOOST_REQUIRE_EQUAL(reject.sha256,
    "cb285631df27bba0cd7564f4213ff9f3f77bed2261da0cbb81ee8eb5b458b4e3");
  const auto& ascii = fixtureNamed(fixtures, "legacy-ascii");
  BOOST_REQUIRE_EQUAL(ascii.sha256,
    "bf0f0fa65dc5aafe690ee497b4c8e2abe408fb4788c5ef035964dc94f15ca5a6");
  const auto& unicode = fixtureNamed(fixtures, "legacy-unicode");
  BOOST_REQUIRE_EQUAL(unicode.sha256,
    "90db6ef1a0f74a22133269b170a5a4c08a5a4c0d5a76a746614f779e5b2f2404");
}

BOOST_AUTO_TEST_CASE(ByteFallbackSpecialPrefixesMatchFrozenReference)
{
  const auto fixtures = loadStableFixtures();
  const auto& fixture = fixtureNamed(fixtures, "byte-fallback-special");
  const auto tokenizer = openStableFixture(fixture);
  BOOST_REQUIRE_EQUAL(tokenizer.digest(), "sha256:" + fixture.sha256);
  // Rows 0-8 skip specials; rows 9-10 retain them (retained specials close
  // runs).  openRunHoldsEvenValidPrefix pins the hold-even-if-valid rule and
  // legalReplacementFlushOnFinal pins the never-delete-legal-U+FFFD rule.
  for (const auto& row : fixture.cases) {
    compareStableRow(fixture, tokenizer, row.second);
  }
}

BOOST_AUTO_TEST_CASE(ByteLevelPrefixesMatchFrozenReference)
{
  const auto fixtures = loadStableFixtures();
  const auto& fixture = fixtureNamed(fixtures, "bytelevel");
  const auto tokenizer = openStableFixture(fixture);
  BOOST_REQUIRE_EQUAL(tokenizer.digest(), "sha256:" + fixture.sha256);
  // Rows 0-11 skip specials; row 12 retains them.  The suite covers complete
  // characters split across tokens, truncated leads/continuations held and
  // flushed, lone invalid bytes, boundary-breaking continuations, surrogate
  // sequences, legal U+FFFD input kept verbatim, whole raw fallback tokens
  // and the alphabet-char <-> byte mapping on retained specials.
  for (const auto& row : fixture.cases) {
    compareStableRow(fixture, tokenizer, row.second);
  }
}

BOOST_AUTO_TEST_CASE(LegacyAsciiNullPrefixesMatchFrozenReference)
{
  const auto fixtures = loadStableFixtures();
  const auto& fixture = fixtureNamed(fixtures, "legacy-ascii");
  const auto tokenizer = openStableFixture(fixture);
  BOOST_REQUIRE_EQUAL(tokenizer.digest(), "sha256:" + fixture.sha256);
  // Null-decoder dispatch: full HF decode of the prefix is already stable
  // (contents joined with spaces), identical for both skip flags.
  for (const auto& row : fixture.cases) {
    compareStableRow(fixture, tokenizer, row.second);
  }
}

BOOST_AUTO_TEST_CASE(LegacyUnicodeWordPiecePrefixesMatchFrozenReference)
{
  const auto fixtures = loadStableFixtures();
  const auto& fixture = fixtureNamed(fixtures, "legacy-unicode");
  const auto tokenizer = openStableFixture(fixture);
  BOOST_REQUIRE_EQUAL(tokenizer.digest(), "sha256:" + fixture.sha256);
  // WordPiece-decoder dispatch: the real decoder concatenates contents
  // without spaces, which distinguishes this profile from the null decoder
  // row above (dispatch is per-profile, not per-fallback).
  for (const auto& row : fixture.cases) {
    compareStableRow(fixture, tokenizer, row.second);
  }
}

BOOST_AUTO_TEST_CASE(EmptyIdsAreStableOnEverySupportedProfile)
{
  const auto fixtures = loadStableFixtures();
  for (const auto& name : {"byte-fallback-special", "bytelevel",
                           "legacy-ascii", "legacy-unicode"}) {
    const auto& fixture = fixtureNamed(fixtures, name);
    const auto tokenizer = openStableFixture(fixture);
    BOOST_REQUIRE_EQUAL(tokenizer.decodeStable({}, true, false), "");
    BOOST_REQUIRE_EQUAL(tokenizer.decodeStable({}, false, false), "");
    BOOST_REQUIRE_EQUAL(tokenizer.decodeStable({}, true, true), "");
    BOOST_REQUIRE_EQUAL(tokenizer.decodeStable({}, false, true), "");
  }
}

BOOST_AUTO_TEST_CASE(InterleavedStableCallsAreStateless)
{
  const auto fixtures = loadStableFixtures();
  // Two held-byte profiles with conflicting rules on the same owner: no call
  // may leak state into the next (each result derives from its own id list
  // alone), and results must be identical under interleaving.
  const auto& bytefallback = fixtureNamed(fixtures, "byte-fallback-special");
  const auto& bytelevel = fixtureNamed(fixtures, "bytelevel");
  const auto fallbackTokenizer = openStableFixture(bytefallback);
  const auto bytelevelTokenizer = openStableFixture(bytelevel);

  // (owner, case index, final flag) interleave schedule.
  struct Step
  {
    const ndnsf::di::qwen::NativeTokenizer* owner;
    int index;
    bool finalFlag;
  };
  std::vector<Step> schedule;
  const int fallbackRows = 11;
  const int bytelevelRows = 13;
  for (int step = 0; step != 12; ++step) {
    Step onFallback{&fallbackTokenizer, step % fallbackRows, step % 3 == 0};
    Step onByteLevel{&bytelevelTokenizer, (step * 5 + 3) % bytelevelRows,
                     step % 2 == 0};
    schedule.push_back(onFallback);
    schedule.push_back(onByteLevel);
  }
  auto collect = [&]() {
    std::vector<std::string> results;
    for (const auto& call : schedule) {
      int seen = 0;
      const auto& rows = call.owner == &fallbackTokenizer
        ? bytefallback.cases : bytelevel.cases;
      for (const auto& row : rows) {
        if (seen != call.index) {
          ++seen;
          continue;
        }
        const auto skip = row.second.get<bool>("skip");
        const auto ids = toI64Vector(row.second.get_child("ids"));
        results.push_back(call.owner->decodeStable(ids, skip, call.finalFlag));
        break;
      }
    }
    return results;
  };
  const auto firstPass = collect();
  const auto secondPass = collect();
  BOOST_REQUIRE_EQUAL_COLLECTIONS(firstPass.begin(), firstPass.end(),
                                  secondPass.begin(), secondPass.end());
}

BOOST_AUTO_TEST_CASE(RejectFuseFailsClosedForStableOnly)
{
  const auto fixtures = loadStableFixtures();
  const auto& fixture = fixtureNamed(fixtures, "reject-fuse");
  const auto tokenizer = openStableFixture(fixture);
  BOOST_REQUIRE_EQUAL(tokenizer.digest(), "sha256:" + fixture.sha256);
  // An unknown decoder pipeline must not guess a prefix-stability property:
  // every stable call fails closed, on empty ids and final flags included.
  BOOST_CHECK_THROW(tokenizer.decodeStable({}, true, false),
                    std::runtime_error);
  BOOST_CHECK_THROW(tokenizer.decodeStable({}, false, true),
                    std::runtime_error);
  BOOST_CHECK_THROW(tokenizer.decodeStable({0}, true, false),
                    std::runtime_error);
  BOOST_CHECK_THROW(tokenizer.decodeStable({0}, false, true),
                    std::runtime_error);
  // Full decode and encode are unaffected by the stable rejection.
  BOOST_REQUIRE(!tokenizer.decode({0}, false).empty());
  BOOST_REQUIRE_EQUAL(tokenizer.decode({}, true), "");
}

BOOST_AUTO_TEST_CASE(StableRejectsUnknownAndOutOfRangeIds)
{
  const auto fixtures = loadStableFixtures();
  const auto& fixture = fixtureNamed(fixtures, "byte-fallback-special");
  const auto tokenizer = openStableFixture(fixture);
  // Negative and beyond-u32 ids are rejected before the ABI sees them.
  BOOST_CHECK_THROW(tokenizer.decodeStable({std::int64_t(-1)}, true, false),
                    std::invalid_argument);
  BOOST_CHECK_THROW(
    tokenizer.decodeStable({std::int64_t(4294967296LL)}, true, false),
    std::invalid_argument);
  // An in-range id the frozen vocabulary does not contain is an ABI error on
  // the stable path as well, never a guessed text result.
  BOOST_CHECK_THROW(tokenizer.decodeStable({std::int64_t(4000000000LL)}, true,
                                           false),
                    std::runtime_error);
  BOOST_CHECK_THROW(tokenizer.decodeStable({std::int64_t(4000000000LL)}, true,
                                           true),
                    std::runtime_error);
}

BOOST_AUTO_TEST_SUITE_END()
