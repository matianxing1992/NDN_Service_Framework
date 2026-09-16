# Shared NDNSF YOLO Bundle

This guide is for running the local four-provider YOLO demo. It is written for
users who only need the runnable package, without the internal experiment
bookkeeping.

## Download

Download the SIF image from Google Drive:

[ndnsfdi.sif](https://drive.google.com/file/d/17yb0fC3erx1fUw_gCKwL0z8QCh_8ownY/view?usp=sharing)

An older SIF is also available for reference:

[legacy ndnsfdi.sif](https://drive.google.com/file/d/1sCgOzXscRLhXMq8paiBGJBmPn6rrWoNq/view?usp=sharing)

The legacy image is not the new build. The next SIF version is still under
development and has not been released; use the image explicitly identified by
the experiment owner.

Download the companion directory from the same restricted handoff location:

```text
ndnsf-share/
├── app_bundle/
├── test_case/
└── yolo_profile.json
```

The SIF is intentionally hosted outside GitHub because it is a large binary.
The application bundle is a build artifact, and `test_case` contains the model,
catalogue data, and test-only key material. Do not commit these files or make
the test package public.

For an integrity check, the expected values are:

```text
SIF SHA256:          089a4bc942db5fcc9d01ba2bf62b01ae1836416a232f0806a61931f26d946547
app_bundle digest:   92934b89e842483a12b6669cf935b62a1ec8b1750d1093f097ccc4cbcbc58f79
test_case digest:    4fbbc772cb4002b8104c012bbfc598159a638115d619db204ef285a0c4b14cf4
```

## Requirements

- Linux
- Apptainer 1.5.3
- Python 3
- MiniNDN/Mininet
- `sudo` access

## Setup

Clone this branch:

```bash
git clone --branch SPEC184Experiments --single-branch \
  https://github.com/matianxing1992/NDN_Service_Framework.git

cd NDN_Service_Framework
mkdir -p .codex-tmp
```

Copy the downloaded files into the repository:

```bash
cp /path/to/ndnsfdi.sif .codex-tmp/
cp -a /path/to/ndnsf-share/app_bundle .codex-tmp/
cp -a /path/to/ndnsf-share/test_case .codex-tmp/
cp /path/to/ndnsf-share/yolo_profile.json .codex-tmp/
```

If the profile contains paths from another machine, update only those local
paths so they point to `.codex-tmp/ndnsfdi.sif`, `.codex-tmp/app_bundle`, and
`.codex-tmp/test_case`. Keep the declared model, application, and checksum
values unchanged.

## Run

```bash
PROFILE="$PWD/.codex-tmp/yolo_profile.json"
RUN_ID=yolo-four-provider-replay
RUN_ROOT="$PWD/Experiments/TigerCluster/results/$RUN_ID"
CANDIDATE="$PWD/$RUN_ID.candidate.json"

python3 Experiments/TigerCluster/jobs/spec184/submit.py prepare \
  --profile "$PROFILE" \
  --run-id "$RUN_ID" \
  --candidate-output "$CANDIDATE" \
  --output "$PWD/$RUN_ID.prepare.json"

python3 Experiments/TigerCluster/jobs/spec184/submit.py check \
  --profile "$PROFILE" \
  --candidate "$CANDIDATE"

sudo -E python3 Experiments/TigerCluster/jobs/spec184/submit.py local \
  --profile "$PROFILE" \
  --candidate "$CANDIDATE" \
  --run-id "$RUN_ID" \
  --run-root "$RUN_ROOT"
```

The expected user-level result is:

```text
YOLO_ACK_DRIVEN_RESULT status=true
```

Evidence is written below:

```text
Experiments/TigerCluster/results/$RUN_ID/
```

This reproduces the local CPU/MiniNDN four-provider demo. It is not a
TigerCluster GPU run.
