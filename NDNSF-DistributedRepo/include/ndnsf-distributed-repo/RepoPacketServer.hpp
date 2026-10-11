#ifndef NDNSF_DISTRIBUTED_REPO_PACKET_SERVER_HPP
#define NDNSF_DISTRIBUTED_REPO_PACKET_SERVER_HPP

#include <ndn-cxx/face.hpp>
#include <functional>
#include <memory>
#include <string>

namespace ndnsf_distributed_repo {
class RepoCore;

// Call on the Face's event thread; the Face must outlive close/drain.
class RepoPacketServer
{
public:
  RepoPacketServer(ndn::Face& face, std::shared_ptr<RepoCore> store);
  ~RepoPacketServer();
  RepoPacketServer(const RepoPacketServer&) = delete;
  RepoPacketServer& operator=(const RepoPacketServer&) = delete;
  void servePrefix(const ndn::Name& originalDataPrefix,
                   std::function<void()> onReady,
                   std::function<void(const std::string&)> onFailure);
  void close() noexcept;
private:
  struct State;
  std::shared_ptr<State> m_state;
};
} // namespace ndnsf_distributed_repo
#endif
