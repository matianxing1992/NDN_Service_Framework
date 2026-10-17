#!/usr/bin/env python3
"""Focused ownership and readback checks for Spec206's MiniNDN link driver."""

import json
from pathlib import Path
import sys
import tempfile
import time
import unittest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "Experiments" / "system-benefits"))
import run_dynamic  # noqa: E402


class Interface:
    def __init__(self, name):
        self.name = name
        self.up = True

    def isUp(self):
        return self.up


class Node:
    def __init__(self):
        self.peers = {}
        self.rules = set()

    def connectionsTo(self, other):
        return [self.peers[other]]

    def cmd(self, command):
        tokens = command.split()
        interface = tokens[4]
        if tokens[1] == "qdisc" and tokens[2] == "show":
            loss = " loss 100%" if interface in self.rules else ""
            return "qdisc netem 10: parent 5:1 delay 10.0ms" + loss + "\n"
        if tokens[1:3] != ["qdisc", "change"]:
            raise AssertionError(command)
        if "100%" in tokens:
            self.rules.add(interface)
        else:
            self.rules.discard(interface)
        return "\nEXIT:0\n"


class Network:
    def __init__(self):
        self.nodes = {name: Node() for name in ("relay", "drone1", "drone2")}
        for name in ("drone1", "drone2"):
            pair = (Interface("relay-" + name), Interface(name + "-relay"))
            self.nodes["relay"].peers[self.nodes[name]] = pair

    def __getitem__(self, name):
        return self.nodes[name]

    def configLinkStatus(self, src, dst, status):
        raise AssertionError("firewall outage must retain NFD faces")


class Spec206LinkTraceTests(unittest.TestCase):
    def test_offline_trace_applies_both_links_and_restores_them(self):
        class Ndn:
            net = Network()

        traces = [
            {"changes": [{"offset_ms": 0, "available": False},
                         {"offset_ms": 20, "available": True}]},
            {"changes": [{"offset_ms": 0, "available": True},
                         {"offset_ms": 20, "available": False}]},
        ]
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "links.jsonl"
            restored = []
            active = run_dynamic.start_link_trace(
                Ndn(), traces, time.monotonic_ns() + 20000000, log, restored.append)
            try:
                active[0].join(timeout=1)
                self.assertFalse(active[0].is_alive())
                rows = [json.loads(line) for line in log.read_text().splitlines()]
                self.assertEqual(len(rows), 4)
                self.assertTrue(all(row["observed_up"] == row["expected_up"]
                                    for row in rows))
                self.assertEqual(restored, ["drone2", "drone1"])
            finally:
                run_dynamic.stop_link_trace(Ndn(), active)
            self.assertTrue(all(interface.isUp()
                                for pair in Ndn.net["relay"].peers.values()
                                for interface in pair))
            self.assertFalse(Ndn.net["relay"].rules)


if __name__ == "__main__":
    unittest.main()
