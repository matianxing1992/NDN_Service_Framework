#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>


//#include <attribute-authority.hpp> // or <nac-abe/attribute-authority.hpp>
#include <nac-abe/attribute-authority.hpp>
#include <nac-abe/cache-producer.hpp>
#include <iostream>

namespace examples {
using ndn::nacabe::KpAttributeAuthority;
class AttributeAuthority
{
public:
  AttributeAuthority()
    : m_aaCert(m_keyChain.getPib().getIdentity("/muas/aa").getDefaultKey().getDefaultCertificate())
    , m_aa(m_aaCert, m_face, m_validator, m_keyChain)
  {
    //auto serviceProviderCert1 = m_keyChain.getPib().getIdentity("/example/service-provider").getDefaultKey().getDefaultCertificate();
    // 1. this approach will directly use the certificate passed in without validation
    // m_aa.addNewPolicy(consumerCert1, "attribute");
    // 2. this approach will try fetch corresponding certificate when receiving 
    //    corresponding DKEY Interest
    auto Cert1 = m_keyChain.getPib().getIdentity("/muas/drone1").getDefaultKey().getDefaultCertificate();
    auto Cert3 = m_keyChain.getPib().getIdentity("/muas/gs1").getDefaultKey().getDefaultCertificate();
    //auto Cert3 = m_keyChain.getPib().getIdentity("/muas/drone2").getDefaultKey().getDefaultCertificate();
    m_aa.addNewPolicy(Cert1, "/ID/muas/drone1 OR /SERVICE/FlightControl/ManualControl");
    m_aa.addNewPolicy(Cert3, "/ID/muas/gs1 OR /PERMISSION/muas/drone1/FlightControl/ManualControl");

    m_validator.load("trust-schema.conf");

    // root certificate filter
    ndn::security::Certificate rootCert(m_keyChain.getPib().getIdentity("/muas").getDefaultKey().getDefaultCertificate());
    m_face.setInterestFilter(rootCert.getKeyName(),
      [&] (auto&...) {
        m_face.put(rootCert); 
      }
    );
        // root certificate filter
    ndn::security::Certificate gsCert(m_keyChain.getPib().getIdentity("/muas/gs1").getDefaultKey().getDefaultCertificate());
    m_face.setInterestFilter(gsCert.getKeyName(),
      [&] (auto&...) {
        m_face.put(gsCert); 
      }
    );
        // root certificate filter
    ndn::security::Certificate droneCert(m_keyChain.getPib().getIdentity("/muas/drone1").getDefaultKey().getDefaultCertificate());
    m_face.setInterestFilter(droneCert.getKeyName(),
      [&] (auto&...) {
        m_face.put(droneCert); 
      }
    );
    // self certificate filter
    m_face.setInterestFilter(m_aaCert.getKeyName(),
      [this] (auto&...) {
        m_face.put(m_aaCert); 
      }
    );
  }

  void
  run()
  {
    m_face.processEvents();
  }

private:
  ndn::Face m_face;
  ndn::KeyChain m_keyChain;
  ndn::ValidatorConfig m_validator{m_face};
  ndn::security::Certificate m_aaCert;
  ndn::nacabe::KpAttributeAuthority m_aa;
};

} // namespace examples

int
main(int argc, char** argv)
{
  try {
    examples::AttributeAuthority aa;
    aa.run();
    return 0;
  }
  catch (const std::exception& e) {
    std::cerr << "ERRAND: " << e.what() << std::endl;
    return 1;
  }
}