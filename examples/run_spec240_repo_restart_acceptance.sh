#!/usr/bin/env bash
# Spec 240 acceptance: publish original Data into a Repo store, stop the writer, restart
# the Repo on the same store and read the same original wire back with a new reader.
# The forwarder is restarted between reads so the content store cannot answer.
set -u

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${NDNSF_BUILD_DIR:-${repo_root}/build}"
seeder="${build_dir}/examples/seed-store"
server="${NDNSF_REPO_PACKET_SERVER:-/usr/local/bin/NDNSF_RepoPacketServer}"
peek="${NDNSF_NDNPEEK:-/usr/local/bin/ndnpeek}"
nfd_started="false"

wait_for_socket() {
  for _ in $(seq 1 300); do [[ -S /run/nfd/nfd.sock ]] && return 0; sleep 0.1; done
  return 1
}

stop_nfd() {
  sudo -n pkill -x nfd 2>/dev/null
  for _ in $(seq 1 200); do pgrep -x nfd >/dev/null || break; sleep 0.1; done
  rm -f /run/nfd/nfd.sock 2>/dev/null || true
}

start_nfd() {
  stop_nfd
  for attempt in 1 2 3; do
    sudo -n nfd-start >"${work}/nfd-${attempt}.log" 2>&1 &
    if wait_for_socket; then nfd_started="true"; return 0; fi
    stop_nfd
  done
  return 1
}

work="$(mktemp -d /tmp/spec240-acceptance.XXXXXX)"
store="${work}/store"
prefix="/ndnsf/spec240/accept/repo"
source_file="${work}/source.bin"
cleanup() {
  local pids
  pids="$(ps -eo pid,comm | awk '$2 ~ /^NDNSF_RepoPacke/ {print $1}')"
  # shellcheck disable=SC2086
  [[ -n "${pids}" ]] && kill ${pids} 2>/dev/null
  if [[ "${nfd_started}" == "true" ]]; then
    sudo -n pkill -x nfd 2>/dev/null
  fi
  rm -rf "${work}"
}
trap cleanup EXIT

[[ -x "${seeder}" ]] || { echo "missing seeder ${seeder}" >&2; exit 1; }
[[ -x "${server}" ]] || { echo "missing ${server}" >&2; exit 1; }
[[ -x "${peek}" ]] || { echo "missing ${peek}" >&2; exit 1; }

printf 'spec240 acceptance payload %s\n' "$(date +%s)" > "${source_file}"
mkdir -p "${store}"

"${seeder}" "${store}" "${prefix}" "${prefix}" "${source_file}" >"${work}/seed.log" 2>&1 || {
  echo "seed failed"; cat "${work}/seed.log"; exit 1; }

[[ -S /run/nfd/nfd.sock ]] || { start_nfd || { echo "NFD did not start"; exit 1; }; }

start_server() {
  setsid "${server}" "${store}" "${prefix}" 1048576 >"${work}/server-$1.log" 2>&1 &
  for _ in $(seq 1 200); do
    grep -q REPO_ORIGINAL_PACKETS_READY "${work}/server-$1.log" 2>/dev/null && return 0
    sleep 0.1
  done
  echo "server did not become ready"; cat "${work}/server-$1.log"; return 1
}

start_server first || exit 1
timeout 20 "${peek}" -f "${prefix}/object/seg=0" >"${work}/wire-1.bin" 2>/dev/null || {
  echo "first fetch failed"; exit 1; }

# Stop the writer and prove the process is gone before restarting.
server_pid="$(ps -eo pid,comm | awk '$2 ~ /^NDNSF_RepoPacke/ {print $1}')"
[[ -n "${server_pid}" ]] || { echo "server pid not found"; exit 1; }
kill "${server_pid}"
for _ in $(seq 1 100); do
  ps -eo comm | grep -q '^NDNSF_RepoPacke$' || break
  sleep 0.1
done
ps -eo comm | grep -q '^NDNSF_RepoPacke$' && { echo "server did not stop"; exit 1; }

# Restart the forwarder so its content store cannot serve the second read.
start_nfd || { echo "NFD did not restart"; exit 1; }

start_server second || exit 1
timeout 20 "${peek}" -f "${prefix}/object/seg=0" >"${work}/wire-2.bin" 2>/dev/null || {
  echo "second fetch failed"; exit 1; }
timeout 20 "${peek}" -p "${prefix}/object/seg=0" >"${work}/payload.bin" 2>/dev/null || {
  echo "payload fetch failed"; exit 1; }

status=0
cmp -s "${work}/wire-1.bin" "${work}/wire-2.bin" || { echo "FAIL wire differs"; status=1; }
cmp -s "${work}/payload.bin" "${source_file}" || { echo "FAIL payload differs"; status=1; }

echo "wire_bytes=$(wc -c <"${work}/wire-1.bin")"
echo "payload_bytes=$(wc -c <"${work}/payload.bin")"
echo "first_sha256=$(sha256sum "${work}/wire-1.bin" | cut -d' ' -f1)"
echo "second_sha256=$(sha256sum "${work}/wire-2.bin" | cut -d' ' -f1)"
[[ "${status}" == "0" ]] && echo "SPEC240_ACCEPTANCE_PASS"
exit "${status}"
