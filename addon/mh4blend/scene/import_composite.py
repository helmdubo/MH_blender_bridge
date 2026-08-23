"""Composite import surface reserved for the Source Protocol v4 S3 slice."""

__all__ = ["import_composite_file"]


def import_composite_file(*_args, **_kwargs):
    raise RuntimeError(
        "MH_E_INVALID_COMPOSITE: Source Protocol v4 composite import "
        "is unavailable until slice S3")
