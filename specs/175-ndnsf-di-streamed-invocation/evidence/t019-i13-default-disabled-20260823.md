# T019 I13 default-disabled lower-bound evidence

**Date**: 2026-08-23  
**Build**: current `Experimental` worktree, `/usr/bin/g++`, Boost 1.71, rebuilt `build/integration-tests`  
**Case**: `Spec170NdnsfDiCoreFlow/Spec175NativeTinyOnnxI13ProviderUnavailableAfterEvent3NoReplacement`

The focused process completed with exit code 0 and reported:

```text
requests=1 completed=0 failed=1 events=4 suppressed=1
error=stream event gap exceeded retry budget
```

The registered gate invocation also returned `PASS` with one result and wrote
`results/spec175/g2/i13-default-disabled-20260823.json`.

This proves the default-disabled side of the replacement contract at the
current Core fixture boundary: one public Request is published, the stream
does not silently create a second Request, and the terminal result is an
explicit failure rather than a duplicated or mixed transcript.

This is not yet the full I12/I13 acceptance evidence. The fixture cannot kill a
live Provider process at the exact event-3 publication boundary, and the
current lower-bound fault is deterministic loss/no-retry after the committed
prefix. The opt-in Python Coordinator path still requires a fresh-process
I12 proof with a second ACK/plan/Selection/token authority set and exact
continuation. T019 therefore remains open.
