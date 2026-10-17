#include "credential-receiver.hpp"
#include "../targeted-invocation/common.hpp"
#include <ndn-cxx/util/random.hpp>
#include <boost/asio/signal_set.hpp>
#include <optional>
#include <iomanip>
#include <sstream>
#include <ctime>
#include <iostream>

using namespace credential204;
using sim203::Options;
using sim203::nowNs;
class Ledger {
  std::ofstream m_file;
public:
  explicit Ledger(const std::string& path):m_file(path) {
    m_file.exceptions(std::ios::failbit|std::ios::badbit);
  }
  void write(Json row) { row["schema"]="ndnsf-credential-network-v1";row["ns"]=nowNs();m_file<<row.dump()<<'\n';m_file.flush(); }
};
std::string hash(const ndn::Block& block) {
  uint8_t digest[SHA256_DIGEST_LENGTH];SHA256(block.data(),block.size(),digest);
  std::ostringstream result;result<<std::hex<<std::setfill('0');
  for(auto byte:digest) result<<std::setw(2)<<static_cast<unsigned>(byte);
  return result.str();
}
class Client {
  struct Task { size_t object,packet;ndn::Name name;size_t bytes; };
  ndn::Face& m_face;
  ndn::Scheduler m_scheduler;
  ndn::scheduler::ScopedEventId m_poll,m_deadline;
  Ledger& m_ledger;
  Json m_manifest;
  std::filesystem::path m_stage,m_probeStage;
  ndn::Buffer m_bag;
  ndn::security::Certificate m_own;
  ndn::Name m_certificateName;
  std::string m_certificateHash,m_cache,m_arm;
  std::optional<ndn::security::Certificate> m_certificate;
  std::unique_ptr<Receiver> m_receiver;
  int m_user,m_epoch=0;
  bool m_failed=false,m_stopped=false,m_phaseActive=false,m_complete=false,m_waitingProbes=false;
  size_t m_generation=0,m_next=0,m_received=0,m_inflight=0;
  uint64_t m_begin=0,m_fetchBegin=0;
  std::clock_t m_cpuBegin=0,m_phaseCpu=0;
  size_t m_interests=0,m_interestBytes=0,m_dataBytes=0;
  std::string m_fetchKind;
  std::vector<Task> m_tasks;
  std::vector<ndn::PendingInterestHandle> m_handles;
  std::vector<std::vector<std::optional<ndn::Data>>> m_packets;
  std::function<void()> m_done;
public:
  Client(ndn::Face& face,Ledger& ledger,const Options& options)
    :m_face(face),m_scheduler(face.getIoContext()),m_ledger(ledger),
     m_manifest(readJson(options.get("--manifest"))),m_stage(options.get("--stage")),m_probeStage(options.get("--probe-stage")),
     m_bag(readBytes(options.get("--bag"))),m_own(ndn::Block(readBytes(options.get("--own-cert")))),
     m_certificateName(options.get("--certificate-name")),m_certificateHash(options.get("--certificate-sha256")),
     m_cache(options.get("--cache")),m_arm(m_manifest.at("arm")),m_user(options.number("--user",9)) {
    require(m_user>=0 && m_user<8 && (m_cache=="reuse" || m_cache=="cold"),"client profile");
    require(m_manifest.at("schema")=="ndnsf-credential-archive-v1" && m_manifest.at("component_oracle")==true,"incomplete case");
    require(m_certificateHash.size()==64 && m_certificateName.size()>0,"missing certificate pin");
    m_packets.resize(m_manifest.at("objects").size());
    for(size_t i=0;i<m_packets.size();++i) m_packets[i].resize(m_manifest.at("objects")[i].at("packets").size());
  }
  ~Client() { stop(); }
  void start() { m_ledger.write({{"event","ready"},{"user",m_user},{"arm",m_arm},{"cache",m_cache}});tick(); }
  bool failed() const { return m_failed || !m_complete; }
  void stop() {
    if(m_stopped)return;
    m_stopped=true;++m_generation;m_poll.cancel();m_deadline.cancel();
    for(auto& handle:m_handles)handle.cancel();
    m_handles.clear();
    sim203::shutdownFace(m_face);
  }
private:
  void fail(const std::string& reason) {
    if(m_stopped)return;
    m_failed=true;
    m_ledger.write({{"event","failure"},{"epoch",m_epoch},{"reason",reason},{"inflight",m_inflight},{"received",m_received}});stop();
  }
  void tick() {
    if(m_stopped)return;
    try {
      std::ifstream stage(m_stage);int epoch=0;stage>>epoch;
      require(epoch<=m_epoch+1,"stage skipped an epoch");
      if(epoch==m_epoch+1 && epoch<=3) {
        require(!m_phaseActive,"stage advanced before phase completion");
        m_phaseActive=true;m_epoch=epoch;m_begin=nowNs();m_phaseCpu=std::clock();
        m_ledger.write({{"event","phase_begin"},{"epoch",epoch}});
        m_deadline=m_scheduler.schedule(ndn::time::seconds(10),[this,epoch] {
          if(!m_stopped && m_phaseActive && m_epoch==epoch)fail("phase deadline");
        });
        if(m_cache=="cold")m_certificate.reset();
        if(m_certificate) {
          m_ledger.write({{"event","certificate_cache_hit"},{"epoch",m_epoch}});prepare();
        }
        else fetchCertificate();
      }
      std::ifstream probeStage(m_probeStage);int probeEpoch=0;probeStage>>probeEpoch;
      require(probeEpoch<=m_epoch,"probe stage advanced before management stage");
      if(m_waitingProbes && probeEpoch==m_epoch) {m_waitingProbes=false;probes();}
    } catch(const std::exception& e) {fail(e.what());return;}
    if(!m_stopped)m_poll=m_scheduler.schedule(ndn::time::milliseconds(25),[this]{tick();});
  }
  void fetchCertificate() {
    fetch("certificate",{{0,0,m_certificateName,0}},[this] {
      const auto begin=nowNs();const auto cpu=std::clock();
      const auto data=*m_packets[0][0];
      require(hash(data.wireEncode())==m_certificateHash,"certificate fingerprint mismatch");
      ndn::security::Certificate certificate(data);
      require(certificate.getName()==m_certificateName && certificate.isValid() &&
        certificate.getIdentity()==ndn::Name("/system-benefits/controller"),"certificate identity/validity");
      m_certificate=std::move(certificate);m_packets[0][0].reset();
      m_ledger.write({{"event","certificate_validated"},{"epoch",m_epoch},{"elapsed_ms",(nowNs()-begin)/1e6},
        {"cpu_ms",1000.0*(std::clock()-cpu)/CLOCKS_PER_SEC}});prepare();
    });
  }
  void prepare() {
    const auto begin=nowNs();const auto cpu=std::clock();const bool bootstrap=!m_receiver;
    require(m_certificate && m_certificate->isValid(),"cached certificate invalid");
    if(!m_receiver)m_receiver=std::make_unique<Receiver>(m_user,m_arm,m_bag,m_own,*m_certificate);
    m_receiver->beginEpoch(m_epoch);
    m_ledger.write({{"event","recipient_state_prepared"},{"epoch",m_epoch},{"initial_bootstrap",bootstrap},
      {"elapsed_ms",(nowNs()-begin)/1e6},{"cpu_ms",1000.0*(std::clock()-cpu)/CLOCKS_PER_SEC}});
    fetchObjects("management",[this](const Json& object) {
      return object.at("kind")=="public-parameters" ||
        (object.at("kind")=="management" && object.at("recipient")==m_own.getIdentity().toUri());
    },[this] {
      const auto cpu=std::clock();
      for(size_t i=0;i<m_packets.size();++i) {
        const auto& object=m_manifest.at("objects")[i];if(object.at("epoch")!=m_epoch)continue;
        if(object.at("kind")=="public-parameters")m_receiver->acceptParams(objectPackets(i),m_epoch);
        if(object.at("kind")=="management" && object.at("recipient")==m_own.getIdentity().toUri())
          m_receiver->acceptManagement(objectPackets(i),m_epoch);
      }
      m_ledger.write({{"event","management_installed"},{"epoch",m_epoch},{"cpu_ms",1000.0*(std::clock()-cpu)/CLOCKS_PER_SEC},
        {"phase_elapsed_ms",(nowNs()-m_begin)/1e6},{"management_cpu_ms",1000.0*(std::clock()-m_phaseCpu)/CLOCKS_PER_SEC}});
      m_waitingProbes=true;
    });
  }
  std::vector<ndn::Data> objectPackets(size_t object) const {
    std::vector<ndn::Data> result;for(const auto& data:m_packets[object]) {require(data.has_value(),"missing fetched packet");result.push_back(*data);}return result;
  }
  void probes() {
    fetchObjects("probes",[](const Json& object) {
      return object.at("kind")=="probe-business" || object.at("kind")=="probe-content-key";
    },[this] {
      std::map<int,ndn::Data> business,keys;const auto cpu=std::clock();
      for(size_t i=0;i<m_packets.size();++i) {
        const auto& object=m_manifest.at("objects")[i];if(object.at("epoch")!=m_epoch)continue;
        if(object.at("kind")=="probe-business" || object.at("kind")=="probe-content-key") {
          auto& target=object.at("kind")=="probe-business"?business:keys;
          require(target.emplace(boundedInt(object.at("permission"),0,16),objectPackets(i).at(0)).second,"duplicate probe");
        }
      }
      const int p=boundedInt(m_manifest.at("permissions"),1,16),count=m_epoch==1?p:p+1;
      require(business.size()==static_cast<size_t>(count) && keys.size()==(m_arm=="BATCHED_RSA"?0:business.size()),"incomplete network probes");
      for(int service=0;service<count;++service) {
        const auto key=keys.find(service);
        const bool allowed=m_epoch==3?m_user>0 && service<p:service<p || m_user==0;
        const bool actual=m_receiver->canAccess(business.at(service),key==keys.end()?nullptr:&key->second,service,m_epoch);
        require(actual==allowed,"network received-key access/revocation oracle");
        m_ledger.write({{"event","access_result"},{"epoch",m_epoch},{"permission",service},{"allowed",actual}});
      }
      m_deadline.cancel();
      m_ledger.write({{"event","phase_complete"},{"epoch",m_epoch},{"oracle",true},{"access_checks",count},
        {"probe_cpu_ms",1000.0*(std::clock()-cpu)/CLOCKS_PER_SEC},{"phase_elapsed_ms",(nowNs()-m_begin)/1e6},
        {"phase_cpu_ms",1000.0*(std::clock()-m_phaseCpu)/CLOCKS_PER_SEC}});
      m_phaseActive=false;if(m_epoch==3)m_complete=true;
    });
  }
  template<class Select> void fetchObjects(const std::string& kind,Select select,std::function<void()> done) {
    std::vector<Task> tasks;
    for(size_t i=0;i<m_packets.size();++i) {
      const auto& object=m_manifest.at("objects")[i];if(object.at("epoch")!=m_epoch || !select(object))continue;
      for(size_t j=0;j<object.at("packets").size();++j) {
        const auto& entry=object.at("packets")[j];
        tasks.push_back({i,j,ndn::Name(entry.at("name").get<std::string>()),static_cast<size_t>(boundedInt(entry.at("wire_bytes"),1,1024*1024))});
      }
    }
    fetch(kind,std::move(tasks),std::move(done));
  }
  void fetch(const std::string& kind,std::vector<Task> tasks,std::function<void()> done) {
    for(auto& handle:m_handles)handle.cancel();
    m_handles.clear();
    ++m_generation;m_tasks=std::move(tasks);m_done=std::move(done);m_next=m_received=m_inflight=0;
    m_interests=m_interestBytes=m_dataBytes=0;m_fetchKind=kind;m_fetchBegin=nowNs();m_cpuBegin=std::clock();pump();
  }
  void pump() {
    if(m_stopped)return;
    while(m_next<m_tasks.size() && m_inflight<8) {
      const auto task=m_tasks[m_next++];const auto generation=m_generation;
      ndn::Interest interest(task.name);interest.setCanBePrefix(false).setMustBeFresh(false)
        .setInterestLifetime(ndn::time::milliseconds(1000)).setNonce(ndn::random::generateWord32());
      ++m_interests;m_interestBytes+=interest.wireEncode().size();++m_inflight;
      m_handles.push_back(m_face.expressInterest(interest,[this,task,generation](const auto&,const ndn::Data& data) {
        if(m_stopped || generation!=m_generation)return;
        try {
          require(data.getName()==task.name && (task.bytes==0 || data.wireEncode().size()==task.bytes),"network packet reference mismatch");
          m_packets.at(task.object).at(task.packet)=data;m_dataBytes+=data.wireEncode().size();++m_received;--m_inflight;pump();
        } catch(const std::exception& e){fail(e.what());}
      },[this,generation](const auto&,const auto&){if(!m_stopped && generation==m_generation)fail("Interest Nack");},
        [this,generation](const auto&){if(!m_stopped && generation==m_generation)fail("Interest timeout");}));
    }
    if(m_received==m_tasks.size()) {
      m_ledger.write({{"event","fetch_complete"},{"epoch",m_epoch},{"kind",m_fetchKind},{"interests",m_interests},
        {"interest_wire_bytes",m_interestBytes},{"data_wire_bytes",m_dataBytes},{"elapsed_ms",(nowNs()-m_fetchBegin)/1e6},
        {"cpu_ms",1000.0*(std::clock()-m_cpuBegin)/CLOCKS_PER_SEC}});
      auto done=std::move(m_done);try {done();}catch(const std::exception& e){fail(e.what());}
    }
  }
};
int main(int argc,char** argv) try {
  Options options(argc,argv);const auto role=options.get("--role");Ledger ledger(options.get("--records"));ndn::Face face;
  if(role=="client") {
    Client client(face,ledger,options);boost::asio::signal_set signals(face.getIoContext(),SIGINT,SIGTERM);
    signals.async_wait([&](const auto&,int){client.stop();});client.start();face.processEvents();client.stop();return client.failed()?2:0;
  }
  require(role=="server","unknown network role");
  const std::filesystem::path root(options.get("--archive"));
  const auto dir=leaf(root,options.get("--case"));const auto manifest=readJson(dir/"manifest.json");
  const ndn::security::Certificate certificate{ndn::Block(readBytes(root/"authority.cert"))};
  require(certificate.isValid(),"server certificate expired");std::map<ndn::Name,ndn::Data> objects;
  objects.emplace(certificate.getName(),certificate);
  for(const auto& object:manifest.at("objects"))for(const auto& entry:object.at("packets")) {
    const auto data=readPacket(dir,entry);
    require(certificate.getIdentity().isPrefixOf(data.getName()) &&
      data.getSignatureInfo().getKeyLocator().getName()==certificate.getName() && ndn::security::verifySignature(data,certificate),"server original packet signature/namespace");
    const auto result=objects.emplace(data.getName(),data);
    require(result.second || result.first->second.wireEncode()==data.wireEncode(),"conflicting original Data name");
  }
  const auto registration=face.setInterestFilter(certificate.getIdentity(),[&](const auto&,const ndn::Interest& interest) {
    const auto found=objects.find(interest.getName());if(found==objects.end())return;
    face.put(found->second);ledger.write({{"event","served"},{"name",interest.getName().toUri()},
      {"wire_bytes",found->second.wireEncode().size()}});
  },[&](const auto&){ledger.write({{"event","ready"},{"objects",objects.size()}});},
    [&](const auto&,const std::string& reason){throw std::runtime_error("server registration: "+reason);});
  boost::asio::signal_set signals(face.getIoContext(),SIGINT,SIGTERM);
  signals.async_wait([&](const auto&,int){sim203::shutdownFace(face);});face.processEvents();return 0;
} catch(const std::exception& e){std::cerr<<"CREDENTIAL_NETWORK_FAILURE "<<e.what()<<std::endl;return 2;}
catch(...){std::cerr<<"CREDENTIAL_NETWORK_FAILURE crypto exception"<<std::endl;return 2;}
