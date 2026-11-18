#!/usr/bin/env python3
"""Assert the installed MiniMUAS browser adapter against its real native Job.

Run in the gs namespace through run_multicamera_tracking_demo.py. No inference,
authorization or tracking decisions are implemented here.
"""

import argparse
import hashlib
import json
import time
from pathlib import Path
from urllib.parse import parse_qs, urlparse

from playwright.sync_api import sync_playwright


def validate_presentation(snapshot, definition):
    display = snapshot.get("presentation")
    if not display or display["resultDigest"] != snapshot["lastResultDigest"]:
        return None
    assert display["generation"] == snapshot["generation"]
    encoded = display["resultJson"]
    assert "sha256:" + hashlib.sha256(encoded.encode()).hexdigest() == display["resultDigest"]
    result = json.loads(encoded)
    assert result["runId"] == snapshot["runId"] == definition["runId"]
    assert snapshot["missionId"] == definition["missionId"]
    assert result["missionId"] == snapshot["wireMissionId"] == "tracking-" + definition["runId"]
    assert result["windowId"] == "window-" + str(display["windowIndex"])
    assert len(display["frames"]) == len(result["frames"]) == 3
    descriptors = {source["cameraId"]: source for source in snapshot["sourceDescriptors"]}
    assert set(descriptors) == {"UAV1", "UAV2", "UAV3"}
    images = {image["cameraId"]: image for image in display["frames"]}
    assert set(images) == {"UAV1", "UAV2", "UAV3"}
    for frame in result["frames"]:
        image = images[frame["cameraId"]]
        source = next(source for source in definition["sources"] if source["cameraId"] == frame["cameraId"])
        assert image["sourceEpoch"] == source["sourceEpoch"]
        assert image["sourceId"] == source["sourceId"]
        assert image["producerIdentity"] == source["producerIdentity"]
        descriptor = descriptors[frame["cameraId"]]
        for field in ("sourceId", "sourceEpoch", "producerIdentity"):
            assert descriptor[field] == image[field]
        assert descriptor["deliveryMode"] == "replay" and descriptor["acquisitionKind"] == "measured"
        for field in ("sourceId", "sourceEpoch", "sequence", "width", "height", "inputDigest"):
            assert image[field] == frame[field], (field, image, frame)
        assert frame["observation"]["imageRef"]["name"] == image["frameName"]
    assert result["actualDetectorDevice"] == "cpu"
    has_tracks = any(frame.get("tracks") for frame in result["frames"])
    assert result["actualReidDevice"] == ("cpu" if has_tracks else "not-run")
    return display


def check_dom(page, display):
    # One browser turn: the UI may atomically replace the group between Python calls.
    cells = page.evaluate("""() => [...document.querySelectorAll('#tracking-cells > div')].map(c => ({
      camera: c.dataset.camera, window: c.dataset.windowIndex, digest: c.dataset.resultDigest,
      bounds: c.getBoundingClientRect().toJSON(),
      ready: c.querySelector('img').complete && c.querySelector('img').naturalWidth > 0,
      boxes: [...c.querySelectorAll('[data-global-id]')].map(b => ({
        id: b.dataset.globalId, bounds: b.getBoundingClientRect().toJSON()
      }))
    }))""")
    if len(cells) != 3 or any(cell["digest"] != display["resultDigest"] for cell in cells):
        return False
    assert all(cell["ready"] for cell in cells)
    result = json.loads(display["resultJson"])
    for frame in result["frames"]:
        matches = [cell for cell in cells if cell["camera"] == frame["cameraId"]]
        assert len(matches) == 1
        cell = matches[0]
        assert cell["window"] == str(display["windowIndex"])
        assert sorted(box["id"] for box in cell["boxes"]) == sorted(
            str(track["globalId"]) for track in frame["tracks"] if track.get("globalId") is not None)
        bounds = cell["bounds"]
        assert abs(bounds["width"] / bounds["height"] - frame["width"] / frame["height"]) < .03
        for track in frame["tracks"]:
            if track.get("globalId") is None:
                continue
            box = next(box["bounds"] for box in cell["boxes"] if box["id"] == str(track["globalId"]))
            x1, y1, x2, y2 = track["box"]
            assert abs((box["x"] - bounds["x"]) / bounds["width"] - x1 / frame["width"]) < .02
            assert abs((box["y"] - bounds["y"]) / bounds["height"] - y1 / frame["height"]) < .02
            assert abs(box["width"] / bounds["width"] - (x2 - x1) / frame["width"]) < .02
            assert abs(box["height"] / bounds["height"] - (y2 - y1) / frame["height"]) < .02
    return True


