#include "SecurityProfile.hpp"
#include "ProducerDataNaming.hpp"
#include "RequestConfidentiality.hpp"
#include "common.hpp"
#include <ndn-cxx/encoding/block-helpers.hpp>
#include <ndn-cxx/util/sha256.hpp>
#include <boost/asio/post.hpp>
#include <algorithm>
#include <condition_variable>
#include <cctype>
#include <list>
#include <map>
#include <mutex>
#include <stdexcept>

namespace ndn_service_framework {
NDN_LOG_INIT(ndn_service_framework.SecurityProfile);
namespace {
constexpr uint32_t VERSION = 0xF851, PRODUCER = 0xF852, CERTIFICATE = 0xF853, FLAGS = 0xF854;

uint64_t nowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string digestString(ndn::util::Sha256& hash)
{
  auto value = hash.toString();
  std::transform(value.begin(), value.end(), value.begin(),
    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return "sha256:" + value;
}

std::string digest(const ndn::Block& wire)
{
  ndn::util::Sha256 hash;
  hash.update(ndn::span<const uint8_t>(wire.data(), wire.size()));
  return digestString(hash);
}

void checkName(const ndn::Name& name, const ndn::Name& producer)
{
  const auto prefix = makeProducerDataPrefix(producer, "SECURITY")
    .append(makeNamingLabelComponent("encryption-profile"));
  if (!prefix.isPrefixOf(name) || name.size() != prefix.size() + 1 ||
      name[-1].type() != ndn::tlv::GenericNameComponent || name[-1].value_size() != 32)
    throw std::invalid_argument("invalid security profile reference");
  checkNamingNameSize(name);
}
} // namespace

ndn::Block ProducerSecurityProfile::wireEncode() const
{
  if (producer.empty() || certificate.getIdentity() != producer ||
      getCertificateKeyType(certificate) != ndn::KeyType::RSA ||
      (capabilities & ~SUPPORTED_CAPABILITIES) != 0)
    throw std::invalid_argument("invalid security profile owner, certificate or flags");
  ndn::Block wire(TYPE);
  wire.push_back(ndn::makeNonNegativeIntegerBlock(VERSION, 1));
  const auto name = producer.wireEncode();
  wire.push_back(ndn::makeBinaryBlock(PRODUCER, name.begin(), name.end()));
  const auto cert = certificate.wireEncode();
  wire.push_back(ndn::makeBinaryBlock(CERTIFICATE, cert.begin(), cert.end()));
  wire.push_back(ndn::makeNonNegativeIntegerBlock(FLAGS, capabilities));
  wire.encode();
  if (wire.size() > ndn::MAX_NDN_PACKET_SIZE)
    throw std::invalid_argument("security profile exceeds packet bound");
  return wire;
}

ProducerSecurityProfile ProducerSecurityProfile::wireDecode(const ndn::Block& wire)
{
  if (wire.type() != TYPE || wire.size() > ndn::MAX_NDN_PACKET_SIZE)
    throw std::invalid_argument("invalid security profile type or size");
  wire.parse();
  const auto& fields = wire.elements();
  if (fields.size() != 4 || fields[0].type() != VERSION ||
      ndn::readNonNegativeInteger(fields[0]) != 1 || fields[1].type() != PRODUCER ||
      fields[2].type() != CERTIFICATE || fields[3].type() != FLAGS)
    throw std::invalid_argument("invalid security profile fields");
  const auto flags = ndn::readNonNegativeInteger(fields[3]);
  if (flags > SUPPORTED_CAPABILITIES)
    throw std::invalid_argument("unknown security profile capability");
  const ndn::Block name(ndn::span<const uint8_t>(fields[1].value(), fields[1].value_size()));
  const ndn::Block cert(ndn::span<const uint8_t>(fields[2].value(), fields[2].value_size()));
  if (name.size() != fields[1].value_size() || cert.size() != fields[2].value_size())
    throw std::invalid_argument("trailing security profile certificate or producer");
  ProducerSecurityProfile profile;
  profile.producer = ndn::Name(name);
  profile.certificate = ndn::security::Certificate(cert);
  profile.capabilities = static_cast<uint32_t>(flags);
  if (profile.wireEncode() != wire)
    throw std::invalid_argument("noncanonical security profile");
  return profile;
}

ndn::Name ProducerSecurityProfile::getName() const
{
  const auto wire = wireEncode();
  const std::vector<uint8_t> bytes(wire.begin(), wire.end());
  const auto identity = computeNamingDigest("NDNSF-SECURITY-PROFILE-1", {{1, 4, bytes}});
  auto name = makeProducerDataPrefix(producer, "SECURITY")
    .append(makeNamingLabelComponent("encryption-profile"))
    .append(makeNamingDigestComponent(identity));
  checkNamingNameSize(name);
  return name;
}

EncryptionCertificateAdvertisement ProducerSecurityProfile::advertisement() const
{
  EncryptionCertificateAdvertisement result;
  result.certificateName = certificate.getName();
  result.certificateDigest = digest(certificate.wireEncode());
  const auto period = certificate.getValidityPeriod().getPeriod();
  result.validFromMs = boost::chrono::duration_cast<boost::chrono::milliseconds>(
    period.first.time_since_epoch()).count();
  result.validUntilMs = boost::chrono::duration_cast<boost::chrono::milliseconds>(
    period.second.time_since_epoch()).count();
  result.supportedEnvelopeAlgorithms = {"RSA-OAEP-SHA256"};
  return result;
}

std::string ProducerSecurityProfile::publicKeyDigest() const
{
  const auto key = certificate.getPublicKey();
  ndn::util::Sha256 hash;
  hash.update(ndn::span<const uint8_t>(key.data(), key.size()));
  return digestString(hash);
}

std::shared_ptr<const ndn::Data> ProducerSecurityProfile::makeData(
  ndn::KeyChain& keyChain, const ndn::security::SigningInfo& signingInfo,
  ndn::time::milliseconds freshness) const
{
  if (freshness <= ndn::time::milliseconds::zero())
    throw std::invalid_argument("security profile freshness must be positive");
  auto packet = std::make_shared<ndn::Data>(getName());
  const auto wire = wireEncode();
  packet->setContent(ndn::span<const uint8_t>(wire.data(), wire.size()));
  packet->setFreshnessPeriod(freshness);
  keyChain.sign(*packet, signingInfo);
  if (packet->wireEncode().size() > ndn::MAX_NDN_PACKET_SIZE)
    throw std::invalid_argument("signed security profile exceeds packet bound");
  return packet;
}

struct SecurityProfileStore::State : std::enable_shared_from_this<State>
{
  struct Pending
  {
    uint64_t ticket;
    ndn::Name producer;
    ndn::PendingInterestHandle handle;
    std::vector<Handler> handlers;
  };
  ndn::Face& face;
  std::shared_ptr<MessageValidator> validator;
  const size_t capacity;
  std::mutex mutex;
  std::condition_variable drained;
  bool closed = false;
  size_t activeDeliveries = 0;
  // Nested callbacks can enter another store before closing this one.
  static thread_local std::vector<const State*> deliveryStack;
  uint64_t nextTicket = 0, fetches = 0;
  std::map<ndn::Name, std::shared_ptr<const Record>> cache;
  std::list<ndn::Name> lru;
  std::map<ndn::Name, Pending> pending;

