#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/util/logger.hpp>
#include <ndn-cxx/util/scheduler.hpp>
#include <ndn-svs/security-options.hpp>
#include <ndn-svs/svspubsub.hpp>

#include <boost/asio/io_context.hpp>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

NDN_LOG_INIT(ndn_service_framework.AppSvsLatency);

volatile std::sig_atomic_t g_stop = 0;

void
handleSignal(int)
{
  g_stop = 1;
}

uint64_t
nowMicroseconds()
{
  return static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
}

ndn::svs::SecurityOptions
makeSecurityOptions(ndn::KeyChain& keyChain)
{
  ndn::svs::SecurityOptions options(keyChain);
  options.interestSigner = std::make_shared<ndn::svs::BaseSigner>();
  options.dataSigner->signingInfo = ndn::security::signingWithSha256();
  options.pubSigner->signingInfo = ndn::security::signingWithSha256();
  options.validator = std::make_shared<ndn::svs::BaseValidator>();
  options.encapsulatedDataValidator = std::make_shared<ndn::svs::BaseValidator>();
  return options;
}

std::vector<std::string>
split(const std::string& text, char delimiter)
{
  std::vector<std::string> fields;
  std::stringstream ss(text);
  std::string field;
  while (std::getline(ss, field, delimiter)) {
    fields.push_back(field);
  }
  return fields;
}

std::string
payloadOf(const ndn::svs::SVSPubSub::SubscriptionData& publication)
{
  return std::string(reinterpret_cast<const char*>(publication.data.data()),
                     publication.data.size());
}

std::string
namePrefixToRegex(const ndn::Name& prefix)
{
  std::string regex = "^";
  for (const auto& component : prefix) {
    regex += "<" + component.toUri() + ">";
  }
  regex += "(<>*)$";
  return regex;
}

void
publishText(ndn::svs::SVSPubSub& svs, const ndn::Name& name, const std::string& payload)
{
  svs.publish(name,
              ndn::span<const uint8_t>(
                reinterpret_cast<const uint8_t*>(payload.data()), payload.size()));
}

std::string
getArg(int argc, char** argv, const std::string& key, const std::string& fallback)
{
  for (int i = 1; i + 1 < argc; ++i) {
    if (argv[i] == key) {
      return argv[i + 1];
    }
  }
  return fallback;
}

int
getIntArg(int argc, char** argv, const std::string& key, int fallback)
{
  const auto value = getArg(argc, argv, key, "");
  if (value.empty()) {
    return fallback;
  }
  return std::stoi(value);
}

bool
hasFlag(int argc, char** argv, const std::string& key)
{
  for (int i = 1; i < argc; ++i) {
    if (argv[i] == key) {
      return true;
    }
  }
  return false;
}

void
printUsage(const char* program)
{
  NDN_LOG_ERROR("Usage: " << program << " --role ping|pong|pub|sub [options]");
  NDN_LOG_ERROR("Options:");
  NDN_LOG_ERROR("  --sync-prefix NAME      SVS sync/group prefix, default /example/hello/group");
  NDN_LOG_ERROR("  --node-prefix NAME      local SVS producer prefix");
  NDN_LOG_ERROR("  --peer-prefix NAME      peer SVS producer prefix");
  NDN_LOG_ERROR("  --count N               ping count, default 30");
  NDN_LOG_ERROR("  --interval-ms N         ping interval, default 100");
  NDN_LOG_ERROR("  --startup-ms N          wait before first ping, default 2000");
  NDN_LOG_ERROR("  --timeout-ms N          stop after timeout, default 20000");
  NDN_LOG_ERROR("  --parallel-sync        enable experimental ndn-svs parallel sync processing");
  NDN_LOG_ERROR("  --parallel-workers N   worker count for --parallel-sync, default 2");
  NDN_LOG_ERROR("  --parallel-queue N     bounded queue size for --parallel-sync, default 128");
  NDN_LOG_ERROR("  --sync-batching        coalesce local publication-triggered sync interests");
  NDN_LOG_ERROR("  --sync-batch-ms N      batching window for --sync-batching, default 5");
  NDN_LOG_ERROR("  --csv                   emit SVS_LATENCY_CSV/SVS_ONEWAY_CSV log rows");
}

} // namespace

