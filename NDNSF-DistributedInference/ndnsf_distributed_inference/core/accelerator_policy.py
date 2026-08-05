"""Explicit CPU/GPU visibility policy for Spec 170.

The Provider may restrict the scheduler-visible device set, but it may not
invent a GPU or silently switch a GPU-required role to CPU.
"""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import Iterable


class AcceleratorMode(str, Enum):
    AUTO = "AUTO"
    NONE = "NONE"
    EXPLICIT_SUBSET = "EXPLICIT_SUBSET"


@dataclass(frozen=True)
class AcceleratorPolicy:
    mode: AcceleratorMode | str = AcceleratorMode.AUTO
    requested: tuple[str, ...] = ()
    allow_cpu: bool = True
    require_gpu: bool = False

    def __post_init__(self) -> None:
        object.__setattr__(self, "mode", AcceleratorMode(self.mode))
        object.__setattr__(self, "requested", tuple(str(x) for x in self.requested))
        if len(set(self.requested)) != len(self.requested):
            raise ValueError("accelerator subset contains duplicate devices")
        if any(not x.startswith("cuda:") or not x[5:].isdigit()
               for x in self.requested):
            raise ValueError("accelerator subset must use cuda:<index>")
        if self.mode == AcceleratorMode.NONE and self.require_gpu:
            raise ValueError("NONE cannot satisfy a GPU-required role")

    def resolve(self, visible: Iterable[str]) -> tuple[str, ...]:
        devices = tuple(str(x) for x in visible)
        if len(set(devices)) != len(devices) or any(not x for x in devices):
            raise ValueError("runtime-visible device identity is malformed")
        gpus = tuple(x for x in devices if x.startswith("cuda:"))
        if self.mode == AcceleratorMode.NONE:
            selected = ("cpu",)
        elif self.mode == AcceleratorMode.EXPLICIT_SUBSET:
            missing = set(self.requested) - set(gpus)
            if missing:
                raise RuntimeError(
                    "requested accelerator subset is not runtime-visible: "
                    + ",".join(sorted(missing)))
            selected = self.requested
        else:
            selected = gpus or (("cpu",) if self.allow_cpu else ())
        if self.require_gpu and not any(x.startswith("cuda:") for x in selected):
            raise RuntimeError("GPU-required role has no admitted GPU")
        if not selected:
            raise RuntimeError("no accelerator or CPU execution target is admissible")
        return tuple(selected)


__all__ = ["AcceleratorMode", "AcceleratorPolicy"]
