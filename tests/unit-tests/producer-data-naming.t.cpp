#include "tests/boost-test.hpp"
#include "ndn-service-framework/ProducerDataNaming.hpp"

#include <iostream>
#include <limits>
#include <stdexcept>

namespace ndn_service_framework::test {
namespace {

std::vector<uint8_t> raw(const std::string& value)
{
  return {value.begin(), value.end()};
}

std::vector<uint8_t> nameWire(const ndn::Name& name)
{
  const auto& wire = name.wireEncode();
  return {wire.begin(), wire.end()};
}

std::string hex(const ndn::Block& block)
{
  constexpr char DIGITS[] = "0123456789abcdef";
  std::string result;
  for (auto p = block.begin(); p != block.end(); ++p) {
    result.push_back(DIGITS[*p >> 4]);
    result.push_back(DIGITS[*p & 15]);
  }
  return result;
}

} // namespace

BOOST_AUTO_TEST_SUITE(ProducerDataNaming)

BOOST_AUTO_TEST_CASE(ExactProducerBoundary)
{
  const ndn::Name producer("/org/NDNSF/TENSOR/provider");
  auto name = makeProducerDataPrefix(producer, "TENSOR").append("MANIFEST");
  const auto parsed = parseProducerDataName(name, producer);
  BOOST_REQUIRE(parsed);
  BOOST_CHECK(parsed->producer == producer);
  BOOST_CHECK_EQUAL(parsed->category, "TENSOR");
  BOOST_CHECK(parsed->suffix == ndn::Name("/MANIFEST"));
  BOOST_CHECK(!parseProducerDataName(name, ndn::Name("/org/NDNSF")));
  BOOST_CHECK(!parseProducerDataName(name, ndn::Name("/other/provider")));
  BOOST_CHECK(!parseProducerDataName(makeProducerDataPrefix(producer, "TENSOR"), producer));
  BOOST_CHECK(!parseProducerDataName(name, ndn::Name()));
  BOOST_CHECK_THROW(makeProducerDataPrefix(ndn::Name(), "MODEL"), std::invalid_argument);
  BOOST_CHECK_THROW(makeProducerDataPrefix(producer, "MODEL/OTHER"), std::invalid_argument);
  BOOST_CHECK_THROW(makeProducerDataPrefix(producer, "model"), std::invalid_argument);
  BOOST_CHECK_EQUAL(name.size(), producer.size() + 3); // No naming path version.
}

BOOST_AUTO_TEST_CASE(FullDigestAndDecimalWire)
{
  const auto digest = makeNamingDigestComponent(std::string(64, 'a'));
  BOOST_CHECK_EQUAL(digest.type(), ndn::tlv::GenericNameComponent);
  BOOST_CHECK_EQUAL(digest.value_size(), 32);
  for (size_t i = 0; i < digest.value_size(); ++i) BOOST_CHECK_EQUAL(digest.value()[i], 0xaa);
  BOOST_CHECK_THROW(makeNamingDigestComponent(std::string(63, 'a')), std::invalid_argument);
  BOOST_CHECK_THROW(makeNamingDigestComponent(std::string(64, 'A')), std::invalid_argument);
  BOOST_CHECK_THROW(makeNamingDigestComponent("sha256:" + std::string(64, 'a')), std::invalid_argument);
  for (const auto value : {uint64_t(0), uint64_t(1), uint64_t(255), uint64_t(256),
                          std::numeric_limits<uint64_t>::max()}) {
    BOOST_CHECK(makeNamingDecimalComponent(value) == ndn::name::Component(std::to_string(value)));
  }
  BOOST_CHECK(makeNamingDecimalComponent(256) != ndn::name::Component::fromNumber(256));
}

BOOST_AUTO_TEST_CASE(ReadableSemanticNames)
{
  const ndn::Name producer("/org/NDNSF-DI/model/producer");
  const auto name = makeProducerDataPrefix(producer, "NDNSF-DI", "model")
    .append(makeNamingLabelComponent(u8"模型/qwen=α"))
    .append(makeNamingDigestComponent(std::string(64, 'a')));
  const auto parsed = parseProducerDataName(name, producer);
  BOOST_REQUIRE(parsed);
  BOOST_CHECK_EQUAL(parsed->nameSpace, "NDNSF-DI");
  BOOST_CHECK_EQUAL(parsed->category, "model");
  BOOST_CHECK_EQUAL(parsed->suffix.size(), 2);
  BOOST_CHECK(matchesNamingLabelComponent(parsed->suffix[0], u8"模型/qwen=α"));
  BOOST_CHECK(!matchesNamingLabelComponent(parsed->suffix[0], "qwen"));
  BOOST_CHECK_THROW(makeProducerDataPrefix(producer, "NDNSF-DI", "BLOB"), std::invalid_argument);
  BOOST_CHECK_THROW(makeProducerDataPrefix(producer, "unknown", "model"), std::invalid_argument);
  BOOST_CHECK_THROW(makeNamingLabelComponent(""), std::invalid_argument);
  BOOST_CHECK_THROW(makeNamingLabelComponent("\xc0\x80"), std::invalid_argument);
  BOOST_CHECK_THROW(makeNamingLabelComponent("\xc2\x85"), std::invalid_argument);
  const std::string prefix(63, 'a');
  BOOST_CHECK(makeNamingLabelComponent(prefix + u8"模suffix") == ndn::name::Component("name=" + prefix));
  BOOST_CHECK(makeNamingLabelComponent(std::string(64, 'a')) ==
              ndn::name::Component("name=" + std::string(64, 'a')));
  std::cout << "SPEC201_LABEL " << hex(name.wireEncode()) << std::endl;
}

BOOST_AUTO_TEST_CASE(TaggedTranscriptAndValidation)
{
  const std::vector<NamingDigestField> fields{{1, 2, raw("ab")}, {2, 4, raw("a:b")}};
  BOOST_CHECK_EQUAL(computeNamingDigest("ndnsf-naming/test", fields),
                   "c63576ff38a6ae9c6393514d7dd86b57957ad53d8ba67e987e21de4c1d288240");
  BOOST_CHECK(computeNamingDigest("other", fields) != computeNamingDigest("ndnsf-naming/test", fields));
  BOOST_CHECK(computeNamingDigest("d", {{1, 2, raw("ab")}, {2, 2, raw("c")}}) !=
              computeNamingDigest("d", {{1, 2, raw("a")}, {2, 2, raw("bc")}}));
  BOOST_CHECK_THROW(computeNamingDigest("", fields), std::invalid_argument);
  BOOST_CHECK_THROW(computeNamingDigest(std::string(65536, 'x'), fields), std::invalid_argument);
  BOOST_CHECK_THROW(computeNamingDigest("d", std::vector<NamingDigestField>(65536)), std::invalid_argument);
  BOOST_CHECK_THROW(computeNamingDigest("d", {{1, 4, {}}, {1, 4, {}}}), std::invalid_argument);
  BOOST_CHECK_THROW(computeNamingDigest("d", {{2, 4, {}}, {1, 4, {}}}), std::invalid_argument);
  BOOST_CHECK_THROW(computeNamingDigest("d", {{1, 6, {}}}), std::invalid_argument);
  BOOST_CHECK_THROW(computeNamingDigest("d", {{1, 3, {0}}}), std::invalid_argument);
  BOOST_CHECK_THROW(computeNamingDigest("d", {{1, 1, {7, 1}}}), std::invalid_argument);
  BOOST_CHECK_THROW(computeNamingDigest("d", {{1, 2, {0xc0, 0x80}}}), std::invalid_argument);
  BOOST_CHECK_THROW(computeNamingDigest("d", {{1, 5, {0xed, 0xa0, 0x80}}}), std::invalid_argument);
  auto incomplete = nameWire(ndn::Name("/x"));
  incomplete.push_back(0);
  BOOST_CHECK_THROW(computeNamingDigest("d", {{1, 1, incomplete}}), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(ReversibleCanonicalRoutingFields)
{
  auto name = ndn::Name("/full/service").appendVersion(257).appendSegment(0);
  name.append(ndn::name::Component(".")).append(ndn::name::Component(""));
  const auto component = makeReversibleNameComponent(name);
  const auto parsed = parseReversibleNameComponent(component);
  BOOST_REQUIRE(parsed);
  BOOST_CHECK(parsed->wireEncode() == name.wireEncode());
  BOOST_CHECK(!parseReversibleNameComponent(ndn::name::Component::fromSegment(0)));
  BOOST_CHECK(!parseReversibleNameComponent(ndn::name::Component("/service/v=257")));
  BOOST_CHECK(!parseReversibleNameComponent(ndn::name::Component("/service/%61")));
  BOOST_CHECK(!parseReversibleNameComponent(ndn::name::Component("not-a-name")));
  BOOST_REQUIRE(parseReversibleNameComponent(makeReversibleNameComponent(ndn::Name())));
}

BOOST_AUTO_TEST_CASE(WireBudgetsAndIndependentRpc)
{
  const auto name = makeProducerDataPrefix(ndn::Name("/producer"), "MODEL").append(std::string(1024, 'x'));
  const auto size = name.wireEncode().size();
  BOOST_CHECK_NO_THROW(checkNamingNameSize(name, size));
  BOOST_CHECK_THROW(checkNamingNameSize(name, size - 1), std::length_error);
  BOOST_CHECK_THROW(checkNamingNameSize(name), std::length_error);
  BOOST_CHECK_THROW(checkNamingNameSize(name, 0), std::invalid_argument);
  for (const size_t budget : {size_t(512), size_t(1024)}) {
    // Name header=4, /p component=3, large Generic component header=4.
    const auto boundary = ndn::Name("/p").append(ndn::name::Component(std::string(budget - 11, 'x')));
    BOOST_CHECK_EQUAL(boundary.wireEncode().size(), budget);
    BOOST_CHECK_NO_THROW(checkNamingNameSize(boundary, budget));
    const auto oversized = ndn::Name("/p").append(ndn::name::Component(std::string(budget - 10, 'x')));
    BOOST_CHECK_THROW(checkNamingNameSize(oversized, budget), std::length_error);
  }
  const auto rpc = makeProducerRpcName(ndn::Name("/user"), "READY", std::string(64, '0'), "round/nonce");
  BOOST_REQUIRE(parseProducerDataName(rpc, ndn::Name("/user")));
  BOOST_CHECK_EQUAL(rpc[-1].value_size(), 11);
  BOOST_CHECK_THROW(makeProducerRpcName(ndn::Name("/user"), "READY", std::string(64, '0'), ""),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(PythonWireCorpus)
{
  for (const auto& producer : {ndn::Name("/org/provider/p0"),
                             ndn::Name("/org/NDNSF/TENSOR/provider").appendVersion(257)}) {
    const std::vector<NamingDigestField> fields{
      {1, 1, nameWire(producer)}, {2, 2, raw(u8"模型/α")},
      {3, 3, std::vector<uint8_t>(8, 255)}, {4, 4, {0, 255}},
      {5, 5, raw("{\"a\":1,\"b\":\"x\"}")}};
    const auto digest = computeNamingDigest("ndnsf-naming/fixture", fields);
    const auto name = makeProducerDataPrefix(producer, "TENSOR")
      .append(makeNamingDigestComponent(digest)).appendSegment(7);
    const auto routing = ndn::Name("/full/service").appendVersion(257)
      .append(ndn::name::Component(".")).append(ndn::name::Component(""));
    std::cout << "SPEC201_WIRE " << hex(name.wireEncode()) << " " << digest << " "
              << hex(makeReversibleNameComponent(routing).wireEncode()) << std::endl;
    BOOST_CHECK(parseProducerDataName(name, producer).has_value());
  }
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace ndn_service_framework::test
