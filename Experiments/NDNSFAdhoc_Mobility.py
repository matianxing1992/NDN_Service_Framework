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
    ndnwifi = MinindnAdhoc(topoFile="./Topology/UAV_adhoc.conf",link=wmediumd, wmediumd_mode=interference)
    ndnwifi.net.setPropagationModel(model="logDistance", exp=3)

    ndnwifi.start()
    info("Starting NFD")
    AppManager(ndnwifi, ndnwifi.net.stations, Nfd, logLevel='INFO')
    # info('Starting NLSR on nodes\n')
    # nlsrs = AppManager(ndnwifi, ndnwifi.net.stations, Nlsr)

     # create /muas on uiuc and dump the cert
    uiuc = ndnwifi.net['uiuc']
    ucla = ndnwifi.net['ucla']
    neu = ndnwifi.net['neu']
    csu = ndnwifi.net['csu']



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
        # node.cmd("nfdc route add prefix /muas/{}/ping nexthop 256 cost 100".format(node.name))
        node.cmd('nfdc strategy set /muas/NDNSD /localhost/nfd/strategy/multicast')
        # nfdc route add prefix /ndn nexthop 256 cost 100
        # face 256: ether://[01:00:5e:00:17:aa]

        NDNPing.startPingServer(node, f"/muas/{node.name}")
        for node2 in ndnwifi.net.stations:
            if node.name != node2.name:
                node.cmd(f"nfdc route add prefix /muas/{node2.name} nexthop 256 cost 100")
                node.cmd(f'nfdc strategy set /muas/{node2.name} /localhost/nfd/strategy/multicast')

        # nfdc face create remote ether://[01:00:5e:00:17:aa] local 
        time.sleep(1)

    uiuc.cmd('xterm -T "service-controller" -e "service-controller-example" &')
    time.sleep(2)
    ucla.cmd('xterm -T "ucla" -e "multi-drone-example /muas/ucla" &')
    time.sleep(2)
    neu.cmd('xterm -T "neu" -e "multi-drone-example /muas/neu" &')
    time.sleep(2)
    csu.cmd('xterm -T "csu" -e "multi-drone-example /muas/csu" &')
    # time.sleep(2)
    # csu.cmd('xterm -T "csu" -e "multi-drone-example /muas/csu" &')
    time.sleep(5)
    # FirstResponding/LoadBalancing/NoCoordination
    
    ## uiuc.cmd('xterm -T "uiuc" -e "multi-gs-example NoCoordination /muas/uiuc 1 180 /muas/ucla" &')
    
    uiuc.cmd('xterm -T "uiuc" -e "multi-gs-example FirstResponding /muas/uiuc 1 600 /muas/ucla /muas/csu /muas/neu" &')
    
    # uiuc.cmd('wireshark -X lua_script:/usr/local/share/ndn-dissect-wireshark/ndn.lua')
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
