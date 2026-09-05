# Spec178 paired evaluation evidence

The frozen registration was evaluated at 1, 2, 4, and 6 views with the same `vehicle-mvcnn-v1` artifact, preprocessing, CPU provider, and target sample. All four rows completed on the qualification fixture and produced top-1 accuracy and macro-F1 of 1.0 for the synthetic `car` label.

The report also records accepted views, transferred bytes, model-only CPU inference time, end-to-end subprocess time, peak RSS, failure-stage counts, confidence, paired latency/accuracy deltas, bootstrap metadata, and McNemar contingency counts. The observed end-to-end means were 1749.193, 2624.863, 4514.622, and 6164.955 ms for 1/2/4/6 views; model-only means were 8.717, 2.580, 3.588, and 2.794 ms. These timings include large local fixture materialization and are not a flight-network performance claim.

There is one grouped target, so the uncertainty status is `insufficient-samples`. No recognition-quality or multi-view-benefit claim is made. Evaluation SHA-256: `08ab906dfa4d95814098729bf875e69663fd6a59b7f328cc91e71a096a44b055`.
