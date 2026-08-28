# Final Preflight

**Result root**:
`results/spec146-final-preflight-20260726T090000Z`  
**Verdict**: PASS  
**Formal matrix consumed**: no

## Build and test gates

- full native build: 354/354, `./waf build -j2`;
- forced Python binding rebuild:
  `python3 setup.py build_ext --inplace --force -j2`;
- full native suite: 381/381;
- Spec 146 Python: 8/8;
- Core streaming Python: 19/19;
- live-stream generality Python: 27/27;
- rebuilt `ndnsf` binding import: PASS.

## Fresh 60-second diagnostics

| Profile | Delivery | Mean ms | p50 ms | p95 ms | p99 ms | Gap ms | Recovery | Retry | Timeout | Nack | Nonproductive |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| zero-loss | 100% | 8.072 | 4.795 | 17.249 | 28.890 | 92.784 | 0/0 | 69 | 103 | 0 | 0% |
| loss | 100% | 19.033 | 6.182 | 130.683 | 171.408 | 163.326 | 106/108 | 99 | 314 | 0 | 2.276% |
| reorder | 100% | 36.937 | 33.647 | 56.699 | 71.234 | 97.719 | 0/0 | 79 | 114 | 0 | 0% |
| combined | 100% | 47.041 | 33.934 | 152.410 | 233.202 | 251.933 | 80/84 | 112 | 307 | 0 | 2.005% |

Every cell passed every delivery, latency, longest-gap, future-hit, Mapping
novelty, Interest-conservation, and utility gate. Future-hit and Mapping
novelty were 100% in all four cells. Every command explicitly contained
`NDNSF_STREAM_PACKET_TIMELINE_TRACE=0`.

## Subject hashes before formal freeze

```text
15512864c34386fbc5e045b39daa217d79c0a79b345d45338e667f5aab5f5add  ndn-service-framework/Stream.hpp
ee660f7f276e75d9176b6ddf3011af9ddd31609de8fb6603fb056016f2c8551d  ndn-service-framework/Stream.cpp
069b72751ae44f4a2cbfe8c42bedec6aa4d049c50abd03acfac7681f303c3371  ndn-service-framework/common.hpp
05d7a65e7cfd3f2453c7bb4d4b969ddb09864f2a1b7c1c8ec7d815dcdad8d831  ndn-service-framework/ServiceUser.cpp
ae8028ca345d0cb08bd63730fbc2a127875467ca65f3a5b8fe09ed8c0c916970  ndn-service-framework/ServiceProvider.cpp
3686545d5fb846479606946bd18551ead1e50128a3e36cf5490b1f32c5daa0a5  build/libndn-service-framework.so
15b60bc3f6311c69840342715bd13d2ed6b2402d61bff01e004b29cf236b58c3  build/examples/App_ServiceController
7dc53396c952a0ea61b19dd2d815bfa10d228869855d43be9b77027445425932  build/examples/UavSensorStreamNode
320269dccc36a16d98ce61eb508c1a3a8a573631e6d44c10bcd3766689891edb  pythonWrapper/ndnsf/_ndnsf.cpython-38-x86_64-linux-gnu.so
```

The one-shot runner will independently hash all declared inputs into the
formal manifest. A mismatch during the campaign is terminal and cannot be
retried.
