"""Translate explicit model representation fields for experiment configuration."""


def manifest_quantization_subtype(manifest):
    """Preserve the declared representation; an INT8 bit width is ambiguous."""
    quantization = str(manifest.get("quantization", "none")).lower()
    subtype = manifest.get("quantizationSubtype")
    if subtype is not None:
        subtype = str(subtype).lower()
    if quantization == "int8":
        if subtype not in {"dynamic_int8", "weight_only_int8"}:
            raise ValueError("MODEL_INT8_SUBTYPE_REQUIRED")
        return subtype
    if quantization not in {"none", "dynamic_int8", "weight_only_int8", "weight_only_int4"}:
        raise ValueError("MODEL_QUANTIZATION_UNSUPPORTED")
    if subtype is not None and subtype != quantization:
        raise ValueError("MODEL_QUANTIZATION_SUBTYPE_MISMATCH")
    return quantization
