"""Operations-owned Runtime-v1 production and contract-smoke dispatch."""

from __future__ import annotations

import argparse
from pathlib import Path

from .. import runtime_v1 as runtime

def runtime_v1_main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="NDNSF-DI Runtime v1 utilities")
    sub = parser.add_subparsers(dest="command", required=True)

    provider = sub.add_parser("provider", help="run the native provider adapter")
    provider.add_argument("--profile", required=True)
    provider.add_argument("--dry-run", action="store_true")
    provider.set_defaults(func=runtime._cmd_production_provider)

    plan = sub.add_parser("plan", help="build a local Runtime v1 plan lease")
    plan.add_argument("--model", required=True)
    plan.add_argument("--providers", required=True)
    plan.add_argument("--out", required=True)
    plan.add_argument("--explain", required=True)
    plan.add_argument("--target-rps", type=float, default=0.0)
    plan.add_argument("--context-class", default="short")
    plan.add_argument("--prefix-id", default="")
    plan.add_argument("--session-id", default="")
    plan.set_defaults(func=runtime._cmd_plan)

    run = sub.add_parser("run", help="run a real deployment request adapter")
    run.add_argument("--plan", required=True)
    run.add_argument("--profile", default="")
    run.add_argument("--request", default="")
    run.add_argument("--out", default="")
    run.add_argument("--dry-run", action="store_true")
    run.set_defaults(func=runtime._cmd_production_run)

    bench = sub.add_parser("bench", help="run a real MiniNDN campaign adapter")
    bench.add_argument("--campaign", default="")
    bench.add_argument("--out", type=Path)
    bench.add_argument("--dry-run", action="store_true")
    bench.set_defaults(func=runtime._cmd_production_bench)

    status = sub.add_parser("status", help="read the deployment status snapshot")
    status.add_argument("--profile", required=True)
    status.add_argument("--json", action="store_true")
    status.set_defaults(func=runtime._cmd_status)

    metrics = sub.add_parser("metrics", help="export a deployment metrics snapshot")
    metrics.add_argument("--profile", required=True)
    metrics.add_argument("--format", choices=("json", "prometheus-textfile"), default="json")
    metrics.add_argument("--out", required=True)
    metrics.set_defaults(func=runtime._cmd_metrics)

    doctor = sub.add_parser("doctor", help="run production deployment preflight")
    doctor.add_argument("--profile", required=True)
    doctor.add_argument("--json", action="store_true")
    doctor.add_argument("--dry-run", action="store_true")
    doctor.set_defaults(func=runtime._cmd_production_doctor)

    smoke = sub.add_parser("contract-smoke", help="explicit simulated Runtime v1 utilities")
    smoke_sub = smoke.add_subparsers(dest="smoke_command", required=True)

    smoke_run = smoke_sub.add_parser("run")
    smoke_run.add_argument("--plan", required=True)
    smoke_run.add_argument("--requests", type=int, default=1)
    smoke_run.add_argument("--prompt-tokens", type=int, default=1024)
    smoke_run.add_argument("--generated-tokens", type=int, default=32)
    smoke_run.add_argument("--microbatch", type=int, default=1)
    smoke_run.add_argument("--provider-flops-tflops", type=float, default=8.0)
    smoke_run.add_argument("--out", default="")
    smoke_run.set_defaults(func=runtime._cmd_run)

    smoke_bench = smoke_sub.add_parser("bench")
    smoke_bench.add_argument("--out-dir", type=Path, required=True)
    smoke_bench.add_argument("--runs", type=int, default=1)
    smoke_bench.set_defaults(func=runtime._cmd_bench)

    sweep = smoke_sub.add_parser("context-sweep")
    sweep.add_argument("--model", required=True)
    sweep.add_argument("--providers", required=True)
    sweep.add_argument("--out-dir", type=Path, required=True)
    sweep.add_argument("--context-tokens", default="1024,8192")
    sweep.add_argument("--rps", default="1,4,8")
    sweep.add_argument("--generated-tokens", type=int, default=32)
    sweep.add_argument("--microbatch", type=int, default=1)
    sweep.add_argument("--cache-aware", action="store_true")
    sweep.set_defaults(func=runtime._cmd_context_sweep)

    smoke_sample = smoke_sub.add_parser("schema-sample")
    smoke_sample.add_argument("--out", default="")
    smoke_sample.set_defaults(func=runtime._cmd_schema_sample)

    sample = sub.add_parser("schema-sample", help="write a Runtime v1 smoke payload")
    sample.add_argument("--out", default="")
    sample.set_defaults(func=runtime._cmd_schema_sample)

    inspect = sub.add_parser("inspect", help="pretty-print a Runtime v1 JSON file")
    inspect.add_argument("path")
    inspect.set_defaults(func=runtime._cmd_inspect)

    args = parser.parse_args(argv)
    return int(args.func(args))


if __name__ == "__main__":
    raise SystemExit(main())


__all__ = ["runtime_v1_main"]
