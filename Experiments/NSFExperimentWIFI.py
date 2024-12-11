#!/usr/bin/env python

import sys
from mininet.log import setLogLevel, info
from mn_wifi.cli import CLI
from mn_wifi.net import Mininet_wifi

def topology():
    "Create a network."
    net = Mininet_wifi()

    info("*** Creating nodes\n")
    ap1 = net.addAccessPoint('ap1', ssid='simpletopo', mode='g', channel='1')
    gs1 = net.addStation('gs1', ip='10.0.0.1/24')
    drone1 = net.addStation('drone1', ip='10.0.0.2/24')
    drone2 = net.addStation('drone2', ip='10.0.0.3/24')
    drone3 = net.addStation('drone3', ip='10.0.0.4/24')
    drone4 = net.addStation('drone4', ip='10.0.0.5/24')
    c0 = net.addController('c0')

    info("*** Configuring wifi nodes\n")
    net.configureNodes()

    info("*** Creating links\n")
    net.addLink(gs1, ap1)
    net.addLink(drone1, ap1)
    net.addLink(drone2, ap1)
    net.addLink(drone3, ap1)
    net.addLink(drone4, ap1)

    info("*** Starting network\n")
    net.build()
    c0.start()
    ap1.start([c0])

    info("*** Configuring delay and loss using tc\n")
    for sta in [gs1, drone1, drone2, drone3, drone4]:
        intf = sta.params['wlan'][0]
        info(f"*** Configuring {sta.name} interface {intf}\n")

        # Load ifb module
        sta.cmd('modprobe ifb numifbs=1')

        # Create ifb interface and redirect ingress traffic
        sta.cmd(f'ip link add ifb0 type ifb')
        sta.cmd(f'ip link set ifb0 up')
        sta.cmd(f'tc qdisc add dev {intf} ingress')
        sta.cmd(f'tc filter add dev {intf} parent ffff: protocol ip u32 match u32 0 0 action mirred egress redirect dev ifb0')

        # Configure delay and loss on ifb0 interface
        sta.cmd(f'tc qdisc add dev ifb0 root netem delay 10ms loss 5%')

    info("*** Running CLI\n")
    CLI(net)

    info("*** Stopping network\n")
    net.stop()

if __name__ == '__main__':
    setLogLevel('info')
    topology()
