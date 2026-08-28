# Large-data NAC-ABE helper evidence

## Command

```text
nfd-start
bash examples/run_large_data_helper_validation.sh
nfd-stop
```

This is a bounded host-NFD diagnostic, not the MiniNDN formal gate. The test
uses the existing C++ examples and an 8,204-byte plaintext; it does not create
a model artifact or a SIF.

## Result

```text
LARGE_DATA_PUBLISH_SEGMENTS ... plaintextBytes=8204 ... segments=2 ... activePut=true
LARGE_DATA_PUBLISH_SUCCESS ...
LARGE_DATA_FETCH_SUCCESS plaintext=<exact 8204-byte test plaintext>
LARGE_DATA_UNAUTHORIZED_FAILURE_CLEAN error=...
LARGE_DATA_HELPER_VALIDATION=PASS
authorized_status=0
unauthorized_status=0
```

The helper starts a bootstrap Provider before the User because the User needs
the NAC-ABE DKEY during construction. The fetch probe runs its synchronous
fetch on a worker while the main thread services the Face event loop; this
avoids mistaking a CLI event-loop sequencing error for a transport failure.
