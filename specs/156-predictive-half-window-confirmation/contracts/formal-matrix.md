# Formal Matrix Contract

Incorporates the complete Spec 155 formal contract unchanged:

- 10/20/30/40/50/60 fps in order;
- zero loss/reorder, 5-second warm-up, >=60-second measurement;
- same two-node MiniNDN topology, 8 Mbps, width 480, one FEC repair;
- rate error <=5%, delivery >=98%, future-hit >=95%;
- retry/Payload <=2% and timeout/Payload <=2%;
- Mapping=0, exact wire, decoding, bounded queues;
- end-to-end p99 and longest gap <=1000 ms;
- normal exit and terminal APP marker;
- immutable hashes, no automatic retry or selective replacement.
