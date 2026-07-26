#!/usr/bin/env python3
"""Predictive stream provider example (Spec 148 Mapping=Payload)."""

from ndnsf.streaming import StreamPublisher
import sys

def main():
    print("PREDICTIVE_PROVIDER ready")
    print("Usage: integrate with ServiceProvider.create_stream(config)")
    print("")
    print("Example flow:")
    print("  config = StreamConfig(")
    print("    stream_id='video',")
    print("    data_prefix='/example/video',")
    print("    sample_period_ms=40.0,")
    print("    fec=LiveStreamFecOptions.xor_one_repair(10, 8000),")
    print("  )")
    print("  stream = provider.create_stream(config)")
    print("  descriptor = stream.start()")
    print("")
    print("  for frame in frames:")
    print("    for chunk in segment_for_mapping(frame, 8000):")
    print("      data = keychain.sign(Data(name=..., content=chunk))")
    print("      stream.push(data)")
    print("    stream.flush()")
    print("")
    print("  stream.stop()")

if __name__ == "__main__":
    main()
