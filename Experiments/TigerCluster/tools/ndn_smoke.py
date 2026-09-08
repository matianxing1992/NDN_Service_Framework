#!/usr/bin/env python3
"""Two-rank C++/NFD/SIF diagnostic, locally or inside one Slurm allocation."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from runtime.baseline import BIN, Processes, nfd_config


def write(path, value):
    temp = path.with_suffix('.tmp')
    with temp.open('x') as stream:
        json.dump(value, stream, indent=2, sort_keys=True)
        stream.write('\n')
    temp.replace(path)


def digest(path):
    result = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(4*1024*1024), b''): result.update(block)
    return result.hexdigest()


def wait_for(path, root, timeout=30):
    deadline = time.monotonic()+timeout
    while not path.exists():
        if any(root.glob('rank*/failed.json')): raise RuntimeError('peer failed')
        if time.monotonic() >= deadline: raise TimeoutError(str(path))
        time.sleep(.1)
    return json.loads(path.read_text())


def rank(manifest_path, number):
    manifest = json.loads(manifest_path.read_text())
    root, bundle = manifest_path.parent, Path(manifest['bundle'])
    out = root/('rank'+str(number)); out.mkdir(mode=0o700)
    scratch = Path(tempfile.mkdtemp(prefix='ndn-smoke-'+str(number)+'-',
                                  dir=os.environ.get('SLURM_TMPDIR') or '/tmp'))
    (scratch/'home').mkdir(mode=0o700)
    port = manifest['port'] + (number if manifest['local'] else 0)
    (scratch/'nfd.conf').write_text(nfd_config(port))
    shutil.copy2(scratch/'nfd.conf', out/'nfd.conf')
    processes = Processes(out)
    record = dict(rank=number, host=socket.gethostname(), scratch=str(scratch),
                  port=port, status='FAIL', qualification='NDN_TRANSPORT_DIAGNOSTIC')
    def command(argv):
        return [manifest['apptainer'], 'exec', '--cleanenv', '--containall',
                '--home', '/node/home', '--pwd', '/node',
                '--bind', str(scratch)+':/node', '--bind', str(bundle)+':/probe:ro',
                manifest['sif'], '/usr/bin/env', 'HOME=/node/home',
                'PATH='+BIN+':/usr/bin:/bin', 'LD_LIBRARY_PATH=/opt/ndnsf-di/current/lib',
                'NDN_CLIENT_TRANSPORT=unix:///node/nfd.sock'] + argv
    def finite(label, argv):
        with (out/(label+'.log')).open('xb') as log:
            done = subprocess.run(command(argv), stdout=log, stderr=subprocess.STDOUT,
                                  stdin=subprocess.DEVNULL, timeout=20)
        if done.returncode: raise RuntimeError(label+' exit '+str(done.returncode))
    def cancelled(signum, _frame):
        raise RuntimeError('cancelled '+str(signum))
    for sig in (signal.SIGINT, signal.SIGTERM): signal.signal(sig, cancelled)
    try:
        if digest(bundle/'ndn-smoke') != manifest['binarySha256']: raise RuntimeError('binary hash')
        if not manifest['local'] and not manifest['verifySif']: raise RuntimeError('remote SIF hash required')
        for relative, expected in manifest.get('files', {}).items():
            if digest(bundle/relative) != expected: raise RuntimeError('bundle hash: '+relative)
        # A local runtime-only probe cannot repair the previously unstable SIF
        # identity. Cluster ranks must verify the same retained pinned image.
        if manifest['verifySif']:
            record['sifSha256'] = digest(Path(manifest['sif']))
            if record['sifSha256'] != manifest['sifSha256']: raise RuntimeError('SIF hash')
        else:
            record['sifIdentity'] = 'LOCAL_RUNTIME_ONLY_UNQUALIFIED'
        record['apptainerVersion'] = subprocess.check_output(
            [manifest['apptainer'], 'version'], text=True, timeout=10).strip()
        finite('ldd', ['/usr/bin/ldd','/probe/ndn-smoke'])
        if '=> not found' in (out/'ldd.log').read_text(): raise RuntimeError('loader closure')
        finite('help', ['/probe/ndn-smoke','--help'])
        finite('keygen', [BIN+'/ndnsec','key-gen',manifest['prefix']+'/'+str(number)])
        processes.start('nfd', command([BIN+'/nfd','--config','/node/nfd.conf']))
        deadline=time.monotonic()+15
        while not (scratch/'nfd.sock').exists():
            processes.check()
            if time.monotonic()>=deadline: raise TimeoutError('NFD socket')
            time.sleep(.1)
        finite('nfd-status', [BIN+'/nfdc','status','report'])
        address = '127.0.0.1' if manifest['local'] else socket.gethostbyname(socket.gethostname())
        write(out/'ready.json', dict(host=socket.gethostname(), address=address, port=port))
        peer = wait_for(root/('rank'+str(1-number))/'ready.json', root, 90)
        if not manifest['local'] and peer['host']==record['host']: raise RuntimeError('same physical node')
        uri='tcp4://'+peer['address']+':'+str(peer['port'])
        finite('face-create', [BIN+'/nfdc','face','create','remote',uri,'persistency','permanent'])
        finite('route-add', [BIN+'/nfdc','route','add','prefix',manifest['prefix'],'nexthop',uri,'cost','10'])
        finite('routes', [BIN+'/nfdc','route','list'])
        finite('faces', [BIN+'/nfdc','face','list'])
        if number == 0:
            producer = processes.start('producer', command(['/probe/ndn-smoke','producer',manifest['prefix'],manifest['token']]))
            deadline=time.monotonic()+10
            while 'READY ' not in (out/'producer.log').read_text(errors='replace'):
                processes.check()
                if time.monotonic()>=deadline: raise TimeoutError('producer ready')
                time.sleep(.1)
            write(out/'producer-ready.json', {'ready':True})
            # Completion is owned by the local process. Shared-filesystem
            # visibility of a peer marker is not an application verdict;
            # srun and the parent aggregate both ranks after they exit.
            if producer.wait(timeout=20): raise RuntimeError('producer exit')
            text=(out/'producer.log').read_text()
            if 'PASS producer count=3' not in text or any(line.startswith('FAIL ') for line in text.splitlines()):
                raise RuntimeError('producer result')
        else:
            wait_for(root/'rank0/producer-ready.json', root, 20)
            finite('consumer', ['/probe/ndn-smoke','consumer',manifest['prefix'],manifest['token']])
            text=(out/'consumer.log').read_text()
            if 'PASS consumer count=3' not in text or any(line.startswith('FAIL ') for line in text.splitlines()):
                raise RuntimeError('consumer result')
            write(out/'consumer-done.json', {'requests':3})
        record['status']='PASS'
    except BaseException as exc:
        record['error']=type(exc).__name__+':'+str(exc)
        write(out/'failed.json', record)
    finally:
        for sig in (signal.SIGINT, signal.SIGTERM): signal.signal(sig, signal.SIG_IGN)
        record['cleanup']=processes.close(seconds=10)
        if any(not row['reaped'] or row.get('cleanupError') for row in record['cleanup']): record['status']='FAIL'
        shutil.rmtree(scratch)
        record['scratchRemoved']=not scratch.exists()
        write(out/'result.json', record)
    return 0 if record['status']=='PASS' else 1


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=['local','allocated','rank'])
    parser.add_argument('manifest', type=Path)
    parser.add_argument('--rank', type=int, choices=[0,1])
    args=parser.parse_args()
    manifest=args.manifest.resolve()
    if args.mode=='rank': return rank(manifest, args.rank)
    config=json.loads(manifest.read_text())
    if args.mode=='allocated':
        if int(os.environ.get('SLURM_JOB_NUM_NODES','0')) != 2: raise RuntimeError('requires two allocated nodes')
        command=['srun','--kill-on-bad-exit=1','--ntasks=2','--ntasks-per-node=1',
                 '/bin/bash','-c','exec /usr/bin/python3 "$1" rank "$2" --rank "$SLURM_PROCID"',
                 'ndn-smoke',str(Path(__file__).resolve()),str(manifest)]
        code=subprocess.run(command, timeout=220).returncode
    else:
        if not config['local']: raise RuntimeError('local manifest required')
        children=[subprocess.Popen([sys.executable,__file__,'rank',str(manifest),'--rank',str(i)]) for i in (0,1)]
        try: code=max(p.wait(timeout=120) for p in children)
        finally:
            for child in children:
                if child.poll() is None: child.terminate()
            for child in children:
                try: child.wait(timeout=15)
                except subprocess.TimeoutExpired: child.kill(); child.wait(timeout=5)
    results=[json.loads((manifest.parent/('rank'+str(i))/'result.json').read_text()) for i in (0,1)]
    verdict=dict(status='PASS' if code==0 and all(r['status']=='PASS' for r in results) else 'FAIL',
                 scope='NDN_TRANSPORT_DIAGNOSTIC', results=results)
    write(manifest.parent/'result.json', verdict)
    print(json.dumps({'status':verdict['status'],'scope':verdict['scope'],'output':str(manifest.parent)}))
    return 0 if verdict['status']=='PASS' else 1


if __name__=='__main__': raise SystemExit(main())
