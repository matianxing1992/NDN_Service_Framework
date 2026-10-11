"""Host storage tests; no inference or qualification assertions."""
import importlib.util
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


SCRIPT = pathlib.Path(__file__).resolve().parents[2] / 'scripts/ndnsf-di-workspace.py'
SPEC = importlib.util.spec_from_file_location('workspace', SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class WorkspaceTests(unittest.TestCase):
    def test_real_commands_reuse_path_and_clear_previous_scratch(self):
        with tempfile.TemporaryDirectory() as private:
            root = pathlib.Path(private) / 'slots'
            command = [sys.executable, str(SCRIPT), '--root', str(root), 'build', '--',
                       sys.executable, '-c',
                       'import os,pathlib; p=pathlib.Path(os.environ["NDNSF_DI_WORKSPACE"]); '
                       'assert not (p/"old").exists(); (p/"old").write_bytes(b"x"*65536)']
            for _ in range(2):
                subprocess.run(command, check=True, stdout=subprocess.DEVNULL)
            self.assertEqual([p.name for p in root.iterdir()], ['build'])
            self.assertEqual((root / 'build/work/old').stat().st_size, 65536)

    def test_busy_lease_preserves_existing_work(self):
        with tempfile.TemporaryDirectory() as private:
            root = pathlib.Path(private) / 'slots'
            with MODULE.workspace(root, 'build') as (work, _):
                (work / 'keep').write_text('live')
                result = subprocess.run(
                    [sys.executable, str(SCRIPT), '--root', str(root), 'build', '--',
                     sys.executable, '-c', 'pass'], capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('WORKSPACE_BUSY', result.stderr)
                self.assertEqual((work / 'keep').read_text(), 'live')

    def test_unmanaged_or_symlink_work_is_not_removed(self):
        with tempfile.TemporaryDirectory() as private:
            root = pathlib.Path(private) / 'slots'
            work = root / 'build/work'
            work.mkdir(mode=0o700, parents=True)
            root.chmod(0o700)
            work.parent.chmod(0o700)
            (work / 'keep').write_text('evidence')
            with self.assertRaisesRegex(RuntimeError, 'WORKSPACE_UNMANAGED'):
                with MODULE.workspace(root, 'build'):
                    pass
            self.assertEqual((work / 'keep').read_text(), 'evidence')
            (work / 'keep').unlink()
            work.rmdir()
            external = pathlib.Path(private) / 'external'
            external.mkdir()
            (external / 'keep').write_text('external')
            work.symlink_to(external, target_is_directory=True)
            with self.assertRaisesRegex(RuntimeError, 'WORKSPACE_SYMLINK'):
                with MODULE.workspace(root, 'build'):
                    pass
            self.assertEqual((external / 'keep').read_text(), 'external')

    def test_named_workspace_refuses_shm(self):
        with self.assertRaisesRegex(ValueError, 'DISK_NOT_DEV_SHM'):
            with MODULE.workspace(pathlib.Path('/dev/shm'), 'build'):
                pass

    def test_orphaned_process_blocks_reset_after_parent_lease_closes(self):
        with tempfile.TemporaryDirectory() as private:
            root = pathlib.Path(private) / 'slots'
            with MODULE.workspace(root, 'build') as (work, _):
                (work / 'keep').write_text('live')
                child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(30)'],
                                         cwd=work)
            try:
                with self.assertRaisesRegex(RuntimeError, 'WORKSPACE_PROCESS_ACTIVE'):
                    with MODULE.workspace(root, 'build'):
                        pass
                self.assertEqual((work / 'keep').read_text(), 'live')
            finally:
                child.terminate()
                child.wait(timeout=5)

    def test_mount_in_work_is_rejected_before_deletion(self):
        with tempfile.TemporaryDirectory() as private:
            root = pathlib.Path(private) / 'slots'
            with MODULE.workspace(root, 'build') as (work, _):
                (work / 'keep').write_text('mounted')
            mountinfo = f'99 1 8:5 / {work}/nested rw - ext4 /dev/test rw\n'
            original = pathlib.Path.read_text
            def read(path, *args, **kwargs):
                if path == pathlib.Path('/proc/self/mountinfo'):
                    return mountinfo
                return original(path, *args, **kwargs)
            with mock.patch.object(pathlib.Path, 'read_text', read):
                with self.assertRaisesRegex(RuntimeError, 'WORKSPACE_CONTAINS_MOUNT'):
                    with MODULE.workspace(root, 'build'):
                        pass
            self.assertEqual((work / 'keep').read_text(), 'mounted')


if __name__ == '__main__':
    unittest.main()
