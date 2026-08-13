"""Predictive high-level provider example.

The application must build and sign each complete NDN Data packet.  The
``make_app_signed_data`` helper below represents the application's normal
python-ndn/keychain integration and must return the exact wire bytes.
"""

from __future__ import annotations

from collections.abc import Callable, Iterable

from ndnsf import (SampleClassProfile, ServiceProvider, StreamConfig,
                   make_predictive_data_name)


def make_ndn_python_signer(provider: ServiceProvider):
    """Use python-ndn's keychain and the exact Provider signing key."""

    from ndn.client_conf import default_keychain, read_client_conf

    config = read_client_conf()
    keychain = default_keychain(config["pib"], config["tpm"])
    return keychain.get_signer({"key": provider.provider_signing_key_name})


def make_ndn_python_data(provider: ServiceProvider, name: str,
                         payload: bytes, *, freshness_ms: int = 300) -> bytes:
    """Create one exact-name Data wire with python-ndn."""

    from ndn.encoding import MetaInfo, make_data

    return bytes(make_data(
        name, MetaInfo(freshness_period=freshness_ms), bytes(payload),
        signer=make_ndn_python_signer(provider)))


def publish(
    provider: ServiceProvider,
    payloads: Iterable[bytes],
    make_app_signed_data: Callable[[str, bytes], bytes] | None = None,
) -> None:
    """Publish with an application signer; default uses python-ndn.

    ``make_app_signed_data`` remains injectable for applications with another
    keychain. The default demonstrates the supported python-ndn integration.
    """

    stream = provider.create_stream(StreamConfig(
        stream_id="binary-demo",
        data_prefix="/example/live/provider/samples",
        sample_period_ms=33.0,
        sample_classes=(SampleClassProfile("demo", 1, 4),),
    ))
    descriptor = stream.start()

    for sequence, payload in enumerate(payloads):
        name = make_predictive_data_name(
            descriptor.definition.mapping_root,
            descriptor.definition.mapping_version,
            sequence,
        )
        if make_app_signed_data is None:
            wire = make_ndn_python_data(provider, name, payload)
        else:
            wire = make_app_signed_data(name, payload)
        stream.push(wire)
        stream.flush()

    stream.stop()


# provider = ServiceProvider(...)
# publish(provider, camera_payloads, app_keychain_signer)
