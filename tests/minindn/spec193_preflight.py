"""Side-effect-free candidate checks for the shared Spec202 MiniNDN campaign."""

import hashlib
import fnmatch
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path


DESIGN_FILES = (
    "AGENTS.md",
    ".specify/feature.json",
    ".specify/memory/constitution.md",
    "specs/202-distributedrepo-convergence/spec.md",
    "specs/202-distributedrepo-convergence/plan.md",
    "specs/202-distributedrepo-convergence/tasks.md",
    "specs/202-distributedrepo-convergence/validation-matrix.md",
    "specs/202-distributedrepo-convergence/api-inventory.md",
    "specs/202-distributedrepo-convergence/contracts/architecture-and-api.md",
    "specs/202-distributedrepo-convergence/contracts/candidate-and-validation.md",
    "tests/minindn/run_spec192_real_cpp_repo.py",
    "tests/minindn/spec193_preflight.py",
)

# These owners supply the production native libraries and Python consumers.
# Inventory clean files too: committing an input must not remove it from a lock.
SOURCE_DIRECTORIES = (
    "ndn-service-framework", "NDNSF-DistributedRepo",
    "NDNSF-DistributedInference", "ndnsf-di", "pythonWrapper", ".waf-tools",
    "examples/python/NDNSF-DistributedRepo", "tests/fixtures",
)
SOURCE_SUFFIXES = {".cpp", ".hpp", ".h", ".proto", ".py", ".yaml", ".sql", ".conf", ".json"}
SOURCE_EXCLUDED_PARTS = {"build", "dist", "__pycache__", ".pytest_cache", "evidence", "results"}
SOURCE_REQUIRED = (
    "waf", "wscript", "tests/wscript", "tests/main.cpp",
    "NDNSF-DistributedRepo/wscript",
    "libndn-service-framework.pc.in",
    "NDNSF-DistributedInference/ndnsf-distributed-inference.pc.in",
    "NDNSF-DistributedRepo/src/LegacyRepoImporter.cpp",
    "NDNSF-DistributedRepo/src/ArtifactRepositoryApi.cpp",
    "NDNSF-DistributedRepo/src/RepoClient.cpp",
    "NDNSF-DistributedRepo/src/RepoNode.cpp",
    "NDNSF-DistributedRepo/src/RepoProtocol.cpp",
    "NDNSF-DistributedRepo/pythonWrapper/setup.py", "pythonWrapper/setup.py",
    "Experiments/NDNSF_DistributedRepo_Generic_Minindn.py",
    "tests/standalone/spec192-repo-client.cpp",
    "tests/standalone/spec193-object-repo-server.cpp",
    "tests/standalone/spec202-openabe-policy-lifecycle.cpp",
    "tests/fixtures/spec202-syscall-fault.hpp",
    "tests/fixtures/spec202-import-lock.hpp",
    "tests/minindn/run_spec192_real_cpp_repo.py",
    "tests/minindn/spec193_preflight.py",
    "tests/minindn/spec193_packet_oracle.py",
    "tests/minindn/spec192_binding_client.py",
    "tests/minindn/spec192_di_peer.py",
    "tests/minindn/spec193_retired_python_peer.py",
    "tests/minindn/spec202_build_candidate.py",
    "tests/integration-tests/ndnsf-integration-fixture.cpp",
    "tests/integration-tests/ndnsf-integration-fixture.hpp",
    "tests/python/test_ndnsf_core_boundary_imports.py",
    "tests/python/test_ndnsf_app_core_envelope_migration.py",
)
SOURCE_PATTERNS = (
    "tests/unit-tests/*repo*.cpp", "tests/unit-tests/spec193*.cpp",
    "tests/integration-tests/spec192*.cpp", "tests/python/*repo*.py",
    "tests/python/test_spec192*.py", "tests/python/test_spec164*.py",
    "tests/minindn/spec193*.conf",
    "tests/minindn/test_spec*preflight.py", "tests/minindn/test_spec202*.py",
)
BUILD_TARGETS = (
    "ndn-service-framework", "ndnsf-distributed-inference",
    "DistributedRepoServer", "ndnsf-repo-import", "spec192-repo-client",
    "spec193-object-repo-server", "spec202-repo-conformance",
    "spec192-repo-inapp-api", "spec192-repo-persistent-service",
    "spec192-repo-persistent-api", "spec192-repo-legacy-import-unit",
    "spec192-repo-legacy-import-integration", "spec193-original-data-receipt",
)
BUILD_OUTPUTS = (
    "libndn-service-framework.so", "libndnsf-distributed-inference.so",
    "NDNSF-DistributedRepo/libndnsf-distributed-repo.a",
    "NDNSF-DistributedRepo/DistributedRepoServer",
    "NDNSF-DistributedRepo/ndnsf-repo-import", *BUILD_TARGETS[4:],
)
PYTHON_IMPORT_PROBE = """import json,pathlib
import ndnsf._ndnsf, py_repoclient._py_repoclient
paths=set()
for line in pathlib.Path('/proc/self/maps').read_text().splitlines():
    fields=line.split()
    if len(fields)>=6 and fields[-1].startswith('/') and '.so' in fields[-1]:
        paths.add(str(pathlib.Path(fields[-1]).resolve()))
print(json.dumps(sorted(paths)))
"""


