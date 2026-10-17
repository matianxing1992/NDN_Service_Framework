#include "credential-receiver.hpp"
#include <iostream>
#include <set>
#include <ndn-cxx/security/key-params.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>

using namespace credential204;
template<class F> void rejected(F&& action) {
  bool failed=false; try { action(); } catch(...) { failed=true; }
  require(failed,"negative archive input accepted");
}
size_t signedBoundaryFixtures(const std::filesystem::path& root) {
  // A unit fixture owns a fresh authority key solely to sign malformed inputs.
  // Runtime network recipients never hold an authority private key.
  ndn::security::KeyChain signer("pib-memory:boundary","tpm-memory:boundary");
  const auto authority=signer.createIdentity(ndn::Name("/system-benefits/controller"),ndn::EcKeyParams())
    .getDefaultKey().getDefaultCertificate();
  const ndn::security::Certificate own{ndn::Block(readBytes(root/"recipient-0.cert"))};
  auto literal=[](const std::string& value) {
    return ndn::name::Component(ndn::tlv::GenericNameComponent,
      {reinterpret_cast<const uint8_t*>(value.data()),value.size()});
  };
  Receiver rsa(0,"BATCHED_RSA",readBytes(root/"recipient-0.safebag"),own,authority);
  rsa.beginEpoch(1);
  auto business=[&](uint64_t size) {
    ndn::Block content(ndn::tlv::Content);
    const uint8_t dummy=0;
    content.push_back(ndn::makeBinaryBlock(602,{&dummy,1}));
    content.push_back(ndn::makeNonNegativeIntegerBlock(603,size)); content.encode();
    uint8_t digest[SHA256_DIGEST_LENGTH]; SHA256(content.data(),content.size(),digest);
    ndn::Name name(authority.getIdentity()); name.append("NDNSF").append("component-probe")
      .append(literal("name=service-0")).appendVersion(1)
      .append(ndn::name::Component(ndn::tlv::GenericNameComponent,{digest,sizeof(digest)}));
    ndn::Data data(name); data.setContent(content);
    signer.sign(data,ndn::security::signingByCertificate(authority)); return data;
  };
  const auto normal=business(64);
  require(!rsa.canAccess(normal,nullptr,0,1),"empty RSA recipient acquired permission");
  for(uint64_t size:{uint64_t(0),uint64_t(63),uint64_t(65),(uint64_t(1)<<32)+64}) {
    const auto malformed=business(size);
    rejected([&]{rsa.canAccess(malformed,nullptr,0,1);});
  }
  const auto dir=root/"AGGREGATED_ABE-p1-r1";
  const auto manifest=readJson(dir/"manifest.json");
  for(const auto& object:manifest.at("objects")) {
    if(object.at("kind")!="management" || object.at("epoch")!=1 || object.at("recipient")!=own.getIdentity().toUri()) continue;
    std::vector<ndn::Data> valid,contradictory;
    for(const auto& entry:object.at("packets")) {
      auto packet=readPacket(dir,entry);
      signer.sign(packet,ndn::security::signingByCertificate(authority)); valid.push_back(packet);
      const auto index=authority.getIdentity().size()+3;
      ndn::Name wrong(packet.getName().getPrefix(index));
      wrong.append(ndn::Name("/system-benefits/user/1"))
        .append(packet.getName().getSubName(index+own.getIdentity().size()));
      packet.setName(wrong); signer.sign(packet,ndn::security::signingByCertificate(authority));
      contradictory.push_back(packet);
    }
    Receiver abe(0,"AGGREGATED_ABE",readBytes(root/"recipient-0.safebag"),own,authority);
    abe.beginEpoch(1); rejected([&]{abe.acceptManagement(contradictory,1);});
    abe.acceptManagement(valid,1); return 5;
  }
  throw std::runtime_error("missing boundary fixture source");
}
int main(int argc,char** argv) try {
  require(argc==3,"usage: ndnsf-credential-archive-oracle archive expected-repetitions1..5");
  size_t parsed=0; const std::string argument(argv[2]); const int repetitions=std::stoi(argument,&parsed);
  require(parsed==argument.size() && repetitions>=1 && repetitions<=5,"invalid expected repetitions");
  const std::filesystem::path root(argv[1]);
  auto complete=readJson(root/"complete.json");
  require(complete.at("schema")=="ndnsf-credential-archive-v1" && complete.at("component_oracle")==true,"incomplete archive");
  const auto& names=complete.at("cases");
  require(boundedInt(complete.at("repetitions"),1,5)==repetitions,"archive repetitions differ from caller");
  require(names.is_array() && names.size()==static_cast<size_t>(9*repetitions),"incomplete archive matrix");
  std::set<std::string> expected,seen;
  for(const auto& arm:{"AGGREGATED_ABE","PERMISSION_ABE","BATCHED_RSA"})
    for(int p:{1,4,16}) for(int r=1;r<=repetitions;++r)
      expected.insert(std::string(arm)+"-p"+std::to_string(p)+"-r"+std::to_string(r));
  const ndn::security::Certificate authority{ndn::Block(readBytes(root/"authority.cert"))};
  size_t cases=0,checks=0,negative=0,wirePackets=0;
  for(const auto& caseName:complete.at("cases")) {
    const auto name=caseName.get<std::string>();
    require(expected.count(name)==1 && seen.insert(name).second,"duplicate/unexpected archive case");
    const auto dir=leaf(root,caseName.get<std::string>());
    const auto manifest=readJson(dir/"manifest.json");
    require(manifest.at("schema")=="ndnsf-credential-archive-v1" && manifest.at("component_oracle")==true,"incomplete case");
    const auto arm=manifest.at("arm").get<std::string>(); const int p=boundedInt(manifest.at("permissions"),1,16);
    const int repetition=boundedInt(manifest.at("repetition"),1,repetitions);
    require(name==arm+"-p"+std::to_string(p)+"-r"+std::to_string(repetition),"case manifest identity binding");
    require(p==1 || p==4 || p==16,"permission profile");
    std::vector<std::vector<ndn::Data>> loaded;
    for(const auto& object:manifest.at("objects")) {
      const int epoch=boundedInt(object.at("epoch"),1,3); (void)epoch;
      const auto kind=object.at("kind").get<std::string>();
      require(kind=="public-parameters" || kind=="management" || kind=="business" || kind=="content-key" ||
              kind=="probe-business" || kind=="probe-content-key","unknown archive object kind");
      const auto recipient=object.at("recipient").get<std::string>();
      bool allowedRecipient=kind!="management" && recipient=="/";
      if(kind=="management") for(int u=0;u<8;++u)
        allowedRecipient|=recipient=="/system-benefits/user/"+std::to_string(u);
      require(allowedRecipient,"unknown archive recipient");
      loaded.emplace_back();
      for(const auto& entry:object.at("packets")) {
        loaded.back().push_back(readPacket(dir,entry)); ++wirePackets;
      }
    }
    for(int u=0;u<=8;++u) {
      const auto prefix="recipient-"+std::to_string(u);
      const ndn::security::Certificate own{ndn::Block(readBytes(root/(prefix+".cert")))};
      Receiver receiver(u,arm,readBytes(root/(prefix+".safebag")),own,authority);
      for(int epoch=1;epoch<=3;++epoch) {
        receiver.beginEpoch(epoch);
        std::map<int,ndn::Data> probes,keys; size_t installed=0,params=0,business=0,wrapped=0;
        for(size_t i=0;i<loaded.size();++i) {
          const auto& object=manifest.at("objects")[i];
          if(object.at("epoch")!=epoch) continue;
          const auto kind=object.at("kind").get<std::string>();
          if(kind=="business" || kind=="content-key") {
            receiver.validate(loaded[i],epoch,kind=="business"?"component-data":"content-key",false);
            if(kind=="business") ++business; else ++wrapped;
          }
          if(kind=="public-parameters") { receiver.acceptParams(loaded[i],epoch); ++params; }
          if(kind=="management") {
            if(object.at("recipient")==own.getIdentity().toUri()) {
              receiver.acceptManagement(loaded[i],epoch); ++installed;
              if(epoch==1 && u==0) {
                auto tampered=loaded[i]; auto bytes=tampered.front().getContent();
                ndn::Buffer corrupt(bytes.value_begin(),bytes.value_end());
                require(!corrupt.empty(),"empty management cipher"); corrupt[0]^=1;
                tampered.front().setContent(corrupt);
                rejected([&]{receiver.acceptManagement(tampered,epoch);}); ++negative;
                rejected([&]{receiver.acceptManagement(loaded[i],epoch+1);}); ++negative;
              }
            } else if(epoch==1 && u==1 && object.at("recipient")=="/system-benefits/user/0") {
              rejected([&]{receiver.acceptManagement(loaded[i],epoch);}); ++negative;
            }
          }
          if(kind=="probe-business" || kind=="probe-content-key") {
            require(loaded[i].size()==1,"probe packet cardinality");
            const int permission=boundedInt(object.at("permission"),0,p);
            auto& target=kind=="probe-business"?probes:keys;
            require(target.emplace(permission,loaded[i].front()).second,"duplicate probe permission");
          }
        }
        const size_t expected=u==8?0:epoch==2?(u==0?1:0):epoch==3?(u==0?0:(arm=="PERMISSION_ABE"?p:1)):(arm=="PERMISSION_ABE"?p:1);
        require(business==1 && wrapped==(arm=="BATCHED_RSA"?0:1) &&
          params==(arm!="BATCHED_RSA" && epoch!=2?1:0),"original object cardinality");
        require(installed==expected,"recipient management object count");
        const int services=epoch==1?p:p+1;
        require(probes.size()==static_cast<size_t>(services) &&
          keys.size()==(arm=="BATCHED_RSA"?0:probes.size()),"complete probe matrix");
        for(int service=0;service<services;++service) {
          const auto key=keys.find(service);
          const bool allowed=u<8 && (epoch==3?u>0 && service<p:service<p || u==0);
          require(receiver.canAccess(probes.at(service),key==keys.end()?nullptr:&key->second,service,epoch)==allowed,"received-key permission/revocation oracle");
          ++checks;
        }
      }
    }
    ++cases;
  }
  require(seen==expected,"incomplete archive matrix");
  negative+=signedBoundaryFixtures(root);
  std::cout<<Json{{"schema","ndnsf-credential-archive-oracle-v1"},{"oracle",true},
    {"cases",cases},{"wire_packets",wirePackets},{"access_checks",checks},{"negative_checks",negative},
    {"scope","native offline packet reload and recipient crypto; not network qualification"}}.dump()<<std::endl;
  return 0;
} catch(const std::exception& e) {
  std::cerr<<"CREDENTIAL_ARCHIVE_ORACLE_FAILURE "<<e.what()<<std::endl; return 2;
} catch(...) { std::cerr<<"CREDENTIAL_ARCHIVE_ORACLE_FAILURE crypto exception"<<std::endl; return 2; }
