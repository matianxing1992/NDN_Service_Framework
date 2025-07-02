from subprocess import PIPE

from mininet.log import setLogLevel, info
from mininet.topo import Topo

from minindn.minindn import Minindn
from minindn.apps.app_manager import AppManager
from minindn.util import MiniNDNCLI, getPopen
from minindn.apps.nfd import Nfd
from minindn.helpers.nfdc import Nfdc

from mininet.node import Controller

PREFIX = "/example"

def printOutput(output):
    _out = output.decode("utf-8").split("\n")
    for _line in _out:
        info(_line + "\n")

def configure_static_routes(ndn):
    hosts = ["gs1", "drone1", "drone2", "drone3", "drone4", "drone5"]
    
    # forward route: gs1 -> drone1 -> ... -> drone5
    for i in range(len(hosts) - 1):
        src = ndn.net[hosts[i]]
        next_hop = ndn.net[hosts[i + 1]]


        iface = next_hop.connectionsTo(src)[0][0]
        next_hop_ip = iface.IP()

        dst = ndn.net["drone5"]
        dst_ip = dst.IP()

        src.cmd(f"ip route add {dst_ip} via {next_hop_ip}")

    # reverse route: drone5 -> ... -> drone1 -> gs1
    for i in range(len(hosts) - 1, 0, -1):
        src = ndn.net[hosts[i]]
        next_hop = ndn.net[hosts[i - 1]]

        iface = next_hop.connectionsTo(src)[0][0]
        next_hop_ip = iface.IP()

        dst = ndn.net["gs1"]
        dst_ip = dst.IP()

        src.cmd(f"ip route add {dst_ip} via {next_hop_ip}")

    info("Static IP routes configured.\n")


def run():
    Minindn.cleanUp()
    Minindn.verifyDependencies()

    ndn = Minindn(topoFile="./Topology/UAV(loss=0%)", controller=Controller)
    ndn.start()
    
    configure_static_routes(ndn)

    # configure and start nfd on each node
    info("Configuring NFD\n")
    AppManager(ndn, ndn.net.hosts, Nfd, logLevel="DEBUG")

    """
    There are multiple ways of setting up routes in Mini-NDN
    refer: https://minindn.memphis.edu/experiment.html#routing-options
    It can also be set manually as follows. The important bit to note here
    is the use of the Nfdc command
    """
    
    gs1 = ndn.net['gs1']
    drone1 = ndn.net['drone1']
    drone2 = ndn.net['drone2']
    drone3 = ndn.net['drone3']
    drone4 = ndn.net['drone4']
    drone5 = ndn.net['drone5']
    
    links = {"gs1":["drone1"], "drone1":["drone2"], "drone2":["drone3"], "drone3":["drone4"], "drone4":["drone5"]}
    for first in links:
        for second in links[first]:
            host1 = ndn.net[first]
            host2 = ndn.net[second]
            interface = host2.connectionsTo(host1)[0][0]
            interface_ip = interface.IP()
            Nfdc.createFace(host1, interface_ip)
            Nfdc.registerRoute(host1, PREFIX, interface_ip, cost=0)

    # Start ping server
    info("Starting pings...\n")
    pingserver_log = open("{}/drone5/ndnpingserver.log".format(ndn.workDir), "w")
    getPopen(ndn.net["drone5"], "ndnpingserver {}".format(PREFIX), stdout=pingserver_log,\
             stderr=pingserver_log)
             
    drone5_ip = ndn.net["drone5"].IP()
    gs1.cmd("xterm -hold -T 'IP ping: gs1 ping drone5' -e 'ping {}' &".format(drone5_ip))

    gs1.cmd("xterm -hold -T 'ndnping: gs1 ping /example' -e 'ndnping /example' &")


    info("\nExperiment Completed!\n")
    MiniNDNCLI(ndn.net)
    ndn.stop()

if __name__ == '__main__':
    setLogLevel("info")
    run()
