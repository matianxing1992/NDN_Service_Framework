#pragma once

#include <nac-abe/algo/abe-support.hpp>
#include <nac-abe/ndn-crypto/data-enc-dec.hpp>
#include <ndn-cxx/encoding/block-helpers.hpp>
#include <ndn-cxx/security/safe-bag.hpp>
#include <ndn-cxx/security/verification-helpers.hpp>
#include <nlohmann/json.hpp>
#include <openssl/sha.h>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <memory>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace credential204 {
namespace alg=ndn::nacabe::algo;
using Json=nlohmann::json;
inline void require(bool value,const char* reason) {
  if(!value) throw std::runtime_error(reason);
}
inline ndn::Buffer readBytes(const std::filesystem::path& path) {
  require(!std::filesystem::is_symlink(path) && std::filesystem::is_regular_file(path),"invalid archive file");
  const auto size=std::filesystem::file_size(path);
  require(size>0 && size<=1024*1024,"archive file size outside bounds");
  ndn::Buffer bytes(size); std::ifstream stream(path,std::ios::binary);
  stream.exceptions(std::ios::badbit|std::ios::failbit);
  stream.read(reinterpret_cast<char*>(bytes.data()),bytes.size()); return bytes;
}
inline Json readJson(const std::filesystem::path& path) {
  const auto bytes=readBytes(path); return Json::parse(bytes.begin(),bytes.end());
}
inline int boundedInt(const Json& value,int low,int high) {
  require(value.is_number_integer(),"non-integer archive number");
  if(value.is_number_unsigned()) {
    const auto number=value.get<uint64_t>();
    require(number<=static_cast<uint64_t>(high),"archive number overflow");
    require(number>=static_cast<uint64_t>(low),"archive number below bounds");
    return static_cast<int>(number);
  }
  const auto number=value.get<int64_t>();
  require(number>=low && number<=high,"archive number outside bounds");
  return static_cast<int>(number);
}
inline std::filesystem::path leaf(const std::filesystem::path& root,const std::string& value) {
  const std::filesystem::path path(value);
  require(!value.empty() && path==path.filename() && value!="." && value!="..","invalid archive leaf");
  return root/path;
}
inline ndn::Data readPacket(const std::filesystem::path& root,const Json& entry) {
  const auto bytes=readBytes(leaf(root,entry.at("file").get<std::string>()));
  ndn::Data data{ndn::Block(bytes)};
  const auto& wire=data.wireEncode();
  require(bytes.size()==wire.size() && std::equal(bytes.begin(),bytes.end(),wire.begin()),"wire reload changed Data");
  require(data.getName()==ndn::Name(entry.at("name").get<std::string>()) &&
          bytes.size()==entry.at("wire_bytes").get<size_t>(),"packet manifest binding");
  return data;
}

