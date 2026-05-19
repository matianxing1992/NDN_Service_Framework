import argparse
import atexit
import json
import os
import shlex
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

from mininet.log import setLogLevel, info

from minindn.minindn import Minindn
from minindn.wifi.minindnwifi import MinindnAdhoc
from minindn.util import MiniNDNCLI, getPopen
from minindn.apps.app_manager import AppManager
from minindn.apps.nfd import Nfd
from minindn.apps.nlsr import Nlsr
from minindn.helpers.ndnping import NDNPing
from minindn.helpers.nfdc import Nfdc

DEFAULT_MAX_RUNTIME_SECONDS = 180
PROCESS_SHUTDOWN_TIMEOUT_SECONDS = 5
LOG_TAIL_BYTES = 64 * 1024
REPO_ROOT = Path(__file__).resolve().parents[1]


class ExperimentTimeout(RuntimeError):
    pass


def shell_quote(value):
    return shlex.quote(str(value))


def build_parser():
    parser = argparse.ArgumentParser(
        description="MiniNDN NDNSF auto-config experiment")
    parser.add_argument("--max-runtime-seconds", type=int,
                        default=DEFAULT_MAX_RUNTIME_SECONDS,
                        help="Hard wall-clock timeout for setup, workload, cleanup, and summary")
    parser.add_argument("--output-dir", default="",
                        help="Directory for logs and summary files")
    parser.add_argument("--interactive-cli", action="store_true",
                        help="Open MiniNDNCLI after workload startup")
    return parser


def output_directory(args):
    if args.output_dir:
        return Path(args.output_dir)
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    return REPO_ROOT / "results" / "ndnsf_autoconfig_{}".format(timestamp)


def ensure_time_remaining(deadline, phase):
    if time.monotonic() >= deadline:
        raise ExperimentTimeout("max runtime reached during {}".format(phase))


def bounded_sleep(seconds, deadline, phase):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        ensure_time_remaining(deadline, phase)
        time.sleep(min(0.5, end - time.monotonic()))


def printOutput(output):
    _out = output.decode("utf-8").split("\n")
    for _line in _out:
        info(_line + "\n")


class ProcessTracker:
    def __init__(self):
        self.processes = []

    def getPopen(self, host, name, cmd, **params):
        process = getPopen(host, cmd, **params)
        self.processes.append({
            "name": name,
            "host": getattr(host, "name", str(host)),
            "cmd": cmd,
            "process": process,
            "log_file": params.get("stdout") if params.get("stdout") is params.get("stderr") else None,
        })
        return process

    def terminate_all(self, timeout=PROCESS_SHUTDOWN_TIMEOUT_SECONDS):
        for entry in reversed(self.processes):
            process = entry["process"]
            if process.poll() is None:
                info("Terminating {} on {}\n".format(entry["name"], entry["host"]))
                process.terminate()

        deadline = time.monotonic() + timeout
        for entry in reversed(self.processes):
            process = entry["process"]
            while process.poll() is None and time.monotonic() < deadline:
                try:
                    process.wait(timeout=0.2)
                except subprocess.TimeoutExpired:
                    pass
            if process.poll() is None:
                info("Killing {} on {}\n".format(entry["name"], entry["host"]))
                process.kill()
                try:
                    process.wait(timeout=1)
                except subprocess.TimeoutExpired:
                    pass
            log_file = entry.get("log_file")
            if log_file and not log_file.closed:
                log_file.close()

    def summary(self):
        items = []
        for entry in self.processes:
            process = entry["process"]
            items.append({
                "name": entry["name"],
                "host": entry["host"],
                "cmd": entry["cmd"],
                "pid": process.pid,
                "returncode": process.poll(),
            })
        return items


def wait_process(process, timeout, name):
    try:
        return process.wait(timeout=timeout)
    except subprocess.TimeoutExpired as exc:
        raise ExperimentTimeout("{} did not exit within {}s".format(name, timeout)) from exc


