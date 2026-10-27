"""Shared process ownership and bounded cleanup; no application protocol decisions."""
from __future__ import annotations

import json
import os
from pathlib import Path
import signal
import subprocess
import time


def write_json(path: Path, value: dict) -> None:
    """Atomically publish a complete private operation record."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + f".{os.getpid()}.tmp")
    descriptor = os.open(str(tmp), os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(descriptor, 'w') as stream:
            json.dump(value, stream, indent=2, sort_keys=True)
            stream.write('\n')
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(tmp, path)
    finally:
        if tmp.exists():
            tmp.unlink()


class Processes:
    """Own child process groups and their logs until close has reaped them."""
    def __init__(self, log_dir: Path):
        self.log_dir = log_dir
        log_dir.mkdir(parents=True, exist_ok=True)
        self.children = []

    def start(self, name: str, argv: list[str], env: dict | None = None):
        log = (self.log_dir / (name + '.log')).open('wb')
        try:
            child = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=log,
                                     stderr=subprocess.STDOUT, start_new_session=True, env=env)
        except BaseException:
            log.close()
            raise
        self.children.append((name, child, log))
        return child

    def check(self):
        for name, child, _ in self.children:
            if child.poll() is not None:
                raise RuntimeError(f'CHILD_EXIT:{name}:{child.returncode}')

    def close(self) -> list[dict]:
        result = []
        for name, child, log in reversed(self.children):
            before = child.poll()
            forced = False
            error = None
            try:
                try:
                    child.terminate()
                except ProcessLookupError:
                    pass
                try:
                    child.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    forced = True
                    os.killpg(child.pid, signal.SIGKILL)
                    child.wait(timeout=5)
                try:
                    os.killpg(child.pid, 0)
                    os.killpg(child.pid, signal.SIGKILL)
                    forced = True
                except ProcessLookupError:
                    pass
            except (OSError, subprocess.TimeoutExpired) as failure:
                error = str(failure)
            finally:
                log.close()
            result.append({'name': name, 'pid': child.pid, 'exitCode': child.returncode,
                           'exitedBeforeCleanup': before is not None, 'forced': forced,
                           'reaped': child.poll() is not None, 'error': error})
        self.children.clear()
        return result


def run_finite_application(name, argv, log_path, cleanup, seconds=60,
                           allowed_exits=(0,), env=None):
    """Wait for an owned application; retain its actual exit and bounded group cleanup."""
    with Path(log_path).open('wb') as log:
        proc = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=log,
                                stderr=subprocess.STDOUT, start_new_session=True, env=env)
        forced = False
        primary_failure = False
        try:
            rc = proc.wait(timeout=seconds)
            if rc not in allowed_exits:
                raise RuntimeError('APP_EXIT:' + name + ':' + str(rc))
            return rc
        except BaseException:
            primary_failure = True
            raise
        finally:
            error = None
            try:
                if proc.poll() is None:
                    proc.terminate()
                    try:
                        proc.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        forced = True
                        os.killpg(proc.pid, signal.SIGKILL)
                        proc.wait(timeout=5)
                try:
                    os.killpg(proc.pid, 0)
                    os.killpg(proc.pid, signal.SIGKILL)
                    forced = True
                except ProcessLookupError:
                    pass
            except (OSError, subprocess.TimeoutExpired) as failure:
                error = str(failure)
            finally:
                cleanup.append({'name': name, 'kind': 'finite', 'pid': proc.pid,
                                'exitCode': proc.poll(), 'forced': forced,
                                'reaped': proc.poll() is not None,
                                'exitedBeforeCleanup': False, 'error': error})
            if error is not None and not primary_failure:
                raise RuntimeError('APP_CLEANUP:' + name + ':' + error)


_BINDINGS = ('NDNSF_DEPLOYMENT_ACTOR_ID', 'NDNSF_DEPLOYMENT_BOOT_ID',
             'NDNSF_DEPLOYMENT_CONFIG_SHA256')


def process_identity(pid):
    """Read Linux process identity; absent is None and unreadable raises OSError."""
    if type(pid) is not int or pid <= 1:
        raise ValueError('invalid owned PID')
    root = Path('/proc') / str(pid)
    try:
        fields = (root / 'stat').read_text().rsplit(')', 1)[1].split()
        result = {'pid': pid, 'uid': root.stat().st_uid, 'pgid': int(fields[2]),
                  'start_ticks': int(fields[19]), 'state': fields[0]}
        if fields[0] == 'Z':
            return dict(result, exe='', bindings={})
        values = dict(pair.split(b'=', 1) for pair in (root / 'environ').read_bytes().split(b'\0')
                      if b'=' in pair)
        exe = os.readlink(root / 'exe')
        if exe.endswith(' (deleted)'):
            exe = exe[:-10]
        return dict(result, exe=exe,
                    bindings={key: values.get(key.encode(), b'').decode('utf-8')
                              for key in _BINDINGS})
    except FileNotFoundError:
        if root.exists():
            raise
        return None


def owned_state(record):
    """Never signal a PID whose original ownership proof is missing or mismatched."""
    try:
        if (not isinstance(record, dict) or
                any(type(record.get(key)) is not int for key in
                    ('pid', 'uid', 'pgid', 'start_ticks')) or record['pid'] <= 1 or
                record['start_ticks'] <= 0 or not isinstance(record.get('exe'), str) or
                not Path(record['exe']).is_absolute() or
                record.get('uid') != os.getuid() or record.get('pgid') != record['pid'] or
                not isinstance(record.get('bindings'), dict) or
                any(not isinstance(record['bindings'].get(key), str) or not record['bindings'][key]
                    for key in _BINDINGS)):
            return 'UNKNOWN'
        actual = process_identity(record['pid'])
        if actual is None:
            return 'EXITED'
        fields = ('pid', 'uid', 'pgid', 'start_ticks')
        if any(actual[key] != record[key] for key in fields):
            return 'UNKNOWN'
        if actual['state'] == 'Z':
            return 'EXITED'
        return 'RUNNING' if all(actual[key] == record[key] for key in ('exe', 'bindings')) else 'UNKNOWN'
    except (OSError, ValueError, KeyError, IndexError, UnicodeError):
        return 'UNKNOWN'


def _group_members(record):
    members = []
    for root in Path('/proc').iterdir():
        if not root.name.isdigit():
            continue
        try:
            fields = (root / 'stat').read_text().rsplit(')', 1)[1].split()
            if int(fields[2]) != record['pid']:
                continue
            member = process_identity(int(root.name))
            if member is None:
                continue
            if member['uid'] != record['uid'] or (
                    member['state'] != 'Z' and member['bindings'] != record['bindings']):
                raise RuntimeError('group ownership is unknown')
            members.append(member)
        except (FileNotFoundError, ProcessLookupError):
            if root.exists():
                raise OSError('group member changed during audit')
            continue
    if not members:
        try:
            os.killpg(record['pid'], 0)
        except ProcessLookupError:
            return members
        raise RuntimeError('process group exists but cannot be audited')
    return members


def stop_owned(record, term_seconds=5, kill_seconds=5):
    """Bounded TERM/KILL with identity checks; preserve UNKNOWN instead of assuming zero."""
    result = {'pid': record.get('pid') if isinstance(record, dict) else None,
              'status': 'UNKNOWN', 'forced': False}
    try:
        if any(type(value) not in (int, float) or not 0 <= value <= 60
               for value in (term_seconds, kill_seconds)):
            raise ValueError('cleanup intervals must be bounded seconds')
        if owned_state(record) == 'UNKNOWN':
            return result
        members = _group_members(record)
        if not members:
            result['status'] = 'ZERO'
            return result
        if any(m['pid'] == record['pid'] and m['state'] != 'Z' for m in members):
            try:
                os.kill(record['pid'], signal.SIGTERM)
            except ProcessLookupError:
                pass
        else:
            try:
                os.killpg(record['pid'], signal.SIGTERM)
            except ProcessLookupError:
                result['status'] = 'ZERO'
                return result
        for duration, force in ((term_seconds, False), (kill_seconds, True)):
            deadline = time.monotonic() + duration
            while time.monotonic() < deadline:
                try:
                    waited, status = os.waitpid(record['pid'], os.WNOHANG)
                    if waited:
                        result['exitCode'] = (os.WEXITSTATUS(status) if os.WIFEXITED(status)
                                              else -os.WTERMSIG(status))
                        result['reaped'] = True
                except ChildProcessError:
                    pass
                # Linux can deny /proc reads while a signalled task exits. Wait
                # within the existing deadline; never signal an uncertain PID.
                if owned_state(record) == 'UNKNOWN':
                    time.sleep(0.05)
                    continue
                try:
                    members = _group_members(record)
                except OSError:
                    # A task can exit between identity and group reads. Audit
                    # again within this deadline before sending another signal.
                    time.sleep(0.05)
                    continue
                if not members:
                    result['status'] = 'ZERO'
                    return result
                if force:
                    try:
                        os.killpg(record['pid'], signal.SIGKILL)
                    except ProcessLookupError:
                        result['status'] = 'ZERO'
                        return result
                    result['forced'] = True
                time.sleep(0.05)
        if owned_state(record) == 'UNKNOWN':
            return result
        members = _group_members(record)
        result.update(status='SURVIVORS' if members else 'ZERO',
                      survivors=[m['pid'] for m in members])
    except (OSError, ValueError, KeyError, IndexError, UnicodeError, RuntimeError) as error:
        result['error'] = str(error)
    return result
