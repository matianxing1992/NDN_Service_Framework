#!/usr/bin/env python

"""
This example shows how to enable the adhoc mode
Alternatively, you can use the MANET routing protocol of your choice
"""

import sys
from mininet.log import setLogLevel, info
from mn_wifi.cli import CLI
from mn_wifi.net import Mininet_wifi
from mn_wifi.link import wmediumd, adhoc
from mn_wifi.wmediumdConnector import interference

# interface = f"{sta.name}-wlan0"
def configure_tc(interface, delay, loss):
    """Configure tc on a given interface."""
    # Create an IFB device
    ifb = f"ifb{interface[-1]}"  # Assuming interface name ends with a number
    cmd = f"sudo ip link add {ifb} type ifb"
    cmd += f" && sudo ip link set {ifb} up"

    # Redirect ingress traffic to IFB device
    cmd += f" && sudo tc qdisc add dev {interface} handle ffff: ingress"
    cmd += f" && sudo tc filter add dev {interface} parent ffff: protocol ip u32 match u32 0 0 action mirred egress redirect dev {ifb}"

    # Add delay and loss to the IFB device (ingress traffic)
    cmd += f" && sudo tc qdisc add dev {ifb} root netem delay {delay} loss {loss}%"

    # Add delay and loss to the original interface (egress traffic)
    cmd += f" && sudo tc qdisc add dev {interface} root netem delay {delay} loss {loss}%"

    return cmd

def topology(args):
    "Create a network."
    net = Mininet_wifi(link=wmediumd, wmediumd_mode=interference)

    info("*** Creating nodes\n")
    kwargs = {}
    if '-a' in args:
        kwargs['range'] = 100

    gs1 = net.addStation('gs1', ip6='fe80::1',
                         position='0,0,0', **kwargs)
    drone1 = net.addStation('drone1', ip6='fe80::2',
                            position='-60,0,0', **kwargs)
    drone2 = net.addStation('drone2', ip6='fe80::3',
                            position='-20,30,0', **kwargs)
    drone3 = net.addStation('drone3', ip6='fe80::4',
                            position='20,30,0', **kwargs)
    drone4 = net.addStation('drone4', ip6='fe80::5',
                            position='60,0,0', **kwargs)

    net.setPropagationModel(model="logDistance", exp=4)

    info("*** Configuring nodes\n")
    net.configureNodes()

    info("*** Creating links\n")
    delay = '0ms'
    loss = 0

    net.addLink(gs1, cls=adhoc, intf='gs1-wlan0',
                ssid='adhocNet', mode='g', channel=5)
    net.addLink(drone1, cls=adhoc, intf='drone1-wlan0',
                ssid='adhocNet', mode='g', channel=5)
    net.addLink(drone2, cls=adhoc, intf='drone2-wlan0',
                ssid='adhocNet', mode='g', channel=5)
    net.addLink(drone3, cls=adhoc, intf='drone3-wlan0',
                ssid='adhocNet', mode='g', channel=5)
    net.addLink(drone4, cls=adhoc, intf='drone4-wlan0',
                ssid='adhocNet', mode='g', channel=5)

    info("*** Starting network\n")
    net.build()

    info("\n*** Addressing...\n")
    if 'proto' not in kwargs:
        gs1.setIP6('2001::1/64', intf="gs1-wlan0")
        drone1.setIP6('2001::2/64', intf="drone1-wlan0")
        drone2.setIP6('2001::3/64', intf="drone2-wlan0")
        drone3.setIP6('2001::4/64', intf="drone3-wlan0")
        drone4.setIP6('2001::5/64', intf="drone4-wlan0")

    info("*** Configuring traffic control\n")
    for sta in [gs1, drone1, drone2, drone3, drone4]:
        intf = f"{sta.name}-wlan0"
        cmd = configure_tc(intf, delay, loss)
        sta.cmd(cmd)

    info("*** Running CLI\n")
    # gs1 xterm -T "gs1" &
    # drone1 xterm -T "drone1" &
    # source /home/tianxing/NDN/ndn-service-framework/Experiments/pythonEnv/bin/activate
    # python /home/tianxing/NDN/ndn-service-framework/Experiments/gRPC/greeter_server.py
    # python /home/tianxing/NDN/ndn-service-framework/Experiments/gRPC/greeter_client.py
    CLI(net)

    info("*** Stopping network\n")
    net.stop()

if __name__ == '__main__':
    setLogLevel('info')
    topology(sys.argv)
