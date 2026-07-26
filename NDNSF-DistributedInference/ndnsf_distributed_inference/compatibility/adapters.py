"""Bounded method-level adapters for migration to the Spec 116 API."""

from __future__ import annotations

import logging
import warnings


_LOG = logging.getLogger("ndnsf.di.compatibility")
_WARNED: set[str] = set()


def _warn_once(old: str, replacement: str) -> None:
    key = old + "->" + replacement
    if key not in _WARNED:
        warnings.warn(
            f"{old} is deprecated; use {replacement}",
            DeprecationWarning, stacklevel=3)
        _WARNED.add(key)
    _LOG.info("NDNSF_DI_COMPAT_CALL old=%s replacement=%s", old, replacement)


class LegacyClientAdapter:
    def __init__(self, client):
        self._client = client

    def submit(self, deployment, *, input, timeout=None, deadline=None, options=None):
        _warn_once("submit", "InferenceClient.request")
        return self._client.request(
            deployment, input=input, timeout=timeout, deadline=deadline,
            options=options)

    def distributed_inference(self, deployment, **arguments):
        _warn_once("distributed_inference", "InferenceClient.run")
        return self._client.run(deployment, **arguments)

    def infer(self, deployment, **arguments):
        _warn_once("infer", "InferenceClient.run")
        return self._client.run(deployment, **arguments)

    async def infer_async(self, deployment, **arguments):
        _warn_once("infer_async", "InferenceClient.run_async")
        return await self._client.run_async(deployment, **arguments)

    def discover(self, *, service, model=None, constraints=None):
        _warn_once("discover", "InferenceClient.deployments.discover")
        return self._client.deployments.discover(
            service=service, model=model, constraints=constraints)

    def deploy(self, definition):
        _warn_once("deploy", "InferenceClient.deploy")
        return self._client.deploy(definition)


class LegacyProviderAdapter:
    def __init__(self, provider):
        self._provider = provider

    def serve_service(self, service, runner, *, capabilities):
        _warn_once("serve_service", "InferenceProvider.serve")
        return self._provider.serve(service, runner, capabilities=capabilities)


class LegacyProviderLifecycleAdapter:
    """Compatibility view requiring an already authorized admin port."""

    def __init__(self, admin_port):
        self._admin = admin_port

    def stage(self, *args, **kwargs):
        _warn_once("provider.stage", "ProviderAdminPort.stage")
        return self._admin.stage(*args, **kwargs)

    def activate(self, *args, **kwargs):
        _warn_once("provider.activate", "ProviderAdminPort.activate")
        return self._admin.activate(*args, **kwargs)

    def drain(self, *args, **kwargs):
        _warn_once("provider.drain", "ProviderAdminPort.drain")
        return self._admin.drain(*args, **kwargs)

    def delete(self, *args, **kwargs):
        _warn_once("provider.delete", "ProviderAdminPort.delete")
        return self._admin.delete(*args, **kwargs)


__all__ = [
    "LegacyClientAdapter", "LegacyProviderAdapter",
    "LegacyProviderLifecycleAdapter",
]
