# Isolated Final V3 Validation

## Identity

- commit: `eb0754d9d250f65789928cfdf7f3917fae996f07`;
- subject: `svs: implement standards-compliant V3 synchronization`;
- tree: `db1a5e34675da2e54223d992cc0c3e36f5cc1d1c`;
- parent: `7804202578ffed5d160ac8178a59593942372b9f`.

This tree is identical to validated owner `49ec007`. Its isolated ASan gate
passed 44/44 unit cases. It directly owns the V3 codec, malformed-packet and
validation-order tests, group-prefix registration, versioned local dispatch,
registration failure/lifecycle handling, and neutral deterministic fixtures.
It does not own Mapping/Repair extensions or the executable external harness;
the latter is owned by NDNSF.

Fixture generation was repeated twice with zero diff. The deterministic
fixture hash-list SHA-256 is:

```text
dbd63dc5a5e5b344268865c159aa8bcc10a31ba719f56ed4ba18f8a1a51d046e
```

The five-case NDNSF-owned C++/TypeScript NDNts gate was then run against clean
`Experimental@70e682f` without manual route injection. It passed C++ to
TypeScript, TypeScript to C++, concurrent V3, explicit V2, and V2/V3 mismatch
isolation with zero Sync-Ack. Summary SHA-256:

```text
129ce878fd54be785a2f9a3fe767cb2b80a4ad1426701700f8d45dcff99eef9b
```
