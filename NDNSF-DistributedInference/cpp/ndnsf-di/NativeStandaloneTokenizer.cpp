#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeStandaloneTokenizer.hpp"

#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace ndnsf::di {
namespace {

std::filesystem::path
makeTemporaryDirectory()
{
  std::string pattern = "/tmp/ndnsf-di-tokenizer-XXXXXX";
  std::vector<char> writable(pattern.begin(), pattern.end());
  writable.push_back('\0');
  const auto created = ::mkdtemp(writable.data());
  if (created == nullptr) {
    throw std::runtime_error("cannot create tokenizer helper directory");
  }
  return std::filesystem::path(created);
}

std::string
readText(const std::filesystem::path& path)
{
  std::ifstream input(path, std::ios::binary);
  if (!input.good()) {
    throw std::runtime_error("tokenizer helper did not produce output");
  }
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

std::string
idsJson(const std::vector<std::int64_t>& ids)
{
  std::string result = "[";
  for (std::size_t index = 0; index < ids.size(); ++index) {
    if (index != 0) {
      result.push_back(',');
    }
    result += std::to_string(ids[index]);
  }
  result.push_back(']');
  return result;
}

} // namespace

std::function<std::string(const std::vector<std::int64_t>&)>
makeNativeStandaloneTokenizerDecoder(
  NativeStandaloneTokenizerOptions options,
  std::string expectedDigest)
{
  if (options.tokenizerPath.empty() || expectedDigest.empty()) {
    throw std::invalid_argument("standalone tokenizer path/digest is required");
  }
  const auto tokenizer = std::filesystem::absolute(options.tokenizerPath);
  if (!std::filesystem::is_regular_file(tokenizer) ||
      tokenizer.filename() != "tokenizer.json") {
    throw std::invalid_argument("standalone tokenizer.json is unavailable");
  }
  return [options = std::move(options), tokenizer, expectedDigest]
         (const std::vector<std::int64_t>& tokenIds) {
    const auto directory = makeTemporaryDirectory();
    const auto output = directory / "decoded.txt";
    const auto stderrPath = directory / "helper.stderr";
    const auto ids = idsJson(tokenIds);
    const auto child = ::fork();
    if (child < 0) {
      std::filesystem::remove_all(directory);
      throw std::runtime_error("cannot fork tokenizer helper");
    }
    if (child == 0) {
      const auto fd = ::open(stderrPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
      if (fd >= 0) {
        ::dup2(fd, STDERR_FILENO);
        ::close(fd);
      }
      ::execlp(options.pythonExecutable.c_str(), options.pythonExecutable.c_str(),
               "-m", options.pythonModule.c_str(),
               "--tokenizer", tokenizer.c_str(),
               "--expected-digest", expectedDigest.c_str(),
               "--ids-json", ids.c_str(), "--output", output.c_str(),
               static_cast<char*>(nullptr));
      _exit(127);
    }
    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      std::string detail;
      try {
        detail = readText(stderrPath);
      }
      catch (...) {
        detail = "helper stderr unavailable";
      }
      std::filesystem::remove_all(directory);
      throw std::runtime_error("standalone tokenizer helper failed: " + detail);
    }
    const auto value = readText(output);
    std::filesystem::remove_all(directory);
    return value;
  };
}

} // namespace ndnsf::di
