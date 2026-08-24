"""Composite export surface reserved for the Source Protocol v4 S3 slice."""

__all__ = ["export_composite_collection"]


def export_composite_collection(*_args, **_kwargs):
    raise RuntimeError(
        "MH_E_INVALID_COMPOSITE: Source Protocol v4 composite export "
        "is unavailable until slice S3")
