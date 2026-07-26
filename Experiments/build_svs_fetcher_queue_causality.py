#!/usr/bin/env python3
"""Build the isolated RSA-2048 Spec 135 diagnostic subject."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from typing import Any


REPO = Path(__file__).resolve().parents[1]
SVS_REPO = Path(os.environ.get("NDN_SVS_SOURCE_REPO", "/home/tianxing/NDN/ndn-svs"))
ROOT = REPO / "build/spec135"
WORKTREE = ROOT / "worktrees/fetcher-queue-causality"
BRANCH = "spec135-fetcher-queue-causality"
PARENT = "e9913c9a957a214d699ab5eb0bc99684e06573c5"
BASE_DRIVER = REPO / "Experiments/ndn-svs-pubsub-benchmark/svs-sync-stage-profile.cpp"
RSA_HELPER = REPO / "Experiments/ndn-svs-pubsub-benchmark/spec135-rsa-security.hpp"
GENERATED_DRIVER = ROOT / "generated/svs-fetcher-queue-causality.cpp"
MANIFEST = ROOT / "subject-manifest.json"


def run(command: list[str], *, cwd: Path | None = None,
        env: dict[str, str] | None = None, log: Path | None = None) -> str:
    if log is None:
        result = subprocess.run(command, cwd=cwd, env=env, text=True, check=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        return result.stdout
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open("w", encoding="utf-8") as output:
        subprocess.run(command, cwd=cwd, env=env, text=True, check=True,
                       stdout=output, stderr=subprocess.STDOUT)
    return ""


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def git_value(repo: Path, expression: str) -> str:
    return run(["git", "rev-parse", expression], cwd=repo).strip()


def status(repo: Path) -> str:
    return run(["git", "status", "--porcelain=v1", "--untracked-files=all"],
               cwd=repo).strip()


def branch_exists() -> bool:
    return subprocess.run(
        ["git", "show-ref", "--verify", "--quiet", f"refs/heads/{BRANCH}"],
        cwd=SVS_REPO,
    ).returncode == 0


def protected_state() -> dict[str, dict[str, str]]:
    state: dict[str, dict[str, str]] = {}
    current: dict[str, str] = {}
    records: list[dict[str, str]] = []
    for line in run(["git", "worktree", "list", "--porcelain"], cwd=SVS_REPO).splitlines():
        if not line:
            if current:
                records.append(current)
                current = {}
            continue
        key, _, value = line.partition(" ")
        current[key] = value
    if current:
        records.append(current)
    for record in records:
        path = Path(record["worktree"]).resolve()
        if path == WORKTREE.resolve():
            continue
        state[str(path)] = {
            "head": git_value(path, "HEAD") if path.is_dir() else "missing",
            "status": hashlib.sha256(status(path).encode()).hexdigest()
                      if path.is_dir() else "missing",
        }
    return state


def ensure_worktree() -> None:
    if git_value(SVS_REPO, f"{PARENT}^{{commit}}") != PARENT:
        raise RuntimeError("Spec 135 parent commit does not resolve exactly")
    if not WORKTREE.exists():
        if branch_exists():
            raise RuntimeError(f"orphan branch exists without worktree: {BRANCH}")
        WORKTREE.parent.mkdir(parents=True, exist_ok=True)
        run(["git", "worktree", "add", "-b", BRANCH, str(WORKTREE), PARENT],
            cwd=SVS_REPO)
    head = git_value(WORKTREE, "HEAD")
    if head not in {PARENT} and git_value(WORKTREE, "HEAD^") != PARENT:
        raise RuntimeError(f"unexpected diagnostic worktree head: {head}")


def apply_diagnostic_patch() -> None:
    if git_value(WORKTREE, "HEAD") != PARENT:
        if status(WORKTREE):
            raise RuntimeError("existing Spec 135 diagnostic worktree is dirty")
        return
    header = WORKTREE / "ndn-svs/fetcher.hpp"
    source = WORKTREE / "ndn-svs/fetcher.cpp"
    header_text = header.read_text(encoding="utf-8")
    source_text = source.read_text(encoding="utf-8")
    old_member = "  uint16_t m_windowSize = 10;"
    if header_text.count(old_member) != 1:
        raise RuntimeError("Fetcher window member shape changed")
    header_text = header_text.replace(old_member, "  uint16_t m_windowSize = 10; // set by Spec 135 constructor")
    old_body = """{
}"""
    new_body = """{
  const char* raw = std::getenv("NDN_SVS_DIAGNOSTIC_FETCHER_WINDOW");
  if (raw == nullptr || (std::string(raw) != "10" && std::string(raw) != "40"))
    throw std::runtime_error("invalid NDN_SVS_DIAGNOSTIC_FETCHER_WINDOW");
  m_windowSize = static_cast<uint16_t>(std::stoul(raw));
  std::cerr << "SPEC135_FETCHER_WINDOW window=" << m_windowSize << std::endl;
}"""
    constructor = """Fetcher::Fetcher(Face& face, const SecurityOptions& securityOptions)
  : m_face(face)
  , m_scheduler(face.getIoContext())
  , m_securityOptions(securityOptions)
