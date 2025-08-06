from subprocess import PIPE

from mininet.log import setLogLevel, info

from minindn.minindn import Minindn
from minindn.util import MiniNDNCLI, getPopen
from minindn.apps.app_manager import AppManager
from minindn.apps.nfd import Nfd
from minindn.apps.nlsr import Nlsr

from minindn.helpers.ndnping import NDNPing

import time


def registerRouteToAllNeighbors(ndn, host, syncPrefix):
    for node in ndn.net.hosts:
        for neighbor in node.connectionsTo(host):
            ip = node.IP(neighbor[0])
            faceID = Nfdc.createFace(host, ip)
            Nfdc.registerRoute(host, syncPrefix, faceID)

def printOutput(output):
    _out = output.decode("utf-8").split("\n")
    for _line in _out:
        info(_line + "\n")

if __name__ == '__main__':
    setLogLevel('info')

    Minindn.cleanUp()
    Minindn.verifyDependencies()

    ndn = Minindn(topoFile="./Topology/testbed(loss=0%).conf")

    ndn.start()

    info('Starting NFD on nodes\n')
    nfds = AppManager(ndn, ndn.net.hosts, Nfd)
    info('Starting NLSR on nodes\n')
    nlsrs = AppManager(ndn, ndn.net.hosts, Nlsr)

    # create /muas on memphis and dump the cert
    memphis = ndn.net['memphis']
    ucla = ndn.net['ucla']
    memphis.cmd('ndnsec key-gen -t r /muas > /dev/null')

    for node in ndn.net.hosts:
        print(node.name)
        node.cmd('ndnsec key-gen -t r /muas/{} > /dev/null'.format(node.name))
        # node.cmd("nlsrc advertise /muas")
        node.cmd('nfdc strategy set /muas /localhost/nfd/strategy/multicast')
        node.cmd("nlsrc advertise /muas/{}".format(node.name))
        time.sleep(5)

    # sleep for 60 seconds to allow NLSR to propagate the prefixes
    print("Waiting for NLSR to propagate prefixes...")
    time.sleep(20)

    # do some experiment here
    # ndn.net['drone1'].cmd('/home/tianxing/NDN/ndn-service-framework/Experiments/NDN_NSC/producer /muas/drone1 /FlightControl /ManualControl &')

    # nsc = getPopen(ndn.net["memphis"], "/home/tianxing/NDN/ndn-service-framework/Experiments/NDN_NSC/consumer /muas/memphis /muas/drone1 /FlightControl /ManualControl 1000 10", stdout=PIPE, stderr=PIPE)
    # nsc.wait()
    # printOutput(nsc.stdout.read())

    getPopen(ndn.net["ucla"], "ndnpingserver /muas/ucla")

    ucla.cmd('xterm -T "ucla" -e "/home/tianxing/NDN/ndn-service-framework/Experiments/NDN_NSC/producer /muas/ucla /FlightControl /ManualControl" &')
    # /home/tianxing/NDN/ndn-service-framework/Experiments/NDN_NSC/producer /muas/ucla /FlightControl /ManualControl
    time.sleep(5)
    memphis.cmd('xterm -T "memphis" -e "/home/tianxing/NDN/ndn-service-framework/Experiments/NDN_NSC/consumer /muas/memphis /muas/ucla /FlightControl /ManualControl 100 1000" &')
    # /home/tianxing/NDN/ndn-service-framework/Experiments/NDN_NSC/consumer /muas/memphis /muas/ucla /FlightControl /ManualControl 100 1000


    MiniNDNCLI(ndn.net)

    ndn.stop()