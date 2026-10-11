#include "ndnsf-distributed-repo/RepoPacketServer.hpp"
#include "ndnsf-distributed-repo/RepoCore.hpp"
#include <ndn-cxx/data.hpp>
#include <stdexcept>
#include <vector>
#include <algorithm>

namespace ndnsf_distributed_repo {
struct RepoPacketServer::State
{
  State(ndn::Face& face, std::shared_ptr<RepoCore> store)
    : face(face), store(std::move(store)) {}
  ndn::Face& face;
  std::shared_ptr<RepoCore> store;
  bool closed = false;
  std::vector<ndn::ScopedRegisteredPrefixHandle> registrations;
};

RepoPacketServer::RepoPacketServer(ndn::Face& face, std::shared_ptr<RepoCore> store)
  : m_state(std::make_shared<State>(face, std::move(store)))
{
  if (!m_state->store)
    throw std::invalid_argument("Repo packet server requires a store");
}

RepoPacketServer::~RepoPacketServer() { close(); }

void
RepoPacketServer::servePrefix(const ndn::Name& prefix,
                            std::function<void()> onReady,
                            std::function<void(const std::string&)> onFailure)
{
  if (m_state->closed || prefix.empty())
    throw std::invalid_argument("Repo packet server closed or prefix is root");
  bool hasPacket = false;
  for (const auto& record : m_state->store->list()) {
    if ((record.objectType == "ndn-data-wire" || record.objectType == "original-packet-set") &&
        std::any_of(record.packetNames.begin(), record.packetNames.end(),
          [&](const std::string& name) { return prefix.isPrefixOf(ndn::Name(name)); })) {
      hasPacket = true;
      break;
    }
  }
  if (!hasPacket)
    throw std::invalid_argument("Repo serving prefix has no committed original packet");
  const std::weak_ptr<State> weak = m_state;
  m_state->registrations.emplace_back(m_state->face.setInterestFilter(prefix,
    [weak](const ndn::InterestFilter&, const ndn::Interest& interest) {
      const auto state = weak.lock();
      if (!state || state->closed)
        return;
      auto name = interest.getName();
      if (!name.empty() && name[-1].isImplicitSha256Digest())
        name = name.getPrefix(-1);
      const auto sendOriginal = [&](const ndn::Name& candidate) {
        const auto wire = state->store->getDataPacket(
          candidate.toUri(ndn::name::UriFormat::CANONICAL));
        const ndn::Data data(ndn::Block(ndn::span<const uint8_t>(wire.data(), wire.size())));
        if (data.getName() != candidate || !interest.matchesData(data))
          return false;
        state->face.put(data);
        return true;
      };
      try {
        if (state->store->hasDataPacket(name.toUri(ndn::name::UriFormat::CANONICAL))) {
          sendOriginal(name);
          return;
        }
        if (!interest.getCanBePrefix())
          return;
        for (const auto& record : state->store->list()) {
          if (record.objectType != "ndn-data-wire" && record.objectType != "original-packet-set")
            continue;
          for (const auto& packet : record.packetNames) {
            const ndn::Name candidate(packet);
            if (name.isPrefixOf(candidate) && sendOriginal(candidate))
              return;
          }
        }
      }
      catch (const std::exception&) {
        // Unknown/corrupt records produce no replacement or fabricated Data.
      }
    },
    [weak, onReady = std::move(onReady)](const ndn::Name&) {
      const auto state = weak.lock();
      if (state && !state->closed && onReady)
        onReady();
    },
    [weak, onFailure = std::move(onFailure)](const ndn::Name&, const std::string& reason) {
      const auto state = weak.lock();
      if (state && !state->closed && onFailure)
        onFailure(reason);
    }));
}

void
RepoPacketServer::close() noexcept
{
  m_state->closed = true;
  m_state->registrations.clear();
}
} // namespace ndnsf_distributed_repo
