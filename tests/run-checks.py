#!/usr/bin/env python3
"""Run the selected native check profile from the roster.

This owns selection and reporting only. It does not implement inference, authorization or any
protocol, and its report is never an oracle: the native executables are the evidence.

Two rules it enforces because their absence made earlier runs misleading:

  * A case that no profile selects is an error, not a silent omission. The roster is the list of
    checks this project claims to run; a selector that matches nothing means the claim is stale.
  * A selected check whose declared asset is missing exits with PREREQUISITE_MISSING and a non-zero
    status. Returning success when a model was absent is how a skipped run gets counted as a pass.

Usage:
    python3 tests/run-checks.py --profile unit
    python3 tests/run-checks.py --profile model --asset-manifest PATH
    python3 tests/run-checks.py --profile all --fixture-root PATH --report report.json
"""
import argparse
from contextlib import contextmanager
import fnmatch
import hashlib
import importlib.util
import json
import os
import re
import selectors
import signal
import stat
import shutil
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET
from pathlib import Path

def installed_prefix():
  """Return the prefix this runner was installed under, or None when running from a checkout.

  An installed runner lives at PREFIX/share/ndnsf/tests/run-checks.py and must resolve its roster,
  fixtures and binaries from that prefix -- not from the checkout it may still be sitting beside,
  and not from the developer's build tree.
  """
  here = Path(__file__).resolve().parent
  if here.name != 'tests' or here.parent.name != 'ndnsf' or here.parent.parent.name != 'share':
      return None
  return here.parent.parent.parent
EXIT_OK = 0
EXIT_FAILED = 1
EXIT_PREREQUISITE = 2
EXIT_ROSTER = 3
MAX_OUTPUT_BYTES = 16 * 1024 * 1024


def configured_roster_path(prefix, build_root):
    if prefix is not None:
        return prefix / 'share' / 'ndnsf' / 'tests' / 'check-profiles.json'
    return build_root / 'tests' / 'check-profiles.json'


def verify_installed_files(prefix, roster, profiles):
    """Verify the existing installer's receipt; never manufacture a second install authority."""
    manifest = prefix / 'share/ndnsf/stack-install-manifest.json'
    data = json.loads(manifest.read_text())
    if data.get('schema') != 1 or not isinstance(data.get('files'), dict):
        raise ValueError('INSTALL_MANIFEST_SCHEMA_UNSUPPORTED')
    required = {prefix / 'bin/ndnsf-checks',
                prefix / 'share/ndnsf/tests/run-checks.py',
                prefix / 'share/ndnsf/tests/check-profiles.json'}
    required.update(prefix / 'libexec/ndnsf-tests' / row['target']
                    for profile in profiles for row in roster['profiles'][profile]['targets'])
    if any(row.get('assets') for profile in profiles
           for row in roster['profiles'][profile]['targets']):
        required.update(prefix / 'share/ndnsf' / name for name in (
            'Experiments/model_assets.py', 'Experiments/recipes/qwen3-0.6b-int4.json'))
    if any('identity-check' in row.get('facilities', [])
           for profile in profiles for row in roster['profiles'][profile]['targets']):
        required.add(prefix / 'bin/ndnsf-identity-check')
    if any('repo-owner-delete' in row.get('facilities', [])
           for profile in profiles for row in roster['profiles'][profile]['targets']):
        required.add(prefix / 'bin/ndnsf-repo-owner-delete')
    if any('gpu-query-fixtures' in row.get('facilities', [])
           for profile in profiles for row in roster['profiles'][profile]['targets']):
        vendor_root, files = gpu_query_files(prefix, None)
        required.update(files)
        required.update(Path(name) for name in data['files'] if vendor_root in Path(name).parents)
    fixture_root = prefix / 'share/ndnsf/tests/fixtures'
    libraries = prefix / 'lib'
    component_libraries = {'core': 'libndn-service-framework.so',
                           'repo': 'libndnsf-distributed-repo.a',
                           'di': 'libndnsf-distributed-inference.so'}
    required.update(libraries / name for component, name in component_libraries.items()
                    if component in roster['configuredComponents'])
    if 'di' in roster['configuredComponents']:
        required.add(prefix / 'libexec/ndnsf-di/DI_NativeOnnxAssemblyWorker')
        required.update(prefix / 'libexec/ndnsf-tests' / ('worker-tool-' + role)
                        for role in ('block', 'sigkill', 'silent0', 'silent7', 'garbage'))
    if not roster.get('fixtureFiles'):
        raise ValueError('CONFIGURED_TEST_FIXTURES_MISSING')
    for name in roster['fixtureFiles']:
        relative = Path(name)
        if relative.is_absolute() or '..' in relative.parts:
            raise ValueError('CONFIGURED_TEST_FIXTURE_PATH_INVALID: ' + name)
        required.add(fixture_root / relative)
    if 'di' in roster['configuredComponents'] and not roster.get('driverFiles'):
        raise ValueError('CONFIGURED_TEST_DRIVERS_MISSING')
    for name in roster.get('driverFiles', []):
        if Path(name).name != name:
            raise ValueError('CONFIGURED_TEST_DRIVER_PATH_INVALID: ' + name)
        required.add(prefix / 'share/ndnsf/tests/standalone' / name)
    python_root = prefix / 'libexec/ndnsf-tests/python'
    process_profile = any('native-process-python' in row.get('facilities', [])
                          for profile in profiles for row in roster['profiles'][profile]['targets'])
    if process_profile:
        # The process cases drive these two installed sub-targets from the prefix, so the manifest must
        # fail before startup when either is missing rather than when the case runs.
        for sub_target in ('di-prepared-request', 'di-provider-assembly'):
            required.add(prefix / 'libexec/ndnsf-tests' / sub_target)
        required.update(Path(name) for name in data['files'] if python_root in Path(name).parents)
        required.update(prefix / 'bin' / name for name in (
            'App_ServiceController', 'DI_NativeArtifactAuthority', 'DI_NativeRequester',
            'di-native-provider', 'DistributedRepoServer'))
        if not any(python_root in path.parents for path in required):
            raise ValueError('INSTALL_MANIFEST_PROCESS_PYTHON_MISSING')
    for name in data['files']:
        path = Path(name)
        if (fixture_root in path.parents or
                (path.parent == libraries and
                 path.name.startswith(('libndnsf-', 'libndn-service-framework')))):
            required.add(path)
    for path in sorted(required):
        record = data['files'].get(str(path))
        owner = 'ndnsf-test-python' if python_root in path.parents else 'ndnsf-native'
        if record is None or owner not in record.get('owners', []):
            raise ValueError('INSTALL_MANIFEST_ENTRY_MISSING: ' + str(path))
        expected = record['installed']
        info = path.lstat()
        if stat.S_ISLNK(info.st_mode):
            actual = {'kind': 'symlink', 'target': os.readlink(path),
                      'mode': stat.S_IMODE(info.st_mode)}
        elif stat.S_ISREG(info.st_mode):
            digest = hashlib.sha256()
            with path.open('rb') as stream:
                for block in iter(lambda: stream.read(1024 * 1024), b''):
                    digest.update(block)
            actual = {'kind': 'file', 'sha256': digest.hexdigest(),
                      'mode': stat.S_IMODE(info.st_mode)}
        else:
            raise ValueError('INSTALL_MANIFEST_FILE_TYPE: ' + str(path))
        if actual != expected:
            raise ValueError('INSTALLED_FILE_MISMATCH: ' + str(path))
    return len(required)


