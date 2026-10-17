#!/usr/bin/env python3
"""Fixed-coefficient accounting from qualified native component observations.

This does not simulate successive policy changes or network distribution.
All alternatives already reuse signing identities. Cold/warm access records
measure local content-key decryption, not credential retrieval traffic.
"""
import argparse
import json
from pathlib import Path

from analyze_credentials import ARMS, analyze, estimate


def derive(path):
    qualified = analyze(path, 5)
    records = [json.loads(line) for line in path.read_text().splitlines() if line.strip()]
    costs = {(r["arm"], r["permissions"], r["repetition"], r["phase"]): r
             for r in records if r.get("schema") == "ndnsf-system-cost-v1"}
    metrics = {
        "local_cpu_ms": lambda r: r["setup_cpu_ms"] + r["management_cpu_ms"],
        "signed_object_bytes": lambda r: r["management_wire_bytes"]
                                      + r["public_parameter_wire_bytes_once"],
    }
    output = []
    for permissions in (1, 4, 16):
        for alternative in ARMS[1:]:
            for metric, value in metrics.items():
                differences = {}
                for phase in ("initial", "grant", "revoke"):
                    differences[phase] = [
                        value(costs[("AGGREGATED_ABE", permissions, repetition, phase)])
                        - value(costs[(alternative, permissions, repetition, phase)])
                        for repetition in range(1, 6)
                    ]
                coefficients = {phase: estimate(values)
                                for phase, values in differences.items()}
                initial = coefficients["initial"]["mean"]
                slope = coefficients["grant"]["mean"]
                # Only an initial saving eroded by positive grant cost defines
                # this crossing. Do not turn a negative/undefined root into zero.
                crossing = -initial / slope if initial < 0 and slope > 0 else None
                scenarios = []
                for grants, rotations in ((0, 0), (1, 0), (4, 0), (8, 0),
                                          (12, 0), (16, 0), (32, 0), (1, 1)):
                    paired = [differences["initial"][i] + grants * differences["grant"][i]
                              + rotations * differences["revoke"][i] for i in range(5)]
                    scenarios.append({"grants": grants, "rotations": rotations,
                                      "aggregate_minus_alternative": estimate(paired)})
                output.append({"permissions": permissions, "alternative": alternative,
                    "metric": metric, "paired_coefficients": coefficients,
                    "zero_rotation_grant_crossing_of_mean_model": crossing,
                    "scenarios": scenarios})
    return {"native_component_qualified": qualified["qualified"],
        "input_sha256": qualified["input_sha256"],
        "scope": "derived fixed-coefficient accounting; not sequential updates or measured network break-even",
        "formula": "aggregate minus alternative = initial difference + G*grant difference + R*rotation difference",
        "statistical_unit": "five paired component repetitions; intervals conditional on measured phase coefficients",
        "identity_setup": qualified["identity_bootstrap"],
        "identity_scope": "common to all alternatives; one elapsed observation includes a negative-fixture identity; no differential savings",
        "access_scope": "local content-key cold/warm crypto; no certificate-fetch or network measurement",
        "accounting": output}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    args = parser.parse_args()
    print(json.dumps(derive(args.input), indent=2, allow_nan=False))
