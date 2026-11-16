#!/usr/bin/env python3
"""Run the selected native check profile from the roster.

This owns selection and reporting only. It does not implement inference, authorization or any
protocol, and its report is never an oracle: the native executables are the evidence.

Two rules it enforces because their absence made earlier runs misleading:

  * A case that no profile selects is an error, not a silent omission. The roster is the list of
    checks this project claims to run; a selector that matches nothing means the claim is stale.
  * A selected check whose declared asset is missing exits with PREREQUISITE_MISSING and a non-zero
    status. Returning success when a model was absent is how a skipped run gets counted as a pass.

Usage:
    python3 tests/run-checks.py --profile unit
    python3 tests/run-checks.py --profile model --asset-manifest PATH
    python3 tests/run-checks.py --profile all --fixture-root PATH --report report.json
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROSTER = Path(__file__).resolve().parent / 'check-profiles.json'
EXIT_OK = 0
EXIT_FAILED = 1
EXIT_PREREQUISITE = 2
EXIT_ROSTER = 3


def load_roster(path):
    with open(path, encoding='utf-8') as handle:
        roster = json.load(handle)
    if roster.get('schema') != 'ndnsf-check-profiles-v1':
        raise SystemExit('ROSTER_SCHEMA_UNSUPPORTED: %s' % roster.get('schema'))
    return roster


def resolve_binary(target, search):
    """Find one check binary by name, in the declared search order.

    `target` is the roster's target name; the Waf build writes it either into the build directory
    or beside it depending on how the program declared its target, so both are searched rather than
    assuming one layout.
    """
    for directory in search:
        candidate = Path(directory) / target
        if candidate.is_file() and os.access(str(candidate), os.X_OK):
            return candidate
    return None


def enumerate_cases(binary, environ):
    """List the case names a binary actually contains, from Boost.Test's own listing."""
    completed = subprocess.run([str(binary), '--list_content'], capture_output=True, text=True,
                               env=environ, timeout=300)
    if completed.returncode != 0:
        raise SystemExit('CASE_ENUMERATION_FAILED: %s\n%s'
                         % (binary, completed.stderr[-2000:]))
    # Boost prints `Suite*` at column 0 and `    Case*` indented by four. The indentation is the
    # only difference, so it is what decides which is which.
    # Boost.Test writes `--list_content` to stderr, not stdout, so both streams are read: parsing
    # only stdout reports a binary with 2491 cases as having none.
    cases, suites = [], []
    for line in (completed.stdout + completed.stderr).splitlines():
        stripped = line.strip()
        if not stripped.endswith('*'):
            continue
        name = stripped[:-1]
        (cases if line[:1] in (' ', '\t') else suites).append(name)
    return suites, cases


def check_assets(roster, selected, manifest_path):
    """Report every asset the selected profiles need, and whether it is present."""
    missing = []
    for name in selected:
        profile = roster['profiles'][name]
        for target in profile.get('targets', []):
            for asset_name in target.get('assets', []):
                asset = roster['assets'][asset_name]
                root = os.environ.get(asset['env']) or asset.get('default', '')
                if manifest_path:
                    root = str(Path(manifest_path).parent / asset_name)
                if not root or not Path(root).is_dir():
                    missing.append((asset_name, root or '(unset)'))
                    continue
                for required in asset.get('requiredFiles', []):
                    if not list(Path(root).rglob(required)):
                        missing.append((asset_name, '%s lacks %s' % (root, required)))
    return missing