def collect_logs(output_dir, extra_log_paths):
    output_dir.mkdir(parents=True, exist_ok=True)
    collected = []
    for log_path in extra_log_paths:
        path = Path(log_path)
        if not path.exists():
            continue
        target = output_dir / path.name
        if path.resolve() != target.resolve():
            target.write_bytes(path.read_bytes())
        tail_path = output_dir / "{}.tail".format(path.name)
        data = target.read_bytes()[-LOG_TAIL_BYTES:] if target.exists() else path.read_bytes()[-LOG_TAIL_BYTES:]
        tail_path.write_bytes(data)
        collected.append(str(target))
    return collected


def write_summary(output_dir, summary):
    output_dir.mkdir(parents=True, exist_ok=True)
    json_path = output_dir / "summary.json"
    txt_path = output_dir / "summary.txt"
    json_path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
    lines = [
        "NDNSF AutoConfig MiniNDN Summary",
        "================================",
        "status: {}".format(summary["status"]),
        "timed out: {}".format(summary["timed_out"]),
        "runtime seconds: {:.3f}".format(summary["runtime_seconds"]),
        "max runtime seconds: {}".format(summary["max_runtime_seconds"]),
        "output dir: {}".format(summary["output_dir"]),
    ]
    if summary.get("exception"):
        lines.append("exception: {}".format(summary["exception"]))
    lines.append("tracked processes: {}".format(len(summary["processes"])))
    for process in summary["processes"]:
        lines.append("  {host}:{name} pid={pid} returncode={returncode}".format(**process))
    if summary.get("logs"):
        lines.append("logs:")
        lines.extend("  {}".format(path) for path in summary["logs"])
    txt_path.write_text("\n".join(lines) + "\n")
    return txt_path, json_path


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
        # node.cmd('export NDN_LOG="ndn_service_framework.*=TRACE:muas.*=TRACE:nacabe.*=TRACE:ndnsvs.svspubsub=TRACE:ndnsd.*=TRACE"')
        # node.cmd('export NDN_LOG="muas.main_gs=TRACE:ndnsvs.pubsub=TRACE:ndnsvs.core=TRACE"')
        #node.cmd('export NDN_LOG="muas.*=TRACE:ndnsvs.pubsub=TRACE:ndnsvs.core=TRACE"')
        node.cmd('export NDN_LOG="ndn_service_framework.*=TRACE:muas.*=TRACE:nacabe.*=TRACE:ndnsvs.*=TRACE"')
        # node.cmd('export NDN_LOG="*=TRACE"')
        
        node.cmd('ndnsec import -P 123456 /tmp/muas.ndnkey')
        node.cmd('ndnsec cert-install -f /tmp/muas.cert')

        node.cmd('ndnsec import -P 123456 /tmp/aa.ndnkey')
        node.cmd('ndnsec cert-install -f /tmp/aa.cert')
        for node2 in ndn.net.hosts:
            # node.cmd('ndnsec cert-install -f /tmp/{}.cert'.format(node2.name))
            node.cmd('ndnsec import -P 123456 /tmp/{}.ndnkey'.format(node2.name))
            node.cmd('ndnsec cert-install -f /tmp/{}.cert'.format(node2.name))


def registerRouteToAllNeighbors(ndn, host, syncPrefix):
    for node in ndn.net.hosts:
        for neighbor in node.connectionsTo(host):
            ip = node.IP(neighbor[0])
            faceID = Nfdc.createFace(host, ip)
            Nfdc.registerRoute(host, syncPrefix, faceID)

def configure_static_routes(ndn, hosts):
    
    # forward route: gs1 -> drone1 -> ... -> drone5
    for i in range(len(hosts) - 1):
        src = ndn.net[hosts[i]]
        next_hop = ndn.net[hosts[i + 1]]


        iface = next_hop.connectionsTo(src)[0][0]
        next_hop_ip = iface.IP()

        dst = ndn.net[hosts[-1]]
        dst_ip = dst.IP()

        src.cmd(f"ip route add {dst_ip} via {next_hop_ip}")
        info(f"ip route add {dst_ip} via {next_hop_ip}")

    # reverse route: drone5 -> ... -> drone1 -> gs1
    for i in range(len(hosts) - 1, 0, -1):
        src = ndn.net[hosts[i]]
        next_hop = ndn.net[hosts[i - 1]]

        iface = next_hop.connectionsTo(src)[0][0]
        next_hop_ip = iface.IP()

        dst = ndn.net[hosts[0]]
        dst_ip = dst.IP()

        src.cmd(f"ip route add {dst_ip} via {next_hop_ip}")
        info(f"ip route add {dst_ip} via {next_hop_ip}")

    info("Static IP routes configured.\n")

