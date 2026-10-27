from __future__ import annotations

import importlib.util
import json
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]
SCRIPTS = ROOT / "Experiments/TigerCluster/adapters/slurm-apptainer/scripts"


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    obj = importlib.util.module_from_spec(spec)
    sys.modules[name] = obj
    spec.loader.exec_module(obj)
    return obj


handoff = load("test_handoff", SCRIPTS / "prepare-development-handoff.py")
sealer = load("test_handoff_sealer", SCRIPTS / "prepare-local-sif-source.py")


def test_archive_includes_native_pkgconfig_build_inputs():
    """Both selected native libraries must carry their Waf substitution inputs."""
    selected = set(sealer.selected_files(ROOT))
    for relative in ("libndn-service-framework.pc.in",
                     "NDNSF-DistributedInference/ndnsf-distributed-inference.pc.in",
                     "NDNSF-DistributedInference/ndnsf_di_launcher.py"):
        assert Path(relative) in selected


def commit_fixture(path):
    for command in [
        ["git", "init", "-q", str(path)],
        ["git", "-C", str(path), "add", "--all"],
        ["git", "-C", str(path), "-c", "user.name=Fixture", "-c",
         "user.email=fixture@example.invalid", "commit", "-qm", "fixture"],
    ]:
        subprocess.run(command, check=True, capture_output=True)
    return handoff.git(path, "rev-parse", "HEAD")


@pytest.fixture
def inputs(tmp_path):
    workspaces = {}
    revisions = {}
    for name, entries in [("ndnsf", sealer.FILES), ("ndnSvs", sealer.NDN_SVS_FILES),
                          ("nacAbe", sealer.NAC_ABE_FILES), ("ndnSd", sealer.NDNSD_FILES)]:
        folder = tmp_path / name
        folder.mkdir()
        for entry in entries:
            dest = folder / entry
            source_root = ROOT if name == "ndnsf" else ROOT.parent / {"ndnSvs": "ndn-svs", "nacAbe": "NAC-ABE", "ndnSd": "NDNSD"}[name]
            if (source_root / entry).is_dir():
                dest.mkdir(parents=True, exist_ok=True)
                (dest / "fixture.hpp").write_text("// sealed dependency fixture\n")
            else:
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.write_text("pass\n" if dest.name == "wscript" else "fixture\n")
        if name == "ndnSvs":
            (folder / "wscript").write_text("VERSION = '0.1.0'\nGIT_TAG_PREFIX = 'ndn-svs-'\n")
            (folder / "VERSION.info").unlink()
        revisions[name] = commit_fixture(folder)
        workspaces[name] = folder
    # Source identities/receipts below are synthetic, not native crypto evidence.
    workspace = workspaces['ndnsf']
    for path in (ROOT / 'patches/openabe').iterdir():
        if path.name == 'series.json' or path.suffix == '.patch':
            destination = workspace / 'patches/openabe' / path.name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path, destination)
    series = json.loads((ROOT / 'patches/nac-abe/series.json').read_text())
    series['upstreamCommit'] = revisions['nacAbe']
    for entry in series['patches']:
        target = workspace / 'patches/nac-abe' / entry['file']
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / 'patches/nac-abe' / entry['file'], target)
    (workspace / 'patches/nac-abe/series.json').write_text(json.dumps(series))
    host_lock = json.loads((ROOT / 'packaging/host-dependencies.lock.json').read_text())
    host_lock['sources']['NAC-ABE']['commit'] = revisions['nacAbe']
    host_lock['sources']['NAC-ABE']['patch_series']['sha256'] = handoff.digest(
        workspace / 'patches/nac-abe/series.json')[7:]
    (workspace / 'packaging/host-dependencies.lock.json').write_text(json.dumps(host_lock))
    shutil.copyfile(ROOT / 'scripts/stack_sources.py', workspace / 'scripts/stack_sources.py')
    revisions['ndnsf'] = commit_fixture(workspace)
    wheels = tmp_path / "wheel-inputs"
    wheels.mkdir()
    for filename in handoff.REQUIRED_WHEELS:
        (wheels / filename).write_bytes(b"pinned external wheel fixture")
    base = tmp_path / "base.sif"
    base.write_bytes(b"pinned external base fixture")
    native = tmp_path / 'native-inputs'
    native.mkdir()
    for filename in handoff.NATIVE_FILES:
        (native / filename).write_bytes(b'unit native source fixture')
    lock = tmp_path / "lock.json"
    lock.write_text(json.dumps({
        "schema": handoff.LOCK_SCHEMA, "release": "fixture-r1",
        "repositories": {name: {"revision": value} for name, value in revisions.items()},
        "wheels": [{"filename": wheel.name, "sha256": handoff.digest(wheel)}
                   for wheel in sorted(wheels.iterdir())],
        "baseSif": {"sha256": handoff.digest(base)},
        'nativeBuild': {'files': {p.name: handoff.digest(p) for p in native.iterdir()}},
    }))
    return lock, workspaces, wheels, base


