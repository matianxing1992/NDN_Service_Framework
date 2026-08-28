#!/usr/bin/env python3
"""Build or validate the immutable Spec175 Qwen3.6 workload manifest.

This is an offline packaging tool.  It intentionally uses the standalone
``tokenizers`` runtime plus the pinned Jinja chat template; it does not import
PyTorch or Transformers and therefore cannot silently select a model runtime.
"""

import argparse
import hashlib
import json
from pathlib import Path
from typing import Any, Dict, Iterable, Mapping

from jinja2.sandbox import SandboxedEnvironment
from tokenizers import Tokenizer


SCHEMA = "ndnsf-di-spec175-workload-v1"
MODEL_REPOSITORY = "Qwen/Qwen3.6-27B"
MODEL_REVISION = "6a9e13bd6fc8f0983b9b99948120bc37f49c13e9"
PROMPTS = (
    (
        "P1",
        "Write exactly 64 numbered words about content-addressed model "
        "artifacts. Do not stop before word 64.",
    ),
    (
        "P2",
        "Generate a 64-item comma-separated sequence alternating the words "
        "network and model. Do not add an introduction or stop early.",
    ),
)
TOKENIZER_FILES = (
    "chat_template.jinja",
    "tokenizer.json",
    "tokenizer_config.json",
)


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _canonical_json(value: Any) -> bytes:
    return json.dumps(
        value, sort_keys=True, separators=(",", ":"), ensure_ascii=False,
    ).encode("utf-8")


def _raise_exception(message: str) -> None:
    raise ValueError(message)


def _render_prompt(template_text: str, prompt: str) -> str:
    environment = SandboxedEnvironment(trim_blocks=True, lstrip_blocks=True)
    environment.globals["raise_exception"] = _raise_exception
    # The registered messages do not use this function.  A deterministic value
    # prevents a future template edit from making this workload time-dependent.
    environment.globals["strftime_now"] = lambda _format: "1970-01-01"
    template = environment.from_string(template_text)
    return template.render(
        messages=[{"role": "user", "content": prompt}],
        tools=None,
        add_generation_prompt=True,
        enable_thinking=False,
        preserve_thinking=False,
        add_vision_id=False,
    )


def _required_file_bytes(tokenizer_dir: Path) -> Dict[str, bytes]:
    values = {}
    for name in TOKENIZER_FILES:
        path = tokenizer_dir / name
        if not path.is_file():
            raise ValueError("missing pinned tokenizer file: {}".format(path))
        values[name] = path.read_bytes()
    return values


def _unique(values: Iterable[int]):
    output = []
    for value in values:
        if value is not None and value not in output:
            output.append(int(value))
    return output


def build_workload(tokenizer_dir: Path) -> Dict[str, Any]:
    tokenizer_dir = Path(tokenizer_dir)
    file_bytes = _required_file_bytes(tokenizer_dir)
    template_text = file_bytes["chat_template.jinja"].decode("utf-8")
    tokenizer_config = json.loads(
        file_bytes["tokenizer_config.json"].decode("utf-8")
    )
    if tokenizer_config.get("chat_template") != template_text:
        raise ValueError(
            "tokenizer_config chat_template differs from chat_template.jinja"
        )
    tokenizer = Tokenizer.from_file(str(tokenizer_dir / "tokenizer.json"))

    eos_token = tokenizer_config.get("eos_token")
    pad_token = tokenizer_config.get("pad_token")
    if not isinstance(eos_token, str) or not isinstance(pad_token, str):
        raise ValueError("tokenizer config must define string EOS and pad tokens")
    eos_ids = _unique((
        tokenizer.token_to_id(eos_token),
        tokenizer.token_to_id(pad_token),
    ))
    if not eos_ids:
        raise ValueError("tokenizer does not contain the configured EOS tokens")

    file_records = {
        name: {
            "sha256": _sha256(file_bytes[name]),
            "sizeBytes": len(file_bytes[name]),
        }
        for name in TOKENIZER_FILES
    }
    tokenizer_artifact_digest = _sha256(_canonical_json({
        name: record["sha256"] for name, record in file_records.items()
    }))

    prompt_records = []
    for prompt_id, prompt in PROMPTS:
        prompt_bytes = prompt.encode("utf-8")
        rendered = _render_prompt(template_text, prompt)
        input_token_ids = tokenizer.encode(
            rendered, add_special_tokens=False,
        ).ids
        if not input_token_ids:
            raise ValueError("{} produced an empty token sequence".format(prompt_id))
        prompt_records.append({
            "canonicalMessageUtf8Hex": prompt_bytes.hex(),
            "id": prompt_id,
            "inputTokenCount": len(input_token_ids),
            "inputTokenIds": input_token_ids,
            "inputTokensSha256": _sha256(_canonical_json(input_token_ids)),
            "message": {"content": prompt, "role": "user"},
            "promptSha256": _sha256(prompt_bytes),
            "renderedPromptSha256": _sha256(rendered.encode("utf-8")),
        })

    return {
        "generation": {
            "doSample": False,
            "eosTokenIds": eos_ids,
            "maxEvents": 65,
            "maxGeneratedTokens": 64,
            "requestDeadlineMs": 120000,
            "samplingMode": "greedy",
            "thinkingMode": "disabled",
        },
        "model": {
            "modality": "text-only",
            "mtpEnabled": False,
            "repository": MODEL_REPOSITORY,
            "revision": MODEL_REVISION,
        },
        "prompts": prompt_records,
        "schema": SCHEMA,
        "tokenizer": {
            "artifactSha256": tokenizer_artifact_digest,
            "chatTemplateArguments": {
                "addGenerationPrompt": True,
                "enableThinking": False,
                "preserveThinking": False,
            },
            "files": file_records,
            "implementation": "standalone-tokenizers+jinja2",
        },
    }


def validate_workload(value: Mapping[str, Any], tokenizer_dir: Path) -> None:
    expected = build_workload(tokenizer_dir)
    if value != expected:
        raise ValueError(
            "workload does not match the pinned prompts, tokenizer, template, "
            "or generation contract"
        )


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tokenizer-dir", required=True, type=Path)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--output", type=Path)
    mode.add_argument("--check", type=Path)
    return parser.parse_args()


def main() -> int:
    args = _parse_args()
    if args.output is not None:
        value = build_workload(args.tokenizer_dir)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(
            json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False) + "\n",
            encoding="utf-8",
        )
        print(args.output)
        return 0
    value = json.loads(args.check.read_text(encoding="utf-8"))
    validate_workload(value, args.tokenizer_dir)
    print("PASS {}".format(args.check))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
