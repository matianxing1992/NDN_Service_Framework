#include "dynamic-common.hpp"
#include <ndn-service-framework/ServiceController.hpp>
#include <ndn-cxx/security/validator-config.hpp>
#include <filesystem>
#include <ctime>

int main(int argc,char** argv) try {
  using namespace dynamic204;
  Options options(argc,argv);Records records(options.get("--records"));
  ndn::Face face;ndn::KeyChain keys;ndn::ValidatorConfig validator(face);
  ndn::Scheduler scheduler(face.getIoContext());
  const ndn::Name identity("/example/hello/controller"),target("/example/hello/user/u0");
  const ndn::Name hello("/HELLO"),extra("/EXTRA");
  const auto cert=certificate(keys,identity);validator.load(options.get("--trust"));
  size_t publicIdentities=0,privateKeys=0;
  for(const auto& peer:keys.getPib().getIdentities()) {
    ++publicIdentities;
    for(const auto& key:peer.getKeys())if(keys.getTpm().hasKey(key.getName())) {
      if(peer.getName()!=identity)throw std::runtime_error("Controller contains foreign private key");
      ++privateKeys;
    }
  }
  if(privateKeys!=1)throw std::runtime_error("Controller own private key inventory");
  records.write({{"event","bootstrap_key_inventory"},{"public_identities",publicIdentities},
    {"private_keys",privateKeys},{"oracle",true},{"mono_ns",nowNs()}});
  keys.setDefaultIdentity(keys.getPib().getIdentity(identity));
  nsf::CertificatePublisher publisher(face,keys,identity);
  auto version=[](const nsf::ControllerVersion& v) {
    return Json{{"generation",v.controllerGenerationTimestamp},{"epoch",v.controllerEpoch}};
  };
  const auto stagePath=options.get("--stage"),triggerPath=options.get("--trigger");
  std::string phase="initial",lastCommand;
  bool granted=false,revoked=false;
  uint64_t phaseBegin=0;double phaseCpu=0;
  auto cpu=[] {return 1000.0*std::clock()/CLOCKS_PER_SEC;};
  {
    nsf::ServiceController controller(face,cert,validator,options.get("--policy"));
    controller.setControllerPrefix(identity);controller.start();
    const auto initialDigest=controller.getPolicyStatus(hello).getAbePublicParametersDigest();
    auto writeStage=[&] {
      Json row{{"phase",phase},{"mono_ns",nowNs()},{"global",version(controller.getControllerVersion())},
        {"hello",version(controller.getPolicyStatus(hello).getControllerVersion())},
        {"extra",version(controller.getPolicyStatus(extra).getControllerVersion())}};
      const auto temporary=stagePath+".pending";
      {std::ofstream file(temporary);file<<row.dump()<<'\n';if(!file)throw std::runtime_error("stage write");}
      std::filesystem::rename(temporary,stagePath);return row;
    };
    auto initial=writeStage();initial["event"]="ready";records.write(initial);
    std::cout<<"SPEC204_PERMISSION_CONTROLLER_READY"<<std::endl;
    std::function<void()> poll;
    poll=[&] {
      std::ifstream file(triggerPath);std::string command;file>>command;
      if(!command.empty() && command!=lastCommand) {
        lastCommand=command;
        if(command=="grant" || command=="revoke") {
          const auto begin=nowNs();const auto startCpu=cpu();bool ok=false;
          if(command=="grant") {
            if(granted || revoked)throw std::runtime_error("invalid grant stage");
            ok=controller.grant(target,extra,ndn::Name("/PERMISSION").append(extra));granted=ok;
            if(controller.getPolicyStatus(hello).getAbePublicParametersDigest()!=initialDigest)
              throw std::runtime_error("grant rotated ABE generation");
          }else {
            if(!granted || revoked)throw std::runtime_error("invalid revoke stage");
            nsf::RevocationTarget revoke;revoke.kind=nsf::RevocationKind::IDENTITY;revoke.targetIdentity=target;
            ok=controller.revoke(revoke);revoked=ok;
            if(controller.getPolicyStatus(hello).getAbePublicParametersDigest()==initialDigest)
              throw std::runtime_error("revoke did not rotate ABE generation");
          }
          if(!ok)throw std::runtime_error("production mutation failed");
          phase=command;phaseBegin=begin;phaseCpu=startCpu;
          auto row=writeStage();row["event"]="commit";row["begin_ns"]=begin;
          row["commit_cpu_ms"]=cpu()-startCpu;row["oracle"]=true;records.write(row);
        }else if(command=="checkpoint-grant" || command=="checkpoint-revoke") {
          if(command!="checkpoint-"+phase)throw std::runtime_error("checkpoint phase mismatch");
          records.write({{"event","recovery_checkpoint"},{"phase",phase},{"mono_ns",nowNs()},
            {"begin_ns",phaseBegin},{"process_cpu_since_mutation_ms",cpu()-phaseCpu}});
        }else if(command=="stop")shutdownFace(face);
        else throw std::runtime_error("unknown admin command");
      }
      if(!face.getIoContext().stopped())scheduler.schedule(ndn::time::milliseconds(20),poll);
    };
    scheduler.schedule(ndn::time::milliseconds(20),poll);
    boost::asio::signal_set signals(face.getIoContext(),SIGINT,SIGTERM);
    signals.async_wait([&](const auto&,int){shutdownFace(face);});face.processEvents();
  }
  records.write({{"event","shutdown"},{"granted",granted},{"revoked",revoked},{"mono_ns",nowNs()}});
  return 0;
}catch(const std::exception& e){std::cerr<<"PERMISSION_CONTROLLER_FAILURE "<<e.what()<<std::endl;return 2;}
