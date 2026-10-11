#!/usr/bin/env python3
"""Record the configured Waf graph without executing or storing any task.

Used by installed-candidate preparation. Every selected native target records
its actual sources, transitive headers, object signatures and linked output.
"""
import argparse
import ast
import hashlib
import json
import os
from pathlib import Path
import sys
from types import SimpleNamespace


def identity(path):
    path = Path(path).resolve(strict=True)
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return {"path": str(path), "bytes": path.stat().st_size, "sha256": digest.hexdigest()}


def inspect_graph(root, lock, targets):
    os.chdir(root)
    waf = sorted(root.glob('.waf3-*/waflib/__init__.py'))
    if len(waf) != 1:
        raise RuntimeError("SPEC201_WAF_LIBRARY_AMBIGUOUS")
    sys.path[:0] = [str(waf[0].parent.parent), str(root / '.waf-tools')]
    from waflib import Context, Build, Options
    from waflib.Tools import cxx, c, ccroot  # noqa: F401
    values = {}
    for node in ast.parse(lock.read_text()).body:
        if (isinstance(node, ast.Assign) and isinstance(node.targets[0], ast.Name)
                and node.targets[0].id in ('top_dir', 'out_dir', 'run_dir', 'options')):
            values[node.targets[0].id] = ast.literal_eval(node.value)
    options = dict(values['options'])
    options.update(jobs=2, targets=','.join(targets), keep=0, progress_bar=0)
    Options.options = SimpleNamespace(**options)
    Context.top_dir = values['top_dir']
    Context.out_dir = values['out_dir']
    Context.run_dir = values['run_dir']
    Context.launch_dir = str(root)
    Context.g_module = Context.load_module(str(root / 'wscript'))
    build = Build.BuildContext(top_dir=values['top_dir'], out_dir=values['out_dir'],
                               run_dir=values['run_dir'])
    def forbidden(*args, **kwargs):
        raise RuntimeError('SPEC201_READ_ONLY_GRAPH_FORBIDS_EXECUTION')
    build.exec_command = forbidden
    build.store = forbidden
    build.restore()
    build.load_envs()
    build.recurse([str(root)])
    for target in targets:
        build.get_tgen_by_name(target).post()
    build.cur_tasks = []
    rows, outputs, inputs = [], [], {}
    for group in build.groups:
        for generator in group:
            for task in getattr(generator, 'tasks', []):
                if not task.inputs:
                    continue
                if task.inputs[0].suffix() in ('.cpp', '.cc', '.cxx'):
                    uid = task.uid()
                    stored = build.task_sigs.get(uid, b'').hex()
                    current = task.signature().hex()
                    if not stored or stored != current:
                        raise RuntimeError('SPEC201_BUILD_SOURCE_STALE:' + generator.name + ':' +
                                           task.inputs[0].abspath())
                    source = identity(task.inputs[0].abspath())
                    inputs[source['path']] = source
                    dependencies = []
                    for dependency in build.node_deps.get(uid, []):
                        if Path(dependency.abspath()).is_file():
                            item = identity(dependency.abspath())
                            inputs[item['path']] = item
                            dependencies.append(item)
                    rows.append({'target': generator.name, 'source': source,
                                 'dependencies': dependencies, 'storedSignature': stored,
                                 'currentSignature': current,
                                 'outputs': [identity(node.abspath()) for node in task.outputs]})
                elif task is getattr(generator, 'link_task', None):
                    stored = build.task_sigs.get(task.uid(), b'').hex()
                    if not stored or stored != task.signature().hex():
                        raise RuntimeError('SPEC201_BUILD_LINK_STALE:' + generator.name)
                    outputs.extend({'target': generator.name, **identity(node.abspath())}
                                   for node in task.outputs)
    if not rows or not outputs or not set(targets).issubset({row['target'] for row in outputs}):
        raise RuntimeError('SPEC201_BUILD_TARGET_CLOSURE_INCOMPLETE')
    return {'schema': 'ndnsf-spec201-build-closure-1', 'rows': rows,
            'artifacts': outputs, 'inputs': sorted(inputs.values(), key=lambda row: row['path'])}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--lock', type=Path, default=Path('.lock-waf_linux_build'))
    parser.add_argument('--targets', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    lock = args.lock if args.lock.is_absolute() else root / args.lock
    result = inspect_graph(root, lock, args.targets.split(','))
    with args.output.resolve().open('x') as stream:
        json.dump(result, stream, indent=2, sort_keys=True)
        stream.write('\n')
    print(json.dumps({'rows': len(result['rows']), 'artifacts': len(result['artifacts']),
                      'inputs': len(result['inputs']), 'status': 'PASS'}))


if __name__ == '__main__':
    main()
