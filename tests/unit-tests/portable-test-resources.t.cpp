#include "tests/fixtures/test-resource-paths.hpp"
#include "tests/unit-tests/generic-dynamic-api-fixture.hpp"

#include <fstream>

#include <unistd.h>

namespace ndn_service_framework::test {
namespace {

struct ResourceFixture
{
  ResourceFixture()
  {
    auto pattern = ndnsf::test::makeTestScratchTemplate("portable-test-resources");
    const auto directory = ::mkdtemp(pattern.data());
    if (directory == nullptr) {
      throw std::runtime_error("cannot create resource fixture");
    }
    root = directory;
    std::filesystem::create_directory(root / "fixtures");
    std::ofstream(root / "fixtures" / "probe.txt") << "tracked-style input";
    std::ofstream(root / "outside.txt") << "not a fixture";
  }

  ~ResourceFixture()
  {
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  }

  std::filesystem::path root;
};

} // namespace

BOOST_AUTO_TEST_SUITE(PortableTestResources)

BOOST_FIXTURE_TEST_CASE(ExplicitRootResolvesOnlyItsResources, ResourceFixture)
{
  ScopedEnvironmentValue environment("NDNSF_TEST_FIXTURE_ROOT", (root / "fixtures").c_str());
  BOOST_CHECK_EQUAL(ndnsf::test::testFixturePath("probe.txt"), root / "fixtures" / "probe.txt");
  BOOST_CHECK_THROW(ndnsf::test::testFixturePath("missing.txt"), std::runtime_error);
}

BOOST_FIXTURE_TEST_CASE(ExplicitBadRootNeverFallsBack, ResourceFixture)
{
  ScopedEnvironmentValue environment("NDNSF_TEST_FIXTURE_ROOT", (root / "missing").c_str());
  BOOST_CHECK_THROW(ndnsf::test::testFixturePath("probe.txt"), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(ExplicitEmptyRootNeverFallsBack)
{
  ScopedEnvironmentValue environment("NDNSF_TEST_FIXTURE_ROOT", "");
  BOOST_CHECK_THROW(ndnsf::test::testFixturePath("probe.txt"), std::runtime_error);
}

BOOST_FIXTURE_TEST_CASE(AbsoluteTraversalAndSymlinkEscapeAreRejected, ResourceFixture)
{
  ScopedEnvironmentValue environment("NDNSF_TEST_FIXTURE_ROOT", (root / "fixtures").c_str());
  BOOST_CHECK_THROW(ndnsf::test::testFixturePath(root / "outside.txt"), std::invalid_argument);
  BOOST_CHECK_THROW(ndnsf::test::testFixturePath("../outside.txt"), std::invalid_argument);
  std::filesystem::create_symlink(root / "outside.txt", root / "fixtures" / "escaped.txt");
  BOOST_CHECK_THROW(ndnsf::test::testFixturePath("escaped.txt"), std::runtime_error);
}

BOOST_FIXTURE_TEST_CASE(BinaryRootIsExplicitAndBasenamesCannotEscape, ResourceFixture)
{
  const auto binary = root / "probe-bin";
  std::ofstream(binary) << "#!/bin/sh\nexit 0\n";
  std::filesystem::permissions(binary, std::filesystem::perms::owner_all);
  ScopedEnvironmentValue environment("NDNSF_SPEC182_BIN_DIR", root.c_str());
  BOOST_CHECK_EQUAL(ndnsf::test::testBinaryPath("probe-bin"), binary);
  BOOST_CHECK_THROW(ndnsf::test::testBinaryPath("../probe-bin"), std::invalid_argument);
  BOOST_CHECK_THROW(ndnsf::test::testBinaryPath(binary), std::invalid_argument);
  BOOST_CHECK_THROW(ndnsf::test::testBinaryPath("missing-bin"), std::runtime_error);
}

BOOST_FIXTURE_TEST_CASE(BadBinaryRootNeverUsesConfiguredBuild, ResourceFixture)
{
  ScopedEnvironmentValue environment("NDNSF_SPEC182_BIN_DIR", (root / "missing").c_str());
  BOOST_CHECK_THROW(ndnsf::test::testBinaryPath("unit-tests"), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(EmptyBinaryRootIsNotAnUnsetOverride)
{
  ScopedEnvironmentValue environment("NDNSF_SPEC182_BIN_DIR", "");
  BOOST_CHECK_THROW(ndnsf::test::testBinaryPath("unit-tests"), std::runtime_error);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndn_service_framework::test