"""
    position = source_text.find(constructor)
    if position < 0:
        raise RuntimeError("Fetcher constructor shape changed")
    body_position = source_text.find(old_body, position + len(constructor))
    if body_position < 0:
        raise RuntimeError("Fetcher constructor body shape changed")
    source_text = (source_text[:body_position] + new_body +
                   source_text[body_position + len(old_body):])
    include_anchor = '#include "security-options.hpp"\n'
    if source_text.count(include_anchor) != 1:
        raise RuntimeError("Fetcher include anchor changed")
    source_text = source_text.replace(
        include_anchor,
        include_anchor + "\n#include <cstdlib>\n#include <iostream>\n#include <stdexcept>\n#include <string>\n",
    )
    header.write_text(header_text, encoding="utf-8")
    source.write_text(source_text, encoding="utf-8")
    changed = run(["git", "diff", "--name-only"], cwd=WORKTREE).splitlines()
    if changed != ["ndn-svs/fetcher.cpp", "ndn-svs/fetcher.hpp"]:
        raise RuntimeError(f"diagnostic patch escaped Fetcher: {changed}")
    run(["git", "add", "--", *changed], cwd=WORKTREE)
    run([
        "git", "-c", "user.name=Spec135 Build",
        "-c", "user.email=spec135@invalid", "commit", "-m",
        "diagnostics: expose bounded Fetcher window for Spec 135",
    ], cwd=WORKTREE)


def transform_driver() -> None:
    text = BASE_DRIVER.read_text(encoding="utf-8")
    anchor = "#include <ndn-cxx/util/dummy-client-face.hpp>\n"
    if text.count(anchor) != 1:
        raise RuntimeError("driver include anchor changed")
    text = text.replace(anchor, anchor + '#include "spec135-rsa-security.hpp"\n')

    main_old = """    security.dataSigner->signingInfo.setSha256Signing();

    SVSPubSubOptions pubsubOptions;"""
    main_new = """    m_rsaSignatureType = spec135::configureRsa2048(
      m_keyChain, security,
      Name("/spec135/rsa").append(m_options.cellId).append(m_options.peerId));

    SVSPubSubOptions pubsubOptions;
    pubsubOptions.maxApplicationParametersSize = spec135::readBoundedSize(
      std::getenv("NDN_SVS_DIAGNOSTIC_MAX_APP_PARAMS"),
      "NDN_SVS_DIAGNOSTIC_MAX_APP_PARAMS", 4096, 7168);"""
    if text.count(main_old) != 1:
        raise RuntimeError("main Data signer anchor changed")
    text = text.replace(main_old, main_new)

    self_old = """  producerSecurity.dataSigner->signingInfo.setSha256Signing();
  receiverSecurity.dataSigner->signingInfo.setSha256Signing();"""
    self_new = """  spec135::configureRsa2048(
    producerKeyChain, producerSecurity, Name("/spec135/selftest/producer"));
  spec135::configureRsa2048(
    receiverKeyChain, receiverSecurity, Name("/spec135/selftest/receiver"));"""
    if text.count(self_old) != 1:
        raise RuntimeError("self-test Data signer anchor changed")
    text = text.replace(self_old, self_new)

    ready_old = '              << " executionModel=single-face-io-thread" << std::endl;'
    ready_new = """              << " executionModel=single-face-io-thread"
              << " dataSigner=RSA-2048"
              << " rsaSignatureType=" << m_rsaSignatureType
              << " fetcherWindow="
              << readEnv("NDN_SVS_DIAGNOSTIC_FETCHER_WINDOW")
              << " maxApplicationParameters="
              << readEnv("NDN_SVS_DIAGNOSTIC_MAX_APP_PARAMS") << std::endl;"""
    if text.count(ready_old) != 1:
        raise RuntimeError("ready evidence anchor changed")
    text = text.replace(ready_old, ready_new)

    detail_old = '      "\\",\\"executionModel\\":\\"single-face-io-thread\\",\\"ioCpu\\":" +'
    detail_new = """      "\\",\\"dataSigner\\":\\"RSA-2048\\",\\"rsaSignatureType\\":" +
      std::to_string(m_rsaSignatureType) +
      ",\\"fetcherWindow\\":" +
      readEnv("NDN_SVS_DIAGNOSTIC_FETCHER_WINDOW") +
      ",\\"maxApplicationParameters\\":" +
      readEnv("NDN_SVS_DIAGNOSTIC_MAX_APP_PARAMS") +
      ",\\"executionModel\\":\\"single-face-io-thread\\",\\"ioCpu\\":" +"""
    if text.count(detail_old) != 1:
        raise RuntimeError("process-start evidence anchor changed")
    text = text.replace(detail_old, detail_new)

    member_old = "  uint64_t m_publishErrors = 0;\n"
    if text.count(member_old) != 1:
        raise RuntimeError("driver member anchor changed")
    text = text.replace(member_old, member_old + "  uint32_t m_rsaSignatureType = 0;\n")
    if text.count("setSha256Signing") != 0:
        raise RuntimeError("DigestSha256 signer remains in generated driver")
    GENERATED_DRIVER.parent.mkdir(parents=True, exist_ok=True)
    GENERATED_DRIVER.write_text(text, encoding="utf-8")


def build() -> dict[str, Any]:
    if MANIFEST.exists():
        raise RuntimeError(f"subject already frozen: {MANIFEST}")
    before = protected_state()
    ensure_worktree()
    apply_diagnostic_patch()
    if status(WORKTREE):
        raise RuntimeError("diagnostic worktree is dirty after commit")
    if git_value(WORKTREE, "HEAD^") != PARENT:
        raise RuntimeError("diagnostic commit parent mismatch")
    paths = run(["git", "diff", "--name-only", "HEAD^", "HEAD"],
                cwd=WORKTREE).splitlines()
    if paths != ["ndn-svs/fetcher.cpp", "ndn-svs/fetcher.hpp"]:
        raise RuntimeError(f"unexpected diagnostic commit paths: {paths}")
    patch_path = ROOT / "fetcher-window.patch"
    patch_path.parent.mkdir(parents=True, exist_ok=True)
    patch_path.write_bytes(run(["git", "diff", "--binary", "HEAD^", "HEAD"],
                               cwd=WORKTREE).encode())

    configure = [sys.executable, "waf", "configure", "--enable-shared", "--disable-static"]
    compile_library = [sys.executable, "waf", "build", "-j2"]
    run(configure, cwd=WORKTREE, log=ROOT / "configure.log")
    run(compile_library, cwd=WORKTREE, log=ROOT / "build.log")
    library = WORKTREE / "build/libndn-svs.so"
    if not library.is_file():
        raise RuntimeError("diagnostic libndn-svs.so missing")

    transform_driver()
    binary = ROOT / "bin/svs-fetcher-queue-causality"
    binary.parent.mkdir(parents=True, exist_ok=True)
    pkg = run(["pkg-config", "--cflags", "--libs", "libndn-cxx"]).split()
    command = [
        "g++", "-std=c++17", "-O2", "-pthread", "-DSPEC133_PROFILED=1",
        "-I", str(WORKTREE), "-I", str(WORKTREE / "build"),
        "-I", str(RSA_HELPER.parent), str(GENERATED_DRIVER),
        "-L", str(library.parent), "-lndn-svs", f"-Wl,-rpath,{library.parent}",
        *pkg, "-o", str(binary),
    ]
    run(command, log=ROOT / "driver-build.log")
    environment = dict(os.environ)
    environment.update({
        "LD_LIBRARY_PATH": str(library.parent),
        "NDN_LOG": "ndn_svs.Profile=TRACE",
        "NDN_SVS_PROFILE_ENABLED": "1",
        "NDN_SVS_PROFILE_CELL_ID": "spec135-self-test",
        "NDN_SVS_PROFILE_PEER_ID": "local",
        "NDN_SVS_PROFILE_SAMPLE_MODULUS": "1",
        "NDN_SVS_DIAGNOSTIC_FETCHER_WINDOW": "10",
        "NDN_SVS_DIAGNOSTIC_MAX_APP_PARAMS": "4096",
    })
    run([str(binary), "--self-test"], env=environment, log=ROOT / "self-test.log")
    self_test = (ROOT / "self-test.log").read_text(encoding="utf-8")
    if "SPEC133_SELF_TEST_OK" not in self_test or \
       "SPEC135_FETCHER_WINDOW window=10" not in self_test:
        raise RuntimeError("RSA diagnostic self-test evidence missing")
    linkage = run(["ldd", str(binary)])
    (ROOT / "binary-ldd.txt").write_text(linkage, encoding="utf-8")
    if re.search(r"boost[^\\n]*1\\.74", linkage, re.I):
        raise RuntimeError("Boost 1.74 residue in Spec 135 binary")
    if protected_state() != before:
        raise RuntimeError("a protected NDN-SVS worktree changed")

    parent_manifest = REPO / "build/spec133/subject-manifest-io.json"
    parent = json.loads(parent_manifest.read_text(encoding="utf-8"))
    manifest: dict[str, Any] = {
        "schemaVersion": "spec135-rsa-subject-v1",
        "subjectId": "spec135-rsa2048-sync-publish-fetcher-diagnostic",
        "parentHead": PARENT,
        "parentManifest": str(parent_manifest.resolve()),
        "parentManifestSha256": sha256(parent_manifest),
        "diagnosticHead": git_value(WORKTREE, "HEAD"),
        "diagnosticTree": git_value(WORKTREE, "HEAD^{tree}"),
        "diagnosticPatch": str(patch_path.resolve()),
        "diagnosticPatchSha256": sha256(patch_path),
        "diagnosticPatchPaths": paths,
        "profileWorktree": str(WORKTREE.resolve()),
        "profiledLibrary": str(library.resolve()),
        "profiledLibrarySha256": sha256(library),
        "profiledBinary": str(binary.resolve()),
        "profiledBinarySha256": sha256(binary),
        "generatedDriver": str(GENERATED_DRIVER.resolve()),
        "generatedDriverSha256": sha256(GENERATED_DRIVER),
        "driverTransform": str(Path(__file__).resolve()),
        "driverTransformSha256": sha256(Path(__file__)),
        "rsaHelper": str(RSA_HELPER.resolve()),
        "rsaHelperSha256": sha256(RSA_HELPER),
        "securityProfile": {
            "data": "RSA-2048/SignatureSha256WithRsa",
            "syncInterest": "HMAC",
            "validators": "disabled",
            "keyGenerationMeasured": False,
        },
        "executionModel": "single-face-io-thread",
        "publishApi": "publish",
        "parallelWorkers": None,
        "compressionEnabled": False,
        "profileConfig": parent["profileConfig"],
        "compileCommand": command,
        "configureCommand": configure,
        "buildCommand": compile_library,
        "linkage": str((ROOT / "binary-ldd.txt").resolve()),
        "selfTest": str((ROOT / "self-test.log").resolve()),
    }
    MANIFEST.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n",
                        encoding="utf-8")
    return manifest


def verify() -> dict[str, Any]:
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    for key in ("profiledLibrary", "profiledBinary", "generatedDriver",
                "diagnosticPatch", "driverTransform", "rsaHelper"):
        path = Path(manifest[key])
        if not path.is_file() or sha256(path) != manifest[f"{key}Sha256"]:
            raise RuntimeError(f"frozen artifact mismatch: {key}")
    if manifest["securityProfile"]["data"] != "RSA-2048/SignatureSha256WithRsa":
        raise RuntimeError("RSA security profile mismatch")
    if status(Path(manifest["profileWorktree"])):
        raise RuntimeError("diagnostic worktree is dirty")
    return manifest


def refresh_driver() -> dict[str, Any]:
    """Refresh only the not-yet-campaigned generated driver and its manifest."""
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    for key in ("profiledLibrary", "diagnosticPatch", "rsaHelper"):
        artifact = Path(manifest[key])
        if not artifact.is_file() or sha256(artifact) != manifest[f"{key}Sha256"]:
            raise RuntimeError(f"non-driver frozen artifact mismatch: {key}")
    transform_driver()
    library = Path(manifest["profiledLibrary"])
    binary = Path(manifest["profiledBinary"])
    pkg = run(["pkg-config", "--cflags", "--libs", "libndn-cxx"]).split()
    command = [
        "g++", "-std=c++17", "-O2", "-pthread", "-DSPEC133_PROFILED=1",
        "-I", str(WORKTREE), "-I", str(WORKTREE / "build"),
        "-I", str(RSA_HELPER.parent), str(GENERATED_DRIVER),
        "-L", str(library.parent), "-lndn-svs", f"-Wl,-rpath,{library.parent}",
        *pkg, "-o", str(binary),
    ]
    run(command, log=ROOT / "driver-build.log")
    environment = dict(os.environ)
    environment.update({
        "LD_LIBRARY_PATH": str(library.parent),
        "NDN_LOG": "ndn_svs.Profile=TRACE",
        "NDN_SVS_PROFILE_ENABLED": "1",
        "NDN_SVS_PROFILE_CELL_ID": "spec135-self-test",
        "NDN_SVS_PROFILE_PEER_ID": "local",
        "NDN_SVS_PROFILE_SAMPLE_MODULUS": "1",
        "NDN_SVS_DIAGNOSTIC_FETCHER_WINDOW": "10",
        "NDN_SVS_DIAGNOSTIC_MAX_APP_PARAMS": "4096",
    })
    run([str(binary), "--self-test"], env=environment, log=ROOT / "self-test.log")
    linkage = run(["ldd", str(binary)])
    (ROOT / "binary-ldd.txt").write_text(linkage, encoding="utf-8")
    if re.search(r"boost[^\\n]*1\\.74", linkage, re.I):
        raise RuntimeError("Boost 1.74 residue in refreshed Spec 135 binary")
    manifest.update({
        "profiledBinarySha256": sha256(binary),
        "generatedDriverSha256": sha256(GENERATED_DRIVER),
        "driverTransformSha256": sha256(Path(__file__)),
        "compileCommand": command,
    })
    MANIFEST.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n",
                        encoding="utf-8")
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=("build", "refresh-driver", "verify"))
    args = parser.parse_args()
    if args.mode == "build":
        result = build()
    elif args.mode == "refresh-driver":
        result = refresh_driver()
    else:
        result = verify()
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
