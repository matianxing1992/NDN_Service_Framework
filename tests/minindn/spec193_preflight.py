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
    "NDNSF-DistributedInference", "pythonWrapper", ".waf-tools",
    "examples/python/NDNSF-DistributedRepo", "tests/fixtures",
)
SOURCE_SUFFIXES = {".cpp", ".hpp", ".h", ".proto", ".py", ".yaml", ".sql", ".conf"}
SOURCE_EXCLUDED_PARTS = {"build", "dist", "__pycache__", ".pytest_cache", "evidence", "results"}
SOURCE_REQUIRED = (
    "waf", "wscript", "tests/wscript", "tests/main.cpp",
    "NDNSF-DistributedRepo/wscript",
    "NDNSF-DistributedRepo/src/LegacyRepoImporter.cpp",
    "NDNSF-DistributedRepo/src/ArtifactRepositoryApi.cpp",
    "NDNSF-DistributedRepo/src/RepoClient.cpp",
    "NDNSF-DistributedRepo/src/RepoNode.cpp",
    "NDNSF-DistributedRepo/src/RepoProtocol.cpp",
    "NDNSF-DistributedRepo/pythonWrapper/setup.py", "pythonWrapper/setup.py",
    "Experiments/NDNSF_DistributedRepo_Generic_Minindn.py",
    "tests/standalone/spec192-repo-client.cpp",
    "tests/standalone/spec193-object-repo-server.cpp",
    "tests/fixtures/spec202-syscall-fault.hpp",
    "tests/fixtures/spec202-import-lock.hpp",
    "tests/minindn/run_spec192_real_cpp_repo.py",
    "tests/minindn/spec193_preflight.py",
    "tests/minindn/spec193_packet_oracle.py",
    "tests/minindn/spec192_binding_client.py",
    "tests/minindn/spec192_di_peer.py",
    "tests/minindn/spec193_retired_python_peer.py",
    "tests/integration-tests/ndnsf-integration-fixture.cpp",
    "tests/integration-tests/ndnsf-integration-fixture.hpp",
)
SOURCE_PATTERNS = (
    "tests/unit-tests/*repo*.cpp", "tests/unit-tests/spec193*.cpp",
    "tests/integration-tests/spec192*.cpp", "tests/python/*repo*.py",
    "tests/python/test_spec192*.py", "tests/minindn/spec193*.conf",
    "tests/minindn/test_spec*preflight.py", "tests/minindn/test_spec202*.py",
)


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
    for name in DESIGN_FILES:
        path = require_file(root / name, "DESIGN")
        files[name] = sha256(path)
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
    }
    encoded = json.dumps(state, sort_keys=True, separators=(",", ":")).encode("utf-8")
    state["candidate_lock_sha256"] = hashlib.sha256(encoded).hexdigest()
    if lock_path is not None:
        locked = json.loads(require_file(lock_path, "CANDIDATE_LOCK")
                            .read_text(encoding="utf-8"))
        if locked != state:
            raise PreflightError("CANDIDATE_MISMATCH", str(lock_path))
    return state
