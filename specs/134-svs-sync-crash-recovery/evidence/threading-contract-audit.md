# NDN-SVS Threading-Contract Audit

## Verdict

`BLOCK` for the previous repair-and-qualification design.

The historical subject does not have one coherent, tested cross-thread
contract. README and the public header are silent; examples claim thread
safety; unit tests do not test concurrency; source protection is partial; and
sanitizers demonstrate races. The repair path is therefore not a necessary
prerequisite for measuring the old serial architecture. The revised design
uses one Face/io_context execution thread per peer process.

## Source Matrix

| Evidence | Exact identity | Observed fact | What it proves |
|---|---|---|---|
| README | `a994401...:README.md` | Installation/chat commands only; no threading language | No README-level guarantee |
| Public API | `a994401...:ndn-svs/svspubsub.hpp` | `publish()` documented; no thread safety/affinity statement | Caller-thread contract unspecified |
| PubSub example | `a994401...:examples/chat-pubsub.cpp:77-94,136` | Face on second thread; application thread publishes; comment says thread-safe | Intended example usage, not verified guarantee |
| Regex example | `a994401...:examples/chat-pubsub-regex.cpp:80-98,141` | Same pattern and claim | Repeated example intent |
| Unit tests | `a994401...:tests/unit-tests/svspubsub.t.cpp` | `DummyClientFace`, direct single-thread calls | Functional behavior only; no concurrency proof |
| Historical source | `core.cpp`, `svspubsub.cpp`, `mapping-provider.cpp`, `store-memory.hpp` | Partial mutexes; uncovered objects/operations | No complete cross-thread ownership policy |
| Receive parallelization | `a8a9656` | Worker computation with serialized Face-thread state application | Later design recognizes Face ownership |
| Async production | `15d1bc6` | Preparation workers and ordered Face/io_context commit | Not equivalent to arbitrary old synchronous calls |
| ASan/UBSan | `results/spec134-svs-sync-crash-recovery/diagnosis-asan-01/` | Scheduler container corruption/double free | Cross-thread harness is unsafe at the tested boundary |
| TSan | `results/spec134-svs-sync-crash-recovery/diagnosis-tsan-01/` | Notification Mapping read/write race | Confirms unsynchronized cross-thread state |

## Reclassification

- Spec 133's previous three-arm preflight remains a real failed execution, but
  it used the disputed cross-thread model and is ineligible for formal
  profiling admission.
- Spec 134's sanitizer results remain measured root-cause evidence for that
  model.
- The experimental repair commit remains local forensic evidence and is not
  authorized for merge, qualification, or Spec 133.
- No old result is deleted, overwritten, retried, or converted into a
  performance conclusion.

## Correct Formal Model

Two MiniNDN nodes run two independent peer processes. Each process has one
Face/io_context execution thread. The application release timer, synchronous
`publish()`, Sync/Mapping/payload processing, and subscription callbacks all run
on that thread. Missed release slots are recorded and skipped; there is no
catch-up queue or second NDN-SVS caller.

## Audit Scorecard

| Dimension | Before revision | After document revision |
|---|---|---|
| Intent fidelity | BLOCK: wrong concurrency model | PASS |
| Code reality | BLOCK: example treated as full contract | PASS |
| Necessity/ownership | BLOCK: unnecessary old-library repair | PASS |
| Evidence integrity | BLOCK: cross-thread failure overgeneralized | PASS |
| Validation design | BLOCK: qualification repeated wrong model | CONDITIONAL PASS |
| Task executability | Repair tasks executable but wrong | PASS after task replacement |

The remaining condition is implementation and local validation of the corrected
single-I/O-thread harness before the once-only MiniNDN qualification.
