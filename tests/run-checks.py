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
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROSTER = Path(__file__).resolve().parent / 'check-profiles.json'


def installed_prefix():
  """Return the prefix this runner was installed under, or None when running from a checkout.

  An installed runner lives at PREFIX/share/ndnsf/tests/run-checks.py and must resolve its roster,
  fixtures and binaries from that prefix -- not from the checkout it may still be sitting beside,
  and not from the developer's build tree.
  """
  here = Path(__file__).resolve().parent
  if here.name != 'tests' or here.parent.name != 'ndnsf' or here.parent.parent.name != 'share':
      return None
  return here.parent.parent.parent
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
    """List the suites and cases a binary contains, from Boost.Test's own tree.

    Boost prints one `name*` per node, indented by depth, and a deeper node belongs to the one above
    it. Telling cases from suites by "indented" is wrong: nested suites are indented too, so a binary
    with 168 cases inside 5 sub-suites counted as 173 cases. A node is a case exactly when the next
    node is not deeper than it is.

    Both streams are read because Boost.Test writes this listing to stderr; parsing only stdout
    reports a binary with thousands of cases as having none.
    """
    completed = subprocess.run([str(binary), '--list_content'], capture_output=True, text=True,
                               env=environ, timeout=300)
    if completed.returncode != 0:
        raise SystemExit('CASE_ENUMERATION_FAILED: %s\n%s'
                         % (binary, completed.stderr[-2000:]))
    nodes = []
    for line in (completed.stdout + completed.stderr).splitlines():
        stripped = line.strip()
        if not stripped.endswith('*'):
            continue
        nodes.append((len(line) - len(line.lstrip()), stripped[:-1]))
    suites, cases = [], []
    for index, (depth, name) in enumerate(nodes):
        next_depth = nodes[index + 1][0] if index + 1 < len(nodes) else -1
        (cases if next_depth <= depth else suites).append(name)
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


_ANSI = re.compile(r'\x1b\[[0-9;]*[A-Za-z]')


def _boostCounts(text):
    """Read selected, executed and failed out of one run's output.

    Three different numbers, and only the binary knows them:

      selected  `Running N test cases...` -- what the selector announced
      executed  completed `Leaving test case` lines -- what actually finished
      failed    the count in `*** N failures ... ***`

    A selector can announce 168 and complete 102 before the budget expires; reporting the
    announcement as executed is exactly the overstatement this replaces. A case that entered and
    never left is neither executed nor failed, which is the honest reading of a timeout.
    """
    # Colour is presentation, not data: the escape sequences carry digits, so they are removed
    # before any number is read out of the text.
    text = _ANSI.sub('', text)
    selected, executed, failed = 0, 0, 0
    for line in text.splitlines():
        if 'Running ' in line and 'test case' in line:
            selected = int(''.join(ch for ch in line.split('Running ', 1)[1].split()[0]
                                   if ch.isdigit()) or 0)
        elif 'Leaving test case' in line:
            executed += 1
        elif 'Entering test case' in line and executed == 0:
            # Some Boost versions report completion only through the summary; counting entries is a
            # lower bound, never an overstatement.
            pass
        if 'failure' in line and 'detected in the test module' in line:
            # The line carries ANSI colour codes, whose escape sequences contain digits
            # ("[1;31;49m"), so stripping every digit from the prefix counted the colour as the
            # failure count -- 1314910 for a run with 10 failures. Only the leading decimal token
            # is the count.
            head = line.split('failure', 1)[0]
            digits = ''
            for ch in head:
                if ch.isdigit():
                    digits += ch
                elif digits:
                    break
            failed = int(digits or 1)
    return selected, executed, failed


def _binaryCaseCount(binary, environ):
    """Cases the binary holds, from its own listing."""
    _, cases = enumerate_cases(binary, environ)
    return len(cases)


def _count_matching(binary, pattern, environ):
    """Count the cases a roster selector reaches, from the binary's own tree.

    Boost has no way to report a count without running, and running every selector twice would
    double the budget. So the tree from `--list_content` is matched here -- but only for the two
    selector forms the roster is allowed to use: `*` and `<suite>/*`. Anything else raises, rather
    than guessing at Boost expression semantics and silently disagreeing with what runs.
    """
    if pattern == '*':
        return _binaryCaseCount(binary, environ)
    if not pattern.endswith('/*') or pattern[:-2].count('/') != 0:
        raise SystemExit('ROSTER_SELECTOR_UNSUPPORTED: %s (allowed: * or <suite>/*)' % pattern)
    suite = pattern[:-2]
    completed = subprocess.run([str(binary), '--list_content'], capture_output=True, text=True,
                               env=environ, timeout=300)
    nodes = []
    for line in (completed.stdout + completed.stderr).splitlines():
        stripped = line.strip()
        if not stripped.endswith('*'):
            continue
        nodes.append((len(line) - len(line.lstrip()), stripped[:-1]))
    count, inside, depth = 0, False, None
    for index, (level, name) in enumerate(nodes):
        next_level = nodes[index + 1][0] if index + 1 < len(nodes) else -1
        is_case = next_level <= level
        if not inside:
            if name == suite:
                inside, depth = True, level
            continue
        if level <= depth:
            break
        if is_case:
            count += 1
    if not inside:
        return 0
    return count