def candidate_build_commands(root, fresh, source, build, python):
    """Canonical commands shared by the rebuild owner and receipt admission."""
    return [
        ("configure", [str(root / "waf"), "configure", "--out=" + fresh.name,
                       "--with-tests", "--with-examples",
                       "--ndn-svs-source-tree=" + str(source),
                       "--ndn-svs-build-tree=" + str(build)], root),
        ("native-build", [str(root / "waf"), "build", "-j2",
                          "--targets=" + ",".join(BUILD_TARGETS)], root),
        ("native-install", ["sudo", "-n", "/usr/bin/env",
                            "WAFLOCK=.lock-waf_" + fresh.name,
                            "NDNSF_SKIP_DEV_PIP_INSTALL=1", str(root / "waf"),
                            "install", "-j2",
                            "--targets=ndn-service-framework,ndnsf-distributed-inference"], root),
        ("base-binding", [python, "setup.py", "build_ext", "--inplace", "--force",
                          "--parallel=2", "--build-temp=" + str(fresh / "python-base")], root / "pythonWrapper"),
        ("repo-binding", [python, "setup.py", "build_ext", "--inplace", "--force",
                          "--parallel=2", "--build-temp=" + str(fresh / "python-repo")], root / "NDNSF-DistributedRepo/pythonWrapper"),
        ("runtime-import", [python, "-c", PYTHON_IMPORT_PROBE], root),
    ]


def candidate_build_environment(root, fresh, source, build, bindings=False):
    values = {"WAFLOCK": ".lock-waf_" + fresh.name, "CFLAGS": "-O0 -g0 -MD",
              "CXXFLAGS": "-O0 -g0 -MD", "CC": "/usr/bin/gcc", "CXX": "/usr/bin/g++"}
    if bindings:
        values.update(
            NDNSF_LIBRARY_DIR="/usr/local/lib",
            NDNSF_GLOBAL_NATIVE_DIGESTS=json.dumps({
                name: sha256(Path("/usr/local/lib") / name) for name in (
                    "libndn-service-framework.so", "libndnsf-distributed-inference.so")}),
            NDNSF_NDN_SVS_SOURCE_TREE=str(source), NDNSF_NDN_SVS_BUILD_TREE=str(build),
            LD_LIBRARY_PATH=":".join((str(build), str(root / "build"),
                                      "/usr/local/lib", "/opt/gstreamer-1.20/lib")),
            PYTHONPATH=":".join((str(root / "pythonWrapper"),
                                 str(root / "NDNSF-DistributedRepo/pythonWrapper"),
                                 str(root / "NDNSF-DistributedInference"))))
    return values


