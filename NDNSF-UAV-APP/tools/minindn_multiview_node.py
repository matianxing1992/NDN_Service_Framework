#!/usr/bin/env python3
"""NDN wire processes used by the Spec 177 MiniNDN fixture.

The launcher starts this file in isolated MiniNDN namespaces.  It is a small
transport fixture, not a replacement for the NDNSF C++ authorization runtime:
all application messages contain compact references and metadata, while image
bytes are served only as named Data by the UAV producers.  Data packets use a
digest signature so the consumer can verify the exact packet it requested
without requiring a shared PIB/TPM in the test fixture.
"""

from __future__ import annotations

import argparse
import asyncio
from io import BytesIO
import hashlib
import json
from pathlib import Path
import tempfile
import time
from typing import Any, Dict, List, Optional, Tuple

from ndn.app import NDNApp
from ndn.encoding import Name
from ndn.security import DigestSha256Signer, KeychainDigest, sha256_digest_checker


JOB_SCHEMA = "ndnsf-uav-multiview-job/v1"
RESPONSE_SCHEMA = "ndnsf-uav-multiview-minindn-response/v1"
RESULT_SCHEMA = "ndnsf-uav-multiview-result/v1"
SIGNER = DigestSha256Signer()


def emit(event: str, **fields: Any) -> None:
    print(json.dumps({"event": event, **fields}, sort_keys=True), flush=True)


