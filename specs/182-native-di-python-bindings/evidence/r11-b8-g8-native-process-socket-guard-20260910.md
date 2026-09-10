# R11-B8-G8 Native Process Socket Guard

**Date**: 2026-09-10
**Status**: `CLOSED_FOR_VALIDATION` for fixture path handling
**Parent**: R11-B8 / T014-A

## Change

The shared native process fixture helper now keeps the normal `run-root/nfd.sock`
when its encoded pathname is safe and chooses a short per-process `/tmp`
pathname for deeply nested retained run roots. This prevents NFD's
`File name too long` startup boundary without changing the C++ request path.

## Validation

- `run-spec182-native-grant-process.py`, `run-spec182-native-stream-process.py`,
  and `run-spec182-native-unary-process.py` pass `py_compile` and whitespace
  checks.
- A direct boundary check confirms short roots remain colocated and long roots
  resolve to a pathname below the Unix-domain socket limit.
- The corrected long-root process behavior is recorded in
  [R11-B8-G7](r11-b8-g7-native-cross-process-revalidation-20260910.md).

This is a harness robustness fix only; it does not advance T014/T016 or alter
the native protocol implementation.
