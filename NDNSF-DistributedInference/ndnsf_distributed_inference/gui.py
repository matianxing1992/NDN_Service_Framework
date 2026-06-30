"""Tkinter GUI for NDNSF-DistributedInference deployment workflows.

The GUI is intentionally a thin shell around the APP-level API and existing
command-line tools. It helps users create and inspect policy files, choose NDN
identities, run policy validation, and launch example controller/provider/user
processes without exposing low-level NDN packet details.
"""

from __future__ import annotations

import json
import queue
import shlex
import subprocess
import sys
import tempfile
import threading
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any, Callable

try:
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk
except ImportError as exc:  # pragma: no cover - depends on system Tk package
    raise RuntimeError("NDNSF-DI GUI requires Python tkinter") from exc

from .onnx_graph import analyze_onnx_graph, estimate_split_candidates
from .policy import explain_policy, load_config, load_or_generate_deployment


DEFAULT_POLICY = Path(
    "examples/python/NDNSF-DistributedInference/yolo_2x2/yolo_policy.yaml"
)


@dataclass
class RuntimeRoleProfile:
    role: str
    config: str = str(DEFAULT_POLICY)
    example: str = "YOLO 2x2"
    generated_policy_dir: str = "/tmp/ndnsf-di-gui-policy"
    group: str = ""
    provider_id: str = "A"
    roles: str = "all"
    service: str = "/AI/YOLO/2x2Inference"
    ack_timeout_ms: str = "1500"
    timeout_ms: str = "60000"
    extra_args: str = ""

    @classmethod
    def from_mapping(cls, role: str, data: dict[str, Any] | None) -> "RuntimeRoleProfile":
        profile = cls(role=role)
        if not isinstance(data, dict):
            return profile
        allowed = set(asdict(profile))
        values = {key: str(value) for key, value in data.items() if key in allowed}
        values["role"] = role
        return cls(**values)


@dataclass
class RuntimeGuiProfile:
    controller: RuntimeRoleProfile
    provider: RuntimeRoleProfile
    user: RuntimeRoleProfile

    @classmethod
    def default(cls) -> "RuntimeGuiProfile":
        return cls(
            controller=RuntimeRoleProfile(role="controller"),
            provider=RuntimeRoleProfile(role="provider"),
            user=RuntimeRoleProfile(role="user"),
        )

    @classmethod
    def from_mapping(cls, data: dict[str, Any]) -> "RuntimeGuiProfile":
        return cls(
            controller=RuntimeRoleProfile.from_mapping("controller", data.get("controller")),
            provider=RuntimeRoleProfile.from_mapping("provider", data.get("provider")),
            user=RuntimeRoleProfile.from_mapping("user", data.get("user")),
        )

    def to_mapping(self) -> dict[str, Any]:
        return {
            "controller": asdict(self.controller),
            "provider": asdict(self.provider),
            "user": asdict(self.user),
        }


def load_runtime_profile(path: str | Path) -> RuntimeGuiProfile:
    return RuntimeGuiProfile.from_mapping(json.loads(Path(path).read_text(encoding="utf-8")))


def write_runtime_profile(path: str | Path, profile: RuntimeGuiProfile) -> None:
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_text(json.dumps(profile.to_mapping(), indent=2), encoding="utf-8")


def split_extra_args(value: str) -> list[str]:
    return shlex.split(value) if value.strip() else []


@dataclass
class RoleProcessState:
    label: str
    status: str = "stopped"
    pid: int | None = None
    returncode: int | None = None

    def mark_starting(self) -> str:
        self.status = "starting"
        self.pid = None
        self.returncode = None
        return self.status

    def mark_running(self, pid: int | None) -> str:
        self.status = f"running pid={pid}" if pid is not None else "running"
        self.pid = pid
        self.returncode = None
        return self.status

    def mark_stopping(self) -> str:
        self.status = "stopping"
        return self.status

    def mark_exited(self, returncode: int) -> str:
        state = "exited" if returncode == 0 else "failed"
        self.status = f"{state} rc={returncode}"
        self.returncode = returncode
        self.pid = None
        return self.status


def build_role_command(
    *,
    role: str,
    script_path: str | Path,
    config: str,
    generated_policy_dir: str,
    group: str = "",
    provider_id: str = "",
    roles: str = "",
    ack_timeout_ms: str = "",
    timeout_ms: str = "",
    extra_args: str = "",
    python_executable: str = sys.executable,
) -> list[str]:
    args = [
        python_executable,
        str(script_path),
        "--config", config,
        "--generated-policy-dir", generated_policy_dir,
    ]
    if group and role != "controller":
        args.extend(["--group", group])
    if role == "provider":
        if provider_id:
            args.extend(["--provider-id", provider_id])
        if roles:
            args.extend(["--roles", roles])
    elif role == "user":
        if ack_timeout_ms:
            args.extend(["--ack-timeout-ms", ack_timeout_ms])
        if timeout_ms:
            args.extend(["--timeout-ms", timeout_ms])
    args.extend(split_extra_args(extra_args))
    return args


def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def run_command(args: list[str], *, cwd: Path | None = None) -> tuple[int, str]:
    proc = subprocess.run(
        args,
        cwd=str(cwd or repo_root()),
        text=True,
        capture_output=True,
    )
    output = proc.stdout
    if proc.stderr:
        output += ("\n" if output else "") + proc.stderr
    return proc.returncode, output


def load_policy_text(path: str | Path) -> str:
    return Path(path).read_text(encoding="utf-8")


def write_policy_text(path: str | Path, text: str) -> None:
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_text(text, encoding="utf-8")


def read_text_file(path: str | Path) -> str:
    return Path(path).read_text(encoding="utf-8")


def write_text_file(path: str | Path, text: str) -> None:
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_text(text, encoding="utf-8")


def summarize_policy_file(path: str | Path) -> str:
    out_dir = Path(tempfile.mkdtemp(prefix="ndnsf-di-gui-policy-"))
    deployment = load_or_generate_deployment(path, out_dir)
    return explain_policy(deployment)


def policy_service_names(path: str | Path) -> list[str]:
    config = load_config(path)
    return [
        str(service.get("name", ""))
        for service in config.get("services", [])
        if isinstance(service, dict) and service.get("name")
    ]