def gpu_query_files(prefix, build_root):
    """Keep fake vendor DSOs private to their owning test invocation."""
    root = (prefix / 'libexec/ndnsf-tests/vendor-gpu' if prefix is not None else
            build_root / 'spec217-gpu-fixtures')
    query = (prefix / 'libexec/ndnsf-di/ndnsf-di-device-query' if prefix is not None else
             build_root / 'ndnsf-di-device-query')
    return root, [root / name for name in (
        'libcudart.so.12', 'libcuda.so.1', 'libnvidia-ml.so.1')] + [query]


ROUTINE_PROFILES = frozenset({'unit', 'integration'})

# Inputs the T001 audit proved can change a routine case's data, helper or assertions. A routine profile
# must not inherit them from the developer shell: each one switches a default case onto host data, and the
# explicit `qualification`/`model` profiles are where they may be supplied.
QUALIFICATION_ONLY_INPUTS = frozenset({
    'NDNSF_DI_TEST_ONNX_MODEL',              # D2b real-model branch
    'NDNSF_DI_NATIVE_PLAN_JSON',             # async runtime plan override
    'NDNSF_DI_NATIVE_PLAN_SERVICE',
    'NDNSF_SPEC190_RUN_INT8',                # direct INT8 gate
    'NDNSF_SPEC190_INT8_ONNX',
    'SPEC190_NATIVE_REQUESTER_BINARY',       # real CLI live turns
    'SPEC190_NATIVE_TURNS_CONFIG',
    'NDNSF_TEST_PORTABLE_ONNX_GRAPH',        # frozen CUDA replay
    'NDNSF_TEST_PORTABLE_CUDA_PROFILE',
})


def target_environment(target, environ, prefix, build_root, routine=False):
    result = environ.copy()
    if routine:
        for name in QUALIFICATION_ONLY_INPUTS:
            result.pop(name, None)
    if 'repo-owner-delete' in target.get('facilities', []):
        binary = (prefix / 'bin/ndnsf-repo-owner-delete' if prefix is not None else
                  build_root / 'NDNSF-DistributedRepo/ndnsf-repo-owner-delete')
        if not binary.is_file() or not os.access(binary, os.X_OK):
            raise ValueError('REPO_OWNER_DELETE_TEST_INPUT_MISSING: ' + str(binary))
        result['NDNSF_REPO_OWNER_DELETE_BIN'] = str(binary)
    if 'identity-check' in target.get('facilities', []):
        binary = (prefix / 'bin/ndnsf-identity-check' if prefix is not None else
                  build_root / 'examples/ndnsf-identity-check')
        if not binary.is_file() or not os.access(binary, os.X_OK):
            raise ValueError('IDENTITY_CHECK_TEST_INPUT_MISSING: ' + str(binary))
        result['NDNSF_IDENTITY_CHECK_EXECUTABLE'] = str(binary)
    if 'gpu-query-fixtures' in target.get('facilities', []):
        root, files = gpu_query_files(prefix, build_root)
        missing = [str(path) for path in files if not path.is_file()]
        if missing or not os.access(files[-1], os.X_OK):
            raise ValueError('GPU_QUERY_TEST_INPUT_MISSING: ' + ', '.join(missing or [str(files[-1])]))
        result['LD_LIBRARY_PATH'] = os.pathsep.join(filter(None, (
            str(root), environ.get('LD_LIBRARY_PATH', ''))))
        result['NDNSF_DEVICE_QUERY_FIXTURE_DIR'] = str(root)
        result['NDNSF_DEVICE_QUERY_EXECUTABLE'] = str(files[-1])
        result.pop('SPEC217_GPU_FIXTURE_SCENARIO', None)
    return result


