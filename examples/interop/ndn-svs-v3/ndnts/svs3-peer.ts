#!/usr/bin/env node

import fs from "node:fs";
import path from "node:path";
import { createHash } from "node:crypto";
import { openUplinks, closeUplinks } from "@ndn/cli-common";
import { createSigner, createVerifier, HMAC } from "@ndn/keychain";
import { AltUri, Name, type Signer, type Verifier } from "@ndn/packet";
import { DataArray } from "@ndn/repo-api";
import { SvPublisher, SvSubscriber, SvSync } from "@ndn/svs";

function arg(name: string, fallback = ""): string {
  const index = process.argv.indexOf(name);
  return index >= 0 && index + 1 < process.argv.length ? process.argv[index + 1] : fallback;
}

const mode = arg("--mode", "sync");
const version = arg("--version", "v3");
const syncPrefix = new Name(arg("--sync-prefix", "/ndn/svs-v3-interop"));
const nodePrefix = new Name(arg("--node-prefix", "/ndnts"));
const publishCount = Number(arg("--publish-count", "5"));
const intervalMs = Number(arg("--publish-interval-ms", "20"));
const startDelayMs = Number(arg("--start-delay-ms", "500"));
const settleMs = Number(arg("--settle-ms", "1500"));
const eventsPath = arg("--events", "/tmp/svs-v3-ndnts.jsonl");
const manifestPath = arg("--manifest", "");
const hmac = Buffer.from(
  arg("--hmac-key-base64", "c3BlYzExNC1wdWJsaWMtaG1hYy10ZXN0LWtleQ=="), "base64");

const output = fs.openSync(eventsPath, "w");
let outputOpen = true;

function record(values: Record<string, unknown>): void {
  if (!outputOpen) {
    return;
  }
  fs.writeSync(output, `${JSON.stringify({
    implementation: "ndnts-typescript",
    protocolVersion: version === "v3" ? 3 : 2,
    timestampNs: Number(process.hrtime.bigint()),
    ...values,
  })}\n`);
}

function event(kind: string, node: unknown, boot = 0, low = 0, high = 0,
               reason = ""): void {
  record({
    event: kind,
    nodeName: String(node),
    bootstrapTime: boot,
    low,
    high,
    reason,
  });
}

const sleep = (delay: number): Promise<void> =>
  new Promise((resolve) => setTimeout(resolve, delay));

interface PayloadCase {
  caseId: string;
  path: string;
  names: Record<"cpp" | "ndnts", string>;
  length: number;
  sha256: string;
  requiresSegmentation: boolean;
  segmentHint: number;
}

interface PayloadManifest {
  schemaVersion: string;
  applicationPrefix: string;
  cases: PayloadCase[];
}

function digest(payload: Uint8Array): string {
  return createHash("sha256").update(payload).digest("hex");
}

function segmentLowerBound(payloadLength: number): number {
  return Math.max(1, Math.ceil(payloadLength / 8800));
}

function loadPayloadManifest(filename: string): Array<PayloadCase & { payload: Uint8Array }> {
  if (!filename) {
    throw new Error("--manifest is required in payload mode");
  }
  const manifest = JSON.parse(fs.readFileSync(filename, "utf8")) as PayloadManifest;
  if (manifest.schemaVersion !== "spec117-payload-corpus-v1" || manifest.cases.length !== 4) {
    throw new Error("invalid Spec 117 payload manifest");
  }
  return manifest.cases.map((item) => {
    const payload = fs.readFileSync(path.resolve(path.dirname(filename), item.path));
    if (payload.byteLength !== item.length || digest(payload) !== item.sha256) {
      throw new Error(`manifest payload mismatch: ${item.caseId}`);
    }
    return { ...item, payload };
  });
}

async function createSync(signer: Signer, verifier: Verifier): Promise<SvSync> {
  return SvSync.create({
    syncPrefix,
    svs3: version === "v3",
    syncInterestLifetime: version === "v3" ? 1000 : 1,
    periodicTimeout: [30000, 0.1],
    suppressionPeriod: version === "v3" ? 200 : 500,
    signer,
    verifier,
  });
}

async function runSync(signer: Signer, verifier: Verifier): Promise<void> {
  const sync = await createSync(signer, verifier);
  event("startup", nodePrefix);
  sync.addEventListener("update", ({ id, loSeqNum, hiSeqNum }) => {
    event("update", id.name ?? id, id.boot ?? 0, loSeqNum, hiSeqNum);
  });
  sync.addEventListener("rxerror", ({ detail }) => {
    event("reject", nodePrefix, 0, 0, 0, String(detail?.[1] ?? "rxerror"));
  });

  const node = sync.add(nodePrefix);
  await sleep(startDelayMs);
  for (let seq = 1; seq <= publishCount; ++seq) {
    node.seqNum = seq;
    event("publish", node.id.name ?? node.id, node.id.boot ?? 0, seq, seq);
    await sleep(intervalMs);
  }
  await sleep(settleMs);
  event("final", node.id.name ?? node.id, node.id.boot ?? 0, 1, node.seqNum,
        JSON.stringify(sync.currentStateVector.toJSON()));
  for (const [id, seqNum] of sync.currentStateVector) {
    event("state", id.name ?? id, id.boot ?? 0, seqNum, seqNum);
  }
  sync.close();
  event("shutdown", nodePrefix);
}

