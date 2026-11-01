#include "ndnsf-distributed-repo/RepoTypes.hpp"

#include <boost/test/unit_test.hpp>
#include <limits>

namespace {
using namespace ndnsf_distributed_repo;
const std::string grantWire = "{\"schemaVersion\":1,\"grantId\":\"g\",\"writerIdentity\":\"/provider/a\",\"objectName\":\"/producer/kv\",\"expiresAtMs\":10000}";
const std::string bindingWire = "{\"schemaVersion\":1,\"ownerIdentity\":\"/user/u\",\"grantId\":\"g\",\"ownerEpoch\":1}";
const std::string listWire = "{\"schemaVersion\":1,\"afterObjectName\":\"\",\"limit\":100}";
const std::string deleteWire = "{\"schemaVersion\":1,\"cleanupId\":\"clear\",\"objectName\":\"/producer/kv\",\"ownerEpoch\":1}";
std::string replace(std::string wire, const std::string& old, const std::string& value)
{
  const auto at = wire.find(old);
  if (at == std::string::npos) throw std::logic_error("invalid fixture mutation");
  wire.replace(at, old.size(), value);
  return wire;
}
} // namespace

BOOST_AUTO_TEST_SUITE(Spec204UserStorageWire)

BOOST_AUTO_TEST_CASE(CanonicalRequestsPreserveProducerAndExactSelectors)
{
  const auto grant = RepoUserStorageGrantRequest::fromJson(grantWire);
  BOOST_CHECK_EQUAL(grant.objectName, "/producer/kv");
  BOOST_CHECK_EQUAL(grant.writerIdentity, "/provider/a");
  BOOST_CHECK_EQUAL(grant.toJson(), grantWire);
  BOOST_CHECK_EQUAL(RepoUserStorageBinding::fromJson(bindingWire).toJson(), bindingWire);
  BOOST_CHECK_EQUAL(RepoUserStorageUsageRequest::fromJson(" { \"schemaVersion\" : 1 } ").toJson(), "{\"schemaVersion\":1}");
  BOOST_CHECK_EQUAL(RepoUserStorageListRequest::fromJson(listWire).toJson(), listWire);
  BOOST_CHECK_EQUAL(RepoUserStorageDeleteRequest::fromJson(deleteWire).toJson(), deleteWire);
  const std::string target = "{\"schemaVersion\":1,\"target\":{\"repoNode\":\"/repo/a\",\"providerIdentity\":\"/repo-provider/a\",\"servicePrefix\":\"/service/repo\"},\"binding\":" + bindingWire + ",\"writerIdentity\":\"/provider/a\"}";
  const auto parsed = RepoUserStorageTargetBinding::fromJson(target);
  BOOST_CHECK_EQUAL(parsed.toJson(), target);
  BOOST_CHECK_EQUAL(parsed.target.providerIdentity, "/repo-provider/a");
  BOOST_CHECK_EQUAL(parsed.binding.ownerIdentity, "/user/u");
  BOOST_CHECK_EQUAL(parsed.writerIdentity, "/provider/a");
}

BOOST_AUTO_TEST_CASE(ControlMessagesRejectSelfAssertedAuthority)
{
  for (const auto* key : {"ownerIdentity", "repoNode", "authenticatedOwner", "authenticatedWriter", "userStorageBinding"}) {
    const auto field = std::string(",\"") + key + "\":\"/attacker\"}";
    BOOST_CHECK_THROW(RepoUserStorageGrantRequest::fromJson(grantWire.substr(0, grantWire.size()-1) + field), std::invalid_argument);
    BOOST_CHECK_THROW(RepoUserStorageUsageRequest::fromJson("{\"schemaVersion\":1" + field), std::invalid_argument);
    BOOST_CHECK_THROW(RepoUserStorageListRequest::fromJson(listWire.substr(0, listWire.size()-1) + field), std::invalid_argument);
    BOOST_CHECK_THROW(RepoUserStorageDeleteRequest::fromJson(deleteWire.substr(0, deleteWire.size()-1) + field), std::invalid_argument);
  }
}

