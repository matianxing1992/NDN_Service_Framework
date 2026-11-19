#include "tests/boost-test.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/RuntimeTiming.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/runtime-logging.hpp"
#include <ndn-cxx/util/logging.hpp>
#include <ndn-cxx/util/logger.hpp>
#include <boost/scope_exit.hpp>
#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>
#include <mutex>
#include <regex>
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

class ScopedSynchronousDestination
{
public:
  explicit ScopedSynchronousDestination(std::ostream& output)
  {
    detail::configureSynchronousLogging(output);
  }

  ~ScopedSynchronousDestination()
  {
    detail::configureSynchronousLogging(std::clog);
  }

  ScopedSynchronousDestination(const ScopedSynchronousDestination&) = delete;
  ScopedSynchronousDestination& operator=(const ScopedSynchronousDestination&) = delete;
};

class CallingThreadBuffer : public std::stringbuf
{
public:
  bool
  hasForeignWriter() const
  {
    return m_foreignWriter.load(std::memory_order_relaxed);
  }

  std::size_t
  writeCount() const
  {
    return m_writes.load(std::memory_order_relaxed);
  }

protected:
  std::streamsize
  xsputn(const char* text, std::streamsize size) override
  {
    observeWriter();
    return std::stringbuf::xsputn(text, size);
  }

  int_type
  overflow(int_type ch) override
  {
    observeWriter();
    return std::stringbuf::overflow(ch);
  }

private:
  void
  observeWriter()
  {
    m_writes.fetch_add(1, std::memory_order_relaxed);
    if (std::this_thread::get_id() != m_caller) {
      m_foreignWriter.store(true, std::memory_order_relaxed);
    }
  }

  const std::thread::id m_caller = std::this_thread::get_id();
  std::atomic<bool> m_foreignWriter{false};
  std::atomic<std::size_t> m_writes{0};
};

} // namespace

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

BOOST_AUTO_TEST_CASE(Spec216SynchronousSinkPreservesFormatAndImmediateRecords)
{
  ndn::util::Logging::setLevel("*=WARN");
  std::ostringstream output;
  ScopedSynchronousDestination destination(output);
  logRuntimeEvidence("record=one\nspaces=two\rwords");
  logRuntimeInfo("filtered=info");
  logRuntimeTrace("filtered=trace");
  logRuntimeWarn("record=warn");
  logRuntimeError("record=error");
  // Do not flush here: a completed call must already have emitted its record.
  std::istringstream lines(output.str());
  std::string line;
  BOOST_REQUIRE(static_cast<bool>(std::getline(lines, line)));
  BOOST_CHECK(std::regex_match(line, std::regex(
    R"(^[0-9]+\.[0-9]{6}  WARN: \[ndnsf\.di\.RuntimeEvidence\] record=one spaces=two words$)")));
  BOOST_REQUIRE(static_cast<bool>(std::getline(lines, line)));
  BOOST_CHECK(line.find(" WARN: [ndnsf.di.RuntimeEvidence] record=warn") != std::string::npos);
  BOOST_REQUIRE(static_cast<bool>(std::getline(lines, line)));
  BOOST_CHECK(line.find("ERROR: [ndnsf.di.RuntimeEvidence] record=error") != std::string::npos);
  BOOST_CHECK(!std::getline(lines, line));
}

BOOST_AUTO_TEST_CASE(Spec216SynchronousSinkRetainsConfiguredModuleLevels)
{
  ndn::util::Logging::setLevel("*=WARN:ndnsf.di.EvidenceLineFixture=TRACE");
  std::ostringstream output;
  ScopedSynchronousDestination destination(output);
  NDN_LOG_TRACE("module=trace");
  NDN_LOG_INFO("module=info");
  logRuntimeTrace("runtime=filtered");
  logRuntimeWarn("runtime=warn");
  const auto text = output.str();
  BOOST_CHECK(text.find("TRACE: [ndnsf.di.EvidenceLineFixture] module=trace") !=
              std::string::npos);
  BOOST_CHECK(text.find(" INFO: [ndnsf.di.EvidenceLineFixture] module=info") !=
              std::string::npos);
  BOOST_CHECK(text.find("runtime=filtered") == std::string::npos);
  BOOST_CHECK(text.find(" WARN: [ndnsf.di.RuntimeEvidence] runtime=warn") != std::string::npos);
  BOOST_CHECK_EQUAL(std::count(text.begin(), text.end(), '\n'), 3);
}

BOOST_AUTO_TEST_CASE(Spec216SynchronousSinkKeepsLongRecordsComplete)
{
  ndn::util::Logging::setLevel("*=WARN");
  std::ostringstream output;
  ScopedSynchronousDestination destination(output);
  const auto record = "NDNSF_LOG_LARGE value=" + std::string(65536, 'x') + " END";
  logRuntimeEvidence(record);
  const auto text = output.str();
  const auto start = text.find("NDNSF_LOG_LARGE");
  BOOST_REQUIRE(start != std::string::npos);
  BOOST_CHECK_EQUAL(text.substr(start), record + '\n');
  BOOST_CHECK_EQUAL(std::count(text.begin(), text.end(), '\n'), 1);
}

