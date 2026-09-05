"""Real native lifecycle probes inside the maintained Controller node.

SPEC181_LIFECYCLE_PROBE selects margin, cancel, or provider-idle. The parent
owns MiniNDN, the node keychain/environment, subprocess collection, and logs.
"""
import json
import os
from pathlib import Path
import runpy
import socket
import sys
import tempfile
import threading
import time

from ndnsf import ServiceController, ServiceProvider

start_background = ServiceController.start_background


def report(mode, **values):
    print("SPEC181_LIFECYCLE_PASS " + json.dumps(
        {"mode": mode, **values}, sort_keys=True), flush=True)


def provider_idle():
    provider = ServiceProvider(
        group="/example/group", controller="/example/controller",
        provider_prefix="/example/provider/BackboneNeck",
        trust_schema="examples/trust-schema.conf", handler_threads=1,
        ack_threads=1)
    try:
        begin = time.monotonic()
        ready = provider._native.wait_until_ready(80)
        elapsed = time.monotonic() - begin
        print(f"SPEC181_PROVIDER_IDLE ready={ready} elapsed={elapsed}", flush=True)
        if ready or elapsed < 0.06:
            raise RuntimeError("SPEC181_PROVIDER_READINESS_PREMATURE_TERMINAL")
        provider.add_handler("/BackboneNeck", lambda payload: payload)
        thread = provider.start_background("/BackboneNeck")
        try:
            if not provider._native.wait_until_ready(1):
                raise RuntimeError("SPEC181_RUNNING_PROVIDER_NOT_READY")
        finally:
            provider.stop()
            thread.join(1.0)
        if thread.is_alive():
            raise RuntimeError("SPEC181_PROVIDER_THREAD_NOT_COLLECTED")
        provider.stop()
        begin = time.monotonic()
        if provider._native.wait_until_ready(1000):
            raise RuntimeError("SPEC181_STOPPED_PROVIDER_REPORTED_READY")
        stopped_wait = time.monotonic() - begin
        if stopped_wait > 0.25:
            raise RuntimeError("SPEC181_STOPPED_PROVIDER_WAITER_NOT_RELEASED")
        report("provider-idle", idleWaitSeconds=elapsed,
               stoppedWaitSeconds=stopped_wait, startedReady=True,
               threadCollected=True)
    finally:
        provider.stop()


def margin(controller):
    original_run = controller.run
    # Delay the actual native start, so the ordinary 15 s Python waiter must
    # tolerate a scheduling delay beyond the former 10 s boundary.
    def delayed_run():
        time.sleep(10.25)
        return original_run()
    controller.run = delayed_run
    begin = time.monotonic()
    thread = start_background(controller)
    elapsed = time.monotonic() - begin
    try:
        if not 10.0 < elapsed < 15.0:
            raise RuntimeError("SPEC181_CONTROLLER_MARGIN_BOUNDARY_FAILED")
        if not controller._native.wait_until_ready(1):
            raise RuntimeError("SPEC181_CONTROLLER_LOST_READY")
    finally:
        controller.stop()
        thread.join(1.0)
    if thread.is_alive():
        raise RuntimeError("SPEC181_CONTROLLER_MARGIN_THREAD_NOT_COLLECTED")
    report("margin", readySeconds=elapsed, threadCollected=True)


def cancellation(controller):
    errors = []
    waiter = []
    def run():
        try:
            controller.run()
        except RuntimeError as error:
            errors.append(str(error))
    def wait():
        try:
            waiter.append(controller._native.wait_until_ready(15000))
        except RuntimeError as error:
            waiter.append(str(error))

    # The Controller's primary Face already captured the real NFD endpoint.
    # Its independent readiness Face connects to this local silent endpoint,
    # keeping the real Core probe pending until explicit cancellation.
    old_transport = os.environ.get("NDN_CLIENT_TRANSPORT")
    with tempfile.TemporaryDirectory(prefix="spec181-probe-") as directory:
        endpoint = str(Path(directory) / "silent.sock")
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        listener.bind(endpoint)
        listener.listen(1)
        listener.settimeout(8.0)
        os.environ["NDN_CLIENT_TRANSPORT"] = "unix://" + endpoint
        worker = threading.Thread(target=run)
        waiting = threading.Thread(target=wait)
        connection = None
        try:
            worker.start()
            connection, _ = listener.accept()  # proves the Core probe started
            waiting.start()
            wall_begin, cpu_begin = time.monotonic(), time.process_time()
            time.sleep(0.3)
            wall, cpu = time.monotonic() - wall_begin, time.process_time() - cpu_begin
            if cpu > wall * 0.5 + 0.01:
                raise RuntimeError("SPEC181_CONTROLLER_PROBE_BUSY_SPIN")
            begin = time.monotonic()
            controller.stop()
            worker.join(1.0)
            waiting.join(1.0)
            elapsed = time.monotonic() - begin
            if worker.is_alive() or waiting.is_alive() or elapsed > 1.0:
                raise RuntimeError("SPEC181_CONTROLLER_CANCELLATION_DID_NOT_DRAIN")
            expected = ("readiness cancelled", "event loop stopped")
            if not errors or any(not any(token in error for token in expected)
                                 for error in errors):
                raise RuntimeError("SPEC181_CONTROLLER_CANCELLATION_REASON_INVALID")
            if len(waiter) != 1 or waiter[0] is True or (
                    isinstance(waiter[0], str) and
                    not any(token in waiter[0] for token in expected)):
                raise RuntimeError("SPEC181_CONTROLLER_CANCEL_WAITER_INVALID")
            report("cancel", probeWallSeconds=wall, probeCpuSeconds=cpu,
                   stopSeconds=elapsed, threadCollected=True)
        finally:
            controller.stop()
            if connection is not None:
                connection.close()
            listener.close()
            worker.join(2.0)
            if waiting.ident is not None:
                waiting.join(2.0)
            if old_transport is None:
                os.environ.pop("NDN_CLIENT_TRANSPORT", None)
            else:
                os.environ["NDN_CLIENT_TRANSPORT"] = old_transport


def checked_start(self):
    mode = os.environ["SPEC181_LIFECYCLE_PROBE"]
    if mode == "provider-idle":
        thread = start_background(self)
        try:
            # Match the maintained Controller's PUBPARAMS freshness window
            # before another NAC consumer fetches the canonical parameters.
            time.sleep(6.0)
            provider_idle()
        finally:
            self.stop()
            thread.join(1.0)
        if thread.is_alive():
            raise RuntimeError("SPEC181_FIXTURE_CONTROLLER_NOT_COLLECTED")
    elif mode == "margin":
        margin(self)
    elif mode == "cancel":
        cancellation(self)
    else:
        raise RuntimeError("SPEC181_LIFECYCLE_PROBE_UNKNOWN")
    raise SystemExit(0)


ServiceController.start_background = checked_start
root = Path(__file__).resolve().parents[3]
controller_script = root / "examples/python/NDNSF-DistributedInference/yolo_2x2/controller.py"
sys.argv[0] = str(controller_script)
runpy.run_path(str(controller_script), run_name="__main__")