@pytest.mark.parametrize("stale_version", [False, True])
def test_source_archives_survive_relocation_and_reject_tampering(inputs, tmp_path, stale_version):
    lock, workspaces, wheels, _ = inputs
    version_file = workspaces['ndnSvs'] / 'VERSION.info'
    if stale_version:
        version_file.write_text('stale-host-version')
    original = tmp_path / "original"
    result = handoff.prepare(lock, workspaces, wheels, original)
    assert result["status"] == "SOURCE_READY"
    assert result["sifBuild"] == "NOT_RUN"
    seal = json.loads((original / "source/source-seal.json").read_text())
    assert set(seal["dependencies"]) == {"nacAbe", "ndnSvs", "ndnSd"}
    with tarfile.open(original / "source/ndn-svs.tar") as source:
        expected = '0.1.0+git.' + handoff.git(workspaces['ndnSvs'], 'rev-parse', '--short=8', 'HEAD')
        assert source.extractfile("VERSION.info").read().decode() == expected
    if stale_version:
        assert version_file.read_text() == 'stale-host-version'
    else:
        assert not version_file.exists()
    with tarfile.open(original / "source/nacAbe.tar") as source:
        assert source.extractfile("src/fixture.hpp").read() == b"// sealed dependency fixture\n"
    relocated = tmp_path / "relocated"
    shutil.move(str(original), relocated)
    assert handoff.verify(relocated)["sourceSealDigest"] == seal["sealDigest"]
    archive = relocated / "source/nacAbe.tar"
    with archive.open("ab") as stream:
        stream.write(b"altered")
    with pytest.raises(ValueError, match="FILE_DIGEST"):
        handoff.verify(relocated)


@pytest.mark.parametrize("change", ["revision", "dirty", "untracked", "wheel", "missing-wheel", "native", "missing-native"])
def test_prepare_rejects_unpinned_inputs_before_output(inputs, tmp_path, change):
    lock, workspaces, wheels, _ = inputs
    if change == "revision":
        data = json.loads(lock.read_text())
        data["repositories"]["ndnSvs"]["revision"] = "0" * 40
        lock.write_text(json.dumps(data))
    elif change == "missing-wheel":
        data = json.loads(lock.read_text())
        data["wheels"] = [row for row in data["wheels"] if not row["filename"].startswith("python_ndn-")]
        lock.write_text(json.dumps(data))
    elif change == "dirty":
        (workspaces["ndnSvs"] / "wscript").write_text("changed\n")
    elif change == "untracked":
        (workspaces["ndnSvs"] / "ndn-svs/uncommitted.hpp").write_text("uncommitted\n")
    elif change == 'native':
        (tmp_path / 'native-inputs/onnx.tar').write_bytes(b'changed source archive')
    elif change == 'missing-native':
        (tmp_path / 'native-inputs/vendor.tar').unlink()
    else:
        next(wheels.iterdir()).write_bytes(b"wrong wheel")
    output = tmp_path / "rejected"
    with pytest.raises(ValueError):
        handoff.prepare(lock, workspaces, wheels, output)
    assert not output.exists()


def test_verifier_rejects_unlisted_payload_and_false_revision(inputs, tmp_path):
    lock, workspaces, wheels, _ = inputs
    bundle = tmp_path / "bundle"
    handoff.prepare(lock, workspaces, wheels, bundle)
    added = bundle / "wheels/host.so"
    added.write_bytes(b"host native payload")
    with pytest.raises(ValueError, match="UNEXPECTED_FILE"):
        handoff.verify(bundle)
    added.unlink()
    path = bundle / "handoff.json"
    manifest = json.loads(path.read_text())
    manifest["repositories"]["nacAbe"]["revision"] = "f" * 40
    path.write_text(json.dumps(manifest))
    with pytest.raises(ValueError, match="LOCK_MISMATCH"):
        handoff.verify(bundle)


