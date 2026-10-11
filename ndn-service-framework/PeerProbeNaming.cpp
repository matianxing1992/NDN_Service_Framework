#include "PeerProbeNaming.hpp"
#include "ProducerDataNaming.hpp"
#include <algorithm>
#include <stdexcept>

namespace ndn_service_framework {
    ndn::Name
    makePeerProbeRpcName(const PeerProbeCall& call)
    {
        const bool rtt = call.operation == "PROBE-RTT";
        const bool ready = call.operation == "PROBE-READY";
        const bool goodput = call.operation == "PROBE-GOODPUT";
        if ((!rtt && !ready && !goodput) || call.observer.empty() ||
            call.target.empty() || call.observer == call.target ||
            call.requestScopeDigest.empty() || call.requestScopeDigest.size() > 128 ||
            call.runNonce.empty() || call.runNonce.size() > 20 ||
            !std::all_of(call.runNonce.begin(), call.runNonce.end(),
                         [](unsigned char c) { return c >= '0' && c <= '9'; }) ||
            (rtt && (!call.requireRtt || call.requireGoodput)) ||
            (goodput && (call.requireRtt || !call.requireGoodput)) ||
            (ready && !call.requireRtt && !call.requireGoodput) ||
            (goodput && (call.segmentCount == 0 || call.segment >= call.segmentCount ||
                         call.contentBytes == 0)) ||
            (!goodput && (call.segment != 0 || call.segmentCount != 0 || call.contentBytes != 0)))
            throw std::invalid_argument("invalid peer probe naming context");
        const auto integer = [](uint64_t value) {
            std::vector<uint8_t> bytes(8);
            for (size_t i = 0; i < bytes.size(); ++i)
                bytes[bytes.size() - 1 - i] = static_cast<uint8_t>(value >> (8 * i));
            return bytes;
        };
        const auto& observer = call.observer.wireEncode();
        const auto& target = call.target.wireEncode();
        const auto digest = computeNamingDigest("ndnsf-naming/probe-call", {
            {1, 2, {call.operation.begin(), call.operation.end()}},
            {2, 1, {observer.data(), observer.data() + observer.size()}},
            {3, 1, {target.data(), target.data() + target.size()}},
            {4, 2, {call.requestScopeDigest.begin(), call.requestScopeDigest.end()}},
            {5, 2, {call.runNonce.begin(), call.runNonce.end()}},
            {6, 3, integer(call.segment)}, {7, 3, integer(call.segmentCount)},
            {8, 3, integer(call.contentBytes)}, {9, 3, integer(call.requireRtt)},
            {10, 3, integer(call.requireGoodput)}});
        auto name = makeProducerRpcName(call.target, call.operation, digest, call.runNonce);
        checkNamingNameSize(name);
        return name;
    }

    bool
    matchesPeerProbeRpcName(const ndn::Interest& interest, const PeerProbeCall& call)
    {
        try {
            const auto& name = interest.getName();
            return !name.empty() && name[-1].isParametersSha256Digest() &&
                interest.hasApplicationParameters() && interest.isParametersDigestValid() &&
                name.getPrefix(-1) == makePeerProbeRpcName(call);
        }
        catch (const std::exception&) { return false; }
    }

}
