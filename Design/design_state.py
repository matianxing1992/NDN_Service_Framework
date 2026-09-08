"""Shared, explicit source scope and build provenance for design tools."""
from pathlib import Path
import hashlib
import subprocess

ROOT = Path(__file__).resolve().parent.parent
API_ROOTS = {
    'Core': ('ndn-service-framework/', 'pythonWrapper/ndnsf/'),
    'Repo': ('NDNSF-DistributedRepo/include/', 'NDNSF-DistributedRepo/pythonWrapper/'),
    'DI': ('NDNSF-DistributedInference/cpp/', 'NDNSF-DistributedInference/ndnsf_distributed_inference/'),
    'UAV': ('NDNSF-UAV-APP/',),
}
SOURCE_ROOTS = ('ndn-service-framework/', 'pythonWrapper/', 'NDNSF-DistributedRepo/',
                'NDNSF-DistributedInference/', 'NDNSF-UAV-APP/')
EXCLUDED = {'tests', 'test', 'build', 'vendor', '__pycache__', 'results', 'models', '.git'}


def tracked_paths(root=ROOT):
    return [p for p in subprocess.check_output(
        ['git', 'ls-files', '-z'], cwd=root).decode().split('\0') if p]


def api_files(root=ROOT):
    return {p: module for p in tracked_paths(root)
            if not EXCLUDED.intersection(Path(p).parts)
            and Path(p).suffix in {'.hpp', '.h', '.py'}
            for module, prefixes in API_ROOTS.items() if p.startswith(prefixes)}


def source_files(root=ROOT):
    extensions = {'.cpp', '.cc', '.cxx', '.c', '.hpp', '.h', '.py', '.cmake',
                  '.json', '.yaml', '.yml', '.toml', '.proto', '.tlv'}
    return {p for p in tracked_paths(root) if p.startswith(SOURCE_ROOTS)
            and not EXCLUDED.intersection(Path(p).parts)
            and (Path(p).suffix in extensions or Path(p).name in {'CMakeLists.txt', 'wscript'})}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def build_inputs(design):
    # Include generators, contracts, frozen inventories and all TeX inputs.
    return {str(p.relative_to(design)): digest(p)
            for p in sorted(design.rglob('*')) if p.is_file()
            and p.suffix in {'.tex', '.json', '.py', '.cjs', '.patch', '.sty', '.cls', '.bib', '.png', '.jpg', '.svg'}
            and p.name != 'build-provenance.json' and '__pycache__' not in p.parts}


def verify_provenance(design, record):
    if record['inputs'] != build_inputs(design):
        raise ValueError('Document inputs changed after PDF build')
    for name, value in record['pdfs'].items():
        if digest(design / name) != value:
            raise ValueError('PDF does not match build: ' + name)
