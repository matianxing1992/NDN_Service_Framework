"""Python-facing NDNSF service API backed by a pybind11 extension.

Python application code defines request handlers and issues service requests in
Python. The NDNSF runtime itself stays in C++ through ``ndnsf._ndnsf``: Face,
SVS, NAC-ABE, signing, token checks, and worker threads are managed by the
framework rather than by Python.
"""

from __future__ import annotations

from dataclasses import dataclass
import threading
from typing import Callable, Optional

from . import _ndnsf


@dataclass(frozen=True)
class ServiceResponse:
    status: bool
    payload: bytes = b""
    error: str = ""


@dataclass(frozen=True)
class AckDecision:
    status: bool = True
    payload: bytes = b""
    message: str = "ok"
    suppress: bool = False


@dataclass(frozen=True)
class AllowedService:
    """A service permission entry visible to a Python NDNSF user.

    provider_service is the full permission namespace, typically
    /<provider>/<service>. service is the unified service name applications pass
    to request_service(), such as /HELLO. token is retained only for legacy
    compatibility; current dynamic-runtime permissions normally leave it empty.
    """

    provider_service: str
    service: str
    token: str = ""


def _to_native_response(response: ServiceResponse) -> _ndnsf.ServiceResponse:
    native = _ndnsf.ServiceResponse()
    native.status = response.status
    native.payload = response.payload
    native.error = response.error
    return native


def _from_native_response(response: _ndnsf.ServiceResponse) -> ServiceResponse:
    return ServiceResponse(
        status=bool(response.status),
        payload=bytes(response.payload),
        error=str(response.error),
    )


def _to_native_ack(decision: AckDecision) -> _ndnsf.AckDecision:
    native = _ndnsf.AckDecision()
    native.status = decision.status
    native.payload = decision.payload
    native.message = decision.message
    native.suppress = decision.suppress
    return native


class ServiceProvider:
    """Python API for writing NDNSF provider business logic."""

    def __init__(
        self,
        *,
        provider_id: str = "",
        group: str = "/example/hello/group",
        controller: str = "/example/hello/controller",
        provider_prefix: str = "/example/hello/provider",
        trust_schema: str = "examples/trust-schema.conf",
        handler_threads: int = 4,
        ack_threads: int = 2,
        serve_certificates: bool = True,
        binary: str = "",
        binary_dir=None,
        library_dirs=None,
        cwd=None,
        env=None,
    ) -> None:
        # The last five parameters are accepted for source compatibility with
        # the previous subprocess bridge. pybind11 uses the loaded extension
        # module, not a separate host binary.
        del binary, binary_dir, library_dirs, cwd, env
        self._native = _ndnsf.NativeServiceProvider(
            provider_id=provider_id,
            group=group,
            controller=controller,
            provider_prefix=provider_prefix,
            trust_schema=trust_schema,
            handler_threads=handler_threads,
            ack_threads=ack_threads,
            serve_certificates=serve_certificates,
        )
        self._handlers: dict[str, Callable[[bytes], bytes | ServiceResponse]] = {}
        self._ack_handlers: dict[str, Callable[[bytes], bool | AckDecision]] = {}

    def add_handler(
        self,
        service: str,
        handler: Callable[[bytes], bytes | ServiceResponse],
    ) -> None:
        self._handlers[service] = handler

    def handler(self, service: str):
        def decorator(fn: Callable[[bytes], bytes | ServiceResponse]):
            self.add_handler(service, fn)
            return fn
        return decorator

    def set_ack_handler(
        self,
        service: str,
        handler: Callable[[bytes], bool | AckDecision],
    ) -> None:
        self._ack_handlers[service] = handler

    def ack_handler(self, service: str):
        def decorator(fn: Callable[[bytes], bool | AckDecision]):
            self.set_ack_handler(service, fn)
            return fn
        return decorator

    def _register_service(self, service: str) -> None:
        if service not in self._handlers:
            raise ValueError(f"no handler registered for {service}")

        def request_handler(payload: bytes):
            result = self._handlers[service](payload)
            if isinstance(result, ServiceResponse):
                return _to_native_response(result)
            return bytes(result)

        ack_handler = None
        if service in self._ack_handlers:
            def ack_handler(payload: bytes):
                result = self._ack_handlers[service](payload)
                if isinstance(result, AckDecision):
                    return _to_native_ack(result)
                return bool(result)

        self._native.add_service(service, request_handler, ack_handler)

    def run(self, service: Optional[str] = None) -> int:
        if service is None:
            if len(self._handlers) != 1:
                raise ValueError("service must be specified when multiple handlers are registered")
            service = next(iter(self._handlers))
        self._register_service(service)
        self._native.run()
        return 0

    def start_background(self, service: Optional[str] = None) -> threading.Thread:
        thread = threading.Thread(target=self.run, args=(service,), daemon=True)
        thread.start()
        return thread

    def stop(self) -> int:
        self._native.stop()
        return 0


