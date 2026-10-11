import importlib.util
from pathlib import Path
import subprocess
import tempfile
import sys
from types import SimpleNamespace
import unittest
from unittest import mock

from ndn.encoding import Component, Name

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("native_prefix_roles", ROOT / "Experiments/native_prefix_roles.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class PrefixRoles(unittest.TestCase):
    def test_exact_sync_and_complete_producer_data_roles(self):
        group = "/org/group"
        actor = "/org/NDNSF/STREAM/provider/p0"
        rows = MODULE.prefix_role_plan(actors=[actor], groups=[group], data_roots=["/org"])
        sync = [row for row in rows if row["role"] == "sync"]
        self.assertEqual(len(sync), 1)
        self.assertEqual(Name.from_str(sync[0]["prefix"]),
                         Name.from_str(group) + [Component.from_version(3)])
        self.assertEqual(sync[0]["required_strategy"], MODULE.MULTICAST)
        data = {row["prefix"]: row for row in rows if row["role"] == "data"}
        self.assertEqual(data[actor]["required_strategy"], MODULE.BEST_ROUTE)
        self.assertEqual(data[group]["required_strategy"], MODULE.BEST_ROUTE)

    def test_conflicts_roots_and_unavailable_version_fail_before_external_calls(self):
        for kwargs in [dict(actors=["/"], groups=[]),
                       dict(actors=["/g/v=3"], groups=["/g"]),
                       dict(actors=["/p"], groups=["/g"], protocol_version=4)]:
            with self.assertRaises(ValueError):
                MODULE.prefix_role_plan(**kwargs)

    def test_real_command_interface_readback_and_failures(self):
        class Process:
            def __init__(self, stdout, returncode=0):
                self.stdout, self.returncode = stdout.encode(), returncode
            def communicate(self, timeout=None):
                return self.stdout, b"failure" if self.returncode else b""
        class Node:
            name = "provider-node"
            def __init__(self, mode):
                self.mode, self.commands, self.table = mode, [], {}
            def popen(self, args, **kwargs):
                self.commands.append(args)
                if args[1:3] == ["strategy", "set"]:
                    if self.mode == "set-failure": return Process("", 1)
                    self.table[args[3]] = args[4] + "/v=1"
                    return Process("")
                if args[1:3] == ["strategy", "list"]:
                    if self.mode == "longest-conflict": self.table["/g/v=3"] = MODULE.BEST_ROUTE
                    if self.mode == "deep-data-conflict": self.table["/p/NDNSF-DI/tensor"] = MODULE.MULTICAST
                    return Process("\n".join(f"prefix={p} strategy={s}" for p, s in self.table.items()))
                if args[1:3] == ["fib", "list"]: return Process("prefix=/p nexthops={faceid=1 cost=0}")
                raise AssertionError(args)
        for mode in ("pass", "set-failure", "longest-conflict", "deep-data-conflict"):
            node = Node(mode)
            with tempfile.TemporaryDirectory() as directory:
                receipt = Path(directory) / "roles.json"
                args = dict(actors=["/p"], groups=["/g"], receipt=receipt)
                self.addCleanup(mock.patch.stopall)
                transport = mock.patch.dict(sys.modules, {"minindn.util": SimpleNamespace(
                    getPopen=lambda node, argv, **kwargs: node.popen(argv, **kwargs))})
                transport.start()
                if mode == "pass":
                    result = MODULE.configure_prefix_roles(SimpleNamespace(net=SimpleNamespace(hosts=[node])), **args)
                    self.assertTrue(receipt.exists())
                    self.assertEqual(len(result[0]["roles"]), 3)
                    self.assertIn("faceid=1", result[0]["fib_list"])
                else:
                    with self.assertRaises(RuntimeError):
                        MODULE.configure_prefix_roles(SimpleNamespace(net=SimpleNamespace(hosts=[node])), **args)
                    self.assertFalse(receipt.exists())


if __name__ == "__main__":
    unittest.main()
