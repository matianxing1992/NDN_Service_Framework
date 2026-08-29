#!/usr/bin/env bash
set -euo pipefail

: "${SPEC175_SIF:?exact candidate SIF is required}"
: "${SPEC175_SIF_SHA256:?candidate SIF digest is required}"
: "${SPEC175_BUNDLE:?bundle directory is required}"
: "${SPEC175_OUTPUT:?output directory is required}"
: "${SPEC175_GATE:?gate name is required}"

# Model weights/tokenizer remain outside the immutable SIF.  Functional gates
# must opt into one content-addressed, read-only model root; the control gate
# has no model and therefore does not require this bind.
if [[ "$SPEC175_GATE" != control ]]; then
  : "${SPEC175_MODEL_ROOT:?external model root is required for this gate}"
  [[ -d "$SPEC175_MODEL_ROOT" ]] || {
    echo "SPEC175_MODEL_ROOT_MISSING" >&2
    exit 4
  }
fi

actual=$(sha256sum "$SPEC175_SIF" | awk '{print $1}')
[[ "$actual" == "${SPEC175_SIF_SHA256#sha256:}" ]] || {
  echo "SPEC175_SIF_DIGEST_MISMATCH" >&2
  exit 4
}
[[ -d "$SPEC175_BUNDLE" ]] || { echo "SPEC175_BUNDLE_MISSING" >&2; exit 4; }
mkdir -p "$SPEC175_OUTPUT"
binds="$SPEC175_BUNDLE:/bundle:ro,$SPEC175_OUTPUT:/evidence"
if [[ "$SPEC175_GATE" != control ]]; then
  binds="$binds,$SPEC175_MODEL_ROOT:/model:ro"
fi

# Provider children inherit the bundle cwd so relative model/tokenizer paths
# cannot resolve against the scheduler's submit directory.
set +e
apptainer exec --cleanenv \
  --env "SLURM_JOB_ID=${SLURM_JOB_ID:-spec175-local}" \
  --env "SPEC175_GATE=${SPEC175_GATE}" \
  --bind "$binds" \
  "$SPEC175_SIF" /bin/bash -lc '
    cd /bundle
    test -x /opt/ndnsf/bin/run-ndnsf-qwen.sh || {
      echo SPEC175_SIF_CONTROL_ENTRYPOINT_MISSING >&2
      exit 4
    }
    test -f /bundle/nfd.conf || { echo SPEC175_BUNDLE_NFD_CONFIG_MISSING >&2; exit 4; }
    test -f /bundle/controller.args || { echo SPEC175_BUNDLE_CONTROLLER_ARGS_MISSING >&2; exit 4; }
    test -d /bundle/providers || { echo SPEC175_BUNDLE_PROVIDER_ARGS_MISSING >&2; exit 4; }
    test -f /bundle/user.args || { echo SPEC175_BUNDLE_USER_ARGS_MISSING >&2; exit 4; }
    set +e
    /opt/ndnsf/bin/run-ndnsf-qwen.sh \
      --scratch /evidence/runtime \
      --evidence /evidence \
      --nfd-config /bundle/nfd.conf \
      --controller-args /bundle/controller.args \
      --provider-args-dir /bundle/providers \
      --user-args /bundle/user.args
    status=$?
    set -e
    printf "{\"schemaVersion\":\"spec175-sif-control-v1\",\"gate\":\"%s\",\"exitCode\":%d,\"status\":\"%s\"}\n" \
      "$SPEC175_GATE" "$status" "$([[ $status -eq 0 ]] && echo PASS || echo FAIL)" \
      >/evidence/spec175-gate-terminal.json
    exit "$status"
  '
status=$?
set -e
exit "$status"
