"""Exercise the source gate with real sudo Git ownership checks."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

import pytest

ROOT = Path(__file__).resolve().parents[2]


@pytest.mark.parametrize('matching_owner', [True, False])
def test_sudo_gate_accepts_only_the_actual_checkout_owner(matching_owner):
    if not shutil.which('sudo'):
        pytest.skip('sudo is unavailable')
    uid = os.getuid() if os.getuid() else int(os.environ.get('SUDO_UID', ROOT.stat().st_uid))
    if uid == 0:
        pytest.skip('a distinct invoking user is required')
    if subprocess.run(['sudo', '-n', 'true'], capture_output=True).returncode:
        pytest.skip('non-interactive sudo is unavailable')
    owner_command = ['sudo', '-n', '-u', '#' + str(uid)] if os.getuid() == 0 else []
    environment = {k: v for k, v in os.environ.items() if not k.startswith('GIT_')}
    environment.update(GIT_CONFIG_NOSYSTEM='1', GIT_CONFIG_GLOBAL=os.devnull)
    with tempfile.TemporaryDirectory(prefix='spec181-sudo-source-') as temporary:
        checkout = Path(temporary)
        if os.getuid() == 0:
            os.chown(checkout, uid, -1)
        (checkout / 'source.txt').write_text('registered source\n')

        def git(*args):
            return subprocess.check_output(
                owner_command + ['git', '-C', temporary, *args],
                env=environment, stderr=subprocess.PIPE, text=True).strip()

        git('init', '-q')
        git('add', '--', 'source.txt')
        git('-c', 'user.name=Spec181 fixture', '-c',
            'user.email=spec181@example.invalid', '-c', 'commit.gpgsign=false',
            'commit', '-qm', 'Seal source fixture')
        revision = git('rev-parse', 'HEAD')
        code = '''
import os, sys
from pathlib import Path
sys.path.insert(0, sys.argv[1])
from run_spec180_local_gate import _validate_source_checkout
os.environ['SUDO_UID'] = sys.argv[4]
os.environ['GIT_DIR'] = '/not-the-selected-repository'
os.environ['GIT_INDEX_FILE'] = '/not-the-selected-index'
_validate_source_checkout(Path(sys.argv[2]), sys.argv[3])
print('SUDO_SOURCE_CHECK_OK')
'''
        result = subprocess.run(
            ['sudo', '-n', '/usr/bin/python3', '-c', code,
             str(ROOT / 'scripts'), temporary, revision,
             str(uid if matching_owner else uid + 1)], capture_output=True, text=True)
        if matching_owner:
            assert result.returncode == 0, result.stderr
            assert 'SUDO_SOURCE_CHECK_OK' in result.stdout
        else:
            assert result.returncode != 0
            assert 'SOURCE_CHECKOUT_UNAVAILABLE' in result.stderr
