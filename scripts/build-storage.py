#!/usr/bin/env python3
"""Admit and measure host build/install allocations on their actual filesystems."""

import argparse
import ast
import fcntl
import hashlib
import json
import os
from pathlib import Path
import runpy
import shutil
import signal
import stat
import subprocess
import sys
import tempfile


MIB = 1024 ** 2
GIB = 1024 * MIB
PEAK_FILE = ".ndnsf-storage-peaks.json"
STAGE_NAME = "install-stage"
STAGE_MARKER = ".ndnsf-native-install-stage"


def waf_cache_values(path, names):
    return {node.targets[0].id: ast.literal_eval(node.value)
            for node in ast.parse(path.read_text()).body
            if isinstance(node, ast.Assign) and isinstance(node.targets[0], ast.Name)
            and node.targets[0].id in names}


def configure_options(arguments):
    options = {}
    for index, argument in enumerate(arguments):
        if argument.startswith("--") and argument != "--":
            name, equals, value = argument.partition("=")
            following = arguments[index + 1:index + 2]
            options[name] = (value if equals else following[0]
                             if following and not following[0].startswith("-") else None)
    return options


def configured(args):
    build = Path(args.build_dir).resolve()
    source_root = Path(args.source_dir).resolve() if args.source_dir else build.parent
    try:
        lock = waf_cache_values(build / ".lock-waf_linux_build",
                                {"argv", "files", "hash", "out_dir", "top_dir"})
        cache = waf_cache_values(build / "c4che/_cache.py", {"CXX", "sanitize_di", "LINKFLAGS"})
        if lock["out_dir"] != str(build) or lock["top_dir"] != str(source_root):
            return False
        if cache.get("sanitize_di") or (not args.source_dir and not (build / "config.hpp").is_file()):
            return False
        if args.cxx and Path(cache["CXX"][0]).resolve() != Path(args.cxx).resolve():
            return False
        if args.runtime_rpath is not None:
            prefix = "-Wl,-rpath,"
            expected = [prefix + path for path in args.runtime_rpath.split(os.pathsep) if path]
            cached = [flag for flag in cache.get("LINKFLAGS", [])
                      if isinstance(flag, str) and flag.startswith(prefix)]
            if cached != expected:
                return False
        signature = 0
        for source in lock["files"]:
            signature = hashlib.md5(repr((signature, Path(source).read_bytes())).encode()).digest()
        if not lock["files"] or signature != lock["hash"]:
            return False
        actual = configure_options(lock["argv"])
        return all(name in actual and actual[name] == value
                   for name, value in configure_options(args.options).items())
    except (OSError, ValueError, TypeError, KeyError, IndexError, SyntaxError):
        return False


def nearest_directory(path):
    path = Path(path).resolve()
    while not path.is_dir():
        if path.exists() or path == path.parent:
            raise ValueError(f"expected destination directory: {path}")
        path = path.parent
    return path


def available(path):
    info = os.statvfs(path)
    return info.f_bavail * info.f_frsize


def grouped_allocations(allocations):
    groups = {}
    for name, path, size in allocations:
        directory = nearest_directory(path)
        device = directory.stat().st_dev
        group = groups.setdefault(device, (directory, []))
        group[1].append((name, Path(path), size))
    return groups


def largest_entries(path):
    try:
        result = subprocess.run(
            ["du", "-x", "-B1", "-d1", "--", str(path)],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, check=False,
            timeout=5)
        output = result.stdout
    except subprocess.TimeoutExpired as error:
        output = (error.stdout or b"").decode(errors="replace")
        print(f"  inventory timed out: {path}", file=sys.stderr)
    entries = []
    for line in output.splitlines():
        size, separator, entry = line.partition("\t")
        if separator and size.isdigit() and Path(entry) != path:
            entries.append((int(size), entry))
    return sorted(entries, reverse=True)[:5]


def admit(allocations, reserve):
    groups = grouped_allocations(allocations)
    blocked = False
    for device, (directory, entries) in groups.items():
        needed = sum(size for _, _, size in entries) + reserve
        free = available(directory)
        print(f"NDNSF_DISK device={device} available={free} estimated={needed-reserve} "
              f"reserve={reserve} path={directory}", flush=True)
        if free >= needed:
            continue
        blocked = True
        for name, path, size in sorted(entries, key=lambda entry: entry[2], reverse=True):
            print(f"  allocation {size:>12} {name} {path}", file=sys.stderr)
        for path in dict.fromkeys(nearest_directory(path) for _, path, _ in entries):
            for size, entry in largest_entries(path):
                print(f"  allocated  {size:>12} {entry}", file=sys.stderr)
    if blocked:
        raise ValueError("available disk space is below the estimated allocation plus reserve")
    return groups


