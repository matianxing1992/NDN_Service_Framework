from subprocess import PIPE

from mininet.log import setLogLevel, info

from minindn.minindn import Minindn
from minindn.util import MiniNDNCLI, getPopen
from minindn.apps.app_manager import AppManager
from minindn.apps.nfd import Nfd
from minindn.apps.nlsr import Nlsr

import time

def printOutput(output):
    _out = output.decode("utf-8").split("\n")
    for _line in _out:
        info(_line + "\n")

def generateAndSignCertificates(ndn: Minindn):
    # delete identities
    for node in ndn.net.hosts:
        node.cmd('ndnsec delete /muas')
        node.cmd('ndnsec delete /muas/aa')
        for node2 in ndn.net.hosts:
            node.cmd('ndnsec delete /muas/{}'.format(node2.name))

    # generate trust anchor on the first node
    controller_node = ndn.net.hosts[0]
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
    for node in ndn.net.hosts:
        controller_node.cmd('ndnsec key-gen -t r /muas/{} > /tmp/{}.key'.format(node.name, node.name))
    
    info('Installing keys on nodes\n')
    # install keys on controller_node
    for node in ndn.net.hosts:
        controller_node.cmd('ndnsec cert-install -f /tmp/{}.key'.format(node.name))

    info('Signing certificates on controller_node\n')
    # sign certificates on controller_node using /muas
    for node in ndn.net.hosts:
        controller_node.cmd('ndnsec cert-gen -s /muas -i default /tmp/{}.key > /tmp/{}.cert'.format(node.name, node.name))
        controller_node.cmd('ndnsec-export -P 123456 -o /tmp/{}.ndnkey -i /muas/{}'.format(node.name, node.name))

    info('Installing keys and certificates on nodes\n')
    for node in ndn.net.hosts:
        node.cmd('export NDN_LOG="ndn_service_framework.*=TRACE:muas.*=TRACE:nacabe.*=TRACE:ndnsvs.svspubsub=TRACE:ndnsd.*=TRACE"')
        
        node.cmd('ndnsec import -P 123456 /tmp/muas.ndnkey')
        node.cmd('ndnsec cert-install -f /tmp/muas.cert')

        node.cmd('ndnsec import -P 123456 /tmp/aa.ndnkey')
        node.cmd('ndnsec cert-install -f /tmp/aa.cert')
        for node2 in ndn.net.hosts:
            # node.cmd('ndnsec cert-install -f /tmp/{}.cert'.format(node2.name))
            node.cmd('ndnsec import -P 123456 /tmp/{}.ndnkey'.format(node2.name))
            node.cmd('ndnsec cert-install -f /tmp/{}.cert'.format(node2.name))


if __name__ == '__main__':
    setLogLevel('info')

    Minindn.cleanUp()
    Minindn.verifyDependencies()

    ndn = Minindn(topoFile="./Topology/UAV(loss=0%)")

    ndn.start()

    info('Starting NFD on nodes\n')
    nfds = AppManager(ndn, ndn.net.hosts, Nfd, logLevel='DEBUG')
    
    info('Starting NLSR on nodes\n')
    nlsrs = AppManager(ndn, ndn.net.hosts, Nlsr)

    generateAndSignCertificates(ndn)
    
    # nlsr
    for node in ndn.net.hosts:
        node.cmd("nlsrc advertise /muas")
        # node.cmd("nlsrc advertise /ndn/broadcast")
        node.cmd('nfdc strategy set /muas /localhost/nfd/strategy/multicast')
        node.cmd("nlsrc advertise /muas/{}".format(node.name))
        node.cmd('nfdc strategy set /muas/{} /localhost/nfd/strategy/multicast'.format(node.name))
        node.cmd('nfdc strategy set /muas/{}/NDNSF /localhost/nfd/strategy/multicast'.format(node.name))
        node.cmd("nlsrc advertise /muas/NDNSD") # for ndnsd
        node.cmd('nfdc strategy set /muas/NDNSD /localhost/nfd/strategy/multicast')

    # sleep for 10 seconds to allow NLSR to propagate the prefixes
    print("Waiting for NLSR to propagate prefixes...")
    time.sleep(15)

    # do some experiment here

    gs1 = ndn.net["gs1"]
    drone1 = ndn.net["drone1"]

    gs1.cmd('xterm -T "service-controller" -e "service-controller-example" &')
    time.sleep(2)
    drone1.cmd('xterm -T "drone1" -e "drone-example /muas/drone1" &')
    time.sleep(2)
    gs1.cmd('xterm -T "gs1" -e "gs-example /muas/gs1 100 100" &')
    time.sleep(2)


    # service-controller-example
    # drone-example /muas/drone1
    # gs-example /muas/gs1 100 100
    



    # run aa on gs1
    # gs1.cmd('export NDN_LOG="*=TRACE"')
    # gs1.cmd('export NDN_LOG="muas.main_gs=TRACE"')
    # gs1.cmd('/home/tianxing/NDN/ndn-service-framework/build/examples/aa-example &')
    
    # for aa-example
    # gs1.cmd('/home/tianxing/NDN/ndn-service-framework/build/examples/aa-example &')
    # gs1.cmd('xterm -T "gs1" -e "/home/tianxing/NDN/ndn-service-framework/build/examples/aa-example" &') # for aa-example
    # drone1.cmd('export NDN_LOG="muas.main_gs=TRACE"')

    # time.sleep(3)

    # drone1.cmd('xterm -T "drone1" &') # for drone-example
    
    # time.sleep(3)

    # gs1.cmd('xterm -T "gs1" &') # for gs-example
    
    # /home/tianxing/NDN/ndn-service-framework/build/examples/aa-example
    # /home/tianxing/NDN/ndn-service-framework/build/examples/gs-example 100 100
    # /home/tianxing/NDN/ndn-service-framework/build/examples/drone-example
    # export NDN_LOG="ndn_service_framework.*=TRACE:muas.*=TRACE:nacabe.*=TRACE"
    
    # nsc = getPopen(ndn.net["gs1"], "/home/tianxing/NDN/ndn-service-framework/Experiments/NDN_NSC/consumer /muas/gs1 /muas/drone1 /FlightControl /ManualControl 1000 10", stdout=PIPE, stderr=PIPE)
    # nsc.wait()
    # printOutput(nsc.stdout.read())
    
    MiniNDNCLI(ndn.net)

    ndn.stop()