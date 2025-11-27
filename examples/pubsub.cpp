/* -*- Mode: C++; c-file-style: "gnu"; indent-tabs-mode:nil -*- */
/*
 * Modified version of ndn-svs pubsub chat example
 * Automatically publishes messages every 6 ms and measures delay
 */

#include <ctime>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <chrono>

#include <ndn-svs/svspubsub.hpp>
#include <ndn-cxx/util/time.hpp>
#include <ndn-cxx/util/scheduler.hpp>

using namespace ndn::svs;
using namespace std::chrono;

struct Options
{
  std::string prefix;
  std::string m_id;
};

class Program
{
public:
  Program(const Options& options)
    : m_options(options)
  {
    // === 安全设置 ===
    SecurityOptions secOpts(m_keyChain);
    secOpts.interestSigner->signingInfo.setSigningHmacKey("dGhpcyBpcyBhIHNlY3JldCBtZXNzYWdl");
    secOpts.dataSigner->signingInfo.setSha256Signing();

    // === SVS Pub/Sub 选项 ===
    SVSPubSubOptions opts;
    opts.useTimestamp = true;
    opts.maxPubAge = ndn::time::seconds(10);

    // === 创建 Pub/Sub 实例 ===
    m_svsps = std::make_shared<SVSPubSub>(
      ndn::Name(m_options.prefix),      // group 改为 /muas
      ndn::Name(m_options.m_id),
      face,
      std::bind(&Program::onMissingData, this, _1),
      opts,
      secOpts);

    std::cout << "SVS client starting in group: " << m_options.prefix
              << " node: " << m_options.m_id << std::endl;

    // === 订阅 /muas/chat 话题 ===
    m_svsps->subscribe(ndn::Name("/muas/chat"), [this] (const auto& subData)
    {
      auto now = steady_clock::now();
      double recvTime = duration_cast<microseconds>(now.time_since_epoch()).count() / 1000.0;

      std::string content(reinterpret_cast<const char*>(subData.data.data()), subData.data.size());
      double sendTime = 0.0;
      if (content.size() >= sizeof(double))
        memcpy(&sendTime, content.data(), sizeof(double));

      double latency = recvTime - sendTime;
      std::cout << "[RECV] " << subData.producerPrefix << " [" << subData.seqNo << "] "
                << " at " << recvTime << " ms"
                << " latency=" << latency << " ms" << std::endl;
    });
  }

  void run()
  {
    std::thread svsThread([this] { face.processEvents(); });

    std::cout << "Start periodic publishing every 2 ms" << std::endl;

    int i = 0;
    while (i < 10000) {
      publishMsg();
      i++;
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    svsThread.join();
  }

protected:
  void onMissingData(const std::vector<MissingDataInfo>&)
  {
    // Ignored for now
  }

  void publishMsg()
  {
    auto now = steady_clock::now();
    double nowMs = duration_cast<microseconds>(now.time_since_epoch()).count() / 1000.0;

    std::string payload(sizeof(double), '\0');
    memcpy(payload.data(), &nowMs, sizeof(double));

    // === 发布到 /muas/chat 话题 ===
    ndn::Name name("/muas/chat");
    name.append(m_options.m_id);
    name.appendTimestamp();

    m_svsps->publish(name,
                     ndn::make_span(reinterpret_cast<const uint8_t*>(payload.data()), payload.size()));

    std::cout << "[SEND] " << name.toUri()
              << " at " << nowMs << " ms" << std::endl;
  }

private:
  const Options m_options;
  ndn::Face face;
  std::shared_ptr<SVSPubSub> m_svsps;
  ndn::KeyChain m_keyChain;
};

int main(int argc, char** argv)
{
  if (argc != 2) {
    std::cerr << "Usage: " << argv[0] << " <node-id>" << std::endl;
    return 1;
  }

  Options opt;
  opt.prefix = "/muas";   // group 改为 /muas
  opt.m_id = argv[1];

  Program program(opt);
  program.run();

  return 0;
}
