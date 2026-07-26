from setuptools import find_packages, setup
setup(name="ndnsf-di-adapter-onnx", version="0.111.0", package_dir={"":"../../../.."}, packages=find_packages("../../../..", include=["ndnsf_distributed_inference.adapters.onnx*"]), install_requires=["ndnsf-di-sdk==0.111.0","ndnsf-di-app==0.111.0","ndnsf-di-planner==0.111.0"])
