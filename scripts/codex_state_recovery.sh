#!/usr/bin/env bash
set -Eeuo pipefail

# Recover a corrupt Codex log database without touching the active app-server.
# The operation is deliberately opt-in: run --check first, then --apply only
# after closing the full VS Code/Codex client.

codex_home="${CODEX_HOME:-${HOME:?}/.codex}"
codex_home=$(cd "$codex_home" 2>/dev/null && pwd -P) || {
  echo "CODEX_HOME does not exist: ${CODEX_HOME:-$HOME/.codex}" >&2
  exit 2
}

mode="check"
case "${1:-}" in
  "") ;;
  --check) mode="check" ;;
  --apply) mode="apply" ;;
  --help|-h)
    sed -n '1,24p' "$0"
    exit 0
    ;;
  *)
    echo "usage: $0 [--check|--apply]" >&2
    exit 2
    ;;
esac

db="$codex_home/logs_2.sqlite"
wal="$db-wal"
shm="$db-shm"
for path in "$db" "$wal" "$shm"; do
  if [[ -e "$path" ]]; then
    [[ -f "$path" ]] || { echo "refusing non-file path: $path" >&2; exit 2; }
  fi
done

owners=""
if command -v lsof >/dev/null 2>&1; then
  owners=$(lsof -t -- "$db" "$wal" "$shm" 2>/dev/null | sort -nu || true)
else
  for path in "$db" "$wal" "$shm"; do
    if command -v fuser >/dev/null 2>&1 && [[ -e "$path" ]]; then
      owners+="$(fuser "$path" 2>/dev/null || true)\n"
    fi
  done
  owners=$(printf '%b' "$owners" | tr ' ' '\n' | awk '/^[0-9]+$/' | sort -nu)
fi

if [[ -n "$owners" ]]; then
  echo "ACTIVE_APP_SERVER_OWNS_CODEX_LOG_DB"
  while read -r pid; do
    [[ -n "$pid" ]] || continue
    ps -o pid=,ppid=,stat=,cmd= -p "$pid" || true
  done <<< "$owners"
  echo "Close the full VS Code/Codex client, then rerun --check." >&2
  exit 3
fi

echo "CODEX_HOME=$codex_home"
echo "log_db=$db"
echo "owners=none"
if [[ "$mode" == check ]]; then
  echo "READY_FOR_APPLY=true"
  echo "No Codex process currently owns the log database trio."
  echo "Run --apply only after confirming the full client is closed."
  exit 0
fi

stamp=$(date -u +%Y%m%dT%H%M%SZ)
backup="$codex_home/recovery-backups/logs_2.sqlite-$stamp"
mkdir -p -m 700 "$backup"
for path in "$db" "$wal" "$shm"; do
  if [[ -e "$path" ]]; then
    mv -- "$path" "$backup/$(basename "$path")"
  fi
done
chmod 600 "$backup"/* 2>/dev/null || true
echo "MOVED_TO=$backup"
echo "Restart the full VS Code/Codex client so Codex can rebuild logs_2.sqlite."
echo "Then run: codex doctor"
