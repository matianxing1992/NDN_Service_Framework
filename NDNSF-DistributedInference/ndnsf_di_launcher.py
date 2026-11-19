"""Start the installed management CLI without legacy user-site packages."""
import os
import sys
from importlib.machinery import PathFinder
from importlib.util import module_from_spec
from pathlib import Path


def _bind_package_root():
    # The Ops wheel is a namespace portion. A regular legacy parent package in
    # a system site can otherwise override it, even when this wheel's venv is
    # first on sys.path. Bind only this wheel's package root; system facilities
    # such as Mininet remain importable without borrowing another DI owner.
    name = 'ndnsf_distributed_inference'
    base = Path(__file__).resolve().parent
    expected = base / name
    if not expected.is_dir() or expected.is_symlink():
        raise RuntimeError('OPS_PACKAGE_ROOT_INVALID: ' + str(expected))
    existing = sys.modules.get(name)
    if existing is not None:
        if [Path(path).resolve() for path in getattr(existing, '__path__', ())] != [expected]:
            raise RuntimeError('OPS_NAMESPACE_LOADED_FROM_ANOTHER_PREFIX')
    else:
        spec = PathFinder.find_spec(name, [str(base)])
        if spec is None:
            raise RuntimeError('OPS_PACKAGE_ROOT_MISSING: ' + str(expected))
        package = module_from_spec(spec)
        sys.modules[name] = package
        if spec.loader is not None:
            spec.loader.exec_module(package)
        package.__path__ = [str(expected)]


def main():
    # Replace this process before importing DI. Keep the selected interpreter
    # and arguments, including inside a container or virtual environment.
    if not sys.flags.isolated:
        os.execv(sys.executable, [sys.executable, '-I', sys.argv[0], *sys.argv[1:]])
    _bind_package_root()
    from ndnsf_distributed_inference.ops.cli import main as run
    return run()
