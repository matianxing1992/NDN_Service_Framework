#pragma once
#include <ndn-service-framework/ServiceUser.hpp>
#include <ndn-service-framework/ServiceProvider.hpp>
#include <ndn-service-framework/CertificatePublisher.hpp>
#include <ndn-cxx/security/key-params.hpp>
#include <ndn-cxx/util/scheduler.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/post.hpp>
#include <nlohmann/json.hpp>
#include <chrono>
#include <fstream>
#include <map>
#include <mutex>
#include <stdexcept>

namespace sim203 {
using Json = nlohmann::json;
namespace nsf = ndn_service_framework;
using Clock = std::chrono::steady_clock;
inline uint64_t nowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}
inline void shutdownFace(ndn::Face& face) {
  face.shutdown();
  // Framework periodic timers can outlive the transport. Stop this actor's
  // loop explicitly; scoped framework owners still join before state release.
  face.getIoContext().stop();
}
struct Options {
  std::map<std::string, std::string> values;
  Options(int argc, char** argv) {
    for (int i = 1; i < argc; i += 2) {
      if (i + 1 == argc || std::string(argv[i]).rfind("--", 0) != 0)
        throw std::invalid_argument("expected --option value");
      if (!values.emplace(argv[i], argv[i + 1]).second)
        throw std::invalid_argument("duplicate option");
    }
  }
  std::string get(const std::string& key, const std::string& fallback = "") const {
    const auto it = values.find(key); return it == values.end() ? fallback : it->second;
  }
  int number(const std::string& key, int fallback) const {
    const auto text = get(key, std::to_string(fallback)); size_t end = 0;
    const int value = std::stoi(text, &end);
    if (end != text.size() || value < 0) throw std::invalid_argument("invalid numeric option " + key);
    return value;
  }
};
inline ndn::security::Certificate certificate(ndn::KeyChain& keys, const ndn::Name& name) {
  // The orchestrator seeds identities before starting any concurrent actors.
  return keys.getPib().getIdentity(name).getDefaultKey().getDefaultCertificate();
}
class Records {
  std::ofstream file;
  std::mutex mutex;
public:
  explicit Records(const std::string& path) : file(path) {
    if (!file) throw std::runtime_error("cannot open records: " + path);
  }
  void write(Json value) {
    value["schema"] = "ndnsf-spec203-record-v1";
    std::lock_guard<std::mutex> lock(mutex);
    file << value.dump() << '\n'; file.flush();
    if (!file) throw std::runtime_error("cannot write record");
  }
};
inline std::string stringPayload(const ndn::Buffer& payload) {
  return std::string(reinterpret_cast<const char*>(payload.data()), payload.size());
}
}
