#ifndef NDNSF_PUBLISH_MESSAGE_BRIDGE_HPP
#define NDNSF_PUBLISH_MESSAGE_BRIDGE_HPP

#include <cstddef>
#include <functional>
#include <utility>
#include <vector>

#include <ndn-cxx/encoding/block.hpp>
#include <ndn-cxx/name.hpp>

#include <ndn-service-framework/NDNSFMessages.hpp>

namespace muas {

class PublishMessageBridge
{
public:
  using RuntimePublisher =
      std::function<void(const ndn::Name& messageName,
                         const ndn::Name& messageNameWithoutPrefix,
                         const ndn::Block& encodedBlock)>;

  struct PublishedRecord
  {
    ndn::Name messageName;
    ndn::Name messageNameWithoutPrefix;
    ndn::Block encodedBlock;
  };

  void
  setRuntimePublisher(RuntimePublisher publisher)
  {
    m_runtimePublisher = std::move(publisher);
  }

  void
  publish(const ndn::Name& messageName,
          const ndn::Name& messageNameWithoutPrefix,
          const ndn_service_framework::AbstractMessage& message)
  {
    m_publishedRecords.push_back(PublishedRecord{
        messageName,
        messageNameWithoutPrefix,
        message.WireEncode()});

    if (m_runtimePublisher) {
      const auto& publishedRecord = m_publishedRecords.back();
      m_runtimePublisher(publishedRecord.messageName,
                         publishedRecord.messageNameWithoutPrefix,
                         publishedRecord.encodedBlock);
    }
  }

  size_t
  getPublishedCount() const
  {
    return m_publishedRecords.size();
  }

  ndn::Name
  getLastPublishedName() const
  {
    if (m_publishedRecords.empty()) {
      return ndn::Name();
    }

    return m_publishedRecords.back().messageName;
  }

  ndn::Name
  getLastPublishedNameWithoutPrefix() const
  {
    if (m_publishedRecords.empty()) {
      return ndn::Name();
    }

    return m_publishedRecords.back().messageNameWithoutPrefix;
  }

  const std::vector<PublishedRecord>&
  getPublishedRecords() const
  {
    return m_publishedRecords;
  }

  void
  clear()
  {
    m_publishedRecords.clear();
  }

private:
  std::vector<PublishedRecord> m_publishedRecords;
  RuntimePublisher m_runtimePublisher;
};

} // namespace muas

#endif // NDNSF_PUBLISH_MESSAGE_BRIDGE_HPP
