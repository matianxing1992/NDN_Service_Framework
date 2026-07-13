#!/bin/sh
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
python3 - "$repo" <<'PY'
import copy,importlib.util,sys
from pathlib import Path
root=Path(sys.argv[1]);path=root/'packaging/ndnsf-di-container/lib/gpu_compatibility.py'
spec=importlib.util.spec_from_file_location('gpu_compatibility',path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
base={
 'binaries':{x:True for x in ('nfd','nfdc','App_ServiceController','di-native-provider')},
 'imports':{x:True for x in ('ndnsf','ndnsf_distributed_inference','torch','transformers','onnxruntime')},
 'missingLibraries':[], 'driverVersion':'550.54.15', 'torchCudaAvailable':True,
 'torchCudaVersion':'12.4', 'torchCudnnMajor':9,
 'ortProviders':['CUDAExecutionProvider','CPUExecutionProvider'], 'ortCudaVersion':'12.4',
 'ortCudnnMajor':9, 'profileProviders':['CUDAExecutionProvider'],
 'allocatedGpuUuid':'GPU-a', 'torchGpuUuid':'GPU-a', 'ortGpuUuid':'GPU-a'}
assert m.evaluate_runtime_facts(base)['status']=='PASS'
cases=[
 ('cpu-only',lambda x:x.update(torchCudaAvailable=False),'FAIL_PYTORCH_CUDA_UNAVAILABLE'),
 ('missing-library',lambda x:x['missingLibraries'].append('libcudnn.so.9'),'FAIL_RUNTIME_LIBRARY_MISSING'),
 ('driver-too-old',lambda x:x.update(driverVersion='545.23.08'),'FAIL_DRIVER_TOO_OLD'),
 ('ort-fallback',lambda x:x.update(profileProviders=['CUDAExecutionProvider','CPUExecutionProvider']),'FAIL_ORT_CPU_FALLBACK'),
 ('pytorch-ort-mismatch',lambda x:x.update(ortCudaVersion='11.8'),'FAIL_PYTORCH_ORT_CUDA_MISMATCH')]
for name,mutate,code in cases:
 value=copy.deepcopy(base);mutate(value)
 try:m.evaluate_runtime_facts(value)
 except m.GpuCompatibilityError as e:
  assert str(e).startswith(code),(name,e,code)
 else:raise AssertionError(name+' unexpectedly passed')
print('RUNTIME_COMPATIBILITY_PASS cases=6')
PY
