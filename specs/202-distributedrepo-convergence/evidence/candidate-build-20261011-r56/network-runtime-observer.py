"""Read-only process runtime evidence around one privileged MiniNDN runner."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import time


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def identity(pid):
    text = (Path('/proc') / str(pid) / 'stat').read_text()
    fields = text[text.rindex(')') + 2:].split()
    return int(fields[1]), fields[19]


def save(path, data):
    temporary = path.with_name(path.name + '.tmp')
    temporary.write_text(json.dumps(data, sort_keys=True, indent=2) + '\n')
    temporary.replace(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--record', type=Path, required=True)
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command
    if command and command[0] == '--':
        command = command[1:]
    if os.geteuid() != 0 or not command or args.record.exists():
        parser.error('requires root read access, a command and fresh record')
    rows, known, errors = {}, {}, []
    process = None
    signals = []

    def forward(number, frame):
        signals.append(number)
        if process is not None and process.poll() is None:
            process.send_signal(number)

    signal.signal(signal.SIGINT, forward)
    signal.signal(signal.SIGTERM, forward)
    record = {'status': 'OBSERVING', 'command': command,
              'observer_sha256': digest(Path(__file__)), 'started_unix': time.time()}
    save(args.record, record)

    def sample():
        live = {}
        for entry in Path('/proc').iterdir():
            if not entry.name.isdigit():
                continue
            try:
                parent, birth = identity(int(entry.name))
                live[int(entry.name)] = (parent, birth)
            except (OSError, ValueError):
                continue
        # Remember identities after reparenting, but never inherit a reused PID.
        selected = {pid for pid, birth in known if live.get(pid, (None, None))[1] == birth}
        while True:
            new = {pid for pid, (parent, birth) in live.items() if parent in selected} - selected
            if not new:
                break
            selected.update(new)
        for pid in selected:
            parent, birth = live[pid]
            key = (pid, birth)
            known.setdefault(key, parent)
            proc = Path('/proc') / str(pid)
            try:
                executable = os.readlink(proc / 'exe')
                cmdline = (proc / 'cmdline').read_bytes().replace(b'\0', b' ').decode(errors='replace')
                maps = (proc / 'maps').read_text()
                if identity(pid)[1] != birth:
                    continue
            except (OSError, ValueError):
                continue
            row = rows.setdefault(str(pid) + ':' + birth, {
                'pid': pid, 'birth_ticks': birth, 'first_parent_pid': parent,
                'first_seen_unix': time.time(), 'executables': [], 'cmdlines': [], 'mappings': {}})
            row['last_seen_unix'] = time.time()
            if executable not in row['executables']:
                row['executables'].append(executable)
            if cmdline not in row['cmdlines']:
                row['cmdlines'].append(cmdline)
            for line in maps.splitlines():
                fields = line.split(None, 5)
                if len(fields) != 6 or not fields[5].startswith('/'):
                    continue
                path = re.sub(r'\\([0-7]{3})', lambda match: chr(int(match[1], 8)), fields[5])
                if '.so' not in path and path != executable:
                    continue
                mapping_key = path + ':' + fields[3] + ':' + fields[4]
                row['mappings'].setdefault(mapping_key, {'path': path, 'device': fields[3],
                    'inode': int(fields[4]), 'first_seen_unix': time.time()})

    try:
        process = subprocess.Popen(command)
        known[(process.pid, identity(process.pid)[1])] = None
        while process.poll() is None:
            sample()
            time.sleep(0.01)
        sample()
        cache = {}
        for row in rows.values():
            for mapping in row['mappings'].values():
                path = Path(mapping['path'])
                if mapping['path'].endswith(' (deleted)'):
                    raise RuntimeError('deleted mapped library: ' + str(path))
                metadata = path.stat()
                major, minor = (int(value, 16) for value in mapping['device'].split(':'))
                if (metadata.st_ino, os.major(metadata.st_dev), os.minor(metadata.st_dev)) != (mapping['inode'], major, minor):
                    raise RuntimeError('replaced mapped library: ' + str(path))
                key = (str(path), metadata.st_dev, metadata.st_ino)
                if key not in cache:
                    cache[key] = digest(path)
                mapping['sha256'] = cache[key]
                mapping['bytes'] = metadata.st_size
        record['status'] = 'CAPTURED_PENDING_COVERAGE_AUDIT'
    except BaseException as error:
        errors.append(repr(error))
        record['status'] = 'OBSERVATION_FAILED'
        if process is not None and process.poll() is None:
            process.send_signal(signal.SIGINT)
            try:
                process.wait(timeout=60)
            except subprocess.TimeoutExpired:
                record['forced_cleanup'] = []
                try:
                    sample()
                except BaseException as discovery_error:
                    errors.append('cleanup discovery: ' + repr(discovery_error))
                for pid, birth in list(known):
                    try:
                        if identity(pid)[1] == birth:
                            os.kill(pid, signal.SIGKILL)
                            record['forced_cleanup'].append({'pid': pid, 'birth_ticks': birth})
                    except ProcessLookupError:
                        pass
                    except OSError as cleanup_error:
                        errors.append(repr(cleanup_error))
                # Startup identity collection can fail before the runner enters known.
                if process.poll() is None:
                    process.kill()
                process.wait(timeout=10)
        raise
    finally:
        record.update({'runner_exit_status': process.poll() if process is not None else None, 'finished_unix': time.time(),
                       'signals': signals, 'processes': rows, 'errors': errors})
        save(args.record, record)
    return process.returncode if process.returncode >= 0 else 128 - process.returncode


if __name__ == '__main__':
    raise SystemExit(main())
