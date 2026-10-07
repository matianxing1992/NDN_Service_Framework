# Spec193 network object wrapper checkpoint

2026-10-07 12:32 CDT, branch Experimental. T005 is in progress; Spec193 is not complete.

`NW_OBJECT` passed twice on five independent NFD hosts with candidate lock `fe4e9e4f223ec6f6a46738973cb7595307aaa5cf606622aa99a0e21a85dcf3d2`. Evidence: `specs/193-distributedrepo-multinode-api-campaign/evidence/nw-object-diag-004/005/run-manifest.json`. Both runs have PASS scenario/cleanup, two zero-exit native clients without Python mapping, and five nonempty packet captures.

Each run executes 17 public object wrapper calls and four explicitly selected catalog calls. The independent oracle verifies caller/provider certificates and Request, ACK, Selection, Response inner/outer signatures; each stage's inner wire digest agrees at caller and Repo A. Eight host/name observations of broadcast B ACKs are verified. B has no final Selection/Response, handler execution or physical mutation.

Independent response and storage checks cover 521-byte complete writes, tail-first ranges and commit, partial staging abort, exact read windows including the direct-provider overload, original producer-signed Data INSERT/STATUS/FETCH, four catalog views and deletion tombstone. The final exact manifest map contains the range object, inserted parent and original packet; real payload digest set contains source and original wire, with no staging. Producer root-issued identity, packet signature and original wire are independently checked.

Build: native fixture PASS 27.371 seconds, Python compilation and whitespace PASS. Focused pre-runtime review is in `evidence/nw-object-review-20261007/review.md`; design is `contracts/network-wrapper-campaign.md`. Failures 001 (unrouted producer namespace) and 002 (ACK/Response schema mix and buffered capture tail) remain preserved and documented in failure-log.md. Run 003 was an intermediate diagnostic PASS before strengthening the oracle.

This closes local evidence for N01/N02/N04/N05/N06/N09 only. N03/N07 artifact wrappers, N08 negative callbacks and N10 Persistent legacy rejection remain open, followed by P01–P06, A19/A21/large-range gaps and T007 full same-candidate qualification. The native fixture/runner/oracle remain part of the preexisting uncommitted Spec192/193 composition; this checkpoint does not claim clean-checkout reproduction or commit unrelated source. No push.

Workflow gates: Context Mode active health passed and durable registry remains authority; CodeGraph synchronized/reviewed; canonical Spec Kit design/review applied. Existing constitution's Spec registry owns resumable state; no GSD parallel authority was added. ARS statistical/research workflow is not applicable to this correctness batch. No Tiger or performance qualification is claimed.
