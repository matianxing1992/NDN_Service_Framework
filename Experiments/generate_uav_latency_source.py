#!/usr/bin/env python3
"""Generate and recover a codec-independent UAV video frame oracle.

The marker is deliberately large and monochrome so it survives the same H.264
encode/decode path used by the MiniNDN acceptance source.  It carries the
source-frame identity and acquisition timestamp inside the pixels; candidate
backend metadata therefore cannot validate itself.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import shutil
import struct
import zlib


ORACLE_VERSION = 1
MAGIC = 0x4E44
GRID_COLUMNS = 19
GRID_ROWS = 8
CELL_PIXELS = 12
ORIGIN_X = 32
ORIGIN_Y = 32
PAYLOAD_STRUCT = struct.Struct(">HBIQ")


def _marker_payload(source_frame_id: int, capture_origin_ns: int) -> bytes:
    if not 0 <= source_frame_id <= 0xFFFFFFFF:
        raise ValueError("source frame ID is outside uint32")
    if not 0 <= capture_origin_ns <= 0xFFFFFFFFFFFFFFFF:
        raise ValueError("capture origin is outside uint64")
    body = PAYLOAD_STRUCT.pack(
        MAGIC, ORACLE_VERSION, source_frame_id, capture_origin_ns)
    return body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)


def _payload_bits(payload: bytes) -> list[int]:
    return [
        (byte >> shift) & 1
        for byte in payload
        for shift in range(7, -1, -1)
    ]


def render_frame_rgb(width: int, height: int, source_frame_id: int,
                     capture_origin_ns: int) -> bytes:
    marker_width = (GRID_COLUMNS + 2) * CELL_PIXELS
    marker_height = (GRID_ROWS + 2) * CELL_PIXELS
    if width < ORIGIN_X + marker_width or height < ORIGIN_Y + marker_height:
        raise ValueError("frame is too small for the visual oracle")
    frame = bytearray([96] * (width * height * 3))
    bits = _payload_bits(_marker_payload(source_frame_id, capture_origin_ns))
    if len(bits) != GRID_COLUMNS * GRID_ROWS:
        raise AssertionError("oracle grid and payload size disagree")

    def fill_cell(column: int, row: int, value: int) -> None:
        start_x = ORIGIN_X + column * CELL_PIXELS
        start_y = ORIGIN_Y + row * CELL_PIXELS
        for y in range(start_y, start_y + CELL_PIXELS):
            offset = (y * width + start_x) * 3
            frame[offset:offset + CELL_PIXELS * 3] = bytes(
                [value] * (CELL_PIXELS * 3))

    for row in range(GRID_ROWS + 2):
        for column in range(GRID_COLUMNS + 2):
            if row in (0, GRID_ROWS + 1) or column in (0, GRID_COLUMNS + 1):
                fill_cell(column, row, 0)
    for index, bit in enumerate(bits):
        fill_cell(1 + index % GRID_COLUMNS, 1 + index // GRID_COLUMNS,
                  235 if bit else 20)
    return bytes(frame)


def decode_marker_rgb(frame: bytes, width: int, height: int) -> dict[str, int]:
    if len(frame) != width * height * 3:
        raise ValueError("RGB frame size does not match dimensions")
    bits: list[int] = []
    margin = CELL_PIXELS // 4
    for row in range(GRID_ROWS):
        for column in range(GRID_COLUMNS):
            start_x = ORIGIN_X + (column + 1) * CELL_PIXELS + margin
            start_y = ORIGIN_Y + (row + 1) * CELL_PIXELS + margin
            total = 0
            samples = 0
            for y in range(start_y, start_y + CELL_PIXELS - 2 * margin):
                for x in range(start_x, start_x + CELL_PIXELS - 2 * margin):
                    offset = (y * width + x) * 3
                    total += sum(frame[offset:offset + 3]) // 3
                    samples += 1
            bits.append(1 if total / samples >= 128 else 0)
    encoded = bytearray()
    for start in range(0, len(bits), 8):
        value = 0
        for bit in bits[start:start + 8]:
            value = (value << 1) | bit
        encoded.append(value)
    body, checksum = bytes(encoded[:-4]), bytes(encoded[-4:])
    if zlib.crc32(body) & 0xFFFFFFFF != struct.unpack(">I", checksum)[0]:
        raise ValueError("visual oracle checksum mismatch")
    magic, version, source_frame_id, capture_origin_ns = PAYLOAD_STRUCT.unpack(body)
    if magic != MAGIC or version != ORACLE_VERSION:
        raise ValueError("visual oracle magic/version mismatch")
    return {
        "oracleVersion": version,
        "sourceFrameId": source_frame_id,
        "captureOriginNs": capture_origin_ns,
    }


def generate_source(output: Path, *, frames: int, width: int, height: int,
                    fps: int, capture_origin_ns: int) -> dict[str, object]:
    if frames <= 0 or fps <= 0:
        raise ValueError("frames and fps must be positive")
    output.mkdir(parents=True, exist_ok=True)
    raw_path = output / "source.rgb"
    h264_path = output / "source.h264"
    decoded_path = output / "decoded.rgb"
    manifest_path = output / "manifest.json"
    frame_manifest = []
    with raw_path.open("wb") as stream:
        for frame_id in range(frames):
            timestamp = capture_origin_ns + round(frame_id * 1_000_000_000 / fps)
            stream.write(render_frame_rgb(width, height, frame_id, timestamp))
            frame_manifest.append({
                "sourceFrameId": frame_id,
                "captureOriginNs": timestamp,
            })
    manifest = {
        "oracleVersion": ORACLE_VERSION,
        "width": width,
        "height": height,
        "fps": fps,
        "frames": frame_manifest,
    }
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    ffmpeg = shutil.which("ffmpeg")
    if ffmpeg is None:
        raise RuntimeError("ffmpeg is required for the H.264 oracle gate")
    encode = [
        ffmpeg, "-hide_banner", "-loglevel", "error", "-y",
        "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", f"{width}x{height}",
        "-r", str(fps), "-i", str(raw_path), "-frames:v", str(frames),
        "-c:v", "libx264", "-preset", "ultrafast", "-tune", "zerolatency",
        "-bf", "0", "-g", str(fps), "-crf", "18", "-f", "h264",
        str(h264_path),
    ]
    decode = [
        ffmpeg, "-hide_banner", "-loglevel", "error", "-y",
        "-i", str(h264_path), "-frames:v", str(frames),
        "-f", "rawvideo", "-pix_fmt", "rgb24", str(decoded_path),
    ]
    return {
        "manifestPath": str(manifest_path),
        "rawRgbPath": str(raw_path),
        "h264Path": str(h264_path),
        "decodedRgbPath": str(decoded_path),
        "encodeCommand": encode,
        "decodeCommand": decode,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--frames", type=int, default=300)
    parser.add_argument("--width", type=int, default=640)
    parser.add_argument("--height", type=int, default=360)
    parser.add_argument("--fps", type=int, default=30)
    parser.add_argument("--capture-origin-ns", type=int, default=1_000_000_000)
    args = parser.parse_args()
    generated = generate_source(
        args.output, frames=args.frames, width=args.width, height=args.height,
        fps=args.fps, capture_origin_ns=args.capture_origin_ns)
    print(json.dumps(generated, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
