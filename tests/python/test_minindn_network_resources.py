"""Kernel-resource observation boundaries; real probe explicitly requires root."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
from types import SimpleNamespace

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from Experiments import minindn_network_resources as resources


def add_process(root, pid, namespace):
    path = root/str(pid)
    (path/'ns').mkdir(parents=True)
    (path/'ns/net').symlink_to(namespace)
    (path/'fd').mkdir()
    (path/'task').mkdir()
    (path/'task'/str(pid)).symlink_to('..')
    (path/'stat').write_text(str(pid)+' (name with spaces) '+' '.join(['S']+['0']*18+['123']))
    return path


@pytest.fixture
def kernel(tmp_path):
    root = tmp_path/'proc'
    (root/'self/ns').mkdir(parents=True)
    (root/'self/ns/net').symlink_to('net:[1]')
    (root/'self/mountinfo').write_text('')
    private = add_process(root, 22, 'net:[2]')
    shared = add_process(root, 33, 'net:[1]')
    nodes = [SimpleNamespace(name='private', pid=22, intfList=lambda: [SimpleNamespace(name='collision')]),
             SimpleNamespace(name='shared', pid=33, intfList=lambda: [SimpleNamespace(name='owned'),SimpleNamespace(name='lo')])]
    network = SimpleNamespace(net=SimpleNamespace(hosts=nodes))
    captured = resources.capture(network, proc_root=root, interfaces=[(1,'lo'),(5,'owned'),(9,'collision')])
    return root, private, shared, captured


def test_capture_excludes_shared_namespace_loopback_and_private_name_collision(kernel):
    _root, _private, _shared, captured = kernel
    assert captured['namespaces'] == ['net:[2]']
    assert captured['interfaces'] == [dict(name='owned', ifindex=5)]
    assert all(row['startTicks'] == 123 for row in captured['nodes'])


def test_remaining_process_fd_and_interface_each_prevent_clean(kernel):
    root, private, shared, captured = kernel
    assert not resources.inspect(captured, proc_root=root, interfaces=[])['clean']
    (shared/'fd/7').symlink_to('net:[2]')
    shutil.rmtree(private)
    observed = resources.inspect(captured, proc_root=root, interfaces=[])
    assert not observed['clean'] and observed['references'][0]['kind'] == 'fd'
    (shared/'fd/7').unlink()
    assert not resources.inspect(captured, proc_root=root, interfaces=[(5,'owned')])['clean']
    assert resources.inspect(captured, proc_root=root, interfaces=[(6,'owned'),(9,'collision')])['clean']


def test_non_leader_thread_namespace_prevents_clean(kernel):
    root, private, shared, captured = kernel
    shutil.rmtree(private)
    task = shared/'task/34'
    (task/'ns').mkdir(parents=True)
    (task/'ns/net').symlink_to('net:[2]')
    (task/'fd').mkdir()
    observed = resources.inspect(captured, proc_root=root, interfaces=[])
    assert not observed['clean'] and observed['references'][0]['tid'] == 34


def test_nsfs_mount_keeps_namespace_alive(kernel, tmp_path):
    root, private, _shared, captured = kernel
    shutil.rmtree(private)
    pin = tmp_path/'pinned'
    pin.touch()
    captured['namespaces'] = ['net:[%d]' % pin.stat().st_ino]
    (root/'self/mountinfo').write_text('1 2 0:1 / '+str(pin)+' rw - nsfs nsfs rw\n')
    observed = resources.inspect(captured, proc_root=root, interfaces=[])
    assert not observed['clean'] and observed['references'][0]['kind'] == 'mount'


def test_unreadable_namespace_is_not_clean(kernel, monkeypatch):
    root, _private, _shared, captured = kernel
    original = resources._namespace
    def unreadable(path):
        if '/22/' in str(path): raise PermissionError('fixture')
        return original(path)
    monkeypatch.setattr(resources, '_namespace', unreadable)
    with pytest.raises(PermissionError): resources.inspect(captured, proc_root=root, interfaces=[])


def test_observation_deadline_is_not_clean(kernel, monkeypatch):
    root, _private, _shared, captured = kernel
    values = iter([0, 10])
    monkeypatch.setattr(resources.time, 'monotonic', lambda: next(values))
    with pytest.raises(RuntimeError, match='NETWORK_OBSERVATION_TIMEOUT'):
        resources.inspect(captured, proc_root=root, interfaces=[], seconds=1)


@pytest.mark.skipif(os.geteuid() != 0, reason='explicit root-only kernel namespace probe')
def test_real_namespace_remains_while_fd_is_pinned_then_disappears():
    parent_namespace = os.readlink('/proc/self/ns/net')
    proc = subprocess.Popen(['/usr/bin/unshare','--net','/usr/bin/python3','-c',
                             'import time; time.sleep(30)'], stderr=subprocess.PIPE)
    descriptor = None
    try:
        deadline = time.monotonic()+5
        while os.readlink('/proc/%d/ns/net' % proc.pid) == parent_namespace:
            if proc.poll() is not None or time.monotonic() >= deadline:
                pytest.fail('namespace child did not start')
            time.sleep(.01)
        node = SimpleNamespace(name='probe', pid=proc.pid, intfList=lambda: [])
        captured = resources.capture(SimpleNamespace(net=SimpleNamespace(hosts=[node])))
        assert captured['namespaces'] and not resources.inspect(captured)['clean']
        descriptor = os.open('/proc/%d/ns/net' % proc.pid, os.O_RDONLY)
        proc.terminate(); proc.wait(timeout=3)
        observed = resources.inspect(captured)
        assert not observed['clean'] and any(row['kind']=='fd' for row in observed['references'])
        os.close(descriptor); descriptor = None
        assert resources.inspect(captured)['clean']
    finally:
        if descriptor is not None: os.close(descriptor)
        if proc.poll() is None: proc.kill()
        proc.wait(timeout=3)
        proc.stderr.close()