  State(ndn::Face& f, std::shared_ptr<MessageValidator> v, size_t cap)
    : face(f), validator(std::move(v)), capacity(cap) {}

  void post(std::function<void()> work)
  {
    // close() seals the borrowed Face before its caller can destroy it.
    std::lock_guard<std::mutex> lock(mutex);
    if (!closed) boost::asio::post(face.getIoContext(), std::move(work));
  }

  void deliver(std::vector<Handler> handlers, std::shared_ptr<const Record> record,
               const std::string& reason)
  {
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (closed) return;
      deliveryStack.push_back(this);
      ++activeDeliveries;
    }
    const auto release = [this] {
      deliveryStack.pop_back();
      std::lock_guard<std::mutex> lock(mutex);
      --activeDeliveries;
      drained.notify_all();
    };
    for (auto& handler : handlers) {
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (closed) break;
      }
      try { handler(record, reason); }
      catch (const std::exception& error) {
        NDN_LOG_WARN("Security profile consumer callback failed: " << error.what());
      }
      catch (...) { NDN_LOG_WARN("Security profile consumer callback failed"); }
    }
    release();
  }

  void finish(const ndn::Name& name, uint64_t ticket,
              std::shared_ptr<const Record> record, const std::string& reason)
  {
    std::vector<Handler> handlers;
    {
      std::lock_guard<std::mutex> lock(mutex);
      const auto it = pending.find(name);
      if (closed || it == pending.end() || it->second.ticket != ticket) return;
      handlers = std::move(it->second.handlers);
      pending.erase(it);
      if (record) {
        cache[name] = record;
        lru.remove(name);
        lru.push_front(name);
        while (cache.size() > capacity) {
          cache.erase(lru.back());
          lru.pop_back();
        }
      }
    }
    deliver(std::move(handlers), std::move(record), reason);
  }