def run_target(binary, target, environ, timeout, arguments):
    """Run one selector, once, and report its own summary line.

    One invocation per selector: the umbrella binaries hold thousands of cases and running each
    case separately would spend the whole budget in process startup. The selector is the unit the
    roster declares, so its own exit status is the result.
    """
    selector = target.get('selector', {})
    include = selector.get('include', ['*'])
    # An exclusion is a selection decision, not a skip: the roster says which profile owns the case,
    # and a case excluded by one profile must be claimed by another. `validate_ownership` checks that.
    for pattern in selector.get('excludeSuites', []):
        include = include + ['!' + pattern] if pattern not in include else include
    results = []
    for pattern in include:
        # `arguments` is the caller's parsed options; the subprocess argv needs its own name or it
        # shadows them, which is how `arguments.report` first became a list.
        if pattern.startswith('!'):
            # Boost has no exclusion syntax. An excluded suite is run positively by the profile that
            # owns it, so this entry contributes nothing here rather than pretending to filter.
            continue
        argv = [str(binary), '--run_test=' + pattern, '--log_level=test_suite']
        # A roster entry may narrow the run to the suites a component owns; an empty include means
        # everything the binary holds.
        # The selector's reach is counted first, so a run that times out still reports what it was
        # asked to cover -- otherwise the number depends on whether the budget was enough, which
        # conflates selection with outcome.
        selected = (_binaryCaseCount(str(binary), environ) if pattern == '*'
                    else _count_matching(str(binary), pattern, environ))
        try:
            completed = subprocess.run(argv, capture_output=True, text=True, env=environ,
                                       timeout=timeout)
            status = completed.returncode
            combined = (completed.stdout + completed.stderr)
        except subprocess.TimeoutExpired as expired:
            # A timeout is a result, not an absence of one: the earlier form let it escape and the
            # report was never written, so the run that hit the budget left no record at all.
            status = 124
            combined = ''
            for stream in (expired.stdout, expired.stderr):
                if stream:
                    combined += stream if isinstance(stream, str) else stream.decode('utf-8', 'replace')
        lines = combined.strip().splitlines()
        # Executed and failed come from Boost's own counts in this run's output, not from the
        # selector's reach. A selector that ran nothing must not report its target as executed.
        selected_here, executed, failed_here = _boostCounts(combined)
        # The announcement is authoritative for `selected` when the selector completed; when both
        # agree there is nothing to choose between them, and when they disagree the announcement is
        # the larger, which is what the roster promised to cover.
        if selected_here:
            selected = selected_here
        log_path = None
        if arguments.report:
            log_path = str(Path(arguments.report).with_suffix('')) + '.' + target['target'] + \
                '.' + pattern.replace('/', '_').replace('*', 'all') + '.log'
            Path(log_path).write_text(combined)
        results.append({'target': target['target'], 'selector': pattern, 'status': status,
                        'passed': status == 0, 'log': log_path, 'selected': selected,
                        'executed': executed, 'failed': failed_here,
                        'tail': lines[-6:]})
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

    prefix = installed_prefix()
    if prefix is not None:
        # Installed mode: binaries and fixtures come from the prefix, and a build-root override is
        # refused rather than honoured, because accepting one would let an installed check run
        # against a source tree it was not qualified with.
        if arguments.build_root:
            sys.stderr.write('BUILD_ROOT_REFUSED_IN_INSTALLED_MODE: %s\n' % arguments.build_root)
            return EXIT_ROSTER
        search = [str(prefix / 'libexec' / 'ndnsf-tests'), str(prefix / 'bin')]
        if arguments.fixture_root is None:
            arguments.fixture_root = str(prefix / 'share' / 'ndnsf' / 'tests' / 'fixtures')
    else:
        checkout = Path(__file__).resolve().parent.parent
        search = (( [arguments.build_root] if arguments.build_root else []) +
                  [str(checkout / 'build'), str(checkout)])

    environ = dict(os.environ)
    if arguments.fixture_root:
        environ['NDNSF_TEST_FIXTURE_ROOT'] = str(Path(arguments.fixture_root).resolve())
    # An installed run must not silently pick up a developer's build tree, site packages or a
    # preloaded library: each of those would let a check pass against something the prefix does not
    # actually deliver.
    for name in ('PYTHONPATH', 'LD_PRELOAD', 'LD_LIBRARY_PATH'):
        environ.pop(name, None)
    if prefix is not None:
        # The installed artifact has no build-tree RPATH, so its libraries are resolved from the
        # prefix explicitly. This is the environment an installed consumer runs in, not a
        # developer's; the developer's own LD_LIBRARY_PATH was just removed.
        environ['LD_LIBRARY_PATH'] = str(prefix / 'lib')
    elif arguments.build_root:
        # Source mode: the artifact carries no RPATH either, so the build tree it was built in is
        # bound explicitly. Without this the loader resolves whatever the host cache holds, which
        # may be an older installed library rather than the candidate under test.
        environ['LD_LIBRARY_PATH'] = str(Path(arguments.build_root).resolve())
    if prefix is not None:
        # An installed artifact must not name the build tree in its RPATH: that path does not exist
        # on another machine, so a binary carrying it is not portable even if it runs here. This is
        # checked, not assumed, because the build wrote the RPATH and only the artifact knows it.
        for binary in sorted(Path(search[0]).glob('*')) if Path(search[0]).is_dir() else []:
            if not binary.is_file():
                continue
            rpath = subprocess.run(['readelf', '-d', str(binary)], capture_output=True, text=True)
            line = next((entry for entry in rpath.stdout.splitlines()
                         if 'Library rpath' in entry or 'Library runpath' in entry), '')
            for suspect in ('/home/', '/build/'):
                if suspect in line:
                    sys.stderr.write('INSTALLED_RPATH_REFERENCES_BUILD_TREE: %s (%s)\n'
                                     % (binary, line.strip()))
                    return EXIT_ROSTER

    missing = check_assets(roster, selected, arguments.asset_manifest)
    if missing:
        for asset_name, detail in missing:
            sys.stderr.write('PREREQUISITE_MISSING: %s (%s)\n' % (asset_name, detail))
        report = {'schema': 'ndnsf-check-report-v1', 'profile': arguments.profile,
                  'status': 'PREREQUISITE_MISSING', 'missing': missing}
        if arguments.report:
            Path(arguments.report).write_text(json.dumps(report, indent=2) + '\n')
        return EXIT_PREREQUISITE

    # Four numbers, because they answer four different questions and conflating them is how a
    # binary's total got reported as a run's coverage: what the binary holds, what the roster
    # selected, what actually ran, and what could not run at all.
    report = {'schema': 'ndnsf-check-report-v1', 'profile': arguments.profile,
              'status': 'RUN', 'selected': selected, 'targets': [], 'unmatched': 0,
              'discovered': 0, 'selectedCases': 0, 'executed': 0, 'failed': 0,
              'unavailable': 0}
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
            entries = run_target(binary, target, environ, arguments.timeout, arguments)
            for entry in entries:
                entry['binary'] = str(binary)
                entry['suites'] = len(suites)
                entry['cases'] = len(cases)
                if not entry['passed']:
                    failed = True
            report['targets'].append({'target': target['target'], 'binary': str(binary),
                                      'suites': len(suites), 'cases': len(cases),
                                      'runs': entries})

    # `discovered` is the binary's listing; `selected` is what the selector announced. They can
    # differ by one -- `integration-tests` lists 345 cases and announces 346 under `--run_test=*`,
    # because Boost's explicit filter also matches the module-level node that the default run does
    # not count. Both are measured; the gap is recorded rather than smoothed away.
    report['discovered'] = sum(target.get('cases', 0) for target in report['targets'])
    # A selector chooses a subset of the binary; its own count comes from the selector's listing, not
    # from the binary's total. `run_target` records what each selector actually reached.
    report['selectedCases'] = sum(run.get('selected', 0)
                                  for target in report['targets']
                                  for run in target.get('runs', []))
    report['executed'] = sum(run.get('executed', 0)
                             for target in report['targets']
                             for run in target.get('runs', []))
    report['failed'] = sum(run.get('failed', 0)
                           for target in report['targets']
                           for run in target.get('runs', []))
    report['unavailable'] = sum(1 for target in report['targets']
                                if any(run.get('status') == 124
                                       for run in target.get('runs', [])))
    report['status'] = 'FAILED' if failed else 'PASSED'
    if arguments.report:
        Path(arguments.report).write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k: report[k] for k in ('status', 'profile', 'discovered', 'selectedCases',
                                              'executed', 'failed', 'unavailable')}))
    return EXIT_FAILED if failed else EXIT_OK


if __name__ == '__main__':
    sys.exit(main())
