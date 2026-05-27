# NDNSF Examples

This directory contains C++ examples for the current generic NDNSF runtime API.
The Python examples in `examples/python/` use `pythonWrapper/`: HELLO uses true
Python business logic through pybind11-backed `ServiceProvider` and
`ServiceUser`, while larger collaboration examples currently orchestrate the
existing C++ applications.

## Build

From the repository root:

```bash
python3 -m pip install -e ./pythonWrapper
./waf configure
./waf build
export NDNSF_BINARY_DIR=$PWD/build/examples
export NDNSF_LIBRARY_DIR=$PWD/build
```

The example binaries are generated under `build/examples/`.

## Run On One Real Machine

Start a local NFD, run the controller, then run the provider and user examples:

```bash
nfd-start
nfdc strategy set /example/hello/group /localhost/nfd/strategy/multicast

./build/examples/App_ServiceController --policy-file examples/hello.policies
./build/examples/App_Provider
./build/examples/App_User --ack-timeout-ms 300 --timeout-ms 5000
```

The process order matters: start the controller first, then providers, then
users. For collaboration examples, use the matching policy file:

```bash
./build/examples/App_ServiceController --policy-file examples/ai_collaboration.policies
./build/examples/AI_DistributedCollaborationProvider --role p00
./build/examples/AI_User --ack-timeout-ms 1000 --timeout-ms 12000
```

The Python wrappers can launch existing binaries for orchestration examples or
call the NDNSF runtime directly for HELLO-style Python business logic.
`--start-local-nfd` starts NFD only when one is not already running, and stops
only the instance started by the script:

```bash
python3 examples/python/hello_service.py --run --start-local-nfd
python3 examples/python/ai_distributed_collaboration.py --run --start-local-nfd
python3 examples/python/payment_collaboration.py --run --start-local-nfd
```

Use `--dry-run` on any Python example to print the underlying commands/actions.
For multi-node deployments, use role-specific scripts such as
`examples/python/hello_controller.py`, `examples/python/hello_provider.py`,
`examples/python/hello_user.py`, `examples/python/ai_controller.py`,
`examples/python/ai_provider.py`, `examples/python/ai_user.py`,
`examples/python/payment_controller.py`, `examples/python/payment_provider.py`,
and `examples/python/payment_user.py`. SVS latency helpers remain C++/experiment
tools rather than Python application examples.

## Run In MiniNDN

MiniNDN experiments should launch the needed C++ binaries or role-specific
Python scripts inside node shells. The existing experiment scripts under
`Experiments/` show this pattern for Memphis--UCLA tests and SVS latency tests.
A typical C++ flow is:

1. Build the repository before starting MiniNDN.
2. Start NFD/NLSR for the topology.
3. Start `App_ServiceController` on the controller node.
4. Start provider binaries on provider nodes.
5. Start the user binary on the user node.

For Python-managed examples inside MiniNDN, run the script from a node shell and
omit `--start-local-nfd`, because MiniNDN already manages NFD:

```bash
python3 examples/python/hello_service.py --run --no-configure-local-nfd
```

For a ready-to-run two-node smoke test of the Python business API:

```bash
sudo -n python3 Experiments/NDNSF_Python_Hello_Minindn.py
```

In custom multi-node MiniNDN experiments, prefer the dedicated Python experiment
harnesses in `Experiments/` so each process runs on the intended node.