def test_render_checks_base_identity_and_preserves_package(inputs, tmp_path):
    lock, workspaces, wheels, base = inputs
    bundle = tmp_path / "bundle"
    handoff.prepare(lock, workspaces, wheels, bundle)
    wrong = tmp_path / "wrong.sif"
    wrong.write_bytes(b"wrong base")
    destination = tmp_path / "runtime.def"
    with pytest.raises(ValueError, match="BASE_DIGEST"):
        handoff.render(bundle, wrong, destination)
    assert not destination.exists()
    result = handoff.render(bundle, base, destination)
    assert result["status"] == "SOURCE_READY"
    assert result["hostGateManifest"] == "REQUIRED_FOR_RELEASE_PROMOTION"
    assert result["buildOnlySupported"] is True
    text = destination.read_text()
    assert "@BUNDLE@" not in text and "@BASE_SIF@" not in text
    # External sources remain sealed in the handoff for base construction,
    # but the NDNSF consumer inherits their verified products from that base.
    assert str(bundle / "source/nacAbe.tar") not in text
    assert 'BASE_DEPENDENCY_IDENTITY_MISMATCH' in text
    assert handoff.verify(bundle)["sifBuild"] == "NOT_RUN"
    with pytest.raises(FileExistsError):
        handoff.render(bundle, base, destination)


def test_sealer_keeps_legacy_and_canonical_workload_paths(inputs):
    _, workspaces, _, _ = inputs
    repo = workspaces['ndnsf']
    old = repo / 'packaging/ndnsf-di-container/jobs'
    canonical = repo / 'Experiments/TigerCluster/jobs'
    assert canonical.is_dir()
    shutil.rmtree(old)
    old.symlink_to('../../Experiments/TigerCluster/jobs', target_is_directory=True)
    selected = {path.as_posix() for path in sealer.selected_files(repo)}
    assert 'packaging/ndnsf-di-container/jobs/spec175/workload.json' in selected
    assert 'Experiments/TigerCluster/jobs/spec175/workload.json' in selected


def runtime_inputs(inputs, tmp_path):
    _, workspaces, _, base = inputs
    workspace = workspaces['ndnsf']
    source_hashes = {}
    for name in handoff.TOKENIZER_FILES:
        path = workspace / (handoff.TOKENIZER_PREFIX + name)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('frozen bridge source ' + name)
        source_hashes['crate/' + name] = handoff.digest(path).split(':', 1)[1]
    commit_fixture(workspace)
    native = tmp_path / 'runtime-native.json'
    native.write_text(json.dumps({'schema': 'ndnsf-native-build-inputs-v1'}))
    sdk = tmp_path / 'runtime-sdk.json'
    nac_files = {name: handoff.digest(workspace / name) for name in (
        'packaging/host-dependencies.lock.json', 'scripts/stack_sources.py',
        *[str(path.relative_to(workspace)) for directory in ('nac-abe', 'openabe')
          for path in (workspace / 'patches' / directory).iterdir()
          if path.name == 'series.json' or path.suffix == '.patch'])}
    host_lock = json.loads((workspace / 'packaging/host-dependencies.lock.json').read_text())
    sdk.write_text(json.dumps({'schema': 'ndnsf-base-sdk-v1', 'inputs': {'files': {
        'native/native-inputs.json': handoff.digest(native).split(':', 1)[1]},
        'repositories': {'nacAbe': {'revision': host_lock['sources']['NAC-ABE']['commit']}}},
        'artifacts': {'/opt/ndn-base/lib/libndnsf_tokenizer_bridge.a': 'a' * 64,
                      '/opt/ndn-base/lib/libnac-abe.so': 'b' * 64},
        'nacAbePatchClosure': {'abi': 'VERIFIED', 'nativeTests': 'PASS',
            'nativeSuites': ['TestProducerBoundCk', 'TestConsumerRevocation', 'TestConsumer', 'TestProducer'],
            'installedFiles': {'/opt/ndn-base/lib/libnac-abe.so': 'b' * 64},
            'upstreamCommit': host_lock['sources']['NAC-ABE']['commit'],
            'patchSeriesSha256': nac_files['patches/nac-abe/series.json'][7:],
            'hostDependencyLockSha256': nac_files['packaging/host-dependencies.lock.json'][7:],
            'sourceFiles': nac_files},
        'tokenizerBridge': {'status': 'VERIFIED', 'nativeProbes': 'PASS',
                            'archiveSha256': 'a' * 64, 'inputs': source_hashes}}))
    return workspaces['ndnsf'], base, sdk, native


