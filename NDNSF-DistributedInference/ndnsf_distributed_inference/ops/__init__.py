"""Operations-owned command surface with deferred native runtime loading."""


def main(*args, **kwargs):
    from .cli import main as run
    return run(*args, **kwargs)

__all__ = ["main"]
