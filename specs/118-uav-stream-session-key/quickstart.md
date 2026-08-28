# Validation Quickstart: UAV Stream Session-Key Delivery

This is the reproducible implementation acceptance guide. Completion status and
measured outcomes are recorded in `completion-summary.md`.

## 1. Focused Contract And Crypto Gate

First require the app-neutral Spec 119 API gate:

```bash
python3 tests/python/test_ndnsf_live_stream_minindn.py
```

UAV integration is inadmissible if it implements a local Mapping fetcher,
cursor-name payload loop, independent Face pump, controller, or producer
pending/FEC table instead of using `createLiveStream`/`openLiveStream` handles.
The Spec 119 gate must already prove FEC-off and opaque-byte XOR recovery without
any encryption key or UAV type in Core.

```bash
./waf build --targets=unit-tests
./build/unit-tests --run_test=UavProtocolState
python3 tests/run_uav_stream_security_contract.py
python3 tests/python/test_ndnsf_uav_stream_control_isolation_campaign.py
```

Required outcomes:

- deterministic key/salt/nonce/AAD/envelope vectors pass;
- successful start fields decode to exactly 32 key bytes and 4 salt bytes;
- stop/status/failure/unrelated responses contain no secret fields;
- descriptor contract-version and five-frontier ordering vectors pass;
- start success waits for at least three measured publication groups, a
  Mapping-covered H264 decoder-reset join, and bounded readiness; no-data and
  non-decodable-join cases fail before key disclosure;
- fixed-capacity single-Data Mapping vectors resolve
  `block=floor(cursor/B)` and slot `cursor mod B` to original names;
- wrong Mapping name/signer identity/session/version/range/continuity,
  conflicting binding, same-name equivocation, unauthorized/unregistered
  original namespace, stale cross-session cache, and late Mapping are classified;
- predicted-but-unproduced names remain immutable and never become tombstones;
- wrong payload name/signer/session/epoch/nonce/tag/ciphertext fails closed;
- the UAV trust configuration rejects Mapping/payload signed by a different
  otherwise-valid Provider identity;
- duplicate Interest reuses byte-identical ciphertext/signed Content, while
  rollback/overflow/eviction cannot reuse a nonce;
- no plaintext fallback reaches VideoPacket/decoder code;
- source bytes are AES-GCM envelopes before `publish`/`publishGroup`; ordinary
  and `FecRecovered` items run the same UAV AEAD/replay gate afterward;
- one-loss recovery succeeds byte-for-byte, while corrupt/two-loss/wrong-signer/
  wrong-digest/expired groups update no decrypt/decoder state;
- no UAV-owned parity generation or `FecFrameState` XOR recovery authority
  remains after migration.
- a control-only campaign cell treats LiveStream prefetch policy as explicitly
  not applicable, while retaining all control-command, convergence, lifecycle,
  and secret-handling gates.

## 2. Existing NDNSF Non-Regression

```bash
./build/unit-tests --run_test=GenericDynamicApi/TargetedInvocation
./build/unit-tests --run_test=GenericDynamicApi/TokensAndReplay
examples/run_hello_auth_regression.sh
examples/run_token_handshake_negative_regression.sh
```

The two shell scripts require a running host/default NFD. They are diagnostic
helpers, not the final network acceptance path. Do not start a host NFD only to
claim this gate: use the two unit suites above plus the MiniNDN UAV cells for
final normal/Targeted network evidence when `/run/nfd/nfd.sock` is absent.

Required outcomes:

- generic `ResponseMessage` wire and APIs are unchanged;
- Spec 119 `StreamNameMap` codec/resolver tests pass without another RPC path;
- normal and Targeted service calls retain authorization, tokens, replay
  protection, and terminal timeout/response behavior.

## 3. Secret-Handling Gate

Run contract fixtures for successful start, failed start, stop, already-stopped,
status, telemetry, and unrelated services. Scan produced logs/status/evidence:

```bash
python3 tests/run_uav_stream_security_contract.py --secret-scan
```

Required outcome: zero key or nonce-salt matches outside the intended decrypted
start-response test object and bounded process memory.

## 4. MiniNDN Acceptance

Run the existing UAV stream campaign, which creates the MiniNDN topology for
each requested loss cell and delegates to the GUI/headless harness. Execute one
60-second measured window at each required loss setting:

```bash
timeout 480s python3 Experiments/NDNSF_UAV_Stream_Parity_Campaign.py \
  --out results/spec118-uav-session-key-acceptance-20260718-candidate3 \
  --runs 1 \
  --loss-percentages 0,5 \
  --fec-parity-shards 1 \
  --auto-stop-seconds 60
```

Candidate 2 is retained as a security failure because persisted logs exposed
the key and nonce salt. Candidate 3 is a new identity after removing plaintext
response logging and adding a persisted-log scan; it is not a rerun under the
failed identity.

`NDNSF_UAV_Stream_Parity_Campaign.py` owns the generated `loss=0` and `loss=5`
topologies; `NDNSF_UAV_GUI_Minindn.py` intentionally has no independent loss
option. Do not add a duplicate launcher knob.

Each run must preserve:

- encrypted versioned five-frontier start descriptor accepted once;
- every accepted payload name came from an accepted Provider-signed Mapping;
- Mapping lead/late, conflict, tombstone, and pending maxima counters;
- the Provider can serve the complete Mapping chain for every advertised
  retained payload; required block eviction atomically advances the retained
  frontier and a repeated start reports the refreshed checkpoint snapshot;
- each Mapping block fits one signed Data packet under the configured/8800-byte
  cap, and payload/Mapping roots were registered before descriptor visibility;
- Provider signature validation before every accepted packet decrypt;
- Provider-signed FEC group/digest validation before any recovered ciphertext
  reaches UAV decryption, with FEC disabled and enabled outcomes distinguished;
- accepted/decryption-failed/validation-failed/replay counters;
- bounded producer, consumer, decoder, and forwarder PIT state;
- a far-future valid-name flood cannot starve nearer/deadline payloads in the
  application pending table; NFD PIT occupancy is separately deployment-gated;
- decoded video evidence without plaintext fallback;
- every scheduled control request reaches response or configured timeout;
- no Stream keys/salts in persisted evidence.

The capture must show original semantic payload Data names. A run that silently
returns to `<stream_prefix>/<packetSeq>` is a contract failure even if video
decodes.

The 5% run may preserve a negative video/FEC result; it must not be rerun or
tuned to manufacture improvement. Security failure or plaintext fallback is an
unconditional failure at either loss setting.

## 5. Evidence Classification

The completion summary separates:

- implemented: code exists;
- wired: producer and consumer integration exists;
- executed: real MiniNDN path ran;
- measured: 60-second artifacts and counters exist;
- deferred: container, iTiger, real UAV, and long-term performance.