def check_facilities(roster, profiles, environ, prefix=None, build_root=None):
    missing = []
    for profile in profiles:
        for target in roster['profiles'][profile]['targets']:
            for facility in target.get('facilities', []):
                if facility in ('gpu-query-fixtures', 'identity-check', 'repo-owner-delete'):
                    try:
                        target_environment(target, environ, prefix, build_root)
                    except (OSError, ValueError) as error:
                        missing.append((facility, str(error)))
                elif facility == 'ndn-tools':
                    if not all(os.access('/usr/local/bin/' + program, os.X_OK)
                               for program in ('nfd', 'nfdc', 'ndnsec')):
                        missing.append((facility, 'install the declared NFD/ndn-cxx command tools'))
                elif facility == 'process-isolation':
                    if not all(os.access(path, os.X_OK) for path in (
                            '/usr/bin/bwrap', '/usr/bin/openssl', '/usr/bin/ldd')):
                        missing.append((facility, 'install bubblewrap, openssl and libc-bin'))
                elif facility == 'native-process-python':
                    try:
                        result = subprocess.run([environ.get('NDNSF_TEST_PYTHON', sys.executable),
                            '-s', '-c', 'import cryptography; import ndn.encoding'],
                            env=environ, capture_output=True, text=True, timeout=15)
                        if result.returncode:
                            missing.append((facility, 'prepare the --with-tests Python prerequisites'))
                    except (OSError, subprocess.TimeoutExpired) as error:
                        missing.append((facility, str(error)))
                elif facility != 'display':
                    missing.append((facility, 'unknown facility declaration'))
                elif not (environ.get('DISPLAY') or environ.get('WAYLAND_DISPLAY')) and not all(
                        shutil.which(program) for program in ('xvfb-run', 'Xvfb', 'xauth')):
                    missing.append(('display', 'set DISPLAY/WAYLAND_DISPLAY or install xvfb and xauth'))
    return missing


def live_session_processes(session):
    """Include children placed in new process groups by timeout/Xvfb; ignore dead zombies."""
    processes = []
    for entry in Path('/proc').iterdir():
        if not entry.name.isdigit():
            continue
        try:
            fields = (entry / 'stat').read_text().rsplit(')', 1)[1].split()
            if int(fields[3]) == session and fields[0] not in ('Z', 'X'):
                processes.append(int(entry.name))
        except (OSError, ValueError, IndexError):
            continue
    return processes


def signal_owned_session(session, sig):
    for pid in live_session_processes(session):
        try:
            if os.getsid(pid) == session:
                os.kill(pid, sig)
        except ProcessLookupError:
            pass


def native_process(argv, environ, timeout, *, cwd=None):
    """Bound output and reclaim the session created for this native invocation."""
    buffers = {'stdout': bytearray(), 'stderr': bytearray()}
    total = 0
    status = None
    boundary = None
    process = subprocess.Popen(argv, env=environ, cwd=cwd, stdin=subprocess.DEVNULL,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               start_new_session=True)
    try:
        deadline = time.monotonic() + timeout
        with selectors.DefaultSelector() as selector:
            selector.register(process.stdout, selectors.EVENT_READ, 'stdout')
            selector.register(process.stderr, selectors.EVENT_READ, 'stderr')
            while selector.get_map() and status is None:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    status = 124
                    boundary = 'NATIVE_TIMEOUT'
                    break
                for key, _ in selector.select(min(.1, remaining)):
                    data = os.read(key.fileobj.fileno(), 65536)
                    if not data:
                        selector.unregister(key.fileobj)
                        continue
                    accepted = data[:max(0, MAX_OUTPUT_BYTES - total)]
                    buffers[key.data].extend(accepted)
                    total += len(accepted)
                    if len(accepted) < len(data):
                        status = 125
                        boundary = 'NATIVE_OUTPUT_LIMIT'
                        break
        if status is None:
            try:
                status = process.wait(timeout=max(.001, deadline - time.monotonic()))
            except subprocess.TimeoutExpired:
                status = 124
                boundary = 'NATIVE_TIMEOUT'
    finally:
        if process.poll() is None:
            signal_owned_session(process.pid, signal.SIGTERM)
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                signal_owned_session(process.pid, signal.SIGKILL)
                process.wait()
        # A successful parent must not leave children behind, even with redirected output
        # or a separate process group. All these children remain in our new session.
        if live_session_processes(process.pid):
            if status == 0:
                status = 125
                boundary = 'NATIVE_DESCENDANTS_NOT_DRAINED'
        drain_deadline = time.monotonic() + 2
        while live_session_processes(process.pid) and time.monotonic() < drain_deadline:
            signal_owned_session(process.pid, signal.SIGKILL)
            time.sleep(.01)
        if live_session_processes(process.pid):
            status = 125
            boundary = 'NATIVE_PROCESS_SESSION_NOT_DRAINED'
        process.stdout.close()
        process.stderr.close()
    result = subprocess.CompletedProcess(argv, status,
        buffers['stdout'].decode('utf-8', 'replace'), buffers['stderr'].decode('utf-8', 'replace'))
    result.nativeExit = process.returncode
    result.boundary = boundary
    return result