  void receive(const ndn::Name& name, uint64_t ticket, const ndn::Data& data)
  {
    ndn::Name producer;
    {
      std::lock_guard<std::mutex> lock(mutex);
      const auto it = pending.find(name);
      if (closed || it == pending.end() || it->second.ticket != ticket) return;
      producer = it->second.producer;
    }
    auto packet = std::make_shared<const ndn::Data>(data);
    const auto weak = weak_from_this();
    validator->validate(*packet,
      [weak, name, ticket, producer, packet](const ndn::Data&) {
        const auto state = weak.lock();
        if (!state) return;
        state->post([weak, name, ticket, producer, packet] {
          const auto live = weak.lock();
          if (!live) return;
          try {
            if (packet->getName() != name ||
                !live->validator->isProducerSignerAuthorized(*packet, producer))
              throw std::invalid_argument("security profile signer or exact name mismatch");
            const auto& content = packet->getContent();
            const ndn::Block wire(ndn::span<const uint8_t>(content.value(), content.value_size()));
            if (wire.size() != content.value_size())
              throw std::invalid_argument("security profile trailing content");
            auto profile = ProducerSecurityProfile::wireDecode(wire);
            if (profile.producer != producer || profile.getName() != name ||
                !profile.advertisement().isValid(nowMs()))
              throw std::invalid_argument("security profile identity or validity mismatch");
            auto record = std::make_shared<Record>();
            record->packet = packet;
            record->advertisement = profile.advertisement();
            record->publicKeyDigest = profile.publicKeyDigest();
            record->publicKeyHex = selectionGatedHex(profile.certificate.getPublicKey());
            record->profile = std::move(profile);
            live->finish(name, ticket, std::move(record), "");
          }
          catch (const std::exception& error) { live->finish(name, ticket, nullptr, error.what()); }
        });
      },
      [weak, name, ticket](const ndn::Data&, const ndn::security::ValidationError& error) {
        const auto state = weak.lock();
        if (!state) return;
        const auto reason = error.getInfo();
        state->post([weak, name, ticket, reason] {
          if (const auto live = weak.lock()) live->finish(name, ticket, nullptr, reason);
        });
      });
  }
};

thread_local std::vector<const SecurityProfileStore::State*>
  SecurityProfileStore::State::deliveryStack;

SecurityProfileStore::SecurityProfileStore(ndn::Face& face,
  std::shared_ptr<MessageValidator> validator, size_t capacity)
{
  if (!validator || capacity == 0 || capacity > 1024)
    throw std::invalid_argument("invalid security profile cache configuration");
  m_state = std::make_shared<State>(face, std::move(validator), capacity);
}

SecurityProfileStore::~SecurityProfileStore() { close(); }