def check_frame_lease(page, args, definition, job_id, deadline):
    """Exercise the installed HTTP/native frame owner without competing GUI image reads."""
    base = args.url.rstrip("/")
    observed, reads = {}, []

    def snapshot():
        response = page.request.get(base + "/tracking/snapshot")
        assert response.status == 200
        value = response.json()
        assert value["jobId"] == job_id
        display = validate_presentation(value, definition)
        if display:
            observed[display["windowIndex"]] = display
        return value, display

    def frame(display, camera, status=200):
        image = next(image for image in display["frames"] if image["cameraId"] == camera)
        response = page.request.get(base + "/tracking/frame", params={
            "jobId": job_id, "generation": display["generation"],
            "windowIndex": display["windowIndex"], "resultDigest": display["resultDigest"],
            "camera": camera, "frameDigest": image["inputDigest"]})
        assert response.status == status, (response.status, status, display["windowIndex"], camera)
        if status == 200:
            assert "sha256:" + hashlib.sha256(response.body()).hexdigest() == image["inputDigest"]
        reads.append({"window": display["windowIndex"], "camera": camera, "status": status})

    def wait_for(predicate):
        while time.monotonic() < deadline:
            value, display = snapshot()
            if predicate(value, display):
                return value, display
            assert value["state"] not in {"FAILED", "CANCELLED"}, value
            page.wait_for_timeout(50)
        raise AssertionError("frame lease qualification stimulus NOT_COVERED before deadline")

    # Collapse before the first result. The ordinary UI must not consume this test's lease.
    page.locator("#tracking-panel > summary").click()
    _, first = wait_for(lambda value, display: display is not None)
    assert first["windowIndex"] == 0, "lease stimulus missed the first native window"
    started = time.monotonic()
    frame(first, "UAV1")
    frame(first, "UAV1")  # Repeated reads must not count as two distinct cameras.
    _, following = wait_for(lambda value, display: display and display["windowIndex"] > 0)
    frame(first, "UAV2")
    frame(first, "UAV3")
    elapsed = time.monotonic() - started
    assert elapsed < 5, "cross-window lease stimulus exceeded the existing five-second lease"
    frame(first, "UAV1", 409)
    # A fresh partial group must expire without its remaining cameras being read.
    assert following["windowIndex"] < args.windows - 1, "expiry stimulus missed a later native window"
    pinned = following
    frame(pinned, "UAV1")
    expires_after = time.monotonic() + 5.1
    final, last = wait_for(lambda value, display:
        time.monotonic() > expires_after and display and
        display["windowIndex"] > pinned["windowIndex"] and
        value["state"] == "COMPLETED" and value["cleanupState"] == "CLEAN")
    frame(pinned, "UAV2", 409)
    for camera in ("UAV1", "UAV2", "UAV3"):
        frame(last, camera)
    assert set(observed) == set(range(args.windows)), observed
    return final, {"observedWindows": sorted(observed), "frameReads": reads,
                   "crossWindowSeconds": elapsed,
                   "checks": ["repeated-camera-does-not-release-native-group",
                              "three-distinct-exact-jpegs-across-native-window-advance",
                              "three-camera-completion-releases-old-group",
                              "abandoned-group-expires-and-latest-group-remains-readable"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", required=True)
    parser.add_argument("--browser-executable", required=True)
    parser.add_argument("--job-config", type=Path, required=True)
    parser.add_argument("--expect", choices=("completed", "cancelled", "failed", "unconfigured", "early-eof"),
                        default="completed")
    parser.add_argument("--windows", type=int, default=4)
    parser.add_argument("--check-frame-lease", action="store_true")
    parser.add_argument("--check-empty-end", action="store_true")
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.check_frame_lease and (args.expect != "completed" or args.windows < 4):
        parser.error("frame lease check requires a completed scenario with at least four windows")
    if args.check_empty_end and (args.expect != "completed" or args.windows != 4 or args.check_frame_lease):
        parser.error("empty transition requires its own completed four-window scenario")
    definition = json.loads(args.job_config.read_text())
    deadline = time.monotonic() + args.timeout
    observed, rendered, presentations = {}, set(), {}
    slow_crossed = slow_rendered = False
    checks, byte_checks, byte_errors = [], {}, []
    with sync_playwright() as playwright:
        browser = playwright.firefox.launch(headless=True, executable_path=args.browser_executable)
        try:
            page = browser.new_page(viewport={"width": 1400, "height": 1000})
            hello, flight_commands = [], []

            def received(payload):
                if isinstance(payload, str):
                    message = json.loads(payload)
                    if message.get("type") == "hello":
                        hello[:] = [message]

            def sent(payload):
                if isinstance(payload, str):
                    message = json.loads(payload)
                    command = message.get("cmd")
                    if (command in {"flight", "all", "start_mission", "task_abort", "set_enabled",
                                    "candidate_promote", "candidate_dismiss", "sim", "system", "fabric_switch"}
                            or args.expect == "unconfigured" and
                            command in {"tracking_start", "tracking_cancel"}):
                        flight_commands[:] = [command]

            def connected(socket):
                socket.on("framereceived", received)
                socket.on("framesent", sent)

            page.on("websocket", connected)
            if args.expect == "completed" and not args.check_frame_lease:
                # Delay delivery to the UI, not native execution or server admission.
                page.add_init_script("""
                  const originalFetch = window.fetch.bind(window);
                  const slow = window.__spec226Slow = {digest:'', window:-1, started:0, pending:0, requests:0};
                  window.fetch = async (...args) => {
                    const url = new URL(typeof args[0] === 'string' ? args[0] : args[0].url, location.href);
                    const digest = url.searchParams.get('resultDigest');
                    if (url.pathname === '/tracking/frame') slow.requests++;
                    const delay = url.pathname === '/tracking/frame' && slow.started < 3 &&
                      (!slow.digest || slow.digest === digest);
                    if (delay) {
                      slow.digest = digest; slow.window = Number(url.searchParams.get('windowIndex'));
                      slow.started++; slow.pending++;
                    }
                    try {
                      const response = await originalFetch(...args);
                      if (delay && response.ok) await new Promise(resolve => setTimeout(resolve, 3500));
                      return response;
                    } finally { if (delay) slow.pending--; }
                  };
                """)

            def frame_response(response):
                if "/tracking/frame?" not in response.url or response.status != 200:
                    return
                query = parse_qs(urlparse(response.url).query)
                try:
                    body = response.body()
                except Exception:
                    return  # An aborted group provides no successful byte evidence.
                actual = "sha256:" + hashlib.sha256(body).hexdigest()
                if actual != query["frameDigest"][0]:
                    byte_errors.append(response.url)
                byte_checks[(query["resultDigest"][0], query["camera"][0])] = actual

            page.on("response", frame_response)
            page.goto(args.url, wait_until="domcontentloaded")
            if args.expect == "unconfigured":
                while not hello and time.monotonic() < deadline:
                    page.wait_for_timeout(100)
                assert hello and hello[0].get("tracking") is False, hello
                assert hello[0].get("vehicles"), "original hello has no vehicles"
                assert all(hello[0].get(key) is True for key in
                           ("bundle", "video_transport", "command_mode", "sim")), hello
                snapshot = page.request.get(args.url.rstrip("/") + "/tracking/snapshot")
                assert snapshot.status == 200 and snapshot.json() == {"state": "unconfigured"}
                assert page.locator("#tracking-panel").is_hidden()
                for selector in ("#map", "#videos", "#imageryBtn", "#previewBtn"):
                    assert page.locator(selector).is_visible(), selector
                assert page.locator("#videos .vidpanel").count() == len(hello[0]["vehicles"])
                assert page.get_by_role("button", name="Hold", exact=True).count() > 0
                assert not flight_commands, flight_commands
                args.output.parent.mkdir(parents=True, exist_ok=True)
                args.output.write_text(json.dumps({"status": "PASS", "nativeState": "unconfigured",
                    "checks": ["original-websocket-hello-without-tracking",
                               "tracking-hidden-and-original-map-video-controls-present",
                               "no-flight-abort-or-tracking-start-command-sent"],
                    "vehicles": hello[0]["vehicles"],
                    "coverage": "original UI entrypoints; no flight or ordinary-video device execution"},
                    indent=2) + "\n")
                return
            page.locator("#tracking-panel > summary").click()
            with page.expect_response(lambda response: response.url.endswith("/tracking/start")) as response:
                page.locator("#tracking-start").click()
            admission = response.value.json()
            assert response.value.status == 200 and admission["accepted"], admission
            job_id = admission["snapshot"]["jobId"]
            assert job_id == definition["jobId"]
            checks.append("original-browser-start-native-admission")
            duplicate = page.request.post(args.url.rstrip("/") + "/tracking/start", data={})
            assert duplicate.status == 409 and not duplicate.json()["accepted"]
            checks.append("duplicate-start-rejected-by-native-owner")
            # This is a real native guard through HTTP, not a substitute Job.
            rejected = page.request.post(args.url.rstrip("/") + "/tracking/cancel",
                                         data={"jobId": job_id + "-retired"})
            assert rejected.status == 409 and rejected.json()["cancelState"] == "WRONG_JOB"
            checks.append("noncurrent-cancel-rejected-by-native-job")
            if args.check_frame_lease:
                final, lease = check_frame_lease(page, args, definition, job_id, deadline)
                assert not flight_commands, flight_commands
                lease.update({"status": "PASS", "jobId": job_id,
                              "generation": final["generation"], "nativeState": final["state"],
                              "cancelState": final["cancelState"],
                              "cleanupState": final["cleanupState"],
                              "coverage": "installed HTTP/native frame lease; GUI group covered separately"})
                lease["checks"] = checks + lease["checks"] + ["no-flight-or-legacy-mission-command-sent"]
                args.output.parent.mkdir(parents=True, exist_ok=True)
                args.output.write_text(json.dumps(lease, indent=2) + "\n")
                return
            cancelled = False
            final = None
            while time.monotonic() < deadline:
                snapshot = page.request.get(args.url.rstrip("/") + "/tracking/snapshot").json()
                assert snapshot["jobId"] == job_id
                assert not byte_errors, byte_errors
                display = validate_presentation(snapshot, definition)
                if display:
                    presentations[display["windowIndex"]] = display
                    observed[display["windowIndex"]] = {
                        "digest": display["resultDigest"], "frames": display["frames"]}
                    if check_dom(page, display):
                        rendered.add(display["windowIndex"])
                if args.expect == "completed":
                    slow = page.evaluate("window.__spec226Slow")
                    if slow["pending"] and display and display["windowIndex"] > slow["window"]:
                        slow_crossed = True
                    first = presentations.get(slow["window"])
                    if first and check_dom(page, first):
                        slow_rendered = True
                if (args.expect == "cancelled" and not cancelled and
                        snapshot["state"] == "RUNNING" and snapshot["completedWindows"] >= 1 and
                        snapshot["activeRequestId"] and len(snapshot["selectedProviders"]) == 4 and
                        ("SPEC226_NATIVE_BACKEND_INVOKED window=window-" +
                         str(snapshot["completedWindows"])) in
                        args.output.with_name("compute.log").read_text()):
                    page.locator("#tracking-cancel").click()
                    cancelled = True
                target = {"completed": "COMPLETED", "cancelled": "CANCELLED",
                          "failed": "FAILED", "early-eof": "FAILED"}[args.expect]
                if snapshot["state"] == target and snapshot["cleanupState"] == "CLEAN":
                    final = snapshot
                    if args.expect != "completed" or len(observed) == args.windows:
                        break
                page.wait_for_timeout(100)
            assert final is not None, "native job did not reach expected CLEAN terminal state"
            page.wait_for_function(
                "state => document.getElementById('tracking-state').textContent === state",
                arg=final["state"], timeout=7000)
            if args.expect == "completed":
                assert set(observed) == set(range(args.windows)), observed
                assert slow_crossed, "qualification stimulus NOT_COVERED: no next native window within the delayed group"
                assert slow_rendered, "delayed UI group did not render intact"
                checks.append("slow-browser-group-crosses-native-window-and-renders-intact")
                display = validate_presentation(final, definition)
                if args.check_empty_end:
                    assert slow["window"] in {0, 1}, "empty transition missed an earlier person GUI group"
                    assert any(track.get("globalId") is not None
                               for frame in json.loads(presentations[slow["window"]]["resultJson"])["frames"]
                               for track in frame["tracks"]), "initial real input had no person IDs"
                    assert all(not frame["tracks"] for frame in json.loads(display["resultJson"])["frames"]), display
                    for _ in range(80):
                        if check_dom(page, display) and "global IDs: none" in page.locator("#tracking-ids").text_content():
                            break
                        page.wait_for_timeout(100)
                    else:
                        raise AssertionError("empty native window did not replace the original GUI group")
                    assert page.locator("#tracking-cells [data-global-id]").count() == 0
                    for image in display["frames"]:
                        assert byte_checks[(display["resultDigest"], image["cameraId"])] == image["inputDigest"]
                    assert not flight_commands, flight_commands
                    checks.extend(["real-person-window-followed-by-native-empty-window",
                                   "three-valid-exact-jpegs-and-old-gui-ids-cleared",
                                   "no-flight-or-legacy-mission-command-sent"])
                    page.screenshot(path=str(args.output.with_name("dashboard-empty.png")), full_page=True)
                    args.output.parent.mkdir(parents=True, exist_ok=True)
                    args.output.write_text(json.dumps({"status": "PASS", "checks": checks,
                        "jobId": job_id, "generation": final["generation"],
                        "nativeState": final["state"], "cleanupState": final["cleanupState"],
                        "observedWindows": observed, "renderedWindows": sorted(rendered),
                        "coverage": "real input transition; production detector outputs and original GUI"},
                        indent=2) + "\n")
                    return
                assert any(track.get("globalId") is not None
                           for frame in json.loads(display["resultJson"])["frames"]
                           for track in frame["tracks"]), "positive scenario did not exercise ID overlays"
                for width in (560, 1400):
                    page.set_viewport_size({"width": width, "height": 1000})
                    for _ in range(40):
                        if check_dom(page, display):
                            break
                        page.wait_for_timeout(100)
                    else:
                        raise AssertionError("final three-frame DOM group did not decode")
                    layout = page.locator("#tracking-cells > div").evaluate_all(
                        "cells => cells.map(cell => ({x:cell.getBoundingClientRect().x,y:cell.getBoundingClientRect().y}))")
                    if width < 700:
                        assert layout[0]["y"] < layout[1]["y"] < layout[2]["y"]
                        assert max(cell["x"] for cell in layout) - min(cell["x"] for cell in layout) < 1
                        for cell in page.locator("#tracking-cells > div").all():
                            cell.scroll_into_view_if_needed()
                    else:
                        assert layout[0]["x"] < layout[1]["x"] < layout[2]["x"]
                    page.screenshot(path=str(args.output.with_name("dashboard-" + str(width) + ".png")),
                                    full_page=True, timeout=10000)
                for image in display["frames"]:
                    assert byte_checks[(display["resultDigest"], image["cameraId"])] == image["inputDigest"]
                checks.extend(("four-native-cpu-window-bindings", "final-dom-three-images-ids-aspect-ratio-and-jpeg-digests"))
                details = page.locator("#tracking-details").text_content()
                assert display["resultDigest"] in details and "detector cpu | ReID cpu" in details
                for image in display["frames"]:
                    assert image["frameName"] in details and image["sourceId"] in details
                box = page.locator("#tracking-cells [data-global-id]").first
                chosen = box.get_attribute("data-global-id")
                box.press("Enter")
                selection = page.locator("#tracking-cells [data-global-id]").evaluate_all(
                    "boxes => boxes.map(box => ({id:box.dataset.globalId,pressed:box.getAttribute('aria-pressed'),text:box.textContent,color:box.style.borderColor}))")
                assert all(item["pressed"] == ("true" if item["id"] == chosen else "false") for item in selection)
                assert all("selected" in item["text"] for item in selection if item["id"] == chosen)
                assert len({item["color"] for item in selection if item["id"] == chosen}) == 1
                box.press("Enter")
                checks.append("source-provenance-and-keyboard-id-highlight")
                page.locator("#tracking-panel > summary").click()
                assert not page.locator("#tracking-panel").evaluate("panel => panel.open")
                assert page.get_by_role("button", name="Hold", exact=True).last.evaluate("""button => {
                  const r=button.getBoundingClientRect();
                  return document.elementFromPoint(r.x+r.width/2,r.y+r.height/2)?.closest('button') === button;
                }""")
                page.locator("#tracking-panel > summary").click()
                checks.append("tracking-disclosure-releases-existing-manual-controls")
                image = display["frames"][0]
                query = {"jobId": job_id, "generation": final["generation"],
                         "windowIndex": display["windowIndex"], "camera": image["cameraId"],
                         "resultDigest": "sha256:" + "0" * 64, "frameDigest": image["inputDigest"]}
                assert page.request.get(args.url.rstrip("/") + "/tracking/frame", params=query).status == 409
                query["resultDigest"] = display["resultDigest"]
                query["frameDigest"] = "sha256:" + "0" * 64
                assert page.request.get(args.url.rstrip("/") + "/tracking/frame", params=query).status == 409
                query["frameDigest"] = image["inputDigest"]
                query["generation"] = final["generation"] + 1
                assert page.request.get(args.url.rstrip("/") + "/tracking/frame", params=query).status == 409
                checks.append("wrong-frame-result-digest-rejected")
                # Reload/reconnect only observes the current owner. It cannot Start again.
                generation = final["generation"]
                page.reload(wait_until="domcontentloaded")
                page.locator("#tracking-panel > summary").click()
                restored = page.request.get(args.url.rstrip("/") + "/tracking/snapshot").json()
                assert restored["jobId"] == job_id and restored["generation"] == generation
                assert restored["completedWindows"] == args.windows
                checks.append("browser-reload-keeps-execution-identity")
                page.wait_for_function("window.__spec226Slow.pending > 0", timeout=10000)
                page.evaluate("""() => {
                  window.__spec226OldAdoption = false;
                  new MutationObserver(() => {
                    if (document.getElementById('tracking-cells').children.length === 3 &&
                        window.__spec226Slow.requests < 6) window.__spec226OldAdoption = true;
                  }).observe(document.getElementById('tracking-cells'), {childList:true});
                }""")
                page.context.set_offline(True)
                page.evaluate("ws.close()")
                page.wait_for_function("document.getElementById('tracking-state').textContent === 'offline' && document.getElementById('tracking-start').disabled && document.getElementById('tracking-cancel').disabled")
                page.context.set_offline(False)
                page.wait_for_function("document.getElementById('tracking-state').textContent === 'COMPLETED'", timeout=10000)
                page.wait_for_function("document.getElementById('tracking-cells').children.length === 3", timeout=10000)
                assert not page.evaluate("window.__spec226OldAdoption"), "disconnected pending group was adopted before a fresh fetch"
                restored = page.request.get(args.url.rstrip("/") + "/tracking/snapshot").json()
                assert restored["jobId"] == job_id and restored["generation"] == generation
                assert restored["completedWindows"] == args.windows
                checks.append("pending-group-disconnect-disables-controls-and-reconnect-refetches-same-job")
                with page.expect_response(lambda response: response.url.endswith("/tracking/start")) as rejected:
                    page.locator("#tracking-start").click()
                assert rejected.value.status == 409
                page.wait_for_function("document.getElementById('tracking-ids').textContent.includes('a new job requires')")
                page.wait_for_timeout(2200)
                assert "a new job requires" in page.locator("#tracking-ids").text_content()
                checks.append("native-admission-reason-remains-visible-across-refresh")
            elif args.expect == "cancelled":
                assert cancelled and final["cancelState"] == "REMOTE_CONFIRMED", final
                checks.append("active-browser-cancel-remote-confirmed-clean")
            elif args.expect == "early-eof":
                assert final["completedWindows"] == 2, final
                assert set(observed) == {0, 1}, observed
                assert final["failureStage"] not in {"authority", "deadline", "drain", "presentation"}, final
                assert "injected source produced no frame for the window" in final["reason"], final
                checks.append("two-real-windows-then-source-exhaustion-fails-cleanly")
            else:
                assert final["failureStage"] == "source-info", final
                assert final["reason"] == "source-info timeout for UAV3", final
                assert final["completedWindows"] == 0 and not final["hasResult"], final
                checks.append("missing-uav3-source-info-timeout-before-compute")
            assert not flight_commands, flight_commands
            checks.append("no-flight-or-legacy-mission-command-sent")
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps({"status": "PASS", "checks": checks,
                "jobId": job_id, "generation": final["generation"], "nativeState": final["state"],
                "cancelState": final["cancelState"], "cleanupState": final["cleanupState"],
                "observedWindows": observed, "renderedWindows": sorted(rendered),
                "coverage": ("four native windows and final DOM; intermediate GUI updates may be skipped"
                             if args.expect == "completed" else "native terminal state and original browser controls"),
                "frameByteChecks": [{"resultDigest": key[0], "camera": key[1], "digest": digest}
                                    for key, digest in byte_checks.items()]}, indent=2) + "\n")
            print(json.dumps({"status": "PASS", "checks": len(checks), "state": final["state"]}))
        finally:
            browser.close()


if __name__ == "__main__":
    main()