@pytest.mark.parametrize('mutation', ['missing', 'library', 'suite', 'source', 'series', 'upstream'])
def test_runtime_handoff_requires_matching_nac_native_closure(inputs, tmp_path, mutation):
    workspace, base, sdk, native = runtime_inputs(inputs, tmp_path)
    body = json.loads(sdk.read_text())
    closure = body['nacAbePatchClosure']
    if mutation == 'missing':
        del body['nacAbePatchClosure']
    elif mutation == 'library':
        closure['installedFiles']['/opt/ndn-base/lib/libnac-abe.so'] = 'c' * 64
    elif mutation == 'suite':
        closure['nativeSuites'].pop()
    elif mutation == 'source':
        closure['sourceFiles']['scripts/stack_sources.py'] = 'sha256:' + 'c' * 64
    elif mutation == 'series':
        closure['patchSeriesSha256'] = 'c' * 64
    else:
        closure['upstreamCommit'] = 'c' * 40
    sdk.write_text(json.dumps(body))
    output = tmp_path / 'rejected-nac'
    with pytest.raises(ValueError, match='HANDOFF_NAC_'):
        handoff.prepare_runtime(workspace, base, sdk, native, output)
    assert not output.exists()


def test_runtime_handoff_needs_only_ndnsf_and_selected_base(inputs, tmp_path):
    workspace, base, sdk, native = runtime_inputs(inputs, tmp_path)
    # Generic preparation must not read the historical external checkouts.
    for name, path in inputs[1].items():
        if name != 'ndnsf':
            shutil.rmtree(path)
    bundle = tmp_path / 'runtime-bundle'
    result = handoff.prepare_runtime(workspace, base, sdk, native, bundle)
    assert result['runtimeProfile'] == 'di-runtime'
    assert result['sifBuild'] == result['runtimeQualification'] == 'NOT_RUN'
    lock = handoff.load_lock(bundle / 'dependency-lock.json')
    assert lock['schema'] == handoff.RUNTIME_LOCK_SCHEMA
    assert set(lock['repositories']) == {'ndnsf'}
    assert set(lock['nativeBuild']['files']) == {'native-inputs.json'}
    assert not list(bundle.rglob('*.whl'))
    assert not (bundle / 'source/nacAbe.tar').exists()
    relocated = tmp_path / 'runtime-relocated'
    shutil.move(str(bundle), relocated)
    assert handoff.verify(relocated)['sourceSealDigest'] == result['sourceSealDigest']
    definition = tmp_path / 'runtime.def'
    handoff.render(relocated, base, definition)
    text = definition.read_text()
    assert 'NDNSF_RUNTIME_PROFILE=di-runtime' in text
    assert handoff.digest(sdk) in text
    assert handoff.digest(relocated / 'dependency-lock.json') in text
    assert '@RUNTIME_PROFILE@' not in text
    (relocated / 'native/native-inputs.json').write_text('changed')
    with pytest.raises(ValueError, match='FILE_DIGEST'):
        handoff.verify(relocated)


@pytest.mark.parametrize('change', ['missing-native', 'wrong-binding', 'wrong-schema', 'symlink',
                                  'missing-tokenizer', 'changed-bridge-source'])
def test_runtime_handoff_rejects_bad_inputs_before_output(inputs, tmp_path, change):
    workspace, base, sdk, native = runtime_inputs(inputs, tmp_path)
    if change == 'missing-native':
        native.unlink()
    elif change == 'symlink':
        link = tmp_path / 'linked-base.sif'
        link.symlink_to(base)
        base = link
    else:
        body = json.loads(sdk.read_text())
        if change == 'wrong-schema':
            body['schema'] = 'unsupported'
        elif change == 'missing-tokenizer':
            del body['artifacts']['/opt/ndn-base/lib/libndnsf_tokenizer_bridge.a']
        elif change == 'changed-bridge-source':
            (workspace / (handoff.TOKENIZER_PREFIX + 'src/lib.rs')).write_text('changed source')
        else:
            body['inputs']['files']['native/native-inputs.json'] = '0' * 64
        sdk.write_text(json.dumps(body))
    output = tmp_path / 'rejected-runtime'
    with pytest.raises(ValueError):
        handoff.prepare_runtime(workspace, base, sdk, native, output)
    assert not output.exists()


def test_runtime_handoff_rejects_profile_lock_mismatch(inputs, tmp_path):
    workspace, base, sdk, native = runtime_inputs(inputs, tmp_path)
    bundle = tmp_path / 'runtime-bundle'
    handoff.prepare_runtime(workspace, base, sdk, native, bundle)
    report = bundle / 'handoff.json'
    body = json.loads(report.read_text())
    body['runtimeProfile'] = handoff.LEGACY_PROFILE
    report.write_text(json.dumps(body))
    with pytest.raises(ValueError, match='MANIFEST_FILES|PROFILE_LOCK'):
        handoff.verify(bundle)
