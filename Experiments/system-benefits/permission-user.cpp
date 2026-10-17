#include "dynamic-common.hpp"
#include <set>
#include <ndn-cxx/util/io.hpp>

int main(int argc,char** argv) try {
  using namespace dynamic204;
  Options options(argc,argv);Records records(options.get("--records"));
  const ndn::Name identity(options.get("--identity")),provider("/example/hello/provider/A"),controller("/example/hello/controller");
  const ndn::Name hello("/HELLO"),extra("/EXTRA");
  const bool target=identity==ndn::Name("/example/hello/user/u0");
  const int permissionCount=options.number("--permissions",1);
  if(permissionCount!=1 && permissionCount!=16)throw std::invalid_argument("permission profile");
  std::set<std::string> initialServices{"/HELLO"};
  for(int p=1;p<permissionCount;++p)initialServices.insert("/PERM/"+std::to_string(p));
  ndn::Face face;ndn::KeyChain keys;ndn::Scheduler scheduler(face.getIoContext());
  const auto cert=certificate(keys,identity);keys.setDefaultIdentity(keys.getPib().getIdentity(identity));
  const auto controllerCert=ndn::io::load<ndn::security::Certificate>(options.get("--controller-cert"));
  if(!controllerCert || controllerCert->getIdentity()!=controller)throw std::runtime_error("controller certificate binding");
  nsf::CertificatePublisher publisher(face,keys,identity);
  std::string phase="initial";bool pending=false,ready=false,extraVerified=false,denialVerified=false,revokeRenewalRequested=false;
  uint64_t sequence=0,invalid=0,phaseEpoch=0,phaseGeneration=0;
  auto version=[](const std::optional<nsf::ControllerVersion>& v) {
    return v?Json{{"generation",v->controllerGenerationTimestamp},{"epoch",v->controllerEpoch}}:Json();
  };
  {
    nsf::ServiceUser user(face,ndn::Name(options.get("--group")),cert,*controllerCert,options.get("--trust"));
    user.init();user.setUseTokens(true);user.setTimelineTrace(true);user.fetchPermissionsFromController(controller);
    const auto started=Clock::now();std::function<void()> tick;
    tick=[&] {
      try {
        std::ifstream file(options.get("--stage"));Json stage;if(file)file>>stage;
        if(!stage.is_null() && stage.at("phase").get<std::string>()!=phase) {
          phase=stage.at("phase").get<std::string>();
          phaseEpoch=stage.at("global").at("epoch").get<uint64_t>();
          phaseGeneration=stage.at("global").at("generation").get<uint64_t>();
          records.write({{"event","phase_observed"},{"phase",phase},{"mono_ns",nowNs()}});
          if(target && phase=="grant")user.fetchPermissionsFromController(controller);
        }
      }catch(const Json::exception&){} // Atomic stage replacement; retry only metadata read.
      if(pending){scheduler.schedule(ndn::time::milliseconds(20),tick);return;}
      if(!ready && Clock::now()-started>std::chrono::seconds(45)) {++invalid;shutdownFace(face);return;}
      const auto service=target && phase=="grant" && !extraVerified?extra:hello;
      const auto global=user.getControllerVersion();
      const auto permissions=user.getAllowedServices();
      const bool newGlobal=global && global->controllerEpoch==phaseEpoch
        && global->controllerGenerationTimestamp==phaseGeneration;
      if(target && phase=="revoke" && newGlobal && !revokeRenewalRequested) {
        // Status is authoritative; the diagnostic permission inventory is
        // refreshed separately through the ordinary protected Controller RPC.
        revokeRenewalRequested=true;user.fetchPermissionsFromController(controller);
        records.write({{"event","revoked_permission_refresh"},{"global",version(global)},
          {"mono_ns",nowNs()}});
      }
      // Status and PermissionResponse may arrive in either order. Permit
      // removal during refresh; certify the full matrix only at real barriers.
      std::set<std::string> actualServices;
      for(const auto& entry:permissions)actualServices.insert(ndn::Name(std::get<1>(entry)).toUri());
      auto permitted=initialServices;if(target)permitted.insert(extra.toUri());
      for(const auto& s:actualServices)if(!permitted.count(s))
        throw std::runtime_error("production permission leaked to unrelated identity/service");
      if(target && phase=="revoke" && newGlobal && permissions.empty()) {
        for(const auto& revokedService:std::vector<ndn::Name>{hello,extra}) {
        nsf::RequestMessage request;const auto command=identity.toUri()+":deny:"+std::to_string(++sequence);
        std::string padded=command;padded.resize(64,'_');
        ndn::Buffer payload(reinterpret_cast<const uint8_t*>(padded.data()),padded.size());request.setPayload(payload,payload.size());
        const auto id=user.RequestServiceTargeted(provider,revokedService,request,2000,
          [&](const ndn::Name&){records.write({{"event","unexpected_revoked_timeout"}});},
          [&](const nsf::ResponseMessage&){records.write({{"event","unexpected_revoked_response"}});});
        if(!id.empty()){++invalid;throw std::runtime_error("new-version revoked identity admitted native request");}
        if(!denialVerified) {
          records.write({{"event","revoked_gate_denied"},{"identity",identity.toUri()},{"service",revokedService.toUri()},
            {"phase",phase},{"global",version(global)},{"command",padded},{"request_id",id.toUri()},
            {"empty_authorization",true},{"oracle",true},{"mono_ns",nowNs()}});
        }
        }
        denialVerified=true;
        scheduler.schedule(ndn::time::milliseconds(200),tick);return;
      }
      if(!user.isRequestBootstrapReady(service,{provider})) {
        scheduler.schedule(ndn::time::milliseconds(100),tick);return;
      }
      struct Call {uint64_t begin,sequence;std::string command,phase;ndn::Name service,id;bool terminal=false;};
      auto call=std::make_shared<Call>();call->begin=nowNs();call->sequence=++sequence;call->phase=phase;call->service=service;
      call->command=identity.toUri()+":"+std::to_string(sequence);call->command.resize(64,'_');
      pending=true;nsf::RequestMessage request;
      ndn::Buffer payload(reinterpret_cast<const uint8_t*>(call->command.data()),call->command.size());request.setPayload(payload,payload.size());
      auto finish=[&,call](const nsf::ResponseMessage* response) {
        if(call->terminal)return;call->terminal=true;pending=false;
        bool valid=false;
        Json row{{"event","terminal"},{"identity",identity.toUri()},{"phase",call->phase},
          {"service",call->service.toUri()},{"command",call->command},{"request_id",call->id.toUri()},
          {"begin_ns",call->begin},{"terminal_ns",nowNs()},{"global",version(user.getControllerVersion())}};
        if(response) {
          valid=response->getStatus() && response->getProviderIdentity()==provider.toUri()
            && !response->getDataName().empty() && provider.isPrefixOf(ndn::Name(response->getDataName()))
            && !response->getWireDigest().empty() && stringPayload(response->getPayload())==call->command
            && response->hasControllerVersion();
          const bool wrongIdentity=(!response->getProviderIdentity().empty() && response->getProviderIdentity()!=provider.toUri())
            || (!response->getDataName().empty() && !provider.isPrefixOf(ndn::Name(response->getDataName())));
          if(wrongIdentity || (response->getStatus() && !valid))++invalid;
          row["error"]=response->getErrorInfo();row["provider"]=response->getProviderIdentity();
          if(response->hasControllerVersion())row["response_version"]=version(response->getControllerVersion());
        }
        row["verified_success"]=valid;records.write(row);
        std::set<std::string> currentServices;
        for(const auto& entry:user.getAllowedServices())currentServices.insert(ndn::Name(std::get<1>(entry)).toUri());
        if(valid && !ready) {
          if(currentServices!=initialServices)throw std::runtime_error("initial production permission matrix mismatch");
          ready=true;std::cout<<"SPEC204_PERMISSION_USER_READY "<<identity.toUri()<<std::endl;
        }
        if(valid && call->service==extra && call->phase=="grant") {
          const auto v=response->getControllerVersion();
          if(v.controllerEpoch!=phaseEpoch || v.controllerGenerationTimestamp!=phaseGeneration)
            throw std::runtime_error("EXTRA access did not use committed grant version");
          auto expected=initialServices;expected.insert(extra.toUri());
          if(currentServices!=expected)throw std::runtime_error("grant production permission matrix mismatch");
          extraVerified=true;records.write({{"event","grant_extra_verified"},{"identity",identity.toUri()},
            {"response_version",version(v)},{"oracle",true},{"mono_ns",nowNs()}});
        }
        if(valid && !target && phase=="revoke" && response->getControllerVersion().controllerEpoch==phaseEpoch
           && response->getControllerVersion().controllerGenerationTimestamp==phaseGeneration
           && currentServices==initialServices) {
          records.write({{"event","survivor_recovery_verified"},{"identity",identity.toUri()},
            {"response_version",version(response->getControllerVersion())},{"request_id",call->id.toUri()},
            {"command",call->command},{"begin_ns",call->begin},{"mono_ns",nowNs()},
            {"permission_matrix",true},{"oracle",true}});
        }
      };
      call->id=user.RequestServiceTargeted(provider,service,request,2000,
        [&,call,finish](const ndn::Name&){boost::asio::post(face.getIoContext(),[finish]{finish(nullptr);});},
        [&,call,finish](const nsf::ResponseMessage& response){boost::asio::post(face.getIoContext(),[finish,response]{finish(&response);});});
      records.write({{"event","submitted"},{"identity",identity.toUri()},{"phase",phase},
        {"service",service.toUri()},{"command",call->command},{"request_id",call->id.toUri()},
        {"begin_ns",call->begin},{"installed_global",version(global)}});
      if(call->id.empty())finish(nullptr);
      scheduler.schedule(ndn::time::milliseconds(200),tick);
    };
    scheduler.schedule(ndn::time::milliseconds(100),tick);
    boost::asio::signal_set signals(face.getIoContext(),SIGINT,SIGTERM);
    signals.async_wait([&](const auto&,int){shutdownFace(face);});face.processEvents();
  }
  records.write({{"event","shutdown"},{"identity",identity.toUri()},{"ready",ready},{"invalid",invalid},
    {"extra_verified",extraVerified},{"denial_verified",denialVerified},{"mono_ns",nowNs()}});
  return ready && !invalid?0:2;
}catch(const std::exception& e){std::cerr<<"PERMISSION_USER_FAILURE "<<e.what()<<std::endl;return 2;}