// One recipient owns only its own imported key. The same engine is used by the
// offline native oracle and, subsequently, by actual network Data callbacks.
class Receiver {
public:
  Receiver(int id,std::string arm,const ndn::Buffer& bag,
           const ndn::security::Certificate& own,
           const ndn::security::Certificate& authority)
    : m_id(id),m_arm(std::move(arm)),m_own(own),m_authority(authority),
      m_keys("pib-memory:receiver","tpm-memory:receiver") {
    require(id>=0 && id<=8,"recipient outside profile");
    require(m_arm=="AGGREGATED_ABE" || m_arm=="PERMISSION_ABE" || m_arm=="BATCHED_RSA","unknown permission arm");
    require(own.getIdentity()==ndn::Name("/system-benefits/user/"+std::to_string(id)),"recipient identity binding");
    constexpr char password[]="ndnsf-local-comparison-bootstrap";
    m_keys.importSafeBag(ndn::security::SafeBag(ndn::Block(bag)),password,sizeof(password)-1);
    require(m_keys.getPib().getIdentities().size()==1,"recipient imports other identities");
    require(m_keys.getPib().getIdentity(own.getIdentity()).getDefaultKey().getDefaultCertificate().wireEncode()==own.wireEncode(),"SafeBag certificate mismatch");
    require(authority.getIdentity()==ndn::Name("/system-benefits/controller") && authority.isValid(),"authority pin/validity");
  }
  void beginEpoch(int epoch) {
    require(epoch==m_epoch+1 && epoch<=3,"unexpected phase transition");
    m_epoch=epoch;
    // u0 is revoked and deliberately keeps the actual old keys.
    if(epoch==3 && m_id>0 && m_id<8) { m_abe.clear(); m_rsa.clear(); }
    alg::ABESupport::getInstance().clearCachedContentKeys();
  }
  ndn::Buffer validate(const std::vector<ndn::Data>& packets,int epoch,
                       const std::string& category,bool segmented) const {
    require(!packets.empty(),"empty packet object");
    ndn::Name prefix(m_authority.getIdentity()); prefix.append("NDNSF").append(category);
    const auto base=segmented?packets.front().getName().getPrefix(-1):packets.front().getName();
    require(prefix.isPrefixOf(base),"wrong producer/category namespace");
    require(base.size()==prefix.size()+(category=="credentials"?m_own.getIdentity().size()+3:3),"unexpected name component count");
    if(category=="credentials" || category=="public-parameters" || category=="component-data" || category=="content-key") {
      const auto& semantic=base.get(prefix.size());
      const std::string expected=category=="credentials"?"name="+m_arm:category=="public-parameters"?"name=KPABE":"name=service-0";
      require(semantic.type()==ndn::tlv::GenericNameComponent && semantic.value_size()==expected.size() &&
        std::equal(semantic.value_begin(),semantic.value_end(),expected.begin()),"permission scheme name binding");
    }
    require(base.get(-2).isVersion() && base.get(-2).toVersion()==static_cast<uint64_t>(epoch),"name epoch mismatch");
    ndn::Buffer joined;
    for(size_t i=0;i<packets.size();++i) {
      const auto& data=packets[i];
      require(data.getName()==(segmented?ndn::Name(base).appendSegment(i):base),"segment sequence/name mismatch");
      require(segmented || packets.size()==1,"non-segmented object cardinality");
      require(m_authority.isValid() && data.getSignatureInfo().getKeyLocator().getName()==m_authority.getName() &&
              ndn::security::verifySignature(data,m_authority),"Data signature/certificate binding");
      const auto& content=data.getContent();
      const auto* first=segmented?content.value():content.data();
      const auto count=segmented?content.value_size():content.size();
      joined.insert(joined.end(),first,first+count);
    }
    uint8_t digest[SHA256_DIGEST_LENGTH]; SHA256(joined.data(),joined.size(),digest);
    const auto& component=base.get(-1);
    require(component.type()==ndn::tlv::GenericNameComponent && component.value_size()==sizeof(digest) &&
            std::equal(component.value_begin(),component.value_end(),digest),"content digest mismatch");
    return joined;
  }
  void acceptParams(const std::vector<ndn::Data>& packets,int epoch) {
    require(m_arm!="BATCHED_RSA" && epoch==m_epoch,"unexpected public parameters");
    auto bytes=validate(packets,epoch,"public-parameters",true);
    alg::PublicParams params; params.fromBuffer(bytes); m_params=std::move(params); m_hasParams=true;
  }
  void acceptManagement(const std::vector<ndn::Data>& packets,int epoch) {
    require(epoch==m_epoch && !(epoch==3 && m_id==0) && m_id<8,"recipient not entitled in phase");
    auto cipher=validate(packets,epoch,"credentials",true);
    const auto bytes=ndn::nacabe::decryptDataContent(ndn::Block(cipher),m_keys.getTpm(),m_own.getName());
    ndn::Block envelope(bytes); envelope.parse();
    require(envelope.type()==128 && envelope.elements().size()==3 &&
            envelope.elements()[0].type()==ndn::tlv::Name && envelope.elements()[1].type()==129 &&
            envelope.elements()[2].type()==130,"management envelope fields");
    require(ndn::Name(envelope.elements()[0])==m_own.getIdentity() &&
            ndn::readNonNegativeInteger(envelope.elements()[1])==static_cast<uint64_t>(epoch),"management recipient/epoch binding");
    // Name::append(string URI) appends its complete Name, not one string component.
    const auto namedRecipient=packets.front().getName().getSubName(m_authority.getIdentity().size()+3,m_own.getIdentity().size());
    require(namedRecipient==m_own.getIdentity(),"management name recipient binding");
    const auto& material=envelope.elements()[2];
    ndn::Buffer raw(material.value_begin(),material.value_end());
    if(m_arm!="BATCHED_RSA") {
      alg::PrivateKey key; key.fromBuffer(raw);
      if(m_arm=="AGGREGATED_ABE") m_abe.clear();
      m_abe.push_back(std::move(key));
    } else {
      ndn::Block bundle(raw); bundle.parse();
      require(bundle.type()==130 && !bundle.elements().empty(),"RSA bundle type/empty");
      std::map<int,ndn::Buffer> received;
      for(auto entry:bundle.elements()) {
        entry.parse(); require(entry.type()==131 && entry.elements().size()==2 &&
          entry.elements()[0].type()==ndn::tlv::Name && entry.elements()[1].type()==132,"RSA bundle fields");
        const ndn::Name service(entry.elements()[0]);
        require(service.size()==2 && service.get(0)==ndn::name::Component("service"),"RSA service namespace");
        const auto text=service.get(1).toUri(); size_t end=0; const int p=std::stoi(text,&end);
        require(end==text.size() && p>=0 && p<=16 && text==std::to_string(p),"RSA permission index");
        const auto& key=entry.elements()[1]; require(key.value_size()==32,"RSA AES key size");
        require(received.emplace(p,ndn::Buffer(key.value_begin(),key.value_end())).second,"duplicate RSA permission");
      }
      for(auto& pair:received) m_rsa[pair.first]=std::move(pair.second);
    }
  }
  bool canAccess(const ndn::Data& business,const ndn::Data* ck,int p,int epoch) const {
    require(epoch==m_epoch,"probe phase mismatch");
    require(p>=0 && p<=16,"probe permission outside profile");
    validate({business},epoch,"component-probe",false);
    ndn::Name base(m_authority.getIdentity()); base.append("NDNSF").append("component-probe");
    const std::string semantic="name=service-"+std::to_string(p);
    require(business.getName().get(base.size()).type()==ndn::tlv::GenericNameComponent &&
      business.getName().get(base.size()).value_size()==semantic.size() &&
      std::equal(business.getName().get(base.size()).value_begin(),business.getName().get(base.size()).value_end(),semantic.begin()),"probe service binding");
    auto content=business.getContent(); content.parse();
    require(content.elements().size()==2 && content.elements()[0].type()==602 && content.elements()[1].type()==603,"business codec fields");
    alg::CipherText cipher;
    const auto& encoded=content.elements()[0]; cipher.m_content=ndn::Buffer(encoded.value_begin(),encoded.value_end());
    const auto plainSize=ndn::readNonNegativeInteger(content.elements()[1]);
    require(plainSize==64,"probe plaintext size");
    cipher.m_plainTextSize=static_cast<uint32_t>(plainSize);
    if(m_arm!="BATCHED_RSA") {
      require(ck!=nullptr && m_hasParams,"missing ABE parameters/CK");
      validate({*ck},epoch,"component-probe-key",false);
      require(ck->getName().get(base.size())==business.getName().get(base.size()),"CK service binding");
      auto wrapped=ck->getContent(); wrapped.parse();
      require(wrapped.elements().size()==1 && wrapped.elements()[0].type()==601,"CK codec fields");
      const auto& key=wrapped.elements()[0];
      cipher.m_contentKey=std::make_shared<alg::ContentKey>("",ndn::Buffer(key.value_begin(),key.value_end()));
    } else require(ck==nullptr,"unexpected RSA wrapped CK");
    const ndn::Buffer plain(64,0x37);
    if(m_arm=="BATCHED_RSA") {
      const auto it=m_rsa.find(p); if(it==m_rsa.end()) return false;
      try {
        const auto& bytes=it->second;
        oabe::OpenABESymKeyEnc aes(std::string(reinterpret_cast<const char*>(bytes.data()),bytes.size()));
        require(!cipher.m_content.empty() && cipher.m_content.back()==0,"AES codec terminator");
        const auto result=aes.decrypt(std::string(reinterpret_cast<const char*>(cipher.m_content.data()),cipher.m_content.size()-1));
        return ndn::Buffer(result.begin(),result.end())==plain;
      } catch(...) { return false; }
    }
    for(const auto& key:m_abe) try {
      if(alg::ABESupport::getInstance().kpDecrypt(m_params,key,cipher)==plain) return true;
    } catch(...) {}
    return false;
  }
private:
  int m_id,m_epoch=0;
  std::string m_arm;
  ndn::security::Certificate m_own,m_authority;
  ndn::security::KeyChain m_keys;
  alg::PublicParams m_params;
  bool m_hasParams=false;
  std::vector<alg::PrivateKey> m_abe;
  std::map<int,ndn::Buffer> m_rsa;
};
} // namespace credential204