BOOST_AUTO_TEST_CASE(StrictSchemaRejectsAmbiguousAndMalformedJson)
{
  for (const auto& wire : {"{}", "[]", "null", "{\"schemaVersion\":1,\"schemaVersion\":1}",
       "{\"schemaVersion\":1,\"schema\\u0056ersion\":1}", "{\"schemaVersion\":true}",
       "{\"schemaVersion\":-1}", "{\"schemaVersion\":1.0}", "{\"schemaVersion\":\"1\"}",
       "{\"schemaVersion\":2}", "{\"schemaVersion\":4294967297}", "{\"schemaVersion\":1} trailing"})
    BOOST_CHECK_THROW(RepoUserStorageUsageRequest::fromJson(wire), std::invalid_argument);
  BOOST_CHECK_THROW(RepoUserStorageGrantRequest::fromJson(replace(grantWire, "\"grantId\":\"g\",", "")), std::invalid_argument);
  BOOST_CHECK_THROW(RepoUserStorageDeleteRequest::fromJson(replace(deleteWire, "\"clear\"", "null")), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(EpochExpiryAndListBoundsRejectNarrowing)
{
  for (const auto* number : {"0", "-1", "true", "\"1\"", "1.5", "9223372036854775808", "18446744073709551616"}) {
    BOOST_CHECK_THROW(RepoUserStorageGrantRequest::fromJson(replace(grantWire, "10000", number)), std::invalid_argument);
    BOOST_CHECK_THROW(RepoUserStorageBinding::fromJson(replace(bindingWire, "\"ownerEpoch\":1", std::string("\"ownerEpoch\":") + number)), std::invalid_argument);
    BOOST_CHECK_THROW(RepoUserStorageDeleteRequest::fromJson(replace(deleteWire, "\"ownerEpoch\":1", std::string("\"ownerEpoch\":") + number)), std::invalid_argument);
  }
  for (const auto* number : {"0", "1001", "4294967396", "18446744073709551615"})
    BOOST_CHECK_THROW(RepoUserStorageListRequest::fromJson(replace(listWire, "100", number)), std::invalid_argument);
  BOOST_CHECK_EQUAL(RepoUserStorageListRequest::fromJson(replace(listWire, "100", "1000")).limit, 1000U);
  BOOST_CHECK_EQUAL(RepoUserStorageGrantRequest::fromJson(replace(grantWire, "10000", "9223372036854775807")).expiresAtMs, 9223372036854775807ULL);
}

BOOST_AUTO_TEST_CASE(NamesIdsAndOutboundMessagesUseTheSameRestrictions)
{
  for (const auto* name : {"", "/", "relative", "/producer//kv", "/producer/%6Bv", "/producer/kv/"}) {
    BOOST_CHECK_THROW(RepoUserStorageGrantRequest::fromJson(replace(grantWire, "/producer/kv", name)), std::invalid_argument);
    RepoUserStorageDeleteRequest request{1, "clear", name, 1};
    BOOST_CHECK_THROW(request.toJson(), std::invalid_argument);
  }
  BOOST_CHECK_THROW(RepoUserStorageGrantRequest::fromJson(replace(grantWire, "\"g\"", "\"g\\u0000\"")), std::invalid_argument);
  RepoUserStorageGrantRequest grant{1, std::string(256, 'g'), "/provider/a", "/producer/kv", 1};
  BOOST_CHECK_EQUAL(RepoUserStorageGrantRequest::fromJson(grant.toJson()).grantId.size(), 256U);
  grant.grantId += 'g';
  BOOST_CHECK_THROW(grant.toJson(), std::invalid_argument);
  grant.grantId = "escaped\"\\\n";
  BOOST_CHECK_EQUAL(RepoUserStorageGrantRequest::fromJson(grant.toJson()).grantId, grant.grantId);
  grant.schemaVersion = 2;
  BOOST_CHECK_THROW(grant.toJson(), std::invalid_argument);
  RepoUserStorageUsageRequest usage{2};
  BOOST_CHECK_THROW(usage.toJson(), std::invalid_argument);
  RepoUserStorageListRequest list{1, "", 1001};
  BOOST_CHECK_THROW(list.toJson(), std::invalid_argument);
  RepoUserStorageBinding binding{1, "/user/u", "g", std::numeric_limits<uint64_t>::max()};
  BOOST_CHECK_THROW(binding.toJson(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(NestedBindingCannotHideExtraAuthorityOrSchema)
{
  const std::string target = "{\"schemaVersion\":1,\"target\":{\"repoNode\":\"/repo/a\",\"providerIdentity\":\"/repo-provider/a\",\"servicePrefix\":\"/service/repo\"},\"binding\":" + bindingWire + ",\"writerIdentity\":\"/provider/a\"}";
  BOOST_CHECK_THROW(RepoUserStorageTargetBinding::fromJson(replace(target, "\"repoNode\":\"/repo/a\"", "\"repoNode\":\"/repo/a\",\"authenticatedWriter\":\"/attacker\"")), std::invalid_argument);
  BOOST_CHECK_THROW(RepoUserStorageTargetBinding::fromJson(replace(target, "\"ownerIdentity\":\"/user/u\"", "\"ownerIdentity\":\"/user/u\",\"ownerIdentity\":\"/attacker\"")), std::invalid_argument);
  BOOST_CHECK_THROW(RepoUserStorageTargetBinding::fromJson(replace(target, bindingWire, replace(bindingWire, "\"schemaVersion\":1", "\"schemaVersion\":4294967297"))), std::invalid_argument);
  BOOST_CHECK_THROW(RepoUserStorageTargetBinding::fromJson(replace(target, bindingWire, "null")), std::invalid_argument);
  BOOST_CHECK_THROW(RepoUserStorageTargetBinding::fromJson(replace(target, "\"writerIdentity\":\"/provider/a\"", "\"writerIdentity\":null")), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(RequestWireBudgetIsCheckedBeforeParsing)
{
  const auto exact = "{\"schemaVersion\":1}" + std::string(65536-19, ' ');
  BOOST_CHECK_EQUAL(RepoUserStorageUsageRequest::fromJson(exact).schemaVersion, 1U);
  BOOST_CHECK_THROW(RepoUserStorageUsageRequest::fromJson(exact + ' '), std::invalid_argument);
  RepoUserStorageGrantRequest request{1, "g", "/provider/a", "/" + std::string(4095, 'k'), 1};
  BOOST_CHECK_EQUAL(RepoUserStorageGrantRequest::fromJson(request.toJson()).objectName.size(), 4096U);
  request.objectName += 'k';
  BOOST_CHECK_THROW(request.toJson(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(GrantRepliesBindAllRequestAndOwnerSelectors)
{
  const RepoArtifactTarget target{"/repo/a", "/repo-provider/a", "/service/repo"};
  const RepoUserStorageGrantRequest request{1, "g", "/provider/a", "/producer/kv", 10000};
  RepoUserStorageGrantReply reply{1, {"g", "/repo/a", "/user/u", "/provider/a", "/producer/kv", 1, 10000, 1000}};
  const auto wire = reply.toJson();
  BOOST_CHECK_EQUAL(RepoUserStorageGrantReply::fromJson(wire).grant.ownerEpoch, 1U);
  BOOST_CHECK_NO_THROW(reply.validateExpected(target, "/user/u", request));
  for (unsigned field = 0; field != 6; ++field) {
    auto wrong = reply;
    if (field == 0) wrong.grant.repoNode = "/repo/b";
    if (field == 1) wrong.grant.ownerIdentity = "/user/v";
    if (field == 2) wrong.grant.grantId = "other";
    if (field == 3) wrong.grant.writerIdentity = "/provider/b";
    if (field == 4) wrong.grant.objectName = "/producer/other";
    if (field == 5) ++wrong.grant.expiresAtMs;
    BOOST_CHECK_THROW(wrong.validateExpected(target, "/user/u", request), std::invalid_argument);
  }
  BOOST_CHECK_THROW(RepoUserStorageGrantReply::fromJson(replace(wire, "\"ownerEpoch\":1", "\"ownerEpoch\":0")), std::invalid_argument);
  BOOST_CHECK_THROW(RepoUserStorageGrantReply::fromJson(replace(wire, "\"createdAtMs\":1000", "\"createdAtMs\":10000")), std::invalid_argument);
  BOOST_CHECK_THROW(RepoUserStorageGrantReply::fromJson(replace(wire, "\"grantId\":\"g\"", "\"grantId\":\"g\",\"authenticatedOwner\":\"/attacker\"")), std::invalid_argument);
  BOOST_CHECK_THROW(RepoUserStorageGrantReply::fromJson(replace(wire, "\"ownerEpoch\":1", "\"ownerEpoch\":1,\"ownerEpoch\":2")), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(UsageRepliesPreserveMeasuredOverLimitCountsAndRejectWrongAccount)
{
  const RepoArtifactTarget target{"/repo/a", "/repo-provider/a", "/service/repo"};
  RepoUserStorageUsageReply reply{1, {"/repo/a", "/user/u", 1000000000ULL, 999999999ULL, 2}};
  const std::string expected = "{\"schemaVersion\":1,\"usage\":{\"repoNode\":\"/repo/a\",\"ownerIdentity\":\"/user/u\",\"limitBytes\":1000000000,\"committedBytes\":999999999,\"reservedBytes\":2}}";
  BOOST_CHECK_EQUAL(reply.toJson(), expected);
  const auto parsed = RepoUserStorageUsageReply::fromJson(expected);
  BOOST_CHECK_EQUAL(parsed.usage.committedBytes, 999999999ULL);
  BOOST_CHECK_EQUAL(parsed.usage.reservedBytes, 2U);
  BOOST_CHECK_NO_THROW(parsed.validateExpected(target, "/user/u"));
  BOOST_CHECK_THROW(parsed.validateExpected(target, "/user/v"), std::invalid_argument);
  auto other = target; other.repoNode = "/repo/b";
  BOOST_CHECK_THROW(parsed.validateExpected(other, "/user/u"), std::invalid_argument);
  BOOST_CHECK_THROW(RepoUserStorageUsageReply::fromJson(replace(expected, "1000000000", "1073741824")), std::invalid_argument);
  BOOST_CHECK_THROW(RepoUserStorageUsageReply::fromJson(replace(expected, "\"reservedBytes\":2", "\"reservedBytes\":9223372036854775808")), std::invalid_argument);
  BOOST_CHECK_THROW(RepoUserStorageUsageReply::fromJson(replace(expected, "\"reservedBytes\":2", "\"reservedBytes\":-1")), std::invalid_argument);
  BOOST_CHECK_THROW(RepoUserStorageUsageReply::fromJson(replace(expected, "\"reservedBytes\":2", "\"reservedBytes\":null")), std::invalid_argument);
  BOOST_CHECK_THROW(RepoUserStorageUsageReply::fromJson(replace(expected, "\"reservedBytes\":2", "\"reservedBytes\":2,\"extra\":1")), std::invalid_argument);
}

BOOST_AUTO_TEST_SUITE_END()
