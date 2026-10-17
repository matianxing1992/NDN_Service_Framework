"""Offline matrix validation; no MiniNDN process is launched."""
import importlib.util
import json
from pathlib import Path

import pytest
from collections import Counter


SOURCE = Path(__file__).resolve().parents[2] / "Experiments/system-benefits/dynamic_campaign.py"
SPEC = importlib.util.spec_from_file_location("spec205_dynamic_matrix", SOURCE)
CAMPAIGN = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CAMPAIGN)


@pytest.mark.parametrize("mutation", [None, "missing", "seed", "order", "duplicate", "period"])
def test_irregular_formal_matrix(tmp_path, mutation):
    (tmp_path / "design.md").write_text("fixed E3 design")
    # Independent explicit ordering, not the production generator.
    orders = ["01234", "12340", "23401", "34012", "40123", "43210", "04321", "10432",
              "21043", "32104", "01234", "12340", "23401", "34012", "40123", "43210"]
    conditions = [("stable", 4700), ("slow", 4700), ("fast", 4700), ("stable", 100), ("fast", 100)]
    cells = []
    for block, order in enumerate(orders):
        for index in order:
            profile, period = conditions[int(index)]
            cells.append({"seed": 20590 + block, "period_ms": period, "dwell_ms": 0,
                          "window_ms": 60000, "recovery_condition": "prepared",
                          "availability_profile": profile})
    assert cells == CAMPAIGN.e3_formal_cells()
    assert sum(4 * len(range(0, 60000, c["period_ms"])) for c in cells) == 79296
    assert set(Counter((seed - 20401) % 4 for seed in range(20590, 20606)).values()) == {4}
    if mutation == "missing": cells.pop()
    if mutation == "seed": cells[0]["seed"] = 20581
    if mutation == "order": cells[0], cells[1] = cells[1], cells[0]
    if mutation == "duplicate": cells[1] = dict(cells[0])
    if mutation == "period": cells[0]["period_ms"] = 100
    path = tmp_path / "matrix.json"
    path.write_text(json.dumps({"version": 3, "purpose": "formal",
                               "design_reference": "design.md", "cells": cells}))
    if mutation:
        with pytest.raises(ValueError): CAMPAIGN.load_matrix(path)
    else:
        assert len(CAMPAIGN.load_matrix(path)[0]["cells"]) == 80


@pytest.mark.parametrize("mutation", [None, "missing", "condition", "seed", "order", "duplicate"])
def test_recovery_formal_matrix(tmp_path, mutation):
    (tmp_path / "design.md").write_text("frozen test design")
    cells = []
    for i, seed in enumerate(range(20560, 20576)):
        order = ["prepared", "reregister"]
        if (i + i//4) % 2:
            order.reverse()
        cells.extend({"seed": seed, "period_ms": 4700, "dwell_ms": 0,
                      "window_ms": 60000, "recovery_condition": condition} for condition in order)
    if mutation == "missing": cells.pop()
    if mutation == "condition": cells[0]["recovery_condition"] = "none"
    if mutation == "seed": cells[0]["seed"] = 99
    if mutation == "order": cells[0], cells[1] = cells[1], cells[0]
    if mutation == "duplicate": cells[1] = dict(cells[0])
    path = tmp_path / "matrix.json"
    path.write_text(json.dumps({"version": 2, "purpose": "formal",
                               "design_reference": "design.md", "cells": cells}))
    if mutation:
        with pytest.raises(ValueError): CAMPAIGN.load_matrix(path)
    else:
        assert len(CAMPAIGN.load_matrix(path)[0]["cells"]) == 32


@pytest.mark.parametrize("mutation", [None, "formal", "profile", "mode", "dwell", "duplicate", "missing"])
def test_irregular_pilot_matrix(tmp_path, mutation):
    (tmp_path / "design.md").write_text("frozen pilot design")
    cells = [{"seed": 20581, "period_ms": period, "dwell_ms": 0, "window_ms": 60000,
              "recovery_condition": "prepared", "availability_profile": profile}
             for profile, period in (("stable", 4700), ("slow", 4700), ("fast", 4700),
                                     ("stable", 100), ("fast", 100))]
    matrix = {"version": 3, "purpose": "pilot", "design_reference": "design.md", "cells": cells}
    if mutation == "formal": matrix["purpose"] = "formal"
    if mutation == "profile": cells[0]["availability_profile"] = "unknown"
    if mutation == "mode": cells[0]["recovery_condition"] = "reregister"
    if mutation == "dwell": cells[0]["dwell_ms"] = 500
    if mutation == "duplicate": cells[1] = dict(cells[0])
    if mutation == "missing": del cells[0]["availability_profile"]
    path = tmp_path / "matrix.json"
    path.write_text(json.dumps(matrix))
    if mutation:
        with pytest.raises(ValueError): CAMPAIGN.load_matrix(path)
    else:
        assert len(CAMPAIGN.load_matrix(path)[0]["cells"]) == 5
