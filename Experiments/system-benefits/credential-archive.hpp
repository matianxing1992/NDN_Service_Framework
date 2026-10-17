#pragma once

#include <ndn-cxx/security/safe-bag.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

// Trusted experiment bootstrap; importing files does not authorize their content.
class CredentialArchive {
public:
  explicit CredentialArchive(const std::filesystem::path& root) : m_root(root) {
    if (!std::filesystem::create_directory(root))
      throw std::runtime_error("export directory must not already exist");
    std::filesystem::permissions(root,std::filesystem::perms::owner_all,
                                std::filesystem::perm_options::replace);
  }
  void bootstrap(ndn::security::KeyChain& keys,
                 const ndn::security::Certificate& cert,int recipient) {
    constexpr char password[]="ndnsf-local-comparison-bootstrap";
    auto bag=keys.exportSafeBag(cert,password,sizeof(password)-1);
    write(m_root/("recipient-"+std::to_string(recipient)+".safebag"),bag->wireEncode());
    write(m_root/("recipient-"+std::to_string(recipient)+".cert"),cert.wireEncode());
  }
  void authority(const ndn::security::Certificate& cert) {
    write(m_root/"authority.cert",cert.wireEncode());
  }
  void begin(const std::string& arm,int permissions,int repetition) {
    m_case=m_root/(arm+"-p"+std::to_string(permissions)+"-r"+std::to_string(repetition));
    if (!std::filesystem::create_directory(m_case)) throw std::runtime_error("duplicate exported case");
    m_records=nlohmann::json::array();
    m_identity={{"arm",arm},{"permissions",permissions},{"repetition",repetition}};
  }
  void packets(const std::string& kind,int epoch,const ndn::Name& recipient,
               const std::vector<ndn::Data>& packets,int permission=-1) {
    nlohmann::json files=nlohmann::json::array();
    for (const auto& packet:packets) {
      const auto file="packet-"+std::to_string(m_packet++)+".wire";
      write(m_case/file,packet.wireEncode());
      files.push_back({{"file",file},{"name",packet.getName().toUri()},
                       {"wire_bytes",packet.wireEncode().size()}});
    }
    m_records.push_back({{"kind",kind},{"epoch",epoch},
                         {"recipient",recipient.toUri()},{"permission",permission},{"packets",files}});
  }
  void finishCase() {
    auto manifest=m_identity;
    manifest["schema"]="ndnsf-credential-archive-v1";
    manifest["component_oracle"]=true; manifest["objects"]=m_records;
    writeJson(m_case/"manifest.json",manifest);
    m_cases.push_back(m_case.filename().string());
  }
  void finish(int repetitions) {
    writeJson(m_root/"complete.json",{{"schema","ndnsf-credential-archive-v1"},
      {"component_oracle",true},{"cases",m_cases},
      {"repetitions",repetitions},
      {"scope","component-generated original packets; not network measurement"}});
  }
private:
  static void write(const std::filesystem::path& file,const ndn::Block& bytes) {
    std::ofstream stream(file,std::ios::binary);
    stream.exceptions(std::ios::badbit|std::ios::failbit);
    stream.write(reinterpret_cast<const char*>(bytes.data()),bytes.size()); stream.close();
  }
  static void writeJson(const std::filesystem::path& file,const nlohmann::json& value) {
    std::ofstream stream(file); stream.exceptions(std::ios::badbit|std::ios::failbit);
    stream<<value.dump(2)<<'\n'; stream.close();
  }
  std::filesystem::path m_root,m_case;
  nlohmann::json m_records,m_identity,m_cases=nlohmann::json::array();
  size_t m_packet=0;
};
