#ifndef NDNSF_PUBLISH_MESSAGE_BRIDGE_HPP
#define NDNSF_PUBLISH_MESSAGE_BRIDGE_HPP

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include <ndn-cxx/encoding/block.hpp>
#include <ndn-cxx/name.hpp>

#include <ndn-service-framework/NDNSFMessages.hpp>

namespace muas {

class RuntimeBackend
{
public:
  virtual ~RuntimeBackend() = default;

  virtual void
  publish(const ndn::Name& messageName,
          const ndn::Name& messageNameWithoutPrefix,
          const ndn::Block& encodedBlock) = 0;
};

class PublishMessageBridge
{
public:
  struct PublishedRecord
  {
    ndn::Name messageName;
    ndn::Name messageNameWithoutPrefix;
    ndn::Block encodedBlock;
  };

  void
  setRuntimeBackend(std::shared_ptr<RuntimeBackend> backend)
  {
    m_runtimeBackend = std::move(backend);
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

    if (m_runtimeBackend) {
      const auto& publishedRecord = m_publishedRecords.back();
      m_runtimeBackend->publish(publishedRecord.messageName,
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
  std::shared_ptr<RuntimeBackend> m_runtimeBackend;
};

} // namespace muas

#endif // NDNSF_PUBLISH_MESSAGE_BRIDGE_HPP
