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
import time

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

    for node in ndnwifi.net.stations:
        print(node.name)
        node.cmd('ndnsec key-gen -t r /muas/{} > /dev/null'.format(node.name))
        # node.cmd("nlsrc advertise /ndn/broadcast")
        node.cmd('nfdc strategy set /muas /localhost/nfd/strategy/multicast')
        # nfdc route add prefix /ndn nexthop 256 cost 100
        # face 256: ether://[01:00:5e:00:17:aa]
        node.cmd('nfdc route add prefix /muas nexthop 256 cost 100')
        node.cmd('nfdc strategy set /muas/{} /localhost/nfd/strategy/multicast'.format(node.name))
        node.cmd('nfdc strategy set /muas/{}/NDNSF /localhost/nfd/strategy/multicast'.format(node.name))
        node.cmd('nfdc strategy set /discovery /localhost/nfd/strategy/multicast')
        # nfdc route add prefix /ndn nexthop 256 cost 100
        # face 256: ether://[01:00:5e:00:17:aa]
        node.cmd('nfdc route add prefix /discovery nexthop 256 cost 100')
        time.sleep(5)

    # clear identities
    # ndnsec delete /muas
    # ndnsec delete /muas/aa
    # ndnsec delete /muas/drone1
    # ndnsec delete /muas/gsA
    gs1.cmd('ndnsec delete /muas')
    gs1.cmd('ndnsec delete /muas/aa')
    gs1.cmd('ndnsec delete /muas/drone1')
    gs1.cmd('ndnsec delete /muas/gs1')
    drone1.cmd('ndnsec delete /muas')
    drone1.cmd('ndnsec delete /muas/aa')
    drone1.cmd('ndnsec delete /muas/drone1')
    drone1.cmd('ndnsec delete /muas/gs1')
    # create identity for /muas on gs1
    gs1.cmd('ndnsec key-gen -t r /muas > /dev/null')
    # create identity for /muas/aa on gs1, and dump the cert to aa.cert
    gs1.cmd('ndnsec key-gen -t r /muas/aa > /dev/null')
    gs1.cmd('ndnsec cert-dump -i /muas/aa > /home/tianxing/NDN/ndn-service-framework/Experiments/aa.cert')
    # create identity for /muas/gs1 on gs1, and dump the cert to aa.cert
    gs1.cmd('ndnsec key-gen -t r /muas/gs1 > /dev/null')
    gs1.cmd('ndnsec cert-dump -i /muas/gs1 > /home/tianxing/NDN/ndn-service-framework/Experiments/gs1.cert')
    drone1.cmd('ndnsec key-gen -t r /muas/drone1 > /dev/null')
    drone1.cmd('ndnsec cert-dump -i /muas/drone1 > /home/tianxing/NDN/ndn-service-framework/Experiments/drone1.cert')
    # expert ndnkey
    gs1.cmd('ndnsec-export -P 123456 -o /home/tianxing/NDN/ndn-service-framework/Experiments/aa.ndnkey -i /muas/aa')
    gs1.cmd('ndnsec-export -P 123456 -o /home/tianxing/NDN/ndn-service-framework/Experiments/gs1.ndnkey -i /muas/gs1')
    drone1.cmd('ndnsec-export -P 123456 -o /home/tianxing/NDN/ndn-service-framework/Experiments/drone1.ndnkey -i /muas/drone1')
    # import ndnkey
    gs1.cmd('ndnsec import -P 123456 /home/tianxing/NDN/ndn-service-framework/Experiments/drone1.ndnkey')
    drone1.cmd('ndnsec import -P 123456 /home/tianxing/NDN/ndn-service-framework/Experiments/gs1.ndnkey')
    drone1.cmd('ndnsec import -P 123456 /home/tianxing/NDN/ndn-service-framework/Experiments/aa.ndnkey')
    # copy files to gs1 and drone1
    gs1.cmd('cp /home/tianxing/NDN/ndn-service-framework/examples/trust-schema.conf .')
    gs1.cmd('cp /home/tianxing/NDN/ndn-service-framework/examples/FlightControl.info .')
    gs1.cmd('cp /home/tianxing/NDN/ndn-service-framework/examples/ObjectDetection.info .')
    drone1.cmd('cp /home/tianxing/NDN/ndn-service-framework/examples/trust-schema.conf .')
    drone1.cmd('cp /home/tianxing/NDN/ndn-service-framework/examples/FlightControl.info .')
    drone1.cmd('cp /home/tianxing/NDN/ndn-service-framework/examples/ObjectDetection.info .')


    # do some experiment here

    # run aa on gs1
    # gs1.cmd('export NDN_LOG="*=TRACE"')
    # gs1.cmd('export NDN_LOG="muas.main_gs=TRACE"')
    gs1.cmd('export NDN_LOG="ndn_service_framework.*=TRACE:muas.*=TRACE:nacabe.*=TRACE:ndnsvs.svspubsub=TRACE"')
    # gs1.cmd('/home/tianxing/NDN/ndn-service-framework/build/examples/aa-example &')
    gs1.cmd('xterm -T "gs1" &') # for aa-example

    gs1.cmd('xterm -T "gs1" &')
    # drone1.cmd('export NDN_LOG="muas.main_gs=TRACE"')
    drone1.cmd('export NDN_LOG="ndn_service_framework.*=TRACE:muas.*=TRACE:nacabe.*=TRACE:ndnsvs.svspubsub=TRACE"')
    drone1.cmd('xterm -T "drone1" &')

    # /home/tianxing/NDN/ndn-service-framework/build/examples/aa-example
    # /home/tianxing/NDN/ndn-service-framework/build/examples/gs-example 100 100
    # /home/tianxing/NDN/ndn-service-framework/build/examples/drone-example
    # export NDN_LOG="ndn_service_framework.*=TRACE:muas.*=TRACE:nacabe.*=TRACE"
    
    # nsc = getPopen(ndn.net["gs1"], "/home/tianxing/NDN/ndn-service-framework/Experiments/NDN_NSC/consumer /muas/gs1 /muas/drone1 /FlightControl /ManualControl 1000 10", stdout=PIPE, stderr=PIPE)
    # nsc.wait()
    # printOutput(nsc.stdout.read())
    
    # Start the CLI
    MiniNDNWifiCLI(ndnwifi.net)
    ndnwifi.net.stop()
    ndnwifi.cleanUp()

if __name__ == '__main__':
    setLogLevel('info')
    topology()
