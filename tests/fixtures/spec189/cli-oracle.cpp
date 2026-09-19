#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
std::string provider(unsigned index, bool materials, bool terminal, unsigned begin, unsigned end)
{
  const auto name = "/example/ndnsf-qwen06b/provider-" + std::to_string(index);
  const auto identity = " requestId=request attemptEpoch=1 provider=" + name +
    " role=role" + std::to_string(index) + " planDigest=plan";
  std::string text = "NDNSF_DI_NATIVE_SELECTION_ACCEPTED" + identity +
    " manifestDigest=root graphDigest=graph initializerDigest=initializer artifactDigest=artifact" +
    " layerBegin=" + std::to_string(begin) + " layerEnd=" + std::to_string(end) + "\n";
  text += "NDNSF_DI_GRANT_VERIFIED" + identity + "\n";
  const auto stage = [&](const std::string& stage, const std::string& status = "observed") {
    return "NDNSF_DI_PROVIDER_STAGE stage=" + stage + " status=" + status + identity +
      " preparationId=1\n";
  };
  text += stage("GRANT_VERIFIED") + stage("EXECUTION_ENTERED") + stage("ASSEMBLY_STARTED");
  if (materials) {
    for (const auto& kind : {"root", "material-manifest", "material-payload"}) {
      const auto digest = std::string(kind) == "root" ? "root" : "digest";
      for (const auto& status : {"begin", "returned", "verified"}) {
        text += "NDNSF_DI_PROVIDER_MATERIAL_FETCH" + identity + " kind=" + kind +
          " name=/" + kind + " status=" + status + " expectedDigest=" + digest +
          " bytes=12 digest=" + digest + "\n";
      }
    }
  }
  text += stage("RUNNER_READY");
  if (begin != 0) text += stage("DEPENDENCY_FETCH", "begin") + stage("DEPENDENCY_FETCH", "complete");
  text += stage("EXECUTION_COMPLETED");
  if (terminal) text += stage("TERMINAL");
  return text;
}

void write(const std::filesystem::path& path, const std::string& text)
{
  std::ofstream out(path); out << text;
  if (!out) throw std::runtime_error("fixture write failed");
}
} // namespace

int main(int argc, char** argv)
{
  if (argc != 2) return 2;
  char directory[] = "/tmp/spec189-cli-oracle-XXXXXX";
  if (!mkdtemp(directory)) return 2;
  const std::filesystem::path root(directory);
  struct Cleanup {
    std::filesystem::path root;
    ~Cleanup() { std::error_code error; std::filesystem::remove_all(root, error); }
  } cleanup{root};
  try {
    write(root / "requester-0.log",
      "NDNSF_DI_NATIVE_SELECTION_COMMITTED requestId=request attemptEpoch=1 planDigest=plan\n"
      "NDNSF_COLLAB_ASSIGNMENT_SELECTED requestId=request providerName=/example/ndnsf-qwen06b/provider-0 role=role0\n"
      "NDNSF_COLLAB_ASSIGNMENT_SELECTED requestId=request providerName=/example/ndnsf-qwen06b/provider-1 role=role1\n"
      "NATIVE_REQUEST_SUCCEEDED request=request plan=plan\n");
    unsigned cases = 0;
    const auto check = [&](const std::string& first, const std::string& second,
                           const std::string& reason) {
      write(root / "provider-0.log", first);
      write(root / "provider-1.log", second);
      const auto output = (root / "output.log").string();
      const auto child = fork();
      if (child < 0) throw std::runtime_error("fixture fork failed");
      if (child == 0) {
        const int fd = open(output.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0 || dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0) _exit(126);
        close(fd);
        execl(argv[1], argv[1], "--run-root", directory, static_cast<char*>(nullptr));
        _exit(127);
      }
      int status = 0;
      if (waitpid(child, &status, 0) != child || !WIFEXITED(status))
        throw std::runtime_error("oracle did not exit normally");
      std::ifstream in(output); std::ostringstream content; content << in.rdbuf();
      const auto text = content.str();
      if (reason.empty()) {
        if (WEXITSTATUS(status) != 0 || text.find("SPEC189_CPP_ORACLE_PASS") == std::string::npos)
          throw std::runtime_error("positive CLI fixture rejected: " + text);
      }
      else if (WEXITSTATUS(status) != 1 || text.find("reason=" + reason) == std::string::npos ||
               text.find("SPEC189_CPP_ORACLE_PASS") != std::string::npos) {
        throw std::runtime_error("negative CLI fixture mismatch: " + text);
      }
      ++cases;
    };
    const auto first = provider(0, true, false, 0, 14);
    const auto second = provider(1, true, true, 14, 28);
    check(first, second, "");
    check(first, provider(1, false, true, 14, 28), "incomplete-material-sequence");
    check(first, provider(1, true, false, 14, 28), "tail-terminal-missing");
    check(provider(0, true, true, 0, 14), provider(1, true, false, 14, 28), "non-tail-terminal");
    check(first, provider(1, true, true, 15, 28), "range-coverage-mismatch");
    std::cout << "Spec189 CLI oracle: " << cases << " cases passed\n";
    return 0;
  }
  catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
