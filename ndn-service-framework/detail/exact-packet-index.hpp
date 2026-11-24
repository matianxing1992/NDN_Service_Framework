#ifndef NDNSF_DETAIL_EXACT_PACKET_INDEX_HPP
#define NDNSF_DETAIL_EXACT_PACKET_INDEX_HPP

#include <ndn-cxx/encoding/tlv.hpp>
#include <ndn-cxx/name.hpp>

#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
#include <stdexcept>
#include <utility>

namespace ndn_service_framework::detail {

/** Name-to-file-ordinal index for admitted original Data. Owned by one retention lease. */
class ExactPacketIndex
{
public:
  std::optional<std::uint64_t>
  find(const ndn::Name& name) const
  {
    if (const auto segment = canonicalSegment(name)) {
      const auto prefix = name.getPrefix(-1);
      const auto run = precedingRun(m_runs, prefix, *segment);
      if (run != m_runs.end() && *segment - run->first.second < run->second.count)
        return run->second.ordinal + (*segment - run->first.second);
      return std::nullopt;
    }
    const auto item = m_names.find(name);
    return item == m_names.end() ? std::nullopt : std::optional{item->second};
  }

  /** Stage one packet in append order against the available index budget. */
  void
  add(const ndn::Name& name, std::uint64_t ordinal, std::uint64_t budget)
  {
    if (const auto segment = canonicalSegment(name)) {
      auto prefix = name.getPrefix(-1);
      const auto run = precedingRun(m_runs, prefix, *segment);
      if (run != m_runs.end()) {
        const auto distance = *segment - run->first.second;
        if (distance < run->second.count)
          throw std::invalid_argument("exact packet retention repeats a committed name");
        if (distance == run->second.count && ordinal >= run->second.ordinal &&
            ordinal - run->second.ordinal == run->second.count) {
          ++run->second.count;
          return;
        }
      }
      const auto charge = 160 + 2 * prefix.wireEncode().size();
      checkBudget(charge, budget);
      m_runs.emplace(std::make_pair(std::move(prefix), *segment), Run{ordinal, 1});
      m_bytes += charge;
      return;
    }
    const auto charge = 128 + 2 * name.wireEncode().size();
    checkBudget(charge, budget);
    if (!m_names.emplace(name, ordinal).second)
      throw std::invalid_argument("exact packet retention repeats a committed name");
    m_bytes += charge;
  }

  /** Commit a staged, disjoint batch after the file append succeeds. */
  void
  commit(ExactPacketIndex&& batch, std::uint64_t firstOrdinal)
  {
    for (auto& item : batch.m_runs)
      item.second.ordinal += firstOrdinal;
    for (auto& item : batch.m_names)
      item.second += firstOrdinal;
    m_runs.merge(batch.m_runs);
    m_names.merge(batch.m_names);
    m_bytes += batch.m_bytes;
  }

  std::uint64_t
  bytes() const noexcept
  {
    return m_bytes;
  }

private:
  struct Run
  {
    std::uint64_t ordinal;
    std::uint64_t count;
  };
  using Runs = std::map<std::pair<ndn::Name, std::uint64_t>, Run>;

  static std::optional<std::uint64_t>
  canonicalSegment(const ndn::Name& name)
  {
    if (name.empty() || name[-1].type() != ndn::tlv::SegmentNameComponent)
      return std::nullopt;
    const auto& component = name[-1];
    const auto length = component.value_size();
    if (length != 1 && length != 2 && length != 4 && length != 8)
      return std::nullopt;
    const auto number = component.toNumber();
    return length == ndn::tlv::sizeOfNonNegativeInteger(number) ?
      std::optional{number} : std::nullopt;
  }

  template<typename RunMap>
  static auto
  precedingRun(RunMap& runs, const ndn::Name& prefix, std::uint64_t number)
    -> decltype(runs.begin())
  {
    const auto next = runs.upper_bound({prefix, number});
    if (next == runs.begin())
      return runs.end();
    const auto run = std::prev(next);
    return run->first.first == prefix ? run : runs.end();
  }

  void
  checkBudget(std::uint64_t charge, std::uint64_t budget) const
  {
    if (m_bytes > budget || charge > budget - m_bytes)
      throw std::length_error("exact packet retention index capacity");
  }

private:
  Runs m_runs;
  std::map<ndn::Name, std::uint64_t> m_names;
  std::uint64_t m_bytes = 0;
};

} // namespace ndn_service_framework::detail

#endif // NDNSF_DETAIL_EXACT_PACKET_INDEX_HPP
