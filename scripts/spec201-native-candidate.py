#!/usr/bin/env python3
"""Freeze a source/installed/model bundle or validate it without launch."""
import argparse
import json
import os
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'Experiments'))
from native_install_candidate import (freeze_spec201_candidate, sha256_file,
                                     validate_spec201_candidate, write_candidate)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('operation', choices=('freeze', 'check'))
    parser.add_argument('--blueprint', type=Path)
    parser.add_argument('--candidate', type=Path, required=True)
    parser.add_argument('--sha256', default='')
    args = parser.parse_args()
    # Freeze and check the same installed-library paths used by MiniNDN
    # launchers, rather than the build RPATH selected by an ambient loader.
    # The validator still checks every resolved library path and digest.
    os.environ['LD_LIBRARY_PATH'] = '/usr/local/lib'
    if args.operation == 'freeze':
        if args.blueprint is None:
            parser.error('--blueprint is required for freeze')
        blueprint = json.loads(args.blueprint.read_text())
        candidate = freeze_spec201_candidate(
            files={group: [Path(path) for path in paths]
                   for group, paths in blueprint['files'].items()},
            artifacts=[(Path(row['installed']), Path(row['build']))
                       for row in blueprint['artifacts']],
            native_receipts=[Path(path) for path in blueprint['nativeReceipts']],
            prefix_role_plans=blueprint['prefixRolePlans'],
            installed_headers=[(Path(row['source']), Path(row['installed']))
                               for row in blueprint['installedHeaders']],
            profile_arguments=blueprint['profileArguments'],
            credential_bundles=blueprint['credentialBundles'],
            sdk_receipts=[Path(path) for path in blueprint.get('sdkReceipts', [])])
        # Write a draft only after all inputs can be read. The validator below
        # must succeed before this file is eligible for either model launcher.
        write_candidate(args.candidate, candidate)
        digest = sha256_file(args.candidate)
    else:
        digest = args.sha256
    candidate = validate_spec201_candidate(args.candidate, digest, [])
    print(json.dumps({'status': 'PASS', 'candidateId': candidate['candidateId'],
                      'sha256': digest, 'candidate': str(args.candidate.resolve())}))


if __name__ == '__main__':
    main()
