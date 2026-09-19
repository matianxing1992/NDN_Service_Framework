#include "examples/Spec189MaterialFetchOracle.hpp"

#include <iostream>
#include <unistd.h>

namespace {
using namespace spec189::oracle;

std::string event(const std::string& kind, const std::string& name,
                  const std::string& status, const std::string& digest)
{
  return "NDNSF_DI_PROVIDER_MATERIAL_FETCH requestId=req attemptEpoch=1 "
         "provider=provider role=role planDigest=plan kind=" + kind + " name=" + name +
         " status=" + status + " expectedDigest=" + digest +
         " bytes=12 digest=" + digest + "\n";
}

std::string object(const std::string& kind, const std::string& name,
                   const std::string& digest)
{
  return event(kind, name, "begin", digest) + event(kind, name, "returned", digest) +
         event(kind, name, "verified", digest);
}

void replace(std::string& text, const std::string& from, const std::string& to)
{
  const auto offset = text.find(from);
  if (offset == std::string::npos) throw std::runtime_error("invalid test mutation");
  text.replace(offset, from.size(), to);
}
} // namespace

int main()
{
  char name[] = "/tmp/spec189-material-oracle-XXXXXX";
  const int fd = mkstemp(name);
  if (fd < 0) return 2;
  close(fd);
  struct Cleanup { const char* path; ~Cleanup() { unlink(path); } } cleanup{name};
  try {
    PlacementObservation observation;
    observation.selection.requestId = "req";
    observation.selection.attemptEpoch = "1";
    observation.selection.role = "role";
    observation.selection.planDigest = "plan";
    observation.selection.manifestDigest = "root-digest";
    observation.selectionLine = 1;
    observation.grantLine = 2;
    const auto root = object("root", "/root", "root-digest");
    const auto manifest = object("material-manifest", "/manifest", "manifest-digest");
    const auto payload = object("material-payload", "/layer0", "layer0-digest");
    const auto valid = std::string("selection\ngrant\n") + root + manifest + payload +
                       object("material-payload", "/layer1", "layer1-digest");
    unsigned cases = 0;
    auto check = [&](const std::string& log, const std::string& reason) {
      { std::ofstream out(name); out << log; if (!out) throw std::runtime_error("fixture write"); }
      try {
        const auto result = validateMaterialFetches(name, observation, "provider");
        if (!reason.empty()) throw std::runtime_error("accepted invalid fixture: " + reason);
        if (result.postSelection != 4) throw std::runtime_error("wrong object count");
      }
      catch (const std::runtime_error& error) {
        if (reason.empty() || std::string(error.what()).find("reason=" + reason) == std::string::npos)
          throw;
      }
      ++cases;
    };
    check(valid, "");
    check("selection\ngrant\n" + root + manifest, "incomplete-material-sequence");
    check(valid + event("material-payload", "/partial", "begin", "partial"),
          "incomplete-material-sequence");
    check(valid + payload, "duplicate-begin");
    check("selection\ngrant\n" + payload + root + manifest, "unauthenticated-parent");
    check("selection\ngrant\n" + manifest + root + payload, "unauthenticated-parent");
    check(root + manifest + payload, "material-fetch");
    check(valid + object("source", "/whole-source", "source"), "unknown-kind");
    for (const auto& mutation : std::vector<std::pair<std::string, std::string>>{
           {"provider=provider", "provider=other"}, {"role=role", "role=other"},
           {"planDigest=plan", "planDigest=other"}, {"attemptEpoch=1", "attemptEpoch=2"}}) {
      auto log = valid; replace(log, mutation.first, mutation.second);
      check(log, "identity-mismatch");
    }
    auto log = valid;
    replace(log, "status=returned expectedDigest=root-digest bytes=12 digest=root-digest",
                 "status=returned expectedDigest=root-digest bytes=12 digest=corrupt");
    check(log, "verification-evidence-mismatch");
    log = valid;
    replace(log, "status=verified expectedDigest=root-digest bytes=12",
                 "status=verified expectedDigest=root-digest bytes=13");
    check(log, "verification-evidence-mismatch");
    log = valid;
    replace(log, "status=begin", "status=verified");
    check(log, "verified-before-returned");
    std::cout << "Spec189 material event oracle: " << cases << " cases passed\n";
    return 0;
  }
  catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
