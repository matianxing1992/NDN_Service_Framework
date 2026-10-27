"""Exercise the Spec163 client's owner cleanup without starting a network."""
import importlib.util
from pathlib import Path

import pytest


def load_client():
    path = (Path(__file__).resolve().parents[2] /
            'tests/container/placement-preparation/fake_user.py')
    spec = importlib.util.spec_from_file_location('spec163_cleanup_client', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_request_failure_stops_the_native_user(monkeypatch):
    client = load_client()

    class User:
        stopped = False

        def begin_collaboration(self, *args, **kwargs):
            raise RuntimeError('request setup failed')

        def stop(self):
            self.stopped = True

    user = User()
    monkeypatch.setattr(client, 'ServiceUser', lambda: user)
    with pytest.raises(RuntimeError, match='request setup failed'):
        client.main()
    assert user.stopped


def test_success_stops_the_native_user_before_return(monkeypatch):
    client = load_client()
    events = []

    class User:
        def stop(self):
            events.append('stop')

    user = User()
    monkeypatch.setattr(client, 'ServiceUser', lambda: user)

    def request(actual_user):
        assert actual_user is user
        events.append('response')
        return 0

    monkeypatch.setattr(client, 'run_request', request)
    assert client.main() == 0
    assert events == ['response', 'stop']
