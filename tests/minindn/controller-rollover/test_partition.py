import pytest

from partition_active import check_status_blackout, configure, event_name, timestamp_ns, watcher


def test_original_provider_event_must_bind_attempt():
    binding = {"requestId": "/NDNSF/DI/REQUEST/nonce-1", "attemptEpoch": "2"}
    name = "/producer/NDNSF/event/service/s/request/nonce-1/attempt/2/stream/abc/rep/bytes/seq=4"
    event = "1795469727.726058 DEBUG: [nfd.Forwarder] onDataUnsolicited in=264 data=" + name + " decision=drop"
    assert event_name(event, binding) == name
    assert event_name("<D " + name, binding) == name
    assert event_name(event.replace("/attempt/2", "/attempt/3"), binding) is None
    assert event_name(event.replace("/request/nonce-1", "/request/another"), binding) is None
    assert event_name(event.replace("/NDNSF/event", "/NDNSF/status"), binding) is None
    assert timestamp_ns(event) == 1795469727726058000


def test_native_reader_barrier_isolated_from_transport_deadline(tmp_path):
    cfg = {"network": {"service": "/app"}, "actors": [{"kind": "requester", "identity": "/u"}],
           "requester": {"request": {}}, "security": {}}
    controller = {"parameters": {}}
    configure(cfg, tmp_path, controller)
    assert controller["parameters"] == {"revoke-identity": "/u", "revoke-attribute": "/PERMISSION/app"}
    assert cfg["requester"]["test_only"]
    retries = cfg["requester"]["request"]
    assert retries["interest_lifetime_ms"] * retries["max_event_retries"] > cfg["security"]["controller_policy_lease_ms"]


def test_log_watcher_retains_partial_append(tmp_path):
    path = tmp_path / "observed.log"
    poll = watcher(path, b"packet")
    assert not poll()
    path.write_text("irrelevant\npacket")
    assert not poll()
    with path.open("a") as stream:
        stream.write(" committed\n")
    assert poll() == ["packet committed\n"]
    assert not poll()


def test_status_callback_for_already_received_data_during_cut():
    versions = [(80, "1:1"), (105, "1:1")]
    assert check_status_blackout(versions, [95, 550], "1:1", 100, 110, 500, 90) == 95
    with pytest.raises(RuntimeError, match="new target lease"):
        check_status_blackout(versions, [95], "1:1", 100, 110, 500, 101)
    with pytest.raises(RuntimeError, match="new target lease"):
        check_status_blackout(versions + [(300, "1:2")], [95], "1:1", 100, 110, 500, 90)
    with pytest.raises(RuntimeError, match="policy Data reached"):
        check_status_blackout(versions, [95, 300], "1:1", 100, 110, 500, 90)
