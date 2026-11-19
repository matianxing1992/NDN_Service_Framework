"""Offline contract inspection must work without loading production bindings."""
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]


def test_offline_state_and_export_metadata_need_no_native_or_ml_runtime():
    code = '''
import sys
sys.path.insert(0, sys.argv[1])
sys.path.insert(0, sys.argv[2])
class BlockRuntime:
    def find_spec(self, fullname, path=None, target=None):
        if fullname == 'ndnsf' or fullname.startswith('ndnsf.') or fullname in ('torch', 'onnxruntime'):
            raise RuntimeError('unexpected runtime import: ' + fullname)
sys.meta_path.insert(0, BlockRuntime())
from ndnsf_distributed_inference.adapters.qwen.stateful_onnx import DecodeStateBundleV1
from ndnsf_distributed_inference.adapters.qwen import DecodeStateBundleV1 as public_alias
assert public_alias is DecodeStateBundleV1
from types import SimpleNamespace
from llm_pipeline.llm_pipeline_lib import _qwen_head_dim
assert _qwen_head_dim(SimpleNamespace(hidden_size=5120, num_attention_heads=24, head_dim=256)) == 256
assert 'ndnsf' not in sys.modules and 'onnxruntime' not in sys.modules
import ndnsf_distributed_inference.adapters as adapters
import ndnsf_distributed_inference.adapters.qwen as qwen
assert 'GraphAdapter' in adapters.__all__ and 'QwenThreeStageSplitter' in qwen.__all__
try:
    qwen.no_such_adapter
except AttributeError:
    pass
else:
    raise AssertionError('unknown export must fail')
'''
    result = subprocess.run([sys.executable, '-I', '-B', '-c', code,
                             str(ROOT / 'NDNSF-DistributedInference'),
                             str(ROOT / 'examples/python/NDNSF-DistributedInference')],
                            capture_output=True, text=True, timeout=15)
    assert result.returncode == 0, result.stderr
