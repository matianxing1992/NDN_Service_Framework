#!/usr/bin/env bash

TIGER_SSH_OPTIONS=(-o BatchMode=yes -o ConnectTimeout=12 -o StrictHostKeyChecking=yes)

tiger_ssh_target()
{
  local target=$1
  if [[ "$target" != *@* ]]; then
    target="${ITIGER_SSH_USER:-tma1}@$target"
  fi
  if [[ ! "$target" =~ ^[A-Za-z0-9_][A-Za-z0-9._-]*@[A-Za-z0-9_][A-Za-z0-9._-]*$ ]]; then
    printf '%s\n' 'Invalid Tiger SSH target' >&2
    return 2
  fi
  printf '%s\n' "$target"
}