def design_acceptance_digest(path):
    """Exclude only explicitly declared qualification progress from identity."""
    path = require_file(path, "DESIGN")
    if path.name not in ("tasks.md", "validation-matrix.md"):
        return sha256(path)
    try:
        text = path.read_text(encoding="utf-8")
    except UnicodeDecodeError as error:
        raise PreflightError("MALFORMED_DESIGN", "invalid document encoding") from error
    label = "PROGRESS" if path.name == "tasks.md" else "QUALIFICATION STATUS"
    start = "<!-- SPEC202 " + label + " START -->"
    end = "<!-- SPEC202 " + label + " END -->"
    if text.count(start) != 1 or text.count(end) != 1 or text.index(start) >= text.index(end):
        raise PreflightError("MALFORMED_DESIGN", "invalid progress markers: " + str(path))
    before, progress = text.split(start)
    progress, after = progress.split(end)
    if path.name == "tasks.md":
        if re.findall(r"(?m)^## .+$", progress) != [
                "## Execution Progress", "## Current Checkpoint"]:
            raise PreflightError("MALFORMED_DESIGN", "unexpected task progress sections")
        text = before + start + end + after
        ids = re.findall(r"(?m)^- \[[ x]\] (T\d{3})\b", text)
        if (set(ids) != {"T%03d" % number for number in range(1, 11)} or len(ids) != 10 or
                "## Design binding" not in text or "## Dependencies & Execution Order" not in text):
            raise PreflightError("MALFORMED_DESIGN", "missing or duplicate acceptance tasks")
        text = re.sub(r"(?m)^- \[[ x]\] (T\d{3})\b", r"- [ ] \1", text)
    else:
        if len(progress.strip().splitlines()) != 1 or not progress.strip().startswith("**Status**:"):
            raise PreflightError("MALFORMED_DESIGN", "unexpected matrix progress content")
        text = before + start + end + after
        headers = (
            "| ID | Scenario / public entry | Independent oracle | Closing owner | Round 1 | Round 2 |",
            "| ID | Production entry / parameter classes | Required oracle | Task | Status |")
        if any(text.count(header) != 1 for header in headers):
            raise PreflightError("MALFORMED_DESIGN", "invalid acceptance matrix headers")
        ids, lines = [], []
        for line in text.splitlines(keepends=True):
            match = re.match(r"\|\s*([AOLNPR]\d{2})\s*\|", line)
            if match:
                case = match.group(1)
                cells = line.rstrip("\n").split("|")
                native = case.startswith("R")
                if len(cells) != (7 if native else 8):
                    raise PreflightError("MALFORMED_DESIGN", "invalid matrix row: " + case)
                ids.append(case)
                cells[-2] = " PROGRESS "
                if not native:
                    cells[-3] = " PROGRESS "
                line = "|".join(cells) + ("\n" if line.endswith("\n") else "")
            lines.append(line)
        expected = {prefix + "%02d" % number for prefix, count in (
            ("A", 22), ("O", 17), ("L", 4), ("N", 10), ("P", 6), ("R", 7))
                    for number in range(1, count + 1)}
        if set(ids) != expected or len(ids) != len(expected):
            raise PreflightError("MALFORMED_DESIGN", "missing or duplicate acceptance cases")
        text = "".join(lines)
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def participates_in_source(name):
    path = Path(name)
    if set(path.parts) & SOURCE_EXCLUDED_PARTS:
        return False
    return (name in SOURCE_REQUIRED or
            any(fnmatch.fnmatchcase(name, pattern) for pattern in SOURCE_PATTERNS) or
            (path.suffix in SOURCE_SUFFIXES and
             any(name.startswith(directory + "/") for directory in SOURCE_DIRECTORIES)))


def source_candidate(root):
    """Hash participating source and reject missing inputs without side effects."""
    root = root.resolve()
    names = set(SOURCE_REQUIRED)
    for directory in SOURCE_DIRECTORIES:
        if not (root / directory).is_dir():
            raise PreflightError("MISSING_SOURCE", directory)
        for path in (root / directory).rglob("*"):
            relative = path.relative_to(root)
            if path.is_file() and path.suffix in SOURCE_SUFFIXES and not (
                    set(relative.parts) & SOURCE_EXCLUDED_PARTS):
                names.add(relative.as_posix())
    for pattern in SOURCE_PATTERNS:
        names.update(path.relative_to(root).as_posix() for path in root.glob(pattern))
    # HEAD preserves staged deletions; the index also covers newly staged files.
    # Use the same predicate for clean discovery and missing-input admission.
    for command in (["git", "ls-tree", "-r", "--name-only", "-z", "HEAD"],
                    ["git", "ls-files", "--cached", "-z"]):
        registered = subprocess.check_output(command, cwd=str(root)).decode().split("\0")
        names.update(name for name in filter(None, registered) if participates_in_source(name))
    state = hashlib.sha256()
    for name in sorted(names):
        path = require_file(root / name, "SOURCE")
        state.update(name.encode("utf-8") + b"\0")
        state.update(bytes.fromhex(sha256(path)))
        state.update(b"\n")
    return state.hexdigest(), sorted(names)


