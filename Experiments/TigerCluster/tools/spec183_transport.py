#!/usr/bin/env python3
"""Internal explicit transport inventory/receiver; does not qualify or submit."""
import argparse
import json
from pathlib import Path
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from runtime.yolo_profile import _read_plane
from runtime.yolo_transport import inventory, receive


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('inventory', 'receive'))
    parser.add_argument('--artifact-root', required=True)
    parser.add_argument('--run-root', required=True)
    parser.add_argument('--input', required=True, type=Path,
                        help='Explicit JSON file list for inventory, manifest for receive')
    parser.add_argument('--candidate-digest')
    parser.add_argument('--staging', type=Path)
    parser.add_argument('--lock-root', type=Path,
                        help='Same sharedLockRoot used by every submitter; required for receive')
    parser.add_argument('--private-file', action='append', default=[],
                        help='Explicit owner-only secret in the inventory file list')
    args = parser.parse_args()
    value = _read_plane(args.input)
    roots = (args.artifact_root, args.run_root)
    if args.action == 'inventory':
        if not isinstance(value, list) or not all(isinstance(p, str) for p in value):
            raise ValueError('TRANSPORT_FILE_LIST')
        result = inventory(value, roots=roots, candidate_digest=args.candidate_digest,
                           private_paths=args.private_file)
    else:
        if args.staging is None or args.lock_root is None:
            parser.error('receive requires --staging and --lock-root')
        result = receive(value, roots=roots, staging=args.staging, lock_root=args.lock_root)
    print(json.dumps(result, sort_keys=True))


if __name__ == '__main__':
    main()