BOOST_AUTO_TEST_CASE(Spec216SynchronousSinkConservesMixedConcurrentRecords)
{
  ndn::util::Logging::setLevel("*=WARN");
  std::ostringstream output;
  ScopedSynchronousDestination destination(output);
  auto record = [] (unsigned writer, unsigned index) {
    return "NDNSF_LOG_TEST producer=" + std::to_string(writer) + " item=" +
      std::to_string(index);
  };
  std::set<std::string> expected;
  for (unsigned writer = 0; writer < 4; ++writer) {
    for (unsigned index = 0; index < 128; ++index) expected.insert(record(writer, index));
  }
  std::vector<std::future<void>> writers;
  writers.reserve(4);
  for (unsigned writer = 0; writer < 4; ++writer) {
    writers.push_back(std::async(std::launch::async, [&, writer] {
      for (unsigned index = 0; index < 128; ++index) {
        if (writer % 2 == 0) {
          logRuntimeEvidence(record(writer, index));
        }
        else {
          NDN_LOG_WARN(record(writer, index));
        }
      }
    }));
  }
  for (auto& writer : writers) writer.get();
  std::istringstream lines(output.str());
  std::string line;
  unsigned count = 0;
  while (std::getline(lines, line)) {
    ++count;
    const auto start = line.find("NDNSF_LOG_TEST");
    BOOST_REQUIRE(start != std::string::npos);
    BOOST_CHECK_EQUAL(expected.erase(line.substr(start)), 1);
  }
  BOOST_CHECK_EQUAL(count, 512);
  BOOST_CHECK(expected.empty());
}

BOOST_AUTO_TEST_CASE(Spec216SynchronousSinkWritesOnCallingThread)
{
  ndn::util::Logging::setLevel("*=WARN");
  CallingThreadBuffer buffer;
  std::ostream output(&buffer);
  ScopedSynchronousDestination destination(output);
  logRuntimeEvidence("NDNSF_LOG_CALLER expected=caller");
  ndn::util::Logging::flush();
  BOOST_CHECK(buffer.writeCount() > 0);
  BOOST_CHECK(!buffer.hasForeignWriter());
  BOOST_CHECK(buffer.str().find("NDNSF_LOG_CALLER expected=caller") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(Spec216SynchronousSinkReplacesDestinationBeforeStreamRelease)
{
  ndn::util::Logging::setLevel("*=WARN");
  std::ostringstream first, second;
  ScopedSynchronousDestination destination(first);
  logRuntimeEvidence("destination=first");
  detail::configureSynchronousLogging(second);
  logRuntimeEvidence("destination=second");
  ndn::util::Logging::flush();
  BOOST_CHECK(first.str().find("destination=first") != std::string::npos);
  BOOST_CHECK(first.str().find("destination=second") == std::string::npos);
  BOOST_CHECK(second.str().find("destination=second") != std::string::npos);
  BOOST_CHECK(second.str().find("destination=first") == std::string::npos);
}

BOOST_AUTO_TEST_CASE(Spec216DisabledLoggingDoesNotWaitForOutputMutex)
{
  const char* previous = std::getenv("NDNSF_PHASE_TIMING");
  const bool hadPhaseTiming = previous != nullptr;
  const std::string previousPhaseTiming = previous ? previous : "";
  BOOST_SCOPE_EXIT(&hadPhaseTiming, &previousPhaseTiming) {
    if (hadPhaseTiming) ::setenv("NDNSF_PHASE_TIMING", previousPhaseTiming.c_str(), 1);
    else ::unsetenv("NDNSF_PHASE_TIMING");
  } BOOST_SCOPE_EXIT_END
  BOOST_REQUIRE_EQUAL(::setenv("NDNSF_PHASE_TIMING", "1", 1), 0);
  ndn::util::Logging::setLevel("*=NONE");
  std::ostringstream output;
  ScopedSynchronousDestination destination(output);
  std::unique_lock<std::mutex> lock(runtimeTimingOutputMutex());
  auto worker = std::async(std::launch::async, [] {
    logRuntimeEvidence("disabled=evidence");
    logRuntimeTrace("disabled=trace");
    logRuntimeInfo("disabled=info");
    logRuntimeWarn("disabled=warn");
    logRuntimeError("disabled=error");
    logRuntimePhase("di-provider", "ortRunBegin", "disabled-request");
  });
  const auto status = worker.wait_for(std::chrono::seconds(2));
  // Release before collecting the worker, including when a regression blocks it.
  lock.unlock();
  worker.get();
  ndn::util::Logging::flush();
  BOOST_CHECK(status == std::future_status::ready);
  BOOST_CHECK(output.str().empty());
  ndn::util::Logging::setLevel("*=WARN");
  logRuntimeEvidence("enabled=again");
  logRuntimePhase("di-provider", "ortRunBegin", "enabled-request");
  BOOST_CHECK(output.str().find("enabled=again") != std::string::npos);
  BOOST_CHECK(output.str().find("phase=ortRunBegin") != std::string::npos);
  BOOST_CHECK(output.str().find("requestId=enabled-request") != std::string::npos);
}
} // namespace ndnsf::di::test
