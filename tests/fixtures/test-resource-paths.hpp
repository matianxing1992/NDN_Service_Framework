#ifndef NDNSF_TESTS_FIXTURES_TEST_RESOURCE_PATHS_HPP
#define NDNSF_TESTS_FIXTURES_TEST_RESOURCE_PATHS_HPP

#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>

namespace ndnsf::test {

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

/** Resolve one read-only repository resource, without depending on the working directory.
 *
 * Not every input a test needs lives under `tests/fixtures`: a trust schema, a tiny config and a
 * prompt template are shared with production examples. Those are still tracked files, so they are
 * resolved from the same declared roots rather than from `std::filesystem::absolute`, which resolves
 * against the working directory and silently produces a different path for every caller.
 *
 * @param relative Path below the repository (or installed share) root.
 * @returns Absolute path to an existing regular file.
 * @throws std::invalid_argument when the path is absolute or escapes its root.
 * @throws std::runtime_error when no root is configured or the resource is missing.
 */
inline std::filesystem::path
testSourcePath(const std::filesystem::path& relative)
{
  if (relative.is_absolute() || relative.has_root_path()) {
    throw std::invalid_argument(
      "test source path must be relative: " + relative.string());
  }
  for (const auto& part : relative) {
    if (part == "..") {
      throw std::invalid_argument(
        "test source path must not escape its root: " + relative.string());
    }
  }

  const auto resolveUnder = [&relative] (const std::filesystem::path& base)
    -> std::filesystem::path {
    std::error_code statusError;
    const auto candidate = (base / relative).lexically_normal();
    if (!std::filesystem::is_regular_file(candidate, statusError)) return {};
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

  // An installed runner carries the shared inputs under its prefix, beside the fixture root's
  // parent; a source runner uses the tree Waf recorded.
  if (const char* explicitRoot = std::getenv("NDNSF_TEST_FIXTURE_ROOT")) {
    if (*explicitRoot != '\0') {
      std::error_code statusError;
      const auto rootPath = std::filesystem::path(explicitRoot);
      if (!std::filesystem::is_directory(rootPath, statusError)) {
        throw std::runtime_error(
          std::string("NDNSF_TEST_FIXTURE_ROOT is not a directory: ") + explicitRoot);
      }
      const auto candidate = resolveUnder(rootPath);
      if (!candidate.empty()) return candidate;
      // An explicit root that is a directory but lacks the resource is still a configuration
      // error: falling through to the source tree would answer a different question than the
      // caller asked.
      throw std::runtime_error(
        "test source resource is missing under NDNSF_TEST_FIXTURE_ROOT: " + relative.string());
    }
  }
#ifdef NDNSF_TEST_SOURCE_DIR
  {
    const auto candidate = resolveUnder(std::filesystem::path(NDNSF_TEST_SOURCE_DIR));
    if (!candidate.empty()) return candidate;
  }
#endif
  throw std::runtime_error(
    "test source resource is missing: " + relative.string());
}

} // namespace ndnsf::test

#endif