def make_basic_policy(
    *,
    application: str,
    controller: str,
    group: str,
    user_identity: str,
    provider_prefix: str,
    service_name: str,
    provider_ids: list[str],
    roles: list[str],
    model_path: str = "",
    model_kind: str = "onnx-model",
    backend: str = "onnxruntime",
) -> dict[str, Any]:
    providers = []
    prefix = provider_prefix.rstrip("/")
    if prefix:
        providers.append({"identity": prefix, "roles": "all"})
    for provider_id in provider_ids:
        provider_id = provider_id.strip("/")
        if provider_id:
            providers.append({"identity": f"{prefix}/{provider_id}", "roles": "all"})
    artifacts = []
    if model_path and roles:
        artifacts.append({
            "role": roles[0],
            "path": model_path,
            "artifact": service_name.rstrip("/") + "/ARTIFACT/" + roles[0].strip("/"),
            "filename": Path(model_path).name,
            "kind": model_kind,
            "backend": backend,
        })
    return {
        "application": application,
        "controller": controller,
        "group": group,
        "runtime": {
            "user_identity": user_identity,
            "provider_prefix": provider_prefix,
        },
        "trust": {
            "app_roots": ["/" + controller.strip("/").split("/")[0]]
            if controller.strip("/") else [],
        },
        "artifact_security": {
            "allowlist": [],
            "sandbox": {"kind": ""},
        },
        "authorization_summary": {
            "users": [{"identity": user_identity, "services": [service_name]}],
            "providers": [
                {"identity": provider["identity"], "services": [{
                    "service": service_name,
                    "roles": provider["roles"],
                }]}
                for provider in providers
            ],
        },
        "services": [{
            "name": service_name,
            "model": service_name.rstrip("/") + "/Model/v1",
            "users": [user_identity],
            "providers": providers,
            "roles": roles,
            "dependencies": _linear_dependencies(roles),
            "artifacts": artifacts,
            "input": {"codec": "npz"},
            "output": {"codec": "npz"},
        }],
    }


def _linear_dependencies(roles: list[str]) -> list[dict[str, Any]]:
    dependencies = []
    for index in range(len(roles) - 1):
        dependencies.append({
            "producers": [roles[index]],
            "consumers": [roles[index + 1]],
            "key_scope": f"stage{index}-to-stage{index + 1}",
            "topic_prefix": "/activation",
        })
    return dependencies


def policy_to_yaml(policy: dict[str, Any]) -> str:
    try:
        import yaml  # type: ignore
    except ImportError:
        return json.dumps(policy, indent=2)
    return yaml.safe_dump(policy, sort_keys=False)


class TextPane(ttk.Frame):
    def __init__(self, parent, *, height: int = 20):
        super().__init__(parent)
        self.text = tk.Text(self, wrap="word", height=height)
        scroll = ttk.Scrollbar(self, orient="vertical", command=self.text.yview)
        self.text.configure(yscrollcommand=scroll.set)
        self.text.pack(side="left", fill="both", expand=True)
        scroll.pack(side="right", fill="y")

    def set(self, value: str) -> None:
        self.text.delete("1.0", "end")
        self.text.insert("1.0", value)

    def get(self) -> str:
        return self.text.get("1.0", "end-1c")


class WizardTab(ttk.Frame):
    def __init__(self, parent, app: "DistributedInferenceGui"):
        super().__init__(parent)
        self.app = app
        self.fields: dict[str, tk.StringVar] = {}
        self._build()

    def _field(self, row: int, label: str, key: str, value: str = "",
               *, browse: bool = False) -> None:
        ttk.Label(self, text=label).grid(row=row, column=0, sticky="w", padx=6, pady=4)
        var = tk.StringVar(value=value)
        self.fields[key] = var
        entry = ttk.Entry(self, textvariable=var)
        entry.grid(row=row, column=1, sticky="ew", padx=6, pady=4)
        if browse:
            ttk.Button(self, text="Browse", command=lambda: self._browse_file(key)).grid(
                row=row, column=2, sticky="ew", padx=6, pady=4)

    def _build(self) -> None:
        self.columnconfigure(1, weight=1)
        self._field(0, "Model file", "model", "", browse=True)
        self._field(1, "Application", "application", "di-gui-project")
        self._field(2, "Service name", "service", "/AI/Model/Inference")
        self._field(3, "Controller", "controller", "/NDNSF-DistributeInference/example/controller")
        self._field(4, "Group", "group", "/NDNSF-DistributeInference/example/group")
        self._field(5, "Runtime user identity", "user", "/NDNSF-DistributeInference/example/user")
        self._field(6, "Provider prefix", "provider_prefix", "/NDNSF-DistributeInference/example/provider")
        self._field(7, "Provider IDs", "provider_ids", "A,B")
        self._field(8, "Roles", "roles", "/Stage/0,/Stage/1")
        self._field(9, "Output policy", "output", "examples/python/NDNSF-DistributedInference/gui_policy.yaml",
                    browse=True)
        buttons = ttk.Frame(self)
        buttons.grid(row=10, column=0, columnspan=3, sticky="ew", padx=6, pady=8)
        ttk.Button(buttons, text="Generate Policy", command=self.generate_policy).pack(side="left")
        ttk.Button(buttons, text="Open in Policy Editor",
                   command=self.generate_to_editor).pack(side="left", padx=6)
        self.preview = TextPane(self, height=18)
        self.preview.grid(row=11, column=0, columnspan=3, sticky="nsew", padx=6, pady=6)
        self.rowconfigure(11, weight=1)

    def _browse_file(self, key: str) -> None:
        if key == "output":
            path = filedialog.asksaveasfilename(
                title="Policy YAML",
                defaultextension=".yaml",
                filetypes=[("YAML", "*.yaml *.yml"), ("JSON", "*.json"), ("All files", "*")],
            )
        else:
            path = filedialog.askopenfilename(title="Select file")
        if path:
            self.fields[key].set(path)

    def _policy_text(self) -> str:
        policy = make_basic_policy(
            application=self.fields["application"].get(),
            controller=self.fields["controller"].get(),
            group=self.fields["group"].get(),
            user_identity=self.fields["user"].get(),
            provider_prefix=self.fields["provider_prefix"].get(),
            service_name=self.fields["service"].get(),
            provider_ids=[item.strip() for item in self.fields["provider_ids"].get().split(",")],
            roles=[item.strip() for item in self.fields["roles"].get().split(",") if item.strip()],
            model_path=self.fields["model"].get(),
        )
        return policy_to_yaml(policy)

    def generate_policy(self) -> None:
        text = self._policy_text()
        output = self.fields["output"].get()
        if output:
            write_policy_text(output, text)
            self.app.set_status(f"Generated policy: {output}")
        self.preview.set(text)

    def generate_to_editor(self) -> None:
        text = self._policy_text()
        self.preview.set(text)
        self.app.policy_editor.set_policy_text(text)
        output = self.fields["output"].get()
        if output:
            self.app.policy_editor.path_var.set(output)
        self.app.select_tab("Policy Editor")