def read_peaks(build):
    try:
        peaks = json.loads((build / PEAK_FILE).read_text())
        return {key: value for key, value in peaks.items()
                if isinstance(value, dict) and
                all(isinstance(n, int) and n >= 0 for n in value.values())}
    except (OSError, ValueError, AttributeError):
        return {}


def write_peaks(build, peaks):
    with tempfile.NamedTemporaryFile(mode="w", dir=build, delete=False) as stream:
        temporary = Path(stream.name)
        json.dump(dict(list(peaks.items())[-64:]), stream, sort_keys=False)
        stream.write("\n")
    try:
        temporary.replace(build / PEAK_FILE)
    finally:
        temporary.unlink(missing_ok=True)


def link_budget(build, jobs, targets):
    # Existing outputs approximate the concurrent replacement/link footprint.
    # Waf keeps objects under more than one target directory, so inspect the
    # actual link outputs rather than summing the object cache.
    names = set(targets.split(",")) if targets else set()
    sizes = []
    seen = set()
    for directory in (build, build / "examples", build / "NDNSF-DistributedRepo"):
        if not directory.is_dir():
            continue
        for entry in directory.iterdir():
            if entry.is_symlink() or not entry.is_file():
                continue
            info = entry.stat()
            library = entry.name.startswith("lib") and (".so" in entry.name or
                                                        entry.suffix == ".a")
            if (not library and
                    (not info.st_mode & stat.S_IXUSR or (names and entry.name not in names))):
                continue
            identity = (info.st_dev, info.st_ino)
            if identity not in seen:
                sizes.append(info.st_size)
                seen.add(identity)
    return sum(sorted(sizes, reverse=True)[:jobs])


def run(args):
    build = Path(args.build_dir).resolve()
    temporary = Path(args.temporary_dir or os.environ.get("TMPDIR", tempfile.gettempdir()))
    key_data = [args.profile, args.targets, args.jobs, str(temporary.resolve()),
                args.prefix]
    key = hashlib.sha256(json.dumps(key_data).encode()).hexdigest()
    peaks = read_peaks(build)
    previous = peaks.get(key, {})
    replace_bytes = link_budget(build, args.jobs, args.targets)
    allocations = [
        ("new and replaced objects", build, args.build_bytes),
        ("concurrent link replacements", build, replace_bytes),
        ("compiler scratch", temporary, args.jobs * 128 * MIB),
    ]
    if args.prefix:
        staged = max(args.install_bytes, replace_bytes)
        allocations.extend([
            ("install stage", build / STAGE_NAME, staged),
            ("prefix promotion and replacement", args.prefix, 2 * staged),
        ])
    groups = grouped_allocations(allocations)
    allocations.extend(
        ("measured peak margin", directory,
         max(0, previous.get(str(device), 0) - sum(size for _, _, size in entries)))
        for device, (directory, entries) in groups.items())
    groups = admit(allocations, args.reserve)
    command = args.command
    if command and command[0] == "--":
        command = command[1:]
    if not command:
        return 0
    origin = {device: available(directory) for device, (directory, _) in groups.items()}
    low = origin.copy()
    inherited_locks = []
    for fd in (7, 9):
        try:
            if os.readlink(f"/proc/self/fd/{fd}").endswith(
                    ("/.ndnsf-install.lock", "/ndnsf-build.lock")):
                inherited_locks.append(fd)
        except OSError:
            pass
    child = subprocess.Popen(command, start_new_session=True, pass_fds=inherited_locks)

    def forward(signum, _frame):
        try:
            os.killpg(child.pid, signum)
        except ProcessLookupError:
            pass

    old_handlers = {signum: signal.signal(signum, forward)
                    for signum in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)}
    try:
        while True:
            for device, (directory, _) in groups.items():
                low[device] = min(low[device], available(directory))
            try:
                result = child.wait(timeout=0.1)
                break
            except subprocess.TimeoutExpired:
                pass
    finally:
        for signum, handler in old_handlers.items():
            signal.signal(signum, handler)
    measured = {}
    for device, (directory, _) in groups.items():
        free = available(directory)
        high_water = origin[device] - min(low[device], free)
        measured[str(device)] = high_water
        print(f"NDNSF_DISK_RESULT device={device} observed_peak={high_water} "
              f"retained_delta={origin[device]-free} exit={result}", flush=True)
    if build.is_dir() and any(measured.values()):
        peaks.pop(key, None)
        peaks[key] = measured if result == 0 else {
            device: max(size, previous.get(device, 0)) for device, size in measured.items()}
        try:
            write_peaks(build, peaks)
        except OSError as error:
            print(f"could not save build disk observation: {error}", file=sys.stderr)
    return 128 - result if result < 0 else result


