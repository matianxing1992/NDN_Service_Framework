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

    ndn = Minindn(topoFile="./Topology/UAV(loss=5%)")

    ndn.start()

    info('Starting NFD on nodes\n')
    nfds = AppManager(ndn, ndn.net.hosts, Nfd)
    info('Starting NLSR on nodes\n')
    nlsrs = AppManager(ndn, ndn.net.hosts, Nlsr)

    for node in ndn.net.hosts:
        print(node.name)
        node.cmd('ndnsec key-gen -t r /muas/{} > /dev/null'.format(node.name))
        node.cmd("nlsrc advertise /muas")
        # node.cmd("nlsrc advertise /ndn/broadcast")
        node.cmd('nfdc strategy set /muas /localhost/nfd/strategy/multicast')
        node.cmd("nlsrc advertise /muas/{}".format(node.name))
        node.cmd('nfdc strategy set /muas/{} /localhost/nfd/strategy/multicast'.format(node.name))
        node.cmd('nfdc strategy set /muas/{}/NDNSF /localhost/nfd/strategy/multicast'.format(node.name))
        node.cmd("nlsrc advertise /discovery") # for ndnsd
        node.cmd('nfdc strategy set /discovery /localhost/nfd/strategy/multicast')
        time.sleep(5)

    gs1 = ndn.net['gs1']
    drone1 = ndn.net['drone1']
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


    # sleep for 10 seconds to allow NLSR to propagate the prefixes
    print("Waiting for NLSR to propagate prefixes...")
    time.sleep(10)

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
    
    MiniNDNCLI(ndn.net)

    ndn.stop()