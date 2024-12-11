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
from time import sleep

def topology():
    "Create a network."
    ndnwifi = MinindnAdhoc(topoFile="./Topology/UAV_adhoc(loss=0%).conf")

    ndnwifi.start()
    info("Starting NFD")
    AppManager(ndnwifi, ndnwifi.net.stations, Nfd)
    # info('Starting NLSR on nodes\n')
    # nlsrs = AppManager(ndnwifi, ndnwifi.net.stations, Nlsr)

     # create /muas on gs1 and dump the cert
    gs1 = ndnwifi.net['gs1']
    drone1 = ndnwifi.net['drone1']
    gs1.cmd('ndnsec key-gen -t r /muas > /dev/null')

    for node in ndnwifi.net.stations:
        print(node.name)
        node.cmd('ndnsec key-gen -t r /muas/{} > /dev/null'.format(node.name))
        node.cmd('nfdc strategy set /muas /localhost/nfd/strategy/multicast')
        # nfdc route add prefix /ndn nexthop 256 cost 100
        # face 256: ether://[01:00:5e:00:17:aa]
        node.cmd('nfdc route add prefix /muas nexthop 256 cost 100')

    gs1.cmd('xterm -T "gs1" &')
    # /home/tianxing/NDN/ndn-service-framework/Experiments/NDN_NSC/consumer /muas/gs1 /muas/drone1 /FlightControl /ManualControl 100 20
    drone1.cmd('xterm -T "drone1" &')
    # /home/tianxing/NDN/ndn-service-framework/Experiments/NDN_NSC/producer /muas/drone1 /FlightControl /ManualControl
    
    # Start the CLI
    MiniNDNWifiCLI(ndnwifi.net)
    ndnwifi.net.stop()
    ndnwifi.cleanUp()

if __name__ == '__main__':
    setLogLevel('info')
    topology()
