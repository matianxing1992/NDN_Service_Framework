"""Host-only admission, sampling and bounded cleanup for native experiments.

This supervisor owns OS processes, never inference or protocol assertions.
Missing native ownership counters must remain unobserved in the caller.
"""
from __future__ import annotations

import json
import ctypes
import math
import os
from pathlib import Path
import shutil
import signal
import subprocess
import time


DEFAULT_LIMITS = {
    "minAvailableBytes": 1536 * 1024 * 1024,
    "minSwapFreeBytes": 512 * 1024 * 1024,
    "minDiskFreeBytes": 4 * 1024 * 1024 * 1024,
    "maxOwnedSwapBytes": 256 * 1024 * 1024,
    # Kept as a compatibility alias for historical profiles/receipts.  New
    # callers should use maxOwnedSwapBytes; global swap I/O is diagnostic only.
    "maxSwapIoBytes": 256 * 1024 * 1024,
    "timeoutSeconds": 3600,
    "stopGraceSeconds": 10,
}


def validate_limits(value=None):
    if value is not None and not isinstance(value, dict):
        raise ValueError("RESOURCE_LIMITS_INVALID")
    if value is not None and set(value) - set(DEFAULT_LIMITS):
        raise ValueError("RESOURCE_LIMITS_UNKNOWN_FIELD")
    provided = value or {}
    limits = {**DEFAULT_LIMITS, **provided}
    if "maxSwapIoBytes" in provided and "maxOwnedSwapBytes" not in provided:
        limits["maxOwnedSwapBytes"] = provided["maxSwapIoBytes"]
    for key, number in limits.items():
        if (isinstance(number, bool) or not isinstance(number, (int, float)) or
                number > 2**63 - 1 or not math.isfinite(number) or number <= 0):
            raise ValueError("RESOURCE_LIMIT_INVALID:" + key)
        if key.endswith("Bytes") and (not isinstance(number, int) or number > 2**63 - 1):
            raise ValueError("RESOURCE_LIMIT_INVALID:" + key)
    if limits["timeoutSeconds"] > 86400 or limits["stopGraceSeconds"] > 60:
        raise ValueError("RESOURCE_DEADLINE_TOO_LARGE")
    return limits


def host_sample(directory):
    memory = {}
    for line in Path("/proc/meminfo").read_text().splitlines():
        key, value = line.split(":", 1)
        memory[key] = int(value.split()[0]) * 1024
    vm = dict(line.split() for line in Path("/proc/vmstat").read_text().splitlines())
    return {
        "availableBytes": memory["MemAvailable"],
        "swapFreeBytes": memory["SwapFree"],
        "swapUsedBytes": memory["SwapTotal"] - memory["SwapFree"],
        "swapIoBytes": (int(vm["pswpin"]) + int(vm["pswpout"])) * os.sysconf("SC_PAGE_SIZE"),
        "diskFreeBytes": shutil.disk_usage(directory).free,
    }


def process_table():
    """PID/starttime protects retained observations against ordinary PID reuse."""
    result = {}
    for path in Path("/proc").glob("[0-9]*/stat"):
        try:
            # comm may contain spaces and ')'; fields after its last ')' are stable.
            fields = path.read_text().rsplit(")", 1)[1].split()
            pid = int(path.parent.name)
            result[pid] = {
                "pid": pid, "state": fields[0], "parent": int(fields[1]),
                "group": int(fields[2]), "start": int(fields[19]),
                "rssBytes": max(0, int(fields[21])) * os.sysconf("SC_PAGE_SIZE"),
            }
        except (FileNotFoundError, ProcessLookupError, PermissionError):
            continue
    return result


def owned_swap_bytes(rows):
    """Return swap-resident bytes for the processes owned by this supervisor.

    ``/proc/vmstat`` is host-global: an unrelated editor, indexer, or desktop
    process can fault its own swapped pages while the native workload is
    running.  Use the owned process set for the stop decision and retain the
    global counter as diagnostic evidence.  A process can disappear between
    the table and status reads, which is normal during cleanup and contributes
    zero rather than turning a completed run into a monitor failure.
    """
    total = 0
    for row in rows:
        try:
            for line in Path(f"/proc/{row['pid']}/status").read_text().splitlines():
                if line.startswith("VmSwap:"):
                    total += int(line.split()[1]) * 1024
                    break
        except (FileNotFoundError, ProcessLookupError, PermissionError, ValueError):
            continue
    return total


def owned_processes(root_pid, known):
    table = process_table()
    owned = {pid for pid, row in table.items()
             if (row["group"] == root_pid or row["parent"] == os.getpid() or
                 (pid in known and known[pid] == row["start"]))}
    # Expand through descendants, including workers that create their own session.
    while True:
        more = {pid for pid, row in table.items() if row["parent"] in owned}
        if more <= owned:
            break
        owned |= more
    for pid in owned:
        known[pid] = table[pid]["start"]
    return [table[pid] for pid in sorted(owned)]


def _alive(rows):
    return [row for row in rows if row["state"] not in {"Z", "X"}]


