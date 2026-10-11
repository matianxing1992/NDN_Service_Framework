#ifndef NDN_SERVICE_FRAMEWORK_PEER_PROBE_NAMING_HPP
#define NDN_SERVICE_FRAMEWORK_PEER_PROBE_NAMING_HPP

#include <ndn-cxx/interest.hpp>
#include <ndn-cxx/name.hpp>
#include <cstdint>
#include <string>

namespace ndn_service_framework {
    /** Naming context taken from the existing signed probe parameters. */
    struct PeerProbeCall {
        std::string operation;
        ndn::Name observer;
        ndn::Name target;
        std::string requestScopeDigest;
        std::string runNonce;
        uint64_t segment = 0;
        uint64_t segmentCount = 0;
        uint64_t contentBytes = 0;
        bool requireRtt = false;
        bool requireGoodput = false;
    };
    ndn::Name makePeerProbeRpcName(const PeerProbeCall& call);
    bool matchesPeerProbeRpcName(const ndn::Interest& interest, const PeerProbeCall& call);
}
#endif
