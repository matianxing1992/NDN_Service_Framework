# T004 Collaboration Progress Integration Evidence

Status: PASS.

`pythonWrapper/ndnsf/progress_deadline.py` exposes the generic policy, and
`pythonWrapper/ndnsf/service.py` integrates it with collaboration operation
status watching. NDNSF owns authentication/binding, monotonic admission, idle
renewal, and hard-cap behavior; NDNSF-DI continues to own model and phase
interpretation.

`tests/python/test_spec165_collaboration_watch.py` proves advancing progress
survives the original idle window, duplicate or changed same-version status
does not renew it, and continuous progress cannot extend the hard deadline.
