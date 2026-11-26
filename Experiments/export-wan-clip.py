#!/usr/bin/env python3
"""Package a completed native Wan RGB24 output on the experiment host."""

import argparse
from fractions import Fraction
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def file_digest(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            digest.update(block)
    return 'sha256:' + digest.hexdigest()


def require_file(path, expected_digest, expected_size=None):
    if path.is_symlink() or not path.is_file():
        raise ValueError(f'missing regular file {path}')
    size = path.stat().st_size
    if (expected_size is not None and size != expected_size) or file_digest(path) != expected_digest:
        raise ValueError(f'file differs from the native manifest: {path}')
    return size


def check_trace(clip, manifest):
    entries = manifest['fixture_trace']
    if not entries:
        raise ValueError('native manifest contains no trace entries')
    size = 0
    for index, entry in enumerate(entries):
        name = f'trace-{index}.bundle'
        if entry['file'] != name:
            raise ValueError(f'unexpected native trace filename at index {index}')
        size += require_file(clip / name, entry['digest'])
    if size != manifest['fixture_trace_bytes']:
        raise ValueError('native trace byte count differs from the manifest')
    return {'files': len(entries), 'bytes': size}


def check_mp4(probe, width, height, count, fps):
    stream, = probe['streams']
    time_base = Fraction(stream['time_base'])
    if (stream['codec_name'], stream['pix_fmt'], stream['width'], stream['height']) != (
            'h264', 'yuv420p', width, height):
        raise ValueError('MP4 codec, pixel format or dimensions differ from the export settings')
    if (int(stream['nb_read_frames']) != count or len(probe['frames']) != count or
            Fraction(stream['avg_frame_rate']) != fps or
            int(stream['duration_ts']) * time_base != count / fps):
        raise ValueError('MP4 frame count, rate or duration differs from the native manifest')
    for index, row in enumerate(probe['frames']):
        if int(row['best_effort_timestamp']) * time_base != index / fps:
            raise ValueError(f'MP4 presentation timestamp differs at frame {index}')


def package(clip, output, ffmpeg, ffprobe, env, verify_trace):
    manifest_path = clip / 'manifest.json'
    if manifest_path.is_symlink() or not manifest_path.is_file():
        raise ValueError('native manifest must be a regular file')
    wire = manifest_path.read_bytes()
    manifest = json.loads(wire)
    if (manifest['schema'], manifest['format'], manifest['file'], manifest['first_pts']) != (
            'ndnsf-di-wan-rgb24-v1', 'rgb24', 'frames.rgb24', 0):
        raise ValueError('unsupported native Wan clip format')
    count, width, height = (manifest[key] for key in ('frames', 'width', 'height'))
    if not all(type(value) is int and value > 0 for value in (count, width, height)):
        raise ValueError('invalid native frame geometry')
    if width % 2 or height % 2:
        raise ValueError('YUV420 export requires even frame dimensions')
    time_numerator, time_denominator = manifest['time_base']
    ticks = manifest['ticks_per_frame']
    if not all(type(value) is int and value > 0 for value in (time_numerator, time_denominator, ticks)):
        raise ValueError('invalid native frame time base')
    fps = Fraction(time_denominator, time_numerator * ticks)
    if manifest['bytes'] != count * width * height * 3:
        raise ValueError('invalid native frame rate or byte count')
    raw = clip / manifest['file']
    require_file(raw, manifest['digest'], manifest['bytes'])
    trace = check_trace(clip, manifest) if verify_trace else None
    sample_count = min(count, 5)
    samples = [index * (count - 1) // max(1, sample_count - 1) for index in range(sample_count)]
    select = '+'.join(f'eq(n\\,{index})' for index in samples)
    read_raw = [ffmpeg, '-nostdin', '-hide_banner', '-loglevel', 'error', '-n',
                '-f', 'rawvideo', '-pixel_format', 'rgb24', '-video_size', f'{width}x{height}',
                '-framerate', str(fps), '-i', str(raw)]
    with tempfile.TemporaryDirectory(prefix='.wan-export-', dir=output.parent) as temporary:
        staging = Path(temporary)
        encode = [*read_raw, '-frames:v', str(count), '-an', '-c:v', 'libx264',
                  '-threads', '2', '-preset', 'fast', '-crf', '18', '-pix_fmt', 'yuv420p',
                  '-movflags', '+faststart', 'clip.mp4']
        sample = [*read_raw, '-vf',
                  f'select={select},scale={min(width, 320)}:-2,tile={sample_count}x1',
                  '-frames:v', '1', 'frame-samples.png']
        subprocess.run(encode, cwd=staging, env=env, check=True)
        subprocess.run(sample, cwd=staging, env=env, check=True)
        probe_command = [ffprobe, '-v', 'error', '-count_frames', '-show_streams',
                         '-show_frames', '-select_streams', 'v:0', '-of', 'json', 'clip.mp4']
        probe_wire = subprocess.check_output(probe_command, cwd=staging, env=env)
        check_mp4(json.loads(probe_wire), width, height, count, fps)
        require_file(raw, manifest['digest'], manifest['bytes'])
        if manifest_path.read_bytes() != wire:
            raise ValueError('native manifest changed during export')
        (staging / 'ffprobe.json').write_bytes(probe_wire)
        receipt = {
            'schema': 'ndnsf-wan-clip-presentation-v1',
            'source_manifest': str(manifest_path),
            'manifest_sha256': 'sha256:' + hashlib.sha256(wire).hexdigest(),
            'rgb24_sha256': manifest['digest'],
            'verified_trace': trace,
            'frames': count, 'width': width, 'height': height,
            'fps': str(fps), 'duration_seconds': str(count / fps), 'sample_indices': samples,
            'outputs': {name: file_digest(staging / name)
                        for name in ('clip.mp4', 'frame-samples.png', 'ffprobe.json')},
            'commands': [encode, sample, probe_command],
            'tools': {tool: subprocess.check_output([tool, '-version'], env=env, text=True)
                      .splitlines()[0] for tool in (ffmpeg, ffprobe)},
        }
        (staging / 'presentation.json').write_text(json.dumps(receipt, indent=2) + '\n')
        output.mkdir(mode=0o700)
        for name in ('clip.mp4', 'frame-samples.png', 'ffprobe.json', 'presentation.json'):
            (staging / name).rename(output / name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('clip', type=Path)
    parser.add_argument('output', type=Path, help='new directory for the MP4, images and receipts')
    parser.add_argument('--verify-trace', action='store_true', help='verify retained native trace files')
    parser.add_argument('--ffmpeg', default='ffmpeg')
    parser.add_argument('--ffprobe', default='ffprobe')
    parser.add_argument('--tool-library-dir', type=Path)
    args = parser.parse_args()
    try:
        tools = [shutil.which(tool) for tool in (args.ffmpeg, args.ffprobe)]
        if None in tools:
            raise ValueError('FFmpeg and ffprobe executables are required on the host')
        output = args.output.absolute()
        if output.exists() or output.is_symlink():
            raise ValueError(f'output already exists: {output}')
        env = os.environ.copy()
        if args.tool_library_dir:
            env['LD_LIBRARY_PATH'] = str(args.tool_library_dir.resolve(strict=True))
        package(args.clip.resolve(strict=True), output, *(str(Path(tool).resolve()) for tool in tools),
                env, args.verify_trace)
    except (OSError, ValueError, KeyError, TypeError, ArithmeticError, subprocess.SubprocessError) as error:
        parser.exit(1, f'Wan clip export failed: {error}\n')


if __name__ == '__main__':
    main()