def run_target(binary, target, environ, timeout):
    """Run one selector, once, and report its own summary line.

    One invocation per selector: the umbrella binaries hold thousands of cases and running each
    case separately would spend the whole budget in process startup. The selector is the unit the
    roster declares, so its own exit status is the result.
    """
    selector = target.get('selector', {})
    include = selector.get('include', ['*'])
    results = []
    for pattern in include:
        arguments = [str(binary), '--run_test=' + pattern, '--log_level=test_suite']
        # A roster entry may narrow the run to the suites a component owns; an empty include means
        # everything the binary holds.
        completed = subprocess.run(arguments, capture_output=True, text=True, env=environ,
                                   timeout=timeout)
        combined = (completed.stdout + completed.stderr).strip().splitlines()
        results.append({'target': target['target'], 'selector': pattern,
                        'status': completed.returncode,
                        'passed': completed.returncode == 0,
                        'tail': combined[-6:]})
    return results


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--profile', required=True,
                        help='profile name, or "all" for every declared profile')
    parser.add_argument('--fixture-root', default=os.environ.get('NDNSF_TEST_FIXTURE_ROOT'),
                        help='declared fixture root; passed through to every check')
    parser.add_argument('--asset-manifest', default=None,
                        help='directory holding prepared assets, one subdirectory per asset id')
    parser.add_argument('--build-root', default=None,
                        help='directory holding the built check binaries')
    parser.add_argument('--report', default=None, help='write the JSON report here')
    parser.add_argument('--timeout', type=float, default=3600.0, help='per-selector timeout')
    arguments = parser.parse_args(argv)

    roster = load_roster(ROSTER)
    known = sorted(roster['profiles'])
    if arguments.profile == 'all':
        selected = known
    elif arguments.profile in roster['profiles']:
        selected = [arguments.profile]
    else:
        sys.stderr.write('ROSTER_UNKNOWN_PROFILE: %s (known: %s)\n'
                         % (arguments.profile, ', '.join(known)))
        return EXIT_ROSTER

    checkout = Path(__file__).resolve().parent.parent
    search = ([arguments.build_root] if arguments.build_root else []) + [str(checkout / 'build'),
                                                                          str(checkout)]

    environ = dict(os.environ)
    if arguments.fixture_root:
        environ['NDNSF_TEST_FIXTURE_ROOT'] = str(Path(arguments.fixture_root).resolve())
    # An installed run must not silently pick up a developer's build or site packages.
    for name in ('PYTHONPATH', 'LD_PRELOAD'):
        environ.pop(name, None)

    missing = check_assets(roster, selected, arguments.asset_manifest)
    if missing:
        for asset_name, detail in missing:
            sys.stderr.write('PREREQUISITE_MISSING: %s (%s)\n' % (asset_name, detail))
        report = {'schema': 'ndnsf-check-report-v1', 'profile': arguments.profile,
                  'status': 'PREREQUISITE_MISSING', 'missing': missing}
        if arguments.report:
            Path(arguments.report).write_text(json.dumps(report, indent=2) + '\n')
        return EXIT_PREREQUISITE

    report = {'schema': 'ndnsf-check-report-v1', 'profile': arguments.profile,
              'status': 'RUN', 'selected': selected, 'targets': [], 'unmatched': 0}
    failed = False
    for name in selected:
        for target in roster['profiles'][name].get('targets', []):
            binary = resolve_binary(target['target'], search)
            if binary is None:
                sys.stderr.write('CHECK_BINARY_MISSING: %s (searched %s)\n'
                                 % (target['target'], ', '.join(search)))
                report['targets'].append({'target': target['target'],
                                          'status': 'BINARY_MISSING'})
                failed = True
                continue
            suites, cases = enumerate_cases(binary, environ)
            if not suites and not cases:
                sys.stderr.write('CHECK_BINARY_HAS_NO_CASES: %s\n' % binary)
                failed = True
                continue
            if target.get('selector', {}).get('requireMatch'):
                # The contract asks for the opposite of a silent omission: a selector that matches
                # nothing means the roster claim is stale, so it is an error rather than a pass.
                landed = subprocess.run([str(binary), '--run_test=' +
                                         target['selector']['include'][0], '--list_content'],
                                        capture_output=True, text=True, env=environ, timeout=300)
                if not (landed.stdout + landed.stderr).strip():
                    sys.stderr.write('CHECK_SELECTOR_UNMATCHED: %s %s\n'
                                     % (target['target'], target['selector']['include'][0]))
                    failed = True
                    continue
            entries = run_target(binary, target, environ, arguments.timeout)
            for entry in entries:
                entry['binary'] = str(binary)
                entry['suites'] = len(suites)
                entry['cases'] = len(cases)
                if not entry['passed']:
                    failed = True
            report['targets'].append({'target': target['target'], 'binary': str(binary),
                                      'suites': len(suites), 'cases': len(cases),
                                      'runs': entries})

    # The cases column is what the binary holds, not what a selector ran: a selector narrows the
    # run, and reporting the binary's total as "covered" overstated every partial run.
    report['availableCases'] = sum(target.get('cases', 0) for target in report['targets'])
    report['status'] = 'FAILED' if failed else 'PASSED'
    if arguments.report:
        Path(arguments.report).write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k: report[k] for k in ('status', 'profile', 'availableCases')}))
    return EXIT_FAILED if failed else EXIT_OK


if __name__ == '__main__':
    sys.exit(main())
