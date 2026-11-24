#!/usr/bin/env python3
"""Consume the installed C++ user API; the C++ parent owns behavior assertions."""

import argparse
import gc
import hashlib
import json
from pathlib import Path
from threading import Event
import time
import weakref

from ndnsf import _ndnsf
from ndnsf_distributed_inference import api


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cancel-after-event", type=int, choices=(0, 3), default=0)
    parser.add_argument("--repository-input", action="store_true")
    args = parser.parse_args()
    receipt = {"events": [], "completed": [], "errors": [], "terminal_events": 0,
               "status": "NOT_STARTED", "first_failure": None,
               "cleanup_errors": [], "drained": False,
               "completion_probes": [], "completion_callbacks_alive": None}
    callback_references = []
    callback_releases = []
    runtime = None
    reader = None
    handle = None
    completion = None
    active_failure = False
    try:
        configuration = args.config.resolve()
        request = json.loads(configuration.read_text())["request"]
        config = api.RuntimeConfig()
        config.native_config_path = str(configuration)
        runtime = api.Runtime.open(config)
        user = runtime.user()
        model = user.prepare(timeout_s=30)
        options = api.RequestOptions()
        options.timeout_s = int(request["timeout_ms"]) / 1000.0
        options.ack_timeout_s = int(request["ack_timeout_ms"]) / 1000.0
        options.provider_names = request["provider_names"]
        options.output_mode = "TOKEN_STREAMING"
        options.stream = api.StreamOptions()
        # Preserve the exact operator options; C++ parses generation and IDs.
        application_options = (configuration.parent / request["options_file"]).read_bytes()
        if args.repository_input:
            reference = user.publish_input(model, list(args.input.read_bytes()), timeout_s=30)
            value = api.Input.repository(reference, list(application_options))
            del reference
        else:
            value = api.Input.inline_bytes(list(args.input.read_bytes()), list(application_options))
        handle = user.request(model, value, options=options)
        del value

        def on_completion(error, result):
            if error is not None:
                receipt["errors"].append(error)
            elif result is not None:
                receipt["completed"].append(json.loads(bytes(result.payload).decode("utf-8")))

        completion = handle.on_completion(on_completion)
        reader = handle.events()
        while True:
            event = reader.next()
            if event is None:
                break
            if event.terminal:
                receipt["terminal_events"] += 1
                continue
            receipt["events"].append(json.loads(bytes(event.payload).decode("utf-8")))
            if args.cancel_after_event and len(receipt["events"]) == args.cancel_after_event:
                # The reliable reader and cancellation share this thread.
                # This is three consumed events, not a claim about in-flight work.
                handle.cancel()
                break
        if not args.cancel_after_event:
            result = handle.result()
            args.output.write_bytes(bytes(result.payload))
        # Terminal subscriptions still dispatch through the native worker. Drop
        # the Python owner before delivery to exercise native callback ownership.
        for _ in range(16):
            delivered = Event()
            released = Event()

            def callback(error, result, delivered=delivered):
                receipt["completion_probes"].append(
                    {"error": error, "has_result": result is not None})
                delivered.set()

            callback_references.append(weakref.ref(callback))
            callback_releases.append(released)
            weakref.finalize(callback, released.set)
            subscription = handle.on_completion(callback)
            del callback
            if not delivered.wait(2):
                raise RuntimeError("native terminal callback did not arrive")
            subscription.unsubscribe()
            del subscription
    except BaseException as error:
        active_failure = True
        receipt["first_failure"] = type(error).__name__ + ": " + str(error)
        raise
    finally:
        if reader is not None:
            reader.close()
        if runtime is not None:
            try:
                runtime.close()
                receipt["drained"] = runtime.drain(timeout_s=6)
            except BaseException as error:
                receipt["cleanup_errors"].append("runtime: " + str(error))
        if handle is not None:
            receipt["status"] = handle.status_name
        # Retain the reliable completion subscription through native drain.
        if completion is not None:
            completion.cancel()
        gc.collect()
        release_deadline = time.monotonic() + 2
        for released in callback_releases:
            released.wait(max(0, release_deadline - time.monotonic()))
        receipt["completion_callbacks_alive"] = sum(
            reference() is not None for reference in callback_references)
        receipt["modules"] = {"binding": _ndnsf.__file__, "api": api.__file__}
        receipt["libraries"] = sorted({line.split()[-1] for line in
            Path("/proc/self/maps").read_text().splitlines() if any(name in line for name in (
                "libndn-service-framework.so", "libndnsf-distributed-inference.so",
                "libndnsf-uav-tracking.so"))})
        receipt["library_sha256"] = {path: hashlib.sha256(Path(path).read_bytes()).hexdigest()
                                     for path in receipt["libraries"]}
        (args.output.parent / "host-result.json").write_text(json.dumps(receipt, indent=2) + "\n")
        if not active_failure and (receipt["cleanup_errors"] or not receipt["drained"]):
            raise RuntimeError("installed host requester did not drain cleanly")


if __name__ == "__main__":
    main()
