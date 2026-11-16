#!/usr/bin/env python3
"""Qualify installed original-packet APIs on two explicit deployment namespaces."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import signal
import subprocess
import time

from ndn.encoding import Name


ROOT = Path(__file__).resolve().parents[2]


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--scratch-root', type=Path, required=True)
    parser.add_argument('--producer', action='append', required=True)
    parser.add_argument('--prefix', type=Path, default=Path('/usr/local'))
    parser.add_argument('--timeout', type=float, default=15)
    parser.add_argument('--smoke-test', action='store_true')
    args = parser.parse_args()
    if os.geteuid() != 0 or len(args.producer) != 2 or not 0 < args.timeout <= 60:
        parser.error('root, two explicit Producers and a bounded timeout are required')
    names = [Name.from_str(value) for value in args.producer]
    if any(not name for name in names) or any(
            len(a) <= len(b) and all(bytes(x) == bytes(y) for x, y in zip(a, b))
            for a, b in ((names[0], names[1]), (names[1], names[0]))):
        parser.error('Producers must be non-root, component-disjoint namespaces')
    topology = ROOT / 'Experiments/Topology/spec182-native-closure.conf'
    oracle = args.prefix / 'bin/DistributedRepoExactPacketTest'
    server = args.prefix / 'bin/NDNSF_RepoPacketServer'
    artifacts = {}
    for installed, built in (
            (oracle, ROOT / 'build/NDNSF-DistributedRepo/DistributedRepoExactPacketTest'),
            (server, ROOT / 'build/NDNSF-DistributedRepo/NDNSF_RepoPacketServer'),
            (args.prefix / 'lib/libndn-service-framework.so', ROOT / 'build/libndn-service-framework.so')):
        if digest(installed) != digest(built):
            raise RuntimeError('installed/build artifact mismatch: ' + str(installed))
        closure = subprocess.run(['ldd', str(installed)], capture_output=True, text=True, check=True)
        if 'not found' in closure.stdout + closure.stderr:
            raise RuntimeError('incomplete installed closure: ' + str(installed))
        artifacts[str(installed)] = digest(installed)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    receipt = output / 'result.json'
    history = json.loads(receipt.read_text()) if receipt.exists() else {'attempts': []}
    attempt = len(history['attempts']) + 1
    result = dict(status='IN_PROGRESS', attempt=attempt, artifacts=artifacts,
                  sourceCommit=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT,
                                                        text=True).strip(),
                  harnessSha256=digest(__file__), topologySha256=digest(topology),
                  producers=args.producer, phases=[], children=[], forcedCleanup=False)
    from minindn.minindn import Minindn
    from minindn.apps.app_manager import AppManager
    from minindn.apps.nfd import Nfd
    from minindn.helpers.nfdc import Nfdc
    from minindn.util import getPopen
    ndn = None
    manager = None
    active = []
    streams = []
    stage = 'facility-start'
    started = time.monotonic()

    def interrupted(signum, frame):
        raise KeyboardInterrupt('signal ' + str(signum))

    previous = {number: signal.signal(number, interrupted) for number in (signal.SIGINT, signal.SIGTERM)}

    def stop(process):
        if process.poll() is None:
            process.send_signal(signal.SIGINT)
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                result['forcedCleanup'] = True
                process.kill()
                process.wait(timeout=5)
        for row in result['children']:
            if row['pid'] == process.pid:
                row['exit'] = process.returncode

    def run(node, command, label, destination=None):
        error = output / (str(attempt) + '-' + label + '.stderr')
        with error.open('wb') as errors:
            if destination is None:
                proc = getPopen(node, [str(value) for value in command],
                                envDict={'LD_LIBRARY_PATH': ''}, stdout=subprocess.PIPE, stderr=errors)
                active.append(proc)
                stdout, _ = proc.communicate(timeout=args.timeout)
            else:
                with destination.open('wb') as target:
                    proc = getPopen(node, [str(value) for value in command],
                                    envDict={'LD_LIBRARY_PATH': ''}, stdout=target, stderr=errors)
                    active.append(proc)
                    proc.wait(timeout=args.timeout)
                stdout = b''
        if proc.returncode:
            raise RuntimeError(label + ': ' + error.read_text(errors='replace')[-1500:])
        return stdout

    try:
        ndn = Minindn(parser=parser, topoFile=str(topology),
                      workDir=str(args.scratch_root / 'nodes'), controller=None)
        if Minindn.ndnSecurityDisabled:
            raise RuntimeError('dummy keychain cannot qualify signature behavior')
        ndn.start()
        manager = AppManager(ndn, [], Nfd)
        for node in ndn.net.hosts:
            manager.startOnNode(node, logLevel='NONE', csSize=0)
            app = manager[node.name]
            node.cmd('export NDN_CLIENT_TRANSPORT=' + shlex.quote('unix://' + app.sockFile))
            result['children'].append(dict(pid=app.process.pid, role='nfd', node=node.name))
            info = run(node, ['nfdc', 'cs', 'info'], 'cs-' + node.name).decode()
            if 'capacity=0' not in info:
                raise RuntimeError('NFD content store is not disabled: ' + info)
        requester, provider = ndn.net['requester'], ndn.net['provider']
        face = str(Nfdc.createFace(requester, provider.IP()))
        reverse = str(Nfdc.createFace(provider, requester.IP()))
        if '-1' in (face, reverse):
            raise RuntimeError('point-to-point NFD face creation failed')
        result['contentStoreCapacity'] = 0
        selected = args.producer[:1] if args.smoke_test else args.producer
        fetched = []
        for index, producer in enumerate(selected):
            case = output / ('namespace-' + str(index))
            case.mkdir(exist_ok=True)
            seed = case / 'seed'
            stage = 'seed-' + str(index)
            if not seed.exists():
                subprocess.run([str(oracle), '--seed-original-packets', str(seed), producer],
                               check=True, capture_output=True, timeout=args.timeout)
            actual = Name.from_str((seed / 'producer-prefix.txt').read_text().strip())
            if Name.to_bytes(actual) != Name.to_bytes(Name.from_str(producer)):
                raise RuntimeError('retained seed does not match configured Producer')
            rows = [line.split('\t') for line in (seed / 'packets.tsv').read_text().splitlines()]
            if len(rows) != 3 or any(row[2] != str(i) + '.wire' for i, row in enumerate(rows)):
                raise RuntimeError('incomplete original packet fixture')
            run(requester, ['nfdc', 'route', 'add', producer, face], 'route-' + str(index))
            for phase in (('first',) if args.smoke_test else ('first', 'restart')):
                stage = str(index) + '-' + phase
                log = (case / (str(attempt) + '-' + phase + '-server.log')).open('wb')
                streams.append(log)
                proc = getPopen(provider, [str(server), str(seed / 'r1'), producer + '/replica',
                                          str(1 << 20)],
                                envDict={'LD_LIBRARY_PATH': ''}, stdout=log, stderr=subprocess.STDOUT)
                active.append(proc)
                result['children'].append(dict(pid=proc.pid, role='repo', phase=stage))
                deadline = time.monotonic() + args.timeout
                while 'REPO_ORIGINAL_PACKETS_READY ' not in Path(log.name).read_text(errors='replace'):
                    if proc.poll() is not None or time.monotonic() >= deadline:
                        raise RuntimeError('Repo original-prefix registration failed: ' + str(log.name))
                    time.sleep(0.05)
                destination = case / (str(attempt) + '-' + phase)
                destination.mkdir()
                for packet_name, expected_digest, filename in rows:
                    run(requester, [str(args.prefix / 'bin/ndnpeek'), '-w', '5000', '-f', packet_name],
                        stage + '-' + filename, destination / filename)
                    if digest(destination / filename) != expected_digest:
                        raise RuntimeError('fetched original wire digest mismatch')
                proof = subprocess.run([str(oracle), '--verify-original-packets', str(seed),
                                        str(destination)], check=True, capture_output=True,
                                       text=True, timeout=args.timeout)
                result['phases'].append(dict(namespace=producer, phase=phase, pid=proc.pid,
                                             packets=len(rows), oracle=proof.stdout.strip(),
                                             wires=[dict(name=row[0], sha256=row[1]) for row in rows]))
                fetched.append((seed, destination))
                stop(proc)
                if proc.returncode != 0:
                    raise RuntimeError('Repo did not close normally')
            run(requester, ['nfdc', 'route', 'remove', producer, face], 'remove-route-' + str(index))
        if not args.smoke_test:
            stage = 'foreign-producer-rejection'
            rejected = subprocess.run([str(oracle), '--verify-original-packets', str(fetched[0][0]),
                                       str(fetched[-1][1])], capture_output=True, text=True,
                                      timeout=args.timeout)
            if rejected.returncode == 0 or 'fetched wire differs' not in rejected.stderr:
                raise RuntimeError('cross-namespace original-wire reference was not rejected')
            result['foreignProducerRejected'] = True
        result['status'] = 'SMOKE_OK' if args.smoke_test else 'PASS'
    except BaseException as error:
        result.update(status='FAIL', firstFailure=dict(boundary=stage, reason=str(error)))
    finally:
        def cleanup(action):
            try:
                action()
            except BaseException as error:
                result['status'] = 'FAIL'
                result.setdefault('cleanupErrors', []).append(str(error))

        for process in reversed(active):
            cleanup(lambda process=process: stop(process))
        for log in streams:
            log.close()
        if manager is not None:
            for app in manager:
                if app.process is not None:
                    cleanup(lambda app=app: stop(app.process))
        if ndn is not None:
            cleanup(ndn.stop)
        if result['forcedCleanup']:
            result['status'] = 'FAIL'
        result['wallSeconds'] = time.monotonic() - started
        if args.smoke_test and result['wallSeconds'] >= 10:
            result.update(status='FAIL', firstFailure=dict(boundary='smoke-budget',
                          reason='smoke exceeded 10 seconds; no qualification claim'))
        history['attempts'].append(result)
        receipt.write_text(json.dumps(history, indent=2, sort_keys=True) + '\n')
        for number, handler in previous.items():
            signal.signal(number, handler)
    print(json.dumps({key: result.get(key) for key in
                      ('status', 'wallSeconds', 'firstFailure', 'forcedCleanup')}, sort_keys=True))
    return 0 if result['status'] in ('PASS', 'SMOKE_OK') else 1


if __name__ == '__main__':
    raise SystemExit(main())