def registered_stage(build):
    stage = build / STAGE_NAME
    marker = stage / STAGE_MARKER
    if (stage.is_symlink() or not stage.is_dir() or
            stage.stat().st_uid != os.getuid() or
            stat.S_IMODE(stage.stat().st_mode) != 0o700 or marker.is_symlink() or
            not marker.is_file() or marker.read_text() != str(build) + "\n"):
        raise ValueError(f"unregistered install stage: {stage}")
    workspace = runpy.run_path(str(Path(__file__).with_name("ndnsf-di-workspace.py")))
    workspace["reject_mounts"](stage)
    workspace["reject_live_users"](stage)
    return stage


def install_stage(args):
    build = Path(args.build_dir).resolve(strict=True)
    stage = build / STAGE_NAME
    if stage.exists() or stage.is_symlink():
        shutil.rmtree(registered_stage(build))
        print(f"Retired owned install stage: {stage}")
    if args.stage_action == "prepare":
        stage.mkdir(mode=0o700)
        (stage / STAGE_MARKER).write_text(str(build) + "\n")


def file_digest(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(MIB), b""):
            digest.update(block)
    return digest.digest()


def install_paths(args):
    prefix = Path(args.prefix).resolve()
    stage = registered_stage(Path(args.build_dir).resolve(strict=True)) / prefix.relative_to("/")
    if not stage.is_dir():
        raise ValueError(f"install stage does not contain the selected prefix: {stage}")
    return stage, prefix


def promotion_budget(args):
    stage, prefix = install_paths(args)
    costs = {}
    for source in stage.rglob("*"):
        if not source.is_file() or source.is_symlink():
            continue
        target = prefix / source.relative_to(stage)
        directory = nearest_directory(target.parent)
        device = directory.stat().st_dev
        anchor, size = costs.get(device, (directory, 0))
        prior = target.stat().st_size if target.is_file() and not target.is_symlink() else 0
        costs[device] = (anchor, size + source.stat().st_size + prior)
    admit([("staged prefix and backups", path, size) for path, size in costs.values()],
          args.reserve)


