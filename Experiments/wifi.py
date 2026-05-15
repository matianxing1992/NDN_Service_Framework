#!/usr/bin/env python3
import os
import sys
import time
import json
import tempfile
import re
from typing import Optional, Tuple, List, Dict

from subprocess import PIPE

from mininet.log import setLogLevel, info
from minindn.wifi.minindnwifi import MinindnWifi
from minindn.util import MiniNDNWifiCLI, getPopen

from minindn.apps.app_manager import AppManager
from minindn.apps.nfd import Nfd

# ====== 参数 ======
RPC_RATE_HZ = 10
RPC_DURATION_SEC = 60
RPC_TIMEOUT_SEC = 0.25
GRPC_PORT = 50051

NDN_PING_INTERVAL_MS = 100
NDN_PING_TIMEOUT_MS = 250
NDN_PING_COUNT = int(RPC_DURATION_SEC * (1000 // NDN_PING_INTERVAL_MS))

DRONE_NAMES = ["drone1", "drone2", "drone3", "drone4"]

PROTO = r"""
syntax = "proto3";
package ndnsf;

service PingService {
  rpc Ping (PingReq) returns (PingResp) {}
}

message PingReq {
  int64 seq = 1;
  int64 client_ts_ms = 2;
}

message PingResp {
  int64 seq = 1;
  int64 server_ts_ms = 2;
}
""".lstrip()

SERVER_PY = r"""
import time
import grpc
from concurrent import futures
import ping_pb2, ping_pb2_grpc

def now_ms():
    return int(time.time() * 1000)

class Svc(ping_pb2_grpc.PingServiceServicer):
    def Ping(self, request, context):
        return ping_pb2.PingResp(seq=request.seq, server_ts_ms=now_ms())

def main():
    server = grpc.server(
        futures.ThreadPoolExecutor(max_workers=8),
        options=[
            ("grpc.so_reuseport", 0),
            ("grpc.keepalive_time_ms", 10000),
            ("grpc.keepalive_timeout_ms", 5000),
        ],
    )
    ping_pb2_grpc.add_PingServiceServicer_to_server(Svc(), server)
    server.add_insecure_port("[::]:%d")
    server.start()
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        server.stop(0)

if __name__ == "__main__":
    main()
""" % GRPC_PORT

CLIENT_PY = r"""
import time, json, argparse
import grpc
import ping_pb2, ping_pb2_grpc

def now_ms():
    return int(time.time() * 1000)

def make_channel(target):
    opts = [
        ("grpc.keepalive_time_ms", 5000),
        ("grpc.keepalive_timeout_ms", 2000),
        ("grpc.http2.max_pings_without_data", 0),
        ("grpc.keepalive_permit_without_calls", 1),
        ("grpc.enable_retries", 0),
    ]
    return grpc.insecure_channel(target, options=opts)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--target", required=True)
    ap.add_argument("--rate", type=float, default=10.0)
    ap.add_argument("--duration", type=float, default=300.0)
    ap.add_argument("--timeout", type=float, default=0.25)
    ap.add_argument("--out", default="grpc_results.jsonl")
    args = ap.parse_args()

    interval = 1.0 / args.rate
    total = int(args.duration * args.rate)

    ch = make_channel(args.target)
    stub = ping_pb2_grpc.PingServiceStub(ch)

    ok = 0
    fail = 0

    start = time.time()
    next_t = start

    with open(args.out, "w", buffering=1) as f:
        for seq in range(total):
            now = time.time()
            if now < next_t:
                time.sleep(next_t - now)
            next_t += interval

            t0 = time.time()
            req = ping_pb2.PingReq(seq=seq, client_ts_ms=now_ms())

            try:
                stub.Ping(req, timeout=args.timeout)
                rtt_ms = (time.time() - t0) * 1000.0
                ok += 1
                rec = {"seq": seq, "ok": True, "rtt_ms": rtt_ms, "t_ms": now_ms()}
            except grpc.RpcError as e:
                fail += 1
                rec = {"seq": seq, "ok": False, "code": str(e.code()), "details": e.details(), "t_ms": now_ms()}
                ch = make_channel(args.target)
                stub = ping_pb2_grpc.PingServiceStub(ch)

            f.write(json.dumps(rec) + "\n")

    elapsed = time.time() - start
    success = (ok / total) if total else 0.0
    print("TOTAL=%d OK=%d FAIL=%d SUCCESS=%.4f ELAPSED=%.2fs OUT=%s" % (total, ok, fail, success, elapsed, args.out))

if __name__ == "__main__":
    main()
""".lstrip()


def write_grpc_artifacts(workdir: str) -> None:
    proto_path = os.path.join(workdir, "ping.proto")
    with open(proto_path, "w") as f:
        f.write(PROTO)

    cmd = '"%s" -m grpc_tools.protoc -I "%s" --python_out "%s" --grpc_python_out "%s" "%s"' % (
        sys.executable, workdir, workdir, workdir, proto_path
    )
    rc = os.system(cmd)
    if rc != 0:
        raise RuntimeError("Failed to run grpc_tools.protoc. Make sure grpcio-tools is installed for this Python.")

    with open(os.path.join(workdir, "server.py"), "w") as f:
        f.write(SERVER_PY)
    with open(os.path.join(workdir, "client.py"), "w") as f:
        f.write(CLIENT_PY)


def get_sta_ipv4(sta, prefer_ifname: Optional[str] = None) -> Optional[str]:
    if prefer_ifname is None:
        prefer_ifname = "%s-wlan0" % sta.name
    ip = sta.cmd("ip -4 -o addr show dev %s | awk '{print $4}' | cut -d/ -f1" % prefer_ifname).strip()
    if ip:
        return ip
    ip = sta.cmd("ip -4 -o addr | awk '{print $4}' | cut -d/ -f1 | grep '^10\\.0\\.0\\.' | head -n1").strip()
    return ip if ip else None


def wait_for_ip(sta, timeout_sec: int = 30) -> str:
    t0 = time.time()
    while time.time() - t0 < timeout_sec:
        ip = get_sta_ipv4(sta)
        if ip:
            return ip
        time.sleep(0.5)
    raise RuntimeError("%s has no IPv4 after %ds" % (sta.name, timeout_sec))


def wait_listen_tcp(sta, port: int, timeout_sec: int = 10) -> bool:
    t0 = time.time()
    while time.time() - t0 < timeout_sec:
        rc = sta.cmd("ss -lnt | awk '{print $4}' | grep -q ':%d$'; echo $?" % port).strip()
        if rc == "0":
            return True
        time.sleep(0.5)
    return False


def parse_ndnping_log(text: str) -> Tuple[int, int]:
    m = re.search(r"(\d+)\s+packets transmitted,\s+(\d+)\s+received", text)
    if not m:
        return 0, 0
    return int(m.group(1)), int(m.group(2))


def cleanup_gs_ping_producers(gs, drone_names: List[str]) -> None:
    gs.cmd('pkill -f "ndnpingserver /drone" >/dev/null 2>&1 || true')
    for name in drone_names:
        gs.cmd('nfdc route remove "/%s/ping" >/dev/null 2>&1 || true' % name)


def must_not_run_on_gs(gs) -> None:
    ps = gs.cmd('pgrep -fa "ndnpingserver /drone" 2>/dev/null || true').strip()
    if ps:
        info("\n!!! WARNING: ndnpingserver is still running on gs namespace:\n%s\n" % ps)


def ensure_gs_routes_to_prefixes(gs, routes: List[Tuple[str, str]]) -> None:
    for prefix, ip in routes:
        gs.cmd('nfdc route add "%s" udp4://%s >/dev/null 2>&1 || true' % (prefix, ip))


def proc_wait_or_kill(proc, timeout_sec: int, name: str) -> None:
    if proc is None:
        return
    try:
        proc.wait(timeout=timeout_sec)
    except Exception:
        info("\n!!! WARNING: %s timeout after %ds, killing...\n" % (name, timeout_sec))
        try:
            proc.terminate()
            proc.wait(timeout=2)
        except Exception:
            try:
                proc.kill()
            except Exception:
                pass


def nfd_sock_path_exists(node) -> str:
    # 常见两种路径都测一下（不同安装/发行版可能不同）
    for p in ["/run/nfd/nfd.sock", "/var/run/nfd/nfd.sock"]:
        if node.cmd('test -S %s; echo $?' % p).strip() == "0":
            return p
    return ""


def wait_nfd_ready(node, name: str, timeout_sec: float = 8.0) -> str:
    t0 = time.time()
    while time.time() - t0 < timeout_sec:
        sock = nfd_sock_path_exists(node)
        if sock:
            # 再验证 nfd-status 能跑通（有些情况下 sock 存在但进程挂了）
            rc = node.cmd("nfd-status >/dev/null 2>&1; echo $?").strip()
            if rc == "0":
                return sock
        time.sleep(0.2)

    # 失败就打印诊断
    info("\n!!! ERROR: NFD not ready on %s\n" % name)
    info(node.cmd("pgrep -fa nfd || true"))
    info(node.cmd("ls -l /run/nfd /var/run/nfd 2>/dev/null || true"))
    info(node.cmd("which nfd-start nfd-status nfdc 2>/dev/null || true"))
    info(node.cmd("nfd-status 2>&1 | tail -n 50 || true"))
    raise RuntimeError("NFD not ready on %s (no socket / status failed)" % name)


def start_ndnpingserver_popen(node, prefix: str, log_path: str):
    # prefix 绝不能加引号
    node.cmd('pkill -f "ndnpingserver %s" >/dev/null 2>&1 || true' % prefix)
    logf = open(log_path, "w")
    proc = getPopen(node, "ndnpingserver %s" % prefix, stdout=logf, stderr=logf)
    return proc, logf


def start_cmd_popen(node, cmd: str, log_path: str):
    logf = open(log_path, "w")
    proc = getPopen(node, cmd, stdout=logf, stderr=logf)
    return proc, logf


def main():
    setLogLevel('info')
    ndn = MinindnWifi(topoFile='Topology/1gs3drones_wifi.conf')

    workdir = None
    procs: Dict[str, object] = {}
    log_handles: Dict[str, object] = {}

    try:
        ndn.start()
        time.sleep(10)
        gs = ndn.net['gs']

        workdir = tempfile.mkdtemp(prefix="minindn-ndnping-grpc-")
        info("\n*** workdir: %s\n" % workdir)

        # 先清 gs 污染
        info("\n*** Cleaning gs local ndnpingserver + /drone*/ping app routes (avoid local self-reply)\n")
        cleanup_gs_ping_producers(gs, DRONE_NAMES)
        must_not_run_on_gs(gs)

        # 先拿 IP（你已经跑到这里没问题）
        drone_ips = {}
        for name in DRONE_NAMES:
            node = ndn.net[name]
            ip = wait_for_ip(node, timeout_sec=30)
            drone_ips[name] = ip
            info("*** %s IPv4 = %s\n" % (name, ip))

        # ====== 统一用 AppManager 启动 NFD（放在拿到 IP 之后，减少并发 cmd 冲突） ======
        info("\n*** Starting NFD via AppManager on all nodes\n")
        AppManager(ndn, ndn.net.hosts, Nfd, logLevel="INFO")

        time.sleep(10)

        # 在 gs 上给 /droneX 前缀加 route
        routes = [("/%s" % name, drone_ips[name]) for name in DRONE_NAMES]
        ensure_gs_routes_to_prefixes(gs, routes)

        info("\n*** gs routes (should NOT include /drone*/ping origin=app):\n")
        info(gs.cmd("nfdc route list | cat"))

        # 启动 ndnpingserver（只在 drone 上）
        for name in DRONE_NAMES:
            node = ndn.net[name]
            prefix = "/%s" % name
            safe = prefix.strip("/").replace("/", "_")
            log_path = "%s/ndnpingserver_%s.log" % (workdir, safe)

            proc, logf = start_ndnpingserver_popen(node, prefix, log_path)
            procs["ndnpingserver:%s" % name] = proc
            log_handles["ndnpingserver:%s" % name] = logf

            info("*** started ndnpingserver on %s for prefix %s\n" % (name, prefix))

            chk = node.cmd('pgrep -fa "ndnpingserver %s" 2>/dev/null || true' % prefix).strip()
            if not chk:
                info("\n!!! WARNING: ndnpingserver NOT found on %s for %s (check log)\n" % (name, prefix))
                info(node.cmd('tail -n 80 "%s" 2>/dev/null || true' % log_path))

        # gRPC artifacts
        write_grpc_artifacts(workdir)

        # gRPC server：drone1
        drone1 = ndn.net['drone1']
        drone1_ip = drone_ips["drone1"]
        py = sys.executable

        info("\n*** Starting gRPC server on drone1 (%s:%d)\n" % (drone1_ip, GRPC_PORT))
        drone1.cmd('pkill -f "%s/server.py" >/dev/null 2>&1 || true' % workdir)

        grpc_server_log = "%s/server.log" % workdir
        cmd = '"%s" "%s/server.py"' % (py, workdir)
        proc, logf = start_cmd_popen(drone1, cmd, grpc_server_log)
        procs["grpc:server"] = proc
        log_handles["grpc:server"] = logf

        if not wait_listen_tcp(drone1, GRPC_PORT, timeout_sec=10):
            info(drone1.cmd("tail -n 120 %s || true" % grpc_server_log))
            raise RuntimeError("gRPC server did not start/listen on drone1")

        # ndnping /drone1（后台）
        ndn_ping_log = "%s/ndnping_drone1.log" % workdir
        info("\n*** Starting ndnping on gs -> /drone1, interval=%dms, timeout=%dms, count=%d (~%ds)\n" %
             (NDN_PING_INTERVAL_MS, NDN_PING_TIMEOUT_MS, NDN_PING_COUNT, RPC_DURATION_SEC))

        gs.cmd('pkill -f "ndnping .* /drone1" >/dev/null 2>&1 || true')
        ndnping_cmd = 'ndnping -i %d -o %d -c %d /drone1' % (
            NDN_PING_INTERVAL_MS, NDN_PING_TIMEOUT_MS, NDN_PING_COUNT
        )
        proc, logf = start_cmd_popen(gs, ndnping_cmd, ndn_ping_log)
        procs["ndnping:/drone1"] = proc
        log_handles["ndnping:/drone1"] = logf

        # ANY-DRONE probe（后台）
        any_log = "%s/ndnping_any_drone.log" % workdir
        targets = " ".join(["/%s" % n for n in DRONE_NAMES])

        probe_bash = r"""
DUR={dur}
INT_MS={int_ms}
END=$((SECONDS + DUR))
OK=0
TOTAL=0
while [ $SECONDS -lt $END ]; do
  HIT=0
  for PFX in {targets}; do
    OUT=$(timeout 0.40 ndnping -i 1 -o {to_ms} -c 1 "$PFX" 2>/dev/null || true)
    echo "$OUT" | grep -q "1 packets transmitted, 1 received" && {{ HIT=1; break; }}
  done
  echo $HIT
  TOTAL=$((TOTAL+1))
  OK=$((OK+HIT))
  sleep $(python3 - <<PY
print({int_ms}/1000.0)
PY)
done
python3 - <<PY
ok={ok}
total={total}
ratio=(ok/total) if total else 0.0
print("ANY_OK_RATIO %.6f (%d/%d)"%(ratio,ok,total))
PY
""".format(
            dur=RPC_DURATION_SEC,
            int_ms=NDN_PING_INTERVAL_MS,
            targets=targets,
            to_ms=NDN_PING_TIMEOUT_MS,
            ok="${OK}",
            total="${TOTAL}",
        ).strip()

        probe_cmd = 'bash -lc "{}"'.format(probe_bash.replace('"', r'\"'))
        proc, logf = start_cmd_popen(gs, probe_cmd, any_log)
        procs["ndnping:any"] = proc
        log_handles["ndnping:any"] = logf

        # gRPC client（前台）
        target = "%s:%d" % (drone1_ip, GRPC_PORT)
        grpc_out = "%s/grpc_results.jsonl" % workdir
        info("\n*** Running gRPC client on gs -> %s, rate=%d/s, duration=%ds\n" %
             (target, RPC_RATE_HZ, RPC_DURATION_SEC))

        grpc_cmd = ('"%s" "%s/client.py" --target "%s" --rate %d --duration %d --timeout %.3f --out "%s"' %
                    (py, workdir, target, RPC_RATE_HZ, RPC_DURATION_SEC, RPC_TIMEOUT_SEC, grpc_out))
        print(gs.cmd(grpc_cmd))

        # 等 ping 结束
        proc_wait_or_kill(procs.get("ndnping:/drone1"), timeout_sec=RPC_DURATION_SEC + 60, name="ndnping:/drone1")
        proc_wait_or_kill(procs.get("ndnping:any"), timeout_sec=RPC_DURATION_SEC + 60, name="ndnping:any")

        # flush/close
        for k in ["ndnping:/drone1", "ndnping:any"]:
            try:
                if log_handles.get(k):
                    log_handles[k].flush()
                    log_handles[k].close()
            except Exception:
                pass

        # 解析 ndnping
        ndn_text = gs.cmd('cat "%s" 2>/dev/null || true' % ndn_ping_log)
        sent, recv = parse_ndnping_log(ndn_text)
        if sent == 0:
            info("\n*** ndnping parse failed; tail:\n")
            info(gs.cmd('tail -n 120 "%s" || true' % ndn_ping_log))
        else:
            succ = float(recv) / float(sent)
            info("\n*** NDN PING RESULT (/drone1): sent=%d recv=%d success=%.4f loss=%.2f%%\n" %
                 (sent, recv, succ, (1.0 - succ) * 100.0))

        ratio_line = gs.cmd('grep "ANY_OK_RATIO" "%s" | tail -n 1 2>/dev/null || true' % any_log).strip()
        if ratio_line:
            info("\n*** %s\n" % ratio_line)
        else:
            info("\n*** ANY-DRONE ratio not found; tail:\n")
            info(gs.cmd('tail -n 120 "%s" || true' % any_log))

        info("\n*** Files:\n")
        info("  gRPC: %s\n" % grpc_out)
        info("  ndnping(/drone1): %s\n" % ndn_ping_log)
        info("  ndnping(any): %s\n" % any_log)
        info("  grpc server log: %s\n" % grpc_server_log)

        MiniNDNWifiCLI(ndn.net)

        ndn.net.stop()
        ndn.cleanUp()

    finally:
        for _, proc in list(procs.items()):
            try:
                if proc is not None and proc.poll() is None:
                    proc.terminate()
                    try:
                        proc.wait(timeout=2)
                    except Exception:
                        proc.kill()
            except Exception:
                pass

        for _, logf in list(log_handles.items()):
            try:
                logf.flush()
                logf.close()
            except Exception:
                pass

        try:
            ndn.stop()
        except Exception:
            pass


if __name__ == '__main__':
    main()