int
main(int argc, char** argv)
{
  std::signal(SIGINT, handleSignal);
  std::signal(SIGTERM, handleSignal);

  const std::string role = getArg(argc, argv, "--role", "");
  if (role != "ping" && role != "pong" && role != "pub" && role != "sub") {
    printUsage(argv[0]);
    return 2;
  }

  const ndn::Name syncPrefix(getArg(argc, argv, "--sync-prefix", "/example/hello/group"));
  const ndn::Name defaultPingPrefix("/example/hello/user/svs-latency");
  const ndn::Name defaultPongPrefix("/example/hello/provider/A/svs-latency");
  const ndn::Name nodePrefix(getArg(argc, argv, "--node-prefix",
                                    (role == "ping" || role == "pub") ?
                                      defaultPingPrefix.toUri() : defaultPongPrefix.toUri()));
  const ndn::Name peerPrefix(getArg(argc, argv, "--peer-prefix",
                                    (role == "ping" || role == "pub") ?
                                      defaultPongPrefix.toUri() : defaultPingPrefix.toUri()));
  const int count = getIntArg(argc, argv, "--count", 30);
  const int intervalMs = getIntArg(argc, argv, "--interval-ms", 100);
  const int startupMs = getIntArg(argc, argv, "--startup-ms", 2000);
  const int timeoutMs = getIntArg(argc, argv, "--timeout-ms", 20000);
  const bool parallelSync = hasFlag(argc, argv, "--parallel-sync") ||
                            std::getenv("SVS_PARALLEL_SYNC") != nullptr;
  const int parallelWorkers = getIntArg(argc, argv, "--parallel-workers", 2);
  const int parallelQueue = getIntArg(argc, argv, "--parallel-queue", 128);
  const bool syncBatching = hasFlag(argc, argv, "--sync-batching") ||
                            std::getenv("SVS_SYNC_BATCHING") != nullptr;
  const int syncBatchMs = getIntArg(argc, argv, "--sync-batch-ms", 5);
  const bool csv = hasFlag(argc, argv, "--csv");

  if (count <= 0 || intervalMs <= 0 || startupMs < 0 || timeoutMs <= 0 || syncBatchMs < 0) {
    NDN_LOG_ERROR("count, interval-ms, startup-ms, timeout-ms, and sync-batch-ms must be valid");
    return 2;
  }

  ndn::Face face;
  ndn::KeyChain keyChain("pib-memory:svs-latency", "tpm-memory:svs-latency");
  auto securityOptions = makeSecurityOptions(keyChain);
  ndn::svs::SVSPubSubOptions svsOptions;
  svsOptions.useTimestamp = false;

  ndn::svs::SVSPubSub svs(syncPrefix,
                          nodePrefix,
                          face,
                          [] (const std::vector<ndn::svs::MissingDataInfo>&) {},
                          svsOptions,
                          securityOptions);
  if (parallelSync) {
    svs.getSVSync().getCore().setParallelSyncProcessing(true,
                                                        static_cast<size_t>(parallelWorkers),
                                                        static_cast<size_t>(parallelQueue));
  }
  if (syncBatching) {
    svs.getSVSync().getCore().setSyncInterestBatching(true,
                                                      ndn::time::milliseconds(syncBatchMs));
  }

  ndn::Scheduler scheduler(face.getIoContext());
  std::map<int, uint64_t> sentAtUs;
  int sent = 0;
  int received = 0;
  int echoed = 0;

  if (csv) {
    if (role == "ping") {
      NDN_LOG_INFO("SVS_LATENCY_CSV role,event,seq,ping_send_us,pong_recv_us,pong_send_us,"
                   "ping_recv_us,forward_ms,backward_ms,rtt_ms,name");
    }
    else if (role == "sub") {
      NDN_LOG_INFO("SVS_ONEWAY_CSV role,event,seq,publish_us,receive_us,oneway_ms,name");
    }
    else {
      NDN_LOG_INFO("SVS_LATENCY_CSV role,event,seq,ping_send_us,pong_recv_us,pong_send_us,"
                   "forward_ms,name");
    }
  }

  const std::string peerRegex = namePrefixToRegex(peerPrefix);
  NDN_LOG_INFO("App_SvsLatency subscriptionRegex=" << peerRegex);
  svs.subscribeWithRegex(
    ndn::Regex(peerRegex),
    [&] (const ndn::svs::SVSPubSub::SubscriptionData& publication) {
      const auto payload = payloadOf(publication);
      const auto fields = split(payload, ',');
      const uint64_t recvUs = nowMicroseconds();

      if (role == "pong") {
        if (fields.size() != 3 || fields[0] != "ping") {
          return;
        }
        const int seq = std::stoi(fields[1]);
        const uint64_t pingSendUs = std::stoull(fields[2]);
        const uint64_t pongRecvUs = recvUs;
        const uint64_t pongSendUs = nowMicroseconds();
        const double forwardMs =
          pongRecvUs >= pingSendUs ? static_cast<double>(pongRecvUs - pingSendUs) / 1000.0 : 0.0;
        const ndn::Name echoName = ndn::Name(nodePrefix).append("echo").appendNumber(seq);
        publishText(svs, echoName,
                    "echo," + std::to_string(seq) + "," +
                    std::to_string(pingSendUs) + "," +
                    std::to_string(pongRecvUs) + "," +
                    std::to_string(pongSendUs));
        ++echoed;
        if (csv) {
          NDN_LOG_INFO("SVS_LATENCY_CSV pong,echo," << seq << "," << pingSendUs << ","
                       << pongRecvUs << "," << pongSendUs << ","
                       << forwardMs << "," << publication.name.toUri());
        }
        else {
          NDN_LOG_INFO("PONG seq=" << seq
                       << " forward_ms=" << forwardMs
                       << " name=" << publication.name.toUri());
        }
        return;
      }

      if (role == "sub") {
        if (fields.size() != 3 || fields[0] != "pub") {
          return;
        }
        const int seq = std::stoi(fields[1]);
        const uint64_t publishUs = std::stoull(fields[2]);
        const double onewayMs =
          recvUs >= publishUs ? static_cast<double>(recvUs - publishUs) / 1000.0 : 0.0;
        ++received;
        if (csv) {
          NDN_LOG_INFO("SVS_ONEWAY_CSV sub,receive," << seq << "," << publishUs << ","
                       << recvUs << "," << onewayMs << "," << publication.name.toUri());
        }
        else {
          NDN_LOG_INFO("SUB_RECEIVED seq=" << seq
                       << " oneway_ms=" << onewayMs
                       << " name=" << publication.name.toUri());
        }
        if (received >= count) {
          face.getIoContext().stop();
        }
        return;
      }

      if (fields.size() != 5 || fields[0] != "echo") {
        return;
      }
      const int seq = std::stoi(fields[1]);
      const uint64_t pingSendUs = std::stoull(fields[2]);
      const uint64_t pongRecvUs = std::stoull(fields[3]);
      const uint64_t pongSendUs = std::stoull(fields[4]);
      const uint64_t pingRecvUs = recvUs;
      const double forwardMs =
        pongRecvUs >= pingSendUs ? static_cast<double>(pongRecvUs - pingSendUs) / 1000.0 : 0.0;
      const double backwardMs =
        pingRecvUs >= pongSendUs ? static_cast<double>(pingRecvUs - pongSendUs) / 1000.0 : 0.0;
      const double rttMs =
        pingRecvUs >= pingSendUs ? static_cast<double>(pingRecvUs - pingSendUs) / 1000.0 : 0.0;
      ++received;

      if (csv) {
        NDN_LOG_INFO("SVS_LATENCY_CSV ping,echo," << seq << "," << pingSendUs << ","
                     << pongRecvUs << "," << pongSendUs << "," << pingRecvUs << ","
                     << forwardMs << "," << backwardMs << "," << rttMs << ","
                     << publication.name.toUri());
      }
      else {
        NDN_LOG_INFO("PING_ECHO seq=" << seq
                     << " forward_ms=" << forwardMs
                     << " backward_ms=" << backwardMs
                     << " rtt_ms=" << rttMs
                     << " name=" << publication.name.toUri());
      }

      if (received >= count) {
        face.getIoContext().stop();
      }
    },
    true,
    false);

  std::function<void()> sendOne;
  sendOne = [&] {
    if ((role != "ping" && role != "pub") || g_stop || sent >= count) {
      return;
    }
    ++sent;
    const uint64_t sendUs = nowMicroseconds();
    sentAtUs[sent] = sendUs;
    const bool oneWay = role == "pub";
    const ndn::Name publishName =
      ndn::Name(nodePrefix).append(oneWay ? "pub" : "ping").appendNumber(sent);
    publishText(svs, publishName,
                std::string(oneWay ? "pub," : "ping,") +
                std::to_string(sent) + "," + std::to_string(sendUs));
    if (!csv) {
      NDN_LOG_INFO((oneWay ? "PUB_SENT seq=" : "PING_SENT seq=") << sent
                   << " send_us=" << sendUs
                   << " name=" << publishName.toUri());
    }
    if (sent < count) {
      scheduler.schedule(ndn::time::milliseconds(intervalMs), [sendOne] { sendOne(); });
    }
    else if (oneWay) {
      scheduler.schedule(ndn::time::milliseconds(1000), [&] {
        face.getIoContext().stop();
      });
    }
  };

  if (role == "ping" || role == "pub") {
    scheduler.schedule(ndn::time::milliseconds(startupMs), [sendOne] { sendOne(); });
  }

  scheduler.schedule(ndn::time::milliseconds(timeoutMs), [&] {
    face.getIoContext().stop();
  });

  NDN_LOG_INFO("App_SvsLatency role=" << role
               << " syncPrefix=" << syncPrefix
               << " nodePrefix=" << nodePrefix
               << " peerPrefix=" << peerPrefix
               << " count=" << count
               << " intervalMs=" << intervalMs
               << " parallelSync=" << (parallelSync ? "yes" : "no")
               << " parallelWorkers=" << parallelWorkers
               << " parallelQueue=" << parallelQueue
               << " syncBatching=" << (syncBatching ? "yes" : "no")
               << " syncBatchMs=" << syncBatchMs);

  while (!g_stop) {
    face.processEvents();
    break;
  }

  if (role == "ping") {
    auto syncStats = svs.getSVSync().getCore().getSyncProcessingStats();
    NDN_LOG_INFO("SVS_SYNC_STATS role=" << role
                 << " submitted=" << syncStats.syncJobsSubmitted
                 << " completed=" << syncStats.syncJobsCompleted
                 << " dropped=" << syncStats.syncJobsDropped
                 << " stale=" << syncStats.syncJobsStale
                 << " queueDepth=" << syncStats.syncWorkerQueueDepth
                 << " workerMs=" << syncStats.syncWorkerProcessingMs
                 << " publishMs=" << syncStats.syncMainThreadPublishMs
                 << " serialMs=" << syncStats.syncInterestSerialHandlerMs
                 << " parallelTotalMs=" << syncStats.syncInterestParallelTotalMs
                 << " mainBlockingMs=" << syncStats.syncInterestMainThreadBlockingMs);
    NDN_LOG_INFO("App_SvsLatency summary sent=" << sent
                 << " received=" << received);
    return received == count ? 0 : 1;
  }
  if (role == "pub") {
    auto syncStats = svs.getSVSync().getCore().getSyncProcessingStats();
    NDN_LOG_INFO("SVS_SYNC_STATS role=" << role
                 << " submitted=" << syncStats.syncJobsSubmitted
                 << " completed=" << syncStats.syncJobsCompleted
                 << " dropped=" << syncStats.syncJobsDropped
                 << " stale=" << syncStats.syncJobsStale
                 << " queueDepth=" << syncStats.syncWorkerQueueDepth
                 << " workerMs=" << syncStats.syncWorkerProcessingMs
                 << " publishMs=" << syncStats.syncMainThreadPublishMs
                 << " serialMs=" << syncStats.syncInterestSerialHandlerMs
                 << " parallelTotalMs=" << syncStats.syncInterestParallelTotalMs
                 << " mainBlockingMs=" << syncStats.syncInterestMainThreadBlockingMs);
    NDN_LOG_INFO("App_SvsLatency summary sent=" << sent);
    return sent == count ? 0 : 1;
  }
  if (role == "sub") {
    auto syncStats = svs.getSVSync().getCore().getSyncProcessingStats();
    NDN_LOG_INFO("SVS_SYNC_STATS role=" << role
                 << " submitted=" << syncStats.syncJobsSubmitted
                 << " completed=" << syncStats.syncJobsCompleted
                 << " dropped=" << syncStats.syncJobsDropped
                 << " stale=" << syncStats.syncJobsStale
                 << " queueDepth=" << syncStats.syncWorkerQueueDepth
                 << " workerMs=" << syncStats.syncWorkerProcessingMs
                 << " publishMs=" << syncStats.syncMainThreadPublishMs
                 << " serialMs=" << syncStats.syncInterestSerialHandlerMs
                 << " parallelTotalMs=" << syncStats.syncInterestParallelTotalMs
                 << " mainBlockingMs=" << syncStats.syncInterestMainThreadBlockingMs);
    NDN_LOG_INFO("App_SvsLatency summary received=" << received);
    return received == count ? 0 : 1;
  }

  auto syncStats = svs.getSVSync().getCore().getSyncProcessingStats();
  NDN_LOG_INFO("SVS_SYNC_STATS role=" << role
               << " submitted=" << syncStats.syncJobsSubmitted
               << " completed=" << syncStats.syncJobsCompleted
               << " dropped=" << syncStats.syncJobsDropped
               << " stale=" << syncStats.syncJobsStale
               << " queueDepth=" << syncStats.syncWorkerQueueDepth
               << " workerMs=" << syncStats.syncWorkerProcessingMs
               << " publishMs=" << syncStats.syncMainThreadPublishMs
               << " serialMs=" << syncStats.syncInterestSerialHandlerMs
               << " parallelTotalMs=" << syncStats.syncInterestParallelTotalMs
               << " mainBlockingMs=" << syncStats.syncInterestMainThreadBlockingMs);
  NDN_LOG_INFO("App_SvsLatency summary echoed=" << echoed);
  return 0;
}
