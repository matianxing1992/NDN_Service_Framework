#include "tests/boost-test.hpp"

#include "ndn-service-framework/ServiceAuthorizationTable.hpp"

#include <atomic>
#include <thread>

namespace ndn_service_framework::test {

BOOST_AUTO_TEST_SUITE(ServiceAuthorizationTableTests)

namespace {

ServiceAuthorizationRecord
makeRecord(const std::string& providerService,
           const std::string& service,
           size_t kind,
           size_t epoch)
{
  return ServiceAuthorizationRecord{providerService, service, kind, epoch};
}

} // namespace

BOOST_AUTO_TEST_CASE(RejectsInvalidRecords)
{
  ServiceAuthorizationTable table;
  BOOST_CHECK(!table.upsert(ServiceAuthorizationRecord{}));
  BOOST_CHECK(!table.replacePermissions(tlv::UserPermission, 0, {}));
  BOOST_CHECK(!table.replacePermissions(9999, 1, {}));
  BOOST_CHECK(table.snapshot().empty());
}

BOOST_AUTO_TEST_CASE(ExactKindAndServiceLookup)
{
  ServiceAuthorizationTable table;
  BOOST_REQUIRE(table.upsert(
    makeRecord("/provider/HELLO", "/HELLO", tlv::UserPermission, 3)));
  BOOST_REQUIRE(table.upsert(
    makeRecord("/provider/CAMERA", "/CAMERA", tlv::ProviderPermission, 7)));

  BOOST_CHECK(table.contains("/provider/HELLO", "/HELLO", tlv::UserPermission));
  BOOST_CHECK(!table.contains("/provider/HELLO", "/OTHER", tlv::UserPermission));
  BOOST_CHECK(!table.contains("/provider/HELLO", "/HELLO", tlv::ProviderPermission));
  BOOST_CHECK(table.contains("/provider/CAMERA", "/CAMERA", tlv::ProviderPermission));
}

BOOST_AUTO_TEST_CASE(ReplacesRoleSnapshotAndRejectsOlderEpoch)
{
  ServiceAuthorizationTable table;
  BOOST_REQUIRE(table.replacePermissions(
    tlv::UserPermission, 4,
    {makeRecord("/provider/A", "/A", tlv::UserPermission, 4),
     makeRecord("/provider/B", "/B", tlv::UserPermission, 4)}));
  BOOST_REQUIRE(table.upsert(
    makeRecord("/provider/P", "/P", tlv::ProviderPermission, 2)));

  BOOST_CHECK(!table.replacePermissions(
    tlv::UserPermission, 3,
    {makeRecord("/provider/C", "/C", tlv::UserPermission, 3)}));
  BOOST_CHECK(table.contains("/provider/A", "/A", tlv::UserPermission));

  BOOST_REQUIRE(table.replacePermissions(
    tlv::UserPermission, 4,
    {makeRecord("/provider/A2", "/A2", tlv::UserPermission, 4)}));
  BOOST_CHECK(!table.find("/provider/A"));
  BOOST_CHECK(table.contains("/provider/A2", "/A2", tlv::UserPermission));

  BOOST_REQUIRE(table.replacePermissions(
    tlv::UserPermission, 5,
    {makeRecord("/provider/C", "/C", tlv::UserPermission, 5)}));
  BOOST_CHECK(!table.find("/provider/A"));
  BOOST_CHECK(table.contains("/provider/C", "/C", tlv::UserPermission));
  BOOST_CHECK(table.contains("/provider/P", "/P", tlv::ProviderPermission));
}

BOOST_AUTO_TEST_CASE(ConcurrentReadersObserveValidSnapshots)
{
  ServiceAuthorizationTable table;
  BOOST_REQUIRE(table.replacePermissions(
    tlv::UserPermission, 1,
    {makeRecord("/provider/HELLO", "/HELLO", tlv::UserPermission, 1)}));

  std::atomic<bool> valid{true};
  std::vector<std::thread> readers;
  for (int i = 0; i < 8; ++i) {
    readers.emplace_back([&] {
      for (int j = 0; j < 1000; ++j) {
        auto record = table.find("/provider/HELLO");
        if (!record || !record->isValid()) {
          valid = false;
        }
      }
    });
  }
  for (auto& reader : readers) {
    reader.join();
  }
  BOOST_CHECK(valid.load());
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::test
