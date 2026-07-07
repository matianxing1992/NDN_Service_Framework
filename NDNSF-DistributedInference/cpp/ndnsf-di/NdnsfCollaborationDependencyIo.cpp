#include "NDNSF-DistributedInference/cpp/ndnsf-di/NdnsfCollaborationDependencyIo.hpp"

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace ndnsf::di {
namespace {

std::string
tensorList(const std::vector<std::string>& tensors)
{
  std::ostringstream os;
  for (std::size_t i = 0; i < tensors.size(); ++i) {
    if (i > 0) {
      os << ",";
    }
    os << tensors[i];
  }
  return os.str();
}

bool
truthyEnv(const char* name)
{
  const char* value = std::getenv(name);
  if (value == nullptr) {
    return false;
  }
  const std::string text(value);
  return !(text.empty() || text == "0" || text == "false" || text == "FALSE" ||
           text == "off" || text == "OFF");
}

bool
streamDependencyTraceEnabled()
{
  return truthyEnv("NDNSF_DI_RUNTIME_TIMING") ||
         truthyEnv("NDNSF_DI_STREAM_DEPENDENCY_TRACE");
}

void
logStreamDependency(const std::string& sessionId,
                    const DependencyEdge& edge,
                    const char* direction,
                    const char* mode,
                    std::size_t payloadBytes,
                    std::size_t wireBytes,
                    const char* status)
{
  if (!streamDependencyTraceEnabled()) {
    return;
  }
  const auto envelopeBytes = wireBytes > payloadBytes ? wireBytes - payloadBytes : 0;
  std::cout << "\nNDNSF_DI_STREAM_DEPENDENCY"
            << " session=" << sessionId
            << " scope=" << edge.scope
            << " producer=" << edge.producerRole
            << " consumer=" << edge.consumerRole
            << " mode=" << mode
            << " direction=" << direction
            << " payload_bytes=" << payloadBytes
            << " wire_bytes=" << wireBytes
            << " envelope_bytes=" << envelopeBytes
            << " planned_name=" << (edge.plannedDataName.empty() ? "none" : edge.plannedDataName)
            << " status=" << status
            << std::endl;
}

std::string
bundleNameFor(const DependencyEdge& edge, const TensorBundle& bundle)
{
  if (!bundle.name.empty()) {
    return bundle.name;
  }
  if (edge.tensors.size() == 1) {
    return edge.tensors.front();
  }
  return edge.plannedDataName;
}

std::string
decodedBundleNameFor(const DependencyEdge& edge,
                     const ndn_service_framework::StreamChunk& chunk)
{
  const auto found = chunk.metadata.find("bundleName");
  if (found != chunk.metadata.end() && !found->second.empty()) {
    return found->second;
  }
  if (edge.tensors.size() == 1) {
    return edge.tensors.front();
  }
  return edge.plannedDataName;
}

} // namespace

NdnsfCollaborationDependencyIo::NdnsfCollaborationDependencyIo(
  ndn_service_framework::ServiceProvider::CollaborationContext& ctx,
  int fetchTimeoutMs,
  std::size_t maxSegmentSize,
  int freshnessMs,
  bool streamChunkDependencies)
  : m_ctx(ctx)
  , m_fetchTimeoutMs(fetchTimeoutMs)
  , m_maxSegmentSize(maxSegmentSize)
  , m_freshnessMs(freshnessMs)
  , m_streamChunkDependencies(streamChunkDependencies)
{
}

ndn::Buffer
NdnsfCollaborationDependencyIo::encodeTensorBundleAsStreamChunk(
  const std::string& sessionId,
  const DependencyEdge& edge,
  const TensorBundle& bundle)
{
  ndn_service_framework::StreamChunk chunk;
  chunk.streamId = edge.plannedDataName.empty()
    ? sessionId + ":" + edge.scope
    : edge.plannedDataName;
  chunk.sessionEpoch = 1;
  chunk.seq = 0;
  chunk.payload = bundle.payload;
  chunk.contentType = StreamTensorBundleContentType;
  chunk.captureMs = ndn_service_framework::streamNowMs();
  chunk.segmentIndex = 0;
  chunk.segmentCount = 1;
  chunk.keyChunk = true;
  chunk.metadata["sessionId"] = sessionId;
  chunk.metadata["scope"] = edge.scope;
  chunk.metadata["producerRole"] = edge.producerRole;
  chunk.metadata["consumerRole"] = edge.consumerRole;
  chunk.metadata["plannedDataName"] = edge.plannedDataName;
  chunk.metadata["bundleName"] = bundleNameFor(edge, bundle);
  chunk.metadata["tensors"] = tensorList(edge.tensors);

  const auto block = chunk.wireEncode();
  return ndn::Buffer(block.data(), block.size());
}

TensorBundle
NdnsfCollaborationDependencyIo::decodeTensorBundleFromStreamChunk(
  const std::string& sessionId,
  const DependencyEdge& edge,
  const ndn::Buffer& payload)
{
  ndn::Block block(ndn::span<const uint8_t>(payload.data(), payload.size()));
  ndn_service_framework::StreamChunk chunk;
  if (!chunk.wireDecode(block)) {
    throw std::runtime_error("dependency payload is not an NDNSF StreamChunk");
  }
  if (chunk.contentType != StreamTensorBundleContentType) {
    throw std::runtime_error("dependency StreamChunk has unexpected content type: " +
                             chunk.contentType);
  }
  if (chunk.segmentIndex != 0 || chunk.segmentCount != 1) {
    throw std::runtime_error("dependency StreamChunk must contain one complete tensor bundle");
  }
  if (!edge.plannedDataName.empty() && chunk.streamId != edge.plannedDataName) {
    throw std::runtime_error("dependency StreamChunk streamId does not match plannedDataName");
  }
  const auto scopeIt = chunk.metadata.find("scope");
  if (scopeIt != chunk.metadata.end() && scopeIt->second != edge.scope) {
    throw std::runtime_error("dependency StreamChunk scope metadata mismatch");
  }
  const auto sessionIt = chunk.metadata.find("sessionId");
  if (sessionIt != chunk.metadata.end() && sessionIt->second != sessionId) {
    throw std::runtime_error("dependency StreamChunk session metadata mismatch");
  }

  TensorBundle bundle;
  bundle.name = decodedBundleNameFor(edge, chunk);
  bundle.payload = std::move(chunk.payload);
  bundle.expectedSegments = edge.expectedSegments;
  bundle.expectedBytes = edge.expectedBytes;
  return bundle;
}

std::future<TensorBundle>
NdnsfCollaborationDependencyIo::prefetchInput(const std::string& sessionId,
                                              const DependencyEdge& edge)
{
  if (edge.plannedDataName.empty()) {
    throw std::invalid_argument(
      "NdnsfCollaborationDependencyIo requires plannedDataName for input " +
      edge.scope);
  }
  return std::async(std::launch::async, [this, sessionId, edge] {
    auto payload = m_ctx.fetchLarge(
      ndn::Name(edge.plannedDataName),
      edge.scope,
      m_fetchTimeoutMs,
      edge.expectedSegments);
    if (!payload) {
      throw std::runtime_error(
        "failed to fetch planned dependency object: " +
        edge.plannedDataName);
    }
    if (m_streamChunkDependencies) {
      try {
        auto bundle = decodeTensorBundleFromStreamChunk(sessionId, edge, *payload);
        logStreamDependency(sessionId,
                            edge,
                            "fetch",
                            "streamchunk",
                            bundle.payload.size(),
                            payload->size(),
                            "ok");
        return bundle;
      }
      catch (...) {
        logStreamDependency(sessionId,
                            edge,
                            "fetch",
                            "streamchunk",
                            0,
                            payload->size(),
                            "decode-error");
        throw;
      }
    }
    TensorBundle bundle;
    bundle.name = edge.tensors.size() == 1 ? edge.tensors.front() : edge.plannedDataName;
    bundle.payload.assign(payload->data(), payload->data() + payload->size());
    bundle.expectedSegments = edge.expectedSegments;
    bundle.expectedBytes = edge.expectedBytes;
    logStreamDependency(sessionId,
                        edge,
                        "fetch",
                        "raw",
                        bundle.payload.size(),
                        payload->size(),
                        "ok");
    return bundle;
  });
}

void
NdnsfCollaborationDependencyIo::publishOutput(const std::string& sessionId,
                                              const DependencyEdge& edge,
                                              const TensorBundle& bundle)
{
  const auto payload = m_streamChunkDependencies
    ? encodeTensorBundleAsStreamChunk(sessionId, edge, bundle)
    : ndn::Buffer(bundle.payload.data(), bundle.payload.size());
  logStreamDependency(sessionId,
                      edge,
                      "publish",
                      m_streamChunkDependencies ? "streamchunk" : "raw",
                      bundle.payload.size(),
                      payload.size(),
                      "ok");
  if (edge.plannedDataName.empty()) {
    m_ctx.publishLarge(
      edge.scope,
      edge.producerRole.empty() ? ndn::Name("/output") : ndn::Name(edge.producerRole),
      payload,
      m_maxSegmentSize,
      m_freshnessMs);
    return;
  }
  m_ctx.publishLargeNamed(
    edge.scope,
    ndn::Name(edge.plannedDataName),
    payload,
    m_maxSegmentSize,
    m_freshnessMs);
}

} // namespace ndnsf::di
