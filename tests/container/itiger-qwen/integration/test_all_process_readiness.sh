#!/bin/bash
set -euo pipefail

repo=$(cd "$(dirname "$0")/../../../.." && pwd)
runner="$repo/packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/run-ndnsf-qwen.sh"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/bin" "$work/provider-args" "$work/scratch" "$work/evidence"

cat >"$work/bin/nfd" <<'EOF'
#!/bin/bash
exec python3 -c 'import os, signal, socket, time
transport=os.environ["NDN_CLIENT_TRANSPORT"]
path=transport[len("unix://"):] if transport.startswith("unix://") else transport
sock=socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
sock.bind(path)
sock.listen(1)
signal.signal(signal.SIGTERM, lambda *_: exit(0))
while True: time.sleep(1)'
EOF
cat >"$work/bin/nfdc" <<'EOF'
#!/bin/bash
if [[ ${1:-} == status && ${2:-} == report ]]; then echo fake-nfd-ready; fi
exit 0
EOF
chmod +x "$work/bin/nfd" "$work/bin/nfdc"

printf '%s\n' bash -c \
  "sleep 0.15; touch '$work/controller-ready'; echo 'ServiceController started...'; trap 'exit 0' TERM; while :; do sleep 1; done" \
  >"$work/controller.args"
printf '%s\n' bash -c \
  "echo NDNSF_DI_NATIVE_PROVIDER_READY; trap 'exit 0' TERM; while :; do sleep 1; done" \
  >"$work/provider-args/provider-0.args"
printf '%s\n' bash -c \
  "sleep 0.30; touch '$work/provider-1-ready'; echo NDNSF_DI_NATIVE_PROVIDER_READY; trap 'exit 0' TERM; while :; do sleep 1; done" \
  >"$work/provider-args/provider-1.args"
printf '%s\n' bash -c \
  "test -f '$work/controller-ready' && test -f '$work/provider-1-ready' && echo USER_AFTER_ALL_READY" \
  >"$work/user.args"
: >"$work/nfd.conf"

PATH="$work/bin:$PATH" SLURM_JOB_ID=spec109-readiness \
  "$runner" \
    --scratch "$work/scratch" \
    --evidence "$work/evidence" \
    --nfd-config "$work/nfd.conf" \
    --controller-args "$work/controller.args" \
    --provider-args-dir "$work/provider-args" \
    --user-args "$work/user.args"

grep -q USER_AFTER_ALL_READY "$work/evidence/user.log"
grep -q '"exitCode":0' "$work/evidence/orchestration-terminal.json"
test "$(find "$work/evidence" -name 'provider-*.log' | wc -l)" -eq 2
echo SPEC109_ALL_PROCESS_READINESS_PASS
