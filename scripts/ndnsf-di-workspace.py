#!/usr/bin/env python3
"""Run a command in a locked, reusable disk workspace, separate from evidence.

Example: python3 scripts/ndnsf-di-workspace.py build -- command {workspace}
Only this tool's marked `work` directory is reset. Model caches and archived
experiment evidence must live outside it. This does not impose a disk quota.
"""
import argparse
import contextlib
import fcntl
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess


DEFAULT_ROOT = Path(f"/var/tmp/ndnsf-di-workspaces-{os.geteuid()}")
MARKER = "ndnsf-di-disposable-workspace-v1\n"


def mount_entries():
    for line in Path("/proc/self/mountinfo").read_text().splitlines():
        fields = line.split()
        mount = re.sub(r"\\([0-7]{3})", lambda m: chr(int(m[1], 8)),
                       fields[4])
        yield Path(mount), fields[fields.index('-') + 1]


def reject_memory_filesystem(path):
    """Reject known RAM-backed filesystems even outside /dev/shm."""
    ancestors = [(point, filesystem) for point, filesystem in mount_entries()
                 if point == path or point in path.parents]
    if not ancestors:
        raise RuntimeError('WORKSPACE_FILESYSTEM_UNRESOLVED:' + str(path))
    _, filesystem = max(ancestors, key=lambda row: len(row[0].parts))
    if filesystem in ('rootfs', 'ramfs', 'tmpfs'):
        raise ValueError('WORKSPACE_REQUIRES_DISK_NOT_' + filesystem.upper())


def reject_mounts(path):
    """Reject nested mounts, including same-device bind mounts, before reset."""
    for point, _ in mount_entries():
        if point == path or path in point.parents:
            raise RuntimeError(f"WORKSPACE_CONTAINS_MOUNT:{point}")


def private_directory(path):
    # Refuse symlink aliases and wrong-owner directories before any reset.
    if path.resolve() != path or path.is_symlink():
        raise RuntimeError(f"WORKSPACE_SYMLINK:{path}")
    path.mkdir(mode=0o700, parents=True, exist_ok=True)
    stat = path.stat()
    if not path.is_dir() or stat.st_uid != os.geteuid() or stat.st_mode & 0o077:
        raise RuntimeError(f"WORKSPACE_NOT_PRIVATE:{path}")


def reject_live_users(path):
    """Protect orphaned workers even if they closed their inherited lease."""
    prefix = str(path) + '/'
    for process in Path('/proc').iterdir():
        if not process.name.isdigit():
            continue
        try:
            links = [process / 'cwd', process / 'exe', *list((process / 'fd').iterdir())]
            for link in links:
                try:
                    target = os.readlink(link)
                    if target.endswith(' (deleted)'):
                        target = target[:-10]
                except OSError:
                    continue
                if target == str(path) or target.startswith(prefix):
                    raise RuntimeError(f'WORKSPACE_PROCESS_ACTIVE:{process.name}')
            try:
                mappings = (process / 'maps').read_text()
            except OSError:
                mappings = ''
            if prefix in mappings:
                raise RuntimeError(f'WORKSPACE_PROCESS_ACTIVE:{process.name}')
        except (FileNotFoundError, PermissionError, ProcessLookupError):
            continue


@contextlib.contextmanager
def workspace(root, name):
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_-]{0,63}", name):
        raise ValueError("WORKSPACE_NAME_INVALID")
    root = Path(os.path.abspath(root.expanduser()))
    if root == Path('/dev/shm') or Path('/dev/shm') in root.parents:
        raise ValueError("WORKSPACE_REQUIRES_DISK_NOT_DEV_SHM")
    private_directory(root)
    reject_memory_filesystem(root)
    slot = root / name
    private_directory(slot)
    fd = os.open(slot / '.active.lock', os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
    try:
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as exc:
            raise RuntimeError(f"WORKSPACE_BUSY:{slot}") from exc
        marker = slot / '.disposable'
        work = slot / 'work'
        if marker.is_symlink() or work.is_symlink():
            raise RuntimeError(f"WORKSPACE_SYMLINK:{slot}")
        if marker.exists():
            if marker.read_text() != MARKER:
                raise RuntimeError(f"WORKSPACE_MARKER_INVALID:{slot}")
        else:
            if work.exists():
                raise RuntimeError(f"WORKSPACE_UNMANAGED:{work}")
            marker.write_text(MARKER)
        reject_mounts(work)
        if work.exists():
            if not work.is_dir():
                raise RuntimeError(f"WORKSPACE_ENTRY_INVALID:{work}")
            reject_live_users(work)
            shutil.rmtree(work)
        work.mkdir(mode=0o700)
        yield work, fd
    finally:
        # Closing releases the lease; do not explicitly unlock it while an
        # inherited descriptor in a still-running child might retain it.
        os.close(fd)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('name', help='stable purpose name, e.g. build or yolo')
    parser.add_argument('--root', type=Path, default=DEFAULT_ROOT)
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    command = args.command
    if command[:1] == ['--']:
        command = command[1:]
    if not command:
        parser.error('a command after -- is required')
    with workspace(args.root, args.name) as (work, lock_fd):
        temporary = work / 'tmp'
        temporary.mkdir(mode=0o700)
        env = {**os.environ, 'TMPDIR': str(temporary),
               'NDNSF_DI_WORKSPACE': str(work)}
        command = [arg.replace('{workspace}', str(work)) for arg in command]
        print(f'NDNSF_DI_REUSABLE_WORKSPACE={work}', flush=True)
        # The inherited flock remains held through exec and child lifetime.
        proc = subprocess.Popen(command, cwd=work, env=env,
                                start_new_session=True, pass_fds=(lock_fd,))
        def stop(signum, frame):
            raise KeyboardInterrupt
        old = {s: signal.signal(s, stop) for s in (signal.SIGINT, signal.SIGTERM)}
        try:
            return proc.wait()
        except KeyboardInterrupt:
            try:
                os.killpg(proc.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(proc.pid, signal.SIGKILL)
                proc.wait()
            return 130
        finally:
            for sig, handler in old.items():
                signal.signal(sig, handler)


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError) as exc:
        raise SystemExit(str(exc))
