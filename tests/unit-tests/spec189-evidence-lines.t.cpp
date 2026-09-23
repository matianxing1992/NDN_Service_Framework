#include "tests/boost-test.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/RuntimeTiming.hpp"
#include <ndn-cxx/util/logging.hpp>
#include <ndn-cxx/util/logger.hpp>
#include <array>
#include <algorithm>
#include <iostream>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

namespace ndnsf::di::test {
NDN_LOG_INIT(ndnsf.di.EvidenceLineFixture);
namespace {
struct Capture
{
  std::ostringstream stream;
  Capture() { ndn::util::Logging::setDestination(stream, true); }
  ~Capture() { ndn::util::Logging::setDestination(std::clog, true); }
};
}

BOOST_AUTO_TEST_CASE(Spec189EvidenceSinkKeepsConcurrentRecordsWhole)
{
  ndn::util::Logging::setLevel("*=WARN");
  Capture capture;
  constexpr unsigned count = 32;
  const std::array<std::string, 5> prefixes{{"NDNSF_DI_GRANT_VERIFIED",
    "NDNSF_DI_PROVIDER_BOUNDARY", "NDNSF_DI_DEPENDENCY", "NDNSF_DI_ASSEMBLY_PROGRESS",
    "NDNSF_DI_EXECUTION_EVIDENCE_UPDATE"}};
  const auto record = [&](unsigned writer, unsigned index) {
    return prefixes[writer] + " id=" + std::to_string(writer) + ":" + std::to_string(index) +
      " planDigest=sha256:" + std::string(64, 'a') + " payload=" +
      (writer == 4 ? "{\"data\":\"" + std::string(65536, 'x') + "\"}" : "ok") + " END";
  };
  std::set<std::string> expected;
  for (unsigned w = 0; w != prefixes.size(); ++w)
    for (unsigned i = 0; i != count; ++i) expected.insert(record(w, i));
  std::vector<std::thread> writers;
  for (unsigned w = 0; w != prefixes.size(); ++w)
    writers.emplace_back([&, w] {
      for (unsigned i = 0; i != count; ++i) logRuntimeEvidence(record(w, i));
    });
  // Other ndn-cxx components share the backend, not our evidence mutex.
  writers.emplace_back([] {
    for (unsigned i = 0; i != count; ++i) NDN_LOG_WARN("FRAMEWORK_RECORD id=" << i << " END");
  });
  for (auto& writer : writers) writer.join();
  ndn::util::Logging::flush();
  std::istringstream lines(capture.stream.str());
  std::string line;
  unsigned framework = 0;
  while (std::getline(lines, line)) {
    const auto start = line.find("NDNSF_DI_");
    if (start != std::string::npos) BOOST_CHECK_EQUAL(expected.erase(line.substr(start)), 1);
    else {
      BOOST_CHECK(line.find("FRAMEWORK_RECORD id=") != std::string::npos);
      BOOST_CHECK(line.size() >= 4 && line.substr(line.size() - 4) == " END");
      ++framework;
    }
  }
  BOOST_CHECK(expected.empty());
  BOOST_CHECK_EQUAL(framework, count);
}

BOOST_AUTO_TEST_CASE(Spec189EvidenceSinkEscapesPhysicalNewlines)
{
  ndn::util::Logging::setLevel("*=WARN");
  Capture capture;
  logRuntimeEvidence("NDNSF_DI_RECORD detail=first\nsecond\rthird\n");
  ndn::util::Logging::flush();
  const auto text = capture.stream.str();
  BOOST_CHECK(text.find("NDNSF_DI_RECORD detail=first second third ") != std::string::npos);
  BOOST_CHECK_EQUAL(std::count(text.begin(), text.end(), '\n'), 1);
}
} // namespace ndnsf::di::test
