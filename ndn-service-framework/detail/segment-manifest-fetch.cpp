#include "segment-manifest-fetch.hpp"

#include "../common.hpp"

#include <algorithm>
#include <stdexcept>

namespace ndn_service_framework::detail {

SegmentManifestFetch::SegmentManifestFetch(ndn::Face& face,
                                          std::shared_ptr<MessageValidator> validator,
                                          ndn::time::milliseconds lifetime, uint32_t window,
                                          GroupAt groupAt, Consume consume, Finish finish)
  : m_face(face)
  , m_validator(std::move(validator))
  , m_lifetime(lifetime)
  , m_window(window)
  , m_groupAt(std::move(groupAt))
  , m_consume(std::move(consume))
  , m_finish(std::move(finish))
{
}

void
SegmentManifestFetch::start(const ndn::Data* initialRoot)
{
  startGroup(initialRoot);
}

void
SegmentManifestFetch::expireAfter(ndn::Scheduler& scheduler, ndn::time::milliseconds remaining)
{
  auto weak = weak_from_this();
  m_expiry = scheduler.schedule(remaining, [weak] {
    if (const auto self = weak.lock())
      self->finish("signed segment manifest acquisition expired");
  });
}

void
SegmentManifestFetch::stop()
{
  m_stopped = true;
  m_expiry.cancel();
  m_pending.clear();
}

void
SegmentManifestFetch::finish(std::string error)
{
  if (m_stopped) return;
  stop();
  m_finish(std::move(error), m_roots.size(), m_verifiedMembers);
}

void
SegmentManifestFetch::startGroup(const ndn::Data* initialRoot)
{
  m_group = m_groupAt(m_groupId);
  if (!m_group) {
    if (!std::all_of(m_roots.begin(), m_roots.end(),
                     [](const auto& root) { return root.isCurrent(); })) {
      throw std::runtime_error("segment manifest trust expired");
    }
    finish({});
    return;
  }
  m_phase = Phase::ROOT;
  m_cursor = 0;
  m_next = 0;
  m_verifier.reset();
  if (initialRoot) {
    receive(0, *initialRoot);
  }
  else {
    request(m_group->scope.rootName(), 0, 1);
  }
}

void
SegmentManifestFetch::request(const ndn::Name& name, uint32_t index, uint32_t attempt)
{
  ndn::Interest interest(name);
  interest.setCanBePrefix(false);
  interest.setMustBeFresh(true);
  interest.setInterestLifetime(m_lifetime);
  const auto weak = weak_from_this();
  const auto retry = [weak, index, attempt](const ndn::Interest& expired) {
    const auto self = weak.lock();
    if (!self || self->m_stopped) return;
    try {
      self->m_pending.erase(index);
      constexpr uint32_t MAX_ATTEMPTS = 3;
      if (attempt == MAX_ATTEMPTS)
        throw std::runtime_error("signed segment manifest packet unavailable");
      self->request(expired.getName(), index, attempt + 1);
    }
    catch (const std::exception& failure) {
      self->finish(failure.what());
    }
  };
  m_pending.emplace(index, m_face.expressInterest(interest,
    [weak, index](const ndn::Interest& request, const ndn::Data& packet) {
      const auto self = weak.lock();
      if (!self || self->m_stopped) return;
      try {
        self->m_pending.erase(index);
        if (!request.matchesData(packet))
          throw std::runtime_error("signed segment manifest packet name or digest mismatch");
        self->receive(index, packet);
      }
      catch (const std::exception& failure) {
        self->finish(failure.what());
      }
    },
    [retry](const ndn::Interest& expired, const ndn::lp::Nack&) { retry(expired); }, retry));
}

void
SegmentManifestFetch::receive(uint32_t index, const ndn::Data& packet)
{
  if (m_phase == Phase::ROOT) {
    const auto weak = weak_from_this();
    m_validator->validateManifestRoot(packet, m_group->scope,
      [weak](std::optional<ValidatedManifestRoot> root) {
        const auto self = weak.lock();
        if (!self || self->m_stopped) return;
        try {
          if (!root)
            throw std::runtime_error("signed segment manifest root was rejected");
          self->m_verifier.emplace(*root, self->m_group->scope, self->m_group->limits);
          if (!self->m_verifier->applicationManifest().empty())
            throw std::runtime_error("unexpected application metadata in segment manifest");
          self->m_roots.push_back(std::move(*root));
          self->m_phase = self->m_verifier->pageCount() == 0 ? Phase::SOURCES : Phase::PAGES;
          self->pump();
        }
        catch (const std::exception& failure) {
          self->finish(failure.what());
        }
      });
    return;
  }
  if (m_phase == Phase::PAGES) {
    m_verifier->acceptPage(index, packet);
    ++m_verifiedMembers;
    ++m_cursor;
  }
  else {
    if (!m_verifier->matchesSource(index, packet))
      throw std::runtime_error("signed segment manifest source digest mismatch");
    ++m_verifiedMembers;
    m_ready.emplace(index, packet.getContent());
    for (auto it = m_ready.find(m_cursor); it != m_ready.end(); it = m_ready.find(m_cursor)) {
      m_consume(it->second);
      m_ready.erase(it);
      ++m_cursor;
    }
  }
  pump();
}

void
SegmentManifestFetch::pump()
{
  if (!m_verifier->isCurrent())
    throw std::runtime_error("segment manifest trust expired");
  const bool pages = m_phase == Phase::PAGES;
  const auto count = pages ? m_verifier->pageCount() : m_verifier->sourceCount();
  if (m_cursor == count) {
    if (pages) {
      m_phase = Phase::SOURCES;
      m_cursor = 0;
      m_next = 0;
      pump();
    }
    else {
      ++m_groupId;
      startGroup();
    }
    return;
  }
  while (m_next < std::min(count, m_cursor + m_window)) {
    const auto index = m_next++;
    request(pages ? m_verifier->pageFullName(index) : m_verifier->sourceFullName(index),
            index, 1);
  }
}

} // namespace ndn_service_framework::detail
