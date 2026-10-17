#include "dynamic-common.hpp"
#include <thread>
#include <ndn-cxx/util/io.hpp>

int main(int argc,char** argv) try {
  using namespace dynamic204;
  Options options(argc,argv);Records records(options.get("--records"));
  ndn::Face face;ndn::KeyChain keys;ndn::Scheduler scheduler(face.getIoContext());
  const ndn::Name identity("/example/hello/provider/A"),controller("/example/hello/controller");
  const auto cert=certificate(keys,identity);keys.setDefaultIdentity(keys.getPib().getIdentity(identity));
  const auto controllerCert=ndn::io::load<ndn::security::Certificate>(options.get("--controller-cert"));
  if(!controllerCert || controllerCert->getIdentity()!=controller)throw std::runtime_error("controller certificate binding");
  nsf::CertificatePublisher publisher(face,keys,identity);bool ready=false;
  {
    nsf::ServiceProvider provider(face,ndn::Name(options.get("--group")),cert,*controllerCert,options.get("--trust"));
    provider.setUseTokens(true);provider.setTimelineTrace(true);
    nsf::ServiceProvider::AckStrategyHandler ack=[](const nsf::RequestMessage&) {
      nsf::ServiceProvider::AckDecision decision;decision.status=true;return decision;
    };
    nsf::ServiceProvider::RequestHandler handler=[&](const ndn::Name& user,const ndn::Name&,
        const ndn::Name& service,const ndn::Name& requestId,const nsf::RequestMessage& request) {
      const auto begin=nowNs();auto payload=request.getPayload();
      if(payload.size()!=64)throw std::runtime_error("permission provider malformed business payload");
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      records.write({{"event","execution"},{"user",user.toUri()},{"service",service.toUri()},
        {"request_id",requestId.toUri()},{"command",stringPayload(payload)},
        {"begin_ns",begin},{"end_ns",nowNs()},{"request_epoch",request.getPolicyEpoch()}});
      nsf::ResponseMessage response;response.setStatus(true);response.setPayload(payload,payload.size());return response;
    };
    auto hello=provider.addScopedService(ndn::Name("/HELLO"),ack,handler,nsf::ServiceProvider::ServiceInvocationMode::NormalAndTargeted);
    auto extra=provider.addScopedService(ndn::Name("/EXTRA"),ack,handler,nsf::ServiceProvider::ServiceInvocationMode::NormalAndTargeted);
    provider.init();provider.fetchPermissionsFromController(controller);
    const auto start=Clock::now();std::function<void()> check;
    check=[&] {
      if(provider.hasProviderPermissionForService(ndn::Name("/HELLO"))
         && provider.hasProviderPermissionForService(ndn::Name("/EXTRA")) && provider.isRequestDecryptionReady()) {
        ready=true;records.write({{"event","ready"},{"mono_ns",nowNs()}});
        std::cout<<"SPEC204_PERMISSION_PROVIDER_READY"<<std::endl;
      }else if(Clock::now()-start>std::chrono::seconds(45))shutdownFace(face);
      else scheduler.schedule(ndn::time::milliseconds(100),check);
    };
    scheduler.schedule(ndn::time::milliseconds(100),check);
    boost::asio::signal_set signals(face.getIoContext(),SIGINT,SIGTERM);
    signals.async_wait([&](const auto&,int){shutdownFace(face);});face.processEvents();
  }
  records.write({{"event","shutdown"},{"ready",ready},{"mono_ns",nowNs()}});return ready?0:2;
}catch(const std::exception& e){std::cerr<<"PERMISSION_PROVIDER_FAILURE "<<e.what()<<std::endl;return 2;}
