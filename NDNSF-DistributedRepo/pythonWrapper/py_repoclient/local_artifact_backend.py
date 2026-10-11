"""Compatibility name for the retired Python-owned artifact store."""

from .compat import retired_repo_feature

FilesystemArtifactApiBackend = retired_repo_feature(
    "FilesystemArtifactApiBackend",
    "use ArtifactRepositoryApi.make_in_app(InAppRepoConfig)",
)

__all__ = ["FilesystemArtifactApiBackend"]