async function runPayload(signer: Signer, verifier: Verifier): Promise<void> {
  if (version !== "v3") {
    throw new Error("payload mode requires SVS V3");
  }
  const cases = loadPayloadManifest(manifestPath);
  const expectedRemote = new Map(cases.map((item) => [item.names.cpp, item]));
  const received = new Set<string>();
  let sawRemoteSync = false;
  const sync = await createSync(signer, verifier);
  sync.addEventListener("update", ({ id, loSeqNum, hiSeqNum }) => {
    const remote = AltUri.ofName(id.name ?? id);
    if (remote === "/cpp") {
      sawRemoteSync = true;
    }
    record({ event: "sync-update", direction: "cpp-to-ndnts", stage: "sync",
             nodeName: remote, bootstrapTime: id.boot ?? 0,
             low: loSeqNum, high: hiSeqNum, reason: "" });
  });
  sync.addEventListener("rxerror", ({ detail }) => {
    record({ event: "error", direction: "cpp-to-ndnts", stage: "sync",
             reason: String(detail?.[1] ?? "rxerror") });
  });

  const subscriber = new SvSubscriber({
    sync,
    retxLimit: 2,
    innerVerifier: verifier,
    outerVerifier: verifier,
    mappingVerifier: verifier,
  });
  subscriber.addEventListener("error", ({ detail }) => {
    const reason = String(detail?.message ?? detail ?? "subscriber error");
    const stage = reason.includes("Mapping") || reason.includes("mapping") ? "mapping" :
                  reason.includes("dispatchUpdate") ? "outer-fetch" : "validation";
    record({ event: "error", direction: "cpp-to-ndnts", stage, reason });
  });
  const subscription = subscriber.subscribe(new Name("/ndnsf/svs-pubsub-interop/payload"));
  subscription.addEventListener("update", (update) => {
    const name = AltUri.ofName(update.name);
    const item = expectedRemote.get(name);
    const caseId = item?.caseId ?? "unknown";
    received.add(caseId);
    record({
      event: "receive",
      direction: "cpp-to-ndnts",
      caseId,
      name,
      sequence: update.seqNum,
      length: update.payload.byteLength,
      sha256: digest(update.payload),
      segments: segmentLowerBound(update.payload.byteLength),
      stage: "payload-check",
      reason: "",
    });
  });

  const publisher = new SvPublisher({
    sync,
    id: nodePrefix,
    store: new DataArray(),
    chunkSize: 4096,
    innerSigner: signer,
    outerSigner: signer,
    mappingSigner: signer,
  });
  record({ event: "startup", direction: "ndnts-to-cpp", stage: "sync",
           nodeName: AltUri.ofName(nodePrefix), reason: "" });
  await sleep(startDelayMs);
  for (const item of cases) {
    const sequence = await publisher.publish(new Name(item.names.ndnts), item.payload);
    record({
      event: "publish",
      direction: "ndnts-to-cpp",
      caseId: item.caseId,
      name: item.names.ndnts,
      sequence,
      length: item.length,
      sha256: item.sha256,
      segments: Math.max(1, Math.ceil(item.length / 4096)),
      stage: "publish",
      reason: "",
    });
    await sleep(intervalMs);
  }
  await sleep(settleMs);
  for (const item of cases) {
    if (!received.has(item.caseId)) {
      record({ event: "error", direction: "cpp-to-ndnts", caseId: item.caseId,
               stage: sawRemoteSync ? "mapping" : "sync",
               reason: "payload not received before bounded deadline" });
    }
  }
  await publisher.close();
  subscription[Symbol.dispose]();
  subscriber.close();
  sync.close();
  record({ event: "shutdown", direction: "ndnts-to-cpp", stage: "payload-check", reason: "" });
}

try {
  await openUplinks({ autoClose: false });
  const key = await HMAC.cryptoGenerate({ importRaw: hmac }, false);
  const signer = createSigner(HMAC, key);
  const verifier = createVerifier(HMAC, key);
  if (mode === "payload") {
    await runPayload(signer, verifier);
  }
  else if (mode === "sync") {
    await runSync(signer, verifier);
  }
  else {
    throw new Error(`unsupported --mode ${mode}`);
  }
  closeUplinks();
  outputOpen = false;
  fs.closeSync(output);
}
catch (caught: unknown) {
  const error = caught instanceof Error ? caught : new Error(String(caught));
  record({ event: mode === "payload" ? "error" : "reject",
           direction: mode === "payload" ? "unknown" : undefined,
           stage: mode === "payload" ? "orchestration" : undefined,
           nodeName: String(nodePrefix), reason: String(error.stack ?? error.message) });
  closeUplinks();
  outputOpen = false;
  fs.closeSync(output);
  console.error(error);
  process.exitCode = 2;
}
