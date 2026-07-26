from __future__ import annotations

import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[2]
BUILDER_PATH = REPO / "Experiments/build_svs_sync_stage_profile.py"
RUNNER_PATH = REPO / "Experiments/NDN_SVS_Sync_Stage_Profile_Minindn.py"
ANALYZER_PATH = REPO / "Experiments/analyze_svs_sync_stage_profile.py"


def load_builder():
    spec = importlib.util.spec_from_file_location("spec133_builder", BUILDER_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {BUILDER_PATH}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def load_runner():
    spec = importlib.util.spec_from_file_location("spec133_runner", RUNNER_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {RUNNER_PATH}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def load_analyzer():
    spec = importlib.util.spec_from_file_location("spec133_analyzer", ANALYZER_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {ANALYZER_PATH}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class Spec133SubjectBuilderContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.builder = load_builder()

    def test_exact_historical_identity_and_branch_names(self):
        self.assertEqual(
            self.builder.BASE_COMMIT,
            "a9944019f76791773604999f00128057b9534ace",
        )
        self.assertEqual(
            self.builder.BASE_TREE,
            "945a321d473f44f29e8349a83ce60373f3e37420",
        )
        self.assertEqual(self.builder.CLEAN_BRANCH, "spec133-build-clean-control")
        self.assertEqual(self.builder.PROFILE_BRANCH, "spec133-build-sync-stage-profile")

    def test_boost_patch_identity_is_canonical_spec132_content(self):
        patch = self.builder.canonical_patch_bytes().decode("utf-8")
        self.assertTrue(patch.startswith("spec132-boost171-build-patch-v1\n"))
        self.assertIn("BOOST_VERSION_NUMBER < 107400", patch)
        self.assertIn("BOOST_VERSION_NUMBER < 107100", patch)
        self.assertIn("minimum supported version of Boost is 1.74.0", patch)
        self.assertIn("minimum supported version of Boost is 1.71.0", patch)
        self.assertNotIn("NDNSF", patch)

    def test_profile_patch_allowlist_is_diagnostics_only(self):
        expected = {
            "ndn-svs/profile.hpp",
            "ndn-svs/profile.cpp",
            "ndn-svs/svspubsub.cpp",
            "ndn-svs/svsync-base.cpp",
            "ndn-svs/core.cpp",
            "ndn-svs/mapping-provider.cpp",
            "ndn-svs/fetcher.hpp",
            "ndn-svs/fetcher.cpp",
            "ndn-svs/version-vector.cpp",
            "ndn-svs/store-memory.hpp",
            "wscript",
        }
        self.assertEqual(self.builder.PROFILE_PATCH_ALLOWLIST, expected)

    def test_validate_profile_paths_rejects_missing_and_unexpected_changes(self):
        with self.assertRaisesRegex(RuntimeError, "profiling patch is empty"):
            self.builder.validate_profile_paths([])
        with self.assertRaisesRegex(RuntimeError, "unexpected profiling patch paths"):
            self.builder.validate_profile_paths(["ndn-svs/core.cpp", "README.md"])
        self.builder.validate_profile_paths(["ndn-svs/core.cpp", "ndn-svs/profile.cpp"])

    def test_compression_audit_requires_explicit_disabled_config(self):
        self.builder.verify_compression_disabled(
            "/* #undef NDN_SVS_COMPRESSION */\n"
        )
        self.builder.verify_compression_disabled("#define NDN_SVS_COMPRESSION 0\n")
        with self.assertRaisesRegex(RuntimeError, "compression is not disabled"):
            self.builder.verify_compression_disabled("#define NDN_SVS_COMPRESSION 1\n")
        with self.assertRaisesRegex(RuntimeError, "compression state missing"):
            self.builder.verify_compression_disabled("#define OTHER 1\n")

    def test_foundation_schema_rejects_wrong_identity_or_missing_artifact(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            library = root / "libndn-svs.so"
            library.write_bytes(b"clean-library")
            foundation = {
                "schemaVersion": self.builder.FOUNDATION_SCHEMA,
                "baseCommit": self.builder.BASE_COMMIT,
                "baseTree": self.builder.BASE_TREE,
                "cleanHead": "1" * 40,
                "cleanTree": "2" * 40,
                "cleanWorktree": str(root / "clean"),
                "profileWorktree": str(root / "profile"),
                "cleanLibrary": str(library),
                "cleanLibrarySha256": self.builder.sha256_file(library),
                "canonicalBoostPatchSha256": "3" * 64,
                "compressionEnabled": False,
            }
            path = root / "subject-foundation.json"
            path.write_text(json.dumps(foundation), encoding="utf-8")
            loaded = self.builder.load_foundation(path, require_worktrees=False)
            self.assertEqual(loaded["baseCommit"], self.builder.BASE_COMMIT)

            foundation["baseCommit"] = "0" * 40
            path.write_text(json.dumps(foundation), encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "foundation base commit"):
                self.builder.load_foundation(path, require_worktrees=False)

    def test_finalize_dry_contract_rejects_missing_reviewable_patch(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            library = root / "libndn-svs.so"
            library.write_bytes(b"clean-library")
            profile = REPO / "build/spec133/worktrees/sync-stage-profile"
            foundation = {
                "schemaVersion": self.builder.FOUNDATION_SCHEMA,
                "baseCommit": self.builder.BASE_COMMIT,
                "baseTree": self.builder.BASE_TREE,
                "cleanHead": "1" * 40,
                "cleanTree": "2" * 40,
                "cleanWorktree": str(root / "clean"),
                "profileWorktree": str(profile),
                "cleanLibrary": str(library),
                "cleanLibrarySha256": self.builder.sha256_file(library),
                "canonicalBoostPatchSha256": "3" * 64,
                "compressionEnabled": False,
            }
            path = root / "subject-foundation.json"
            path.write_text(json.dumps(foundation), encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "profiling patch is empty"):
                self.builder.finalize(root, foundation_path=path, dry_contract=True)

    def test_protected_snapshot_tolerates_existing_stale_worktree_metadata(self):
        source = BUILDER_PATH.read_text(encoding="utf-8")
        self.assertIn('"MISSING_WORKTREE_PATH"', source)
        self.assertNotIn('worktree", "prune"', source)


class Spec133ProfilerPrimitiveTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.builder = load_builder()
        cls.profile_root = REPO / "build/spec133/worktrees/sync-stage-profile"

    def test_fixed_registry_covers_contract_and_has_no_worker(self):
        header = (self.profile_root / "ndn-svs/profile.hpp").read_text(encoding="utf-8")
        source = (self.profile_root / "ndn-svs/profile.cpp").read_text(encoding="utf-8")
        for stage in (
            "PUB_INNER_SIGN",
            "SYNC_MAPPING_CANDIDATE_ENCODE",
            "MAP_DATA_SIGN",
            "MAP_LIST_DECODE",
            "PAYLOAD_INNER_DECODE",
            "PUB_EXTRA_DATA_LOCK_WAIT",
        ):
            self.assertIn(stage, header)
        self.assertIn("CLOCK_MONOTONIC_RAW", source)
        self.assertIn("NDN_LOG_INIT(ndn_svs.Profile)", source)
        self.assertIn("schema=spec133-stage-span-v1", source)
        self.assertIn("schema=spec133-stage-summary-v1", source)
        self.assertNotIn("std::thread", header + source)
        self.assertNotIn("boost::asio::post", header + source)

    def test_runtime_registry_exactly_matches_frozen_stage_contract(self):
        header = (self.profile_root / "ndn-svs/profile.hpp").read_text(encoding="utf-8")
        contract = (REPO / "specs/133-svs-sync-stage-profiling/contracts/"
                    "stage-measurement-contract.md").read_text(encoding="utf-8")
        runtime = set(re.findall(r'X\([A-Z0-9_]+, "([A-Z]+\.[A-Z0-9_.]+)"', header))
        frozen = set(re.findall(r'^\| `([A-Z]+\.[A-Z0-9_.]+)` \|', contract, re.M))
        self.assertEqual(runtime, frozen)

    def test_profiler_compiles_and_emits_span_summary_and_lifecycle(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            program = root / "profile-smoke.cpp"
            program.write_text(
                """
#include "ndn-svs/profile.hpp"

int main()
{
  using namespace ndn::svs::profile;
  {
    Span parent(StageId::PUB_TOTAL, "peer-a:1");
    {
      Span child(StageId::PUB_INNER_SIGN, "peer-a:1");
      child.setCounts(64, 1);
    }
  }
  Profiler::get().flush();
  return 0;
}
""",
                encoding="utf-8",
            )
            binary = root / "profile-smoke"
            pkg = shlex.split(
                subprocess.run(
                    ["pkg-config", "--cflags", "--libs", "libndn-cxx"],
                    check=True,
                    text=True,
                    stdout=subprocess.PIPE,
                ).stdout
            )
            command = [
                "g++", "-std=c++17", "-O2", "-pthread",
                "-I", str(self.profile_root),
                "-I", str(self.profile_root / "build"),
                str(program),
                str(self.profile_root / "ndn-svs/profile.cpp"),
                *pkg,
                "-o", str(binary),
            ]
            compiled = subprocess.run(command, check=False, text=True, stdout=subprocess.PIPE,
                                      stderr=subprocess.STDOUT)
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            env = dict(os.environ)
            env.update({
                "NDN_LOG": "ndn_svs.Profile=TRACE",
                "NDN_SVS_PROFILE_ENABLED": "1",
                "NDN_SVS_PROFILE_CELL_ID": "smoke",
                "NDN_SVS_PROFILE_PEER_ID": "peer-a",
                "NDN_SVS_PROFILE_SAMPLE_MODULUS": "1",
            })
            result = subprocess.run([str(binary)], check=False, text=True, env=env,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(result.returncode, 0, result.stdout)
            output = result.stdout
            self.assertIn("schema=spec133-profile-lifecycle-v1 event=profile-start", output)
            self.assertIn("schema=spec133-stage-span-v1 event=stage-span", output)
            self.assertIn("stage=PUB.TOTAL", output)
            self.assertIn("stage=PUB.INNER_SIGN", output)
            self.assertIn("schema=spec133-stage-summary-v1 event=stage-summary", output)
            self.assertIn("schema=spec133-profile-lifecycle-v1 event=profile-stop", output)
            self.assertIn("sampleModulus=1", output)

    def test_t004_publisher_and_sync_production_stage_anchors_exist(self):
        svspubsub = (self.profile_root / "ndn-svs/svspubsub.cpp").read_text(encoding="utf-8")
        base = (self.profile_root / "ndn-svs/svsync-base.cpp").read_text(encoding="utf-8")
        core = (self.profile_root / "ndn-svs/core.cpp").read_text(encoding="utf-8")
        for stage in (
            "PUB_TOTAL", "PUB_INNER_BUILD", "PUB_INNER_SIGN",
            "PUB_INNER_WIRE_ENCODE", "PUB_EXTRA_DATA_LOCK_WAIT",
            "MAP_TIMESTAMP_BUILD", "MAP_NOTIFICATION_ENQUEUE", "MAP_STORE_INSERT",
            "SYNC_EXTRA_BLOCK_TOTAL", "SYNC_MAPPING_CANDIDATE_ENCODE",
            "SYNC_PIGGY_WIRE_SCAN", "SYNC_EXTRA_BLOCK_FINAL_ENCODE",
        ):
            self.assertIn(f"StageId::{stage}", svspubsub)
        for stage in (
            "PUB_OUTER_BUILD", "PUB_OUTER_SIGN", "PUB_OUTER_STORE_INSERT",
            "PUB_OUTER_FACE_PUT",
        ):
            self.assertIn(f"StageId::{stage}", base)
        for stage in (
            "PUB_VV_READ_LOCK_WAIT", "PUB_SEQ_READ", "PUB_VV_UPDATE_LOCK_WAIT",
            "PUB_LOCAL_STATE_UPDATE", "PUB_SCHEDULER_LOCK_WAIT",
            "SYNC_TIMER_ARM_CANCEL", "SYNC_TIMER_WAIT", "SYNC_PRODUCE_TOTAL",
            "SYNC_VV_ENCODE_LOCK_WAIT", "SYNC_VV_ENCODE",
            "SYNC_APP_PARAMS_ENCODE", "SYNC_INTEREST_BUILD",
            "SYNC_INTEREST_SIGN", "SYNC_INTEREST_EXPRESS",
        ):
            self.assertIn(f"StageId::{stage}", core)
        self.assertIn("std::unique_lock<std::mutex> lock(m_extraDataMutex, std::defer_lock)",
                      svspubsub)
        self.assertIn("lock.lock();\n  lockSpan.stop();", svspubsub)

    def test_t005_receive_mapping_fetcher_and_payload_stage_anchors_exist(self):
        core = (self.profile_root / "ndn-svs/core.cpp").read_text(encoding="utf-8")
        svspubsub = (self.profile_root / "ndn-svs/svspubsub.cpp").read_text(encoding="utf-8")
        mapping = (self.profile_root / "ndn-svs/mapping-provider.cpp").read_text(encoding="utf-8")
        fetcher_hpp = (self.profile_root / "ndn-svs/fetcher.hpp").read_text(encoding="utf-8")
        fetcher = (self.profile_root / "ndn-svs/fetcher.cpp").read_text(encoding="utf-8")
        base = (self.profile_root / "ndn-svs/svsync-base.cpp").read_text(encoding="utf-8")

        for stage in (
            "SYNC_RECEIVE_TOTAL", "SYNC_INTEREST_VERIFY", "SYNC_APP_PARAMS_PARSE",
            "SYNC_VV_DECODE", "SYNC_VV_MERGE_LOCK_WAIT", "SYNC_VV_MERGE",
            "SYNC_UPDATE_DISPATCH", "SYNC_SUPPRESSION_DECISION",
            "SYNC_RECORDED_VV_LOCK_WAIT", "SYNC_SCHEDULER_LOCK_WAIT",
        ):
            self.assertIn(f"StageId::{stage}", core)
        for stage in (
            "SYNC_EXTRA_MAPPING_DECODE", "SYNC_PIGGY_DATA_DECODE_CACHE",
            "MAP_PROCESS_TOTAL", "MAP_LOCAL_LOOKUP", "MAP_FRESHNESS_CHECK",
            "MAP_SUBSCRIPTION_MATCH", "MAP_EXTRA_DATA_LOCK_WAIT",
            "MAP_PIGGY_CACHE_LOOKUP", "MAP_FETCH_QUEUE_INSERT",
            "MAP_PIGGY_CALLBACK", "PAYLOAD_INNER_DECODE",
            "PAYLOAD_INNER_VERIFY", "PAYLOAD_SUBSCRIPTION_CALLBACK",
        ):
            self.assertIn(f"StageId::{stage}", svspubsub)
        for stage in (
            "MAP_INTEREST_BUILD", "MAP_QUERY_PARSE", "MAP_RANGE_LOOKUP",
            "MAP_LIST_ENCODE", "MAP_DATA_BUILD", "MAP_DATA_SIGN",
            "MAP_DATA_FACE_PUT", "MAP_CONTENT_EXTRACT", "MAP_LIST_DECODE",
            "MAP_REMOTE_STORE_INSERT",
        ):
            self.assertIn(f"StageId::{stage}", mapping)
        for stage in (
            "MAP_FETCHER_QUEUE_WAIT", "MAP_INTEREST_EXPRESS", "MAP_NETWORK_WAIT",
            "MAP_DATA_VERIFY", "PAYLOAD_FETCHER_QUEUE_WAIT",
            "PAYLOAD_INTEREST_EXPRESS", "PAYLOAD_NETWORK_WAIT", "PAYLOAD_OUTER_VERIFY",
        ):
            self.assertIn(f"StageId::{stage}", fetcher)
        for stage in (
            "PAYLOAD_INTEREST_BUILD", "PAYLOAD_PROVIDER_STORE_FIND",
            "PAYLOAD_PROVIDER_FACE_PUT", "PAYLOAD_OUTER_CACHE_INSERT",
        ):
            self.assertIn(f"StageId::{stage}", base)

        self.assertIn("profileTraceId", fetcher_hpp)
        self.assertIn("profileEnqueueRawNs", fetcher_hpp)
        self.assertIn("profileExpressRawNs", fetcher_hpp)
        self.assertIn("CorrelationMode::Exact", fetcher)
        self.assertIn("Outcome::Nack", fetcher)
        self.assertIn("Outcome::Timeout", fetcher)
        self.assertNotIn("std::mutex m_map", mapping)

    def test_t005_fetcher_runtime_reports_both_paths_and_terminal_outcomes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            program = root / "fetcher-profile-smoke.cpp"
            program.write_text(
                r'''
#include "ndn-svs/fetcher.hpp"
#include "ndn-svs/mapping-provider.hpp"
#include "ndn-svs/profile.hpp"
#include "ndn-svs/svspubsub.hpp"

#include <boost/asio/io_context.hpp>
#include <ndn-cxx/util/dummy-client-face.hpp>

#include <chrono>
#include <stdexcept>
#include <thread>

using namespace ndn;
using namespace ndn::svs;
using namespace std::chrono_literals;

static void pump(DummyClientFace& face, std::chrono::milliseconds duration)
{
  face.getIoContext().restart();
  face.getIoContext().run_for(duration);
}

int main()
{
  DummyClientFace face;
  KeyChain keyChain("pib-memory:spec133-fetcher", "tpm-memory:spec133-fetcher");
  keyChain.createIdentity("/spec133-fetcher");
  SecurityOptions security(keyChain);
  Fetcher fetcher(face, security);
  int satisfied = 0;
  int nacked = 0;
  int timedOut = 0;

  auto success = [&](const Interest&, const Data&) { ++satisfied; };
  auto nack = [&](const Interest&, const lp::Nack&) { ++nacked; };
  auto timeout = [&](const Interest&) { ++timedOut; };

  Interest mapping("/peer/sync/MAPPING/1/1");
  mapping.setInterestLifetime(20_ms);
  fetcher.expressInterest(mapping, success, nack, timeout);
  pump(face, 2ms);
  Data mappingData(mapping.getName());
  mappingData.setContent(make_span(reinterpret_cast<const uint8_t*>("m"), 1));
  keyChain.sign(mappingData);
  face.receive(mappingData);
  pump(face, 2ms);

  Interest payload("/peer/sync/DATA/1");
  payload.setInterestLifetime(20_ms);
  fetcher.expressInterest(payload, success, nack, timeout);
  pump(face, 2ms);
  Data payloadData(payload.getName());
  payloadData.setContent(make_span(reinterpret_cast<const uint8_t*>("p"), 1));
  keyChain.sign(payloadData);
  face.receive(payloadData);
  pump(face, 2ms);

  Interest nackedInterest("/peer/sync/DATA/2");
  nackedInterest.setInterestLifetime(20_ms);
  fetcher.expressInterest(nackedInterest, success, nack, timeout);
  pump(face, 2ms);
  lp::Nack networkNack(face.sentInterests.back());
  networkNack.setReason(lp::NackReason::CONGESTION);
  face.receive(networkNack);
  pump(face, 2ms);

  Interest timeoutInterest("/peer/sync/DATA/3");
  timeoutInterest.setInterestLifetime(1_ms);
  fetcher.expressInterest(timeoutInterest, success, nack, timeout);
  pump(face, 5ms);

  DummyClientFace mappingProviderFace(DummyClientFace::Options{true, true});
  MappingProvider mappingProvider("/sync", "/producer", mappingProviderFace, security);
  mappingProvider.insertMapping("/producer", 1, {Name("/app/one"), {}});
  pump(mappingProviderFace, 5ms);
  mappingProviderFace.sentData.clear();
  Name mappingQuery("/producer");
  mappingQuery.append(Name("/sync")).append("MAPPING").appendNumber(1).appendNumber(1);
  mappingProviderFace.receive(Interest(mappingQuery));
  pump(mappingProviderFace, 5ms);
  if (mappingProviderFace.sentData.empty())
    throw std::runtime_error("mapping provider did not answer");

  DummyClientFace payloadProviderFace(DummyClientFace::Options{true, true});
  SVSPubSubOptions options;
  options.useTimestamp = false;
  SVSPubSub payloadProvider("/payload-sync", "/payload-provider", payloadProviderFace,
                            [](const std::vector<MissingDataInfo>&) {}, options, security);
  pump(payloadProviderFace, 5ms);
  const uint8_t providerPayload[] = {'d'};
  const SeqNo providerSeq =
    payloadProvider.publish("/app/provider", make_span(providerPayload, sizeof(providerPayload)));
  pump(payloadProviderFace, 5ms);
  Name outerName;
  Data outerData;
  for (const auto& candidate : payloadProviderFace.sentData) {
    if (Name("/payload-provider").isPrefixOf(candidate.getName())) {
      outerName = candidate.getName();
      outerData = candidate;
      break;
    }
  }
  if (outerName.empty())
    throw std::runtime_error("payload provider did not publish outer Data");
  payloadProviderFace.sentData.clear();
  payloadProviderFace.receive(Interest(outerName));
  pump(payloadProviderFace, 5ms);
  if (payloadProviderFace.sentData.empty())
    throw std::runtime_error("payload provider store did not answer");

  DummyClientFace receiverFace(DummyClientFace::Options{true, true});
  SVSPubSub receiver("/payload-sync", "/receiver", receiverFace,
                     [](const std::vector<MissingDataInfo>&) {}, options, security);
  int delivered = 0;
  receiver.subscribe("/app", [&](const SVSPubSub::SubscriptionData&) { ++delivered; });
  receiver.insertMapping("/payload-provider", providerSeq, "/app/provider", {});
  if (!receiver.processMapping("/payload-provider", providerSeq))
    throw std::runtime_error("receiver did not queue fallback payload");
  receiver.onSyncData(outerData, {Name("/payload-provider"), providerSeq});
  if (delivered != 1)
    throw std::runtime_error("receiver did not deliver decoded inner Data");

  DummyClientFace coreFace(DummyClientFace::Options{true, true});
  int updates = 0;
  SVSyncCore core(coreFace, "/core-sync",
                  [&](const std::vector<MissingDataInfo>& missing) {
                    updates += static_cast<int>(missing.size());
                  }, security, "/core-local");
  VersionVector remoteVector;
  remoteVector.set("/core-remote", 1);
  Block parameters(ndn::tlv::ApplicationParameters);
  parameters.push_back(remoteVector.encode());
  parameters.encode();
  Interest syncInterest("/core-sync/v=2");
  syncInterest.setApplicationParameters(parameters);
  core.onSyncInterest(syncInterest);
  if (updates != 1)
    throw std::runtime_error("core receive path did not dispatch missing range");

  profile::Profiler::get().flush();
  if (satisfied != 2 || nacked != 1 || timedOut != 1)
    throw std::runtime_error("unexpected Fetcher callback counts");
  return 0;
}
''',
                encoding="utf-8",
            )
            binary = root / "fetcher-profile-smoke"
            pkg = shlex.split(
                subprocess.run(
                    ["pkg-config", "--cflags", "--libs", "libndn-cxx"],
                    check=True, text=True, stdout=subprocess.PIPE,
                ).stdout
            )
            build_dir = self.profile_root / "build"
            command = [
                "g++", "-std=c++17", "-O2", "-pthread", "-DNDN_SVS_HAVE_TESTS",
                "-I", str(self.profile_root),
                "-I", str(build_dir),
                str(program), "-L", str(build_dir), "-lndn-svs",
                f"-Wl,-rpath,{build_dir}", *pkg, "-o", str(binary),
            ]
            compiled = subprocess.run(command, check=False, text=True, stdout=subprocess.PIPE,
                                      stderr=subprocess.STDOUT)
            self.assertEqual(compiled.returncode, 0, compiled.stdout)
            env = dict(os.environ)
            env.update({
                "NDN_LOG": "ndn_svs.Profile=TRACE",
                "NDN_SVS_PROFILE_ENABLED": "1",
                "NDN_SVS_PROFILE_CELL_ID": "t005-fetcher",
                "NDN_SVS_PROFILE_PEER_ID": "local",
                "NDN_SVS_PROFILE_SAMPLE_MODULUS": "1",
            })
            result = subprocess.run([str(binary)], check=False, text=True, env=env,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            self.assertEqual(result.returncode, 0, result.stdout)
            output = result.stdout
            self.assertRegex(output, r"stage=MAP\.NETWORK_WAIT .*outcome=success")
            self.assertRegex(output, r"stage=MAP\.DATA_VERIFY .*outcome=skipped")
            self.assertRegex(output, r"stage=PAYLOAD\.NETWORK_WAIT .*outcome=success")
            self.assertRegex(output, r"stage=PAYLOAD\.NETWORK_WAIT .*outcome=nack")
            self.assertRegex(output, r"stage=PAYLOAD\.NETWORK_WAIT .*outcome=timeout")
            self.assertRegex(output, r"stage=PAYLOAD\.OUTER_VERIFY .*outcome=skipped")
            self.assertRegex(output, r"stage=MAP\.QUERY_PARSE .*outcome=success")
            self.assertRegex(output, r"stage=MAP\.RANGE_LOOKUP .*outcome=success")
            self.assertRegex(output, r"stage=MAP\.DATA_SIGN .*outcome=success")
            self.assertRegex(output, r"stage=MAP\.DATA_FACE_PUT .*outcome=success")
            self.assertRegex(output, r"stage=PAYLOAD\.PROVIDER_STORE_FIND .*outcome=success")
            self.assertRegex(output, r"stage=PAYLOAD\.PROVIDER_FACE_PUT .*outcome=success")
            self.assertRegex(output, r"stage=PAYLOAD\.INNER_DECODE .*outcome=success")
            self.assertRegex(output, r"stage=PAYLOAD\.INNER_VERIFY .*outcome=skipped")
            self.assertRegex(output, r"stage=PAYLOAD\.SUBSCRIPTION_CALLBACK .*outcome=success")
            self.assertRegex(output, r"stage=SYNC\.RECEIVE_TOTAL .*outcome=success")
            self.assertRegex(output, r"stage=SYNC\.INTEREST_VERIFY .*outcome=skipped")
            self.assertRegex(output, r"stage=SYNC\.APP_PARAMS_PARSE .*outcome=success")
            self.assertRegex(output, r"stage=SYNC\.VV_DECODE .*outcome=success")
            self.assertRegex(output, r"stage=SYNC\.VV_MERGE .*outcome=success")
            self.assertRegex(output, r"stage=SYNC\.UPDATE_DISPATCH .*outcome=success")
            self.assertRegex(output, r"stage=SYNC\.SCHEDULER_LOCK_WAIT .*outcome=success")

    def test_t006_driver_is_direct_synchronous_and_self_tests_clean_and_profiled(self):
        driver = REPO / "Experiments/ndn-svs-pubsub-benchmark/svs-sync-stage-profile.cpp"
        source = driver.read_text(encoding="utf-8")
        self.assertIn("m_pubsub->publish(name, make_span(payload))", source)
        self.assertNotIn("publishAsync", source)
        self.assertNotIn("Scheduler", source)
        self.assertNotIn("pacer", source.lower())
        self.assertNotIn("NDNSF", source)
        self.assertNotIn("std::thread", source)
        self.assertNotIn("sleep_until", source)
        self.assertIn("boost::asio::steady_timer", source)
        self.assertIn("m_face.processEvents", source)
        self.assertIn("--io-cpu", source)
        self.assertIn("single-face-io-thread", source)
        self.assertIn('"deadline"', source)
        self.assertIn('"state-update"', source)
        self.assertIn('"delivery"', source)
        self.assertIn("StageId::APP_PAYLOAD_CHECK", source)
        self.assertIn("StageId::APP_STATE_UPDATE", source)
        self.assertIn("StageId::APP_DELIVERY", source)
        self.assertLess(source.index("profile::Profiler::get().flush();"),
                        source.index("flushEvents();"))

        foundation = json.loads(
            (REPO / "build/spec133/subject-foundation.json").read_text(encoding="utf-8")
        )
        subjects = (
            ("clean", Path(foundation["cleanWorktree"]), 0),
            ("profile", self.profile_root, 1),
        )
        pkg = shlex.split(
            subprocess.run(
                ["pkg-config", "--cflags", "--libs", "libndn-cxx"],
                check=True, text=True, stdout=subprocess.PIPE,
            ).stdout
        )
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            outputs = {}
            for label, subject_root, profiled in subjects:
                binary = root / f"spec133-{label}-self-test"
                build_dir = subject_root / "build"
                command = [
                    "g++", "-std=c++17", "-O2", "-pthread",
                    f"-DSPEC133_PROFILED={profiled}",
                    "-I", str(subject_root), "-I", str(build_dir),
                    str(driver), "-L", str(build_dir), "-lndn-svs",
                    f"-Wl,-rpath,{build_dir}", *pkg, "-o", str(binary),
                ]
                compiled = subprocess.run(command, check=False, text=True,
                                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                self.assertEqual(compiled.returncode, 0, compiled.stdout)
                env = dict(os.environ)
                if profiled:
                    env.update({
                        "NDN_LOG": "ndn_svs.Profile=TRACE",
                        "NDN_SVS_PROFILE_ENABLED": "1",
                        "NDN_SVS_PROFILE_CELL_ID": "t006-self-test",
                        "NDN_SVS_PROFILE_PEER_ID": "local",
                        "NDN_SVS_PROFILE_SAMPLE_MODULUS": "1",
                    })
                result = subprocess.run([str(binary), "--self-test"], check=False,
                                        text=True, env=env, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT)
                self.assertEqual(result.returncode, 0, result.stdout)
                self.assertIn("SPEC133_SELF_TEST_OK", result.stdout)
                self.assertIn("piggyback=1 fallback=1", result.stdout)
                outputs[label] = result.stdout

            profiled_output = outputs["profile"]
            for stage in (
                "MAP.NETWORK_WAIT", "PAYLOAD.NETWORK_WAIT",
                "MAP.PIGGY_CALLBACK", "PAYLOAD.SUBSCRIPTION_CALLBACK",
                "APP.PAYLOAD_CHECK", "APP.STATE_UPDATE", "APP.DELIVERY",
            ):
                self.assertRegex(profiled_output,
                                 rf"event=stage-summary .*stage={re.escape(stage)} .*calls=[1-9]")

    def test_t006_runner_reuses_one_face_and_verifies_both_fib_routes(self):
        runner = (
            REPO / "Experiments/NDN_SVS_Sync_Stage_Profile_Minindn.py"
        ).read_text(encoding="utf-8")
        self.assertIn("install_verified_routes", runner)
        self.assertIn("nfdc face create", runner)
        self.assertIn("nfdc route add", runner)
        self.assertIn("nfdc route list", runner)
        self.assertIn("faceId", runner)
        self.assertIn("--io-cpu", runner)
        self.assertNotIn("--main-cpu", runner)
        self.assertNotIn("--face-cpu", runner)


class Spec133RunnerContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.runner = load_runner()

    def make_subject(self, root: Path) -> Path:
        record = {
            "schemaVersion": "spec133-subject-manifest-io-v2",
            "executionModel": "single-face-io-thread",
            "publishApi": "publish",
            "parallelWorkers": None,
            "compressionEnabled": False,
            "profileConfig": {
                "sampleModulus": 100,
                "stageCount": 81,
                "logger": "ndn_svs.Profile=TRACE",
            },
        }
        for key in ("cleanBinary", "cleanLibrary", "profiledBinary", "profiledLibrary"):
            artifact = root / key
            artifact.write_bytes(key.encode("ascii"))
            record[key] = str(artifact)
            record[f"{key}Sha256"] = self.runner.sha256_file(artifact)
        path = root / "subject-manifest.json"
        path.write_text(json.dumps(record), encoding="utf-8")
        return path

    def make_overhead(self, root: Path, subject: Path, verdict: str = "ADMITTED") -> Path:
        path = root / "overhead-receipt.json"
        path.write_text(json.dumps({
            "schemaVersion": "spec133-overhead-admission-v1",
            "verdict": verdict,
            "subjectManifestSha256": self.runner.sha256_file(subject),
        }), encoding="utf-8")
        return path

    def test_t007_manifest_is_exactly_five_ascending_once_only_cells(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            subject = self.make_subject(root)
            overhead = self.make_overhead(root, subject)
            manifest = self.runner.make_manifest("fixture", subject, overhead)
            self.assertEqual(len(manifest["cells"]), 5)
            self.assertEqual([cell["ratePpsPerPeer"] for cell in manifest["cells"]],
                             [200, 400, 600, 800, 1000])
            self.assertEqual([cell["ordinal"] for cell in manifest["cells"]],
                             [1, 2, 3, 4, 5])
            self.assertEqual([cell["attempt"] for cell in manifest["cells"]], [1] * 5)
            self.assertEqual(len({cell["cellId"] for cell in manifest["cells"]}), 5)
            self.assertFalse(manifest["automaticRetry"])
            for cell in manifest["cells"]:
                self.assertEqual(cell["peers"], ["peer-a", "peer-b"])
                self.assertEqual(cell["profileMode"], "enabled")
                self.assertEqual(cell["warmupSeconds"], 10)
                self.assertEqual(cell["measureSeconds"], 60)
                self.assertEqual(cell["drainSeconds"], 10)

            manifest["cells"][4]["ratePpsPerPeer"] = 900
            with self.assertRaisesRegex(RuntimeError, "rates or order"):
                self.runner.validate_manifest(manifest)

    def test_t007_planning_rejects_failed_or_foreign_overhead_receipt(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            subject = self.make_subject(root)
            rejected = self.make_overhead(root, subject, "REJECTED")
            with self.assertRaisesRegex(RuntimeError, "admitted overhead"):
                self.runner.make_manifest("fixture", subject, rejected)
            rejected.write_text(json.dumps({
                "schemaVersion": "spec133-overhead-admission-v1",
                "verdict": "ADMITTED", "subjectManifestSha256": "0" * 64,
            }), encoding="utf-8")
            with self.assertRaisesRegex(RuntimeError, "another subject"):
                self.runner.make_manifest("fixture", subject, rejected)

    def test_t007_overhead_gate_reports_all_three_comparisons_and_thresholds(self):
        base = {"attemptedPpsPerPeer": 1000.0, "deliveryRatio": 0.99,
                "aggregateCpuPercent": 100.0, "profileComplete": True, "invalid": 0,
                "deliveredAllPhases": 1}
        good = {
            "A-clean-control": dict(base),
            "B-profiled-disabled": dict(base, attemptedPpsPerPeer=980.0,
                                         aggregateCpuPercent=103.0),
            "C-profiled-enabled": dict(base, attemptedPpsPerPeer=970.0,
                                        deliveryRatio=0.97, aggregateCpuPercent=104.0),
        }
        result = self.runner.compare_overhead(good)
        self.assertTrue(result["admitted"])
        self.assertEqual(set(result["comparisons"]), {"A-vs-B", "B-vs-C", "A-vs-C"})

        bad = {key: dict(value) for key, value in good.items()}
        bad["C-profiled-enabled"]["attemptedPpsPerPeer"] = 900.0
        self.assertFalse(self.runner.compare_overhead(bad)["admitted"])
        bad = {key: dict(value) for key, value in good.items()}
        bad["C-profiled-enabled"]["profileComplete"] = False
        self.assertFalse(self.runner.compare_overhead(bad)["admitted"])

    def test_t007_profile_environment_binds_cell_peer_and_formal_logger(self):
        subject = {"profileConfig": {"sampleModulus": 100,
                                     "logger": "ndn_svs.Profile=TRACE"}}
        enabled = self.runner.profile_environment("enabled", "cell", "peer-a", subject)
        self.assertEqual(enabled["NDN_SVS_PROFILE_ENABLED"], "1")
        self.assertEqual(enabled["NDN_SVS_PROFILE_CELL_ID"], "cell")
        self.assertEqual(enabled["NDN_SVS_PROFILE_PEER_ID"], "peer-a")
        self.assertEqual(enabled["NDN_SVS_PROFILE_SAMPLE_MODULUS"], "100")
        self.assertEqual(enabled["NDN_LOG"], "ndn_svs.Profile=TRACE")
        disabled = self.runner.profile_environment("disabled", "cell", "peer-b", subject)
        self.assertEqual(disabled["NDN_SVS_PROFILE_ENABLED"], "0")
        self.assertNotIn("NDN_LOG", disabled)

    def test_t007_source_has_single_writer_seal_and_terminal_receipts(self):
        source = RUNNER_PATH.read_text(encoding="utf-8")
        self.assertIn("fcntl.LOCK_EX | fcntl.LOCK_NB", source)
        self.assertIn("formal cell already attempted", source)
        self.assertIn("spec133-terminal-receipt-v1", source)
        self.assertIn("INFRA_INVALID", source)
        self.assertIn("SUBJECT_FAILURE", source)
        self.assertIn("spec133-campaign-terminal-v1", source)
        self.assertIn('"automaticRetry": False', source)


class Spec133AnalyzerContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.analyzer = load_analyzer()

    def test_t010_nearest_rank_and_interval_union_are_exact(self):
        self.assertEqual(self.analyzer.nearest_rank([40, 10, 30, 20], .50), 20)
        self.assertEqual(self.analyzer.nearest_rank([40, 10, 30, 20], .95), 40)
        self.assertEqual(self.analyzer.interval_union_ns([(10, 30), (20, 40), (50, 60)]), 40)
        with self.assertRaisesRegex(RuntimeError, "invalid interval"):
            self.analyzer.interval_union_ns([(9, 8)])

    def test_t010_profile_parser_fails_on_malformed_or_negative_fields(self):
        record = self.analyzer.parse_profile_record(
            "prefix schema=spec133-stage-span-v1 event=stage-span durationNs=12")
        self.assertEqual(record["durationNs"], 12)
        with self.assertRaisesRegex(RuntimeError, "invalid nonnegative integer"):
            self.analyzer.parse_profile_record(
                "schema=spec133-stage-span-v1 event=stage-span durationNs=-1")
        with self.assertRaisesRegex(RuntimeError, "duplicate profile field"):
            self.analyzer.parse_profile_record(
                "schema=spec133-stage-span-v1 event=stage-span stage=A stage=B")

    def test_t010_registry_and_required_output_contract_are_frozen(self):
        registry = self.analyzer.parse_registry(
            REPO / "build/spec133/worktrees/sync-stage-profile/ndn-svs/profile.hpp")
        self.assertEqual(len(registry), 81)
        self.assertEqual(registry["PUB.INNER_SIGN"]["kind"], "leaf-cpu")
        self.assertEqual(registry["MAP.NETWORK_WAIT"]["kind"], "external-wait")
        self.assertEqual(set(self.analyzer.OUTPUTS), {
            "campaign-summary.json", "cell-summary.csv", "rate-stage-summary.csv",
            "critical-path-groups.csv", "path-frequency.csv", "bottleneck-ranking.csv",
            "bottleneck-report.md", "limitations.md",
        })

    def test_t010_two_signal_rule_and_wait_separation(self):
        cells = []
        stages = []
        for rate in (200, 400, 600, 800, 1000):
            cells.append({"ratePpsPerPeer": rate, "valid": True,
                          "attemptedPpsPerPeer": rate if rate < 1000 else 900,
                          "deliveryRatio": 1.0 if rate < 1000 else .90})
            stages.append({"ratePpsPerPeer": rate, "peerId": "peer-a",
                           "stageId": "PUB.INNER_SIGN", "threadRole": "app-main",
                           "kind": "leaf-cpu", "calls": rate, "totalDurationNs": rate * 100,
                           "p95Ns": 100 if rate == 200 else 200,
                           "threadCpuShare": .60})
            stages.append({"ratePpsPerPeer": rate, "peerId": "peer-a",
                           "stageId": "MAP.NETWORK_WAIT", "threadRole": "external",
                           "kind": "external-wait", "calls": rate, "totalDurationNs": rate * 1000,
                           "p95Ns": 1000, "threadCpuShare": None})
        findings = self.analyzer.rank_bottlenecks(stages, cells)
        signing = next(row for row in findings if row["stageId"] == "PUB.INNER_SIGN")
        self.assertEqual(signing["verdict"], "supported")
        external = next(row for row in findings if row["stageId"] == "MAP.NETWORK_WAIT")
        self.assertNotEqual(external["group"], "publisher-main")

    def test_t010_uncontained_sample_is_flagged_and_excluded_from_residuals(self):
        registry = {
            "PUB.TOTAL": {"kind": "aggregate", "thread": "app-main", "parent": ""},
            "PUB.CHILD": {
                "kind": "leaf-cpu", "thread": "app-main", "parent": "PUB.TOTAL"
            },
        }
        spans = [
            {"trace": "7", "stage": "PUB.TOTAL", "kind": "aggregate",
             "startRawNs": 100, "durationNs": 20},
            {"trace": "7", "stage": "PUB.CHILD", "kind": "leaf-cpu",
             "startRawNs": 200, "durationNs": 5},
        ]
        self.analyzer.validate_containment(spans, registry, "cell", "peer-a")
        self.assertFalse(spans[1]["containmentValid"])
        self.assertEqual(
            self.analyzer.aggregate_residuals(spans, registry)["PUB.TOTAL"], [20]
        )


if __name__ == "__main__":
    unittest.main()
