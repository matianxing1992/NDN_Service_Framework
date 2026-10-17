#pragma once
#include "../targeted-invocation/common.hpp"
#include <ndn-svs/svspubsub.hpp>
#include <ndn-cxx/security/verification-helpers.hpp>
#include <atomic>
#include <algorithm>
#include <iomanip>
#include <iterator>
#include <optional>
#include <set>
#include <sstream>

namespace dynamic204 {
using namespace sim203;
constexpr uint64_t TTL_NS = 2500ULL * 1000000;
constexpr uint64_t COARSE_AD_TTL_NS = 120ULL * 1000000000;

struct TaskDescription
{
  uint64_t id;
  bool memory;
  bool warmup;
};

inline Json
taskJson(const TaskDescription& task)
{
  if (task.id == 0 || task.id > 1000000000) {
    throw std::invalid_argument("task identity out of range");
  }
  return {{"version", 1}, {"task", task.id},
          {"class", task.memory ? "memory" : "compute"},
          {"phase", task.warmup ? "warmup" : "measure"}};
}

inline TaskDescription
decodeTask(const std::string& text)
{
  if (text.empty() || text.size() > 256) {
    throw std::invalid_argument("task descriptor size");
  }
  const auto value = Json::parse(text);
  if (!value.is_object() || value.size() != 4 || value.at("version") != 1
      || !value.at("task").is_number_unsigned()
      || (value.at("class") != "compute" && value.at("class") != "memory")
      || (value.at("phase") != "measure" && value.at("phase") != "warmup")) {
    throw std::invalid_argument("task descriptor schema");
  }
  TaskDescription task{value.at("task").get<uint64_t>(), value.at("class") == "memory",
                       value.at("phase") == "warmup"};
  if (taskJson(task).dump() != text) {
    throw std::invalid_argument("noncanonical task descriptor");
  }
  return task;
}

inline std::string
taskInput(const TaskDescription& task)
{
  taskJson(task); // Validate the same finite identity domain.
  std::ostringstream stream;
  stream << "T205|" << (task.warmup ? 'W' : 'M') << '|'
         << (task.memory ? 'M' : 'C') << '|'
         << std::setfill('0') << std::setw(10) << task.id << "|private-input";
  auto text = stream.str();
  text.resize(64, '_');
  return text;
}

inline TaskDescription
decodeTaskInput(const std::string& text)
{
  if (text.size() != 64 || text.compare(0, 5, "T205|") != 0
      || (text[5] != 'W' && text[5] != 'M') || text[6] != '|'
      || (text[7] != 'C' && text[7] != 'M') || text[8] != '|'
      || text[19] != '|') {
    throw std::invalid_argument("private task input schema");
  }
  uint64_t id = 0;
  for (size_t index = 9; index < 19; ++index) {
    if (text[index] < '0' || text[index] > '9') {
      throw std::invalid_argument("private task identity encoding");
    }
    id = id * 10 + static_cast<unsigned>(text[index] - '0');
  }
  TaskDescription task{id, text[7] == 'M', text[5] == 'W'};
  if (taskInput(task) != text) {
    throw std::invalid_argument("private task input content");
  }
  return task;
}

inline int
resourceUnits(const Json& observation, const char* field)
{
  const auto& value = observation.at(field);
  if (!value.is_number_integer() || value < 0 || value > 1024) {
    throw std::invalid_argument("resource observation type/range");
  }
  return value.get<int>();
}

inline uint64_t
unsignedField(const Json& observation, const char* field)
{
  const auto& value = observation.at(field);
  if (!value.is_number_unsigned()) {
    throw std::invalid_argument("unsigned observation field required");
  }
  return value.get<uint64_t>();
}

inline int
taskScore(const TaskDescription& task, bool available, int compute, int memory)
{
  if (compute < 0 || compute > 1024 || memory < 0 || memory > 1024) {
    throw std::invalid_argument("resource observation out of range");
  }
  const int computeDemand = task.memory ? 2 : 4;
  const int memoryDemand = task.memory ? 4 : 2;
  if (!available || compute < computeDemand || memory < memoryDemand) {
    return 0;
  }
  return std::min(1000 * compute / computeDemand, 1000 * memory / memoryDemand);
}

struct PlannedTask
{
  TaskDescription description;
  uint64_t arrivalMs;
};

inline std::vector<PlannedTask>
decodeTaskPlan(const Json& plan, uint64_t windowMs, uint64_t periodMs)
{
  if ((periodMs != 100 && periodMs != 1000 && periodMs != 4700)
      || windowMs < 1000 || windowMs > 60000
      || !plan.is_object() || plan.size() != 4
      || !plan.at("version").is_number_integer() || plan.at("version") != 1
      || unsignedField(plan,"window_ms") != windowMs
      || unsignedField(plan,"period_ms") != periodMs || !plan.at("tasks").is_array()) {
    throw std::invalid_argument("task plan parameters");
  }
  const auto& rows = plan.at("tasks");
  const uint64_t count = (windowMs + periodMs - 1) / periodMs;
  if (rows.size() != count || count > 600) {
    throw std::invalid_argument("task plan count");
  }
  std::vector<PlannedTask> tasks;
  for (uint64_t index = 0; index < count; ++index) {
    const auto& row = rows.at(index);
    if (!row.is_object() || row.size() != 3 || unsignedField(row,"task") != index + 1
        || unsignedField(row,"arrival_ms") != index * periodMs
        || (row.at("class") != "compute" && row.at("class") != "memory")) {
      throw std::invalid_argument("task plan row");
    }
    tasks.push_back({{index + 1, row.at("class") == "memory", false}, index * periodMs});
  }
  return tasks;
}
inline ndn::name::Component literal(const std::string& value) {
  return {ndn::tlv::GenericNameComponent,
          {reinterpret_cast<const uint8_t*>(value.data()), value.size()}};
}
inline ndn::Name statusPrefix(const ndn::Name& producer) {
  return ndn::Name(producer).append("NDNSF").append("service-status").append(literal("name=HELLO"));
}
// Experiment advertisement transport over the maintained public SVSPubSub API.
// No dependency on NDNSD's fixed HMAC or heartbeat implementation. Receipt is
// still pinned to the actual Producer certificate by verifySnapshot below.
class AdvertisementBus {
  std::unique_ptr<ndn::svs::SVSPubSub> bus;
public:
  using Callback=std::function<void(const ndn::svs::SVSPubSub::SubscriptionData&)>;
  AdvertisementBus(const ndn::Name& group,const ndn::Name& producer,
                   const std::string& session,ndn::Face& face,ndn::KeyChain& keys,
                   const ndn::security::Certificate& cert,Callback callback) {
    ndn::svs::SecurityOptions security(keys);
    security.interestSigner->signingInfo.setSigningCertName(cert.getName());
    security.dataSigner->signingInfo.setSigningCertName(cert.getName());
    security.pubSigner->signingInfo.setSigningCertName(cert.getName());
    ndn::svs::SVSPubSubOptions options;options.useTimestamp=false;
    bus=std::make_unique<ndn::svs::SVSPubSub>(ndn::Name(group).append("advertisements"),
      ndn::Name(producer).append("NDNSF").append("advertisement-sync").append(session),
      face,[](const std::vector<ndn::svs::MissingDataInfo>&){},options,security);
    bus->subscribeWithRegex(ndn::Regex("^(<>*)<NDNSF><advertisement><>+"),std::move(callback),true,false);
  }
  void publish(const ndn::Data& snapshot,const ndn::Name& producer,
               const std::string& session,uint64_t sequence) {
    const auto& wire=snapshot.wireEncode();
    bus->publish(ndn::Name(producer).append("NDNSF").append("advertisement")
      .append(session).appendSequenceNumber(sequence),{wire.data(),wire.size()});
  }
};
struct AvailabilityChange
{
  uint64_t offsetMs;
  bool available;
};

inline Json
parseAvailabilityTrace(const std::string& bytes)
{
  if (bytes.size() > 65536) {
    throw std::invalid_argument("availability trace size");
  }
  std::vector<std::set<std::string>> keys;
  return Json::parse(bytes, [&](int depth, Json::parse_event_t event, Json& value) {
    if (depth > 8) {
      throw std::invalid_argument("availability trace nesting");
    }
    if (event == Json::parse_event_t::object_start) {
      keys.emplace_back();
    }
    else if (event == Json::parse_event_t::key) {
      if (keys.empty() || !keys.back().insert(value.get<std::string>()).second) {
        throw std::invalid_argument("duplicate availability trace key");
      }
    }
    else if (event == Json::parse_event_t::object_end) {
      keys.pop_back();
    }
    return true;
  });
}

inline std::vector<AvailabilityChange>
decodeAvailabilityTrace(const Json& trace, const ndn::Name& producer, uint64_t windowMs)
{
  if (windowMs == 0 || windowMs > 60000 || !trace.is_object() || trace.size() != 5
      || trace.at("version") != 1 || !trace.at("version").is_number_integer()
      || trace.at("producer") != producer.toUri()
      || (producer != ndn::Name("/example/hello/provider/A")
          && producer != ndn::Name("/example/hello/provider/B"))
      || unsignedField(trace, "window_ms") != windowMs || trace.at("tail") != "hold"
      || !trace.at("changes").is_array() || trace.at("changes").empty()
      || trace.at("changes").size() > 256) {
    throw std::invalid_argument("availability trace identity/shape");
  }
  std::vector<AvailabilityChange> changes;
  for (const auto& row : trace.at("changes")) {
    if (!row.is_object() || row.size() != 2 || !row.at("available").is_boolean()) {
      throw std::invalid_argument("availability trace row");
    }
    const auto offset = unsignedField(row, "offset_ms");
    const bool available = row.at("available").get<bool>();
    if (offset >= windowMs || (changes.empty() ? offset != 0
        : offset <= changes.back().offsetMs || available == changes.back().available)) {
      throw std::invalid_argument("availability trace order/state");
    }
    changes.push_back({offset, available});
  }
  return changes;
}

// Provider-only trace parameters; consumers receive authenticated observations.
class LocalState {
  int index, dwellMs;bool forceUnavailable;
  const std::vector<AvailabilityChange> m_changes;
  std::atomic<uint64_t> origin{0};
public:
  LocalState(int index, int dwellMs, bool forceUnavailable = false,
             std::vector<AvailabilityChange> changes = {})
    : index(index), dwellMs(dwellMs), forceUnavailable(forceUnavailable)
    , m_changes(std::move(changes)) {
    if ((index != 0 && index != 1) || (dwellMs != 0 && dwellMs != 500 && dwellMs != 5000))
      throw std::invalid_argument("unsupported trace");
    if (!m_changes.empty() && (dwellMs != 0 || forceUnavailable))
      throw std::invalid_argument("mixed availability modes");
    for (size_t i = 0; i < m_changes.size(); ++i) {
      if (m_changes[i].offsetMs >= 60000 || (i == 0 ? m_changes[i].offsetMs != 0
          : m_changes[i].offsetMs <= m_changes[i - 1].offsetMs
            || m_changes[i].available == m_changes[i - 1].available))
        throw std::invalid_argument("invalid local availability changes");
    }
    if (m_changes.size() > 256)
      throw std::invalid_argument("too many availability changes");
  }
  void setOrigin(uint64_t value) { origin.store(value); }
  bool available(uint64_t now) const {
    const auto start = origin.load();
    if (start && now >= start && !m_changes.empty()) {
      const auto elapsedMs = (now - start) / 1000000;
      const auto next = std::upper_bound(m_changes.begin(), m_changes.end(), elapsedMs,
        [](uint64_t elapsed, const AvailabilityChange& change) {
          return elapsed < change.offsetMs;
        });
      return std::prev(next)->available;
    }
    if(forceUnavailable && start && now>=start)return false;
    return !start || now < start || !dwellMs || ((now-start)/(uint64_t(dwellMs)*1000000))%2 == uint64_t(index);
  }
  int computeUnits() const { return index == 0 ? 8 : 4; }
  int memoryUnits() const { return index == 0 ? 4 : 8; }
};
struct Snapshot {
  ndn::Name producer;
  std::string session;
  uint64_t sequence, published, expires;
  bool available;
  int computeUnits;
  int memoryUnits;
};
inline Json snapshotJson(const Snapshot& s) {
  return {{"producer",s.producer.toUri()},{"service","/HELLO"},{"session",s.session},
          {"sequence",s.sequence},{"published_ns",s.published},{"expires_ns",s.expires},
          {"available",s.available},
          {"compute_units",s.computeUnits},{"memory_units",s.memoryUnits}};
}
inline Snapshot decodeSnapshot(const Json& j, const ndn::Name& producer,
                               const std::string& session, uint64_t now,
                               uint64_t expectedTtl = TTL_NS) {
  Snapshot s{ndn::Name(j.at("producer").get<std::string>()),j.at("session").get<std::string>(),
    unsignedField(j,"sequence"),unsignedField(j,"published_ns"),unsignedField(j,"expires_ns"),
    j.at("available").get<bool>(),
    resourceUnits(j,"compute_units"),resourceUnits(j,"memory_units")};
  if (s.producer != producer || j.at("service") != "/HELLO" || s.session != session || !s.sequence
      || s.published > now || s.expires <= now || s.expires-s.published != expectedTtl
      || s.computeUnits < 0 || s.computeUnits > 1024
      || s.memoryUnits < 0 || s.memoryUnits > 1024)
    throw std::runtime_error("snapshot binding/freshness/resources");
  return s;
}
inline Snapshot verifySnapshot(const ndn::Data& data, const ndn::security::Certificate& cert,
                               const std::string& session, uint64_t now,
                               uint64_t expectedTtl = TTL_NS) {
  const auto prefix=statusPrefix(cert.getIdentity());
  if (!ndn::Name(prefix).append(session).isPrefixOf(data.getName())
      || !ndn::security::verifySignature(data,cert))
    throw std::runtime_error("snapshot name/signature");
  const auto& b=data.getContent();
  if (b.value_size()>4096) throw std::runtime_error("snapshot too large");
  const auto s=decodeSnapshot(Json::parse(b.value(),b.value()+b.value_size()),
                              cert.getIdentity(),session,now,expectedTtl);
  if (expectedTtl == COARSE_AD_TTL_NS && data.getName().size() != prefix.size() + 2)
    throw std::runtime_error("coarse advertisement name");
  if(data.getName().size()==prefix.size()+2
     && (!data.getName()[-1].isVersion() || data.getName()[-1].toVersion()!=s.sequence))
    throw std::runtime_error("advertisement sequence/name binding");
  return s;
}
inline uint64_t readOrigin(const std::string& path) {
  std::ifstream file(path); uint64_t origin=0; file>>origin; return origin;
}
} // namespace dynamic204