def stop_owned(proc, known, grace):
    """Signal the owned session plus observed detached descendants, then reap root."""
    for sig in (signal.SIGINT, signal.SIGTERM, signal.SIGKILL):
        rows = _alive(owned_processes(proc.pid, known))
        if not rows and proc.poll() is not None:
            break
        # The session was created by Popen; never signal our supervisor's group.
        if proc.poll() is None or any(row["group"] == proc.pid for row in rows):
            try:
                os.killpg(proc.pid, sig)
            except ProcessLookupError:
                pass
        for row in rows:
            if row["group"] != proc.pid:
                current = process_table().get(row["pid"])
                if current and current["start"] == row["start"]:
                    try:
                        os.kill(row["pid"], sig)
                    except ProcessLookupError:
                        pass
        deadline = time.monotonic() + (2 if sig == signal.SIGKILL else grace)
        while time.monotonic() < deadline:
            proc.poll()
            if proc.poll() is not None and not _alive(owned_processes(proc.pid, known)):
                break
            time.sleep(0.05)
    proc.poll()
    # The dedicated subreaper also owns orphaned grandchildren. Reap those
    # without stealing the Popen root's wait status.
    for row in owned_processes(proc.pid, known):
        if row["parent"] == os.getpid() and row["pid"] != proc.pid:
            try:
                os.waitpid(row["pid"], os.WNOHANG)
            except ChildProcessError:
                pass
    return _alive(owned_processes(proc.pid, known))


def emergency_reap():
    """Independent fallback when /proc table parsing or normal cleanup fails.

    Only the isolated subreaper calls this. Its kernel children list contains
    no caller-owned unrelated processes; killing a parent adopts its orphans.
    Do not use the failed sampling implementation during emergency cleanup.
    """
    children_file = Path(f"/proc/self/task/{os.getpid()}/children")
    deadline = time.monotonic() + 3
    killed = set()
    try:
        while True:
            children = [int(value) for value in children_file.read_text().split()]
            if not children:
                return {"remainingPids": [], "killCount": len(killed)}
            for pid in children:
                try:
                    os.kill(pid, signal.SIGKILL)
                    killed.add(pid)
                except ProcessLookupError:
                    pass
            while True:
                try:
                    pid, _ = os.waitpid(-1, os.WNOHANG)
                except ChildProcessError:
                    break
                if pid == 0:
                    break
            if time.monotonic() >= deadline:
                return {"remainingPids": [int(value) for value in children_file.read_text().split()],
                        "killCount": len(killed)}
            time.sleep(0.02)
    except OSError as exc:
        return {"remainingPids": None, "killCount": len(killed),
                "error": type(exc).__name__}


def run_guarded(command, *, cwd, stdout, sample_path, limits):
    """Main-thread Linux entry with an isolated subreaper ownership boundary.

    The forked supervisor has no unrelated children: even a double-forked
    worker that calls setsid is adopted here, not lost between /proc samples.
    Fork also preserves the already-open log descriptor without shell quoting.
    """
    limits = validate_limits(limits)
    read_fd, write_fd = os.pipe()
    try:
        worker = os.fork()
    except BaseException:
        os.close(read_fd)
        os.close(write_fd)
        raise
    if worker == 0:
        os.close(read_fd)
        def cancel(signum, frame):
            raise KeyboardInterrupt
        signal.signal(signal.SIGTERM, cancel)
        signal.signal(signal.SIGINT, cancel)
        try:
            libc = ctypes.CDLL(None, use_errno=True)
            if libc.prctl(36, 1, 0, 0, 0) != 0:  # PR_SET_CHILD_SUBREAPER
                raise OSError(ctypes.get_errno(), "cannot establish child subreaper")
            result = _run_guarded(command, cwd=cwd, stdout=stdout,
                                  sample_path=sample_path, limits=limits)
        except FileExistsError:
            result = {"returncode": None, "boundary": "EVIDENCE_CONFLICT",
                      "cleanup": "NOT_STARTED", "remainingProcesses": [],
                      "limits": limits, "samplePath": str(sample_path)}
        except BaseException as exc:
            # An unexpected supervisor error is never cleanup or native PASS.
            signal.signal(signal.SIGTERM, signal.SIG_IGN)
            signal.signal(signal.SIGINT, signal.SIG_IGN)
            emergency = emergency_reap()
            result = {"returncode": None,
                      "boundary": "SUPERVISOR_ERROR:" + type(exc).__name__,
                      "cleanup": "UNOBSERVED",
                      "remainingProcesses": [{"pid": pid, "state": "UNOBSERVED"}
                                             for pid in (emergency["remainingPids"] or [])],
                      "emergencyCleanup": emergency,
                      "limits": limits, "samplePath": str(sample_path)}
        try:
            with os.fdopen(write_fd, "w") as pipe:
                json.dump(result, pipe)
        finally:
            os._exit(0)

    os.close(write_fd)
    def forward(signum, frame):
        try:
            os.kill(worker, signal.SIGTERM)
        except ProcessLookupError:
            pass
    previous_term = signal.signal(signal.SIGTERM, forward)
    previous_int = signal.signal(signal.SIGINT, forward)
    try:
        with os.fdopen(read_fd) as pipe:
            payload = pipe.read()
        _, status = os.waitpid(worker, 0)
        if status != 0 or not payload:
            return {"returncode": None, "boundary": "SUPERVISOR_EXIT",
                    "cleanup": "UNOBSERVED", "remainingProcesses": [],
                    "limits": limits, "samplePath": str(sample_path)}
        return json.loads(payload)
    finally:
        signal.signal(signal.SIGTERM, previous_term)
        signal.signal(signal.SIGINT, previous_int)


