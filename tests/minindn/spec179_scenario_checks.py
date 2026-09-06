#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Spec179 MiniNDN per-scenario evidence checks (T011d).

This module turns a completed run output directory into scenario-specific,
time-attributed assertions.  It is the *second* half of the T011d release
gate: the launcher produces the packet campaign, and this module verifies
that the revocation/authorization event was actually discovered inside the
window and actually denied traffic afterwards ("applied-and-observed"), never
merely that a --revoke-* flag was passed to the controller.

Evidence model (all timestamps are seconds since the run's own epoch):

* version events -- controller-*.log lines
    "NDNSF_CONTROLLER_REVOKED kind=K identity=.. service=.. generation=G epoch=E"
  (NDN_LOG WARN, carries a wall-clock prefix) and stdout-only
  "NDNSF_GRANT_ONLY_APPLIED success=1 ... epoch=E" (no prefix).
* role discovery -- each role log line
    "Installed PolicyStatus service=.. generation=G epoch=E"
* role traffic -- request-results.csv (one terminal row per request) joined
  with request_lifecycle.csv (per-state rows) on request_id; enqueue and
  completion come from lifecycle microsecond fields.  Requests denied
  locally (a revoked user's RequestService returns an empty request id, so
  the App never writes a row) leave their evidence as per-request reject log
  lines, counted by denial_log_counts() with the USER_DENIAL_PATTERNS set.
* role publications -- "NDNSF_PUBLICATION_AUDIT role=.. type=.. validated=.."
* scheduled revalidation -- "NDNSF_POLICY_STATUS_REVALIDATION" fire markers
  (NDN_LOG_INFO, wall-clock prefix).

Every evaluator returns {"checks": {name: bool}, "details": {name: str},
"evidence": {...}} and never modifies the run directory.  Checks are
intentionally asymmetric: they require the *negative* half (post-discovery
denials, revoked provider stops serving) and the *control* half (unaffected
identities keep succeeding) to both be observable.
"""

from __future__ import annotations

import argparse
import csv
import json
import re
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

# Log line: "<float>  LEVEL: [logger] message".  stdout lines without a
# prefix are ignored by every time-attributed parser.
_TS_LINE = re.compile(
    r"^(\d+\.\d+)\s+(?:TRACE|DEBUG|INFO|WARN|ERROR|FATAL):\s*\[[^\]]+\]\s+(.*)$")

_INSTALL = re.compile(r"Installed PolicyStatus service=(\S+) generation=(\d+) epoch=(\d+)")
_REVOKED = re.compile(
    r"NDNSF_CONTROLLER_REVOKED kind=(\d+) identity=(\S+) service=(\S+)"
    r" generation=(\d+) epoch=(\d+)")
_GRANTED = re.compile(r"NDNSF_GRANT_ONLY_APPLIED success=(\d+) identity=(\S+)"
                      r" service=(\S+) generation=(\d+) epoch=(\d+)")
_AUDIT = re.compile(r"NDNSF_PUBLICATION_AUDIT role=(\S+) type=(\S+) validated=(true|false)")
_INVALIDATED = re.compile(r"NDNSF_CONTROLLER_CACHE_INVALIDATED role=(\S+)"
                          r" serviceName=(\S+) generation=(\d+) epoch=(\d+)")
# Provider "serving" evidence (ServiceProvider.cpp): a request is served only
# when the provider publishes its ACK/RESPONSE, which logs exactly one
# DEBUG "[NDNSF_HYBRID] role=provider event=HYBRID_PUBLISH" line per
# publication.  NDNSF_PUBLICATION_AUDIT rows are *inbound observations*
# (REQUEST/SELECTION arrivals, one per SVS delivery, unconditional before the
# admission gate) and must never be used as a serving signal.
_PROV_SERVING = re.compile(
    r"event=HYBRID_PUBLISH messageName=\S+ messageType=(ACK|RESPONSE)(?:\s|$)")

TIMEOUT_REASONS = {"", "request_timeout", "timeout", "inflight_timeout"}

# Local admission denials never reach the network, so they never appear as
# request-results.csv rows: a revoked user's RequestService returns an empty
# request id and the App drops the tick without writing a row.  The denial
# evidence therefore lives in the role log -- every reject path below logs
# one ERROR/WARN line per denied request (ServiceUser.cpp).
USER_DENIAL_PATTERNS = (
    "Reject request without user permission",
    "Reject request under revoked Controller status",
    "NDNSF_USER_REVOCATION_REJECT",
    "Reject request without installed ControllerVersion",
    "Reject request with stale ControllerVersion",
    "Reject targeted request under revoked Controller status",
    "Reject targeted request without user permission",
)


def denial_log_counts(logs: List[Path], lo: float = 0.0,
                      hi: float = float("inf")) -> Counter:
    """Per-pattern counts of user-side denial log lines in [lo, hi]."""
    counts: Counter = Counter()
    for path in logs:
        for ts, msg in _ts_lines(path):
            if lo <= ts <= hi:
                for pattern in USER_DENIAL_PATTERNS:
                    if pattern in msg:
                        counts[pattern] += 1
                        break
    return counts


# Provider-side enforcement evidence (ServiceProvider.cpp): a provider whose
# grant was withdrawn refuses each arriving request at the OnRequest
# admission gate and logs exactly one ERROR "Not serving: <service>" per
# refusal (co-located INFO "OnRequest missing permission"); refused requests
# never produce a serving publication.  "Stops serving" therefore reads as:
# refusals present AND serving publications absent after the enforcement
# window opens.  The matrix's provider-affected scenarios revoke the only
# service the affected provider serves.
PROV_DENIAL_PATTERN = "Not serving:"
# Seconds after the discovery install before enforcement is demanded: the
# permission renewal triggered by the install needs a small window to land,
# and requests admitted before it are pre-revocation material still draining.
PROVIDER_ENFORCEMENT_GRACE_S = 1.0

# Run-window truncation guard (row-based success checks only).  The launcher
# tears the provider processes down while user request streams still run, so
# the last rows of a stream can fail solely because every provider already
# exited -- that is a run-window artifact, not denial evidence.  A failed row
# is treated as such an artifact when it was enqueued so late that it could
# not complete (the request timeout is 5.5 s) before the earliest provider
# stopped logging.
REQUEST_TIMEOUT_BUDGET_S = 6.0


def prov_serving_log_counts(logs: List[Path], lo: float = 0.0,
                            hi: float = float("inf")) -> int:
    """Count provider ACK/RESPONSE serving publications in [lo, hi]."""
    n = 0
    for path in logs:
        for ts, msg in _ts_lines(path):
            if lo <= ts <= hi and _PROV_SERVING.search(msg):
                n += 1
    return n


def prov_denial_log_counts(logs: List[Path], lo: float = 0.0,
                           hi: float = float("inf")) -> int:
    """Count provider "Not serving:" refusal lines in [lo, hi]."""
    n = 0
    for path in logs:
        for ts, msg in _ts_lines(path):
            if lo <= ts <= hi and PROV_DENIAL_PATTERN in msg:
                n += 1
    return n


def _ts_lines(path: Path) -> List[Tuple[float, str]]:
    """Return [(ts_s, message)] for every prefixed log line in *path*."""
    out: List[Tuple[float, str]] = []
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except FileNotFoundError:
        return out
    for line in text.splitlines():
        m = _TS_LINE.match(line)
        if m:
            out.append((float(m.group(1)), m.group(2)))
    return out


def _count(path: Path, substr: str) -> int:
    try:
        return path.read_text(encoding="utf-8", errors="replace").count(substr)
    except FileNotFoundError:
        return 0


def _count_after(path: Path, substr: str, after_ts: float) -> int:
    return sum(1 for ts, msg in _ts_lines(path)
               if ts >= after_ts and substr in msg)


def install_events(logs: List[Path]) -> List[Tuple[float, int, int]]:
    """[(ts, generation, epoch)] merged chronologically across *logs*."""
    rows = []
    for path in logs:
        for ts, msg in _ts_lines(path):
            m = _INSTALL.search(msg)
            if m:
                rows.append((ts, int(m.group(2)), int(m.group(3))))
    rows.sort(key=lambda r: r[0])
    return rows


def revoke_events(output: Path) -> List[Dict[str, Any]]:
    rows = []
    for path in sorted(output.glob("controller-*.log")):
        for ts, msg in _ts_lines(path):
            m = _REVOKED.search(msg)
            if m:
                rows.append({
                    "ts": ts, "kind": int(m.group(1)),
                    "identity": m.group(2), "service": m.group(3),
                    "generation": int(m.group(4)), "epoch": int(m.group(5)),
                    "log": path.name,
                })
    rows.sort(key=lambda r: r["ts"])
    return rows


def grant_events(output: Path) -> List[Dict[str, Any]]:
    rows = []
    for path in sorted(output.glob("controller-*.log")):
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            m = _GRANTED.search(line)
            if m:
                rows.append({
                    "success": int(m.group(1)), "identity": m.group(2),
                    "service": m.group(3), "generation": int(m.group(4)),
                    "epoch": int(m.group(5)), "log": path.name,
                })
    return rows


def audit_events(logs: List[Path]) -> List[Tuple[float, str, str, bool]]:
    rows = []
    for path in logs:
        for ts, msg in _ts_lines(path):
            m = _AUDIT.search(msg)
            if m:
                rows.append((ts, m.group(1), m.group(2), m.group(3) == "true"))
    rows.sort(key=lambda r: r[0])
    return rows


def last_log_ts(path: Path) -> Optional[float]:
    rows = _ts_lines(path)
    return rows[-1][0] if rows else None


def freeze_window(logs: List[Path],
                  min_gap_s: float = 2.0) -> Optional[Tuple[float, float]]:
    """(start, end) of the longest >= *min_gap_s* log gap across *logs*.

    A SIGSTOPped process writes nothing, so S5's freeze shows up as the
    run's one multi-second gap in user-A's log.  Measuring the freeze from
    the log instead of trusting the launcher's stopUserAMs keeps the
    pre-stop/post-resume windows aligned with what actually happened.
    """
    ts = sorted(t for path in logs for t, _m in _ts_lines(path))
    best: Optional[Tuple[float, float]] = None
    for a, b in zip(ts, ts[1:]):
        if b - a >= min_gap_s and (best is None or b - a > best[1] - best[0]):
            best = (a, b)
    return best


def request_rows(output: Path, user_dir: str) -> Dict[str, Dict[str, Any]]:
    """Join request-results.csv with the last lifecycle row per request_id."""
    base = output / user_dir
    results: Dict[str, int] = {}
    res_path = base / "request-results.csv"
    if res_path.is_file():
        for row in csv.DictReader(res_path.open(encoding="utf-8")):
            try:
                results[row["request_id"]] = int(row["success"])
            except (KeyError, ValueError):
                continue
    final: Dict[str, Dict[str, Any]] = {}
    lc_path = base / "request_lifecycle.csv"
    if lc_path.is_file():
        for row in csv.DictReader(lc_path.open(encoding="utf-8")):
            rid = row.get("request_id")
            if not rid:
                continue
            final[rid] = row
    rows: Dict[str, Dict[str, Any]] = {}
    for rid in sorted(set(results) | set(final)):
        lc = final.get(rid, {})
        try:
            enqueue_us = int(lc.get("enqueue_timestamp_us") or 0)
            latency_ms = float(lc.get("end_to_end_latency_ms") or 0.0)
        except ValueError:
            enqueue_us, latency_ms = 0, 0.0
        rows[rid] = {
            "success": bool(results.get(rid, 0)),
            "terminal": bool(lc.get("state")),
            "enqueue_s": enqueue_us / 1e6,
            "completion_s": enqueue_us / 1e6 + latency_ms / 1e3,
            "selected_provider": lc.get("selected_provider") or "",
            "reason": lc.get("final_cleanup_reason") or "",
        }
    return rows


def row_success_counts(rows: Dict[str, Dict[str, Any]],
                       lo: float, hi: float) -> Tuple[int, int]:
    ok = fail = 0
    for r in rows.values():
        if lo <= r["enqueue_s"] <= hi:
            if r["success"]:
                ok += 1
            else:
                fail += 1
    return ok, fail


def failure_reasons(rows: Dict[str, Dict[str, Any]],
                    lo: float, hi: float) -> Counter:
    return Counter(r["reason"] for r in rows.values()
                   if lo <= r["enqueue_s"] <= hi and not r["success"])


# --------------------------------------------------------------------------
# Shared scenario shapes
# --------------------------------------------------------------------------

ROLE_KEY = {"userA": "user-A", "userB": "user-B",
            "providerA": "provider-A", "providerB": "provider-B"}


def _role_logs(output: Path, role_key: str) -> List[Path]:
    stem = ROLE_KEY[role_key]
    logs = [output / (stem + ".log")]
    for extra in sorted(output.glob(stem + "-*.log")):
        logs.append(extra)
    return logs


class RunCtx:
    """Everything an evaluator needs, parsed once per scenario."""

    def __init__(self, output: Path, config: Dict[str, Any]):
        self.output = output
        self.config = config or {}
        self.revokes = revoke_events(output)
        self.grants = grant_events(output)
        self.rev = self.revokes[-1] if self.revokes else None
        self.epoch2 = self.rev["epoch"] if self.rev else None
        self.gen = self.rev["generation"] if self.rev else None
        self.installs: Dict[str, List[Tuple[float, int, int]]] = {}
        self.audits: Dict[str, List[Tuple[float, str, str, bool]]] = {}
        self.rows: Dict[str, Dict[str, Dict[str, Any]]] = {}
        self.checks: Dict[str, bool] = {}
        self.details: Dict[str, str] = {}
        self.evidence: Dict[str, Any] = {}
        for key in ROLE_KEY:
            logs = _role_logs(output, key)
            self.installs[key] = install_events(logs)
            self.audits[key] = audit_events(logs)
            self.rows[key] = request_rows(output, ROLE_KEY[key])
        # Earliest instant any provider was still logging.  Row-based success
        # checks must not count failures enqueued after this (they could not
        # complete before the provider processes were torn down).
        provider_ends = [t for key in ("providerA", "providerB")
                         for p in _role_logs(output, key)
                         for t in (last_log_ts(p),) if t is not None]
        self.providers_deadline = min(provider_ends) if provider_ends else None

    # -- small helpers ------------------------------------------------
    def install_ts(self, role_key: str, epoch: int) -> Optional[float]:
        for ts, _g, e in self.installs[role_key]:
            if e == epoch:
                return ts
        return None

    def check(self, name: str, ok: bool, detail: str) -> None:
        self.checks[name] = bool(ok)
        self.details[name] = detail

    def knob(self, role_key: str) -> int:
        return int((self.config.get("knobMs") or {}).get(role_key, 0))

    def role_knobbed(self, role_key: str) -> bool:
        return self.knob(role_key) > 0

    def report_denials(self, role_key: str, lo: float, hi: float,
                       name: str, min_rows: int, expect_denied: bool,
                       deadline: Optional[float] = None) -> None:
        """Row-based traffic check for *role_key* in [lo, hi] (enqueue_s).

        *deadline* (optional) overrides the run-window truncation deadline:
        a failed row is a run artifact, not denial evidence, when it was
        enqueued so late that it could not complete (REQUEST_TIMEOUT_BUDGET_S)
        before *deadline*.  The default deadline is the earliest provider
        stop; S5 passes the measured SIGSTOP instant instead, because rows
        enqueued just before the freeze can only fail after SIGCONT.
        """
        rows = self.rows[role_key]
        ok, fail = row_success_counts(rows, lo, hi)
        reasons = failure_reasons(rows, lo, hi)
        if expect_denied:
            nontimeout = sum(n for reason, n in reasons.items()
                             if reason not in TIMEOUT_REASONS)
            passed = fail >= min_rows and nontimeout >= 1
            detail = ("rows=[%s] success=%d denied=%d nonTimeoutDenied=%d"
                      " reasons=%s" %
                      (ROLE_KEY[role_key], ok, fail, nontimeout,
                       dict(reasons) or "-"))
        else:
            truncated = 0
            if deadline is None:
                deadline = self.providers_deadline
            if deadline is not None:
                kept_fail = 0
                for r in rows.values():
                    if lo <= r["enqueue_s"] <= hi and not r["success"]:
                        if (r["enqueue_s"] + REQUEST_TIMEOUT_BUDGET_S
                                > deadline):
                            # Enqueued so late that no provider could have
                            # completed it before *deadline* (provider
                            # teardown, or S5's SIGSTOP): a run-window
                            # artifact, not a denial.
                            truncated += 1
                        else:
                            kept_fail += 1
                fail = kept_fail
            passed = ok >= min_rows and fail == 0
            detail = ("rows=[%s] success=%d denied=%d%s reasons=%s" %
                      (ROLE_KEY[role_key], ok, fail,
                       " truncated=%d" % truncated if truncated else "",
                       dict(reasons) or "-"))
        self.check(name, passed, detail)

    def report_log_denials(self, role_key: str, lo: float, hi: float,
                           name: str, min_denials: int) -> None:
        """Count reject log lines (local denials write no CSV rows)."""
        counts = denial_log_counts(_role_logs(self.output, role_key), lo, hi)
        n = sum(counts.values())
        self.check(name, n >= min_denials,
                   "logs=[%s] denials=%d patterns=%s" %
                   (ROLE_KEY[role_key], n, dict(counts) or "-"))

    def require_revalidation(self, role_keys, min_markers: int = 1,
                             after_ts: Optional[float] = None) -> None:
        for key in role_keys:
            if not self.role_knobbed(key):
                continue
            logs = _role_logs(self.output, key)
            if after_ts is not None:
                n = sum(_count_after(log, "NDNSF_POLICY_STATUS_REVALIDATION", after_ts)
                        for log in logs)
            else:
                n = sum(_count(log, "NDNSF_POLICY_STATUS_REVALIDATION") for log in logs)
            self.check("revalidation_%s" % key, n >= min_markers,
                       "markers=%d afterTs=%s" %
                       (n, "yes" if after_ts is not None else "any"))


# --------------------------------------------------------------------------
# Scenario evaluators
# --------------------------------------------------------------------------

def _common_revocation(ctx: RunCtx, affected_key: str,
                       revoke_kind: Optional[int] = None) -> bool:
    """Discovery + denial + control half shared by the revocation family.

    *affected_key* is the role whose authority is withdrawn (its traffic must
    stop once it has discovered the new epoch); every other identity is an
    unaffected control that must keep succeeding.
    """
    rev = ctx.rev
    if rev is None:
        ctx.check("revoke_observed", False, "no NDNSF_CONTROLLER_REVOKED line")
        return False
    if revoke_kind is not None and rev["kind"] != revoke_kind:
        ctx.check("revoke_kind", False, "kind=%d expected=%d" %
                  (rev["kind"], revoke_kind))
    ctx.check("revoke_observed", True,
              "kind=%d identity=%s service=%s epoch=%d ts=%.3f (%s)" %
              (rev["kind"], rev["identity"], rev["service"], rev["epoch"],
               rev["ts"], rev["log"]))
    rev_ts = rev["ts"]

    # Discovery: the affected role must install the new epoch on its own
    # scheduled refresh inside the run window (never discovered == the flag
    # was "applied-but-dormant", which is not evidence).
    inst_ts = ctx.install_ts(affected_key, ctx.epoch2)
    knob = ctx.knob(affected_key)
    discovery_cap = 3.0 + (knob / 1000.0) * 3.0
    ctx.check(
        "discovery_by_%s" % affected_key, inst_ts is not None,
        "epoch%d install=%s" % (ctx.epoch2,
                                ("%.3f" % inst_ts) if inst_ts else "MISSING"))
    if inst_ts is not None:
        ctx.check(
            "discovery_after_revoke", inst_ts >= rev_ts - 0.5,
            "install=%.3f revoke=%.3f" % (inst_ts, rev_ts))
        if knob > 0:
            ctx.check(
                "discovery_within_grace", inst_ts <= rev_ts + discovery_cap,
                "install=%.3f revoke=%.3f knobMs=%d cap=%.1fs" %
                (inst_ts, rev_ts, knob, discovery_cap))
        if affected_key in ("userA", "userB"):
            # Pre-revocation functioning of the affected role: at least one
            # request enqueued a full second before the revocation must have
            # succeeded (the authority worked before the negative half).
            pre_hi = rev_ts - 1.0
            if (ctx.config.get("recovery") == "controller-restart"
                    and rev is not None):
                # The first controller exited before the successor installed
                # the revocation; requests during the handover fail closed by
                # design (authority state was lost with the old controller).
                # Pre-revocation successes are therefore measured only over
                # the first controller's tenure, not across the gap.
                ctrl_logs = sorted(ctx.output.glob("controller-*.log"))
                if ctrl_logs and rev["log"] != ctrl_logs[0].name:
                    prior_death = last_log_ts(ctrl_logs[0])
                    if prior_death is not None and prior_death < rev_ts:
                        pre_hi = min(pre_hi, prior_death - 0.5)
            ctx.report_denials(
                affected_key, 0.0, pre_hi, "affected_pre_revoke_success",
                1, expect_denied=False)
            # Post-discovery: a revoked user's RequestService returns an
            # empty request id (no CSV row), so the denial evidence is the
            # per-request reject log lines after its own epoch-2 install.
            post_lo = inst_ts + 0.2
            ctx.report_log_denials(affected_key, post_lo, float("inf"),
                                   "affected_post_discovery_denied", 2)
            ok, fail = row_success_counts(ctx.rows[affected_key],
                                          post_lo, float("inf"))
            ctx.check("affected_no_success_after_discovery", ok == 0,
                      "rows success=%d deniedRows=%d (denials are log lines)" %
                      (ok, fail))
            ctx.evidence["affectedInstallTs"] = inst_ts
        else:
            # Provider-affected (S2/S3/S5): the affected role's own audit
            # rows (validated publications) are the denial surface, asserted
            # by each scenario evaluator beside this shared discovery half;
            # the row-based halves do not apply (no request CSV on roles).
            ctx.evidence["affectedInstallTs"] = inst_ts
    else:
        # Keep the remaining checks evaluated so the report is complete.
        if affected_key in ("userA", "userB"):
            ctx.check("affected_pre_revoke_success", False,
                      "no discovery; skipped")
            ctx.check("affected_post_discovery_denied", False,
                      "no discovery; skipped")

    # Control half: every unaffected identity keeps succeeding after the
    # revocation propagated (rotation must not break live authority).
    for key in ROLE_KEY:
        if key == affected_key or not ctx.rows[key]:
            continue
        # Allow 2.5 s for the unaffected role's own refresh to converge
        # before demanding successes (its material may still be epoch-1
        # while the providers already rotated).
        ctx.report_denials(key, rev_ts + 2.5, float("inf"),
                           "control_%s_post_revoke" % key, 2, False)

    # Each knobbed role must actually revalidate (proves the scheduled
    # refresh machinery ran, not only bootstrap fetches).
    ctx.require_revalidation([k for k in ROLE_KEY if k != affected_key or True])
    return all(v for k, v in ctx.checks.items() if not k.startswith("detail"))


def _check_identity_family(ctx: RunCtx, affected_key: str,
                           expected_kind: int) -> Dict[str, Any]:
    _common_revocation(ctx, affected_key, expected_kind)
    for key, rows in ctx.rows.items():
        ctx.evidence["rows_%s" % key] = {
            "success": sum(1 for r in rows.values() if r["success"]),
            "denied": sum(1 for r in rows.values() if not r["success"]),
            "reasons": dict(Counter(r["reason"] for r in rows.values()
                                    if not r["success"])),
        }
    ctx.evidence["installEpochs"] = {
        key: sorted({e for _t, _g, e in installs})
        for key, installs in ctx.installs.items()}
    ctx.evidence["revoke"] = ctx.rev
    ctx.evidence["grantCount"] = len(ctx.grants)
    return {"passed": all(ctx.checks.values()), "checks": ctx.checks,
            "details": ctx.details, "evidence": ctx.evidence}


def check_user_identity_revocation(ctx: RunCtx) -> Dict[str, Any]:
    """S1: revoke user/A's identity; user/A must stop, user/B continues."""
    return _check_identity_family(ctx, "userA", 1)


def check_provider_identity_revocation(ctx: RunCtx) -> Dict[str, Any]:
    """S2: revoke provider/A's identity (with a mid-run process restart).

    A1 must stop serving after discovering epoch 2 (refusals present, zero
    ACK/RESPONSE serving publications).  The restarted A2 process (same PIB,
    fresh bootstrap under the *current* epoch) must not resurrect the
    authority.  ServiceProvider blocks its constructor on NAC-ABE DKEY
    readiness before any policy install, and a revoked identity's DKEY
    Interest is never satisfied, so a revoked cold start normally never
    leaves the constructor -- repeated DK_INTEREST_EXPRESSED attempts, no
    DK_DECRYPT_SUCCESS/constructor_done, no epoch install, no serving.  A
    bootstrap that does complete (policy-before-DKEY ordering) must instead
    install only the current epoch and refuse every arriving request.  Both
    fail-closed shapes must end with zero ACK/RESPONSE publications.
    """
    _common_revocation(ctx, "providerA", 1)
    all_logs = _role_logs(ctx.output, "providerA")
    phase1 = [p for p in all_logs if not p.name.endswith("-2.log")]
    phase2 = [p for p in all_logs if p.name.endswith("-2.log")]
    inst_ts = ctx.install_ts("providerA", ctx.epoch2)
    if inst_ts is None:
        ctx.check("prov_a_stops_serving", False, "no epoch2 discovery")
        ctx.check("prov_a_served_before", False, "no epoch2 discovery")
        ctx.check("restart_second_bootstrap", False, "no epoch2 discovery")
        ctx.check("restart_never_serves", False, "no epoch2 discovery")
    else:
        served_before = prov_serving_log_counts(phase1, 0.0, inst_ts)
        ctx.check("prov_a_served_before", served_before >= 1,
                  "serving publications before epoch2 install=%d" %
                  served_before)
        lo = inst_ts + PROVIDER_ENFORCEMENT_GRACE_S
        denials = prov_denial_log_counts(phase1, lo, float("inf"))
        serving = prov_serving_log_counts(phase1, lo, float("inf"))
        ctx.check("prov_a_stops_serving",
                  denials >= 1 and serving == 0,
                  "phase1 post-discovery(ts=%.3f+%.1fs) denials=%d"
                  " servingPublications=%d" %
                  (inst_ts, PROVIDER_ENFORCEMENT_GRACE_S, denials, serving))
        # A second bootstrap: the revoked identity must not resurrect its
        # authority in either fail-closed shape the framework can produce.
        # ServiceProvider blocks its constructor on NAC-ABE DKEY readiness
        # *before* any policy install, and a revoked identity's DKEY Interest
        # is never satisfied (the authority no longer serves it), so the
        # revoked cold start normally never leaves the constructor:
        # repeated DK_INTEREST_EXPRESSED attempts, no DK_DECRYPT_SUCCESS /
        # constructor_done marker, no epoch install, no serving
        # publications.  If a future policy-before-DKEY ordering lets the
        # bootstrap complete, the fresh process must install only the
        # current epoch (an empty permission table for the revoked
        # identity) and refuse every arriving request from its first
        # subscription onward.
        second = install_events(phase2)
        dk_attempts = sum(_count(p, "DK_INTEREST_EXPRESSED") for p in phase2)
        dk_done = sum(_count(p, "DK_DECRYPT_SUCCESS") for p in phase2)
        boot_done = sum(_count(p, "stage=constructor_done") for p in phase2)
        denials2 = prov_denial_log_counts(phase2, 0.0, float("inf"))
        serving2 = prov_serving_log_counts(phase2, 0.0, float("inf"))
        if second:
            # Bootstrap completed: current epoch only, refused requests,
            # never a serving publication.
            restart_ok = (all(e == ctx.epoch2 for _t, _g, e in second) and
                          denials2 >= 1 and serving2 == 0)
        else:
            # Constructor blocked on DKEY readiness: the process must
            # demonstrably keep trying for its DKEY and never finish
            # bootstrapping, so it can never serve.
            restart_ok = (dk_attempts >= 2 and dk_done == 0 and
                          boot_done == 0 and serving2 == 0)
        ctx.check("restart_never_serves", restart_ok,
                  "phase2 installs=%s dkAttempts=%d dkDone=%d"
                  " constructorDone=%d denials=%d servingPublications=%d" %
                  ([{"ts": round(t, 3), "epoch": e} for t, _g, e in second],
                   dk_attempts, dk_done, boot_done, denials2, serving2))
        ctx.details.setdefault("restart_second_bootstrap", "")
        if second:
            ctx.details["restart_second_bootstrap"] = \
                "phase2 first install ts=%.3f epoch=%d" % (second[0][0], second[0][2])
        else:
            ctx.details["restart_second_bootstrap"] = \
                ("phase2 no policy install (constructor blocked on DKEY"
                 " readiness for the revoked identity; dkAttempts=%d)" %
                 dk_attempts)
        # A completed bootstrap must never anchor on a stale epoch: a second
        # install under epoch 1 would mean the restart collided with the
        # pre-revocation generation.  A constructor-blocked process installs
        # nothing, which trivially satisfies this.
        ctx.check("restart_second_bootstrap",
                  all(e == ctx.epoch2 for _t, _g, e in second),
                  "phase2 installs=%s (empty = constructor never completed)" %
                  [{"ts": round(t, 3), "epoch": e} for t, _g, e in second])
    ctx.evidence["installEpochs"] = {
        key: sorted({e for _t, _g, e in installs})
        for key, installs in ctx.installs.items()}
    ctx.evidence["revoke"] = ctx.rev
    return {"passed": all(ctx.checks.values()), "checks": ctx.checks,
            "details": ctx.details, "evidence": ctx.evidence}


def check_service_scoped_revocation(ctx: RunCtx) -> Dict[str, Any]:
    """S3: withdraw provider/A's /SERVICE/HELLO only.  Both users are
    unaffected controls (their own authority is untouched) and provider/A
    stops serving; kind=3 service revocation.  NDNSF_PUBLICATION_AUDIT
    REQUEST rows continue after the withdrawal (unaffected users keep
    sending and the sync group keeps delivering), so "stops serving" is
    asserted on serving publications (ACK/RESPONSE) plus per-request
    refusals, not on request arrivals."""
    _common_revocation(ctx, "providerA", 3)
    inst_ts = ctx.install_ts("providerA", ctx.epoch2)
    if inst_ts is not None:
        logs = _role_logs(ctx.output, "providerA")
        served_before = prov_serving_log_counts(logs, 0.0, inst_ts)
        lo = inst_ts + PROVIDER_ENFORCEMENT_GRACE_S
        denials = prov_denial_log_counts(logs, lo, float("inf"))
        serving = prov_serving_log_counts(logs, lo, float("inf"))
        ctx.check("service_prov_a_stops_serving",
                  served_before >= 1 and denials >= 2 and serving == 0,
                  "servedBefore=%d post-discovery(ts=%.3f+%.1fs)"
                  " denials=%d servingPublications=%d" %
                  (served_before, inst_ts, PROVIDER_ENFORCEMENT_GRACE_S,
                   denials, serving))
    else:
        ctx.check("service_prov_a_stops_serving", False, "no epoch2 discovery")
    ctx.evidence["installEpochs"] = {
        key: sorted({e for _t, _g, e in installs})
        for key, installs in ctx.installs.items()}
    ctx.evidence["revoke"] = ctx.rev
    return {"passed": all(ctx.checks.values()), "checks": ctx.checks,
            "details": ctx.details, "evidence": ctx.evidence}


def check_inflight_revocation(ctx: RunCtx) -> Dict[str, Any]:
    """S4: user/A's identity is revoked while its 3 s admission-delayed
    requests are still in flight.  Identity-family semantics, with one
    deliberate exception: pre-revoke *success* is not required, because a
    request enqueued before the revocation legitimately completes only after
    it (the provider delays the response across the boundary and user/A,
    having discovered its own revocation by then, rejects that response).
    The gate instead demands: (a) pre-revoke rows exist, (b) at least one row
    straddles the boundary, (c) post-discovery local denials, (d) unaffected
    roles keep succeeding, and (e) the discovery happened in the scheduled
    refresh window."""
    rev = ctx.rev
    if rev is None:
        ctx.check("revoke_observed", False, "no NDNSF_CONTROLLER_REVOKED line")
        return {"passed": False, "checks": ctx.checks, "details": ctx.details,
                "evidence": ctx.evidence}
    ctx.check("revoke_observed", True,
              "kind=%d epoch=%d ts=%.3f (%s)" %
              (rev["kind"], rev["epoch"], rev["ts"], rev["log"]))
    rev_ts = rev["ts"]
    inst_ts = ctx.install_ts("userA", ctx.epoch2)
    knob = ctx.knob("userA")
    ctx.check("discovery_by_userA", inst_ts is not None,
              "epoch%d install=%s" %
              (ctx.epoch2, ("%.3f" % inst_ts) if inst_ts else "MISSING"))
    if inst_ts is not None:
        ctx.check("discovery_after_revoke", inst_ts >= rev_ts - 0.5,
                  "install=%.3f revoke=%.3f" % (inst_ts, rev_ts))
        if knob > 0:
            ctx.check("discovery_within_grace",
                      inst_ts <= rev_ts + 3.0 + (knob / 1000.0) * 3.0,
                      "install=%.3f revoke=%.3f knobMs=%d" %
                      (inst_ts, rev_ts, knob))
        ctx.evidence["affectedInstallTs"] = inst_ts
        pre = [r for r in ctx.rows["userA"].values()
               if r["enqueue_s"] <= rev_ts - 1.0]
        ctx.check("affected_pre_revoke_rows", len(pre) >= 1,
                  "userA rows enqueued before revoke-1s=%d "
                  "(success not required: delayed responses complete after "
                  "the revocation)" % len(pre))
        ctx.report_log_denials("userA", inst_ts + 0.2, float("inf"),
                               "affected_post_discovery_denied", 2)
    boundary = []
    for rid, r in ctx.rows["userA"].items():
        if r["enqueue_s"] <= rev_ts - 0.5 and r["completion_s"] >= rev_ts:
            boundary.append(rid)
    ctx.check("inflight_boundary_row", len(boundary) >= 1,
              "requests straddling revocation=%d" % len(boundary))
    # Controls: every other benchmark role keeps succeeding after the
    # revocation propagated (user/B's own 3 s-delayed rows land later).
    for key in ROLE_KEY:
        if key == "userA" or not ctx.rows[key]:
            continue
        ctx.report_denials(key, rev_ts + 2.5, float("inf"),
                           "control_%s_post_revoke" % key, 2, False)
    ctx.require_revalidation([k for k in ROLE_KEY if ctx.role_knobbed(k)])
    ctx.evidence["installEpochs"] = {
        key: sorted({e for _t, _g, e in installs})
        for key, installs in ctx.installs.items()}
    ctx.evidence["revoke"] = rev
    return {"passed": all(ctx.checks.values()), "checks": ctx.checks,
            "details": ctx.details, "evidence": ctx.evidence}


def check_offline_rejoin_epoch_skip(ctx: RunCtx) -> Dict[str, Any]:
    """S5: user/A is SIGSTOPped across two controller version advances
    (grant C -> epoch 2, revoke provider/A -> epoch 3).  On SIGCONT it must
    resume with a *direct* jump to the newest signed version: its install
    history contains epoch 1 and epoch 3 but never epoch 2.

    The freeze window is measured from user-A's log (the SIGSTOP is the
    run's one multi-second gap, since a stopped process writes nothing)
    rather than trusted from the launcher's stopUserAMs: process-start
    skew shifts the effective stop by ~1.5 s, and the pre-stop and
    post-resume traffic windows must align with the freeze that actually
    happened."""
    epochs_a = sorted({e for _t, _g, e in ctx.installs["userA"]})
    e2_install = ctx.install_ts("userA", 2)
    e3_install = ctx.install_ts("userA", 3)
    e3_rev = next((r for r in ctx.revokes if r["epoch"] == 3), None)
    ctx.check("skip_epoch2", 1 in epochs_a and 3 in epochs_a and 2 not in epochs_a,
              "userA epochs=%s" % epochs_a)
    ctx.check("epoch2_existed", len(ctx.grants) >= 1 and
              any(g["epoch"] == 2 for g in ctx.grants),
              "grants=%d epochs=%s" %
              (len(ctx.grants), [g["epoch"] for g in ctx.grants]))
    if e3_rev and e3_install:
        ctx.check("rejoin_jumps_to_latest",
                  e3_install >= e3_rev["ts"] - 0.5 and
                  (e2_install is None or e2_install > e3_rev["ts"]),
                  "e3 install=%.3f rev=%.3f" % (e3_install, e3_rev["ts"]))
    else:
        ctx.check("rejoin_jumps_to_latest", False,
                  "missing e3 install or e3 revoke")
    freeze = freeze_window(_role_logs(ctx.output, "userA"))
    if freeze is None:
        ctx.check("pre_stop_success", False, "no freeze gap in user-A log")
        ctx.check("post_rejoin_success", False, "no freeze gap in user-A log")
    else:
        freeze_start, freeze_end = freeze
        # Traffic enqueued before the freeze completed while user/A was
        # still running; a row enqueued within one request-timeout budget
        # of the freeze can only fail after SIGCONT (its 5.5 s timeout
        # expires inside the freeze), so it is a freeze artifact, not a
        # denial -- report_denials truncates it against *freeze_start*.
        ctx.report_denials("userA", 0.0, freeze_start, "pre_stop_success",
                           1, False, deadline=freeze_start)
        if e3_install is not None:
            # Post-resume traffic in the epoch-3 world: user/A must be
            # authorized and re-converged after its direct jump.
            lo = max(e3_install + 1.5, freeze_end + 1.0)
            ctx.report_denials("userA", lo, float("inf"),
                               "post_rejoin_success", 2, False)
        else:
            ctx.check("post_rejoin_success", False, "no epoch3 install")
    # provider/A (revoked at epoch 3) stops serving once it discovers epoch 3:
    # requests keep arriving (users are authorized controls), so it must
    # refuse them and publish no ACK/RESPONSE after the enforcement window.
    p_inst = ctx.install_ts("providerA", 3)
    if p_inst is not None:
        logs = _role_logs(ctx.output, "providerA")
        lo = p_inst + PROVIDER_ENFORCEMENT_GRACE_S
        denials = prov_denial_log_counts(logs, lo, float("inf"))
        serving = prov_serving_log_counts(logs, lo, float("inf"))
        ctx.check("revoked_provider_stops",
                  denials >= 2 and serving == 0,
                  "post-e3-discovery(ts=%.3f+%.1fs) denials=%d"
                  " servingPublications=%d" %
                  (p_inst, PROVIDER_ENFORCEMENT_GRACE_S, denials, serving))
    else:
        ctx.check("revoked_provider_stops", False, "providerA never saw epoch3")
    ctx.require_revalidation(["userB", "providerB"])
    ctx.evidence["installEpochs"] = {
        key: sorted({e for _t, _g, e in installs})
        for key, installs in ctx.installs.items()}
    return {"passed": all(ctx.checks.values()), "checks": ctx.checks,
            "details": ctx.details, "evidence": ctx.evidence}


def check_status_source_equivalence(ctx: RunCtx) -> Dict[str, Any]:
    """S6: after the revocation every role converges to the same
    (generation, epoch) that the controller published -- the status source
    is the controller, and no role diverges."""
    result = _check_identity_family(ctx, "userA", 1)
    published = sum(_count(p, "NDNSF_POLICY_STATUS_PUBLISHED")
                    for p in ctx.output.glob("controller-*.log"))
    ctx.check("controller_published", published >= 1,
              "NDNSF_POLICY_STATUS_PUBLISHED=%d" % published)
    last_installs = {}
    for key, installs in ctx.installs.items():
        if installs:
            last_installs[key] = (installs[-1][1], installs[-1][2])
    ctx.check("roles_converge_to_controller",
              bool(last_installs) and all(
                  (g, e) == (ctx.gen, ctx.epoch2)
                  for g, e in last_installs.values()),
              "lastInstalls=%s controller=(%d,%d)" %
              (last_installs, ctx.gen, ctx.epoch2))
    result["checks"] = ctx.checks
    result["passed"] = all(ctx.checks.values())
    return result


def check_controller_unavailable(ctx: RunCtx) -> Dict[str, Any]:
    """S7: controller is SIGINTed at 4 s; only user/A is knobbed.  user/A
    discovers the 1.8 s revocation before the controller dies, is denied
    locally from its first request on, and keeps attempting scheduled
    revalidation against the dead authority; user/B (no knob, stale
    epoch-1 material) keeps succeeding end-to-end without the controller."""
    rev = ctx.rev
    if rev is None:
        ctx.check("revoke_observed", False, "no revoke line")
        ctx.check("early_discovery", False, "-")
        ctx.check("userA_all_denied", False, "-")
        ctx.check("revalidation_after_controller_death", False, "-")
        ctx.check("control_userB_without_controller", False, "-")
        ctx.check("providers_serve_without_controller", False, "-")
        return {"passed": False, "checks": ctx.checks, "details": ctx.details,
                "evidence": ctx.evidence}
    ctx.check("revoke_observed", True, "epoch=%d ts=%.3f" % (rev["epoch"], rev["ts"]))
    inst_ts = ctx.install_ts("userA", rev["epoch"])
    ctx.check("early_discovery", inst_ts is not None and
              inst_ts <= rev["ts"] + 3.0,
              "userA epoch%d install=%s" %
              (rev["epoch"], ("%.3f" % inst_ts) if inst_ts else "MISSING"))
    # Every user/A request is enqueued after discovery (revocation at 1.8 s,
    # first request ~3.4 s) and must be denied locally: RequestService returns
    # an empty request id, so no CSV row exists and each denial is one reject
    # log line.
    ok, _fail = row_success_counts(ctx.rows["userA"], 0.0, float("inf"))
    ctx.check("userA_no_success_rows", ok == 0,
              "success rows=%d (locally-denied requests write no rows)" % ok)
    if inst_ts is not None:
        ctx.report_log_denials("userA", inst_ts + 0.2, float("inf"),
                               "userA_all_denied", 2)
    else:
        ctx.check("userA_all_denied", False, "no discovery")
    # user/A keeps revalidating after the controller died (fire markers with
    # ts beyond the controller's last log line).
    ctrl_death = max([last_log_ts(p) or 0.0 for p in ctx.output.glob("controller-*.log")]
                     or [0.0])
    after_death = sum(_count_after(log, "NDNSF_POLICY_STATUS_REVALIDATION", ctrl_death)
                      for log in _role_logs(ctx.output, "userA"))
    ctx.check("revalidation_after_controller_death",
              after_death >= 1 and ctrl_death > 0,
              "markers after controller death(%.3f)=%d" % (ctrl_death, after_death))
    ctx.report_denials("userB", ctrl_death + 0.5, float("inf"),
                       "control_userB_without_controller", 2, False)
    prov_valid = []
    for key in ("providerA", "providerB"):
        prov_valid += [a for a in ctx.audits[key]
                       if a[2] == "REQUEST" and a[3] and a[0] >= ctrl_death + 0.5]
    ctx.check("providers_serve_without_controller", len(prov_valid) >= 1,
              "provider validated REQUEST after death=%d" % len(prov_valid))
    ctx.evidence["installEpochs"] = {
        key: sorted({e for _t, _g, e in installs})
        for key, installs in ctx.installs.items()}
    ctx.evidence["revoke"] = rev
    return {"passed": all(ctx.checks.values()), "checks": ctx.checks,
            "details": ctx.details, "evidence": ctx.evidence}


def _mode_marker_family(ctx: RunCtx, marker: str) -> Dict[str, Any]:
    """S8/S9/S10/S13 common part: identity-family revocation plus a
    mode-specific marker that must exist before the revocation and must not
    be produced by the affected role afterwards (denied pre-publish requests
    never reach the mode machinery)."""
    result = _check_identity_family(ctx, "userA", 1)
    inst_ts = ctx.install_ts("userA", ctx.epoch2)
    counts = {}
    for key in ROLE_KEY:
        logs = _role_logs(ctx.output, key)
        counts[key] = sum(_count(log, marker) for log in logs)
    affected_after = 0
    if inst_ts is not None:
        affected_after = sum(_count_after(log, marker, inst_ts)
                             for log in _role_logs(ctx.output, "userA"))
    ctx.check("affected_%s_before" % marker.replace("_", "-"),
              counts["userA"] >= 1 and affected_after == 0,
              "userA total=%d afterEpoch2Install=%d" %
              (counts["userA"], affected_after))
    ctx.check("unaffected_%s" % marker.replace("_", "-"),
              counts["userB"] >= 1,
              "userB total=%d" % counts["userB"])
    ctx.evidence["markerCounts"] = counts
    result["checks"] = ctx.checks
    result["passed"] = all(ctx.checks.values())
    return result


def check_large_response_invalidation(ctx: RunCtx) -> Dict[str, Any]:
    """S8: large-response mode.  The resolved-payload marker must be
    produced by user/A only before its epoch-2 install and by user/B
    throughout."""
    return _mode_marker_family(ctx, "NDNSF_REQUEST_SCOPED_LARGE_RESPONSE_RESOLVED")


def check_targeted_refill_invalidation(ctx: RunCtx) -> Dict[str, Any]:
    """S9: targeted mode.  Targeted fast-path traffic exists before the
    revocation; afterwards the affected user is denied before any new
    targeted request is created."""
    return _mode_marker_family(ctx, "TARGETED_REQUEST_CREATED")


def check_stream_invalidation(ctx: RunCtx) -> Dict[str, Any]:
    """S10: stream mode.  Stream users write no request CSV (their terminal
    lives in stream markers), so the gate is marker-based: the affected user
    establishes a stream before the revocation, every *later* attempt fails
    at the admission boundary (SPEC179_STREAM_START_FAILED) and no new
    stream starts after its epoch-2 install; the unaffected user keeps
    starting new streams.  Network event flow is required on the provider
    side."""
    MARK_STARTED = "SPEC179_STREAM_STARTED"
    MARK_FAILED = "SPEC179_STREAM_START_FAILED"
    rev = ctx.rev
    ctx.check("revoke_observed", rev is not None,
              "epoch=%s ts=%s" % (rev["epoch"] if rev else "-",
                                  ("%.3f" % rev["ts"]) if rev else "-"))
    inst_ts = ctx.install_ts("userA", ctx.epoch2) if rev else None
    ctx.check("stream_userA_discovery",
              inst_ts is not None and rev is not None and
              inst_ts <= rev["ts"] + 3.0,
              "userA epoch%d install=%s rev=%.3f" %
              (ctx.epoch2, ("%.3f" % inst_ts) if inst_ts else "MISSING",
               rev["ts"] if rev else -1.0))
    if inst_ts is None:
        ctx.check("stream_established_before", False, "no discovery")
        ctx.check("stream_denied_after", False, "no discovery")
        ctx.check("stream_control_after", False, "no discovery")
        ctx.check("provider_stream_events", False, "no discovery")
        return {"passed": False, "checks": ctx.checks,
                "details": ctx.details, "evidence": ctx.evidence}
    started_a = [ts for ts, msg in _ts_lines(_role_logs(ctx.output, "userA")[0])
                 if MARK_STARTED in msg]
    failed_a = [ts for ts, msg in _ts_lines(_role_logs(ctx.output, "userA")[0])
                if MARK_FAILED in msg]
    ctx.check("stream_established_before",
              any(ts < inst_ts for ts in started_a),
              "userA STARTED=%d (pre-install=%d)" %
              (len(started_a), sum(1 for ts in started_a if ts < inst_ts)))
    ctx.check("stream_denied_after",
              any(ts >= inst_ts for ts in failed_a) and
              not any(ts >= inst_ts for ts in started_a),
              "userA START_FAILED post-install=%d STARTED post-install=%d" %
              (sum(1 for ts in failed_a if ts >= inst_ts),
               sum(1 for ts in started_a if ts >= inst_ts)))
    started_b = [ts for ts, msg in _ts_lines(_role_logs(ctx.output, "userB")[0])
                 if MARK_STARTED in msg]
    ctx.check("stream_control_after",
              any(ts >= inst_ts for ts in started_b),
              "userB STARTED total=%d post-install=%d" %
              (len(started_b), sum(1 for ts in started_b if ts >= inst_ts)))
    provider_events = 0
    for key in ("providerA", "providerB"):
        for path in _role_logs(ctx.output, key):
            provider_events += _count(path, "STREAM_EVENT_PUBLISHED")
    ctx.check("provider_stream_events", provider_events >= 1,
              "provider STREAM_EVENT_PUBLISHED=%d" % provider_events)
    ctx.evidence["streamStartUserA"] = [round(ts, 3) for ts in started_a]
    ctx.evidence["streamStartUserB"] = [round(ts, 3) for ts in started_b]
    ctx.evidence["streamFailUserA"] = [round(ts, 3) for ts in failed_a]
    ctx.evidence["revoke"] = rev
    ctx.evidence["installEpochs"] = {
        key: sorted({e for _t, _g, e in installs})
        for key, installs in ctx.installs.items()}
    return {"passed": all(ctx.checks.values()), "checks": ctx.checks,
            "details": ctx.details, "evidence": ctx.evidence}


def check_hintless_scheduled_refresh(ctx: RunCtx) -> Dict[str, Any]:
    """S11: only user/A carries the revalidation knob.  It must discover the
    revocation by its own scheduled refresh (bounded window), with no other
    role ever learning epoch >= 2 (no hints were available to propagate it),
    while user/B keeps succeeding in the untouched epoch-1 world."""
    rev = ctx.rev
    ctx.check("revoke_observed", rev is not None,
              "epoch=%s ts=%s" %
              (rev["epoch"] if rev else "-", rev["ts"] if rev else "-"))
    inst_ts = ctx.install_ts("userA", ctx.epoch2) if rev else None
    ctx.check("self_scheduled_discovery",
              inst_ts is not None and rev is not None and
              rev["ts"] < inst_ts <= rev["ts"] + 3.0,
              "userA epoch%d install=%s rev=%.3f" %
              (ctx.epoch2, ("%.3f" % inst_ts) if inst_ts else "MISSING",
               rev["ts"] if rev else -1.0))
    leaked = 0
    for key in ("userB", "providerA", "providerB"):
        for _t, _g, e in ctx.installs[key]:
            if ctx.epoch2 and e >= ctx.epoch2:
                leaked += 1
    ctx.check("no_other_role_learns_epoch2", leaked == 0,
              "other roles installing epoch>=%s: %d" % (ctx.epoch2, leaked))
    markers = sum(_count(log, "NDNSF_POLICY_STATUS_REVALIDATION")
                  for log in _role_logs(ctx.output, "userA"))
    ctx.check("userA_revalidations", markers >= 3,
              "userA fire markers=%d" % markers)
    if inst_ts is not None:
        ctx.report_log_denials("userA", inst_ts + 0.2, float("inf"),
                               "userA_denied_after_self_discovery", 2)
    else:
        ctx.check("userA_denied_after_self_discovery", False, "no discovery")
    ctx.report_denials("userB", (rev["ts"] if rev else 0.0) + 1.0, float("inf"),
                       "userB_unaffected_epoch1_world", 2, False)
    ctx.evidence["installEpochs"] = {
        key: sorted({e for _t, _g, e in installs})
        for key, installs in ctx.installs.items()}
    ctx.evidence["revoke"] = rev
    return {"passed": all(ctx.checks.values()), "checks": ctx.checks,
            "details": ctx.details, "evidence": ctx.evidence}


def check_controller_restart(ctx: RunCtx) -> Dict[str, Any]:
    """S12: controller-1 exits its bounded window and controller-2 resumes
    under a *new* generation (epoch resets to 1 on restart; the two
    controller generations are distinct -- that is how the authority is
    designed to resume).  Every knobbed role must follow the generation
    switch: chronological installs stay monotonic in (generation, epoch) and
    end on the restarted authority's revoked version, so no role keeps
    serving the dead controller's authority."""
    result = _check_identity_family(ctx, "userA", 1)
    rev = ctx.rev
    gens = sorted({g for key in ROLE_KEY for _t, g, _e in ctx.installs[key]} |
                  ({rev["generation"]} if rev else set()))
    ctx.check("restart_generation_switch", rev is not None and len(gens) >= 2,
              "generations observed=%s revokeGen=%s" %
              (gens, rev["generation"] if rev else "-"))
    violations = []
    for key in ROLE_KEY:
        seq = [(g, e) for _t, g, e in ctx.installs[key]]
        if any(a > b for a, b in zip(seq, seq[1:])):
            violations.append(key)
    ctx.check("install_monotonic_across_restart", not violations,
              "roles with (generation,epoch) regression=%s" % violations)
    stale = []
    for key in ROLE_KEY:
        installs = ctx.installs[key]
        if installs and (installs[-1][1], installs[-1][2]) != (ctx.gen, ctx.epoch2):
            stale.append("%s:%s" % (key, (installs[-1][1], installs[-1][2])))
    ctx.check("roles_converge_on_restarted_authority", not stale and rev is not None,
              "roles not ending on (%s,%s)=%s" %
              (ctx.gen, ctx.epoch2, stale))
    ctx.evidence["generations"] = gens
    result["checks"] = ctx.checks
    result["passed"] = all(ctx.checks.values())
    return result


def _terminal_cleanup_counts(output: Path, user_dir: str) -> Counter:
    """Lifecycle rows carrying a final_cleanup_reason, counted per request id.

    A replayed or double-executed request would produce more than one
    terminal cleanup row for the same id."""
    counts: Counter = Counter()
    path = output / user_dir / "request_lifecycle.csv"
    if path.is_file():
        for row in csv.DictReader(path.open(encoding="utf-8")):
            if row.get("final_cleanup_reason"):
                counts[row.get("request_id", "")] += 1
    return counts


def check_tamper_replay(ctx: RunCtx) -> Dict[str, Any]:
    """S13: replay/at-most-once invariants plus the identity revocation.
    Byte-level tampering is exercised by unit/component tests (T011:43), so
    the network gate verifies that no request id ever produces two terminal
    rows or two executions, and that the revocation still denies."""
    result = _check_identity_family(ctx, "userA", 1)
    rid_seen = defaultdict(list)
    for key in ("userA", "userB"):
        for rid, r in ctx.rows[key].items():
            rid_seen[rid].append((key, r["success"]))
    dup = {rid: v for rid, v in rid_seen.items() if len(v) > 1}
    ctx.check("request_ids_unique_across_users", not dup,
              "duplicated request ids=%d" % len(dup))
    double_terminal = {}
    for key in ("userA", "userB"):
        for rid, n in _terminal_cleanup_counts(ctx.output, ROLE_KEY[key]).items():
            if n > 1:
                double_terminal.setdefault(rid, []).append((key, n))
    ctx.check("at_most_one_terminal", not double_terminal,
              "request ids with >1 terminal cleanup=%s" % double_terminal)
    ctx.evidence["replayMarkers"] = {
        str(p.name): _count(p, "NDNSF_REPLAY") for p in ctx.output.glob("*.log")}
    result["checks"] = ctx.checks
    result["passed"] = all(ctx.checks.values())
    return result


def check_rotation_failure_retry(ctx: RunCtx) -> Dict[str, Any]:
    """Prove the affected runtime denies during failure and after recovery."""
    events = _ts_lines(ctx.output / "controller-1.log")
    failed = [ts for ts, msg in events if "NDNSF_CONTROLLER_ABE_REKEY_FAILED" in msg]
    recovered = [ts for ts, msg in events if "NDNSF_CONTROLLER_ABE_REKEY_RECONCILED" in msg]
    ctx.check("rotation_failed_once", len(failed) == 1, str(failed))
    ctx.check("rotation_recovered_once", len(recovered) == 1, str(recovered))
    ctx.check("same_target_retry_succeeded", ctx.rev is not None and
              ctx.rev["identity"] == "/example/hello/user/A" and ctx.rev["epoch"] == 3,
              str(ctx.rev))
    if len(failed) == 1 and len(recovered) == 1:
        lo, hi = failed[0], recovered[0]
        ctx.check("recovery_follows_failure", hi > lo, "%.3f -> %.3f" % (lo, hi))
        installed = ctx.install_ts("userA", 2)
        ctx.check("failure_status_installed", installed is not None and lo <= installed < hi,
                  str(installed))
        if installed is not None:
            ctx.report_log_denials("userA", installed, hi, "denied_while_rotation_pending", 1)
        for role in ("userA", "userB", "providerA", "providerB"):
            ts = ctx.install_ts(role, 3)
            ctx.check("recovered_status_" + role, ts is not None and ts >= hi - 0.5, str(ts))
        ctx.report_log_denials("userA", hi + 1, float("inf"), "still_denied_after_recovery", 2)
        pre_ok, _ = row_success_counts(ctx.rows["userA"], 0, lo - 1)
        post_ok, post_fail = row_success_counts(ctx.rows["userB"], hi + 2, float("inf"))
        leaked, _ = row_success_counts(ctx.rows["userA"], lo + 1, float("inf"))
        ctx.check("affected_worked_before_failure", pre_ok > 0, str(pre_ok))
        ctx.check("affected_no_post_failure_success", leaked == 0, str(leaked))
        ctx.check("unaffected_recovers_without_failure", post_ok > 0 and post_fail == 0,
                  "success=%d failure=%d" % (post_ok, post_fail))
    return {"passed": all(ctx.checks.values()), "checks": ctx.checks,
            "details": ctx.details, "evidence": ctx.evidence}


def check_provider_online_grant(ctx: RunCtx) -> Dict[str, Any]:
    """First service-offering grant; never censor target or control failures."""
    controller = _ts_lines(ctx.output / "controller-1.log")
    grants = [(ts, msg) for ts, msg in controller
              if "NDNSF_CONTROLLER_GRANT " in msg
              and "identity=/example/hello/provider/B " in msg
              and "service=/HELLO " in msg and "attribute=/SERVICE/HELLO " in msg
              and "abeParametersUnchanged=true" in msg]
    ctx.check("controller_grants_provider_service", len(grants) == 1, str(grants))
    applied = [g for g in ctx.grants if g["success"] == 1
               and g["identity"] == "/example/hello/provider/B" and g["service"] == "/HELLO"]
    ctx.check("grant_applied_once", len(applied) == 1, str(applied))
    provider = _ts_lines(ctx.output / "provider-B.log")
    explicit_renewals = [ts for ts, msg in provider if "NDNSF_APP_PERMISSION_REFETCH" in msg]
    user_renewals = [ts for ts, msg in _ts_lines(ctx.output / "user-B.log")
                     if "NDNSF_APP_PERMISSION_REFETCH" in msg]
    grant_ts = grants[0][0] if len(grants) == 1 else None
    # A scheduled status advance can renew Provider permissions before the
    # App timer. Attribute actual fetches, and verify the later App renewal
    # stays idempotent instead of requiring it to be the first discovery.
    renewals = [ts for ts, msg in provider if "Fetch provider permissions:" in msg
                and grant_ts is not None and ts > grant_ts]
    renew_ts = min(renewals) if renewals else None
    ready_ts = user_renewals[0] if len(user_renewals) == 1 else None
    ordered = (grant_ts is not None and renew_ts is not None and ready_ts is not None
               and grant_ts < renew_ts < ready_ts)
    ctx.check("permission_renewal_order", ordered,
              str((grant_ts, renew_ts, ready_ts)))
    ctx.check("app_renewal_observed", len(explicit_renewals) == 1
              and grant_ts is not None and explicit_renewals[0] > grant_ts,
              str(explicit_renewals))
    ctx.check("provider_initially_pending", any(
        "NDNSF_NAC_BOOTSTRAP_PENDING role=provider" in msg
        and grant_ts is not None and ts < grant_ts for ts, msg in provider), "initial DKEY pending")
    serving = [(ts, msg) for ts, msg in provider if _PROV_SERVING.search(msg)]
    ctx.check("no_service_before_renewal", renew_ts is not None
              and not any(ts < renew_ts for ts, _ in serving), str(serving[:3]))
    ctx.check("provider_responds_after_renewal", ready_ts is not None and any(
        ts >= ready_ts and "messageType=RESPONSE" in msg for ts, msg in serving), str(len(serving)))
    refreshed = [(ts, msg) for ts, msg in provider
                 if re.search(r"NDNSF_NAC_DKEY_REFRESH_REQUESTED\b.*\bepoch=2\b", msg)]
    ctx.check("target_only_refresh_once", len(refreshed) == 1 and renew_ts is not None
              and refreshed[0][0] >= renew_ts and "reason=grant-only" in refreshed[0][1], str(refreshed))
    others = [(p.name, ts, msg) for p in ctx.output.glob("*.log")
              if p.name in {"provider-A.log", "user-A.log", "user-B.log"}
              for ts, msg in _ts_lines(p)
              if re.search(r"NDNSF_NAC_DKEY_REFRESH_REQUESTED\b.*\bepoch=2\b", msg)]
    ctx.check("unaffected_no_dkey_refresh", not others, str(others))
    target = list(ctx.rows["userB"].values())
    post = [r for r in target if ready_ts is not None and r["enqueue_s"] >= ready_ts]
    ctx.check("target_completes_through_granted_provider", bool(post) and all(
        r["success"] and r["selected_provider"] == "/example/hello/provider/B" for r in post),
        "success=%d rows=%d" % (sum(r["success"] for r in post), len(post)))
    ctx.check("target_no_early_success", grant_ts is not None and not any(
        r["success"] and r["enqueue_s"] < grant_ts for r in target), str(len(target)))
    control = list(ctx.rows["userA"].values())
    ctx.check("control_has_no_failures", bool(control) and all(
        r["success"] and r["selected_provider"] == "/example/hello/provider/A" for r in control),
              "success=%d rows=%d" % (sum(r["success"] for r in control), len(control)))
    ctx.check("control_spans_grant", grant_ts is not None and ready_ts is not None
              and any(r["enqueue_s"] < grant_ts for r in control)
              and any(r["enqueue_s"] > ready_ts for r in control), str(len(control)))
    if ctx.config.get("initialProviderPermissionTransportLoss"):
        timeouts = [ts for ts, msg in provider
                    if "PermissionResponse timeout:" in msg and "final=1" in msg]
        ctx.check("exhaustion_precedes_grant_and_renewal", ordered and bool(timeouts)
                  and timeouts[0] < grant_ts, str(timeouts))
    ctx.evidence["providerGrant"] = {"grantTime": grant_ts, "renewalTime": renew_ts,
                                    "userRenewalTime": ready_ts, "targetRows": len(target),
                                    "postRenewalRows": len(post), "controlRows": len(control)}
    return {"passed": all(ctx.checks.values()), "checks": ctx.checks,
            "details": ctx.details, "evidence": ctx.evidence}


EVALUATORS = {
    "provider-grant-only-advance": check_provider_online_grant,
    "provider-grant-after-permission-exhaustion": check_provider_online_grant,
    "revocation-rotation-failure-retry": check_rotation_failure_retry,
    "user-identity-revocation": check_user_identity_revocation,
    "provider-identity-revocation": check_provider_identity_revocation,
    "service-scoped-revocation-with-unaffected-control": check_service_scoped_revocation,
    "inflight-revocation": check_inflight_revocation,
    "offline-rejoin-epoch-skip": check_offline_rejoin_epoch_skip,
    "controller-cache-provider-status-retrieval": check_status_source_equivalence,
    "controller-unavailable-expiry": check_controller_unavailable,
    "large-response-invalidation": check_large_response_invalidation,
    "targeted-refill-invalidation": check_targeted_refill_invalidation,
    "stream-invalidation": check_stream_invalidation,
    "hintless-scheduled-refresh": check_hintless_scheduled_refresh,
    "controller-restart": check_controller_restart,
    "selection-response-tamper-and-replay": check_tamper_replay,
}


def evaluate(scenario: str, output_dir: Path,
             config: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
    """Run the scenario evaluator over a completed run directory."""
    ctx = RunCtx(output_dir, config or {})
    evaluator = EVALUATORS.get(scenario)
    if evaluator is None:
        return {"passed": None, "checks": {}, "details": {},
                "evidence": {}, "reason": "no scenario evaluator"}
    result = evaluator(ctx)
    result["scenario"] = scenario
    result["passed"] = bool(result.get("passed"))
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("scenario", choices=sorted(EVALUATORS))
    parser.add_argument("run_dir")
    parser.add_argument("--config-json", default="",
                        help="manifest config dict (optional, for knobs)")
    args = parser.parse_args()
    config = {}
    if args.config_json:
        config = json.loads(args.config_json)
    elif Path(args.run_dir, "manifest.json").is_file():
        try:
            config = json.loads(
                Path(args.run_dir, "manifest.json").read_text(encoding="utf-8")
            ).get("config", {})
        except json.JSONDecodeError:
            config = {}
    result = evaluate(args.scenario, Path(args.run_dir), config)
    print(json.dumps(result, indent=2, sort_keys=True, default=str))
    return 0 if result.get("passed") else 1


if __name__ == "__main__":
    raise SystemExit(main())
