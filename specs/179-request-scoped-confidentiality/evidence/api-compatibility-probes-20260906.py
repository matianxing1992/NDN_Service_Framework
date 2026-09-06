#!/usr/bin/env python3
"""Compile identical pre-UAV application calls against old/current headers.

This is an audit probe, not a runtime acceptance gate. Negative current-header
results deliberately document source breaks; every probe must first compile
against its historical headers. Logs and exported sources stay in results/.
"""
import io
import json
import pathlib
import shlex
import subprocess
import sys
import tarfile
from concurrent.futures import ThreadPoolExecutor

ROOT = pathlib.Path(__file__).resolve().parents[3]
NAC = ROOT.parent / "NAC-ABE"
OUT = ROOT / "results/api-compatibility-audit-20260906"
OUT.mkdir(parents=True, exist_ok=True)
PREFIX = ROOT / ".deps/nac-abe-spec179-official"
ONLY = set(sys.argv[1:])


def export(repo, revision, label, paths):
    dest = OUT / label
    if ONLY and dest.exists():
        return dest
    dest.mkdir(exist_ok=True)
    archive = subprocess.check_output(["git", "archive", revision, *paths], cwd=repo)
    with tarfile.open(fileobj=io.BytesIO(archive)) as stream:
        stream.extractall(dest)
    return dest


old = export(ROOT, "37f8a468", "ndnsf-baseline", ["ndn-service-framework", "examples"])
nac_old = export(NAC, "1cc17d9", "nac-baseline", ["src", "examples"])
nac_official = export(NAC, "58f3948", "nac-official", ["src", "examples"])
for tree in (nac_old, nac_official):
    link = tree / "nac-abe"
    if not link.exists():
        link.symlink_to("src", target_is_directory=True)

COMMON = ["/usr/bin/clang++-10", "-std=c++17", "-fsyntax-only", "-pthread",
          "-DNAC_ABE_CMAKE_BUILD", "-DBOOST_STACKTRACE_DYN_LINK", "-DBOOST_LOG_DYN_LINK",
          "-I/usr/local/include"]
CASES = {
    "ordinary-dynamic-api": ('#include "ndn-service-framework/ServiceProvider.hpp"\n'
        '#include "ndn-service-framework/ServiceUser.hpp"\n'
        'using namespace ndn_service_framework;\n'
        'struct Payload { bool ParseFromArray(const void*, int) { return true; } bool SerializeToString(std::string*) const { return true; } };\n'
        'void use(ServiceProvider& p, ServiceUser& u) {\n'
        'p.addService(ndn::Name("/HELLO"), ServiceProvider::LegacyAckStrategyHandler{}, ServiceProvider::RequestHandler{});\n'
        'p.addHandler<Payload, Payload>(ndn::Name("/HELLO"), [](const ndn::Name&, const Payload&, Payload&) {});\n'
        'u.RequestService<Payload, Payload>({}, ndn::Name("/HELLO"), Payload{}, [](const Payload&) {}, [] {}, 1000);\n}\n'),
    "removed-large-response-helper": ('#include "ndn-service-framework/ServiceProvider.hpp"\n'
        'void use(ndn_service_framework::ServiceProvider& p, ndn_service_framework::ResponseMessage r) {\n'
        'p.makeResponseWithLargeDataOptimization(ndn::Name("/u"), ndn::Name("/S"), ndn::Name("/r"), r);\n}\n'),
    "large-reference-aggregate": ('#include "ndn-service-framework/utils.hpp"\n'
        'ndn_service_framework::LargeDataReference r{ndn::Name("/data"), "response", "id", 1024, true, "digest"};\n'),
    "stream-binding-aggregate": ('#include "ndn-service-framework/InvocationStream.hpp"\n'
        'ndn_service_framework::StreamBinding b{ndn::Name("/r"), ndn::Name("/u"), ndn::Name("/S"), ndn::Name("/p"), "boot", 1, {}, {}, 1, {}, {}, 1, 12345};\n'),
    "stream-options-aggregate": ('#include "ndn-service-framework/InvocationStream.hpp"\n'
        'ndn_service_framework::StreamRequestOptions o{1, ndn_service_framework::InvocationMode::Normal, {}, 1, 0, {}, 12345, std::nullopt, 512};\n'),
    "hybrid-receive-member-pointer": ('#include "ndn-service-framework/HybridMessageCrypto.hpp"\n'
        'auto member = &ndn_service_framework::HybridMessageCrypto::cacheReceiveKey;\n'),
    "hybrid-wrapped-member-pointer": ('#include "ndn-service-framework/HybridMessageCrypto.hpp"\n'
        'auto member = &ndn_service_framework::HybridMessageCrypto::cacheWrappedSendKey;\n'),
    "hybrid-ordinary-calls": ('#include "ndn-service-framework/HybridMessageCrypto.hpp"\n'
        'void use(ndn_service_framework::HybridMessageCrypto& c, ndn::Buffer b) { c.cacheReceiveKey("k", "e", b); c.cacheWrappedSendKey("k", b); }\n'),
    "nac-fetch-call": ('#include <nac-abe/consumer.hpp>\n#include <nac-abe/param-fetcher.hpp>\n'
        'void use(ndn::nacabe::ParamFetcher& p) { p.fetchPublicParams(); }\n'),
    "nac-fetch-member-pointer": ('#include <nac-abe/consumer.hpp>\n#include <nac-abe/param-fetcher.hpp>\n'
        'auto member = &ndn::nacabe::ParamFetcher::fetchPublicParams;\n'),
    "nac-fetch-typed-pointer": ('#include <nac-abe/consumer.hpp>\n#include <nac-abe/param-fetcher.hpp>\n'
        'void (ndn::nacabe::ParamFetcher::*member)() = &ndn::nacabe::ParamFetcher::fetchPublicParams;\n'),
}
rows = []
jobs = []


