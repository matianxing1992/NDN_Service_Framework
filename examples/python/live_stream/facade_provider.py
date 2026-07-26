"""Predictive high-level provider example.

The application must build and sign each complete NDN Data packet.  The
``make_app_signed_data`` helper below represents the application's normal
python-ndn/keychain integration and must return the exact wire bytes.
"""

from collections.abc import Callable, Iterable

from ndnsf import SampleClassProfile, ServiceProvider, StreamConfig


def publish(
    provider: ServiceProvider,
    payloads: Iterable[bytes],
    make_app_signed_data: Callable[[str, bytes], bytes],
) -> None:
    stream = provider.create_stream(StreamConfig(
        stream_id="binary-demo",
        data_prefix="/example/live/provider/samples",
        sample_period_ms=33.0,
        sample_classes=(SampleClassProfile("demo", 1, 4),),
    ))
    descriptor = stream.start()

    for sequence, payload in enumerate(payloads):
        name = (
            f"{descriptor.definition.mapping_root}/v/"
            f"{descriptor.definition.mapping_version}/seq={sequence}"
        )
        stream.push(make_app_signed_data(name, payload))
        stream.flush()

    stream.stop()


# provider = ServiceProvider(...)
# publish(provider, camera_payloads, app_keychain_signer)