class PolicyEditorTab(ttk.Frame):
    def __init__(self, parent, app: "DistributedInferenceGui"):
        super().__init__(parent)
        self.app = app
        self.path_var = tk.StringVar(value=str(DEFAULT_POLICY))
        self._build()

    def _build(self) -> None:
        self.columnconfigure(1, weight=1)
        self.rowconfigure(1, weight=1)
        top = ttk.Frame(self)
        top.grid(row=0, column=0, columnspan=3, sticky="ew", padx=6, pady=4)
        ttk.Label(top, text="Policy").pack(side="left")
        ttk.Entry(top, textvariable=self.path_var).pack(side="left", fill="x", expand=True, padx=6)
        ttk.Button(top, text="Open", command=self.open_policy).pack(side="left")
        ttk.Button(top, text="Save", command=self.save_policy).pack(side="left", padx=4)
        ttk.Button(top, text="Validate", command=self.validate_policy).pack(side="left")
        ttk.Button(top, text="Explain", command=self.explain_policy).pack(side="left", padx=4)

        self.tree = ttk.Treeview(self, columns=("kind",), show="tree")
        self.tree.grid(row=1, column=0, sticky="nsew", padx=6, pady=6)
        self.editor = TextPane(self)
        self.editor.grid(row=1, column=1, sticky="nsew", padx=6, pady=6)
        self.summary = TextPane(self)
        self.summary.grid(row=1, column=2, sticky="nsew", padx=6, pady=6)
        self.columnconfigure(0, weight=1)
        self.columnconfigure(1, weight=3)
        self.columnconfigure(2, weight=2)
        self.open_policy()

    def set_policy_text(self, text: str) -> None:
        self.editor.set(text)
        self.refresh_tree_from_text(text)

    def open_policy(self) -> None:
        path = self.path_var.get()
        if not path or not Path(path).exists():
            path = filedialog.askopenfilename(
                title="Open policy",
                filetypes=[("YAML/JSON", "*.yaml *.yml *.json"), ("All files", "*")],
            )
            if not path:
                return
            self.path_var.set(path)
        text = load_policy_text(path)
        self.set_policy_text(text)
        self.app.set_status(f"Loaded policy: {path}")

    def save_policy(self) -> None:
        path = self.path_var.get() or filedialog.asksaveasfilename(
            title="Save policy",
            defaultextension=".yaml",
        )
        if not path:
            return
        write_policy_text(path, self.editor.get())
        self.path_var.set(path)
        self.app.set_status(f"Saved policy: {path}")

    def validate_policy(self) -> None:
        self.save_policy()
        try:
            summary = summarize_policy_file(self.path_var.get())
        except Exception as exc:
            self.summary.set(f"Validation failed:\n{exc}")
            messagebox.showerror("Policy validation failed", str(exc))
            return
        self.summary.set(summary)
        self.refresh_tree_from_path(self.path_var.get())
        self.app.set_status("Policy validation passed")

    def explain_policy(self) -> None:
        self.validate_policy()

    def refresh_tree_from_text(self, text: str) -> None:
        tmp = Path(tempfile.mkdtemp(prefix="ndnsf-di-gui-open-")) / "policy.yaml"
        tmp.write_text(text, encoding="utf-8")
        self.refresh_tree_from_path(tmp)

    def refresh_tree_from_path(self, path: str | Path) -> None:
        for item in self.tree.get_children():
            self.tree.delete(item)
        try:
            config = load_config(path)
        except Exception:
            return
        users_node = self.tree.insert("", "end", text="Users", open=True)
        providers_node = self.tree.insert("", "end", text="Providers", open=True)
        services_node = self.tree.insert("", "end", text="Services", open=True)
        users = set()
        providers = set()
        for service in config.get("services", []) or []:
            if not isinstance(service, dict):
                continue
            service_node = self.tree.insert(services_node, "end", text=str(service.get("name", "")))
            for role in service.get("roles", []) or []:
                self.tree.insert(service_node, "end", text=f"role {role}")
            for user in service.get("users", []) or []:
                users.add(str(user))
            for provider in service.get("providers", []) or []:
                if isinstance(provider, dict):
                    providers.add(str(provider.get("identity", "")))
        for user in sorted(users):
            self.tree.insert(users_node, "end", text=user)
        for provider in sorted(providers):
            self.tree.insert(providers_node, "end", text=provider)


class ModelSplitTab(ttk.Frame):
    def __init__(self, parent, app: "DistributedInferenceGui"):
        super().__init__(parent)
        self.app = app
        self.model_var = tk.StringVar()
        self._build()

    def _build(self) -> None:
        self.columnconfigure(1, weight=1)
        self.rowconfigure(1, weight=1)
        top = ttk.Frame(self)
        top.grid(row=0, column=0, columnspan=2, sticky="ew", padx=6, pady=4)
        ttk.Label(top, text="ONNX model").pack(side="left")
        ttk.Entry(top, textvariable=self.model_var).pack(side="left", fill="x", expand=True, padx=6)
        ttk.Button(top, text="Browse", command=self.browse).pack(side="left")
        ttk.Button(top, text="Analyze", command=self.analyze).pack(side="left", padx=4)
        ttk.Button(top, text="Use Top 2-Stage Policy Skeleton",
                   command=self.use_two_stage).pack(side="left")
        self.output = TextPane(self)
        self.output.grid(row=1, column=0, columnspan=2, sticky="nsew", padx=6, pady=6)

    def browse(self) -> None:
        path = filedialog.askopenfilename(
            title="Select ONNX model",
            filetypes=[("ONNX", "*.onnx"), ("All files", "*")],
        )
        if path:
            self.model_var.set(path)

    def analyze(self) -> None:
        path = self.model_var.get()
        if not path:
            messagebox.showwarning("Missing model", "Select an ONNX model first.")
            return
        try:
            summary = analyze_onnx_graph(path)
            candidates = estimate_split_candidates(summary, max_candidates=10)
        except Exception as exc:
            self.output.set(f"ONNX analysis failed:\n{exc}")
            return
        lines = [
            f"Model: {path}",
            f"Inputs: {', '.join(summary.inputs)}",
            f"Outputs: {', '.join(summary.outputs)}",
            f"Nodes: {len(summary.nodes)}",
            "",
            "Top split candidates:",
        ]
        for candidate in candidates[:10]:
            lines.append(
                f"cut_after_node={candidate.cut_after_node} "
                f"boundary_tensors={len(candidate.boundary_tensors)} "
                f"known_boundary_bytes={candidate.known_boundary_bytes} "
                f"unknown_size_tensors={len(candidate.unknown_size_tensors)}")
        self.output.set("\n".join(lines))

    def use_two_stage(self) -> None:
        self.app.wizard.fields["model"].set(self.model_var.get())
        self.app.wizard.fields["roles"].set("/Stage/0,/Stage/1")
        self.app.select_tab("Project Wizard")