def compile_case(label, source, lane, framework, nac_include):
    jobs.append((label, source, lane, framework, nac_include))


def compile_one(job):
    label, source, lane, framework, nac_include = job
    # NAC algo headers use a bare common.hpp. Its own directory must precede
    # NDNSF's identically named common.hpp, as in the dependency's CMake build.
    args = COMMON + ["-I" + str(nac_include / "nac-abe"),
                     "-I" + str(nac_include / "src"),
                     "-I" + str(nac_include), "-I" + str(framework),
                     "-I" + str(framework / "ndn-service-framework"),
                     str(source)]
    run = subprocess.run(args, text=True, capture_output=True)
    log = OUT / (label + "-" + lane + ".log")
    log.write_text(shlex.join(args) + "\n" + run.stdout + run.stderr)
    errors = [line for line in run.stderr.splitlines() if "error:" in line]
    row = {"case": label, "headers": lane, "exit": run.returncode,
           "firstError": errors[0] if errors else "", "log": str(log.relative_to(ROOT))}
    rows.append(row)
    print(json.dumps(row), flush=True)


for label, code in CASES.items():
    if ONLY and label not in ONLY:
        continue
    source = OUT / (label + ".cpp")
    source.write_text(code)
    compile_case(label, source, "baseline", old, nac_old)
    if label.startswith("nac-"):
        compile_case(label, source, "official", old, nac_official)
    compile_case(label, source, "current", ROOT, PREFIX / "include")

# These are byte-for-byte historical applications, not rewritten lookalikes.
for name in ["App_User", "App_Provider", "App_ServiceController"]:
    if ONLY and "old-" + name not in ONLY:
        continue
    for lane, framework, nac_include in [("baseline", old, nac_old),
                                         ("current", ROOT, PREFIX / "include")]:
        compile_case("old-" + name, old / "examples" / (name + ".cpp"), lane, framework, nac_include)
for name in ["kp-aa-example", "kp-consumer-example", "kp-producer-example"]:
    if ONLY and "old-" + name not in ONLY:
        continue
    for lane, nac_include in [("baseline", nac_old), ("official", nac_official),
                              ("current", PREFIX / "include")]:
        # Example files include bare headers, as in the NAC CMake build.
        example = nac_old / "examples" / (name + ".cpp")
        compile_case("old-" + name, example, lane, old, nac_include)

if not jobs or (ONLY and ONLY != {job[0] for job in jobs}):
    raise SystemExit("Unknown or empty probe selection")

with ThreadPoolExecutor(max_workers=2) as pool:
    list(pool.map(compile_one, jobs))
rows.sort(key=lambda row: (row["case"], row["headers"]))

summary = {"ndnsf": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
           "nac": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=NAC, text=True).strip(),
           "compiler": subprocess.check_output([COMMON[0], "--version"], text=True).splitlines()[0],
           "rows": rows}
summary_name = "summary" + ("-" + "-".join(sorted(ONLY)) if ONLY else "") + ".json"
(OUT / summary_name).write_text(json.dumps(summary, indent=2) + "\n")
bad_baselines = [r for r in rows if r["headers"] != "current" and r["exit"]]
print("Baseline/setup failures:", len(bad_baselines), flush=True)
raise SystemExit(bool(bad_baselines))
