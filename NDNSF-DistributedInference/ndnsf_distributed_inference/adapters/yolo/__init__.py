"""Registered YOLO26n ONNX adapter."""

_OWNERS = {
    **dict.fromkeys(('Yolo26Splitter', 'YoloCanonicalArtifactBinding', 'build_yolo26n_adapter'), 'adapter'),
    **dict.fromkeys(('RegisteredYoloCandidate', 'verify_catalogue'), 'candidates'),
    **dict.fromkeys(('Yolo26GraphAdapter', 'load_yolo_graph'), 'graph'),
}


def __getattr__(name):
    from importlib import import_module
    if name not in _OWNERS:
        raise AttributeError(name)
    value = getattr(import_module('.' + _OWNERS[name], __name__), name)
    globals()[name] = value
    return value


def __dir__():
    return sorted(set(globals()) | set(__all__))

__all__ = [
    "RegisteredYoloCandidate", "Yolo26GraphAdapter", "Yolo26Splitter",
    "YoloCanonicalArtifactBinding", "build_yolo26n_adapter",
    "load_yolo_graph", "verify_catalogue",
]