def load_roster(path):
    with open(path, encoding='utf-8') as handle:
        roster = json.load(handle)
    if roster.get('schema') != 'ndnsf-check-profiles-v1':
        raise SystemExit('ROSTER_SCHEMA_UNSUPPORTED: %s' % roster.get('schema'))
    return roster


def resolve_binary(target, search):
    """Find one check binary by name, in the declared search order.

    `target` is the roster's target name; the Waf build writes it either into the build directory
    or beside it depending on how the program declared its target, so both are searched rather than
    assuming one layout.
    """
    for directory in search:
        candidate = Path(directory) / target
        if candidate.is_file() and os.access(str(candidate), os.X_OK):
            return candidate
    return None


@contextmanager
def isolated_native_environment(environ):
    """Own identity and publication paths even during native test registration."""
    with tempfile.TemporaryDirectory(prefix='ndnsf-check-result-') as result_root:
        native_env = environ.copy()
        for key in ('NDN_CLIENT_PIB', 'NDN_CLIENT_TPM', 'NDNSF_CONTROLLER_CERT_FILE'):
            native_env.pop(key, None)
        for key, name in (('HOME', 'home'), ('NDNSF_REQUEST_LARGE_DATA_DIR', 'large-data'),
                          ('TMPDIR', 'scratch')):
            directory = Path(result_root) / name
            directory.mkdir(mode=0o700)
            native_env[key] = str(directory)
        yield Path(result_root), native_env


def enumerate_cases(binary, environ):
    """List the suites and cases a binary contains, from Boost.Test's own tree.

    Boost prints one `name*` per node, indented by depth, and a deeper node belongs to the one above
    it. Telling cases from suites by "indented" is wrong: nested suites are indented too, so a binary
    with 168 cases inside 5 sub-suites counted as 173 cases. A node is a case exactly when the next
    node is not deeper than it is.

    Both streams are read because Boost.Test writes this listing to stderr; parsing only stdout
    reports a binary with thousands of cases as having none.
    """
    with isolated_native_environment(environ) as (result_root, native_env):
        # Enumeration runs through the same bounded session as execution: a listing that times out or
        # leaves descendants behind must fail the run here, because a partial listing would start the
        # selected cases on incomplete knowledge of what the binary contains.
        completed = native_process([str(binary), '--list_content'], native_env, 300,
                                   cwd=result_root / 'scratch')
    if completed.returncode != 0:
        boundary = getattr(completed, 'boundary', None) or 'CASE_ENUMERATION_FAILED'
        raise ValueError('%s: %s\n%s' % (boundary, binary, completed.stderr[-2000:]))
    nodes = []
    for line in (completed.stdout + completed.stderr).splitlines():
        stripped = line.strip()
        if not stripped:
            continue
        nodes.append((len(line) - len(line.lstrip()), stripped.rstrip('*').strip(),
                      stripped.endswith('*')))
    suites, cases, disabled, parents = [], [], [], []
    for index, (depth, name, enabled) in enumerate(nodes):
        next_depth = nodes[index + 1][0] if index + 1 < len(nodes) else -1
        while parents and parents[-1][0] >= depth:
            parents.pop()
        path = '/'.join([p[1] for p in parents] + [name])
        if next_depth <= depth:
            cases.append(path)
            if not enabled:
                disabled.append(path)
        else:
            suites.append(path)
            parents.append((depth, name))
    if len(cases) != len(set(cases)):
        raise ValueError('CASE_IDENTITY_DUPLICATED: %s' % binary)
    return suites, cases, disabled


def select_cases(target, cases, disabled=(), require_match=True):
    """Resolve roster filters using full suite/case identities, before starting a run."""
    selector = target.get('selector', {})
    include = selector.get('include', ['*'])
    exclude = selector.get('excludeSuites', [])
    if not include:
        raise ValueError('ROSTER_SELECTOR_EMPTY: %s' % target['target'])
    selected = set()
    for pattern in include:
        if not isinstance(pattern, str) or not pattern or any(c in pattern for c in ':,!+@'):
            raise ValueError('ROSTER_SELECTOR_UNSUPPORTED: %s' % pattern)
        matches = {case for case in cases if fnmatch.fnmatchcase(case, pattern)}
        if not matches and require_match:
            raise ValueError('CHECK_SELECTOR_UNMATCHED: %s %s' % (target['target'], pattern))
        selected.update(matches)
    selected = {case for case in selected if not any(
        case == suite or case.startswith(suite + '/') for suite in exclude)}
    if not selector.get('includeDisabled', False):
        selected.difference_update(disabled)
    return sorted(selected)