if __name__ == '__main__':
    setLogLevel('info')
    args = build_parser().parse_args()
    deadline = time.monotonic() + args.max_runtime_seconds
    output_dir = output_directory(args)
    output_dir.mkdir(parents=True, exist_ok=True)
    tracker = ProcessTracker()
    atexit.register(tracker.terminate_all)

    Minindn.cleanUp()
    Minindn.verifyDependencies()

    ndn = Minindn(topoFile="./Topology/testbed(loss=0%).conf")
    atexit.register(Minindn.cleanUp)
    atexit.register(ndn.stop)

    ndn.start()

    info('Starting NFD on nodes\n')
    nfds = AppManager(ndn, ndn.net.hosts, Nfd, logLevel='DEBUG')
    
    info('Starting NLSR on nodes\n')
    nlsrs = AppManager(ndn, ndn.net.hosts, Nlsr)

    generateAndSignCertificates(ndn)
    
    # nlsr
    for node in ndn.net.hosts:
        # node.cmd("nlsrc advertise /muas")
        Nfdc.setStrategy(node, "/muas", Nfdc.STRATEGY_MULTICAST)
        registerRouteToAllNeighbors(ndn, node, "/muas")
        
        node.cmd("nlsrc advertise /muas/{}".format(node.name))
        # node.cmd('nfdc strategy set /muas/{} /localhost/nfd/strategy/multicast'.format(node.name))
        # node.cmd('nfdc strategy set /muas/{}/provider /localhost/nfd/strategy/multicast'.format(node.name))
        # node.cmd('nfdc strategy set /muas/{}/user /localhost/nfd/strategy/multicast'.format(node.name))
        node.cmd('nfdc strategy set /muas/{}/NDNSF /localhost/nfd/strategy/multicast'.format(node.name))
        node.cmd("nlsrc advertise /muas/NDNSD") # for ndnsd
        node.cmd('nfdc strategy set /muas/NDNSD /localhost/nfd/strategy/multicast')
        time.sleep(5)

    configure_static_routes(ndn, ['memphis', 'csu', 'ucla'])

    ndn.net["memphis"].cmd("nlsrc advertise /muas/aa")

    # sleep for 30 seconds to allow NLSR to propagate the prefixes
    print("Waiting for NLSR to propagate prefixes...")
    time.sleep(25)

    # do some experiment here

    ucla = ndn.net["ucla"]
    pku = ndn.net["pku"]
    csu = ndn.net["csu"]
    caida = ndn.net["caida"]
    arizona = ndn.net["arizona"]
    uiuc = ndn.net["uiuc"]
    wustl = ndn.net["wustl"]
    umich = ndn.net["umich"]
    neu = ndn.net["neu"]
    memphis = ndn.net["memphis"]


    controller_log = (output_dir / "service-controller.log").open("wb")
    tracker.getPopen(memphis,
                     "service-controller",
                     "exec service-controller-example",
                     shell=True,
                     stdout=controller_log,
                     stderr=controller_log)




    # time.sleep(2)
    # drone2.cmd('xterm -T "drone2" -e "multi-drone-example /muas/drone2" &')
    # time.sleep(2)
    # drone3.cmd('xterm -T "drone3" -e "multi-drone-example /muas/drone3" &')
    # time.sleep(2)
    # drone4.cmd('xterm -T "drone4" -e "multi-drone-example /muas/drone4" &')
    # time.sleep(10)
    # gs1.cmd('xterm -T "gs1" -e "multi-gs-example AllResponders /muas/gs1 200 1000 /muas/drone1 /muas/drone2 /muas/drone3 /muas/drone4 /muas/drone5" &')
    # memphis.cmd('xterm -T "NDNSF Client memphis" -e "multi-gs-example AllResponders /muas/memphis 50 60 /muas/ucla /muas/caida /muas/neu" &')
    # arizona uiuc
    
    frequency = 150
    time_in_seconds = 60
    strategy = "AllResponders"
    # strategy = "FirstResponding"
    # strategy = "RandomSelection"
    clients = ['memphis']
    # clients = ['memphis', 'arizona', 'wustl']
    # clients = ['memphis', 'arizona', 'wustl', 'uiuc', 'neu']
    servers_id = ['ucla']
    #servers_id = ['ucla', 'arizona', 'wustl', 'uiuc', 'neu']

    for node_id in servers_id:
        cmd = f'exec multi-drone-example /muas/{node_id}'
        # cmd = f'xterm -sl 5000 -T "NDNSF Server {node_id}" -hold -e bash -c "gdb -q -ex run -ex bt -ex quit --args multi-drone-example /muas/{node_id} 2>&1 | tee /tmp/{node_id}.log" &'


        info(f"{node_id} {cmd}")
        node_log = (output_dir / "{}.log".format(node_id)).open("wb")
        tracker.getPopen(ndn.net[node_id],
                         "server-{}".format(node_id),
                         cmd,
                         shell=True,
                         stdout=node_log,
                         stderr=node_log)
        time.sleep(1)
        # NDNPing.startPingServer(ndn.net[node_id], f"/muas/{node_id}")
        time.sleep(1)

    for client in clients:
        server_args = ' '.join(f'/muas/{sid}' for sid in servers_id)
        client_identity = f"/muas/{client}"
        ping_target = servers_id[0]  # 假设你只 ping 第一个服务器
        cmd = f'exec multi-gs-example {strategy} {client_identity} {frequency} {time_in_seconds} {server_args}'
        # cmd = f'xterm -sl 5000 -T "NDNSF Client {client}" -hold -e bash -c "gdb -q -ex run -ex bt -ex quit --args multi-gs-example {strategy} {client_identity} {frequency} {time_in_seconds} {server_args} 2>&1 | tee /tmp/{client}.log" &'

        info(f"{client} {cmd}")
        client_log = (output_dir / "{}.log".format(client)).open("wb")
        tracker.getPopen(ndn.net[client],
                         "client-{}".format(client),
                         cmd,
                         shell=True,
                         stdout=client_log,
                         stderr=client_log)
        time.sleep(2)
        cmd2 = (
            f'xterm -hold -T "NDNSF Client {client} ping" '
            f'-e bash -c "ndnping /muas/{ping_target}" &'
        )
        # ndn.net[client].cmd(cmd2)
        time.sleep(1)
        # NDNPing.startPingServer(ndn.net[client], f"/muas/{client}")
        time.sleep(1)
        # for node_id in servers_id:
        #     ping_target = node_id
        #     cmd2 = (
        #         f'xterm -hold -T "NDNSF Client {client} ping" '
        #         f'-e bash -c "ndnping /muas/{ping_target}" &'
        #     )
        #     ndn.net[client].cmd(cmd2)


        # eval(f"{client}.cmd('{cmd2}')")
        time.sleep(2)


    time.sleep(10)


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

    # memphis xterm -T "memphis" &
    # ucla xterm -T "ucla" &
    # source /home/tianxing/NDN/ndn-service-framework/Experiments/pythonEnv/bin/activate
    # python /home/tianxing/NDN/ndn-service-framework/Experiments/gRPC/greeter_server.py
    # python /home/tianxing/NDN/ndn-service-framework/Experiments/gRPC/greeter_client.py
    
    # ucla xterm -T "ucla" -e "/home/tianxing/NDN/ndn-service-framework/Experiments/NDN_NSC/producer /muas/ucla /FlightControl /ManualControl" &
    # memphis xterm -T "memphis" -e "/home/tianxing/NDN/ndn-service-framework/Experiments/NDN_NSC/consumer /muas/memphis /muas/ucla /FlightControl /ManualControl 100 1000" &

    if args.interactive_cli:
        MiniNDNCLI(ndn.net)
    else:
        wait_seconds = max(1, time_in_seconds + 10)
        info("Waiting {}s for workload completion\n".format(wait_seconds))
        bounded_sleep(wait_seconds, deadline, "workload completion")

    tracker.terminate_all()

    ndn.stop()
