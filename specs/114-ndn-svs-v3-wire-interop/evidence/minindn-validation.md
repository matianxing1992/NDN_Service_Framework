# MiniNDN interoperability validation

## Spec 115 rewritten-history candidate

- candidate: `spec114-ae031d4a84f15dc42aae`;
- NDN-SVS head/tree: `fd99b3f51bdd048c783a023f1082a7091a27bac9` /
  `be3d9ccd348377160ab7afffabcbdea9459cff4f`;
- six formal run-once cells: 6/6 SUCCESS;
- 0% coverage: 120/120; 5% coverage: 120/120;
- duplicates/rejects/restarts/SyncAck: 0/0/0/0;
- final vectors equal: 6/6;
- formal summary digest:
  `39b3c3ca00f02a8a88ddc0c356c8a85c28cdf8277b833bef0d10bbf11a049669`.

The launcher now reads the source path sealed in the candidate manifest. This
test-first correction passed 9/9 focused tool tests and avoided moving the live
Experimental branch before validation.

Date: 2026-07-16

## Final frozen candidate

- candidate: `spec114-9116d37e2e705bf281f4`
- NDN-SVS head/tree: `53dd1588201b967a4aa9decd3e51ade3263e0f88` /
  `be3d9ccd348377160ab7afffabcbdea9459cff4f`
- campaign attempt: `audit-convergence-02`
- candidate manifest SHA-256:
  `9e712cfcec62a12f9a5ee6dec60ce30e6f47c5efcf581949986779ca140836c7`
- formal summary SHA-256:
  `a2184d208ac0584ce02e3ace6843d1f545f07617e6ea6fb7e085e4a859f6b52d`

The manifest binds the exact target commit/tree, NDNts lock, protocol/timers,
campaign attempt, and hashes of both campaign tools plus C++/NDNts/standalone
harness inputs. Every formal cell was executed exactly once with 20
publications per peer and a fixed 60-second settle window.

| Cell | Loss | Result | Elapsed s | C++ unique | NDNts unique |
|---|---:|---|---:|---:|---:|
| loss00-run01 | 0% | PASS | 77.425 | 20 | 20 |
| loss00-run02 | 0% | PASS | 78.015 | 20 | 20 |
| loss00-run03 | 0% | PASS | 77.641 | 20 | 20 |
| loss05-run01 | 5% | PASS | 76.961 | 20 | 20 |
| loss05-run02 | 5% | PASS | 77.202 | 20 | 20 |
| loss05-run03 | 5% | PASS | 77.508 | 20 | 20 |

Acceptance totals: 6/6 cells, 120/120 remote sequences at each loss class,
6/6 equal final vectors, 12/12 zero peer return codes, and zero duplicates,
rejects, Sync Ack Data, or restarts.

## Preserved setup-invalid attempts

- `spec114-6c11778537c60569c2b9`: a non-root launch reached MiniNDN preflight;
  no peer/network experiment ran, and its first state remains `running`.
- `spec114-302b1fe3b2305bf16bd9`: Linux `/tmp` protected-regular ownership rejected
  the root lock before any cell state changed; all cells remain `pending`.

Neither directory was deleted, rewritten, or promoted. The tool now checks root
before run-once mutation, uses `/run/lock`, validates bound harness hashes, and
supports a content-bound campaign attempt so setup evidence can be retained.
