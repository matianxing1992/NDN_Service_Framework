#!/usr/bin/env python3
"""Publish opaque bytes under semantic names through the generic LiveStream API."""

import argparse
import json
import os
import time
from pathlib import Path

from ndnsf import LiveStreamDefinition, LiveStreamFecOptions, ServiceProvider


def write_descriptor(path: Path, descriptor) -> None:
    """Publish one complete immutable snapshot without a partial-file race."""
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(descriptor.to_dict(), sort_keys=True),
                         encoding="utf-8")
    os.replace(temporary, path)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--descriptor", required=True)
    parser.add_argument("--latest-descriptor")
    parser.add_argument("--count", type=int, default=12)
    parser.add_argument("--period-ms", type=int, default=50)
    parser.add_argument("--fec", action="store_true")
    args = parser.parse_args()
    if args.fec and args.count % 3 != 0:
        parser.error("--fec requires --count to be divisible by three")

    provider_name = "/example/live/provider"
    provider = ServiceProvider(
        group="/example/live/group",
        controller="/example/live/controller",
        provider_prefix=provider_name,
        trust_schema="examples/trust-schema.conf",
        serve_certificates=True,
    )
    # The dummy handler only starts the shared Face event loop; LiveStream has
    # its own Core-owned Mapping and semantic Data routes.
    provider.add_handler("/LiveStream/Dummy", lambda payload: payload)
    provider.start_background()

    definition = LiveStreamDefinition(
        stream_id="binary-demo",
        provider=provider_name,
        semantic_data_prefix="/example/live/provider/samples/v=1",
        session_epoch=1,
        mapping_version=1,
        mapping_block_capacity=4,
        mapping_ahead_blocks=3,
        retained_items=64,
        max_name_reservations=256,
        fec=(LiveStreamFecOptions.xor_one_repair(3, 4096, 500)
             if args.fec else LiveStreamFecOptions.none()),
    )
    publisher = provider.create_live_stream(definition)
    descriptor_path = Path(args.descriptor)
    latest_descriptor_path = Path(args.latest_descriptor or args.descriptor)

    publication_units = []
    if args.fec:
        for first in range(0, args.count, 3):
            source_names = [
                f"{definition.semantic_data_prefix}/sample/seq={index}"
                for index in range(first, first + 3)
            ]
            repair_names = [
                f"{definition.semantic_data_prefix}/fec/group/seq={first // 3}"
                "/repair/seg=0"
            ]
            group = publisher.reserve_group(
                f"group-{first // 3}", source_names, repair_names)
            opaque = [bytes([index & 0xff, 0, 0xa5, 0xff])
                      for index in range(first, first + 3)]
            publication_units.append((group.sources[0].cursor, group, opaque))
    else:
        names = [f"{definition.semantic_data_prefix}/sample/seq={index}"
                 for index in range(args.count)]
        reservations = publisher.reserve_many_ahead(names)
        publication_units = [
            (reservation.cursor, reservation,
             b"\x00first\xff" if reservation.cursor == 0 else
             bytes([reservation.cursor & 0xff, 0, 0xa5, 0xff]))
            for reservation in reservations
        ]

    first_cursor, first_reservation, first_payload = publication_units[0]
    if args.fec:
        publisher.publish_group(first_reservation, first_payload)
    else:
        publisher.publish(first_reservation, first_payload)

    deadline = time.monotonic() + 5
    descriptor = None
    while descriptor is None and time.monotonic() < deadline:
        try:
            descriptor = publisher.activate(
                measured_sample_period_ms=float(args.period_ms), safe_join_cursor=0)
        except RuntimeError:
            time.sleep(0.02)
    if descriptor is None:
        raise RuntimeError("LiveStream routes did not become ready")
    write_descriptor(descriptor_path, descriptor)
    write_descriptor(latest_descriptor_path, descriptor)

    for cursor, reservation, payload in publication_units[1:]:
        # One FEC publication unit contains three source samples. Preserve the
        # advertised source-sample period instead of collapsing the live
        # timeline merely because the packets are committed atomically.
        samples_in_unit = len(reservation.sources) if args.fec else 1
        time.sleep((args.period_ms * samples_in_unit) / 1000.0)
        if args.fec:
            publisher.publish_group(reservation, payload)
        else:
            publisher.publish(reservation, payload)
        refreshed = publisher.activate(
            measured_sample_period_ms=float(args.period_ms), safe_join_cursor=cursor)
        write_descriptor(latest_descriptor_path, refreshed)
    # Keep both Mapping and payload routes available while a newly started
    # consumer initializes its keychain and enters the live fetch loop.
    time.sleep(5)
    publisher.stop()
    provider.stop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
