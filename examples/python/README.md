# Python NDNSF Examples

These scripts are application examples. They use the installable `ndnsf`
Python package.

The HELLO controller/provider/user scripts show the preferred Python API:
control logic and business logic are written in Python with
`ServiceController`, `ServiceProvider`, and `ServiceUser`, while the
`ndnsf._ndnsf` pybind11 extension calls the NDNSF C++ runtime. Larger
collaboration examples still use `NDNSFSession` to orchestrate existing C++
examples until collaboration-specific Python bindings are added.

## Available Examples

```text
hello_service.py
  Convenience launcher that starts ServiceController and Python HELLO
  provider/user logic on one machine.

hello_controller.py / hello_provider.py / hello_user.py
  Role-specific entry points for the HELLO example. The controller, provider,
  and user roles use the Python NDNSF API.

hello_rate_user.py
  Open-loop Python ServiceUser benchmark entry point using the asynchronous
  Python API.

ai_distributed_collaboration.py
  Convenience launcher that starts controller, all providers, and user on one
  machine for the 3-stage x 3-shard distributed FNN collaboration demo.

ai_controller.py / ai_provider.py / ai_user.py
  Role-specific entry points for the same distributed AI demo. Use these in
  MiniNDN or real multi-machine deployments.

payment_collaboration.py
  Convenience launcher for the online-payment collaboration demo with fraud,
  inventory, payment, and receipt roles.

payment_controller.py / payment_provider.py / payment_user.py
  Role-specific entry points for the payment collaboration example.

intermittent_reliability.py
  Convenience launcher for the intermittent provider reliability demo with
  three /HELLO providers.

intermittent_controller.py / intermittent_provider.py / intermittent_user.py
  Role-specific entry points for the intermittent-provider reliability example.
```

## Install And Build

From the repository root:

```bash
python3 -m pip install -e ./pythonWrapper
./waf configure
./waf build
export NDNSF_BINARY_DIR=$PWD/build/examples
export NDNSF_LIBRARY_DIR=$PWD/build
```

If the C++ binaries are installed into `$PATH`, `NDNSF_BINARY_DIR` is not
needed for orchestration examples. You can also pass `--binary-dir /path/to/bin`
to examples that launch existing C++ binaries.

## Dry Run

Preview the commands/actions without launching anything:

```bash
python3 examples/python/hello_service.py --dry-run
python3 examples/python/hello_controller.py --dry-run
python3 examples/python/hello_provider.py --dry-run
python3 examples/python/hello_user.py --dry-run
python3 examples/python/hello_user.py --list-services
python3 examples/python/hello_rate_user.py --dry-run
python3 examples/python/ai_distributed_collaboration.py --dry-run
python3 examples/python/ai_controller.py --dry-run
python3 examples/python/ai_provider.py --role p00 --dry-run
python3 examples/python/ai_user.py --dry-run
python3 examples/python/payment_collaboration.py --dry-run
python3 examples/python/payment_controller.py --dry-run
python3 examples/python/payment_provider.py --role fraud --dry-run
python3 examples/python/payment_user.py --dry-run
python3 examples/python/intermittent_reliability.py --dry-run
python3 examples/python/intermittent_controller.py --dry-run
python3 examples/python/intermittent_provider.py --provider-id A --dry-run
python3 examples/python/intermittent_user.py --dry-run
```

## Real-Machine Single-Host Run

If local NFD is not already running, the script can start it for this example
and stop only the NFD instance it started:

```bash
python3 examples/python/hello_service.py --run --start-local-nfd
python3 examples/python/ai_distributed_collaboration.py --run --start-local-nfd
python3 examples/python/payment_collaboration.py --run --start-local-nfd
```

If NFD is already running, omit `--start-local-nfd`. The flag is also guarded:
when the script detects an existing `nfd`, it leaves that NFD running and does
not stop it at exit.

```bash
nfdc strategy set /example/hello/group /localhost/nfd/strategy/multicast
python3 examples/python/hello_service.py --run
```

Logs are written under `results/python_*` by default. Use `--log-dir DIR` to
override this.

For multi-machine AI collaboration, start one role per machine or MiniNDN node:

```bash
python3 examples/python/ai_controller.py
python3 examples/python/ai_provider.py --role p00
python3 examples/python/ai_provider.py --role p01 --provider-id A
python3 examples/python/ai_provider.py --role p02 --provider-id B
python3 examples/python/ai_user.py
```

The same pattern applies to the other examples:

```bash
python3 examples/python/hello_controller.py
python3 examples/python/hello_provider.py
python3 examples/python/hello_user.py

python3 examples/python/payment_controller.py
python3 examples/python/payment_provider.py --role fraud
python3 examples/python/payment_provider.py --role inventory --provider-id A
python3 examples/python/payment_provider.py --role payment --provider-id B
python3 examples/python/payment_provider.py --role receipt --provider-id C
python3 examples/python/payment_user.py

python3 examples/python/intermittent_controller.py
python3 examples/python/intermittent_provider.py --provider-id A
python3 examples/python/intermittent_provider.py --provider-id B
python3 examples/python/intermittent_provider.py --provider-id C
python3 examples/python/intermittent_user.py
```

## MiniNDN Run

MiniNDN normally starts NFD per node, so do not use `--start-local-nfd` inside a
node shell. The checked-in MiniNDN smoke test places the Python controller and
user on Memphis and the Python provider on UCLA:

```bash
sudo -n python3 Experiments/NDNSF_Python_Hello_Minindn.py
```

The Python open-loop Memphis-to-UCLA performance test is:

```bash
sudo -n python3 Experiments/NDNSF_Python_Minindn_Perf.py \
  --rate-series 10,30,50,70,100,150 --duration 30
```

For a single-node smoke test:

```bash
python3 examples/python/hello_service.py --run --no-configure-local-nfd
```

For custom multi-node experiments, run the role-specific Python scripts from
the MiniNDN experiment harness so controller, provider, and user runtimes are
placed on the correct nodes. The Python scripts remain useful as command/action
templates via `--dry-run`.

## Python API Sketch

Application code imports reusable wrapper objects from the installed `ndnsf`
package:

```python
from ndnsf import ServiceController, ServiceProvider

controller = ServiceController(
    policy_file="examples/hello.policies",
    bootstrap_identities=[
        "/example/hello/provider",
        "/example/hello/user",
    ])
provider = ServiceProvider()

@provider.handler("/HELLO")
def hello(request: bytes) -> bytes:
    return b"HELLO"

provider.run()
```

```python
from ndnsf import ServiceUser

user = ServiceUser()
for entry in user.get_allowed_services():
    print(entry.service, entry.provider_service)

response = user.request_service("/HELLO", b"HELLO")
print(response.payload)
```

`get_allowed_services()` uses the user's current permission table after
ServiceController permission discovery. It is the Python API for asking what
services this user is authorized to invoke.

For experiment orchestration, `NDNSFSession` can still launch existing C++ apps:

```python
from ndnsf import ControllerConfig, NDNSFSession, ProviderConfig, UserConfig

with NDNSFSession(binary_dir="/usr/local/bin", log_dir="results/demo") as ndnsf:
    ndnsf.start_controller(
        ControllerConfig(policy_file="examples/hello.policies"))
    ndnsf.start_provider(
        ProviderConfig(name="provider", binary="App_Provider"))
    result = ndnsf.run_user(
        UserConfig(name="user", binary="App_User"))
```

Service payloads, collaboration roles, and benchmark parameters belong in
examples or application code.