def digest_bytes(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def load_json(path: str) -> Any:
    return json.loads(Path(path).read_text(encoding="utf-8"))


def put(app: NDNApp, name: str, content: bytes, freshness_ms: int = 1000) -> None:
    app.put_data(name, content=content, signer=SIGNER,
                 freshness_period=freshness_ms)


def decode_content(content: Optional[bytes]) -> Dict[str, Any]:
    if content is None:
        raise ValueError("Data packet has no Content")
    value = json.loads(bytes(content).decode("utf-8"))
    if not isinstance(value, dict):
        raise ValueError("Data Content is not a JSON object")
    return value


def run_controller(args: argparse.Namespace) -> int:
    app = NDNApp(keychain=KeychainDigest())

    @app.route(args.prefix)
    def on_interest(name, _param, _app_param):
        put(app, Name.to_str(name), json.dumps({
            "schema": "ndnsf-uav-minindn-controller/v1",
            "controller": args.identity,
            "ready": True,
        }, sort_keys=True).encode("utf-8"))
        emit("CONTROLLER_DATA_PUBLISHED", exactDataName=Name.to_str(name))

    async def after_start() -> None:
        emit("CONTROLLER_READY", identity=args.identity)
        await asyncio.Event().wait()

    app.run_forever(after_start=after_start())
    return 0


def run_producer(args: argparse.Namespace) -> int:
    app = NDNApp(keychain=KeychainDigest())
    entries = load_json(args.names)
    if not isinstance(entries, list) or not entries:
        raise ValueError("producer names file must contain a non-empty list")
    by_name: Dict[str, Dict[str, Any]] = {
        str(entry["name"]): entry for entry in entries
        if isinstance(entry, dict) and entry.get("name")
    }

    @app.route(args.prefix)
    def on_interest(name, _param, _app_param):
        exact = Name.to_str(name)
        entry = by_name.get(exact)
        if entry is None or entry.get("available", True) is False:
            emit("DATA_UNAVAILABLE", producer=args.identity, exactDataName=exact)
            return

        async def publish() -> None:
            delay_ms = int(entry.get("delayMs", args.delay_ms))
            if delay_ms > 0:
                await asyncio.sleep(delay_ms / 1000.0)
            image_path = Path(args.fixture_root) / str(entry["file"])
            content = image_path.read_bytes()
            actual = digest_bytes(content)
            expected = str(entry.get("contentDigest", entry.get("sha256", "")))
            if expected and expected != actual:
                emit("DATA_DIGEST_ERROR", producer=args.identity,
                     exactDataName=exact, expected=expected, actual=actual)
                return
            put(app, exact, content, freshness_ms=args.freshness_ms)
            emit("DATA_PUBLISHED", producer=args.identity,
                 exactDataName=exact, contentDigest=actual, bytes=len(content))

        asyncio.create_task(publish())

    async def after_start() -> None:
        emit("PRODUCER_READY", identity=args.identity, names=sorted(by_name))
        await asyncio.Event().wait()

    app.run_forever(after_start=after_start())
    return 0


def _view_from_dict(value: Dict[str, Any]) -> Dict[str, Any]:
    # Accept the camelCase job schema and the snake_case fixture manifest.
    return {
        "viewId": str(value.get("viewId", value.get("view_id", ""))),
        "producerIdentity": str(value.get("producerIdentity", value.get("producer_identity", ""))),
        "exactDataName": str(value.get("exactDataName", value.get("exact_data_name", ""))),
        "contentDigest": str(value.get("contentDigest", value.get("content_digest", value.get("sha256", "")))),
        "captureTimeMs": int(value.get("captureTimeMs", value.get("capture_time_ms", 0))),
        "targetId": str(value.get("targetId", value.get("target_id", ""))),
        "mediaType": str(value.get("mediaType", value.get("media_type", "image/png"))),
    }


def _response(status: str, provider: str, job: Dict[str, Any], **fields: Any) -> bytes:
    value: Dict[str, Any] = {
        "schema": RESPONSE_SCHEMA,
        "status": status,
        "provider": provider,
        "missionSessionId": job.get("missionSessionId", ""),
        "jobId": job.get("jobId", ""),
        "attempt": job.get("attempt", 1),
    }
    value.update(fields)
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode("utf-8")


def _annotation_png(content: bytes, view_id: str, label: str) -> bytes:
    # Keep the network fixture truthful: annotations are actual PNG Data,
    # with a deterministic red border and a short label, not image bytes in a
    # request/response payload.
    from PIL import Image, ImageDraw
    image = Image.open(BytesIO(content)).convert("RGB")
    draw = ImageDraw.Draw(image)
    width, height = image.size
    border = max(2, min(width, height) // 128)
    draw.rectangle((0, 0, width - 1, height - 1), outline=(255, 32, 32), width=border)
    draw.text((4, 4), "%s %s" % (label, view_id), fill=(255, 32, 32))
    stream = BytesIO()
    image.save(stream, format="PNG", optimize=False)
    return stream.getvalue()


def _run_real_onnx_job(job: Dict[str, Any], accepted: List[Tuple[Dict[str, Any], bytes]],
                       args: argparse.Namespace) -> Tuple[Dict[str, Any], Dict[str, bytes]]:
    """Run the selected Provider's fetched views through the strict CPU graph."""
    from mvcnn_onnx_worker import run_real_manifest

    with tempfile.TemporaryDirectory(prefix="ndnsf-uav-mvcnn-") as directory:
        root = Path(directory)
        views = []
        for index, (view, raw) in enumerate(accepted):
            path = root / ("view-%02d.png" % index)
            path.write_bytes(raw)
            views.append({"viewId": view["viewId"], "file": str(path),
                          "sha256": view["contentDigest"],
                          "exactDataName": view["exactDataName"],
                          "producerIdentity": view["producerIdentity"],
                          "targetId": view.get("targetId", job.get("targetId", "")),
                          "mediaType": view.get("mediaType", "image/png")})
        result = run_real_manifest(
            {"views": views}, root / "result", model=args.model,
            model_digest=args.model_digest, profile_id=args.profile_id,
            registry=Path(args.model_registry), provider=args.identity,
            mission_id=str(job.get("missionSessionId", "")),
            job_id=str(job.get("jobId", "")), attempt=int(job.get("attempt", 1)),
            minimum_views=int(job.get("minimumViews", 2)))
        annotation_bytes = {
            item["viewId"]: Path(item["annotationFile"]).read_bytes()
            for item in result.get("annotatedViews", [])
        }
        return result, annotation_bytes


def run_provider(args: argparse.Namespace) -> int:
    app = NDNApp(keychain=KeychainDigest())
    jobs: Dict[str, Dict[str, Any]] = {}
    published_data: Dict[str, bytes] = {}

    @app.route(args.prefix + "/UAV/MULTIVIEW/MISSION")
    def on_published_data_interest(name, _param, _app_param):
        exact = Name.to_str(name)
        content = published_data.get(exact)
        if content is not None:
            put(app, exact, content, freshness_ms=5000)
            emit("PUBLISHED_DATA_SERVED", provider=args.identity,
                 exactDataName=exact, contentDigest=digest_bytes(content))

    async def fetch_job(job_name: str) -> Dict[str, Any]:
        if job_name not in jobs:
            _name, _meta, content = await app.express_interest(
                job_name, lifetime=args.lifetime_ms,
                validator=sha256_digest_checker)
            job = decode_content(content)
            if job.get("schema") != JOB_SCHEMA:
                raise ValueError("job schema mismatch")
            jobs[job_name] = job
        return jobs[job_name]

    @app.route(args.prefix + "/UAV/MULTIVIEW/ACK")
    def on_ack(name, _param, app_param):
        request_name = Name.to_str(name)

        async def handle() -> None:
            try:
                request = decode_content(app_param)
                job_name = str(request["jobDataName"])
                job = await fetch_job(job_name)
                put(app, request_name, _response(
                    "ack", args.identity, job, ready=True,
                    accepted=True, exactJobDataName=job_name,
                    supportedViewRange=[2, 6],
                    imageBytesInServicePayload=False))
                emit("ACK_PUBLISHED", provider=args.identity,
                     requestName=request_name, jobId=job.get("jobId"))
            except Exception as exc:
                emit("ACK_FAILED", provider=args.identity, requestName=request_name,
                     error="%s: %s" % (type(exc).__name__, exc))

        asyncio.create_task(handle())

    @app.route(args.prefix + "/UAV/MULTIVIEW/SELECTION")
    def on_selection(name, _param, app_param):
        selection_name = Name.to_str(name)

        async def handle() -> None:
            job: Optional[Dict[str, Any]] = None
            try:
                request = decode_content(app_param)
                job = await fetch_job(str(request["jobDataName"]))
                emit("PROVIDER_SELECTED", provider=args.identity,
                     jobId=job.get("jobId"), selected=True)
                views = [_view_from_dict(v) for v in job.get("views", [])]
                min_views = int(job.get("minimumViews", 2))
                if len(views) < min_views:
                    raise RuntimeError("insufficient-views")
                accepted: List[Tuple[Dict[str, Any], bytes]] = []
                seen_names = set()
                deadline = time.monotonic() + int(job.get("deadlineMs", 5000)) / 1000.0
                for view in views:
                    exact = view["exactDataName"]
                    if exact in seen_names:
                        raise RuntimeError("validation-failed: duplicate exactDataName")
                    seen_names.add(exact)
                    remaining_ms = max(1, int((deadline - time.monotonic()) * 1000))
                    try:
                        _data_name, _meta, content = await app.express_interest(
                            exact, lifetime=min(args.lifetime_ms, remaining_ms),
                            validator=sha256_digest_checker)
                        raw = bytes(content or b"")
                        actual = digest_bytes(raw)
                        if actual != view["contentDigest"]:
                            raise RuntimeError("validation-failed: digest mismatch")
                        accepted.append((view, raw))
                        emit("VIEW_DATA_FETCHED_VERIFIED", provider=args.identity,
                             jobId=job.get("jobId"), exactDataName=exact,
                             signer="digest-sha256", contentDigest=actual,
                             bytes=len(raw))
                    except Exception as exc:
                        emit("VIEW_DATA_FETCH_FAILED", provider=args.identity,
                             jobId=job.get("jobId"), exactDataName=exact,
                             error="%s: %s" % (type(exc).__name__, exc))
                        raise RuntimeError("delivery-timeout: %s" % exact)

                producers = {view["producerIdentity"] for view, _ in accepted}
                required_producers = int(job.get("minimumDistinctProducers", 2))
                if len(producers) < required_producers:
                    raise RuntimeError("correlation-failed: insufficient distinct producers")

                real_result: Optional[Dict[str, Any]] = None
                annotation_bytes: Dict[str, bytes] = {}
                if args.model_mode == "real":
                    real_result, annotation_bytes = _run_real_onnx_job(job, accepted, args)
                    if real_result.get("status") != "completed":
                        raise RuntimeError("inference-failed: %s" % real_result.get("failureStage", "unknown"))
                    emit("FUSION_COMPLETED", provider=args.identity,
                         jobId=job.get("jobId"), consumedViewCount=len(accepted),
                         poolingOperator=real_result.get("fusionEvidence", {}).get("operator"),
                         pooledFeatureDigest=real_result.get("fusionEvidence", {}).get("pooledFeatureDigest"),
                         viewMaskDigest=real_result.get("fusionEvidence", {}).get("viewMaskDigest"))
                    emit("INFERENCE_COMPLETED", provider=args.identity,
                         jobId=job.get("jobId"), modelProfileId=job.get("modelProfileId"),
                         algorithmId=real_result.get("model", {}).get("algorithmId"),
                         executionProviders=real_result.get("model", {}).get("executionProviders"),
                         decision=real_result.get("fusedDecision", {}).get("label"),
                         cpuInferenceMs=real_result.get("model", {}).get("cpuInferenceMs"))
                else:
                    emit("FUSION_COMPLETED", provider=args.identity,
                         jobId=job.get("jobId"), consumedViewCount=len(accepted),
                         poolingOperator="elementwise-max/v1")
                    emit("INFERENCE_COMPLETED", provider=args.identity,
                         jobId=job.get("jobId"), modelProfileId=job.get("modelProfileId"),
                         decision="car")

                if args.scenario == "publication-failure":
                    emit("ANNOTATION_PUBLICATION_FAILED", provider=args.identity,
                         jobId=job.get("jobId"), reason="injected publication failure")
                    put(app, selection_name, _response(
                        "rejected", args.identity, job,
                        terminalStatus="annotation-publication-failed",
                        failureStage="annotation-publication"))
                    return

                annotation_refs: List[Dict[str, Any]] = []
                for view, raw in accepted:
                    annotation_name = (
                        "%s/UAV/MULTIVIEW/MISSION/%s/JOB/%s/ANNOTATION/%s/1/1"
                        % (args.identity.rstrip("/"), job["missionSessionId"],
                           job["jobId"], view["viewId"])
                    )
                    annotated = (annotation_bytes.get(view["viewId"], b"")
                                 if real_result is not None else
                                 _annotation_png(raw, view["viewId"], "car"))
                    if not annotated:
                        raise RuntimeError("annotation-publication-failed: missing model annotation")
                    published_data[annotation_name] = annotated
                    annotation_refs.append({
                        "viewId": view["viewId"],
                        "sourceDataName": view["exactDataName"],
                        "sourceDigest": view["contentDigest"],
                        "exactDataName": annotation_name,
                        "contentDigest": digest_bytes(annotated),
                        "signerIdentity": args.identity,
                        "mediaType": "image/png",
                    })
                    emit("ANNOTATION_PUBLISHED", provider=args.identity,
                         jobId=job.get("jobId"), exactDataName=annotation_name,
                         sourceDataName=view["exactDataName"],
                         contentDigest=digest_bytes(annotated))

                result_name = (
                    "%s/UAV/MULTIVIEW/MISSION/%s/JOB/%s/RESULT/1/1"
                    % (args.identity.rstrip("/"), job["missionSessionId"], job["jobId"])
                )
                if real_result is not None:
                    result = dict(real_result)
                    result["schema"] = RESULT_SCHEMA
                    result["resultManifest"] = {"exactDataName": result_name}
                    result["annotatedViews"] = annotation_refs
                    result["terminalOwner"] = args.identity
                else:
                    pooled = hashlib.sha256()
                    for view, raw in accepted:
                        pooled.update(view["viewId"].encode("utf-8"))
                        pooled.update(view["contentDigest"].encode("utf-8"))
                        pooled.update(raw)
                    result = {
                        "schema": RESULT_SCHEMA,
                        "missionSessionId": job["missionSessionId"],
                        "jobId": job["jobId"],
                        "attempt": job.get("attempt", 1),
                        "status": "completed",
                        "fusedDecision": {"label": "car", "confidence": 0.5},
                        "model": {"profileId": job.get("modelProfileId", "functional-adapter-v1"),
                                  "algorithmId": "detector-guided-mvcnn-pooling/v1",
                                  "modelDigest": args.model_digest},
                        "contributingViews": [view["viewId"] for view, _ in accepted],
                        "rejectedViews": [],
                        "fusionEvidence": {
                            "operator": "elementwise-max/v1",
                            "consumedViewCount": len(accepted),
                            "pooledFeatureDigest": "sha256:" + pooled.hexdigest(),
                        },
                        "annotatedViews": annotation_refs,
                        "terminalOwner": args.identity,
                        "scientificAccuracyClaimAllowed": False,
                    }
                result_bytes = json.dumps(result, sort_keys=True,
                                          separators=(",", ":")).encode("utf-8")
                published_data[result_name] = result_bytes
                emit("RESULT_PUBLISHED", provider=args.identity,
                     jobId=job.get("jobId"), exactDataName=result_name,
                     contentDigest=digest_bytes(result_bytes),
                     consumedViewCount=len(accepted))
                put(app, selection_name, _response(
                    "completed", args.identity, job,
                    terminalStatus="completed", resultDataName=result_name,
                    annotationCount=len(annotation_refs),
                    annotatedViews=annotation_refs,
                    imageBytesInServicePayload=False))
                emit("RESPONSE_PUBLISHED", provider=args.identity,
                     jobId=job.get("jobId"), terminalStatus="completed",
                     resultDataName=result_name,
                     imageBytesInServicePayload=False)
            except Exception as exc:
                if job is None:
                    job = {"missionSessionId": "", "jobId": ""}
                reason = str(exc)
                status = ("delivery-timeout" if reason.startswith("delivery-timeout")
                          else "inference-failed" if reason.startswith("inference-failed")
                          else "validation-failed")
                put(app, selection_name, _response(
                    "rejected", args.identity, job, terminalStatus=status,
                    failureStage="fetch-or-validation", reason=reason,
                    imageBytesInServicePayload=False))
                emit("RESPONSE_PUBLISHED", provider=args.identity,
                     jobId=job.get("jobId"), terminalStatus=status,
                     reason=reason, imageBytesInServicePayload=False)

        asyncio.create_task(handle())

    async def after_start() -> None:
        emit("PROVIDER_READY", identity=args.identity, prefix=args.prefix,
             scenario=args.scenario)
        await asyncio.Event().wait()

    app.run_forever(after_start=after_start())
    return 0


def run_coordinator(args: argparse.Namespace) -> int:
    app = NDNApp(keychain=KeychainDigest())
    job = load_json(args.job)
    if not isinstance(job, dict) or job.get("schema") != JOB_SCHEMA:
        raise ValueError("job file does not contain the Spec 177 job schema")
    job_data_name = str(args.job_data_name)
    selected_provider = args.provider.rstrip("/")
    all_providers = [p.rstrip("/") for p in args.providers.split(",") if p.strip()]

    job_bytes = json.dumps(job, sort_keys=True, separators=(",", ":")).encode("utf-8")

    @app.route(job_data_name)
    def on_job_interest(name, _param, _app_param):
        exact = Name.to_str(name)
        if exact == job_data_name:
            put(app, exact, job_bytes, freshness_ms=5000)
            emit("REQUEST_DATA_SERVED", exactDataName=exact,
                 contentDigest=digest_bytes(job_bytes))

    async def fetch(name: str, **kwargs: Any) -> Tuple[str, Any, Optional[bytes]]:
        return await app.express_interest(name, validator=sha256_digest_checker, **kwargs)

    async def after_start() -> None:
        try:
            emit("REQUEST_PUBLISHED", missionSessionId=job["missionSessionId"],
                 jobId=job["jobId"], requestDataName=job_data_name,
                 imageBytesInServicePayload=False)

            async def ack(provider: str) -> Tuple[str, Optional[Dict[str, Any]], str]:
                name = "%s/UAV/MULTIVIEW/ACK/%s/1" % (provider, job["jobId"])
                params = json.dumps({"jobDataName": job_data_name},
                                    separators=(",", ":")).encode("utf-8")
                try:
                    _name, _meta, content = await fetch(
                        name, app_param=params, lifetime=args.lifetime_ms)
                    value = decode_content(content)
                    emit("ACK_FETCHED", provider=provider, jobId=job["jobId"],
                         accepted=value.get("accepted", False))
                    return provider, value, ""
                except Exception as exc:
                    emit("ACK_FETCH_FAILED", provider=provider, jobId=job["jobId"],
                         error="%s: %s" % (type(exc).__name__, exc))
                    return provider, None, "%s: %s" % (type(exc).__name__, exc)

            ack_results = await asyncio.gather(*(ack(provider) for provider in all_providers))
            eligible = [provider for provider, value, _error in ack_results
                        if value and value.get("accepted")]
            emit("ACK_MATCHED", jobId=job["jobId"], providers=eligible)
            if selected_provider not in eligible:
                raise RuntimeError("no selected Provider returned a valid ACK")
            emit("PROVIDER_SELECTED", jobId=job["jobId"], provider=selected_provider,
                 eligibleProviders=eligible)
            selection_name = "%s/UAV/MULTIVIEW/SELECTION/%s/1" % (
                selected_provider, job["jobId"])
            params = json.dumps({"jobDataName": job_data_name},
                                separators=(",", ":")).encode("utf-8")
            _name, _meta, content = await fetch(
                selection_name, app_param=params, lifetime=args.selection_lifetime_ms)
            response = decode_content(content)
            terminal = str(response.get("terminalStatus", ""))
            if terminal == "completed":
                result_name = str(response["resultDataName"])
                _result_wire_name, _result_meta, result_content = await fetch(
                    result_name, lifetime=args.lifetime_ms)
                result = decode_content(result_content)
                if result.get("terminalOwner") != selected_provider:
                    raise RuntimeError("terminal result owner mismatch")
                emit("RESULT_FETCHED_VERIFIED", provider=selected_provider,
                     jobId=job["jobId"], exactDataName=result_name,
                     contentDigest=digest_bytes(bytes(result_content or b"")))
                for annotation in result.get("annotatedViews", []):
                    annotation_name = str(annotation["exactDataName"])
                    _annotation_wire_name, _annotation_meta, annotation_content = await fetch(
                        annotation_name, lifetime=args.lifetime_ms)
                    actual = digest_bytes(bytes(annotation_content or b""))
                    if actual != annotation.get("contentDigest"):
                        raise RuntimeError("annotation digest mismatch")
                    emit("ANNOTATION_FETCHED_VERIFIED", provider=selected_provider,
                         jobId=job["jobId"], exactDataName=annotation_name,
                         contentDigest=actual)
                emit("TERMINAL_ACCEPTED", jobId=job["jobId"],
                     terminalOwner=selected_provider,
                     consumedViewCount=len(result.get("contributingViews", [])),
                     annotationCount=len(result.get("annotatedViews", [])),
                     imageBytesInServicePayload=False)
                summary = {
                    "status": "completed", "terminalOwner": selected_provider,
                    "jobId": job["jobId"], "resultDataName": result_name,
                    "acceptedViewCount": len(result.get("contributingViews", [])),
                    "annotationCount": len(result.get("annotatedViews", [])),
                    "eligibleProviders": eligible,
                    "imageBytesInServicePayload": False,
                }
            else:
                emit("TERMINAL_REJECTED", jobId=job["jobId"],
                     terminalOwner=selected_provider, terminalStatus=terminal,
                     failureStage=response.get("failureStage"),
                     reason=response.get("reason"))
                summary = {
                    "status": "rejected", "terminalOwner": selected_provider,
                    "jobId": job["jobId"], "terminalStatus": terminal,
                    "failureStage": response.get("failureStage"),
                    "eligibleProviders": eligible,
                    "imageBytesInServicePayload": False,
                }
            if args.summary:
                Path(args.summary).write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n",
                                              encoding="utf-8")
            emit("COORDINATOR_COMPLETE", **summary)
        except Exception as exc:
            summary = {"status": "failed", "jobId": job.get("jobId"),
                       "error": "%s: %s" % (type(exc).__name__, exc)}
            if args.summary:
                Path(args.summary).write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n",
                                              encoding="utf-8")
            emit("COORDINATOR_FAILED", **summary)
        finally:
            app.shutdown()

    app.run_forever(after_start=after_start())
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=("controller", "producer", "provider", "coordinator"), required=True)
    parser.add_argument("--identity", required=True)
    parser.add_argument("--prefix", default="/")
    parser.add_argument("--names", default="")
    parser.add_argument("--fixture-root", default=".")
    parser.add_argument("--delay-ms", type=int, default=0)
    parser.add_argument("--freshness-ms", type=int, default=1000)
    parser.add_argument("--lifetime-ms", type=int, default=1000)
    parser.add_argument("--selection-lifetime-ms", type=int, default=6000)
    parser.add_argument("--scenario", default="nominal")
    parser.add_argument("--model", default="")
    parser.add_argument("--model-registry", default=str(Path(__file__).resolve().parents[2] /
                                                           "NDNSF-UAV-APP/configs/uav_multiview_models.json"))
    parser.add_argument("--profile-id", default="vehicle-mvcnn-v1")
    parser.add_argument("--model-mode", choices=("functional", "real"), default="functional")
    parser.add_argument("--model-digest", default="sha256:fixture-model")
    parser.add_argument("--job", default="")
    parser.add_argument("--job-data-name", default="/example/uav/gs/UAV/MULTIVIEW/REQUEST/recognition-001/1")
    parser.add_argument("--provider", default="/provider/gpu")
    parser.add_argument("--providers", default="/provider/gpu,/provider/cpu")
    parser.add_argument("--summary", default="")
    args = parser.parse_args()
    if args.mode == "controller":
        return run_controller(args)
    if args.mode == "producer":
        if not args.names:
            parser.error("--names is required for producer")
        return run_producer(args)
    if args.mode == "provider":
        return run_provider(args)
    if not args.job:
        parser.error("--job is required for coordinator")
    return run_coordinator(args)


if __name__ == "__main__":
    raise SystemExit(main())
