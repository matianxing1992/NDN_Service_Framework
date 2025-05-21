#!/usr/bin/env python

"""
This example shows how to enable the adhoc mode
Alternatively, you can use the MANET routing protocol of your choice
"""
from mininet.log import setLogLevel, info
from minindn.wifi.minindnwifi import MinindnAdhoc
from minindn.util import MiniNDNWifiCLI
from minindn.apps.app_manager import AppManager
from minindn.apps.nfd import Nfd
from minindn.helpers.nfdc import Nfdc
from minindn.apps.nlsr import Nlsr
from minindn.helpers.ndnping import NDNPing
from mn_wifi.link import wmediumd
from mn_wifi.wmediumdConnector import interference
import time

import random

random.seed(42)

def generateAndSignCertificates(ndn: MinindnAdhoc):
    # delete identities
    for node in ndn.net.stations:
        node.cmd('ndnsec delete /muas')
        node.cmd('ndnsec delete /muas/aa')
        for node2 in ndn.net.stations:
            node.cmd('ndnsec delete /muas/{}'.format(node2.name))

    # generate trust anchor on the first node
    controller_node = ndn.net.stations[0]
    controller_node.cmd('ndnsec key-gen -t r /muas > /tmp/muas.key')
    controller_node.cmd('ndnsec cert-dump -i /muas > /tmp/muas.cert')
    controller_node.cmd('ndnsec-export -P 123456 -o /tmp/muas.ndnkey -i /muas')

    info('Generating certificates for /muas/aa\n')

    controller_node.cmd('ndnsec key-gen -t r /muas/aa > /tmp/aa.key')
    controller_node.cmd('ndnsec cert-gen -s /muas -i default /tmp/aa.key > /tmp/aa.cert')
    controller_node.cmd('ndnsec-export -P 123456 -o /tmp/aa.ndnkey -i /muas/aa')
    
    # controller_node.cmd('ndnsec cert-install -f /tmp/muas.key')
    # controller_node.cmd('ndnsec cert-dump -i /muas > /tmp/muas.cert')

    info('Generating keys for other nodes\n')
    # generate key on controller_node
    for node in ndn.net.stations:
        controller_node.cmd('ndnsec key-gen -t r /muas/{} > /tmp/{}.key'.format(node.name, node.name))
    
    info('Installing keys on nodes\n')
    # install keys on controller_node
    for node in ndn.net.stations:
        controller_node.cmd('ndnsec cert-install -f /tmp/{}.key'.format(node.name))

    info('Signing certificates on controller_node\n')
    # sign certificates on controller_node using /muas
    for node in ndn.net.stations:
        controller_node.cmd('ndnsec cert-gen -s /muas -i default /tmp/{}.key > /tmp/{}.cert'.format(node.name, node.name))
        controller_node.cmd('ndnsec-export -P 123456 -o /tmp/{}.ndnkey -i /muas/{}'.format(node.name, node.name))

    info('Installing keys and certificates on nodes\n')
    for node in ndn.net.stations:
        # node.cmd('export NDN_LOG="ndn_service_framework.*=TRACE:muas.*=TRACE:nacabe.*=TRACE:ndnsvs.svspubsub=TRACE:ndnsd.*=TRACE"')
        node.cmd('export NDN_LOG="muas.main_gs=TRACE"')
        

        node.cmd('ndnsec import -P 123456 /tmp/muas.ndnkey')
        node.cmd('ndnsec cert-install -f /tmp/muas.cert')

        node.cmd('ndnsec import -P 123456 /tmp/aa.ndnkey')
        node.cmd('ndnsec cert-install -f /tmp/aa.cert')
        for node2 in ndn.net.stations:
            # node.cmd('ndnsec cert-install -f /tmp/{}.cert'.format(node2.name))
            node.cmd('ndnsec import -P 123456 /tmp/{}.ndnkey'.format(node2.name))
            node.cmd('ndnsec cert-install -f /tmp/{}.cert'.format(node2.name))