def case_owners(roster, target_name, cases, disabled=()):
    """Check every enabled discovered case against the declared profiles for its binary."""
    owners = {case: [] for case in cases if case not in disabled}
    for name, profile in roster['profiles'].items():
        for target in profile.get('targets', []):
            if target['target'] != target_name:
                continue
            # Another component's unselected model suite need not be present in this binary.
            for case in select_cases(target, cases, disabled, require_match=False):
                if case in owners:
                    owners[case].append(name)
    missing = [case for case, names in owners.items() if not names]
    if missing:
        raise ValueError('CHECK_CASE_UNOWNED: %s: %s' % (target_name, ', '.join(missing[:20])))
    return owners


def read_case_results(path):
    """Use Boost's native per-case results, not assertion totals or announced counts."""
    root = ET.parse(path).getroot().find('TestSuite')
    if root is None:
        raise ValueError('NATIVE_RESULT_MODULE_MISSING')
    results = {}
    def walk(node, parents=()):
        if node.tag == 'TestCase':
            name = '/'.join(parents + (node.attrib['name'],))
            if name in results:
                raise ValueError('CASE_RESULT_DUPLICATED: %s' % name)
            results[name] = node.attrib['result']
        else:
            # The XML contains a module node that --list_content omits.
            parts = parents + (node.attrib['name'],) if parents or node is not root else ()
            for child in node:
                walk(child, parts)
    walk(root)
    return results


def read_case_assertions(path):
    """Map each case to the number of assertions Boost recorded for it.

    Boost reports an early-returning case as `result="passed"` with
    `assertions_passed="0"`, which is how a prerequisite-gated case looks successful while executing
    nothing. The count is in the same XML the runner already parses, so it is read here instead of
    matching a log phrase that this report level does not emit.
    """
    root = ET.parse(path).getroot().find('TestSuite')
    if root is None:
        raise ValueError('NATIVE_RESULT_MODULE_MISSING')
    counts = {}
    def walk(node, parents=()):
        if node.tag == 'TestCase':
            name = '/'.join(parents + (node.attrib['name'],))
            counts[name] = int(node.attrib.get('assertions_passed', '0'))
        else:
            parts = parents + (node.attrib['name'],) if parents or node is not root else ()
            for child in node:
                walk(child, parts)
    walk(root)
    return counts


def check_assets(roster, selected, manifest_path, bindings=None):
    """Admit pinned SEALED assets and pass their exact files to native consumers."""
    missing = []
    names = sorted({asset_name for name in selected
                    for target in roster['profiles'][name].get('targets', [])
                    for asset_name in target.get('assets', [])})
    for asset_name in names:
        try:
            asset = roster['assets'][asset_name]
            path = manifest_path or os.environ.get('NDNSF_MODEL_ASSET_MANIFEST')
            if not path:
                raise ValueError('SEALED manifest required: use --asset-manifest FILE; '
                                 'prepare with prepare-model-assets.py --recipe ' + asset['recipe'])
            if asset['recipe'] != 'qwen3-0.6b-int4':
                raise ValueError('ASSET_RECIPE_UNSUPPORTED: ' + asset['recipe'])
            experiment_root = Path(__file__).resolve().parent.parent / 'Experiments'
            spec = importlib.util.spec_from_file_location(
                'ndnsf_asset_admission', experiment_root / 'model_assets.py')
            module = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(module)
            recipe = module.read_json(experiment_root / 'recipes/qwen3-0.6b-int4.json')
            if recipe['assetId'] != asset_name:
                raise ValueError('ASSET_ROSTER_RECIPE_MISMATCH')
            files = module.validate_manifest(Path(path).absolute(), recipe)
            if bindings is not None:
                bindings[asset_name] = {
                    'NDNSF_TEST_CANONICAL_MODEL_DIR': str(files['canonicalGraph'].parent),
                    'NDNSF_SPEC233_REAL_MODEL': str(files['sourceModel']),
                    'NDNSF_SPEC233_REAL_GENERATION_ROW': 'Q06-I4',
                }
        except (OSError, ValueError, KeyError, TypeError, ImportError) as error:
            missing.append((asset_name, str(error)))
    return missing