class CertificateTab(ttk.Frame):
    def __init__(self, parent, app: "DistributedInferenceGui"):
        super().__init__(parent)
        self.app = app
        self.identity_var = tk.StringVar()
        self.request_path_var = tk.StringVar(value="/tmp/ndnsf-di-identity.req")
        self.safebag_var = tk.StringVar()
        self.password_var = tk.StringVar()
        self._build()

    def _build(self) -> None:
        self.columnconfigure(1, weight=1)
        row = 0
        ttk.Button(self, text="Refresh ndnsec list", command=self.refresh).grid(
            row=row, column=0, sticky="ew", padx=6, pady=4)
        ttk.Button(self, text="Use Selected As Runtime User",
                   command=self.use_selected_identity).grid(
            row=row, column=1, sticky="ew", padx=6, pady=4)
        row += 1
        self.identities = tk.Listbox(self, height=8)
        self.identities.grid(row=row, column=0, columnspan=2, sticky="nsew", padx=6, pady=4)
        self.rowconfigure(row, weight=1)
        row += 1
        ttk.Label(self, text="Identity").grid(row=row, column=0, sticky="w", padx=6)
        ttk.Entry(self, textvariable=self.identity_var).grid(row=row, column=1, sticky="ew", padx=6)
        row += 1
        ttk.Label(self, text="Key request output").grid(row=row, column=0, sticky="w", padx=6)
        ttk.Entry(self, textvariable=self.request_path_var).grid(row=row, column=1, sticky="ew", padx=6)
        row += 1
        ttk.Button(self, text="Generate Key Request",
                   command=self.generate_key_request).grid(row=row, column=0, columnspan=2,
                                                           sticky="ew", padx=6, pady=4)
        row += 1
        ttk.Label(self, text="Safebag file").grid(row=row, column=0, sticky="w", padx=6)
        ttk.Entry(self, textvariable=self.safebag_var).grid(row=row, column=1, sticky="ew", padx=6)
        row += 1
        ttk.Label(self, text="Safebag password").grid(row=row, column=0, sticky="w", padx=6)
        ttk.Entry(self, textvariable=self.password_var, show="*").grid(row=row, column=1,
                                                                       sticky="ew", padx=6)
        row += 1
        ttk.Button(self, text="Import Safebag",
                   command=self.import_safebag).grid(row=row, column=0, columnspan=2,
                                                     sticky="ew", padx=6, pady=4)
        row += 1
        self.output = TextPane(self, height=10)
        self.output.grid(row=row, column=0, columnspan=2, sticky="nsew", padx=6, pady=6)
        self.rowconfigure(row, weight=1)
        self.refresh()

    def refresh(self) -> None:
        code, output = run_command(["ndnsec", "list"])
        self.output.set(output or f"ndnsec list exited with {code}")
        self.identities.delete(0, "end")
        for line in output.splitlines():
            value = line.strip()
            if value.startswith("/"):
                self.identities.insert("end", value.split()[0])

    def _selected_identity(self) -> str:
        selection = self.identities.curselection()
        if selection:
            return self.identities.get(selection[0])
        return self.identity_var.get()

    def use_selected_identity(self) -> None:
        identity = self._selected_identity()
        if identity:
            self.identity_var.set(identity)
            self.app.wizard.fields["user"].set(identity)
            self.app.set_status(f"Selected runtime user identity: {identity}")

    def generate_key_request(self) -> None:
        identity = self.identity_var.get()
        output_path = self.request_path_var.get()
        if not identity or not output_path:
            messagebox.showwarning("Missing fields", "Identity and output path are required.")
            return
        code, output = run_command(["ndnsec", "key-gen", "-n", "-t", "r", identity])
        if code == 0:
            write_policy_text(output_path, output)
            self.output.set(f"Wrote key request to {output_path}\n\n{output}")
        else:
            self.output.set(output)

    def import_safebag(self) -> None:
        safebag = self.safebag_var.get()
        password = self.password_var.get()
        if not safebag or not password:
            messagebox.showwarning("Missing fields", "Safebag file and password are required.")
            return
        code, output = run_command(["ndnsec", "import", "-P", password, safebag])
        self.output.set(output or f"ndnsec import exited with {code}")
        self.refresh()


