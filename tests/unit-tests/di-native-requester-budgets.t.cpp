#define NDNSF_DI_REQUESTER_NO_MAIN
#include "examples/DI_NativeRequester.cpp"
#include <boost/test/unit_test.hpp>

namespace {
ptree budgetConfig(const std::optional<std::string>& timeout)
{
  ptree turn;
  turn.put("input_file", "input.bin");
  turn.put("options_file", "options.json");
  turn.put("output_file", "output.json");
  if (timeout)
    turn.put("request_timeout_ms", *timeout);
  ptree turns;
  turns.push_back({"", turn});
  ptree config;
  config.put_child("turns", turns);
  return config;
}

TurnSpec parseBudget(const std::optional<std::string>& timeout)
{
  return readTurnSpecs(budgetConfig(timeout), "/tmp", std::nullopt, std::nullopt).at(0);
}

RequestOptions sourceOptions()
{
  RequestOptions options;
  options.timeout = std::chrono::milliseconds(2520000);
  options.ackTimeout = std::chrono::milliseconds(120000);
  options.providerNames = {"/source"};
  return options;
}

struct BarrierFiles
{
  std::filesystem::path directory = std::filesystem::temp_directory_path() /
    ("spec212-requester-barrier-" + std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count()));
  BarrierFiles() { std::filesystem::create_directory(directory); }
  ~BarrierFiles()
  {
    std::error_code error;
    std::filesystem::remove_all(directory, error);
  }
  ptree config(std::uint64_t timeout, bool departure = true,
               const char* key = "test_handoff_barrier") const
  {
    ptree value;
    value.put("test_only", true);
    value.put("kv_handoff.wait_for_departure", departure);
    value.put(std::string(key) + ".ready_file", "ready");
    value.put(std::string(key) + ".release_file", "release");
    value.put(std::string(key) + ".timeout_ms", timeout);
    return value;
  }
};
}

BOOST_AUTO_TEST_SUITE(Spec212RequesterTurnBudget)

BOOST_AUTO_TEST_CASE(LegacyAndAbsentBudgetPreserveOptions)
{
  const auto base = sourceOptions();
  const auto absent = requestOptionsForTurn(base, parseBudget(std::nullopt));
  BOOST_CHECK(absent.timeout == base.timeout);
  BOOST_CHECK(absent.ackTimeout == base.ackTimeout);
  BOOST_CHECK(absent.providerNames == base.providerNames);
  const auto legacy = readTurnSpecs(ptree{}, "/tmp", "/tmp/input", "/tmp/output");
  BOOST_REQUIRE_EQUAL(legacy.size(), 1U);
  BOOST_CHECK(!legacy.front().requestTimeout);
  BOOST_CHECK(requestOptionsForTurn(base, legacy.front()).timeout == base.timeout);
}

BOOST_AUTO_TEST_CASE(ExplicitBudgetNarrowsRealRequestOptions)
{
  const auto base = sourceOptions();
  const auto narrowed = requestOptionsForTurn(base, parseBudget("300000"));
  BOOST_CHECK_EQUAL(narrowed.timeout.count(), 300000);
  BOOST_CHECK(narrowed.ackTimeout == base.ackTimeout);
  BOOST_CHECK(narrowed.providerNames == base.providerNames);
  BOOST_CHECK_EQUAL(base.timeout.count(), 2520000);
  BOOST_CHECK(requestOptionsForTurn(base, parseBudget("2520000")).timeout == base.timeout);
  BOOST_CHECK_EQUAL(requestOptionsForTurn(base, parseBudget("120001")).timeout.count(), 120001);
}

BOOST_AUTO_TEST_CASE(MalformedBudgetsRejectAtParse)
{
  for (const auto* wire : {"0", "-1", "3600001", "18446744073709551616", "not-a-number"})
    BOOST_CHECK_THROW(parseBudget(wire), std::exception);
  auto config = budgetConfig("300000");
  config.get_child("turns").front().second.add("request_timeout_ms", "400000");
  BOOST_CHECK_THROW(readTurnSpecs(config, "/tmp", std::nullopt, std::nullopt),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(BudgetCannotExceedRequestOrAckWindow)
{
  const auto base = sourceOptions();
  for (const auto* wire : {"2520001", "3600000", "120000", "1"})
    BOOST_CHECK_THROW(requestOptionsForTurn(base, parseBudget(wire)), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(WithdrawalHandoffBarrierOutlivesLegacyMinute)
{
  BarrierFiles files;
  const auto config = files.config(650000);
  std::atomic<bool> stop{false};
  std::thread releaser([&] {
    while (!stop && !std::filesystem::exists(files.directory / "ready"))
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const auto releaseAt = std::chrono::steady_clock::now() + std::chrono::seconds(61);
    while (!stop && std::chrono::steady_clock::now() < releaseAt)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!stop)
      std::ofstream(files.directory / "release") << "source-closed\n";
  });
  const auto started = std::chrono::steady_clock::now();
  try {
    waitForTestStreamBarrier(config, files.directory, 0, 1, "test_handoff_barrier");
  }
  catch (...) {
    stop = true;
    releaser.join();
    throw;
  }
  stop = true;
  releaser.join();
  BOOST_CHECK(std::chrono::steady_clock::now() - started >= std::chrono::seconds(61));
  std::ifstream ready(files.directory / "ready");
  std::string marker;
  std::getline(ready, marker);
  BOOST_CHECK_EQUAL(marker, "test_handoff_barrier");
  BOOST_CHECK(std::filesystem::exists(files.directory / "release"));
}

BOOST_AUTO_TEST_CASE(WithdrawalBarrierBoundsRemainScopedAndFailClosed)
{
  BarrierFiles files;
  for (const auto timeout : {0U, 650001U})
    BOOST_CHECK_THROW(waitForTestStreamBarrier(files.config(timeout), files.directory,
                      0, 1, "test_handoff_barrier"), std::invalid_argument);
  BOOST_CHECK_THROW(waitForTestStreamBarrier(files.config(60001, false), files.directory,
                    0, 1, "test_handoff_barrier"), std::invalid_argument);
  BOOST_CHECK_THROW(waitForTestStreamBarrier(files.config(60001, true, "test_stream_barrier"),
                    files.directory, 0, 1), std::invalid_argument);
  auto nonTest = files.config(650000);
  nonTest.put("test_only", false);
  BOOST_CHECK_THROW(waitForTestStreamBarrier(nonTest, files.directory,
                    0, 1, "test_handoff_barrier"), std::invalid_argument);
  BOOST_CHECK(!std::filesystem::exists(files.directory / "ready"));
  BOOST_CHECK_THROW(waitForTestStreamBarrier(files.config(20), files.directory,
                    0, 1, "test_handoff_barrier"), std::runtime_error);
  BOOST_CHECK(!std::filesystem::exists(files.directory / "release"));
}

BOOST_AUTO_TEST_SUITE_END()
