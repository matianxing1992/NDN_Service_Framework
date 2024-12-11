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

if __name__ == '__main__':
    setLogLevel('info')

    Minindn.cleanUp()
    Minindn.verifyDependencies()

    ndn = Minindn(topoFile="./Topology/UAV(loss=0%)")

    ndn.start()

    info('Starting NFD on nodes\n')
    nfds = AppManager(ndn, ndn.net.hosts, Nfd)
    info('Starting NLSR on nodes\n')
    nlsrs = AppManager(ndn, ndn.net.hosts, Nlsr)

    # create /muas on gs1 and dump the cert
    gs1 = ndn.net['gs1']
    drone1 = ndn.net['drone1']
    gs1.cmd('ndnsec key-gen -t r /muas > /dev/null')

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

    # nsc = getPopen(ndn.net["gs1"], "/home/tianxing/NDN/ndn-service-framework/Experiments/NDN_NSC/consumer /muas/gs1 /muas/drone1 /FlightControl /ManualControl 1000 10", stdout=PIPE, stderr=PIPE)
    # nsc.wait()
    # printOutput(nsc.stdout.read())

    gs1.cmd('xterm -T "gs1" &')
    # /home/tianxing/NDN/ndn-service-framework/Experiments/NDN_NSC/consumer /muas/gs1 /muas/drone1 /FlightControl /ManualControl 100 100
    drone1.cmd('xterm -T "drone1" &')
    # /home/tianxing/NDN/ndn-service-framework/Experiments/NDN_NSC/producer /muas/drone1 /FlightControl /ManualControl
    
    MiniNDNCLI(ndn.net)

    ndn.stop()