def run_target(binary, target, environ, timeout, arguments, listing=None):
    """Run one selector, once, and report its own summary line.

    One invocation per selector: the umbrella binaries hold thousands of cases and running each
    case separately would spend the whole budget in process startup. The selector is the unit the
    roster declares, so its own exit status is the result.
    """
    _, cases, disabled = listing if listing is not None else enumerate_cases(binary, environ)
    expected = select_cases(target, cases, disabled)
    if not expected:
        raise ValueError('CHECK_SELECTOR_UNMATCHED: %s' % target['target'])
    selector = target.get('selector', {})
    run_name = target['target'] + '.' + hashlib.sha256(
        json.dumps(target, sort_keys=True, separators=(',', ':')).encode()).hexdigest()[:12]
    argv = [str(binary), '--color_output=no', '--log_level=test_suite',
            '--report_level=detailed', '--report_format=XML']
    if ('display' in target.get('facilities', []) and
            not (environ.get('DISPLAY') or environ.get('WAYLAND_DISPLAY'))):
        argv = ['xvfb-run', '--auto-servernum'] + argv
    argv += ['--run_test=' + pattern for pattern in selector.get('include', ['*'])]
    argv += ['--run_test=!' + suite for suite in selector.get('excludeSuites', [])]
    if not selector.get('includeDisabled', False):
        argv += ['--run_test=!' + case for case in disabled]
    report_error = None
    native_results = {}
    with isolated_native_environment(environ) as (result_root, native_env):
        xml_path = Path(result_root) / 'result.xml'
        argv.append('--report_sink=' + str(xml_path))
        try:
            completed = native_process(argv, native_env, timeout, cwd=result_root / 'scratch')
            status = completed.returncode
            native_exit = getattr(completed, 'nativeExit', completed.returncode)
            process_boundary = getattr(completed, 'boundary', None)
            combined = (completed.stdout + completed.stderr)
        except subprocess.TimeoutExpired as expired:
            # A timeout is a result, not an absence of one: the earlier form let it escape and the
            # report was never written, so the run that hit the budget left no record at all.
            status = 124
            native_exit = None
            process_boundary = 'NATIVE_TIMEOUT'
            combined = ''
            for stream in (expired.stdout, expired.stderr):
                if stream:
                    combined += stream if isinstance(stream, str) else stream.decode('utf-8', 'replace')
        try:
            native_results = read_case_results(xml_path)
        except (OSError, ET.ParseError, ValueError, KeyError) as error:
            report_error = 'NATIVE_RESULT_UNAVAILABLE: %s' % error
        actual = {case: native_results.get(case, 'not-run') for case in expected}
        unexpected = sorted(case for case, outcome in native_results.items()
                            if case not in expected and outcome != 'skipped')
        if unexpected:
            report_error = 'NATIVE_SELECTION_MISMATCH: ' + ', '.join(unexpected[:10])
        if re.search(r'\bNOT_RUN\s*:', combined):
            report_error = ((report_error + '; ') if report_error else '') + \
                'NATIVE_NOT_RUN_MARKER: selected native checks did not execute their oracle'
        if re.search(r'\bdid not check any assertions\b', combined):
            report_error = ((report_error + '; ') if report_error else '') + \
                'NATIVE_ASSERTION_FREE_CASE: selected native check has no assertions'
        try:
            assertion_counts = read_case_assertions(xml_path)
        except (OSError, ET.ParseError, ValueError, KeyError):
            assertion_counts = {}
        empty = sorted(case for case in expected
                       if actual.get(case) == 'passed' and assertion_counts.get(case, 0) < 1)
        if empty:
            report_error = ((report_error + '; ') if report_error else '') + \
                'NATIVE_ASSERTION_FREE_CASE: ' + ', '.join(empty[:5])
        if arguments.report and xml_path.exists():
            xml_copy = str(Path(arguments.report).with_suffix('')) + '.' + run_name + '.xml'
            shutil.copyfile(xml_path, xml_copy)
    lines = combined.strip().splitlines()
    log_path = None
    if arguments.report:
        log_path = str(Path(arguments.report).with_suffix('')) + '.' + run_name + '.log'
        Path(log_path).write_text(combined)
    counts = {outcome: sum(value == outcome for value in actual.values())
              for outcome in ('passed', 'failed', 'skipped', 'aborted', 'timed-out', 'not-run')}
    executed = sum(counts[x] for x in ('passed', 'failed', 'aborted', 'timed-out'))
    return [{'target': target['target'], 'selector': selector, 'status': status,
             'nativeExit': native_exit, 'processBoundary': process_boundary,
             'passed': status == 0 and not report_error and counts['passed'] == len(expected),
             'log': log_path, 'selected': len(expected), 'executed': executed,
             'failed': counts['failed'] + counts['aborted'] + counts['timed-out'],
             'skipped': counts['skipped'], 'unavailable': counts['not-run'],
             'caseResults': actual, 'reportError': report_error, 'tail': lines[-6:]}]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--profile', required=True,
                        help='profile name, or "all" for every declared profile')
    parser.add_argument('--fixture-root', default=os.environ.get('NDNSF_TEST_FIXTURE_ROOT'),
                        help='declared fixture root; passed through to every check')
    parser.add_argument('--asset-manifest', default=None,
                        help='SEALED asset-manifest.json from the pinned model preparation recipe')
    parser.add_argument('--build-root', default=None,
                        help='directory holding the built check binaries')
    parser.add_argument('--report', default=None, help='write the JSON report here')
    parser.add_argument('--timeout', type=float, default=3600.0, help='per-selector timeout')
    arguments = parser.parse_args(argv)

    prefix = installed_prefix()
    checkout = Path(__file__).resolve().parent.parent
    build_root = Path(arguments.build_root).resolve() if arguments.build_root else checkout / 'build'
    roster_path = configured_roster_path(prefix, build_root)
    try:
        roster = load_roster(roster_path)
        if not roster.get('configuredComponents'):
            raise ValueError('configuredComponents is missing')
    except (OSError, ValueError) as error:
        sys.stderr.write('CONFIGURED_TEST_ROSTER_UNAVAILABLE: %s (%s)\n' % (roster_path, error))
        return EXIT_ROSTER
    known = sorted(roster['profiles'])
    if arguments.profile == 'all':
        selected = known
    elif arguments.profile in roster['profiles']:
        selected = [arguments.profile]
    else:
        sys.stderr.write('ROSTER_UNKNOWN_PROFILE: %s (known: %s)\n'
                         % (arguments.profile, ', '.join(known)))
        return EXIT_ROSTER

    unavailable_profiles = [name for name in selected if not roster['profiles'][name]['targets']]
    if unavailable_profiles and arguments.profile != 'all':
        sys.stderr.write('CHECK_PROFILE_COMPONENT_UNAVAILABLE: %s (components: %s)\n'
                         % (', '.join(unavailable_profiles), ', '.join(roster['configuredComponents'])))
        return EXIT_ROSTER
    selected = [name for name in selected if roster['profiles'][name]['targets']]
    if prefix is not None:
        # Installed mode: binaries and fixtures come from the prefix, and a build-root override is
        # refused rather than honoured, because accepting one would let an installed check run
        # against a source tree it was not qualified with.
        if arguments.build_root:
            sys.stderr.write('BUILD_ROOT_REFUSED_IN_INSTALLED_MODE: %s\n' % arguments.build_root)
            return EXIT_ROSTER
        search = [str(prefix / 'libexec' / 'ndnsf-tests'), str(prefix / 'bin')]
        # The receipt verifies the prefix's own fixture root, so an explicit root may only name that same
        # directory. Accepting another one would check the receipt against one tree and then read test data
        # from another, which is how a same-named host fixture changes the verdict.
        verified_fixture_root = (prefix / 'share' / 'ndnsf' / 'tests' / 'fixtures').resolve()
        if arguments.fixture_root is None:
            arguments.fixture_root = str(verified_fixture_root)
        else:
            requested_root = Path(arguments.fixture_root).resolve()
            if requested_root != verified_fixture_root:
                sys.stderr.write('FIXTURE_ROOT_OUTSIDE_VERIFIED_PREFIX: %s (verified: %s)\n'
                                 % (requested_root, verified_fixture_root))
                return EXIT_ROSTER
        try:
            verified_files = verify_installed_files(prefix, roster, selected)
        except (OSError, ValueError, KeyError) as error:
            sys.stderr.write('INSTALLED_RECEIPT_REJECTED: %s\n' % error)
            return EXIT_ROSTER
    else:
        search = [str(build_root)]

    environ = dict(os.environ)
    if arguments.fixture_root:
        environ['NDNSF_TEST_FIXTURE_ROOT'] = str(Path(arguments.fixture_root).resolve())
    # An installed run must not silently pick up a developer's build tree, site packages or a
    # preloaded library: each of those would let a check pass against something the prefix does not
    # actually deliver.
    for name in ('PYTHONPATH', 'LD_PRELOAD', 'LD_LIBRARY_PATH'):
        environ.pop(name, None)
    if prefix is not None:
        # The installed artifact has no build-tree RPATH, so its libraries are resolved from the
        # prefix explicitly. This is the environment an installed consumer runs in, not a
        # developer's; the developer's own LD_LIBRARY_PATH was just removed.
        environ['LD_LIBRARY_PATH'] = str(prefix / 'lib')
        environ['PYTHONNOUSERSITE'] = '1'
        environ['PYTHONPATH'] = str(prefix / 'libexec/ndnsf-tests/python')
        environ['NDNSF_TEST_PYTHON'] = '/usr/bin/python3'
        environ['NDNSF_TEST_SUPPORT_ROOT'] = str(prefix / 'share/ndnsf')
        environ['NDNSF_TEST_RUNTIME_PREFIX'] = str(prefix)
        environ.pop('SPEC185_REPO_ROOT', None)
        environ['SPEC185_BUILD_DIR'] = str(prefix / 'libexec/ndnsf-tests')
        environ['SPEC185_EXTERNAL_BUILD_DIR'] = str(prefix / 'libexec/ndnsf-tests')
        environ['NDNSF_SPEC182_BIN_DIR'] = str(prefix / 'libexec/ndnsf-tests')
        if 'di' in roster['configuredComponents']:
            environ['NDNSF_DI_WORKER_BINARY'] = str(
                prefix / 'libexec/ndnsf-di/DI_NativeOnnxAssemblyWorker')
    else:
        # Source mode: the artifact carries no RPATH either, so the build tree it was built in is
        # bound explicitly. Without this the loader resolves whatever the host cache holds, which
        # may be an older installed library rather than the candidate under test.
        environ['LD_LIBRARY_PATH'] = str(Path(search[0]).resolve())
        if ('di' in roster['configuredComponents'] and
                not environ.get('NDNSF_DI_WORKER_BINARY') and
                'NDNSF_SPEC182_BIN_DIR' not in environ):
            environ['NDNSF_DI_WORKER_BINARY'] = str(build_root / 'DI_NativeOnnxAssemblyWorker')
        environ.setdefault('NDNSF_SPEC182_BIN_DIR', str(build_root))
        environ.setdefault('NDNSF_TEST_PYTHON', sys.executable)
        environ.setdefault('NDNSF_TEST_SUPPORT_ROOT', str(checkout))
        environ.setdefault('SPEC185_BUILD_DIR', str(build_root))
    if prefix is not None:
        # An installed artifact must not name the build tree in its RPATH: that path does not exist
        # on another machine, so a binary carrying it is not portable even if it runs here. This is
        # checked, not assumed, because the build wrote the RPATH and only the artifact knows it.
        for binary in sorted(Path(search[0]).glob('*')) if Path(search[0]).is_dir() else []:
            if not binary.is_file():
                continue
            rpath = subprocess.run(['readelf', '-d', str(binary)], capture_output=True, text=True)
            line = next((entry for entry in rpath.stdout.splitlines()
                         if 'Library rpath' in entry or 'Library runpath' in entry), '')
            for suspect in ('/home/', '/build/'):
                if suspect in line:
                    sys.stderr.write('INSTALLED_RPATH_REFERENCES_BUILD_TREE: %s (%s)\n'
                                     % (binary, line.strip()))
                    return EXIT_ROSTER

    asset_bindings = {}
    missing = check_facilities(roster, selected, environ, prefix, build_root) + \
        check_assets(roster, selected, arguments.asset_manifest, asset_bindings)
    if missing:
        for asset_name, detail in missing:
            sys.stderr.write('PREREQUISITE_MISSING: %s (%s)\n' % (asset_name, detail))
        report = {'schema': 'ndnsf-check-report-v1', 'profile': arguments.profile,
                  'status': 'PREREQUISITE_MISSING', 'missing': missing}
        if arguments.report:
            Path(arguments.report).write_text(json.dumps(report, indent=2) + '\n')
        return EXIT_PREREQUISITE

    # Four numbers, because they answer four different questions and conflating them is how a
    # binary's total got reported as a run's coverage: what the binary holds, what the roster
    # selected, what actually ran, and what could not run at all.
    report = {'schema': 'ndnsf-check-report-v1', 'profile': arguments.profile,
              'configuredComponents': roster['configuredComponents'],
              'installedFilesVerified': verified_files if prefix is not None else None,
              'status': 'RUN', 'selected': selected, 'targets': [], 'unmatched': 0,
              'discovered': 0, 'selectedCases': 0, 'executed': 0, 'failed': 0,
              'unavailable': 0}
    failed = False
    for name in selected:
        for target in roster['profiles'][name].get('targets', []):
            binary = resolve_binary(target['target'], search)
            if binary is None:
                sys.stderr.write('CHECK_BINARY_MISSING: %s (searched %s)\n'
                                 % (target['target'], ', '.join(search)))
                report['targets'].append({'target': target['target'],
                                          'status': 'BINARY_MISSING'})
                failed = True
                continue
            try:
                target_env = target_environment(target, environ, prefix, build_root,
                                                 routine=name in ROUTINE_PROFILES)
                for asset_name in target.get('assets', []):
                    target_env.update(asset_bindings[asset_name])
                listing = enumerate_cases(binary, target_env)
                suites, cases, disabled = listing
                if not cases:
                    raise ValueError('CHECK_BINARY_HAS_NO_CASES: %s' % binary)
                owners = case_owners(roster, target['target'], cases, disabled)
                entries = run_target(binary, target, target_env, arguments.timeout, arguments, listing)
            except (ValueError, OSError, subprocess.SubprocessError) as error:
                sys.stderr.write(str(error) + '\n')
                report['targets'].append({'target': target['target'], 'status': 'HARNESS_ERROR',
                                          'error': str(error)})
                report['unmatched'] += 1
                failed = True
                continue
            for entry in entries:
                entry['binary'] = str(binary)
                entry['suites'] = len(suites)
                entry['cases'] = len(cases)
                if not entry['passed']:
                    failed = True
                if entry.get('processBoundary') == 'NATIVE_PROCESS_SESSION_NOT_DRAINED':
                    report['status'] = 'CLEANUP_FAILED'
                    report['targets'].append({'target': target['target'], 'runs': entries})
                    if arguments.report:
                        Path(arguments.report).write_text(json.dumps(report, indent=2) + '\n')
                    return EXIT_FAILED
            report['targets'].append({'target': target['target'], 'binary': str(binary),
                                      'suites': len(suites), 'cases': len(cases),
                                      'caseIds': cases, 'defaultDisabled': disabled,
                                      'profileOwners': owners,
                                      'runs': entries})

    # `discovered` is the binary's listing; `selected` is what the selector announced. They can
    # differ by one -- `integration-tests` lists 345 cases and announces 346 under `--run_test=*`,
    # because Boost's explicit filter also matches the module-level node that the default run does
    # not count. Both are measured; the gap is recorded rather than smoothed away.
    report['discovered'] = sum({target['binary']: target.get('cases', 0)
                                for target in report['targets'] if 'binary' in target}.values())
    # A selector chooses a subset of the binary; its own count comes from the selector's listing, not
    # from the binary's total. `run_target` records what each selector actually reached.
    report['selectedCases'] = sum(run.get('selected', 0)
                                  for target in report['targets']
                                  for run in target.get('runs', []))
    report['executed'] = sum(run.get('executed', 0)
                             for target in report['targets']
                             for run in target.get('runs', []))
    report['failed'] = sum(run.get('failed', 0)
                           for target in report['targets']
                           for run in target.get('runs', []))
    report['skipped'] = sum(run.get('skipped', 0) for target in report['targets']
                            for run in target.get('runs', []))
    report['unavailable'] = sum(run.get('unavailable', 0) for target in report['targets']
                                for run in target.get('runs', []))
    report['status'] = 'FAILED' if failed else 'PASSED'
    if arguments.report:
        Path(arguments.report).write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k: report[k] for k in ('status', 'profile', 'discovered', 'selectedCases',
                                              'executed', 'failed', 'unavailable')}))
    return EXIT_FAILED if failed else EXIT_OK


if __name__ == '__main__':
    sys.exit(main())
