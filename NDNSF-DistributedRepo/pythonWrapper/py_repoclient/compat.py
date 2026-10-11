"""Versioned errors for Python Repo behavior retired by Spec 192."""

from __future__ import annotations


class RepoCompatibilityError(RuntimeError):
    """A named Python-only Repo operation has no native Spec 192 equivalent."""

    code = "SPEC192_COMPATIBILITY_V1"

    def __init__(self, operation: str, replacement: str) -> None:
        self.operation = str(operation)
        self.replacement = str(replacement)
        super().__init__(
            f"{self.code}: {self.operation} is no longer supported; "
            f"{self.replacement}"
        )


def raise_repo_compatibility_error(operation: str, replacement: str) -> None:
    raise RepoCompatibilityError(operation, replacement)


class RetiredRepoFeature:
    """Import-compatible base for APIs whose implementation was Python-owned."""

    _repo_operation = "legacy Python Repo feature"
    _repo_replacement = "use the native ArtifactRepositoryApi"

    def __new__(cls, *args, **kwargs):
        raise_repo_compatibility_error(cls._repo_operation, cls._repo_replacement)


def retired_repo_feature(name: str, replacement: str):
    """Create an import-compatible name with a precise versioned error."""
    return type(
        name,
        (RetiredRepoFeature,),
        {
            "__module__": __name__,
            "_repo_operation": name,
            "_repo_replacement": replacement,
        },
    )