void SecurityProfileStore::resolve(const ndn::Name& name, const ndn::Name& producer,
  ndn::time::milliseconds timeout, Handler handler)
{
  checkName(name, producer);
  if (!handler || timeout <= ndn::time::milliseconds::zero())
    throw std::invalid_argument("invalid security profile resolver arguments");
  const auto state = m_state;
  std::shared_ptr<const Record> hit;
  uint64_t ticket = 0;
  std::string error;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->closed) throw std::logic_error("security profile store is closed");
    const auto cached = state->cache.find(name);
    if (cached != state->cache.end()) {
      const auto now = nowMs();
      const auto& advertisement = cached->second->advertisement;
      if (now >= advertisement.validFromMs && now < advertisement.validUntilMs) {
        hit = cached->second;
        state->lru.remove(name);
        state->lru.push_front(name);
      }
      else {
        state->cache.erase(cached);
        state->lru.remove(name);
        error = "cached security profile is expired";
      }
    }
    if (!hit && error.empty()) {
      const auto pending = state->pending.find(name);
      if (pending != state->pending.end()) {
        if (pending->second.producer != producer || pending->second.handlers.size() >= 64)
          error = "security profile waiters exceed bounds";
        else {
          pending->second.handlers.push_back(std::move(handler));
          return;
        }
      }
      else if (state->pending.size() >= 64) error = "security profile fetches exceed bounds";
      else {
        ticket = ++state->nextTicket;
        state->pending.emplace(name, State::Pending{ticket, producer, {}, {std::move(handler)}});
        ++state->fetches;
      }
    }
  }
  if (hit || !error.empty()) { state->deliver({std::move(handler)}, std::move(hit), error); return; }
  ndn::Interest interest(name);
  interest.setCanBePrefix(false);
  interest.setMustBeFresh(false);
  interest.setInterestLifetime(timeout);
  const std::weak_ptr<State> weak(state);
  try {
    const auto handle = state->face.expressInterest(interest,
      [weak, name, ticket](const ndn::Interest&, const ndn::Data& packet) {
        if (const auto live = weak.lock()) live->receive(name, ticket, packet);
      },
      [weak, name, ticket](const ndn::Interest&, const ndn::lp::Nack&) {
        if (const auto live = weak.lock()) live->finish(name, ticket, nullptr, "security profile Nack");
      },
      [weak, name, ticket](const ndn::Interest&) {
        if (const auto live = weak.lock()) live->finish(name, ticket, nullptr, "security profile timeout");
      });
    std::lock_guard<std::mutex> lock(state->mutex);
    const auto pending = state->pending.find(name);
    if (!state->closed && pending != state->pending.end() && pending->second.ticket == ticket)
      pending->second.handle = handle;
    else handle.cancel();
  }
  catch (const std::exception& error) { state->finish(name, ticket, nullptr, error.what()); }
}

void SecurityProfileStore::invalidate()
{
  const auto state = m_state;
  std::vector<Handler> handlers;
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->cache.clear();
    state->lru.clear();
    for (auto& [name, pending] : state->pending) {
      pending.handle.cancel();
      for (auto& handler : pending.handlers) handlers.push_back(std::move(handler));
    }
    state->pending.clear();
  }
  state->deliver(std::move(handlers), nullptr, "security profile cache invalidated");
}

void SecurityProfileStore::close()
{
  const auto state = m_state;
  if (!state) return;
  std::unique_lock<std::mutex> lock(state->mutex);
  state->closed = true;
  for (auto& [name, pending] : state->pending) pending.handle.cancel();
  state->pending.clear();
  state->cache.clear();
  state->lru.clear();
  // A consumer may cancel from its own callback. Draining that callback here
  // would wait for this close() to return. A subsequent off-thread close still
  // waits for every delivery, including this one, before owner destruction.
  if (std::find(State::deliveryStack.begin(), State::deliveryStack.end(), state.get()) !=
      State::deliveryStack.end()) return;
  state->drained.wait(lock, [&] { return state->activeDeliveries == 0; });
}

size_t SecurityProfileStore::size() const
{
  std::lock_guard<std::mutex> lock(m_state->mutex);
  return m_state->cache.size();
}

uint64_t SecurityProfileStore::fetchCount() const
{
  std::lock_guard<std::mutex> lock(m_state->mutex);
  return m_state->fetches;
}
} // namespace ndn_service_framework
