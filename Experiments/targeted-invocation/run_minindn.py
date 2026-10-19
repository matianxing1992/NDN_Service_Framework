#!/usr/bin/env python3
"""Real MiniNDN orchestration; only the business handler is simulated."""
import argparse
import base64
from datetime import datetime, timezone
import fcntl
import hashlib
import json
import os
import pwd
from pathlib import Path
import random
import re
import shutil
import signal
import subprocess
import time

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
USER = '/example/hello/user'
CONTROLLER = '/example/hello/controller'
TARGET = '/example/hello/provider/A'
OTHER = '/example/hello/provider/B'
GROUP = '/example/hello/group'
TRUST_ROOT = '/ndnsf-experiment-root'
NODE_IDENTITIES = {
    'gs': [CONTROLLER, USER, '/muas/gs1'],
    'drone1': ['/example/hello/provider', TARGET, '/muas/drone1'],
    'drone2': [OTHER, '/example/hello/provider/C', '/muas/drone2'],
}


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def save(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n')


def records(path):
    return [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []


def candidate_identity(policy=None, trust=None, tls=None, seed=None):
    """Exact inputs that must stay constant across paired cells."""
    if seed is None or trust is None:
        raise RuntimeError('fixed-root candidate requires prepared identities and trust schema; '
                           'use run_minindn.py until this caller is migrated')
    binaries = ['/usr/local/bin/spec203-sim-' + role for role in ['requester', 'provider', 'probe']]
    binaries.append('/usr/local/bin/App_ServiceController')
    for role in ['consumer', 'producer']:
        path = '/usr/local/bin/spec203-nsc-' + role
        if Path(path).exists():
            binaries.append(path)
    library_hashes = {}
    for binary in binaries:
        closure = subprocess.check_output(['ldd', binary], text=True,
                                         env=dict(os.environ, LD_LIBRARY_PATH=''))
        if 'not found' in closure or str(REPO) in closure:
            raise RuntimeError('invalid installed closure')
        for word in closure.split():
            if word.startswith('/') and Path(word).is_file():
                library_hashes[str(Path(word).resolve())] = digest(word)
    sources = list(HERE.glob('*.cpp')) + list(HERE.glob('*.hpp')) + list(HERE.glob('*.py')) + [HERE / 'wscript']
    baseline_sources = [REPO / 'Experiments/NDN_NSC' / (role + '.cpp') for role in ['consumer', 'producer']]
    baseline_sources += [REPO / 'Experiments/gRPC' / name for name in ['helloworld.proto', 'helloworld_pb2.py', 'helloworld_pb2_grpc.py']]
    baseline_sources.append(REPO / 'Experiments/gRPC_memphis_ucla_latency.py')
    python_runtime = {}
    if tls:
        import grpc
        import grpc._cython.cygrpc
        import google.protobuf
        import sys
        python_runtime = {'python': digest(sys.executable), 'grpc_version': grpc.__version__,
                          'protobuf_version': google.protobuf.__version__,
                          'grpc_native': digest(grpc._cython.cygrpc.__file__),
                          'tls_certificates': {p.name: digest(p) for p in tls.glob('*.crt')}}
    return {'binaries': {p: digest(p) for p in binaries}, 'libraries': library_hashes,
            'python_runtime': python_runtime,
            'sources': {p.name: digest(p) for p in sources},
            'baseline_sources': {str(p.relative_to(REPO)): digest(p) for p in baseline_sources},
            'headers': {name: digest('/usr/local/include/ndn-service-framework/' + name)
                for name in ['ServiceUser.hpp', 'ServiceProvider.hpp', 'NDNSFMessages.hpp']},
            'policy': digest(policy or HERE / 'hello.policies'),
            'trust': digest(trust or REPO / 'examples/trust-schema.conf'),
            'ndn_trust_root': certificate_info(seed / 'root.cert'),
            'ndn_certificates': {p.name: digest(p) for p in sorted(seed.glob('leaf-*.cert'))},
            'ndn_runtime_signing': json.loads((seed / 'complete.json').read_text()).get(
                'runtime_signing', 'RSA'),
            'contract': digest(REPO / 'specs/203-targeted-invocation-evaluation/contracts/experiment.md'),
            'inherited_environment': {k: v for k, v in os.environ.items()
                if k.startswith(('NDNSF_', 'NDN_')) and k != 'NDNSF_CONFIG'}}


def wait_marker(proc, log, marker, seconds=50):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise RuntimeError('child exited before readiness: ' + str(log))
        if log.exists() and marker in log.read_text(errors='replace'):
            return
        time.sleep(.1)
    raise RuntimeError('readiness timeout: ' + str(log))


def stop(proc):
    if proc.poll() is None:
        proc.send_signal(signal.SIGINT)
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)
            raise RuntimeError('child required forced kill')
    return proc.returncode


def keychain_environment(home):
    return dict(os.environ, HOME=str(home),
                NDN_CLIENT_PIB='pib-sqlite3:' + str(home / '.ndn'),
                NDN_CLIENT_TPM='tpm-file:' + str(home / '.ndn/ndnsec-key-file'))


def ndnsec(home, arguments, stdin=None):
    return subprocess.check_output(['ndnsec'] + arguments, input=stdin,
                                   env=keychain_environment(home))


def certificate_info(path):
    from ndn.app_support.security_v2 import parse_certificate
    from ndn.encoding import Name
    certificate = parse_certificate(base64.b64decode(path.read_bytes()))
    validity = certificate.signature_info.validity_period
    if validity is None:
        raise RuntimeError('certificate has no validity period: ' + str(path))
    before = datetime.strptime(bytes(validity.not_before).decode(), '%Y%m%dT%H%M%S')
    after = datetime.strptime(bytes(validity.not_after).decode(), '%Y%m%dT%H%M%S')
    if not before <= datetime.now(timezone.utc).replace(tzinfo=None) <= after:
        raise RuntimeError('certificate is outside its validity period: ' + str(path))
    return {'identity': Name.to_str(certificate.name[:-4]),
            'certificate': Name.to_str(certificate.name),
            'signer': Name.to_str(certificate.signature_info.key_locator.name),
            'sha256': digest(path)}


def issue_certificate(seed, home, identity, path, *, key=None):
    request = ndnsec(home, ['sign-req'] + (['-k', key] if key else [identity]))
    # Use an exact second so ndn-cxx does not round NotBefore into the future.
    not_before = datetime.now(timezone.utc).strftime('%Y%m%d%H%M%S')
    signed = ndnsec(seed / 'authority', ['cert-gen', '-s', TRUST_ROOT,
                                       '-i', 'experiment', '-S', not_before, '-'], request)
    path.write_bytes(signed)
    ndnsec(home, ['cert-install', '-K' if key else '-I', '-f', str(path)])
    dumped = ndnsec(home, ['cert-dump', '-k', key] if key else ['cert-dump', '-i', identity])
    if base64.b64decode(dumped) != base64.b64decode(signed):
        raise RuntimeError('root-signed certificate was not installed as default: ' + identity)


def seed_keys(root, *, fixed_root=False, runtime_signing=False):
    """Keep one issuer and separate reusable actor keychains in this workspace."""
    if not fixed_root:
        raise RuntimeError('caller must migrate to fixed-root, per-node keychains before running; '
                           'the legacy shared-key seed is no longer supported')
    seed = root / 'identities'
    seed.mkdir(mode=0o700, parents=True, exist_ok=True)
    if (seed / '.ndn').exists():
        raise RuntimeError('legacy shared private keys exist; preserve them and use a new stable workspace')
    authority = seed / 'authority'
    authority.mkdir(mode=0o700, exist_ok=True)
    root_certificate = seed / 'root.cert'
    complete = seed / 'complete.json'
    previous = json.loads(complete.read_text()) if complete.exists() else None
    signing_type = 'ECDSA' if runtime_signing else 'RSA'
    if previous and previous.get('runtime_signing', 'RSA') != signing_type:
        raise RuntimeError('workspace signing profile changed; use a new stable workspace')
    if runtime_signing and not previous and root_certificate.exists():
        raise RuntimeError('unrecorded existing root cannot adopt a new signing profile; '
                           'use a new stable workspace')
    if previous and (previous.get('owners') != NODE_IDENTITIES or not root_certificate.exists()):
        raise RuntimeError('fixed-root identity metadata is incomplete or changed')
    if not root_certificate.exists():
        listing = ndnsec(authority, ['list']).decode().split()
        if TRUST_ROOT not in listing:
            ndnsec(authority, ['key-gen', '-t', 'r', TRUST_ROOT])
        root_certificate.write_bytes(ndnsec(authority, ['cert-dump', '-i', TRUST_ROOT]))
    root_info = certificate_info(root_certificate)
    if root_info['identity'] != TRUST_ROOT or previous and previous['root'] != root_info:
        raise RuntimeError('fixed experiment root changed; refusing to replace the trust anchor')
    if base64.b64decode(ndnsec(authority, ['cert-dump', '-i', TRUST_ROOT])) != base64.b64decode(root_certificate.read_bytes()):
        raise RuntimeError('authority default certificate differs from the pinned root')
    index = 0
    for node, identities in NODE_IDENTITIES.items():
        home = seed / node
        home.mkdir(mode=0o700, exist_ok=True)
        listing = ndnsec(home, ['list']).decode().split()
        for identity in identities:
            path = seed / ('leaf-' + str(index) + '.cert')
            index += 1
            if identity not in listing:
                if path.exists() or previous:
                    raise RuntimeError('owned identity missing from its keychain: ' + identity)
                ndnsec(home, ['key-gen', '-n', '-t', 'r', identity])
            if not path.exists():
                if previous:
                    raise RuntimeError('pinned leaf certificate missing: ' + str(path))
                issue_certificate(seed, home, identity, path)
            info = certificate_info(path)
            root_key = root_info['certificate'].rsplit('/', 2)[0]
            if info['identity'] != identity or info['signer'] not in (root_key, root_info['certificate']):
                raise RuntimeError('leaf certificate is not issued by the fixed experiment root: ' + identity)
            ndnsec(home, ['cert-install', '-I', '-f', str(path)])
            if runtime_signing:
                from Cryptodome.PublicKey import ECC, RSA
                from ndn.encoding import Name, parse_data
                RSA.import_key(bytes(parse_data(base64.b64decode(path.read_bytes()))[2]))
                ec_path = path.with_name(path.stem + '-ecdsa.cert')
                if not ec_path.exists():
                    if previous:
                        raise RuntimeError('pinned signing certificate missing: ' + str(ec_path))
                    request = ndnsec(home, ['key-gen', '-n', '-t', 'e', identity])
                    ec_name = parse_data(base64.b64decode(request))[0]
                    issue_certificate(seed, home, identity, ec_path, key=Name.to_str(ec_name[:-2]))
                ec_info = certificate_info(ec_path)
                ECC.import_key(bytes(parse_data(base64.b64decode(ec_path.read_bytes()))[2]))
                if (ec_info['identity'] != identity or
                        ec_info['signer'] not in (root_key, root_info['certificate'])):
                    raise RuntimeError('signing certificate is not issued by the fixed root: ' + identity)
                ndnsec(home, ['cert-install', '-K', '-f', str(ec_path)])
        ndnsec(home, ['set-default', {'gs': USER, 'drone1': TARGET, 'drone2': OTHER}[node]])
    save(complete, {'owners': NODE_IDENTITIES, 'key_type': 'RSA',
                    'runtime_signing': signing_type, 'root': root_info})
    for node in NODE_IDENTITIES:
        reconcile_keys(seed, seed / node)
    return seed


def prepare_trust_schema(seed, path, svs_group='spec203', *, additional_leaves=(),
                         svs_participants=None):
    """Use the existing Data rules; the root may issue only the enumerated leaves."""
    template = (REPO / 'examples/trust-schema.conf').read_text()
    anchor_pattern = r'trust-anchor\s*\{[^{}]*\}'
    anchors = re.findall(anchor_pattern, template)
    if len(anchors) != 1 or not re.search(r'\btype\s+file\b', anchors[0]):
        raise RuntimeError('the example trust schema must contain one file trust anchor')
    anchor = 'trust-anchor\n{\n  type file\n  file-name ' + json.dumps(str(seed / 'root.cert')) + '\n}'
    template = re.sub(anchor_pattern, lambda _: anchor, template)
    leaves = [identity for identities in NODE_IDENTITIES.values() for identity in identities]
    if any(not isinstance(identity, str) or not re.fullmatch(r'/[A-Za-z0-9_/-]+', identity)
           for identity in additional_leaves):
        raise ValueError('invalid additional experiment leaf identity')
    leaves = list(dict.fromkeys(leaves + list(additional_leaves)))
    participants = [USER, TARGET, OTHER] if svs_participants is None else list(svs_participants)
    if (not participants or len(set(participants)) != len(participants)
            or any(identity not in leaves for identity in participants)):
        raise ValueError('SVS participants must be distinct declared experiment leaves')
    rules = []
    for identity in leaves:
        pattern = ''.join('<' + re.escape(part) + '|8=' + re.escape(part) + '>'
                          for part in identity.strip('/').split('/'))
        rules.append('''rule
{
  id "Experiment leaf %s"
  for data
  filter
  {
    type name
    regex ^%s<KEY|8=KEY><><><>$
  }
  checker
  {
    type customized
    sig-type rsa-sha256
    key-locator
    {
      type name
      name %s
      relation equal
    }
  }
}
''' % (identity, pattern, TRUST_ROOT))
    # ndn-svs owns this group/version StateVector namespace. This exception
    # admits only its declared participants, without broadening application Data.
    sync_checkers = []
    for identity in participants:
        for signature_type in ['rsa-sha256', 'ecdsa-sha256']:
            sync_checkers.append('''  checker
  {
    type customized
    sig-type %s
    key-locator
    {
      type name
      name %s
      relation equal
    }
  }
''' % (signature_type, identity))
    if svs_group not in ('spec203', 'spec204', 'spec204-permissions'):
        raise ValueError('unsupported experiment SVS group')
    rules.append('''rule
{
  id "Experiment SVS V3 group state"
  for data
  filter
  {
    type name
    regex ^<example|8=example><hello|8=hello><group|8=group><%s|8=%s><[0-9a-f]{16}|8=[0-9a-f]{16}><v=3|54=%%03>$
  }
%s}
''' % (svs_group, svs_group, ''.join(sync_checkers)))
    path.write_text('\n'.join(rules) + '\n' + template)


def prepare_tls(root):
    """Stable experiment CA and actor certificates; never print private keys."""
    directory = root / 'tls'
    directory.mkdir(exist_ok=True)
    complete = directory / 'complete.json'
    actors = ['gs', 'drone1', 'drone2']
    def openssl(arguments):
        subprocess.run(['openssl'] + arguments, cwd=directory, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if not complete.exists():
        if (directory / 'ca.key').exists() or (directory / 'ca.crt').exists():
            raise RuntimeError('incomplete TLS CA; refusing to overwrite it')
        openssl(['req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', 'ca.key',
                 '-out', 'ca.crt', '-subj', '/CN=Spec203 Experiment CA', '-days', '30'])
    recorded = json.loads(complete.read_text()).get('actors', []) if complete.exists() else []
    for actor in actors:
        if actor in recorded and all((directory / (actor + suffix)).is_file()
                                     for suffix in ('.key', '.crt')):
            continue
        if (directory / (actor + '.key')).exists() or (directory / (actor + '.crt')).exists():
            raise RuntimeError('incomplete TLS actor: ' + actor)
        openssl(['req', '-newkey', 'rsa:2048', '-nodes', '-keyout', actor + '.key',
                 '-out', actor + '.csr', '-subj', '/CN=' + actor])
        (directory / (actor + '.ext')).write_text(
            'subjectAltName=DNS:' + actor + '\nextendedKeyUsage=serverAuth,clientAuth\n')
        openssl(['x509', '-req', '-in', actor + '.csr', '-CA', 'ca.crt', '-CAkey', 'ca.key',
                 '-CAcreateserial', '-out', actor + '.crt', '-days', '30', '-extfile', actor + '.ext'])
    for path in directory.glob('*.key'):
        path.chmod(0o600)
    for actor in actors:
        openssl(['verify', '-CAfile', 'ca.crt', actor + '.crt'])
    save(complete, {'actors': actors})
    return directory


def reconcile_keys(seed, home):
    """Install peers' public certificates, never their private keys, before launch."""
    from ndn.encoding import Name, parse_data
    from ndn.security.keychain.keychain_sqlite3 import KeychainSqlite3
    from ndn.security.tpm.tpm_file import TpmFile
    if home != seed / home.name or home.name not in NODE_IDENTITIES:
        raise RuntimeError('fixed-root keychains must use their prepared per-node home')
    # ndn-cxx appends ndnsec-key-file to an explicit tpm-file locator;
    # python-ndn's TpmFile instead expects the final directory itself.
    directory = home / '.ndn/ndnsec-key-file/ndnsec-key-file'
    before = {p.name for p in directory.glob('*.privkey')}
    pib = KeychainSqlite3(str(home / '.ndn/pib.db'), TpmFile(str(directory)))
    certificates = sorted(seed.glob('leaf-*.cert'))
    runtime_signing = json.loads((seed / 'complete.json').read_text()).get(
        'runtime_signing', 'RSA') == 'ECDSA'
    try:
        for path in certificates:
            wire = base64.b64decode(path.read_bytes())
            name, _, content, _ = parse_data(wire)
            identity, key = name[:-4], name[:-2]
            if (runtime_signing and Name.to_str(identity) in NODE_IDENTITIES[home.name] and
                    hashlib.sha256(Name.to_bytes(key)).hexdigest() + '.privkey' not in before):
                raise RuntimeError('owned certificate key has no private key: ' + Name.to_str(key))
            if identity not in pib:
                pib.new_identity(identity)
            public_identity = pib[identity]
            key_wire = Name.to_bytes(key)
            try:
                public_key = public_identity[key]
                if bytes(public_key.key_bits) != bytes(content):
                    raise RuntimeError('public key content changed under an existing name')
            except KeyError:
                pib.conn.execute('INSERT INTO keys (identity_id,key_name,key_bits) VALUES (?,?,?)',
                                 (public_identity.row_id, key_wire, bytes(content)))
                pib.conn.commit()
            if name not in public_identity[key]:
                pib.import_cert(key_wire, Name.to_bytes(name), wire)
        allowed = {hashlib.sha256(Name.to_bytes(key)).hexdigest() + '.privkey'
                   for identity in NODE_IDENTITIES[home.name] for key in pib[identity]}
        if not before <= allowed:
            raise RuntimeError('node keychain contains a private key owned by another actor')
        for identity in NODE_IDENTITIES[home.name]:
            key = pib[identity].default_key().name
            if hashlib.sha256(Name.to_bytes(key)).hexdigest() + '.privkey' not in before:
                raise RuntimeError('owned default key has no private key: ' + identity)
    finally:
        pib.shutdown()
    if before != {p.name for p in directory.glob('*.privkey')}:
        raise RuntimeError('public certificate installation changed private keys')
    for path in certificates:
        ndnsec(home, ['cert-install', '-K', '-f', str(path)])
        info = certificate_info(path)
        key = Name.to_str(parse_data(base64.b64decode(path.read_bytes()))[0][:-2])
        if base64.b64decode(ndnsec(home, ['cert-dump', '-k', key])) != base64.b64decode(path.read_bytes()):
            raise RuntimeError('node public certificate differs from the issued certificate: ' + key)
        # NDNSF selects its existing EC signing key separately from RSA NAC-ABE.
        # NSC signs and pins the identity default, so both peers select the EC key.
        ec_default = runtime_signing and info['identity'].startswith('/muas/')
        if path.name.endswith('-ecdsa.cert') == ec_default:
            ndnsec(home, ['cert-install', '-I', '-f', str(path)])
            if base64.b64decode(ndnsec(home, ['cert-dump', '-i', info['identity']])) != base64.b64decode(path.read_bytes()):
                raise RuntimeError('identity default certificate differs from its role: ' + info['identity'])


def run_cell(args, parser):
    from mininet.topo import Topo
    from minindn.minindn import Minindn
    from minindn.apps.app_manager import AppManager
    from minindn.apps.nfd import Nfd
    from minindn.helpers.ndn_routing_helper import NdnRoutingHelper
    from minindn.helpers.nfdc import Nfdc
    from minindn.util import getPopen

    root = Path(args.workspace).resolve()
    root.mkdir(parents=True, exist_ok=True)
    lock = (root / '.active.lock').open('a')
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    output = Path(args.output_dir).resolve()
    output.mkdir(parents=True, exist_ok=False)
    policy = output / 'hello.policies'
    trust = output / 'trust-schema.conf'
    shutil.copy2(HERE / 'hello.policies', policy)
    seed = seed_keys(root, fixed_root=True, runtime_signing=bool(args.wired_rate_rps))
    prepare_trust_schema(seed, trust)
    tls = prepare_tls(root) if args.four_way else None
    candidate = candidate_identity(policy, trust, tls, seed)
    if args.expected_candidate:
        expected = json.loads(Path(args.expected_candidate).read_text())['candidate']
        if candidate != expected:
            raise RuntimeError('candidate mismatch before MiniNDN/business traffic')
    node_names = {'gs': 'gs', 'drone1': 'drone1', 'drone2': 'drone2'}
    topology = None
    if args.wired_rate_rps:
        topology = REPO / 'Experiments/Topology/testbed(loss=0%).conf'
        node_names = {'gs': 'memphis', 'drone1': 'ucla'}
        ndn = Minindn(parser=parser, topoFile=str(topology), workDir=str(root / 'nodes'))
    else:
        topo = Topo()
        for name in ('gs', 'relay', 'drone1', 'drone2'):
            topo.addHost(name)
        for left, right in [('gs', 'relay'), ('relay', 'drone1'), ('relay', 'drone2')]:
            topo.addLink(left, right, delay=str(args.delay_ms) + 'ms',
                         loss=args.loss, bw=1000)
        ndn = Minindn(parser=parser, topo=topo, workDir=str(root / 'nodes'))
    provider_roles = [('drone1', TARGET)]
    if not args.wired_rate_rps:
        provider_roles.append(('drone2', OTHER))

    def actor_node(role):
        return ndn.net[node_names[role]]
    if ndn.args.resultDir:
        raise ValueError('--result-dir would move reusable node state')
    children = []
    nfd_manager = None
    controller = None
    generation_lock_contents = None
    generation_state = root / 'controller-generation.state'
    generation_lock = Path(str(generation_state) + '.lock')
    mono_before, wall, mono_after = time.monotonic_ns(), time.time_ns(), time.monotonic_ns()
    manifest = {'schema': 'ndnsf-spec203-run-v1', 'status': 'PARTIAL',
                'candidate': candidate,
                'smoke': args.smoke, 'delay_ms': args.delay_ms, 'loss_percent': args.loss,
                'block_seed': args.seed, 'window_seconds': 45 if args.recovery_only else 3 if args.smoke else 60,
                'warmup_seconds': 0 if args.smoke or args.recovery_only else 10, 'timeout_ms': 5000,
                'business_delay_ms': 5, 'payload_bytes': 64,
                'clock_anchor': {'wall_ns': wall, 'mono_ns': (mono_before + mono_after) // 2,
                                 'uncertainty_ns': (mono_after - mono_before) // 2},
                'trust_profile': 'one fixed experiment root; root-issued actor certificates; '
                                 'existing application rules; per-node private keys',
                'runs': [], 'children': [], 'identities': [USER, CONTROLLER, TARGET, OTHER]}
    if args.wired_rate_rps:
        manifest['trust_profile'] += '; RSA authorization and ECDSA runtime signing; NSC EC default pin'
    if topology:
        manifest.update({'topology': str(topology), 'topology_sha256': digest(topology),
                         'role_nodes': node_names, 'provider_count': 1,
                         'rate_rps': args.wired_rate_rps, 'delay_ms': None,
                         'workload_mode': 'open-loop'})
    binaries = ['/usr/local/bin/spec203-sim-requester', '/usr/local/bin/spec203-sim-provider',
                '/usr/local/bin/spec203-sim-probe',
                '/usr/local/bin/App_ServiceController', '/usr/local/lib/libndn-service-framework.so']
    manifest['sha256'] = {path: digest(path) for path in binaries}
    manifest['source_sha256'] = {str(path.relative_to(REPO)): digest(path)
        for path in [HERE / n for n in ['requester.cpp', 'provider.cpp', 'common.hpp',
                                       'probe.cpp', 'wscript', 'run_minindn.py']]}
    manifest['config_sha256'] = {path.name: digest(path) for path in [policy, trust]}
    manifest['inherited_ndnsf_environment'] = {key: value for key, value in os.environ.items()
                                              if key.startswith(('NDNSF_', 'NDN_'))}
    loader_env = dict(os.environ, LD_LIBRARY_PATH='')
    manifest['installed_closure'] = {}
    manifest['loaded_library_sha256'] = {}
    for binary in binaries:
        if '/bin/' not in binary:
            continue
        closure = subprocess.check_output(['ldd', binary], env=loader_env, text=True)
        if 'not found' in closure or str(REPO) in closure:
            raise RuntimeError('invalid installed library closure: ' + binary)
        manifest['installed_closure'][binary] = closure
        for line in closure.splitlines():
            for word in line.split():
                if word.startswith('/') and Path(word).is_file():
                    manifest['loaded_library_sha256'][str(Path(word).resolve())] = digest(word)
    manifest['contract_sha256'] = digest(REPO / 'specs/203-targeted-invocation-evaluation/contracts/experiment.md')
    git_command = ['git', '-C', str(REPO)]
    if os.geteuid() == 0 and REPO.stat().st_uid != 0:
        owner = pwd.getpwuid(REPO.stat().st_uid).pw_name
        git_command = ['runuser', '-u', owner, '--', *git_command]
    manifest['git_head'] = subprocess.check_output(
        [*git_command, 'rev-parse', 'HEAD'], text=True).strip()
    (output / 'tracked-diff.patch').write_bytes(
        subprocess.check_output([*git_command, 'diff', 'HEAD']))
    manifest['installed_header_sha256'] = {name: digest('/usr/local/include/ndn-service-framework/' + name)
        for name in ['ServiceUser.hpp', 'ServiceProvider.hpp', 'NDNSFMessages.hpp']}

    def launch(node, label, command, extra_env=None):
        log = output / (label + '.log')
        handle = log.open('wb')
        default_level = 'ERROR' if args.four_way and label != 'controller' else 'INFO'
        log_level = args.actor_log_level or default_level
        log_config = 'ndn_service_framework.*=' + log_level
        if args.actor_log_level is not None:
            log_config += ':ndn_svs.*=' + log_level
        env = {'NDN_LOG': log_config,
               'LD_LIBRARY_PATH': '',
               'NDNSF_CONFIG': str(Path(node.params['params']['homeDir']) / 'ndnsf.conf')}
        if args.wired_rate_rps:
            # Size publication concurrency for the offered open-loop workload.
            # Keep the protocol's normal suppression interval.
            env['NDNSF_SVS_EXPECTED_RPS'] = str(args.wired_rate_rps)
        role = next((role for role, name in node_names.items() if name == node.name), None)
        if role in NODE_IDENTITIES:
            key_env = keychain_environment(seed / role)
            env.update({key: key_env[key] for key in ['NDN_CLIENT_PIB', 'NDN_CLIENT_TPM']})
        env.update(extra_env or {})
        # Diagnostic overrides are retained separately from timing campaign samples.
        manifest['diagnostic_logging'] = args.actor_log_level is not None
        if label == 'controller':
            env['NDNSF_CONTROLLER_GENERATION_STATE'] = str(generation_state)
        manifest.setdefault('actor_environment', {})[label] = env
        manifest.setdefault('actor_commands', {})[label] = list(command)
        proc = getPopen(node, command, envDict=env, stdout=handle, stderr=subprocess.STDOUT)
        children.append((label, proc, handle))
        return proc, log

    def provider_for(label, group, name, identity):
        actor = name + '-' + label
        timeline = not args.wired_rate_rps or args.actor_log_level is not None
        proc, log = launch(actor_node(name), actor, ['/usr/local/bin/spec203-sim-provider',
            '--trust', str(trust), '--identity', identity, '--group', group,
            '--records', str(output / (actor + '.jsonl')), '--delay-ms', '5',
            '--timeline-trace', '1' if timeline else '0'])
        manifest.setdefault('provider_records', []).append(
            {'label': actor, 'records': actor + '.jsonl', 'group': group, 'identity': identity})
        wait_marker(proc, log, 'SPEC203_PROVIDER_READY')
        return proc

    def providers_for(label):
        # Equal-length group components keep basic/targeted wire naming matched.
        group = GROUP + '/spec203/' + hashlib.sha256(label.encode()).hexdigest()[:16]
        owners = []
        for name, identity in provider_roles:
            owners.append(provider_for(label, group, name, identity))
        return group, owners

    def client(run_id, mode, cold=False, purpose='performance', payload_bytes=64, scope=None,
               recovery=False):
        own_scope = scope is None
        group, owners = providers_for(run_id) if own_scope else scope
        command = ['/usr/local/bin/spec203-sim-requester', '--trust', str(trust),
                   '--records', str(output / (run_id + '.jsonl')), '--run-id', run_id,
                   '--group', group,
                   '--mode', mode, '--warmup-ms', '0' if cold or args.smoke or recovery else '10000',
                   '--window-ms', '45000' if recovery else '0' if cold else ('3000' if args.smoke else '60000'),
                   '--count', '1' if cold else '0', '--timeout-ms', '5000',
                   '--interval-ms', '1000' if recovery else '0',
                   '--payload-bytes', str(payload_bytes)]
        if args.wired_rate_rps:
            # Keep response/execution records, but do not construct detailed
            # internal trace fields in a timing-only campaign.
            command += ['--rate-rps', str(args.wired_rate_rps), '--timeline-trace',
                        '1' if args.actor_log_level is not None else '0']
        if mode in ('all', 'all-raw', 'single'):
            command += ['--other', OTHER]
        if args.discovery_payload is not None:
            command += ['--discovery-payload', args.discovery_payload]
        try:
            proc, log = launch(actor_node('gs'), run_id, command)
            wait_marker(proc, log, 'SPEC203_REQUESTER_READY')
            if recovery:
                # Observe the existing C++ oracles while the same User stays
                # alive. Never inject boot hints, tokens or cache entries.
                deadline = time.monotonic() + 15
                while time.monotonic() < deadline:
                    if proc.poll() is not None:
                        raise RuntimeError('requester exited before recovery cut')
                    try:
                        completed = {r['command'] for r in records(output / (run_id + '.jsonl'))
                                     if r.get('event') == 'terminal' and r.get('terminal') == 'success'}
                        warm = [r for r in records(output / ('drone1-' + run_id + '.jsonl'))
                                if r.get('event') == 'execution' and r.get('actual_mode') == 1
                                and r['command'] in completed]
                    except json.JSONDecodeError:
                        # A live final line may not yet have finished its write.
                        time.sleep(.1)
                        continue
                    if len(warm) >= 3:
                        break
                    time.sleep(.1)
                else:
                    raise RuntimeError('no three verified warm executions before restart')
                config = Path(ndn.net['drone1'].params['params']['homeDir']) / 'ndnsf.conf'
                def session():
                    matches = [line.split(',')[2] for line in config.read_text().splitlines()
                               if line.split(',')[:2] == [group, TARGET + '/provider']]
                    if len(matches) != 1:
                        raise RuntimeError('missing unique persistent Provider session')
                    return int(matches[0])
                recovery_row = {'requester_pid': proc.pid, 'old_provider_pid': owners[0].pid,
                    'stopped_ns': time.monotonic_ns(), 'old_session': session(),
                    'sync_group': group, 'credentials_unchanged': not args.rotate_certificate,
                    'certificate_rotation': args.rotate_certificate, 'interval_ms': 1000}
                manifest['recovery'] = recovery_row
                save(output / 'manifest.json', manifest)
                stop(owners[0])
                if args.rotate_certificate:
                    home = seed / 'drone1'
                    ndnsec(home, ['key-gen', '-t', 'r', TARGET])
                    target_index = [identity for identities in NODE_IDENTITIES.values()
                                    for identity in identities].index(TARGET)
                    certificate = seed / ('leaf-' + str(target_index) + '.cert')
                    issue_certificate(seed, home, TARGET, certificate)
                    recovery_row['new_certificate'] = certificate_info(certificate)
                replacement = provider_for(run_id + '-restart', group, 'drone1', TARGET)
                owners.append(replacement)
                recovery_row.update({'new_provider_pid': replacement.pid,
                    'ready_ns': time.monotonic_ns(), 'new_session': session()})
                save(output / 'manifest.json', manifest)
                if recovery_row['new_session'] != recovery_row['old_session'] + 1:
                    raise RuntimeError('Provider did not increment its persistent SVS session')
            proc.wait(timeout=15 if cold or args.smoke else 80)
            if proc.returncode:
                raise RuntimeError('requester failed: ' + str(log))
        finally:
            if own_scope:
                for owner in reversed(owners):
                    stop(owner)
        row = {'id': run_id, 'arm': 'ALL_RESPONDERS' if mode in ('all', 'all-raw') else
               'SINGLE_OF_TWO' if mode == 'single' else
               'TARGETED_FIRST_CALL' if cold else
               ('BASIC' if mode == 'basic' else 'TARGETED_STEADY'), 'records': run_id + '.jsonl',
               'returncode': proc.returncode, 'purpose': purpose, 'payload_bytes': payload_bytes,
               'sync_group': group}
        manifest['runs'].append(row)
        save(output / 'manifest.json', manifest)
        return records(output / row['records'])

    def baseline(mode, label):
        warmup, window = (0, 3000) if args.smoke else (10000, 60000)
        provider_records = label + '-provider.jsonl'
        if mode == 'nsc':
            provider, log = launch(actor_node('drone1'), label + '-provider',
                ['/usr/local/bin/spec203-nsc-producer', '/muas/drone1', '/FlightControl', '/ManualControl',
                 '6000' if args.baseline_check == 'nsc-deadline' else '5'],
                {'SPEC203_RECORDS': str(output / provider_records),
                 'SPEC203_BAD_ECHO': '1' if args.baseline_check == 'nsc-echo' else '0',
                 'SPEC203_BAD_SIGNER': '1' if args.baseline_check == 'nsc-signer' else '0'})
            wait_marker(provider, log, 'SPEC203_NSC_PROVIDER_READY')
            command = ['/usr/local/bin/spec203-nsc-consumer', '/muas/gs1', '/muas/drone1',
                       '/FlightControl', '/ManualControl', '1', '0', label, '0', '5000', '5000']
            extra = {'SPEC203_RECORDS': str(output / (label + '.jsonl')),
                     'SPEC203_DUPLICATE_NOTIFY': '1' if args.baseline_check == 'nsc-retry' else '0',
                     'SPEC203_WARMUP_MS': str(warmup), 'SPEC203_WINDOW_MS': str(window)}
            if args.wired_rate_rps:
                extra['SPEC203_RATE_RPS'] = str(args.wired_rate_rps)
            marker = 'SPEC203_NSC_READY'
        else:
            tls = prepare_tls(root)
            import sys
            sys.path.insert(0, str(REPO / 'Experiments'))
            from gRPC_memphis_ucla_latency import endpoint_ip_on_path, add_route_to_destination
            path = ['gs', 'relay', 'drone1']
            if topology:
                from gRPC_memphis_ucla_latency import parse_topology, shortest_path
                nodes, links = parse_topology(topology)
                path, path_delay = shortest_path(nodes, links, node_names['gs'], node_names['drone1'])
                manifest['ip_path'] = path
                manifest['ip_path_configured_delay_ms'] = path_delay
            server_ip = endpoint_ip_on_path(ndn, path)
            client_ip = endpoint_ip_on_path(ndn, list(reversed(path)))
            add_route_to_destination(ndn, path, server_ip)
            add_route_to_destination(ndn, list(reversed(path)), client_ip)
            base = [sys.executable, str(HERE / 'matched_grpc.py')]
            provider, log = launch(actor_node('drone1'), label + '-provider', base +
                ['server', '--address', server_ip + ':50051', '--key', str(tls / 'drone1.key'),
                 '--cert', str(tls / 'drone1.crt'), '--ca', str(tls / 'ca.crt'),
                 '--records', str(output / provider_records)] +
                 (['--bad-echo'] if args.baseline_check == 'grpc-echo' else []) +
                 (['--handler-ms', '6000'] if args.baseline_check == 'grpc-deadline' else []))
            wait_marker(provider, log, 'SPEC203_GRPC_PROVIDER_READY')
            command = base + ['client', '--address', server_ip + ':50051', '--key', str(tls / 'gs.key'),
                '--cert', str(tls / 'gs.crt'), '--ca', str(tls / 'ca.crt'), '--records', str(output / (label + '.jsonl')),
                '--run-id', label, '--warmup-ms', str(warmup), '--window-ms', str(window)]
            if args.wired_rate_rps:
                command += ['--rate-rps', str(args.wired_rate_rps)]
            extra, marker = {}, 'SPEC203_GRPC_READY'
            if args.baseline_check == 'grpc-client-identity':
                command[command.index('--cert') + 1] = str(tls / 'drone1.crt')
                command[command.index('--key') + 1] = str(tls / 'drone1.key')
            if args.baseline_check == 'grpc-target':
                command += ['--server-name', 'another-provider']
            manifest['tls_public_sha256'] = {p.name: digest(p) for p in tls.glob('*.crt')}
        try:
            proc, log = launch(actor_node('gs'), label, command, extra)
            wait_marker(proc, log, marker)
            proc.wait(timeout=15 if args.smoke else 80)
            if proc.returncode:
                raise RuntimeError(mode + ' requester failed: ' + str(log))
        finally:
            stop(provider)
        manifest['runs'].append({'id': label, 'arm': mode.upper(), 'purpose': 'performance',
            'records': label + '.jsonl', 'provider_records': provider_records,
            'returncode': proc.returncode, 'payload_bytes': 64,
            'security': 'mutual TLS' if mode == 'grpc' else 'signed NDN validated against pinned keys'})
        save(output / 'manifest.json', manifest)

    try:
        ndn.start()
        nfd_manager = AppManager(ndn, ndn.net.hosts, Nfd, logLevel='WARN')
        routing = NdnRoutingHelper(ndn.net, 'udp', 'link-state')
        routing.addOrigin([actor_node('gs')], [CONTROLLER, USER, GROUP])
        if args.four_way:
            routing.addOrigin([actor_node('gs')], ['/muas/gs1'])
            routing.addOrigin([actor_node('drone1')], ['/muas/drone1'])
        for name, identity in provider_roles:
            routing.addOrigin([actor_node(name)], [identity, GROUP])
        routing.calculateRoutes()
        for node in ndn.net.hosts:
            Nfdc.setStrategy(node, '/example/hello', Nfdc.STRATEGY_BEST_ROUTE)
            Nfdc.setStrategy(node, GROUP, Nfdc.STRATEGY_MULTICAST)
            # Installed SVS publishes content/mapping beneath its node ID;
            # those inherit /example/hello best-route. GROUP is the sync prefix.
            strategies, fib = node.cmd('nfdc strategy list'), node.cmd('nfdc fib list')
            (output / (node.name + '-routing.txt')).write_text(strategies + node.cmd('nfdc route list') +
                fib + node.cmd('nfdc face list') + node.cmd('tc qdisc show'))
            for prefix, strategy in [('/example/hello', 'best-route'), (GROUP, 'multicast')]:
                line = [line for line in strategies.splitlines() if ('prefix=' + prefix + ' ') in line]
                if len(line) != 1 or ('/strategy/' + strategy + '/') not in line[0]:
                    raise RuntimeError('strategy readback failed: ' + node.name + ' ' + prefix)
            for prefix, owner in ([(CONTROLLER, 'gs'), (USER, 'gs')] +
                                  [(identity, name) for name, identity in provider_roles]):
                entries = [line for line in fib.splitlines() if re.match(
                    r'^\s*' + re.escape(prefix) + r'\s+nexthops=\{faceid=\d+', line)]
                if node.name != node_names[owner] and not entries:
                    raise RuntimeError('missing FIB: ' + node.name + ' ' + prefix)
        controller, log = launch(actor_node('gs'), 'controller',
            ['/usr/local/bin/App_ServiceController', '--policy-file', str(policy),
             '--trust-schema', str(trust)])
        wait_marker(controller, log, 'ServiceController listening on:')
        if 'NDNSF_CONTROLLER_VERSION generation=' not in log.read_text(errors='replace'):
            raise RuntimeError('Controller generation unavailable; no business dispatch')
        generation_lock_contents = generation_lock.read_bytes()
        _, probe_providers = providers_for('rtt')
        probe, _ = launch(actor_node('gs'), 'rtt', ['/usr/local/bin/spec203-sim-probe',
                          '--trust', str(trust), '--records', str(output / 'rtt.jsonl')])
        probe.wait(timeout=55)
        if probe.returncode:
            raise RuntimeError('signed unique-name RTT probe failed')
        for owner in reversed(probe_providers):
            stop(owner)
        manifest['history_isolation'] = 'fresh-sync-group-and-ready-providers-per-performance-run'
        arms = ['basic', 'targeted']
        if args.four_way:
            arms += ['grpc', 'nsc']
            position = (args.seed - 20301) % 4
            arms = arms[position:] + arms[:position]
        if args.arms:
            arms = [mode for mode in arms if mode in args.arms]
        if args.baseline_check:
            if not args.four_way or not args.smoke:
                raise ValueError('baseline negative checks require --four-way --smoke')
            arms = [args.baseline_check.split('-')[0]]
        # Alternating assignment is balanced 3:2 across the five declared seeds.
        if args.seed % 2 and not args.four_way:
            arms.reverse()
        if args.recovery_only:
            arms = []
        if args.all_responders_smoke:
            arms = []
        manifest['arm_order'] = arms
        for mode in arms:
            label = 'b' + str(args.seed) + '-' + mode
            if args.wired_rate_rps:
                label += '-rps' + str(args.wired_rate_rps)
            if mode in ['grpc', 'nsc']:
                baseline(mode, label)
                continue
            capture = None
            if args.delay_ms == 10 and args.loss == 0 and not args.smoke and not args.four_way:
                interface = ndn.net['relay'].connectionsTo(ndn.net['drone1'])[0][0].name
                capture, log = launch(ndn.net['relay'], label + '-capture',
                    ['tcpdump', '-n', '-U', '-s', '0', '-i', interface, '-w',
                     str(output / (label + '.pcap')), 'ip'])
                wait_marker(capture, log, 'listening on', seconds=5)
                manifest.setdefault('capture', {})[label] = {
                    'interface': interface, 'filter': 'ip', 'endpoint': 'relay',
                    'postfilter': 'complete IPv4 UDP datagrams with port 6363',
                    'started_ns': time.monotonic_ns(), 'guard_ns': 250000000,
                    'directions': 'both', 'pcap': label + '.pcap'}
                time.sleep(.25)
            try:
                client(label, mode)
            finally:
                if capture:
                    time.sleep(.25)
                    manifest['capture'][label]['stopped_ns'] = time.monotonic_ns()
                    stop(capture)
        if args.all_responders_smoke:
            for label, mode in [('all-protected', 'all'), ('all-raw-protected', 'all-raw'),
                                ('single-protected', 'single')]:
                result = client(label, mode, cold=True, purpose='all-responders-qualification')
                terminal = [row for row in result if row.get('event') == 'terminal']
                if len(terminal) != 1 or terminal[0].get('terminal') != 'success':
                    raise RuntimeError(label + ': protected invocation did not complete')
                executions = {
                    identity: [row for row in records(output / (node + '-' + label + '.jsonl'))
                               if row.get('event') == 'execution']
                    for identity, node in [(TARGET, 'drone1'), (OTHER, 'drone2')]
                }
                command = terminal[0]['command']
                expected = (TARGET, OTHER) if mode in ('all', 'all-raw') else (TARGET,)
                publication_log = (output / (label + '.log')).read_text(errors='replace')
                published = re.findall(r'NDNSF_REQUEST_SCOPED_INPUT_PUBLISHED [^\n]*providerName=([^ ]+)',
                                       publication_log)
                if sorted(published) != sorted(expected):
                    raise RuntimeError(label + ': request-scoped input publication set mismatch')
                for identity in expected:
                    matches = executions[identity]
                    if len(matches) != 1 or not matches[0].get('payload_valid') or matches[0].get('command') != command:
                        raise RuntimeError(label + ': Provider input mismatch: ' + identity)
                if mode in ('all', 'all-raw'):
                    received = [row for row in result if row.get('event') == 'all_response']
                    if {row.get('provider') for row in received} != set(expected) or len(received) != 2 or any(
                            not row.get('payload_verified') or not row.get('identity_verified') for row in received):
                        raise RuntimeError('All-Responders did not verify both returned results')
                elif executions[OTHER]:
                    raise RuntimeError('nonselected Provider executed protected input')
            manifest['all_responders_qualification'] = {
                'selected': [TARGET, OTHER], 'nonselected_in_single': OTHER,
                'input_bytes': 64, 'verified_results': 2,
                'negative_oracle': 'nonselected Provider had no Selection key or execution'}
        manifest['measurement_family'] = ('all-responders-qualification' if args.all_responders_smoke else
            'descriptor-qualification'
            if args.discovery_payload is not None else 'diagnostic' if args.actor_log_level else
            'recovery' if args.recovery_only else 'four-way' if args.four_way else
            'wire-supplement' if args.wire_only else 'primary')
        if args.recovery_only:
            client('provider-recovery', 'targeted', purpose='recovery', recovery=True)
        if (args.loss == 0 and not args.wire_only and not args.four_way
                and not args.recovery_only and not args.all_responders_smoke):
            for sample in range(1 if args.smoke else 10):
                client('b' + str(args.seed) + '-cold-' + str(sample), 'targeted', cold=True)
        if args.discovery_payload is not None:
            # Binding parity supplements the native C++ oracles; it does not replace them.
            group, owners = providers_for('python-descriptor')
            code = '''
import hashlib,json,sys
from pathlib import Path
from ndnsf import ServiceUser,_ndnsf
from ndnsf import service as facade
payload=b"binding-private-input".ljust(64,b"_")
user=ServiceUser(group=sys.argv[1],trust_schema=sys.argv[2],permission_wait_ms=15000)
user.start()
try:
    first=user.request_service("/HELLO",payload,discovery_payload=sys.argv[3].encode())
    if not (first.status and first.payload==payload):
        raise RuntimeError("first descriptor request failed: "+first.error)
    rejected=user.request_service("/HELLO",payload,request_capabilities={"ApplicationMode":"explicit"},discovery_payload=b"")
    if rejected.status or "requires request-scoped confidentiality" not in rejected.error:
        raise RuntimeError("unprotected descriptor was not rejected: "+rejected.error)
    second=user.request_service_select("/HELLO",payload,lambda cs:[c.provider_name for c in cs if c.status][:1],discovery_payload=sys.argv[3].encode())
    if not (second.status and second.payload==payload):
        raise RuntimeError("descriptor runtime did not survive rejection: "+second.error)
    print("SPEC205_PYTHON_DESCRIPTOR_PASS "+json.dumps({"extension":_ndnsf.__file__,"extension_sha256":hashlib.sha256(Path(_ndnsf.__file__).read_bytes()).hexdigest(),"facade_sha256":hashlib.sha256(Path(facade.__file__).read_bytes()).hexdigest(),"invalid_call_rejected":True,"runtime_survived":True}),flush=True)
finally:
    user.stop()
'''
            try:
                proc, log = launch(ndn.net['gs'], 'python-descriptor',
                                   ['python3', '-c', code, group, str(trust),
                                    args.discovery_payload])
                proc.wait(timeout=90)
                text = log.read_text()
                if proc.returncode or 'SPEC205_PYTHON_DESCRIPTOR_PASS ' not in text:
                    raise RuntimeError('installed Python descriptor/runtime oracle failed')
                line = next(line for line in text.splitlines()
                            if line.startswith('SPEC205_PYTHON_DESCRIPTOR_PASS '))
                manifest['python_descriptor'] = json.loads(line.split(' ', 1)[1])
            finally:
                for owner in owners:
                    stop(owner)
        if args.smoke and not args.baseline_check and not args.all_responders_smoke and not args.wired_rate_rps:
            client('expected-error', 'targeted', cold=True, purpose='oracle-negative', payload_bytes=63)
        # Isolation positive/offline share only this separately owned group.
        if args.isolation or (args.smoke and not args.baseline_check and not args.all_responders_smoke
                              and not args.wired_rate_rps):
            scope = providers_for('isolation')
            positive = client('target-positive', 'targeted', cold=True, purpose='isolation', scope=scope)
            stop(scope[1][0])
            offline = client('target-offline', 'targeted', cold=True, purpose='isolation', scope=scope)
            stop(scope[1][1])
            manifest['isolation'] = {'positive': positive, 'offline': offline}
        manifest['status'] = 'RUN_COMPLETE'
    except Exception as error:
        manifest['first_failure'] = str(error)
        raise
    finally:
        for label, proc, handle in reversed(children):
            try:
                code = stop(proc)
                manifest['children'].append({'label': label, 'exit': code})
            except Exception as error:
                manifest['status'] = 'PARTIAL'
                manifest.setdefault('cleanup_errors', []).append(str(error))
            handle.close()
        try:
            if nfd_manager:
                for app in nfd_manager:
                    if app.process:
                        try:
                            code = stop(app.process)
                            manifest['children'].append({'label': app.node.name + '-nfd', 'exit': code})
                        except Exception as error:
                            manifest['status'] = 'PARTIAL'
                            manifest.setdefault('cleanup_errors', []).append(app.node.name + ': ' + str(error))
            # The existing Controller app does not handle SIGINT gracefully;
            # release only our exact lock after its PID is confirmed terminal.
            if controller and controller.poll() is not None and generation_lock_contents is not None:
                if generation_lock.exists():
                    if generation_lock.read_bytes() != generation_lock_contents:
                        raise RuntimeError('Controller writer ownership changed; preserve lock')
                    (output / 'controller-writer-release.lock').write_bytes(generation_lock_contents)
                    generation_lock.unlink()
                    manifest['controller_writer_released_after_wait'] = True
            if generation_state.exists():
                shutil.copy2(generation_state, output / 'controller-generation.state')
            for node in ndn.net.hosts:
                home = Path(node.params['params']['homeDir'])
                for filename in ['log/nfd.log', 'nfd.conf', '.ndn/client.conf', 'ndnsf.conf']:
                    if (home / filename).exists():
                        shutil.copy2(home / filename, output / (node.name + '-' + Path(filename).name))
        except Exception as error:
            manifest['status'] = 'PARTIAL'
            manifest.setdefault('cleanup_errors', []).append('archive: ' + str(error))
        finally:
            try:
                ndn.stop()
            except Exception as error:
                manifest['status'] = 'PARTIAL'
                manifest.setdefault('cleanup_errors', []).append('network stop: ' + str(error))
            finally:
                save(output / 'manifest.json', manifest)
                fcntl.flock(lock, fcntl.LOCK_UN)
                lock.close()
    print('SMOKE_COMPLETE' if args.smoke else 'CELL_COMPLETE', output, flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--workspace', default='/var/tmp/ndnsf-targeted-fixed-root')
    parser.add_argument('--output-dir', required=True)
    parser.add_argument('--delay-ms', type=int, choices=[1, 10, 50], default=10)
    parser.add_argument('--loss', type=float, choices=[0, 1], default=0)
    parser.add_argument('--seed', type=int, choices=range(20301, 20306), default=20301)
    parser.add_argument('--smoke', action='store_true')
    parser.add_argument('--all-responders-smoke', action='store_true')
    parser.add_argument('--wire-only', action='store_true')
    parser.add_argument('--four-way', action='store_true')
    parser.add_argument('--wired-rate-rps', type=int, choices=[10, 30, 50, 70],
                        help='one-provider Memphis--UCLA, zero-loss, fixed-rate four-way panel')
    parser.add_argument('--arms', nargs='+', choices=['basic', 'targeted', 'grpc', 'nsc'],
                        help='run only these arms when completing a wired-rate panel')
    parser.add_argument('--actor-log-level', choices=['ERROR', 'INFO', 'DEBUG', 'TRACE'],
                        help='diagnostic override; exclude this run from timing campaigns')
    parser.add_argument('--discovery-payload',
                        help='protected descriptor qualification input; requires --smoke')
    parser.add_argument('--baseline-check', choices=['nsc-echo', 'nsc-signer', 'nsc-retry', 'nsc-deadline', 'grpc-echo', 'grpc-client-identity', 'grpc-target', 'grpc-deadline'])
    parser.add_argument('--isolation', action='store_true')
    parser.add_argument('--recovery-only', action='store_true')
    parser.add_argument('--rotate-certificate', action='store_true')
    parser.add_argument('--expected-candidate')
    args, _ = parser.parse_known_args()
    if args.arms and (not args.wired_rate_rps or len(set(args.arms)) != len(args.arms)):
        parser.error('--arms requires --wired-rate-rps and distinct arm names')
    if args.wired_rate_rps and (not args.four_way or args.loss != 0 or args.baseline_check or
                              args.isolation or args.recovery_only or args.all_responders_smoke or
                              args.discovery_payload is not None or args.wire_only):
        parser.error('--wired-rate-rps requires --four-way and zero loss without other studies')
    if args.discovery_payload is not None and not args.smoke:
        parser.error('--discovery-payload is a smoke qualification input, not a timing campaign')
    if args.all_responders_smoke and not args.smoke:
        parser.error('--all-responders-smoke requires --smoke')
    if args.recovery_only and (args.smoke or args.four_way or args.wire_only or args.isolation):
        parser.error('--recovery-only is a separate acceptance run')
    if args.rotate_certificate and not args.recovery_only:
        parser.error('--rotate-certificate requires --recovery-only')
    if args.baseline_check == 'nsc-signer':
        parser.error('nsc-signer needs a migrated local wrong-signer fixture; '
                     'peer private keys are no longer shared')
    if os.geteuid() != 0:
        raise SystemExit('MiniNDN requires root; use sudo -n python3')
    run_cell(args, parser)


if __name__ == '__main__':
    main()
