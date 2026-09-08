#!/usr/bin/env python3
"""Bounded same-model reference: real ORT execution, no NDNSF qualification."""
import argparse
import hashlib
import json
from pathlib import Path
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--backend', choices=('cpu', 'cuda'), required=True)
    parser.add_argument('--package', type=Path, required=True)
    parser.add_argument('--repository', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    import numpy as np
    import onnx
    import onnxruntime as ort
    from yolo_reference import load_reference, compare_reference

    args.output.mkdir(mode=0o700, parents=True, exist_ok=False)
    result = dict(status='FAIL', scope='STANDALONE_YOLO_REFERENCE', backend=args.backend)
    try:
        manifest = json.loads((args.package / 'manifest.json').read_text())
        graph = (args.package / 'canonical/yolo26n.onnx').read_bytes()
        weights_path = Path(manifest['weights']['path'])
        if weights_path.is_absolute() or '..' in weights_path.parts:
            raise ValueError('WEIGHTS_PATH')
        weights = (args.package / weights_path).read_bytes()
        digest = lambda value: 'sha256:' + hashlib.sha256(value).hexdigest()
        if (digest(graph) != manifest['graph']['graphDigest']
                or digest(weights) != manifest['weights']['digest']):
            raise ValueError('MODEL_DIGEST')
        reference = load_reference(args.package, args.repository, 640)
        model = onnx.load_model_from_string(graph)
        # Canonical transport carries one weights object. Resolve only checked
        # byte ranges from that object, never exporter filesystem paths.
        external_count = 0
        for tensor in model.graph.initializer:
            if tensor.data_location != onnx.TensorProto.EXTERNAL:
                continue
            external_count += 1
            fields = {entry.key: entry.value for entry in tensor.external_data}
            offset, length = int(fields.get('offset', '0')), int(fields['length'])
            if offset < 0 or length <= 0 or offset + length > len(weights):
                raise ValueError('INITIALIZER_RANGE')
            tensor.raw_data = weights[offset:offset + length]
            tensor.data_location = onnx.TensorProto.DEFAULT
            del tensor.external_data[:]
        if not external_count:
            raise ValueError('EXPECTED_EXTERNAL_WEIGHTS')
        onnx.checker.check_model(model)
        options = ort.SessionOptions()
        options.intra_op_num_threads = 1
        options.inter_op_num_threads = 1
        options.enable_profiling = True
        options.profile_file_prefix = str(args.output / 'ort')
        provider = 'CUDAExecutionProvider' if args.backend == 'cuda' else 'CPUExecutionProvider'
        if provider not in ort.get_available_providers():
            raise RuntimeError('BACKEND_UNAVAILABLE')
        if args.backend == 'cuda':
            options.add_session_config_entry('session.disable_cpu_ep_fallback', '1')
        providers = [(provider, {'use_tf32': '0'})] if args.backend == 'cuda' else [provider]
        session = ort.InferenceSession(model.SerializeToString(), sess_options=options,
                                       providers=providers)
        session.disable_fallback()
        timings, comparisons = [], []
        for phase in ('warmup', 'measured'):
            start = time.monotonic()
            values = session.run(['predictions'], {'images': reference.input_tensor})[0]
            timings.append(dict(phase=phase, seconds=time.monotonic() - start))
            comparisons.append(compare_reference(reference, values))
        profile = Path(session.end_profiling())
        events = json.loads(profile.read_text())
        kernels = [event.get('args', {}) for event in events
                   if event.get('cat') == 'Node' and event.get('args', {}).get('provider')]
        observed = sorted({event['provider'] for event in kernels})
        if not kernels or provider not in observed:
            raise RuntimeError('BACKEND_EXECUTION_NOT_OBSERVED')
        if args.backend == 'cuda' and any(event['provider'] != provider for event in kernels):
            raise RuntimeError('CPU_FALLBACK_OBSERVED')
        np.save(args.output / 'predictions.npy', values, allow_pickle=False)
        result.update(ortVersion=ort.__version__, providers=session.get_providers(),
                      providerOptions=session.get_provider_options(),
                      observedProviders=observed, graphDigest=digest(graph),
                      weightsDigest=digest(weights), timings=timings,
                      numerical=comparisons, profile=profile.name)
        if not all(value['matched'] for value in comparisons):
            raise RuntimeError('NUMERICAL_MISMATCH')
        result['status'] = 'PASS'
    except Exception as exc:
        result['error'] = type(exc).__name__ + ':' + str(exc)
    (args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result))
    return 0 if result['status'] == 'PASS' else 1


if __name__ == '__main__':
    raise SystemExit(main())
