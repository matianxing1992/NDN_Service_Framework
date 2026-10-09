#include "ndn-service-framework/detail/RequestSubscriptionRegex.hpp"

#include <ndn-cxx/util/regex.hpp>
#include <boost/test/unit_test.hpp>

namespace ndn_service_framework::test {

BOOST_AUTO_TEST_SUITE(ServiceProviderRequestSubscription)

BOOST_AUTO_TEST_CASE(UncapturedWildcardsPreserveIndividualMatches)
{
  std::vector<std::string> services;
  for (const auto* operation : {"CAPABILITY", "STATUS", "STORE", "STORE_RANGE",
       "COMMIT_RANGES", "ABORT_RANGES", "STORE_MANIFEST", "INSERT", "DELETE",
       "FETCH", "FETCH_RANGE", "MANIFEST", "INVENTORY", "CATALOG_STATUS",
       "CATALOG_SNAPSHOT", "CATALOG_DELTA", "CATALOG_LOOKUP"}) {
    services.push_back(std::string("/NDNSF/DistributedRepo/Object/v1/") + operation);
  }
  // Include duplicates, overlapping parents, regex-bearing and typed components.
  services.insert(services.end(), {services[0], "/a", "/a/b", "/a/b/c",
                                  "/a/b/d", "/a/x.y", "/a/54=typed", "/a/%5C1"});
  std::vector<ndn::Regex> before;
  std::vector<ndn::Regex> after;
  for (const auto& service : services) {
    ndn::Name name(service);
    before.emplace_back("^(<>*)<NDNSF><REQUEST>" + NameToRegexString(name) + "(<>*)$");
    after.emplace_back(detail::requestSubscriptionRegex(name));
  }

  std::vector<ndn::Name> corpus;
  for (const auto& service : services) {
    for (const auto* type : {"REQUEST", "ACK", "RESPONSE", "COLLAB", "EVENT"}) {
      for (const auto* prefix : {"/", "/user", "/user/NDNSF/REQUEST/a"}) {
        ndn::Name publication(prefix);
        publication.append("NDNSF").append(type).append(ndn::Name(service));
        corpus.push_back(publication);
        corpus.push_back(ndn::Name(publication).append("request-id"));
        corpus.push_back(ndn::Name(publication).append("request-id").append("segment"));
      }
    }
  }
  corpus.emplace_back("/user/NDNSF/REQUEST/a/b/NDNSF/REQUEST/a/b/d/request-id");
  corpus.emplace_back("/user/NDNSF/REQUEST/NDNSF/DistributedRepo/Object/v1/UNREGISTERED/id");
  corpus.emplace_back("/user/NDNSF/REQUEST/a/54=typed/id");
  corpus.emplace_back("/user/NDNSF/REQUEST/a/xay/id");
  corpus.emplace_back("/user/NDNSF/REQUEST/a/%5C1/id");
  corpus.emplace_back("/unrelated");
  corpus.emplace_back("/");
  for (const auto& publication : corpus) {
    // Individual equality preserves callback multiplicity and registration order.
    for (size_t i = 0; i < before.size(); ++i) {
      BOOST_TEST_CONTEXT("publication=" << publication << " service=" << services[i]) {
        BOOST_CHECK_EQUAL(after[i].match(publication), before[i].match(publication));
      }
    }
  }
  // Explicitly retain multiple matches for two REQUEST segments and duplicates.
  const ndn::Name twoRequests("/user/NDNSF/REQUEST/a/b/NDNSF/REQUEST/a/b/d/id");
  size_t matches = 0;
  for (auto& regex : after) matches += regex.match(twoRequests) ? 1 : 0;
  BOOST_CHECK_EQUAL(matches, 3);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::test