def verify_install(args):
    stage, prefix = install_paths(args)
    programs = []
    checked = 0
    for source in stage.rglob("*"):
        relative = source.relative_to(stage)
        target = prefix / relative
        if source.is_symlink():
            if not target.is_symlink() or os.readlink(source) != os.readlink(target):
                raise ValueError(f"installed link differs from the stage: {target}")
        elif source.is_dir():
            continue
        elif (not target.is_file() or target.is_symlink() or
              stat.S_IMODE(source.stat().st_mode) != stat.S_IMODE(target.stat().st_mode) or
              file_digest(source) != file_digest(target)):
            raise ValueError(f"installed file differs from the stage: {target}")
        elif relative.parts[0] in ("bin", "libexec") and os.access(target, os.X_OK):
            programs.append(target)
        checked += 1
    if not checked:
        raise ValueError(f"empty install stage: {stage}")
    checkout = Path(__file__).resolve().parents[1]
    scratch = Path(tempfile.gettempdir()).resolve()
    if scratch == checkout or checkout in scratch.parents:
        scratch = Path("/tmp")
    with tempfile.TemporaryDirectory(prefix="ndnsf-installed-check-", dir=scratch) as workspace:
        env = {name: value for name, value in os.environ.items()
               if name not in ("LD_LIBRARY_PATH", "LD_PRELOAD", "PYTHONPATH", "PYTHONHOME",
                               "NDN_CLIENT_PIB", "NDN_CLIENT_TPM")}
        env.update(HOME=workspace, TMPDIR=workspace, PYTHONNOUSERSITE="1",
                   PATH=f"{prefix}/bin:/usr/bin:/bin",
                   LD_LIBRARY_PATH=str(prefix / "lib"))
        for program in programs:
            if program.name in ("DI_NativeRequester", "DI_NativeArtifactAuthority",
                                "multiturn-oracle", "ndnsf-di", "ndnsf-checks"):
                subprocess.run([str(program), "--help"], check=True, cwd=workspace,
                               env=env, timeout=30, stdout=subprocess.DEVNULL)
            elif program.name == "ndnsf-identity-check":
                identity_home = Path(workspace) / "empty-credentials"
                identity_home.mkdir()
                probe = subprocess.run([str(program), str(identity_home)], cwd=workspace, env=env,
                                       timeout=30, text=True, capture_output=True)
                if probe.returncode != 1 or "existing credential PIB and TPM" not in probe.stderr:
                    raise ValueError(f"unexpected installed identity-check result: {probe.stderr}")
            elif program.parent == prefix / "bin":
                with program.open("rb") as stream:
                    native = stream.read(4) == b"\x7fELF"
                if native:
                    loader_env = env
                    if program.name == "UavTrackingNode":
                        # The node must find its installed SDK without a search-path override.
                        loader_env = {key: value for key, value in env.items()
                                      if key != "LD_LIBRARY_PATH"}
                    probe = subprocess.run(["/usr/bin/ldd", str(program)], cwd=workspace,
                                           env=loader_env, timeout=30, text=True, capture_output=True)
                    if "not found" in probe.stdout or (
                            probe.returncode and "not a dynamic executable" not in probe.stderr):
                        raise ValueError(f"installed loader check failed for {program}: "
                                         f"{probe.stdout}{probe.stderr}")
                    if program.name == "UavTrackingNode":
                        resolved = {fields[0]: Path(fields[2]).resolve()
                                    for line in probe.stdout.splitlines()
                                    for fields in [line.split()]
                                    if len(fields) >= 3 and fields[1] == "=>"}
                        for library in ("libndnsf-uav-tracking.so",
                                        "libndn-service-framework.so.0.1.0"):
                            expected = (prefix / "lib" / library).resolve(strict=True)
                            if resolved.get(library) != expected:
                                raise ValueError(f"{program} resolved {library} to "
                                                 f"{resolved.get(library)}, expected {expected}")
    print(f"Verified {checked} installed files against {stage}")


def retire_output(args):
    build = Path(args.build_dir).resolve(strict=True)
    root = Path(__file__).resolve().parents[1]
    if build not in (root / "build", root / "build-ASAN-UBSAN"):
        raise ValueError("retirement requires a maintained build tree")
    if not os.readlink("/proc/self/fd/9").endswith("/ndnsf-build.lock"):
        raise ValueError("retirement must run through scripts/build-safe.sh run")
    fcntl.flock(9, fcntl.LOCK_EX | fcntl.LOCK_NB)
    names = [args.output] + ([args.replacement] if args.replacement else [])
    if any(Path(name).name != name or name.startswith((".", "lib")) or
           name.endswith((".o", ".so", ".a")) for name in names):
        raise ValueError("retirement takes explicit top-level executable names")
    lock = ".lock-waf_linux_build" if build.name == "build" else ".lock-waf_build_asan"
    lock_lines = (build / lock).read_text().splitlines()
    out = next((ast.literal_eval(line.split(" = ", 1)[1])
                for line in lock_lines if line.startswith("out_dir = ")), None)
    if out != str(build):
        raise ValueError(f"Waf lock does not select {build}")
    output = build / args.output
    replacement = build / args.replacement if args.replacement else None
    original = output.lstat()
    if (not stat.S_ISREG(original.st_mode) or original.st_uid != os.getuid() or
            original.st_nlink != 1 or not os.access(output, os.X_OK)):
        raise ValueError("expected an owned executable")
    if replacement is not None and (not replacement.is_file() or replacement.is_symlink() or
                                    not os.access(replacement, os.X_OK)):
        raise ValueError("expected an existing replacement executable")
    with output.open("rb") as stream:
        if stream.read(4) != b"\x7fELF":
            raise ValueError(f"not a generated native executable: {output}")
    listing = subprocess.run(
        [str(root / "waf"), f"--out={build}", "list"], cwd=build,
        env=dict(os.environ, WAFLOCK=lock), check=True, capture_output=True,
        text=True, timeout=30)
    targets = {line.strip() for line in listing.stdout.splitlines()}
    if args.output in targets or (replacement is not None and args.replacement not in targets):
        raise ValueError("the configured Waf tree still selects the old output or lacks the replacement")
    references = subprocess.run(
        ["git", "grep", "-l", "-F", "-e", args.output, "--", "."], cwd=root,
        capture_output=True, text=True, timeout=30)
    if references.returncode != 1:
        raise ValueError(f"source references could not be excluded: {references.stdout.strip()}")
    workspace = runpy.run_path(str(Path(__file__).with_name("ndnsf-di-workspace.py")))
    workspace["reject_mounts"](output)
    workspace["reject_live_users"](build)

    def identity(info):
        return (info.st_dev, info.st_ino, info.st_uid, info.st_mode, info.st_nlink,
                info.st_size, info.st_mtime_ns, info.st_ctime_ns)

    observed = file_digest(output).hex()
    if identity(output.lstat()) != identity(original) or observed != args.sha256:
        raise ValueError(f"retirement identity changed: {output}")
    output.unlink()
    print(f"Retired {output} ({original.st_blocks * 512} allocated bytes)")
    if replacement is not None:
        print(f"Replacement: {replacement}")


