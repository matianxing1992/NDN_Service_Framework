#!/bin/sh
set -eu

test_tmpdir()
{
  base=${TMPDIR:-/tmp}
  mktemp -d "$base/ndnsf-container-test.XXXXXX"
}

require_command()
{
  command -v "$1" >/dev/null 2>&1 || {
    echo "required test command unavailable: $1" >&2
    return 127
  }
}

assert_file()
{
  [ -f "$1" ] || {
    echo "expected file does not exist: $1" >&2
    return 1
  }
}

assert_not_contains()
{
  file=$1
  pattern=$2
  if grep -E "$pattern" "$file" >/dev/null 2>&1; then
    echo "unexpected pattern in $file: $pattern" >&2
    return 1
  fi
}
