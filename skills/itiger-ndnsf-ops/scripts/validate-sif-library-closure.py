#!/usr/bin/env python3
"""Fail-closed dependency closure check for an immutable NDNSF-DI SIF.

The check intentionally runs inside the candidate image.  A successful host
link or a host-side ``ldd`` is not evidence that the promoted SIF contains the
same libraries.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import PurePosixPath
from typing import Any


DEFAULT_ROOT = "/opt/ndnsf-di/current/lib"
DEFAULT_ORT_ROOT = "/opt/onnxruntime/lib"
DEFAULT_SYSTEM_PREFIXES = (
    "/lib",
    "/lib64",
    "/usr/lib",
    "/usr/lib64",
    "/usr/local/cuda",
    "/usr/local/nvidia",
)
FAMILY_RE = re.compile(r"^(lib[^.]+)\.so(?:\..*)?$")
SONAME_RE = re.compile(r"\(SONAME\).*\[(.*?)\]")
NEEDED_RE = re.compile(r"\(NEEDED\).*\[(.*?)\]")
INTERNAL_FAMILY_PREFIXES = (
    "libndn-",
    "libndnsd",
    "libnac-abe",
    "libopenabe",
    "librelic",
    "libonnxruntime",
)


def run_in_image(
    sif: str, command: str, binds: tuple[str, ...] = ()
) -> tuple[int, str, str]:
    bind_args: list[str] = []
    for binding in binds:
        bind_args.extend(["--bind", binding])
    proc = subprocess.run(
        ["apptainer", "exec", "--cleanenv", *bind_args, sif, "sh", "-lc", command],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    return proc.returncode, proc.stdout, proc.stderr


def readelf_binds() -> tuple[str, ...]:
    """Bind a host readelf and its loader libraries into the image."""
    tool = "/usr/bin/readelf" if os.path.exists("/usr/bin/readelf") else shutil.which("readelf")
    if not tool:
        return ()
    deps = {tool}
    proc = subprocess.run(
        ["ldd", tool], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False
    )
    for line in proc.stdout.splitlines():
        match = re.search(r"=>\s+(\/\S+)\s+\(", line)
        if match:
            deps.add(match.group(1))
        elif line.strip().startswith("/"):
            candidate = line.strip().split()[0].rstrip(":")
            if os.path.isabs(candidate) and os.path.exists(candidate):
                deps.add(candidate)
    bindings = [f"{tool}:/host-tools/readelf"]
    bindings.extend(f"{path}:{path}" for path in sorted(deps) if path != tool)
    return tuple(bindings)


def run_capture(
    sif: str,
    command: str,
    failures: list[str],
    label: str,
    *,
    use_host_readelf: bool = False,
) -> str:
    if use_host_readelf:
        return run_readelf_capture(sif, command, failures, label)
    binds = readelf_binds() if use_host_readelf else ()
    code, out, err = run_in_image(sif, command, binds)
    if code != 0:
        failures.append(f"{label}: exit={code}: {err.strip() or out.strip()}")
    return out


def run_readelf_capture(
    sif: str, command: str, failures: list[str], label: str
) -> str:
    """Read ELF metadata without making the image use host libc.

    The fast path binds a host readelf into the image.  Some cluster images
    have a different glibc ABI, so that bind can fail before readelf starts.
    In that case extract only the requested ELF file through ``apptainer
    cat`` and inspect the temporary host copy.  The extracted copy is not a
    runtime dependency and is deleted immediately.
    """
    binds = readelf_binds()
    if binds:
        bound_command = command.replace("readelf ", "/host-tools/readelf ", 1)
        code, out, _ = run_in_image(sif, bound_command, binds)
        if code == 0:
            return out

    match = re.search(r"readelf -d ('(?:[^']|'\"'\"')+')", command)
    if not match:
        failures.append(f"{label}: cannot identify ELF path for fallback")
        return ""
    quoted_path = match.group(1)
    path = quoted_path[1:-1].replace("'\"'\"'", "'")
    tool = "/usr/bin/readelf" if os.path.exists("/usr/bin/readelf") else shutil.which("readelf")
    if not tool:
        failures.append(f"{label}: host readelf is unavailable")
        return ""
    with tempfile.NamedTemporaryFile(prefix="ndnsf-sif-elf-", delete=True) as temp:
        extracted = subprocess.run(
            ["apptainer", "exec", "--cleanenv", sif, "cat", path],
            stdout=temp,
            stderr=subprocess.PIPE,
            check=False,
        )
        if extracted.returncode != 0:
            failures.append(
                f"{label}: SIF extraction failed: "
                f"{extracted.stderr.decode(errors='replace').strip()}"
            )
            return ""
        temp.flush()
        inspected = subprocess.run(
            [tool, "-d", temp.name],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        if inspected.returncode != 0:
            failures.append(
                f"{label}: host readelf fallback failed: "
                f"{inspected.stderr.strip()}"
            )
            return ""
        return inspected.stdout


def parse_ldd(text: str) -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    for line in text.splitlines():
        if "not found" in line:
            rows.append({"name": line.strip(), "path": "NOT_FOUND"})
            continue
        match = re.match(r"\s*(\S+)\s+=>\s+(\S+)\s+\(0x", line)
        if match:
            rows.append({"name": match.group(1), "path": match.group(2)})
            continue
        match = re.match(r"\s*(/\S+)\s+\(0x", line)
        if match:
            rows.append({"name": os.path.basename(match.group(1)), "path": match.group(1)})
    return rows


def parse_dynamic(text: str) -> dict[str, Any]:
    rpaths: list[str] = []
    soname = None
    needed: list[str] = []
    for line in text.splitlines():
        if "(RPATH)" in line or "(RUNPATH)" in line:
            value = re.search(r"\[(.*?)\]", line)
            if value:
                rpaths.extend(value.group(1).split(":"))
        soname_match = SONAME_RE.search(line)
        if soname_match:
            soname = soname_match.group(1)
        needed_match = NEEDED_RE.search(line)
        if needed_match:
            needed.append(needed_match.group(1))
    return {"rpath": rpaths, "soname": soname, "needed": needed}


def sha256_in_image(sif: str, path: str, failures: list[str]) -> str | None:
    output = run_capture(
        sif,
        f"test -f {quote(path)} && sha256sum {quote(path)}",
        failures,
        f"hash {path}",
    ).strip()
    if not output:
        return None
    return output.split()[0]


def quote(value: str) -> str:
    # All paths are generated by the image or supplied as absolute arguments.
    # Use single-quote shell escaping anyway because this command is a gate.
    escaped = value.replace("'", "'\"'\"'")
    return "'" + escaped + "'"


def dependency_is_packaged(path: str, library_roots: tuple[str, ...]) -> bool:
    return any(
        path == root or path.startswith(root.rstrip("/") + "/")
        for root in library_roots
    )


def validate_dependency(
    dep: dict[str, str],
    target: str,
    failures: list[str],
    allowed_prefixes: tuple[str, ...],
    library_roots: tuple[str, ...],
    toolchain: dict[str, Any],
) -> None:
    path = dep.get("path", "")
    if path == "NOT_FOUND":
        failures.append(f"{target}: unresolved dependency {dep.get('name', '')}")
        return
    if not path:
        return
    if not any(path.startswith(prefix.rstrip("/") + "/") for prefix in allowed_prefixes):
        failures.append(f"{target}: dependency outside recorded prefixes: {path}")

    if dependency_is_packaged(path, library_roots):
        return

    basename = PurePosixPath(path).name
    if basename.startswith(INTERNAL_FAMILY_PREFIXES):
        failures.append(
            f"{target}: internal dependency resolved outside packaged library roots: {path}"
        )
        return

    # Boost is often supplied by a qualified base image rather than copied
    # into the application root.  If that route is used, the SONAME filename
    # must still carry the locked major/minor version; otherwise an older or
    # newer host Boost can silently satisfy the link.
    if basename.startswith("libboost_"):
        expected = str(toolchain.get("boost", "")).strip()
        if not expected:
            failures.append(f"{target}: external Boost dependency has no locked version: {path}")
        elif f".so.{expected}." not in basename and not basename.endswith(f".so.{expected}"):
            failures.append(
                f"{target}: external Boost version mismatch: {path} (locked {expected})"
            )


def load_lock(path: str | None, failures: list[str]) -> dict[str, Any] | None:
    if path is None:
        return None
    try:
        with open(path, encoding="utf-8") as handle:
            value = json.load(handle)
    except (OSError, json.JSONDecodeError) as exc:
        failures.append(f"lock file: {exc}")
        return None
    if not isinstance(value, dict):
        failures.append("lock file: top-level value is not an object")
        return None
    if value.get("schemaVersion") != "ndnsf-sif-library-lock-v2":
        failures.append(
            "lock file: schemaVersion must be ndnsf-sif-library-lock-v2"
        )
    return value


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sif", required=True, help="exact local or staged SIF")
    parser.add_argument("--provider", required=True, help="Provider executable path inside SIF")
    parser.add_argument("--extension", required=True, help="Python extension path inside SIF")
    parser.add_argument("--library-root", action="append", default=[])
    parser.add_argument("--allow-prefix", action="append", default=[])
    parser.add_argument("--required-soname", action="append", default=[])
    parser.add_argument(
        "--lock",
        required=True,
        help="sealed JSON lock with libraries [{path, sha256, soname, version}]",
    )
    parser.add_argument("--output", required=True, help="JSON evidence output")
    args = parser.parse_args()
    if not args.library_root:
        args.library_root = [DEFAULT_ROOT, DEFAULT_ORT_ROOT]
    if not args.allow_prefix:
        args.allow_prefix = list(DEFAULT_SYSTEM_PREFIXES)

    failures: list[str] = []
    if not os.path.isfile(args.sif):
        failures.append(f"SIF does not exist: {args.sif}")
    else:
        if os.path.getsize(args.sif) == 0:
            failures.append(f"SIF is empty: {args.sif}")

    lock = load_lock(args.lock, failures)
    toolchain: dict[str, Any] = {}
    if lock is not None:
        toolchain_value = lock.get("toolchain")
        toolchain = toolchain_value if isinstance(toolchain_value, dict) else {}
        if not isinstance(toolchain, dict) or not toolchain:
            failures.append("lock file: missing non-empty toolchain/version map")
        else:
            for name, version in toolchain.items():
                if not str(name).strip() or not str(version).strip():
                    failures.append(f"lock file: empty toolchain/version entry: {name!r}")
    library_roots = tuple(args.library_root)
    allowed_prefixes = tuple(dict.fromkeys([*args.allow_prefix, *library_roots]))
    evidence: dict[str, Any] = {
        "schemaVersion": "itiger-sif-library-closure-v3",
        "checkedAt": dt.datetime.now(dt.timezone.utc).isoformat(),
        "sif": os.path.abspath(args.sif),
        "sifBytes": os.path.getsize(args.sif) if os.path.isfile(args.sif) else None,
        "targets": {},
        "libraries": [],
        "failures": failures,
    }

    for target in (args.provider, args.extension):
        test = run_capture(args.sif, f"test -f {quote(target)}", failures, f"target {target}")
        del test
        ldd_text = run_capture(args.sif, f"ldd {quote(target)}", failures, f"ldd {target}")
        dynamic_text = run_capture(
            args.sif,
            f"readelf -d {quote(target)}",
            failures,
            f"readelf {target}",
            use_host_readelf=True,
        )
        deps = parse_ldd(ldd_text)
        for dep in deps:
            validate_dependency(
                dep, target, failures, allowed_prefixes, library_roots, toolchain
            )
        dynamic = parse_dynamic(dynamic_text)
        for path in dynamic["rpath"]:
            if path.startswith("/tmp") or path.startswith("/home"):
                failures.append(f"{target}: host-only RPATH/RUNPATH: {path}")
            if path.startswith("/") and not any(
                path == prefix or path.startswith(prefix.rstrip("/") + "/")
                for prefix in allowed_prefixes
            ):
                failures.append(f"{target}: unrecorded absolute RPATH/RUNPATH: {path}")
        evidence["targets"][target] = {
            "ldd": deps,
            "dynamic": dynamic,
        }

    for root in library_roots:
        listing = run_capture(
            args.sif,
            f"test -d {quote(root)} && find {quote(root)} -maxdepth 1 \\( -type f -o -type l \\) -name '*.so*' -print | sort",
            failures,
            f"inventory {root}",
        )
        for raw in listing.splitlines():
            path = raw.strip()
            if not path:
                continue
            digest = sha256_in_image(args.sif, path, failures)
            dynamic_text = run_capture(
                args.sif,
                f"readelf -d {quote(path)}",
                failures,
                f"readelf {path}",
                use_host_readelf=True,
            )
            dynamic = parse_dynamic(dynamic_text)
            for rpath in dynamic["rpath"]:
                if rpath.startswith("/tmp") or rpath.startswith("/home"):
                    failures.append(f"{path}: host-only RPATH/RUNPATH: {rpath}")
                if rpath.startswith("/") and not any(
                    rpath == prefix or rpath.startswith(prefix.rstrip("/") + "/")
                    for prefix in allowed_prefixes
                ):
                    failures.append(f"{path}: unrecorded absolute RPATH/RUNPATH: {rpath}")
            resolved = run_capture(args.sif, f"readlink -f {quote(path)}", failures, f"resolve {path}").strip()
            root_prefix = root.rstrip("/") + "/"
            if resolved and not (resolved == root or resolved.startswith(root_prefix)):
                failures.append(f"{path}: symlink resolves outside its packaged root: {resolved}")
            evidence["libraries"].append(
                {
                    "path": path,
                    "resolvedPath": resolved,
                    "sha256": digest,
                    "soname": dynamic["soname"],
                    "needed": dynamic["needed"],
                }
            )

    actual = evidence["libraries"]
    # SONAME compatibility links are meaningful only inside the library root
    # that contains the ELF payload.  A same-named link in another root (for
    # example ORT versus the NDNSF framework root) must not satisfy this gate.
    names_by_root: dict[str, set[str]] = {}
    for row in actual:
        path = PurePosixPath(row["path"])
        names_by_root.setdefault(str(path.parent), set()).add(path.name)
    sonames = {row["soname"] for row in actual if row.get("soname")}
    names = {PurePosixPath(row["path"]).name for row in actual}
    for required in args.required_soname:
        if required not in sonames and required not in names:
            failures.append(f"missing required SONAME/compatibility link: {required}")

    # A SONAME must be addressable by that name in the same packaged library root.
    for row in actual:
        soname = row.get("soname")
        if not soname:
            continue
        root = str(PurePosixPath(row["path"]).parent)
        if soname not in names_by_root.get(root, set()):
            failures.append(f"{row['path']}: SONAME link missing: {soname}")

    # Ignore symlink aliases to the same file, but reject two distinct payloads
    # claiming the same libNAME.so family in one packaged root.
    families: dict[str, set[str]] = {}
    for row in actual:
        family_match = FAMILY_RE.match(PurePosixPath(row["path"]).name)
        if family_match and row.get("resolvedPath"):
            families.setdefault(family_match.group(1), set()).add(row["resolvedPath"])
    for family, paths in families.items():
        if len(paths) > 1:
            failures.append(f"duplicate library family with distinct payloads: {family}: {sorted(paths)}")

    # Checking only the executable can miss a stale or incomplete packaged
    # library that is not loaded on the current link path.  Check every
    # distinct payload as well, while de-duplicating compatibility symlinks.
    checked_payloads: set[str] = set()
    for row in actual:
        payload = row.get("resolvedPath") or row.get("path")
        if not payload or payload in checked_payloads:
            continue
        checked_payloads.add(payload)
        library_ldd = run_capture(
            args.sif,
            f"ldd {quote(payload)}",
            failures,
            f"ldd packaged library {payload}",
        )
        for dep in parse_ldd(library_ldd):
            validate_dependency(
                dep, payload, failures, allowed_prefixes, library_roots, toolchain
            )

    if lock is not None:
        expected = lock.get("libraries", [])
        if not isinstance(expected, list):
            failures.append("lock file: libraries is not a list")
        else:
            by_path = {row["path"]: row for row in actual}
            expected_paths: set[str] = set()
            for row in expected:
                if not isinstance(row, dict) or not row.get("path"):
                    failures.append("lock file: malformed library row")
                    continue
                path = str(row["path"])
                version = row.get("version")
                if not isinstance(version, str) or not version.strip():
                    failures.append(f"lock file: missing library version: {path}")
                if path in expected_paths:
                    failures.append(f"lock file: duplicate library row: {path}")
                expected_paths.add(path)
                current = by_path.get(row["path"])
                if current is None:
                    failures.append(f"locked library missing: {row['path']}")
                    continue
                if row.get("sha256") and current.get("sha256") != row["sha256"]:
                    failures.append(f"locked hash mismatch: {row['path']}")
                if row.get("soname") and current.get("soname") != row["soname"]:
                    failures.append(f"locked SONAME mismatch: {row['path']}")
            for extra in sorted(set(by_path) - expected_paths):
                failures.append(f"unlocked packaged library: {extra}")

    evidence["failures"] = failures
    evidence["status"] = "PASS" if not failures else "FAIL"
    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    with open(args.output, "w", encoding="utf-8") as handle:
        json.dump(evidence, handle, indent=2, sort_keys=True)
        handle.write("\n")
    print(json.dumps({"status": evidence["status"], "output": args.output, "failures": failures}, sort_keys=True))
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
