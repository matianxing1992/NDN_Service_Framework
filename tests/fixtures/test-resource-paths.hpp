#ifndef NDNSF_TESTS_FIXTURES_TEST_RESOURCE_PATHS_HPP
#define NDNSF_TESTS_FIXTURES_TEST_RESOURCE_PATHS_HPP

#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>
#include <system_error>
#include <unistd.h>

namespace ndnsf::test {

/** Resolve a test-owned executable from an explicit or Waf-configured binary root. */
inline std::filesystem::path
testBinaryPath(const std::filesystem::path& basename)
{
  if (basename.empty() || basename.has_parent_path() || basename.is_absolute()) {
    throw std::invalid_argument("test binary must be a basename");
  }
  std::filesystem::path root;
  if (const auto* configured = std::getenv("NDNSF_SPEC182_BIN_DIR")) {
    root = configured;
  }
#ifdef NDNSF_TEST_BINARY_DIR
  else {
    root = NDNSF_TEST_BINARY_DIR;
  }
#endif
  if (root.empty() || !root.is_absolute() || !std::filesystem::is_directory(root)) {
    throw std::runtime_error("test binary root is missing or not absolute");
  }
  const auto path = root / basename;
  if (!std::filesystem::is_regular_file(path) || ::access(path.c_str(), X_OK) != 0) {
    throw std::runtime_error("required native test binary missing: " + path.string());
  }
  return path;
}

/** Resolve one read-only test resource, without guessing where it lives.
 *
 * A test resource is a small tracked file under `tests/fixtures/` -- a tiny ONNX graph, a trust
 * schema, a profile. Resolving it from the current directory is what made several cases pass on one
 * machine and fail on another: the same relative path exists under the checkout root, under `build/`
 * and under a test's own working directory, and a two-level parent search found whichever came first.
 *
 * The order is explicit and each step either succeeds or fails; nothing searches:
 *
 *   1. `NDNSF_TEST_FIXTURE_ROOT` -- an installed runner's fixture prefix. If it is set but not a
 *      directory, this throws rather than falling through, because a wrong explicit root is a
 *      configuration error and falling back would hide it.
 *   2. `NDNSF_TEST_SOURCE_DIR`/tests/fixtures -- a source-tree runner, where Waf defines the root.
 *   3. `NDNSF_TEST_SOURCE_DIR`/tests/fixtures/installed-api -- the installed API roster's home.
 *
 * @param relative Path below the fixture root; must be relative and must not escape the root.
 * @returns Absolute path to an existing regular file.
 * @throws std::invalid_argument when the path is absolute or contains a `..` component.
 * @throws std::runtime_error when no root is configured or the resource is missing.
 */
inline std::filesystem::path
testFixturePath(const std::filesystem::path& relative)
{
  if (relative.is_absolute() || relative.has_root_path()) {
    throw std::invalid_argument(
      "test fixture path must be relative: " + relative.string());
  }
  for (const auto& part : relative) {
    if (part == "..") {
      throw std::invalid_argument(
        "test fixture path must not escape its root: " + relative.string());
    }
  }

  std::filesystem::path root;
  if (const char* explicitRoot = std::getenv("NDNSF_TEST_FIXTURE_ROOT")) {
    if (*explicitRoot == '\0') {
      throw std::runtime_error("NDNSF_TEST_FIXTURE_ROOT is explicitly empty");
    }
    if (*explicitRoot != '\0') {
      std::error_code statusError;
      const auto candidate = std::filesystem::path(explicitRoot);
      if (!std::filesystem::is_directory(candidate, statusError)) {
        throw std::runtime_error(
          std::string("NDNSF_TEST_FIXTURE_ROOT is not a directory: ") + explicitRoot);
      }
      root = candidate;
    }
  }

  const auto resolveUnder = [&relative] (const std::filesystem::path& base)
    -> std::filesystem::path {
    std::error_code statusError;
    const auto candidate = (base / relative).lexically_normal();
    if (!std::filesystem::is_regular_file(candidate, statusError)) return {};
    // Containment: a symlink inside the root must not resolve outside it.
    const auto canonicalBase = std::filesystem::weakly_canonical(base, statusError);
    if (statusError) return {};
    const auto canonicalFile = std::filesystem::weakly_canonical(candidate, statusError);
    if (statusError) return {};
    const auto baseText = canonicalBase.string();
    const auto fileText = canonicalFile.string();
    if (fileText.size() <= baseText.size() ||
        fileText.compare(0, baseText.size(), baseText) != 0 ||
        (fileText[baseText.size()] != '/' && fileText[baseText.size()] != '\\')) {
      return {};
    }
    return canonicalFile;
  };

  if (!root.empty()) {
    const auto resolved = resolveUnder(root);
    if (resolved.empty()) {
      throw std::runtime_error(
        "fixture resource is missing under NDNSF_TEST_FIXTURE_ROOT: " + relative.string());
    }
    return resolved;
  }

#ifdef NDNSF_TEST_SOURCE_DIR
  const std::filesystem::path sourceRoot(NDNSF_TEST_SOURCE_DIR);
  for (const auto& base : {sourceRoot / "tests" / "fixtures",
                           sourceRoot / "tests" / "fixtures" / "installed-api"}) {
    const auto resolved = resolveUnder(base);
    if (!resolved.empty()) return resolved;
  }
  throw std::runtime_error(
    "fixture resource is missing under NDNSF_TEST_SOURCE_DIR: " + relative.string());
#else
  throw std::runtime_error(
    "no test fixture root is configured; set NDNSF_TEST_FIXTURE_ROOT or build the "
    "source-tree target: " + relative.string());
#endif
}

/** A writable scratch directory for one test, with an owner and no shared parent.
 *
 * Tests that need to write use `std::filesystem::temp_directory_path()`, which honours `TMPDIR` and is
 * correct, but several hard-coded `/var/tmp/...XXXXXX` as their `mkstemp`/`mkdtemp` template. That is a
 * real input authority: on a host without `/var/tmp`, or in a container where it is read-only, those
 * cases fail for a reason unrelated to what they test.
 *
 * @param stem Prefix for the entry name.
 * @returns A template string ending in `XXXXXX` under the platform temp directory, for `mkstemp` or
 *          `mkdtemp` to fill in. The caller owns and removes what it creates.
 */
inline std::string
makeTestScratchTemplate(const std::string& stem)
{
  std::error_code statusError;
  auto base = std::filesystem::temp_directory_path(statusError);
  if (statusError || base.empty()) base = std::filesystem::path("/tmp");
  return (base / (stem + "-XXXXXX")).string();
}


} // namespace ndnsf::test

#endif
