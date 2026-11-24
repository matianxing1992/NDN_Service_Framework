"""NDNSF-DI namespace; import the public api or a canonical owner module."""

from pkgutil import extend_path as _extend_path

__path__ = _extend_path(__path__, __name__)
__all__ = ()