class DeploymentRunnerTab(ttk.Frame):
    REGRESSION_CASES = {
        "auto-split": ("YOLO_SPLIT_RESULT", "ok=true"),
        "yolo-2x2": ("YOLO_2X2_RESULT", "ok=true"),
        "yolo-layout": ("YOLO_LAYOUT_DYNAMIC_PROVISIONING_MININDN_OK", ""),
        "yolo-layout-local": ("YOLO_LAYOUT_SMOKE_OK", ""),
        "onnx-executor": ("ONNX_EXECUTOR_FANIN_FANOUT_OK", ""),
        "app-api": ("APP_API_SERVICE_PLAN_OK", ""),
        "all": ("NDNSF_DI_REGRESSION_SUITE_OK", ""),
    }

    def __init__(self, parent, app: "DistributedInferenceGui"):
        super().__init__(parent)
        self.app = app
        self.config_var = tk.StringVar(value=str(DEFAULT_POLICY))
        self.provider_id_var = tk.StringVar(value="A")
        self.regression_case_var = tk.StringVar(value="yolo-2x2")
        self.profile_path_var = tk.StringVar(value="examples/python/NDNSF-DistributedInference/gui_runtime_profile.json")
        self.processes: dict[str, subprocess.Popen[str]] = {}
        self.process_states: dict[str, RoleProcessState] = {}
        self.status_callbacks: dict[str, Callable[[str], None]] = {}
        self.queue: queue.Queue[str | tuple[str, str] | tuple[str, str, str]] = queue.Queue()
        self._build()
        self.after(200, self._drain_queue)

    def _build(self) -> None:
        self.columnconfigure(1, weight=1)
        ttk.Label(self, text="Config").grid(row=0, column=0, sticky="w", padx=6, pady=4)
        ttk.Entry(self, textvariable=self.config_var).grid(row=0, column=1, sticky="ew", padx=6)
        ttk.Button(self, text="Browse", command=self.browse).grid(row=0, column=2, padx=6)
        ttk.Label(self, text="Provider ID").grid(row=1, column=0, sticky="w", padx=6)
        ttk.Entry(self, textvariable=self.provider_id_var).grid(row=1, column=1, sticky="ew", padx=6)
        ttk.Label(self, text="Regression").grid(row=2, column=0, sticky="w", padx=6, pady=4)
        ttk.Combobox(
            self,
            textvariable=self.regression_case_var,
            values=list(self.REGRESSION_CASES.keys()),
            state="readonly",
        ).grid(row=2, column=1, sticky="ew", padx=6, pady=4)
        buttons = ttk.Frame(self)
        buttons.grid(row=3, column=0, columnspan=3, sticky="ew", padx=6, pady=4)
        ttk.Button(buttons, text="Run Controller", command=self.run_controller).pack(side="left")
        ttk.Button(buttons, text="Run Provider", command=self.run_provider).pack(side="left", padx=4)
        ttk.Button(buttons, text="Run User", command=self.run_user).pack(side="left")
        ttk.Button(buttons, text="Run Selected Regression",
                   command=self.run_selected_regression).pack(side="left", padx=4)
        ttk.Button(buttons, text="Run YOLO 2x2 MiniNDN Smoke",
                   command=self.run_yolo_2x2_smoke).pack(side="left", padx=4)
        ttk.Button(buttons, text="Stop Processes", command=self.stop_processes).pack(side="left")
        profile = ttk.Frame(self)
        profile.grid(row=4, column=0, columnspan=3, sticky="ew", padx=6, pady=4)
        ttk.Label(profile, text="Runtime profile").pack(side="left")
        ttk.Entry(profile, textvariable=self.profile_path_var).pack(side="left", fill="x",
                                                                    expand=True, padx=6)
        ttk.Button(profile, text="Load Profile", command=self.load_profile).pack(side="left")
        ttk.Button(profile, text="Save Profile", command=self.save_profile).pack(side="left", padx=4)
        ttk.Button(profile, text="Start All", command=self.start_all).pack(side="left")
        ttk.Button(profile, text="Stop All", command=self.stop_processes).pack(side="left", padx=4)
        ttk.Button(profile, text="Clear Logs", command=self.clear_logs).pack(side="left")
        self.log = TextPane(self, height=25)
        self.log.grid(row=5, column=0, columnspan=3, sticky="nsew", padx=6, pady=6)
        self.rowconfigure(5, weight=1)

    def browse(self) -> None:
        path = filedialog.askopenfilename(
            title="Select config",
            filetypes=[("YAML/JSON", "*.yaml *.yml *.json"), ("All files", "*")],
        )
        if path:
            self.config_var.set(path)

    def _script_for_config(self, role: str) -> list[str]:
        config = self.config_var.get()
        if "yolo_split" in config:
            base = "examples/python/NDNSF-DistributedInference/yolo_split"
        elif "pytorch_eager_2x2" in config:
            base = "examples/python/NDNSF-DistributedInference/pytorch_eager_2x2"
        else:
            base = "examples/python/NDNSF-DistributedInference/yolo_2x2"
        script = repo_root() / base / f"{role}.py"
        args = [sys.executable, str(script), "--config", config]
        if role == "provider":
            args.extend(["--provider-id", self.provider_id_var.get()])
        return args

    def run_controller(self) -> None:
        self._start(self._script_for_config("controller"), "controller")

    def run_provider(self) -> None:
        self._start(self._script_for_config("provider"), "provider")

    def run_user(self) -> None:
        self._start(self._script_for_config("user"), "user")

    def run_selected_regression(self) -> None:
        case = self.regression_case_var.get()
        self._append(
            f"Running DI regression case '{case}'. The selected case starts "
            "MiniNDN, runs distributed inference, and checks its success marker.\n"
        )
        self._start(
            [sys.executable, "Experiments/NDNSF_DI_Run_Minindn_Regressions.py",
             "--case", case],
            f"regression-{case}",
            success_markers=self.REGRESSION_CASES.get(case),
        )

    def run_yolo_2x2_smoke(self) -> None:
        self._start([sys.executable, "Experiments/NDNSF_DI_Yolo2x2_Minindn.py"],
                    "yolo-2x2-minindn",
                    success_markers=("YOLO_2X2_RESULT", "ok=true"))

    def register_status_callback(self, label: str, callback: Callable[[str], None]) -> None:
        self.status_callbacks[label] = callback

    def _start(self, args: list[str], label: str,
               success_markers: tuple[str, str] | None = None) -> None:
        if label in self.processes and self.processes[label].poll() is None:
            self.queue.put(("__STATUS__", f"{label} is already running"))
            self.queue.put(("__ROLE_STATUS__", label, "running"))
            return
        self._append(f"$ {' '.join(args)}\n")
        state = self.process_states.setdefault(label, RoleProcessState(label))
        self.queue.put(("__ROLE_STATUS__", label, state.mark_starting()))
        proc = subprocess.Popen(
            args,
            cwd=str(repo_root()),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        self.processes[label] = proc
        self.queue.put(("__ROLE_STATUS__", label, state.mark_running(proc.pid)))
        threading.Thread(
            target=self._read_process,
            args=(proc, label, success_markers),
            daemon=True,
        ).start()

    def _read_process(self, proc: subprocess.Popen[str], label: str,
                      success_markers: tuple[str, str] | None = None) -> None:
        assert proc.stdout is not None
        saw_first = False
        saw_second = False
        for line in proc.stdout:
            self.queue.put(f"[{label}] {line}")
            if success_markers is not None:
                first, second = success_markers
                saw_first = saw_first or bool(first and first in line)
                saw_second = saw_second or not second or second in line
        proc.wait()
        self.queue.put(f"[{label}] exited with {proc.returncode}\n")
        state = self.process_states.setdefault(label, RoleProcessState(label))
        self.queue.put(("__ROLE_STATUS__", label, state.mark_exited(proc.returncode)))
        if success_markers is not None:
            if proc.returncode == 0 and saw_first and saw_second:
                self.queue.put(f"[{label}] RESULT ok=true\n")
                self.queue.put(("__STATUS__", f"{label} completed: ok=true"))
            else:
                self.queue.put(
                    f"[{label}] RESULT ok=false expected={success_markers} "
                    f"returncode={proc.returncode}\n"
                )
                self.queue.put(("__STATUS__", f"{label} completed: ok=false"))

    def _drain_queue(self) -> None:
        while True:
            try:
                item = self.queue.get_nowait()
            except queue.Empty:
                break
            if isinstance(item, tuple) and item and item[0] == "__STATUS__":
                self.app.set_status(str(item[1]))
            elif isinstance(item, tuple) and item and item[0] == "__ROLE_STATUS__":
                _, label, status = item
                callback = self.status_callbacks.get(str(label))
                if callback is not None:
                    callback(str(status))
            else:
                self._append(str(item))
        self.after(200, self._drain_queue)

    def _append(self, text: str) -> None:
        self.log.text.insert("end", text)
        self.log.text.see("end")

    def stop_processes(self) -> None:
        for label, proc in list(self.processes.items()):
            if proc.poll() is None:
                state = self.process_states.setdefault(label, RoleProcessState(label))
                self.queue.put(("__ROLE_STATUS__", label, state.mark_stopping()))
                proc.terminate()
        self.processes = {
            label: proc for label, proc in self.processes.items()
            if proc.poll() is None
        }
        self._append("Stop requested for running processes.\n")

    def stop_role(self, label: str) -> None:
        proc = self.processes.get(label)
        if proc is None or proc.poll() is not None:
            self.queue.put(("__ROLE_STATUS__", label, "stopped"))
            return
        state = self.process_states.setdefault(label, RoleProcessState(label))
        self.queue.put(("__ROLE_STATUS__", label, state.mark_stopping()))
        proc.terminate()
        self._append(f"Stop requested for {label}.\n")

    def clear_logs(self) -> None:
        self.log.set("")

    def start_all(self) -> None:
        self.app.controller_runtime.run_role()
        self.app.provider_runtime.run_role()
        self.app.user_runtime.run_role()

    def profile(self) -> RuntimeGuiProfile:
        return RuntimeGuiProfile(
            controller=self.app.controller_runtime.profile(),
            provider=self.app.provider_runtime.profile(),
            user=self.app.user_runtime.profile(),
        )

    def apply_profile(self, profile: RuntimeGuiProfile) -> None:
        self.app.controller_runtime.apply_profile(profile.controller)
        self.app.provider_runtime.apply_profile(profile.provider)
        self.app.user_runtime.apply_profile(profile.user)
        self.config_var.set(profile.provider.config)
        self.provider_id_var.set(profile.provider.provider_id)
        self.app.set_status("Runtime profile loaded")

    def load_profile(self) -> None:
        path = self.profile_path_var.get().strip()
        if not path or not Path(path).exists():
            path = filedialog.askopenfilename(
                title="Load runtime profile",
                filetypes=[("JSON", "*.json"), ("All files", "*")],
            )
            if not path:
                return
            self.profile_path_var.set(path)
        try:
            self.apply_profile(load_runtime_profile(path))
        except Exception as exc:
            messagebox.showerror("Load profile failed", str(exc))

    def save_profile(self) -> None:
        path = self.profile_path_var.get().strip()
        if not path:
            path = filedialog.asksaveasfilename(
                title="Save runtime profile",
                defaultextension=".json",
                filetypes=[("JSON", "*.json"), ("All files", "*")],
            )
            if not path:
                return
            self.profile_path_var.set(path)
        try:
            write_runtime_profile(path, self.profile())
        except Exception as exc:
            messagebox.showerror("Save profile failed", str(exc))
            return
        self.app.set_status(f"Saved runtime profile: {path}")


class ControllerCertificateFrame(ttk.LabelFrame):
    """Controller-side root and certificate signing helper.

    This widget intentionally wraps ndnsec commands instead of inventing a new
    certificate format. Operators can paste a key request from a User/Provider
    tab, sign it with the controller/root identity, and copy the signed
    certificate back to the requester.
    """

    def __init__(self, parent):
        super().__init__(parent, text="Controller certificate authority")
        self.root_identity_var = tk.StringVar(value="/NDNSF-DistributeInference/example")
        self.root_cert_path_var = tk.StringVar(value="/tmp/ndnsf-di-root.cert")
        self.issuer_id_var = tk.StringVar(value="ROOT")
        self.request_path_var = tk.StringVar()
        self.signed_cert_path_var = tk.StringVar(value="/tmp/ndnsf-di-signed.cert")
        self._build()

    def _build(self) -> None:
        self.columnconfigure(1, weight=1)
        row = 0
        self._entry(row, "Root identity", self.root_identity_var)
        row += 1
        self._entry(row, "Root cert output", self.root_cert_path_var, browse_save=True)
        row += 1
        ttk.Button(self, text="Generate / Refresh Root Cert",
                   command=self.generate_root_cert).grid(row=row, column=0, columnspan=3,
                                                         sticky="ew", padx=6, pady=4)
        row += 1
        self._entry(row, "Issuer ID", self.issuer_id_var)
        row += 1
        self._entry(row, "Key request file", self.request_path_var, browse_open=True)
        row += 1
        ttk.Label(self, text="Pasted key request").grid(row=row, column=0, sticky="nw", padx=6)
        self.request_text = tk.Text(self, height=5, wrap="word")
        self.request_text.grid(row=row, column=1, columnspan=2, sticky="nsew", padx=6, pady=4)
        row += 1
        self._entry(row, "Signed cert output", self.signed_cert_path_var, browse_save=True)
        row += 1
        ttk.Button(self, text="Sign Request",
                   command=self.sign_request).grid(row=row, column=0, columnspan=3,
                                                   sticky="ew", padx=6, pady=4)
        row += 1
        ttk.Label(self, text="Signed certificate").grid(row=row, column=0, sticky="nw", padx=6)
        self.output_text = tk.Text(self, height=5, wrap="word")
        self.output_text.grid(row=row, column=1, columnspan=2, sticky="nsew", padx=6, pady=4)
        self.rowconfigure(row, weight=1)

    def _entry(self, row: int, label: str, variable: tk.StringVar,
               *, browse_open: bool = False, browse_save: bool = False) -> None:
        ttk.Label(self, text=label).grid(row=row, column=0, sticky="w", padx=6, pady=4)
        ttk.Entry(self, textvariable=variable).grid(row=row, column=1, sticky="ew", padx=6, pady=4)
        if browse_open:
            ttk.Button(self, text="Browse", command=lambda: self._browse_open(variable)).grid(
                row=row, column=2, padx=6, pady=4)
        elif browse_save:
            ttk.Button(self, text="Save As", command=lambda: self._browse_save(variable)).grid(
                row=row, column=2, padx=6, pady=4)

    def _browse_open(self, variable: tk.StringVar) -> None:
        path = filedialog.askopenfilename(title="Select key request")
        if path:
            variable.set(path)
            try:
                self.request_text.delete("1.0", "end")
                self.request_text.insert("1.0", read_text_file(path))
            except OSError:
                pass

    def _browse_save(self, variable: tk.StringVar) -> None:
        path = filedialog.asksaveasfilename(title="Select output file")
        if path:
            variable.set(path)

    def generate_root_cert(self) -> None:
        identity = self.root_identity_var.get().strip()
        output_path = self.root_cert_path_var.get().strip()
        if not identity or not output_path:
            messagebox.showwarning("Missing fields", "Root identity and output path are required.")
            return
        code, output = run_command(["ndnsec", "key-gen", "-t", "r", identity])
        if code == 0:
            write_text_file(output_path, output)
            self._set_output(f"Wrote root cert to {output_path}\n\n{output}")
        else:
            self._set_output(output or f"ndnsec key-gen exited with {code}")

    def sign_request(self) -> None:
        signer = self.root_identity_var.get().strip()
        issuer = self.issuer_id_var.get().strip() or "ROOT"
        output_path = self.signed_cert_path_var.get().strip()
        request_text = self.request_text.get("1.0", "end-1c").strip()
        request_path = self.request_path_var.get().strip()
        if not signer or not output_path:
            messagebox.showwarning("Missing fields", "Root identity and signed cert output are required.")
            return
        if request_text:
            tmp = tempfile.NamedTemporaryFile("w", encoding="utf-8", delete=False,
                                             prefix="ndnsf-di-csr-", suffix=".req")
            with tmp:
                tmp.write(request_text)
            request_path = tmp.name
        if not request_path:
            messagebox.showwarning("Missing request", "Paste a request or select a request file.")
            return
        code, output = run_command(["ndnsec", "cert-gen", "-s", signer, "-i", issuer, request_path])
        if code == 0:
            write_text_file(output_path, output)
            self._set_output(f"Wrote signed cert to {output_path}\n\n{output}")
        else:
            self._set_output(output or f"ndnsec cert-gen exited with {code}")

    def _set_output(self, value: str) -> None:
        self.output_text.delete("1.0", "end")
        self.output_text.insert("1.0", value)


class ParticipantCertificateFrame(ttk.LabelFrame):
    """User/Provider-side key request and certificate install helper."""

    def __init__(self, parent, role: str):
        super().__init__(parent, text=f"{role.title()} certificate request / install")
        role_suffix = role.lower()
        self.identity_var = tk.StringVar(value=f"/NDNSF-DistributeInference/example/{role_suffix}")
        self.request_path_var = tk.StringVar(value=f"/tmp/ndnsf-di-{role_suffix}.req")
        self.cert_path_var = tk.StringVar(value=f"/tmp/ndnsf-di-{role_suffix}.cert")
        self._build()

    def _build(self) -> None:
        self.columnconfigure(1, weight=1)
        row = 0
        self._entry(row, "Identity", self.identity_var)
        row += 1
        self._entry(row, "Key request output", self.request_path_var, browse_save=True)
        row += 1
        ttk.Button(self, text="Generate Key Request",
                   command=self.generate_key_request).grid(row=row, column=0, columnspan=3,
                                                           sticky="ew", padx=6, pady=4)
        row += 1
        ttk.Label(self, text="Copy this request to Controller").grid(row=row, column=0,
                                                                     sticky="nw", padx=6)
        self.request_text = tk.Text(self, height=5, wrap="word")
        self.request_text.grid(row=row, column=1, columnspan=2, sticky="nsew", padx=6, pady=4)
        row += 1
        self._entry(row, "Signed cert file", self.cert_path_var, browse_open=True)
        row += 1
        ttk.Label(self, text="Or paste signed cert").grid(row=row, column=0, sticky="nw", padx=6)
        self.cert_text = tk.Text(self, height=5, wrap="word")
        self.cert_text.grid(row=row, column=1, columnspan=2, sticky="nsew", padx=6, pady=4)
        row += 1
        ttk.Button(self, text="Install Signed Cert",
                   command=self.install_signed_cert).grid(row=row, column=0, columnspan=3,
                                                          sticky="ew", padx=6, pady=4)
        row += 1
        self.status = tk.StringVar(value="")
        ttk.Label(self, textvariable=self.status, anchor="w").grid(row=row, column=0,
                                                                   columnspan=3, sticky="ew",
                                                                   padx=6, pady=4)

    def _entry(self, row: int, label: str, variable: tk.StringVar,
               *, browse_open: bool = False, browse_save: bool = False) -> None:
        ttk.Label(self, text=label).grid(row=row, column=0, sticky="w", padx=6, pady=4)
        ttk.Entry(self, textvariable=variable).grid(row=row, column=1, sticky="ew", padx=6, pady=4)
        if browse_open:
            ttk.Button(self, text="Browse", command=lambda: self._browse_open(variable)).grid(
                row=row, column=2, padx=6, pady=4)
        elif browse_save:
            ttk.Button(self, text="Save As", command=lambda: self._browse_save(variable)).grid(
                row=row, column=2, padx=6, pady=4)

    def _browse_open(self, variable: tk.StringVar) -> None:
        path = filedialog.askopenfilename(title="Select signed certificate")
        if path:
            variable.set(path)
            try:
                self.cert_text.delete("1.0", "end")
                self.cert_text.insert("1.0", read_text_file(path))
            except OSError:
                pass

    def _browse_save(self, variable: tk.StringVar) -> None:
        path = filedialog.asksaveasfilename(title="Select output file")
        if path:
            variable.set(path)

    def generate_key_request(self) -> None:
        identity = self.identity_var.get().strip()
        output_path = self.request_path_var.get().strip()
        if not identity or not output_path:
            messagebox.showwarning("Missing fields", "Identity and output path are required.")
            return
        code, output = run_command(["ndnsec", "key-gen", "-n", "-t", "r", identity])
        if code == 0:
            write_text_file(output_path, output)
            self.request_text.delete("1.0", "end")
            self.request_text.insert("1.0", output)
            self.status.set(f"Wrote key request to {output_path}")
        else:
            self.status.set(output or f"ndnsec key-gen exited with {code}")

    def install_signed_cert(self) -> None:
        cert_text = self.cert_text.get("1.0", "end-1c").strip()
        cert_path = self.cert_path_var.get().strip()
        if cert_text:
            write_text_file(cert_path, cert_text)
        if not cert_path:
            messagebox.showwarning("Missing certificate", "Paste or select a signed certificate.")
            return
        code, output = run_command(["ndnsec", "cert-install", "-f", cert_path])
        self.status.set(output or f"ndnsec cert-install exited with {code}")


class RoleRuntimeTab(ttk.Frame):
    """Role-specific APP runtime launcher.

    One physical node can run any combination of these roles. The tab only
    prepares and launches the corresponding APP-level process; permissions,
    identities, artifacts, and service dependency graph still come from the
    selected policy file.
    """

    EXAMPLE_BASES = {
        "YOLO 2-stage": "examples/python/NDNSF-DistributedInference/yolo_split",
        "YOLO 2x2": "examples/python/NDNSF-DistributedInference/yolo_2x2",
        "PyTorch 2x2": "examples/python/NDNSF-DistributedInference/pytorch_eager_2x2",
    }

    def __init__(self, parent, app: "DistributedInferenceGui", role: str):
        super().__init__(parent)
        self.app = app
        self.role = role
        self.config_var = tk.StringVar(value=str(DEFAULT_POLICY))
        self.example_var = tk.StringVar(value="YOLO 2x2")
        self.generated_dir_var = tk.StringVar(value="/tmp/ndnsf-di-gui-policy")
        self.group_var = tk.StringVar(value="")
        self.provider_id_var = tk.StringVar(value="A")
        self.roles_var = tk.StringVar(value="all")
        self.service_var = tk.StringVar(value="/AI/YOLO/2x2Inference")
        self.ack_timeout_var = tk.StringVar(value="1500")
        self.timeout_var = tk.StringVar(value="60000")
        self.extra_args_var = tk.StringVar(value="")
        self.status_var = tk.StringVar(value="stopped")
        if role == "controller":
            self.identity_tools: ttk.Widget = ControllerCertificateFrame(self)
        else:
            self.identity_tools = ParticipantCertificateFrame(self, role)
        self._build()
        self.app.runner.register_status_callback(role, self.status_var.set)

    def _build(self) -> None:
        self.columnconfigure(1, weight=1)
        row = 0
        ttk.Label(self, text=f"{self.role.title()} runtime").grid(
            row=row, column=0, columnspan=3, sticky="w", padx=6, pady=6)
        row += 1
        self._entry(row, "Policy config", self.config_var, browse=True)
        row += 1
        self._combo(row, "Example app", self.example_var, list(self.EXAMPLE_BASES))
        row += 1
        self._entry(row, "Generated policy dir", self.generated_dir_var)
        row += 1
        self._entry(row, "SVS group override", self.group_var)
        row += 1

        if self.role == "controller":
            self._entry(row, "Extra controller args", self.extra_args_var)
            row += 1
        elif self.role == "provider":
            self._entry(row, "Provider ID", self.provider_id_var)
            row += 1
            self._entry(row, "Roles", self.roles_var)
            row += 1
            self._entry(row, "Extra provider args", self.extra_args_var)
            row += 1
        else:
            self._entry(row, "Service name (policy reference)", self.service_var)
            row += 1
            self._entry(row, "ACK timeout ms", self.ack_timeout_var)
            row += 1
            self._entry(row, "Total timeout ms", self.timeout_var)
            row += 1
            self._entry(row, "Extra user args", self.extra_args_var)
            row += 1

        buttons = ttk.Frame(self)
        buttons.grid(row=row, column=0, columnspan=3, sticky="ew", padx=6, pady=8)
        ttk.Button(buttons, text=f"Start {self.role.title()}",
                   command=self.run_role).pack(side="left")
        ttk.Button(buttons, text="Stop",
                   command=self.stop_role).pack(side="left", padx=6)
        ttk.Button(buttons, text="Restart",
                   command=self.restart_role).pack(side="left")
        ttk.Button(buttons, text="Show Command",
                   command=self.show_command).pack(side="left", padx=6)
        ttk.Button(buttons, text="Open Deployment Logs",
                   command=lambda: self.app.select_tab("Deployment Runner")).pack(side="left")
        row += 1
        ttk.Label(self, text="Status").grid(row=row, column=0, sticky="w", padx=6)
        ttk.Label(self, textvariable=self.status_var, anchor="w").grid(
            row=row, column=1, columnspan=2, sticky="ew", padx=6, pady=4)
        row += 1

        self.output = TextPane(self, height=18)
        self.output.grid(row=row, column=0, columnspan=3, sticky="nsew", padx=6, pady=6)
        self.rowconfigure(row, weight=1)
        row += 1

        self.identity_tools.grid(row=row, column=0, columnspan=3, sticky="nsew", padx=6, pady=6)

    def _entry(self, row: int, label: str, variable: tk.StringVar,
               *, browse: bool = False) -> None:
        ttk.Label(self, text=label).grid(row=row, column=0, sticky="w", padx=6, pady=4)
        ttk.Entry(self, textvariable=variable).grid(row=row, column=1, sticky="ew", padx=6, pady=4)
        if browse:
            ttk.Button(self, text="Browse", command=self._browse_config).grid(
                row=row, column=2, padx=6, pady=4)

    def _combo(self, row: int, label: str, variable: tk.StringVar,
               values: list[str]) -> None:
        ttk.Label(self, text=label).grid(row=row, column=0, sticky="w", padx=6, pady=4)
        ttk.Combobox(self, textvariable=variable, values=values, state="readonly").grid(
            row=row, column=1, sticky="ew", padx=6, pady=4)

    def _browse_config(self) -> None:
        path = filedialog.askopenfilename(
            title="Select policy config",
            filetypes=[("YAML/JSON", "*.yaml *.yml *.json"), ("All files", "*")],
        )
        if path:
            self.config_var.set(path)

    def _script_path(self) -> Path:
        base = self.EXAMPLE_BASES[self.example_var.get()]
        return repo_root() / base / f"{self.role}.py"

    def command(self) -> list[str]:
        return build_role_command(
            role=self.role,
            script_path=self._script_path(),
            config=self.config_var.get(),
            generated_policy_dir=self.generated_dir_var.get(),
            group=self.group_var.get(),
            provider_id=self.provider_id_var.get(),
            roles=self.roles_var.get(),
            ack_timeout_ms=self.ack_timeout_var.get(),
            timeout_ms=self.timeout_var.get(),
            extra_args=self.extra_args_var.get(),
        )

    def show_command(self) -> None:
        command = shlex.join(self.command())
        self.output.set(command)
        self.app.set_status(f"{self.role.title()} command prepared")

    def run_role(self) -> None:
        self.output.set("Starting through Deployment Runner log pane:\n" +
                        shlex.join(self.command()))
        self.app.runner._start(self.command(), self.role)
        self.app.select_tab("Deployment Runner")

    def stop_role(self) -> None:
        self.app.runner.stop_role(self.role)

    def restart_role(self) -> None:
        self.stop_role()
        self.after(300, self.run_role)

    def profile(self) -> RuntimeRoleProfile:
        return RuntimeRoleProfile(
            role=self.role,
            config=self.config_var.get(),
            example=self.example_var.get(),
            generated_policy_dir=self.generated_dir_var.get(),
            group=self.group_var.get(),
            provider_id=self.provider_id_var.get(),
            roles=self.roles_var.get(),
            service=self.service_var.get(),
            ack_timeout_ms=self.ack_timeout_var.get(),
            timeout_ms=self.timeout_var.get(),
            extra_args=self.extra_args_var.get(),
        )

    def apply_profile(self, profile: RuntimeRoleProfile) -> None:
        self.config_var.set(profile.config)
        if profile.example in self.EXAMPLE_BASES:
            self.example_var.set(profile.example)
        self.generated_dir_var.set(profile.generated_policy_dir)
        self.group_var.set(profile.group)
        self.provider_id_var.set(profile.provider_id)
        self.roles_var.set(profile.roles)
        self.service_var.set(profile.service)
        self.ack_timeout_var.set(profile.ack_timeout_ms)
        self.timeout_var.set(profile.timeout_ms)
        self.extra_args_var.set(profile.extra_args)
        self.status_var.set("stopped")


class DistributedInferenceGui(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("NDNSF Distributed Inference")
        self.geometry("1280x820")
        self.status = tk.StringVar(value="Ready")
        self.notebook = ttk.Notebook(self)
        self.notebook.pack(fill="both", expand=True)
        self.wizard = WizardTab(self.notebook, self)
        self.policy_editor = PolicyEditorTab(self.notebook, self)
        self.model_split = ModelSplitTab(self.notebook, self)
        self.certificates = CertificateTab(self.notebook, self)
        self.runner = DeploymentRunnerTab(self.notebook, self)
        self.controller_runtime = RoleRuntimeTab(self.notebook, self, "controller")
        self.user_runtime = RoleRuntimeTab(self.notebook, self, "user")
        self.provider_runtime = RoleRuntimeTab(self.notebook, self, "provider")
        self.notebook.add(self.wizard, text="Project Wizard")
        self.notebook.add(self.policy_editor, text="Policy Editor")
        self.notebook.add(self.model_split, text="Model Split")
        self.notebook.add(self.certificates, text="Certificates")
        self.notebook.add(self.controller_runtime, text="Controller")
        self.notebook.add(self.user_runtime, text="User")
        self.notebook.add(self.provider_runtime, text="Provider")
        self.notebook.add(self.runner, text="Deployment Runner")
        ttk.Label(self, textvariable=self.status, anchor="w").pack(fill="x")
        self.protocol("WM_DELETE_WINDOW", self._on_close)

    def select_tab(self, name: str) -> None:
        for index in range(self.notebook.index("end")):
            if self.notebook.tab(index, "text") == name:
                self.notebook.select(index)
                return

    def set_status(self, value: str) -> None:
        status = getattr(self, "status", None)
        if status is not None:
            status.set(value)

    def _on_close(self) -> None:
        self.runner.stop_processes()
        self.destroy()


def main() -> int:
    app = DistributedInferenceGui()
    app.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
