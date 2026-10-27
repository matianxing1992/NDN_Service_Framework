from pathlib import Path

from setuptools import find_packages, setup
from setuptools.command.build_py import build_py as _build_py


class build_py(_build_py):
    def run(self):
        super().run()
        root = Path(__file__).resolve().parents[4]
        for relative in ("packaging/ndnsf-di-container/lib/allocation_topology.py",
                         "Experiments/TigerCluster/runtime/processes.py"):
            source = root / relative
            target = (Path(self.build_lib) /
                      "ndnsf_distributed_inference/ops" / source.name)
            self.copy_file(str(source), str(target))
        resources = Path(self.build_lib) / 'ndnsf_distributed_inference/ops/_container'
        for name in ('__init__.py', 'base.py', 'slurm_apptainer.py'):
            source = root / 'packaging/ndnsf-di-container/lib/adapters' / name
            target = resources / 'lib/adapters' / name
            target.parent.mkdir(parents=True, exist_ok=True)
            self.copy_file(str(source), str(target))
        target = resources / 'lib/allocation_topology.py'
        self.copy_file(str(root / 'packaging/ndnsf-di-container/lib/allocation_topology.py'), str(target))
        for name in ('run-allocation-topology.sh', 'configure-allocation-routes.sh',
                     'run-container.sh', 'stage-sif.sh', 'validate-local-sif-build-record.py'):
            relative = Path('adapters/slurm-apptainer/scripts') / name
            source = root / 'Experiments/TigerCluster' / relative
            target = resources / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            self.copy_file(str(source), str(target))


setup(
    name="ndnsf-di-ops", version="0.111.0", package_dir={"": "../../.."},
    packages=find_packages("../../..", include=["ndnsf_distributed_inference.ops*"]),
    py_modules=["ndnsf_di_launcher"],
    cmdclass={"build_py": build_py},
    install_requires=["python-ndn>=0.3"],
    extras_require={"app": ["ndnsf-di-app==0.111.0"]},
    entry_points={"console_scripts": ["ndnsf-di=ndnsf_di_launcher:main"]},
)