def _run_guarded(command, *, cwd, stdout, sample_path, limits):
    """Run one isolated workload; return host facts, never a native PASS verdict."""
    limits = validate_limits(limits)
    sample_path = Path(sample_path)
    sample_path.parent.mkdir(parents=True, exist_ok=True)
    proc = None
    known = {}
    boundary = None
    residue = []
    started = time.monotonic()
    baseline_swap = None
    baseline_owned_swap = None
    returncode = None
    # Refuse to overwrite prior run evidence.
    with sample_path.open("x", encoding="utf-8", buffering=1) as output:
        def sample(phase):
            nonlocal baseline_swap, baseline_owned_swap
            value = host_sample(sample_path.parent)
            if baseline_swap is None:
                baseline_swap = value["swapIoBytes"]
            rows = owned_processes(proc.pid, known) if proc else []
            owned_swap = owned_swap_bytes(rows)
            if baseline_owned_swap is None:
                baseline_owned_swap = owned_swap
            record = {"phase": phase, "monotonicSeconds": time.monotonic(),
                      **value, "swapIoDeltaBytes": max(0, value["swapIoBytes"] - baseline_swap),
                      "ownedSwapBytes": owned_swap,
                      "ownedSwapDeltaBytes": max(0, owned_swap - baseline_owned_swap),
                      "processes": rows, "rssBytes": sum(row["rssBytes"] for row in rows),
                      "nativeCounters": None}
            output.write(json.dumps(record, sort_keys=True) + "\n")
            if value["availableBytes"] < limits["minAvailableBytes"]:
                return "MemAvailable"
            if value["diskFreeBytes"] < limits["minDiskFreeBytes"]:
                return "diskFree"
            # The global swap counter remains in every sample for diagnosis,
            # but it is not a workload stop condition.  Otherwise unrelated
            # swapped-out host processes can abort a valid native run.
            if value["swapFreeBytes"] < limits["minSwapFreeBytes"]:
                return "SwapFree"
            # Owned swap is retained as workload-local diagnostic evidence, but
            # it is not a standalone stop condition.  A bounded amount of
            # paging can occur while a large ONNX worker starts even when the
            # host still has safe MemAvailable and SwapFree.  Stopping on this
            # counter alone used to abort a valid startup and then signal the
            # nested MiniNDN supervisor, obscuring cleanup as KeyboardInterrupt.
            # The available-memory, swap-free, disk, and deadline gates above
            # remain the hard safety boundaries.
            return None

        try:
            metric = sample("admission")
            if metric:
                boundary = "RESOURCE_BOUNDARY:" + metric
            else:
                proc = subprocess.Popen(command, cwd=cwd, stdout=stdout,
                                        stderr=subprocess.STDOUT, start_new_session=True)
                while True:
                    metric = sample("running")
                    if metric:
                        boundary = "RESOURCE_BOUNDARY:" + metric
                        break
                    returncode = proc.poll()
                    if returncode is not None:
                        if _alive(owned_processes(proc.pid, known)):
                            boundary = "CHILD_PROCESS_LEAK"
                        break
                    if time.monotonic() - started >= limits["timeoutSeconds"]:
                        boundary = "DEADLINE_BOUNDARY"
                        break
                    time.sleep(min(1.0, limits["timeoutSeconds"]))
        except KeyboardInterrupt:
            boundary = "CANCELLED"
        except (OSError, ValueError, KeyError, IndexError) as exc:
            boundary = "RESOURCE_MONITOR_ERROR:" + type(exc).__name__
        finally:
            # A second operator signal must not interrupt the bounded drain.
            previous_int = signal.signal(signal.SIGINT, signal.SIG_IGN)
            previous_term = signal.signal(signal.SIGTERM, signal.SIG_IGN)
            try:
                if proc:
                    residue = stop_owned(proc, known, limits["stopGraceSeconds"])
                    returncode = proc.poll()
                try:
                    sample("drained" if not residue else "cleanup-failed")
                except (OSError, ValueError, KeyError, IndexError) as exc:
                    boundary = boundary or "RESOURCE_MONITOR_ERROR:" + type(exc).__name__
            finally:
                signal.signal(signal.SIGINT, previous_int)
                signal.signal(signal.SIGTERM, previous_term)
    return {"returncode": returncode, "boundary": boundary,
            "cleanup": "FAIL" if residue else "PASS", "remainingProcesses": residue,
            "limits": limits, "samplePath": str(sample_path)}