class ServiceController:
    """Python API for running the NDNSF ServiceController role."""

    def __init__(
        self,
        *,
        controller_prefix: str = "/example/hello/controller",
        policy_file: str = "examples/hello.policies",
        trust_schema: str = "examples/trust-schema.conf",
        bootstrap_identities: Optional[list[str]] = None,
        serve_certificates: bool = True,
        binary: str = "",
        binary_dir=None,
        library_dirs=None,
        cwd=None,
        env=None,
    ) -> None:
        del binary, binary_dir, library_dirs, cwd, env
        self._native = _ndnsf.NativeServiceController(
            controller_prefix=controller_prefix,
            policy_file=policy_file,
            trust_schema=trust_schema,
            bootstrap_identities=list(bootstrap_identities or []),
            serve_certificates=serve_certificates,
        )

    def start(self) -> None:
        self._native.start()

    def run(self) -> int:
        self._native.run()
        return 0

    def stop(self) -> int:
        self._native.stop()
        return 0

    def start_background(self) -> threading.Thread:
        thread = threading.Thread(target=self.run, daemon=True)
        thread.start()
        return thread


class ServiceUser:
    """Python API for issuing NDNSF service requests."""

    def __init__(
        self,
        *,
        group: str = "/example/hello/group",
        controller: str = "/example/hello/controller",
        user: str = "/example/hello/user",
        trust_schema: str = "examples/trust-schema.conf",
        permission_wait_ms: int = 1500,
        handler_threads: int = 2,
        ack_threads: int = 2,
        adaptive_admission: bool = False,
        serve_certificates: bool = True,
        binary: str = "",
        binary_dir=None,
        library_dirs=None,
        cwd=None,
        env=None,
    ) -> None:
        del binary, binary_dir, library_dirs, cwd, env
        self._native = _ndnsf.NativeServiceUser(
            group=group,
            controller=controller,
            user=user,
            trust_schema=trust_schema,
            permission_wait_ms=permission_wait_ms,
            handler_threads=handler_threads,
            ack_threads=ack_threads,
            adaptive_admission=adaptive_admission,
            serve_certificates=serve_certificates,
        )

    def request_service(
        self,
        service: str,
        payload: bytes,
        *,
        ack_timeout_ms: int = 300,
        timeout_ms: int = 5000,
        strategy: str = "first-responding",
    ) -> ServiceResponse:
        response = self._native.request_service(
            service,
            bytes(payload),
            ack_timeout_ms=ack_timeout_ms,
            timeout_ms=timeout_ms,
            strategy=strategy,
        )
        return _from_native_response(response)

    def request_service_async(
        self,
        service: str,
        payload: bytes,
        *,
        on_response: Callable[[ServiceResponse], None],
        on_timeout: Callable[[str], None],
        ack_timeout_ms: int = 300,
        timeout_ms: int = 5000,
        strategy: str = "first-responding",
    ) -> None:
        """Submit a request and return immediately.

        The C++ runtime owns Face/SVS/NAC-ABE processing in a background event
        loop. Python only receives final response or timeout callbacks.
        """

        self._native.request_service_async(
            service,
            bytes(payload),
            lambda response: on_response(_from_native_response(response)),
            on_timeout,
            ack_timeout_ms=ack_timeout_ms,
            timeout_ms=timeout_ms,
            strategy=strategy,
        )

    def start(self) -> None:
        """Start the user's background Face event loop for async requests."""

        self._native.start()

    def stop(self) -> None:
        """Stop the user's background Face event loop."""

        self._native.stop()

    def get_allowed_services(self) -> list[AllowedService]:
        """Return the current permission snapshot fetched from ServiceController."""

        return [
            AllowedService(
                provider_service=str(provider_service),
                service=str(service),
                token=str(token),
            )
            for provider_service, service, token in self._native.get_allowed_services()
        ]

    def pump(self, milliseconds: int) -> None:
        self._native.pump(milliseconds)
