#!/usr/bin/env bash
#SBATCH --nodes=2
#SBATCH --ntasks=2
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=1
#SBATCH --mem=1G
#SBATCH --time=00:05:00
set -euo pipefail
[[ $# == 1 && -f "$1" ]] || { echo 'expected manifest' >&2; exit 2; }
root=$(cd -- "$(dirname -- "$1")" && pwd)
exec /usr/bin/python3 "$root/bundle/tools/ndn_smoke.py" allocated "$1"
