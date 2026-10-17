#include <nac-abe/algo/abe-support.hpp>
#include <nac-abe/ndn-crypto/aes.hpp>
#include <nac-abe/ndn-crypto/data-enc-dec.hpp>
#include <ndn-cxx/encoding/block-helpers.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/security/verification-helpers.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <iostream>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <openssl/sha.h>
#include <stdexcept>
#include <vector>
#include "credential-archive.hpp"

// This is a component experiment, not the production AA network protocol.
namespace {
using Clock = std::chrono::steady_clock;
using Json = nlohmann::json;
namespace na = ndn::nacabe;
namespace alg = ndn::nacabe::algo;
constexpr int USERS = 8;
std::unique_ptr<CredentialArchive> archive;
constexpr uint32_t Envelope = 128, Epoch = 129, Material = 130, Entry = 131, Key = 132;
ndn::name::Component literal(const std::string& value) {
  // '=' is part of this generic value, not a URI component-type separator.
  return ndn::name::Component(ndn::tlv::GenericNameComponent,
    {reinterpret_cast<const uint8_t*>(value.data()),value.size()});
}
double since(Clock::time_point t) {
  return std::chrono::duration<double, std::milli>(Clock::now()-t).count();
}
void check(bool value, const char* reason) {
  if (!value) throw std::runtime_error(reason);
}
double cpuSince(std::clock_t t) { return 1000.0*(std::clock()-t)/CLOCKS_PER_SEC; }
struct Principal {
  ndn::Name name;
  ndn::security::KeyChain keys;
  ndn::security::Certificate cert;
  explicit Principal(int id)
    : name("/system-benefits/user/"+std::to_string(id)),
      keys("pib-memory:principal"+std::to_string(id), "tpm-memory:principal"+std::to_string(id)),
      cert(keys.createIdentity(name, ndn::RsaKeyParams(2048)).getDefaultKey().getDefaultCertificate()) {}
};
struct Cost {
  double setupMs = 0, keygenMs = 0, transportMs = 0;
  double setupCpuMs = 0, managementMs = 0, managementCpuMs = 0;
  double negativeMs = 0, negativeCpuMs = 0;
  size_t publicParamsBytes = 0, publicParamsPackets = 0;
  size_t objects = 0, packets = 0, wireBytes = 0, rawBytes = 0;
};
std::optional<ndn::Buffer> acceptEnvelope(const ndn::Buffer& input,
                                       const ndn::Name& recipient,int epoch) {
  try {
    ndn::Block b(input); b.parse();
    if(b.type()!=Envelope || b.elements().size()!=3 ||
       ndn::Name(b.get(ndn::tlv::Name))!=recipient ||
       ndn::readNonNegativeInteger(b.get(Epoch))!=static_cast<uint64_t>(epoch)) return std::nullopt;
    const auto& material=b.get(Material);
    return ndn::Buffer(material.value_begin(),material.value_end());
  } catch(...) { return std::nullopt; }
}
ndn::Name service(int p) { return ndn::Name("/service/"+std::to_string(p)); }
std::string attr(int p) { return "PERMISSION"+std::to_string(p); }
std::string policy(int count) {
  std::string result;
  for (int p=0; p<count; ++p) result += (p ? " or " : "")+attr(p);
  return result;
}
ndn::Buffer encodeKeys(const std::map<int, ndn::Buffer>& keys) {
  ndn::Block block(Material);
  for (const auto& item : keys) {
    ndn::Block entry(Entry);
    entry.push_back(service(item.first).wireEncode());
    entry.push_back(ndn::makeBinaryBlock(Key, item.second));
    entry.encode(); block.push_back(entry);
  }
  block.encode(); return ndn::Buffer(block.data(), block.data()+block.size());
}
std::map<int, ndn::Buffer> decodeKeys(const ndn::Buffer& input) {
  ndn::Block block(input); block.parse();
  check(block.type()==Material, "wrong key-bundle type");
  std::map<int, ndn::Buffer> result;
  for (auto entry : block.elements()) {
    check(entry.type()==Entry, "wrong bundle entry"); entry.parse();
    check(entry.elements().size()==2, "bundle cardinality");
    ndn::Name n(entry.get(ndn::tlv::Name));
    const auto p=std::stoi(n.get(-1).toUri());
    const auto& value=entry.get(Key);
    check(n==service(p) && value.value_size()==32, "wrong service/key");
    check(result.emplace(p, ndn::Buffer(value.value_begin(),value.value_end())).second,
          "duplicate bundle key");
  }
  return result;
}

// Pin outer signatures before decoding/decrypting; bind recipient and epoch.
ndn::Buffer distribute(const ndn::Buffer& material, Principal& recipient,
                      int epoch, const std::string& arm, Cost& cost,
                      ndn::security::KeyChain& authority,
                      const ndn::security::Certificate& signer,
                      Principal& wrongRecipient, bool negative) {
  const auto started=Clock::now();
  ndn::Block envelope(Envelope);
  envelope.push_back(recipient.name.wireEncode());
  envelope.push_back(ndn::makeNonNegativeIntegerBlock(Epoch,epoch));
  envelope.push_back(ndn::makeBinaryBlock(Material,material));
  envelope.encode();
  const auto encrypted=na::encryptDataContentWithCK(
    {envelope.data(),envelope.size()},recipient.cert.getPublicKey());
  uint8_t digest[SHA256_DIGEST_LENGTH];
  SHA256(encrypted.data(),encrypted.size(),digest);
  ndn::Name base(signer.getIdentity());
  base.append("NDNSF").append("credentials").append(literal("name="+arm))
      .append(recipient.name.toUri()).appendVersion(epoch)
      .append(ndn::name::Component(ndn::tlv::GenericNameComponent,{digest,sizeof(digest)}));
  ndn::Buffer assembled;
  std::vector<ndn::Data> signedPackets;
  size_t seq=0;
  for (size_t offset=0;offset<encrypted.size();offset+=1500,++seq) {
    const auto size=std::min<size_t>(1500,encrypted.size()-offset);
    ndn::Data data(ndn::Name(base).appendSegment(seq));
    data.setContent({encrypted.data()+offset,size});
    authority.sign(data,ndn::security::signingByCertificate(signer));
    check(ndn::security::verifySignature(data,signer),"management signature");
    check(data.getName()==ndn::Name(base).appendSegment(seq),"management name");
    cost.wireBytes+=data.wireEncode().size(); ++cost.packets;
    signedPackets.push_back(data);
    assembled.insert(assembled.end(),data.getContent().value_begin(),data.getContent().value_end());
  }
  ndn::Block cipher(assembled);
  const auto decoded=na::decryptDataContent(cipher,recipient.keys.getTpm(),recipient.cert.getName());
  const auto result=acceptEnvelope(decoded,recipient.name,epoch);
  check(result.has_value() && *result==material,"recipient/epoch/material binding");
  ++cost.objects; cost.rawBytes+=material.size(); cost.transportMs+=since(started);
  // Negative oracles run outside the normal transport elapsed interval.
  if (negative) {
    const auto negativeStart=Clock::now(); const auto negativeCpu=std::clock();
    auto bad=signedPackets.front();
    ndn::Buffer bytes(bad.getContent().value_begin(),bad.getContent().value_end());
    check(!bytes.empty(),"empty negative content"); bytes[0]^=1; bad.setContent(bytes);
    check(!ndn::security::verifySignature(bad,signer),"tampered signature accepted");
    bool denied=false;
    try { (void)na::decryptDataContent(cipher,wrongRecipient.keys.getTpm(),wrongRecipient.cert.getName()); }
    catch (...) { denied=true; }
    check(denied,"wrong recipient decrypted key envelope");
    // Replay the actual retained signed packets at a later expected epoch.
    ndn::Buffer replay;
    for(const auto& packet:signedPackets) {
      check(ndn::security::verifySignature(packet,signer),"replay signature changed");
      replay.insert(replay.end(),packet.getContent().value_begin(),packet.getContent().value_end());
    }
    const auto old=na::decryptDataContent(ndn::Block(replay),recipient.keys.getTpm(),recipient.cert.getName());
    check(!acceptEnvelope(old,recipient.name,epoch+1),
          "old signed envelope accepted at future epoch");
    check(!acceptEnvelope(old,wrongRecipient.name,epoch),"envelope recipient check bypassed");
    cost.negativeMs+=since(negativeStart); cost.negativeCpuMs+=cpuSince(negativeCpu);
  }
  if (archive) archive->packets("management",epoch,recipient.name,signedPackets);
  return *result;
}
struct Cipher {
  alg::CipherText abe;
};
struct State {
  std::string arm;
  int permissions;
  alg::PublicParams params;
  alg::MasterKey master;
  std::vector<std::vector<alg::PrivateKey>> abeKeys{USERS};
  std::vector<std::map<int,ndn::Buffer>> rsaKeys{USERS};
  std::map<int,ndn::Buffer> serviceKeys;
};
Cipher encrypt(State& state,int p,const ndn::Buffer& plain) {
  Cipher c;
  if (state.arm=="BATCHED_RSA") {
    const auto& bytes=state.serviceKeys.at(p);
    auto key=std::make_shared<alg::ContentKey>(
      std::string(reinterpret_cast<const char*>(bytes.data()),bytes.size()),ndn::Buffer());
    c.abe=alg::ABESupport::getInstance().encrypt(std::move(key),plain);
  } else c.abe=alg::ABESupport::getInstance().kpEncrypt(state.params,{attr(p)},plain);
  return c;
}
bool access(State& state,int u,int p,const Cipher& c,const ndn::Buffer& plain) {
  if (state.arm=="BATCHED_RSA") {
    const auto it=state.rsaKeys[u].find(p);
    if (it==state.rsaKeys[u].end()) return false;
    try {
      const auto& bytes=it->second;
      oabe::OpenABESymKeyEnc aes(std::string(reinterpret_cast<const char*>(bytes.data()),bytes.size()));
      check(!c.abe.m_content.empty() && c.abe.m_content.back()==0,"symmetric codec terminator");
      const auto decoded=aes.decrypt(std::string(reinterpret_cast<const char*>(c.abe.m_content.data()),
                                                c.abe.m_content.size()-1));
      return ndn::Buffer(decoded.begin(),decoded.end())==plain;
    }
    catch (...) { return false; }
  }
  for (const auto& key:state.abeKeys[u]) {
    try { if (alg::ABESupport::getInstance().kpDecrypt(state.params,key,c.abe)==plain) return true; }
    catch (...) {}
  }
  return false;
}
void exportProbe(const State& state,const Cipher& c,int p,int epoch,
                 ndn::security::KeyChain& authority,const ndn::security::Certificate& cert) {
  if (!archive) return;
  auto publish=[&](const char* category,const char* kind,ndn::Block content) {
    content.encode(); uint8_t digest[SHA256_DIGEST_LENGTH];
    SHA256(content.data(),content.size(),digest);
    ndn::Name name(cert.getIdentity());
    name.append("NDNSF").append(category).append(literal("name=service-"+std::to_string(p)))
        .appendVersion(epoch).append(ndn::name::Component(ndn::tlv::GenericNameComponent,{digest,sizeof(digest)}));
    ndn::Data data(name); data.setContent(content); authority.sign(data,ndn::security::signingByCertificate(cert));
    check(ndn::security::verifySignature(data,cert),"probe signature");
    archive->packets(kind,epoch,ndn::Name(),{data},p);
  };
  publish("component-probe","probe-business",c.abe.makeDataContent());
  if(state.arm!="BATCHED_RSA")
    publish("component-probe-key","probe-content-key",c.abe.m_contentKey->makeCKContent());
}
void issue(State& state,int u,int count,bool delta,int epoch,Cost& cost,
           std::vector<std::unique_ptr<Principal>>& principals,
           ndn::security::KeyChain& authority,const ndn::security::Certificate& cert) {
  const auto startedIssue=Clock::now(); const auto cpuIssue=std::clock();
  const auto oldNegative=cost.negativeMs, oldNegativeCpu=cost.negativeCpuMs;
  auto& support=alg::ABESupport::getInstance();
  if (state.arm=="BATCHED_RSA") {
    std::map<int,ndn::Buffer> outgoing;
    for(int p=delta?count-1:0;p<count;++p) outgoing.emplace(p,state.serviceKeys.at(p));
    auto decoded=decodeKeys(distribute(encodeKeys(outgoing),*principals[u],epoch,state.arm,cost,
                                       authority,cert,*principals[USERS],u==0));
    for(const auto& item:decoded) state.rsaKeys[u][item.first]=item.second;
  } else {
    if(state.arm=="AGGREGATED_ABE") state.abeKeys[u].clear();
    for(int p=state.arm=="AGGREGATED_ABE"?count-1:(delta?count-1:0);p<count;++p) {
      const auto started=Clock::now();
      auto key=support.kpPrvKeyGen(state.params,state.master,
                                 state.arm=="AGGREGATED_ABE"?policy(count):attr(p));
      cost.keygenMs+=since(started);
      auto material=distribute(key.toBuffer(),*principals[u],epoch,state.arm,cost,
                              authority,cert,*principals[USERS],u==0);
      alg::PrivateKey received; received.fromBuffer(material);
      state.abeKeys[u].push_back(std::move(received));
    }
  }
  cost.managementMs+=since(startedIssue)-(cost.negativeMs-oldNegative);
  cost.managementCpuMs+=cpuSince(cpuIssue)-(cost.negativeCpuMs-oldNegativeCpu);
}
void publishParams(State& state,int epoch,int recipients,Cost& cost,
                   ndn::security::KeyChain& authority,const ndn::security::Certificate& cert) {
  if(state.arm=="BATCHED_RSA") return;
  const auto started=Clock::now(); const auto cpu=std::clock();
  const auto bytes=state.params.toBuffer(); uint8_t digest[SHA256_DIGEST_LENGTH];
  alg::PublicParams decoded; decoded.fromBuffer(bytes);
  check(decoded.toBuffer()==bytes,"PP roundtrip changed material"); state.params=std::move(decoded);
  SHA256(bytes.data(),bytes.size(),digest);
  ndn::Name base(cert.getIdentity()); base.append("NDNSF").append("public-parameters")
    .append(literal("name=KPABE")).appendVersion(epoch)
    .append(ndn::name::Component(ndn::tlv::GenericNameComponent,{digest,sizeof(digest)}));
  std::vector<ndn::Data> packets;
  for(size_t offset=0,seq=0;offset<bytes.size();offset+=1500,++seq) {
    ndn::Data data(ndn::Name(base).appendSegment(seq));
    data.setContent({bytes.data()+offset,std::min<size_t>(1500,bytes.size()-offset)});
    authority.sign(data,ndn::security::signingByCertificate(cert));
    for(int u=0;u<recipients;++u) check(ndn::security::verifySignature(data,cert),"PP signature");
    cost.publicParamsBytes+=data.wireEncode().size(); ++cost.publicParamsPackets;
    if (archive) packets.push_back(data);
  }
  // Wire counts serialize each shared PP once; these are not measured network bytes.
  cost.managementMs+=since(started); cost.managementCpuMs+=cpuSince(cpu);
  if (archive) archive->packets("public-parameters",epoch,ndn::Name(),packets);
}
void measureAccess(State& state,int epoch,int repetition,
                   ndn::security::KeyChain& authority,const ndn::security::Certificate& cert) {
  const ndn::Buffer plain(64,0x43);
  const auto start=Clock::now(); const auto cpu=std::clock();
  const auto c=encrypt(state,0,plain);
  auto content=c.abe.makeDataContent(); content.encode();
  ndn::Name name(cert.getIdentity()); name.append("NDNSF").append("component-data")
    .append(literal("name=service-0")).appendVersion(epoch);
  uint8_t digest[SHA256_DIGEST_LENGTH]; SHA256(content.data(),content.size(),digest);
  name.append(ndn::name::Component(ndn::tlv::GenericNameComponent,{digest,sizeof(digest)}));
  ndn::Data data(name); data.setContent(content); authority.sign(data,ndn::security::signingByCertificate(cert));
  check(ndn::security::verifySignature(data,cert),"business signature");
  size_t ckWire=0;
  if(state.arm!="BATCHED_RSA") {
    auto ckContent=c.abe.m_contentKey->makeCKContent(); ckContent.encode();
    uint8_t ckDigest[SHA256_DIGEST_LENGTH]; SHA256(ckContent.data(),ckContent.size(),ckDigest);
    ndn::Name ckName(cert.getIdentity()); ckName.append("NDNSF").append("content-key")
      .append(literal("name=service-0")).appendVersion(epoch)
      .append(ndn::name::Component(ndn::tlv::GenericNameComponent,{ckDigest,sizeof(ckDigest)}));
    ndn::Data ck(ckName); ck.setContent(ckContent);
    authority.sign(ck,ndn::security::signingByCertificate(cert));
    check(ndn::security::verifySignature(ck,cert),"content-key signature");
    ckWire=ck.wireEncode().size();
    if (archive) archive->packets("content-key",epoch,ndn::Name(),{ck});
  }
  const double publicationMs=since(start), publicationCpu=cpuSince(cpu);
  if (archive) archive->packets("business",epoch,ndn::Name(),{data});
  alg::ABESupport::getInstance().clearCachedContentKeys();
  auto t=Clock::now(); auto tc=std::clock();
  check(access(state,1,0,c,plain),"cold surviving access");
  const auto coldMs=since(t), coldCpu=cpuSince(tc);
  t=Clock::now(); tc=std::clock();
  for(int i=0;i<100;++i) check(access(state,1,0,c,plain),"warm surviving access");
  const auto warmMs=since(t), warmCpu=cpuSince(tc);
  std::cout<<Json{{"schema","ndnsf-system-access-v1"},{"arm",state.arm},{"epoch",epoch},
    {"archive_export",archive!=nullptr},
    {"permissions",state.permissions},{"repetition",repetition},{"plain_bytes",64},
    {"business_wire_bytes",data.wireEncode().size()},{"publication_elapsed_ms",publicationMs},
    {"content_key_wire_bytes_once",ckWire},
    {"publication_cpu_ms",publicationCpu},{"cold_access_elapsed_ms",coldMs},{"cold_access_cpu_ms",coldCpu},
    {"warm_repeated_accesses",100},{"warm_total_elapsed_ms",warmMs},{"warm_total_cpu_ms",warmCpu},
    {"oracle",true},{"scope","serialized business Data plus separate CK once; local crypto, not network requests/fetch cost"}}.dump()<<'\n';
}
void emit(const State& state,int repetition,const std::string& phase,const Cost& cost) {
  std::cout<<Json{{"schema","ndnsf-system-cost-v1"},{"arm",state.arm},{"phase",phase},
    {"archive_export",archive!=nullptr},
    {"repetition",repetition},{"users",USERS},{"permissions",state.permissions},
    {"setup_elapsed_ms",cost.setupMs},{"keygen_elapsed_ms",cost.keygenMs},{"transport_elapsed_ms",cost.transportMs},
    {"setup_cpu_ms",cost.setupCpuMs},{"management_elapsed_ms",cost.managementMs},{"management_cpu_ms",cost.managementCpuMs},
    {"public_parameter_wire_bytes_once",cost.publicParamsBytes},{"public_parameter_packets_once",cost.publicParamsPackets},
    {"management_objects",cost.objects},{"management_packets",cost.packets},
    {"management_wire_bytes",cost.wireBytes},{"raw_material_bytes",cost.rawBytes},
    {"oracle",true},{"scope","local component; comparison transport, not production AA network"}}.dump()<<'\n';
}
void run(const std::string& arm,int p,int repetition,
         std::vector<std::unique_ptr<Principal>>& principals,
         ndn::security::KeyChain& authority,const ndn::security::Certificate& cert) {
  State state; state.arm=arm; state.permissions=p;
  if (archive) archive->begin(arm,p,repetition);
  Cost initial,grant,revoke;
  auto& support=alg::ABESupport::getInstance(); support.clearCachedContentKeys();
  auto started=Clock::now();
  auto cpu=std::clock();
  if(arm=="BATCHED_RSA") {
    for(int s=0;s<p;++s) state.serviceKeys[s]=na::Aes::generateKey(ndn::AesKeyParams(256));
  } else support.kpInit(state.params,state.master);
  initial.setupMs=since(started);
  initial.setupCpuMs=cpuSince(cpu);
  publishParams(state,1,USERS,initial,authority,cert);
  for(int u=0;u<USERS;++u) issue(state,u,p,false,1,initial,principals,authority,cert);
  const ndn::Buffer plain(64,0x37);
  for(int s=0;s<p;++s) {
    auto c=encrypt(state,s,plain);
    for(int u=0;u<USERS;++u) check(access(state,u,s,c,plain),"initial authorized access failed");
    exportProbe(state,c,s,1,authority,cert);
  }
  measureAccess(state,1,repetition,authority,cert);
  started=Clock::now(); cpu=std::clock();
  if(arm=="BATCHED_RSA") state.serviceKeys[p]=na::Aes::generateKey(ndn::AesKeyParams(256));
  grant.setupMs=since(started);
  grant.setupCpuMs=cpuSince(cpu);
  issue(state,0,p+1,true,2,grant,principals,authority,cert);
  auto extra=encrypt(state,p,plain);
  check(access(state,0,p,extra,plain),"grant target cannot access");
  for(int u=1;u<USERS;++u) check(!access(state,u,p,extra,plain),"grant leaked to other identity");
  exportProbe(state,extra,p,2,authority,cert);
  for(int s=0;s<p;++s) {
    auto c=encrypt(state,s,plain);
    for(int u=0;u<USERS;++u) check(access(state,u,s,c,plain),"grant lost existing permission");
    exportProbe(state,c,s,2,authority,cert);
  }
  measureAccess(state,2,repetition,authority,cert);
  auto oldAbe=state.abeKeys[0]; auto oldRsa=state.rsaKeys[0];
  started=Clock::now(); cpu=std::clock();
  if(arm=="BATCHED_RSA") {
    for(int s=0;s<=p;++s) state.serviceKeys[s]=na::Aes::generateKey(ndn::AesKeyParams(256));
  } else { support.kpInit(state.params,state.master); support.clearCachedContentKeys(); }
  revoke.setupMs=since(started);
  revoke.setupCpuMs=cpuSince(cpu);
  publishParams(state,3,USERS-1,revoke,authority,cert);
  for(int u=1;u<USERS;++u) {
    state.abeKeys[u].clear();
    issue(state,u,p,false,3,revoke,principals,authority,cert);
  }
  measureAccess(state,3,repetition,authority,cert);
  // Keep the revoked identity's actual old keys; never substitute empty material.
  state.abeKeys[0]=oldAbe; state.rsaKeys[0]=oldRsa;
  for(int s=0;s<=p;++s) {
    auto c=encrypt(state,s,plain);
    check(!access(state,0,s,c,plain),"revoked old material reads future epoch");
    for(int u=1;u<USERS;++u) check(access(state,u,s,c,plain)==(s<p),"survivor permission matrix changed");
    exportProbe(state,c,s,3,authority,cert);
  }
  emit(state,repetition,"initial",initial); emit(state,repetition,"grant",grant); emit(state,repetition,"revoke",revoke);
  if (archive) archive->finishCase();
}
} // namespace
int main(int argc,char** argv) try {
  const int repetitions=argc>=2?std::stoi(argv[1]):5;
  check(argc<=3 && repetitions>=1 && repetitions<=5,"usage: repetitions1..5 [new-export-directory]");
  if (argc==3) archive=std::make_unique<CredentialArchive>(argv[2]);
  ndn::security::KeyChain authority("pib-memory:cost-authority","tpm-memory:cost-authority");
  const auto cert=authority.createIdentity(ndn::Name("/system-benefits/controller"),ndn::EcKeyParams())
    .getDefaultKey().getDefaultCertificate();
  auto started=Clock::now();
  std::vector<std::unique_ptr<Principal>> principals;
  for(int u=0;u<=USERS;++u) principals.push_back(std::make_unique<Principal>(u));
  if (archive) {
    archive->authority(cert);
    for(int u=0;u<=USERS;++u) archive->bootstrap(principals[u]->keys,principals[u]->cert,u);
  }
  std::cout<<Json{{"event","identity_bootstrap"},{"users",USERS},{"rsa_bits",2048},
    {"ms",since(started)},{"negative_principals",1},{"shared_by_all_arms",true}}.dump()<<'\n';
  const std::vector<std::string> arms{"AGGREGATED_ABE","PERMISSION_ABE","BATCHED_RSA"};
  for(int p:{1,4,16}) for(int r=1;r<=repetitions;++r)
    for(int i=0;i<3;++i) run(arms[(i+r-1)%3],p,r,principals,authority,cert);
  if (archive) archive->finish(repetitions);
  std::cout<<Json{{"event","complete"},{"oracle",true},{"repetitions",repetitions}}.dump()<<std::endl;
  return 0;
} catch(const std::exception& e) {
  std::cerr<<"SYSTEM_COST_FAILURE "<<e.what()<<std::endl; return 2;
} catch(...) { std::cerr<<"SYSTEM_COST_FAILURE crypto exception"<<std::endl; return 2; }
