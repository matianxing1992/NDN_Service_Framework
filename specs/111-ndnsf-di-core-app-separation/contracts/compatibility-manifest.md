# Contract: Compatibility Manifest

The machine-readable manifest inventories these surface kinds:

- Python public imports and root re-exports;
- console commands and subcommands;
- JSON/YAML schemas and configuration keys;
- C++ targets, headers and executable names;
- examples and regression scripts;
- container/systemd/Slurm/Apptainer packaging callers;
- MiniNDN and iTiger experiment entry points.

Every entry records:

- legacy surface and kind;
- canonical owner and target;
- current repository callers;
- compatibility adapter location;
- usage/deprecation signal;
- rollback release;
- known external callers, external migration evidence, or explicit
  `external_use_unknown` state;
- removal gate and status.

Invariants:

- one legacy surface maps to exactly one canonical target;
- adapter and canonical target cannot both implement behavior;
- re-export cycles are invalid;
- removal requires characterization tests, two zero-caller snapshots, external
  migration confirmation (or explicit user-approved expiry when external use is
  unknown) and rollback evidence;
- historical evidence paths are never rewritten by manifest migration.