def nonnegative(text):
    value = int(text)
    if value < 0:
        raise argparse.ArgumentTypeError("expected nonnegative byte count")
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="action", required=True)
    configuration = commands.add_parser("configured")
    configuration.add_argument("--build-dir", required=True)
    configuration.add_argument("--source-dir")
    configuration.add_argument("--cxx")
    configuration.add_argument("--runtime-rpath")
    configuration.add_argument("options", nargs=argparse.REMAINDER)
    gate = commands.add_parser("run")
    gate.add_argument("--build-dir", required=True)
    gate.add_argument("--temporary-dir")
    gate.add_argument("--profile", default="build")
    gate.add_argument("--targets", default="")
    gate.add_argument("--jobs", type=int, default=6)
    gate.add_argument("--prefix")
    gate.add_argument("--reserve", type=nonnegative,
                      default=os.environ.get("NDNSF_DISK_RESERVE_BYTES", str(2 * GIB)))
    gate.add_argument("--build-bytes", type=nonnegative, default=2 * GIB)
    gate.add_argument("--install-bytes", type=nonnegative, default=2 * GIB)
    gate.add_argument("command", nargs=argparse.REMAINDER)
    stage = commands.add_parser("stage")
    stage.add_argument("--build-dir", required=True)
    stage.add_argument("stage_action", choices=("prepare", "retire"))
    verify = commands.add_parser("verify-install")
    verify.add_argument("--build-dir", required=True)
    verify.add_argument("--prefix", required=True)
    promotion = commands.add_parser("promotion-budget")
    promotion.add_argument("--build-dir", required=True)
    promotion.add_argument("--prefix", required=True)
    promotion.add_argument("--reserve", type=nonnegative,
                           default=os.environ.get("NDNSF_DISK_RESERVE_BYTES", str(2 * GIB)))
    retire = commands.add_parser("retire-output")
    retire.add_argument("--build-dir", required=True)
    retire.add_argument("--output", required=True)
    retire.add_argument("--replacement")
    retire.add_argument("--sha256", required=True)
    args = parser.parse_args()
    if args.action == "configured":
        return 0 if configured(args) else 1
    try:
        if args.action == "stage":
            install_stage(args)
            return 0
        if args.action == "verify-install":
            verify_install(args)
            return 0
        if args.action == "promotion-budget":
            promotion_budget(args)
            return 0
        if args.action == "retire-output":
            retire_output(args)
            return 0
        if args.jobs < 1:
            raise ValueError("parallelism must be positive")
        return run(args)
    except (ValueError, OSError, RuntimeError, subprocess.SubprocessError) as error:
        failure = {
            "run": "NDNSF_DISK_REJECT",
            "promotion-budget": "NDNSF_DISK_REJECT",
            "stage": "NDNSF_INSTALL_STAGE_REJECT",
            "verify-install": "NDNSF_INSTALL_VERIFY_FAILED",
            "retire-output": "NDNSF_OUTPUT_RETIRE_REFUSED",
        }[args.action]
        print(f"{failure}: {error}", file=sys.stderr)
        return 75


if __name__ == "__main__":
    sys.exit(main())