class PreflightError(RuntimeError):
    def __init__(self, failure_class, detail):
        super().__init__(detail)
        self.failure_class = failure_class


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def require_file(path, kind):
    if not path.is_file():
        raise PreflightError("MISSING_" + kind, str(path))
    return path.resolve()


def topology_hosts(path):
    section = None
    hosts = set()
    links = []
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1].strip().lower()
            continue
        if section == "nodes":
            name = line.split(":", 1)[0].strip()
            if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_-]*", name):
                raise PreflightError("TOPOLOGY_NODE", line)
            if name in hosts:
                raise PreflightError("DUPLICATE_TOPOLOGY_NODE", name)
            hosts.add(name)
        elif section == "links":
            pair = line.split(None, 1)[0].split(":")
            if len(pair) != 2 or pair[0] == pair[1]:
                raise PreflightError("TOPOLOGY_LINK", line)
            links.append(pair)
    for pair in links:
        if any(name not in hosts for name in pair):
            raise PreflightError("UNKNOWN_LINK_NODE", ":".join(pair))
    if not links:
        raise PreflightError("TOPOLOGY_LINK", "no links")
    return hosts


def resolved_libraries(binary):
    ldd = shutil.which("ldd")
    if not ldd:
        raise PreflightError("MISSING_TOOL", "ldd")
    result = subprocess.run([ldd, str(binary)], text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.returncode or "not found" in result.stdout.lower():
        raise PreflightError("UNRESOLVED_LIBRARY", str(binary) + "\n" + result.stdout)
    libraries = {}
    for raw in result.stdout.splitlines():
        line = raw.strip()
        if "=>" in line:
            target = line.split("=>", 1)[1].strip().split()[0]
        elif line.startswith("/"):
            target = line.split()[0]
        else:
            continue
        path = require_file(Path(target), "LIBRARY")
        libraries[str(path)] = sha256(path)
    return libraries


def verify_build_receipt(root, source_sha, source_files, binaries, files):
    """Validate an actual rebuild receipt; this admission check never creates one."""
    path = require_file(root / "build/spec202-source-build-receipt.json", "BUILD_RECEIPT")
    try:
        receipt = json.loads(path.read_text(encoding="utf-8"))
        if (receipt["spec"] != "202" or receipt["source_before"] != source_sha or
                receipt["source_after"] != source_sha or receipt["source_files"] != source_files):
            raise PreflightError("STALE_BUILD_RECEIPT", "source changed since rebuild")
        steps = receipt["steps"]
        if [step["step"] for step in steps] != [
                "configure", "native-build", "native-install", "base-binding",
                "repo-binding", "runtime-import"]:
            raise ValueError("incomplete successful build steps")
        for step in steps:
            if step["exit_status"] != 0 or not step["argv"]:
                raise ValueError("failed or missing build command")
            log = require_file(Path(step["log"]), "BUILD_LOG")
            if sha256(log) != step["log_sha256"]:
                raise PreflightError("STALE_BUILD_RECEIPT", "build log changed: " + str(log))
        configuration = receipt["configuration"]
        if len(configuration) != 2:
            raise ValueError("configuration must bind cache and generated header")
        cache = next(Path(name) for name in configuration if name.endswith("/c4che/_cache.py"))
        fresh = cache.parent.parent
        if (fresh.parent != root or not fresh.name.startswith("build-spec202-candidate") or
                set(configuration) != {str(fresh / "config.hpp"), str(cache)}):
            raise ValueError("unrecognized fresh build configuration")
        if "--out=" + fresh.name not in steps[0]["argv"]:
            raise ValueError("configure output does not match fresh configuration")
        if "--targets=" + ",".join(BUILD_TARGETS) not in steps[1]["argv"]:
            raise ValueError("native build does not cover all participating targets")
        source_options = [item.split("=", 1)[1] for item in steps[0]["argv"]
                          if item.startswith("--ndn-svs-source-tree=")]
        build_options = [item.split("=", 1)[1] for item in steps[0]["argv"]
                         if item.startswith("--ndn-svs-build-tree=")]
        if len(source_options) != 1 or len(build_options) != 1:
            raise ValueError("missing declared native SVS SDK pair")
        sdk_source, sdk_build = Path(source_options[0]), Path(build_options[0])
        commands = candidate_build_commands(root, fresh, sdk_source, sdk_build, sys.executable)
        for index, (label, command, cwd) in enumerate(commands):
            step = steps[index]
            if step["argv"] != command or step["cwd"] != str(cwd):
                raise ValueError("noncanonical build command/owner: " + label)
            expected_environment = candidate_build_environment(
                root, fresh, sdk_source, sdk_build, bindings=index >= 3)
            if step["environment"] != expected_environment:
                raise ValueError("noncanonical build/runtime environment: " + label)
        from spec202_build_candidate import sdk_inputs, compiler_dependencies
        if receipt["external_inputs"] != sdk_inputs(sdk_source, sdk_build):
            raise PreflightError("STALE_BUILD_RECEIPT", "external SDK inventory changed")
        if receipt["compiler_dependencies"] != compiler_dependencies(fresh, sdk_source, sdk_build):
            raise PreflightError("STALE_BUILD_RECEIPT", "compiler dependency proof changed")
        expected_fresh = {str(fresh / name) for name in BUILD_OUTPUTS}
        if set(receipt["fresh_outputs"]) != expected_fresh:
            raise ValueError("incomplete fresh native outputs")
        expected_artifacts = {str(root / "build" / name) for name in BUILD_OUTPUTS}
        expected_artifacts.update(str(binary) for binary in binaries)
        expected_artifacts.update("/usr/local/lib/" + name for name in (
            "libndn-service-framework.so", "libndnsf-distributed-inference.so"))
        if not expected_artifacts.issubset(receipt["artifacts"]):
            raise ValueError("incomplete native/binding artifact inventory")
        for category in ("fresh_outputs", "artifacts", "configuration", "external_inputs", "runtime"):
            inputs = receipt[category]
            if not isinstance(inputs, dict) or not inputs:
                raise ValueError("empty input category: " + category)
            for name, expected in inputs.items():
                actual = sha256(require_file(Path(name), "BUILD_INPUT"))
                if actual != expected:
                    raise PreflightError("STALE_BUILD_RECEIPT", category + " changed: " + name)
                files[name] = actual
        for name in BUILD_OUTPUTS:
            if receipt["fresh_outputs"][str(fresh / name)] != receipt["artifacts"][str(root / "build" / name)]:
                raise ValueError("harness output differs from fresh output: " + name)
        for binary in binaries:
            if sha256(binary) != receipt["artifacts"][str(binary)]:
                raise PreflightError("STALE_BUILD_RECEIPT", "runtime artifact changed")
            for library, digest in resolved_libraries(binary).items():
                if receipt["runtime"].get(library) != digest:
                    raise PreflightError("STALE_BUILD_RECEIPT", "loaded dependency changed: " + library)
    except PreflightError:
        raise
    except (ValueError, KeyError, TypeError, StopIteration, RuntimeError,
            OSError, subprocess.CalledProcessError) as error:
        raise PreflightError("INVALID_BUILD_RECEIPT", str(error)) from error
    files[str(path)] = sha256(path)


def verify_runtime_policy(path, service_prefix, operations, repo_users,
                          repo_providers, metadata_denied_user=None):
    import yaml

    config = yaml.safe_load(path.read_text(encoding="utf-8"))
    services = config.get("services", [])
    names = [service.get("name") for service in services]
    expected = {service_prefix + "/" + item for item in operations}
    actual = {name for name in names if isinstance(name, str) and
              name.startswith(service_prefix + "/")}
    if actual != expected or len(actual) != sum(name in expected for name in names):
        raise PreflightError("RUNTIME_ACL_SERVICES", str(sorted(actual ^ expected)))
    for service in services:
        if service.get("name") not in expected:
            continue
        users = service.get("users", [])
        providers = [item.get("identity") for item in service.get("providers", [])]
        expected_users = list(repo_users)
        if metadata_denied_user and service.get("name") in (
                service_prefix + "/DATA_RECEIPT",
                service_prefix + "/DATA_LOCATION"):
            expected_users.remove(metadata_denied_user)
        if users != expected_users or providers != list(repo_providers):
            raise PreflightError("RUNTIME_ACL_PRINCIPALS", str(service.get("name")))
        if service.get("model") != service.get("name"):
            raise PreflightError("RUNTIME_ACL_MODEL", str(service.get("name")))
    return sha256(path)


def check(root, output, topology, source_sha, source_files, roles,
          service_prefix, operations, repo_users, repo_providers,
          require_six=False, require_seven=False, object_mode=False, lock_path=None,
          acl_services=None, include_object_server=False, include_importer=False):
    """Return a lockable manifest without creating files or processes."""
    root = root.resolve()
    output = output.resolve()
    if output.exists():
        raise PreflightError("OUTPUT_EXISTS", str(output))
    pointer = json.loads(require_file(root / ".specify/feature.json", "DESIGN")
                         .read_text(encoding="utf-8"))
    if Path(pointer.get("feature_directory", "")).name != \
            "202-distributedrepo-convergence":
        raise PreflightError("ACTIVE_FEATURE", str(pointer.get("feature_directory")))
    expected_roles = dict(roles)
    if len(expected_roles) != len(set(expected_roles.values())):
        raise PreflightError("DUPLICATE_ROLE", str(expected_roles))
    if require_six and require_seven:
        raise PreflightError("TOPOLOGY_ROLE_MODE", "six and seven host modes conflict")
    if require_seven and (len(expected_roles) != 7 or
                          expected_roles.get("seattle") != "repoC" or
                          expected_roles.get("stanford") != "repoD"):
        raise PreflightError("SEVEN_HOST_ROLE", str(expected_roles))
    if require_six and (len(expected_roles) != 6 or expected_roles.get("seattle") != "repoC"):
        raise PreflightError("SIX_HOST_ROLE", str(expected_roles))
    if not require_six and not require_seven and len(expected_roles) != 5:
        raise PreflightError("FIVE_HOST_ROLE", str(expected_roles))
    topology = require_file(topology, "TOPOLOGY")
    hosts = topology_hosts(topology)
    if hosts != set(expected_roles):
        raise PreflightError("TOPOLOGY_ROLES", "expected {} found {}".format(
            sorted(expected_roles), sorted(hosts)))
    expected_services = {service_prefix + "/" + operation for operation in operations}
    if len(expected_services) != len(operations):
        raise PreflightError("DUPLICATE_OPERATION", str(operations))
    actual_services = set(acl_services if acl_services is not None else expected_services)
    if actual_services != expected_services:
        raise PreflightError("ACL_SERVICES", "missing {} extra {}".format(
            sorted(expected_services - actual_services), sorted(actual_services - expected_services)))
    if len(set(repo_users)) != len(repo_users) or len(set(repo_providers)) != len(repo_providers):
        raise PreflightError("DUPLICATE_PRINCIPAL", "Repo ACL principals")
    files = {}
    progress_provenance = {}
    for name in DESIGN_FILES:
        path = require_file(root / name, "DESIGN")
        files[name] = design_acceptance_digest(path)
        if path.name in ("tasks.md", "validation-matrix.md"):
            progress_provenance[name] = sha256(path)
    for name in source_files:
        path = require_file(root / name, "SOURCE")
        if name not in files:
            files[name] = sha256(path)
    actual_source_sha, actual_source_files = source_candidate(root)
    if source_sha != actual_source_sha or source_files != actual_source_files:
        raise PreflightError("SOURCE_CANDIDATE_MISMATCH", "source inventory/hash changed")
    server = next((item for item in (
        root / "build/NDNSF-DistributedRepo/DistributedRepoServer",
        root / "build/DistributedRepoServer") if item.is_file()), None)
    if server is None:
        raise PreflightError("MISSING_SERVER", "DistributedRepoServer")
    client = require_file(root / "build/spec192-repo-client", "CLIENT")
    extension_matches = sorted((root / "NDNSF-DistributedRepo/pythonWrapper/py_repoclient")
                               .glob("_py_repoclient*.so"))
    if len(extension_matches) != 1:
        raise PreflightError("MISSING_EXTENSION", "expected one py_repoclient extension")
    extension = require_file(extension_matches[0], "EXTENSION")
    base_extension_matches = sorted((root / "pythonWrapper/ndnsf").glob("_ndnsf*.so"))
    if len(base_extension_matches) != 1:
        raise PreflightError("MISSING_BASE_EXTENSION", "expected one _ndnsf extension")
    binaries = [server, client, extension,
                require_file(base_extension_matches[0], "BASE_EXTENSION")]
    if object_mode or include_object_server:
        binaries.append(require_file(root / "build/spec193-object-repo-server",
                                     "OBJECT_SERVER"))
    if include_importer:
        binaries.append(require_file(root / "build/NDNSF-DistributedRepo/ndnsf-repo-import", "LEGACY_IMPORTER"))
    for binary in binaries:
        files[str(binary)] = sha256(binary)
        files.update(resolved_libraries(binary))
    verify_build_receipt(root, source_sha, source_files, binaries, files)
    files[str(topology)] = sha256(topology)
    template = require_file(root / "examples/python/NDNSF-DistributedRepo/generic_object_store/repo_policy.yaml",
                            "POLICY_TEMPLATE")
    files[str(template)] = sha256(template)
    if importlib.util.find_spec("yaml") is None:
        raise PreflightError("MISSING_PYTHON_MODULE", "yaml")
    import yaml
    try:
        template_config = yaml.safe_load(template.read_text(encoding="utf-8"))
    except yaml.YAMLError as error:
        raise PreflightError("POLICY_TEMPLATE", str(error)) from error
    if not isinstance(template_config, dict) or any(
            not isinstance(template_config.get(section), expected_type)
            for section, expected_type in (("trust", dict), ("runtime", dict),
                                           ("identities", dict), ("services", list))):
        raise PreflightError("POLICY_TEMPLATE", "missing trust/runtime/identities/services")
    for tool in ("nfd", "nfdc", "nlsr", "ndnsec", "tcpdump", "tshark", "strace",
                 "c++", "pkg-config"):
        path = shutil.which(tool)
        if not path:
            raise PreflightError("MISSING_TOOL", tool)
        files[str(Path(path).resolve())] = sha256(Path(path).resolve())
    files[str(Path(sys.executable).resolve())] = sha256(Path(sys.executable).resolve())
    for module in ("yaml", "minindn", "py_repoclient", "ndnsf",
                   "ndn", "cryptography"):
        if importlib.util.find_spec(module) is None:
            raise PreflightError("MISSING_PYTHON_MODULE", module)
    state = {
        "spec": "202", "source_candidate_sha256": source_sha,
        "source_revision": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=str(root), text=True).strip(),
        "source_candidate_files": source_files,
        "roles": expected_roles, "services": sorted(expected_services),
        "object_protocol_server": object_mode or include_object_server,
        "legacy_importer": include_importer,
        "repo_users": list(repo_users), "repo_providers": list(repo_providers),
        "files": dict(sorted(files.items())),
        "progress_provenance": progress_provenance,
    }
    provenance_fields = ("source_revision", "progress_provenance")
    identity = {key: value for key, value in state.items() if key not in provenance_fields}
    encoded = json.dumps(identity, sort_keys=True, separators=(",", ":")).encode("utf-8")
    state["candidate_lock_sha256"] = hashlib.sha256(encoded).hexdigest()
    if lock_path is not None:
        locked = json.loads(require_file(lock_path, "CANDIDATE_LOCK")
                            .read_text(encoding="utf-8"))
        if {key: value for key, value in locked.items() if key not in provenance_fields} != \
                {key: value for key, value in state.items() if key not in provenance_fields}:
            raise PreflightError("CANDIDATE_MISMATCH", str(lock_path))
    return state
