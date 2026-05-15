from mininet.log import setLogLevel, info
from minindn.minindn import Minindn
from minindn.wifi.minindnwifi import MinindnWifi
from minindn.apps.app_manager import AppManager
from minindn.util import MiniNDNCLI, getPopen
from minindn.apps.nfd import Nfd
from minindn.helpers.nfdc import Nfdc

import time, os

PREFIX = "/example"

def enable_udp4(node):
    node.cmd("nfdc face_system enable udp4 >/tmp/nfdc-face-system.log 2>&1 || true")

def run():
    Minindn.cleanUp()
    Minindn.verifyDependencies()

    ndn = MinindnWifi(topoFile='Topology/1gs3drones_wifi.conf')
    ndn.start()

    os.makedirs(f"{ndn.workDir}/b", exist_ok=True)

    info("Configuring NFD\n")
    AppManager(ndn, ndn.net.stations, Nfd, logLevel="DEBUG")

    # 给 NFD 启动一点时间（你已有 sleep 30）
    time.sleep(5)

    # 关键：WiFi 下需要让 NFD 接受 UDP4 传输
    for h in ndn.net.stations:
        print(1)
        enable_udp4(h)

    time.sleep(1)

    links = {"gs": ["drone1", "drone2", "drone3", "drone4"]}
    fixed_ip = {
        "gs":     "10.0.0.254",
        "drone1": "10.0.0.1",
        "drone2": "10.0.0.2",
        "drone3": "10.0.0.3",
        "drone4": "10.0.0.4",
    }

    host1 = ndn.net["gs"]

    for second in links["gs"]:
        host2 = ndn.net[second]
        drone_ip = fixed_ip[second]

        prefix = "/" + second   # 👈 每个节点自己的前缀

        Nfdc.createFace(host1, drone_ip)
        Nfdc.registerRoute(host1, prefix, drone_ip, cost=0)

        info(f"Starting pingserver on {second} ({drone_ip})...\n")

        log_path = f"{ndn.workDir}/b/ndnpingserver-{second}.log"
        pingserver_log = open(log_path, "w")

        # 👇 pingserver 也要改成对应前缀
        getPopen(host2, f"ndnpingserver {prefix}",
                stdout=pingserver_log, stderr=pingserver_log)

    info("\nExperiment Completed!\n")
    MiniNDNCLI(ndn.net)
    ndn.stop()

if __name__ == '__main__':
    setLogLevel("info")
    run()