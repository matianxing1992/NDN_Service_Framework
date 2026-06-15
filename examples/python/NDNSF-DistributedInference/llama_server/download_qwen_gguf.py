#!/usr/bin/env python3
"""Download the Qwen2.5-0.5B GGUF model used by the llama-server example."""

from __future__ import annotations

import argparse
from pathlib import Path
from urllib import request


DEFAULT_REPO = "Qwen/Qwen2.5-0.5B-Instruct-GGUF"
DEFAULT_FILENAME = "qwen2.5-0.5b-instruct-q4_k_m.gguf"
DEFAULT_BASE_URL = "https://huggingface.co"


def huggingface_resolve_url(repo: str, filename: str, revision: str = "main") -> str:
    return (
        f"{DEFAULT_BASE_URL}/{repo}/resolve/{revision}/{filename}"
        "?download=true"
    )


def download_file(url: str, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    tmp = destination.with_suffix(destination.suffix + ".part")
    with request.urlopen(url, timeout=60) as response, tmp.open("wb") as output:
        while True:
            chunk = response.read(1024 * 1024)
            if not chunk:
                break
            output.write(chunk)
    tmp.replace(destination)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", default=DEFAULT_REPO)
    parser.add_argument("--filename", default=DEFAULT_FILENAME)
    parser.add_argument("--revision", default="main")
    parser.add_argument("--dest", default="third_party/qwen")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    url = huggingface_resolve_url(args.repo, args.filename, args.revision)
    destination = Path(args.dest) / args.filename
    print(
        "QWEN_GGUF_DOWNLOAD_PLAN",
        f"repo={args.repo}",
        f"filename={args.filename}",
        f"url={url}",
        f"destination={destination}",
        flush=True,
    )
    if not args.dry_run:
        download_file(url, destination)
    print(f"QWEN_GGUF_DOWNLOAD_OK model={destination}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