def topology():
    "Create a network."
    ndnwifi = MinindnAdhoc(topoFile="./Topology/UAV_adhoc(lossy).conf",link=wmediumd, wmediumd_mode=interference)
    ndnwifi.net.setPropagationModel(model="logDistance", exp=3)

    ndnwifi.start()
    info("Starting NFD")
    AppManager(ndnwifi, ndnwifi.net.stations, Nfd, logLevel='DEBUG')
    # info('Starting NLSR on nodes\n')
    # nlsrs = AppManager(ndnwifi, ndnwifi.net.stations, Nlsr)

     # create /muas on gs1 and dump the cert
    gs1 = ndnwifi.net['gs1']
    # gs2 = ndnwifi.net['gs2']
    # gs3 = ndnwifi.net['gs3']
    # gs4 = ndnwifi.net['gs4']
    # gs5 = ndnwifi.net['gs5']
    drone1 = ndnwifi.net['drone1']
    drone2 = ndnwifi.net['drone2']
    # drone3 = ndnwifi.net['drone3']
    # drone4 = ndnwifi.net['drone4']
    # drone5 = ndnwifi.net['drone5']

    generateAndSignCertificates(ndnwifi)

    info("Setting multicast face and strategy on nodes")
    for node in ndnwifi.net.stations:
        node.cmd('nfdc strategy set /muas /localhost/nfd/strategy/multicast')
        # nfdc route add prefix /ndn nexthop 256 cost 100
        # face 256: ether://[01:00:5e:00:17:aa]
        node.cmd('nfdc route add prefix /muas nexthop 256 cost 100')
        node.cmd('nfdc strategy set /muas/{} /localhost/nfd/strategy/multicast'.format(node.name))
        node.cmd('nfdc strategy set /muas/{}/NDNSF /localhost/nfd/strategy/multicast'.format(node.name))
        node.cmd("nfdc route add prefix /muas/NDNSD nexthop 256 cost 100") # for ndnsd
        node.cmd("nfdc route add prefix /muas/{}/ping nexthop 256 cost 100")
        node.cmd('nfdc strategy set /muas/NDNSD /localhost/nfd/strategy/multicast')
        # nfdc route add prefix /ndn nexthop 256 cost 100
        # face 256: ether://[01:00:5e:00:17:aa]
        time.sleep(1)


    # do some experiment here
    NDNPing.startPingServer(drone1, "/muas/drone1/ping")
    gs1.cmd('xterm -T "service-controller" -e "service-controller-example" &')
    time.sleep(2)
    drone1.cmd('xterm -T "drone1" -e "multi-drone-example /muas/drone1" &')
    time.sleep(2)
    drone2.cmd('xterm -T "drone2" -e "multi-drone-example /muas/drone2" &')
    # time.sleep(2)
    # drone3.cmd('xterm -T "drone3" -e "multi-drone-example /muas/drone3" &')
    time.sleep(5)
    # FirstResponding/LoadBalancing/NoCoordination
    # gs1.cmd('xterm -T "gs1" -e "multi-gs-example FirstResponding /muas/gs1 1000 60 /muas/drone1 /muas/drone2 /muas/drone3" &')
    gs1.cmd('xterm -T "gs1" -e "multi-gs-example NoCoordination /muas/gs1 1000 60 /muas/drone1" &')
    
    # gs1.cmd('wireshark -X lua_script:/usr/local/share/ndn-dissect-wireshark/ndn.lua')
    # gs2.cmd('xterm -T "gs2" -e "gs-example /muas/gs2 1000 10" &')
    # gs3.cmd('xterm -T "gs3" -e "gs-example /muas/gs3 1000 10" &')
    # gs4.cmd('xterm -T "gs4" -e "gs-example /muas/gs4 1000 10" &')
    # gs5.cmd('xterm -T "gs5" -e "gs-example /muas/gs5 1000 10" &')
    

    
    # Start the CLI
    MiniNDNWifiCLI(ndnwifi.net)
    ndnwifi.net.stop()
    ndnwifi.cleanUp()

if __name__ == '__main__':
    setLogLevel('info')
    topology()
