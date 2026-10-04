"""Link fixture checks; native measurements and scoring have C++ oracles."""
import ast
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

SOURCE = Path(__file__).resolve().parents[1] / "Experiments/native_probe_validation_links.py"
spec = importlib.util.spec_from_file_location("probe_validation_links", SOURCE)
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)


class ProbeValidationLinks(unittest.TestCase):
    def test_yolo_requester_publication_directory_is_scoped_and_optional(self):
        source = SOURCE.with_name("NDNSF_DI_Yolo2x2_Minindn.py")
        tree = ast.parse(source.read_text())
        definitions = [node for node in tree.body if isinstance(node, ast.FunctionDef)
                       and node.name == "requester_environment"]
        self.assertEqual(len(definitions), 1)
        namespace = {}
        exec(compile(ast.Module(body=definitions, type_ignores=[]), str(source), "exec"),
             namespace)
        build_environment = namespace["requester_environment"]
        parent = {"PATH": "/usr/bin", "NDNSF_DI_STATE_ROOT": "provider"}
        original = dict(parent)
        self.assertEqual(build_environment(parent, Path("user-state")),
                         {**parent, "NDNSF_DI_STATE_ROOT": "user-state"})
        with tempfile.TemporaryDirectory() as root:
            directory = Path(root) / "nested" / ".." / "publications"
            result = build_environment(parent, Path("user-state"), directory)
            self.assertEqual(result["NDNSF_REQUEST_LARGE_DATA_DIR"],
                             str(directory.resolve()))
            self.assertEqual(result["NDNSF_DI_STATE_ROOT"], "user-state")
        self.assertEqual(parent, original)
        profile_arguments = [value for node in ast.walk(tree)
                             if isinstance(node, ast.Dict)
                             for key, value in zip(node.keys, node.values)
                             if isinstance(key, ast.Constant) and key.value == "arguments"]
        self.assertEqual(len(profile_arguments), 1)
        expression = compile(ast.Expression(body=profile_arguments[0]), str(source), "eval")
        for directory in (None, Path("requester-publications")):
            options = SimpleNamespace(requester_publication_dir=directory)
            record = eval(expression, {}, {"args_cli": options})
            encoded = json.loads(json.dumps(record))
            self.assertEqual(encoded["requester_publication_dir"],
                             str(directory.resolve()) if directory is not None else None)
            if directory is not None:
                self.assertEqual(encoded["requester_publication_dir"],
                                 build_environment(parent, "user-state", directory)
                                 ["NDNSF_REQUEST_LARGE_DATA_DIR"])
        calls = [node for node in ast.walk(tree) if isinstance(node, ast.Call)
                 and isinstance(node.func, ast.Name)
                 and node.func.id == "requester_environment"]
        self.assertEqual(len(calls), 2)
        for call in calls:
            self.assertEqual(len(call.args), 3)
            self.assertIsInstance(call.args[2], ast.Attribute)
            self.assertEqual(call.args[2].attr, "requester_publication_dir")
            self.assertEqual(call.args[2].value.id, "args_cli")
        assignments = [node for node in ast.walk(tree) if isinstance(node, ast.Assign)
                       and any(isinstance(target, ast.Name) and target.id == "user_env"
                               for target in node.targets)]
        for call in calls:
            self.assertTrue(any(node.value is call for node in assignments))
        arguments = [node for node in ast.walk(tree) if isinstance(node, ast.Call)
                     and isinstance(node.func, ast.Attribute)
                     and node.func.attr == "add_argument" and node.args
                     and isinstance(node.args[0], ast.Constant)
                     and node.args[0].value == "--requester-publication-dir"]
        self.assertEqual(len(arguments), 1)
        self.assertTrue(any(keyword.arg == "default" and keyword.value.value is None
                            for keyword in arguments[0].keywords))

    def topology(self):
        self.routes = {"11": [], "22": []}
        a, b = SimpleNamespace(name="a", pid=11, params={"params": {"homeDir": "/tmp"}}), SimpleNamespace(name="b", pid=22, params={"params": {"homeDir": "/tmp"}})
        x = SimpleNamespace(name="a-eth0", node=a, params={"bw": 1000, "delay": "2ms"}, config=Mock())
        y = SimpleNamespace(name="b-eth0", node=b, params={"bw": 1000, "delay": "2ms"}, config=Mock())
        link = SimpleNamespace(intf1=x, intf2=y)
        x.link = y.link = link
        x.IP = lambda: "10.0.0.1"
        y.IP = lambda: "10.0.0.2"
        a.intfList = lambda: [SimpleNamespace(name="lo", IP=lambda: "127.0.0.1"), x]
        b.intfList = lambda: [y]
        u = SimpleNamespace(name="a-probe0", node=a)
        v = SimpleNamespace(name="b-probe0", node=b)
        peer = SimpleNamespace(intf1=u, intf2=v)
        u.link = v.link = peer
        u.setIP = Mock()
        v.setIP = Mock()
        return SimpleNamespace(net=SimpleNamespace(hosts=[a, b], addLink=Mock(return_value=peer))), x, y

    def execute(self, argv, **kwargs):
        pid = argv[2]
        if "cat" in argv:
            out = b"1500\n"
        elif "show" in argv and "class" in argv:
            out = b"class htb 5:1 root rate 4Mbit ceil 4Mbit burst 1500b/1 cburst 1500b/1\n"
        elif "qdisc" in argv:
            out = b'[{"kind":"netem"}]'
        elif "face" in argv and "create" in argv:
            out = b"face-created id=256\n"
        elif "face" in argv and "list" in argv:
            out = ("faceid=256 remote=" + argv[argv.index("remote") + 1] +
                   " local=" + argv[argv.index("local") + 1]).encode()
        elif "route" in argv:
            prefix = argv[argv.index("prefix") + 1]
            self.routes[pid].append(prefix + " nexthops={faceid=256 (cost=0)}")
            out = b""
        elif "fib" in argv:
            out = ("/model nexthops={faceid=7 (cost=10)}\n" +
                   "/provider/b/NDNSF/PEER-PROBEX nexthops={faceid=7 (cost=10)}\n" +
                   "\n".join(self.routes[pid])).encode()
        else:
            out = b""
        return subprocess.CompletedProcess(argv, 0, stdout=out, stderr=b"")

    def test_dedicated_directions_preserve_model_routes_and_interfaces(self):
        ndn, x, y = self.topology()
        with tempfile.TemporaryDirectory() as root, \
                patch.object(fixture.shutil, "which", side_effect=lambda n: "/usr/bin/" + n), \
                patch.object(fixture.subprocess, "run", side_effect=self.execute) as run:
            out = Path(root) / "links"
            rows = fixture.configure_probe_validation_links(ndn, out, 4, {"a": "/provider/a", "b": "/provider/b"})
            self.assertEqual(len(rows), 2)
            self.assertEqual(rows[0]["peer_node"], "b")
            self.assertEqual(rows[1]["peer_node"], "a")
            self.assertEqual(json.loads((out / "links.json").read_text())["links"], rows)
            x.config.assert_not_called()
            y.config.assert_not_called()
            ndn.net.addLink.assert_called_once()
            self.assertEqual(ndn.net.addLink.call_args.kwargs["bw"], 4)
            self.assertEqual(ndn.net.addLink.call_args.kwargs["delay"], "1ms")
            link = ndn.net.addLink.return_value
            link.intf1.setIP.assert_called_once_with("10.252.1.1/30")
            link.intf2.setIP.assert_called_once_with("10.252.1.2/30")
            self.assertEqual(len(self.routes["11"]), 3)
            self.assertTrue(any("PEER-PROBE-READY" in p for p in self.routes["11"]))
            self.assertEqual(x.params["bw"], 1000)
            for call in run.call_args_list:
                self.assertGreater(call.kwargs["timeout"], 0)
                self.assertLessEqual(call.kwargs["timeout"], 15)
                self.assertTrue(call.kwargs["check"])
                self.assertEqual(call.args[0][:2], ["/usr/bin/mnexec", "-a"])
            changes = [c.args[0] for c in run.call_args_list if "replace" in c.args[0]]
            self.assertEqual(len(changes), 2)
            self.assertTrue(all("5:1" in c and c[-4:] == ["burst", "1500", "cburst", "1500"] for c in changes))

    def test_invalid_bandwidth_does_not_touch_links(self):
        ndn, x, _ = self.topology()
        with self.assertRaises(ValueError):
            fixture.configure_probe_validation_links(ndn, "unused", 1000, {"a": "/provider/a", "b": "/provider/b"})
        x.config.assert_not_called()

    def test_accepted_route_waits_for_fib_visibility(self):
        ndn, _, _ = self.topology()
        hidden = set()
        def delayed(argv, **kwargs):
            p = self.execute(argv, **kwargs)
            if "fib" in argv and self.routes[argv[2]]:
                key = (argv[2], len(self.routes[argv[2]]))
                if key not in hidden:
                    hidden.add(key)
                    p.stdout = b"/model nexthops={faceid=7 (cost=10)}\n"
            return p
        with tempfile.TemporaryDirectory() as root, \
                patch.object(fixture.shutil, "which", side_effect=lambda n: "/usr/bin/" + n), \
                patch.object(fixture.subprocess, "run", side_effect=delayed), \
                patch.object(fixture.time, "sleep") as sleep:
            rows = fixture.configure_probe_validation_links(ndn, Path(root) / "links", 4,
                                                            {"a": "/provider/a", "b": "/provider/b"})
            self.assertEqual(len(rows), 2)
            self.assertEqual(sleep.call_count, 6)

    def test_bad_readback_fails(self):
        ndn, _, _ = self.topology()
        def bad(argv, **kwargs):
            p = self.execute(argv, **kwargs)
            if "show" in argv and "class" in argv:
                p.stdout = p.stdout.replace(b"burst 1500b", b"burst 15Kb")
            return p
        with tempfile.TemporaryDirectory() as root, \
                patch.object(fixture.shutil, "which", side_effect=lambda n: "/usr/bin/" + n), \
                patch.object(fixture.subprocess, "run", side_effect=bad), \
                self.assertRaises(RuntimeError):
            fixture.configure_probe_validation_links(ndn, Path(root) / "links", 4, {"a": "/provider/a", "b": "/provider/b"})

    def test_existing_fib_change_is_rejected(self):
        ndn, _, _ = self.topology()
        def changed(argv, **kwargs):
            p = self.execute(argv, **kwargs)
            if "fib" in argv and self.routes[argv[2]]:
                p.stdout = p.stdout.replace(b"faceid=7", b"faceid=8")
            return p
        with tempfile.TemporaryDirectory() as root, \
                patch.object(fixture.shutil, "which", side_effect=lambda n: "/usr/bin/" + n), \
                patch.object(fixture.subprocess, "run", side_effect=changed), \
                self.assertRaisesRegex(RuntimeError, "existing FIB"):
            fixture.configure_probe_validation_links(ndn, Path(root) / "links", 4, {"a": "/provider/a", "b": "/provider/b"})

    def test_peer_route_multiple_next_hops_is_rejected(self):
        ndn, _, _ = self.topology()
        def duplicate(argv, **kwargs):
            p = self.execute(argv, **kwargs)
            if "fib" in argv and self.routes[argv[2]]:
                p.stdout = p.stdout.replace(b"faceid=256 (cost=0)", b"faceid=256 (cost=0),faceid=257 (cost=10)")
            return p
        with tempfile.TemporaryDirectory() as root, \
                patch.object(fixture.shutil, "which", side_effect=lambda n: "/usr/bin/" + n), \
                patch.object(fixture.subprocess, "run", side_effect=duplicate), \
                self.assertRaisesRegex(RuntimeError, "FIB is not exclusive"):
            fixture.configure_probe_validation_links(ndn, Path(root) / "links", 4, {"a": "/provider/a", "b": "/provider/b"})


if __name__ == "__main__":
    unittest.main()
