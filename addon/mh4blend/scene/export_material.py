"""Material export surface reserved for the Source Protocol v4 S2 slice."""

__all__ = ["prepare_blender_material_export", "write_prepared_material"]


def _pending():
    raise RuntimeError(
        "MH_E_INVALID_MATERIAL_VALUE: Source Protocol v4 material export "
        "is unavailable until slice S2")


def prepare_blender_material_export(*_args, **_kwargs):
    _pending()


def write_prepared_material(*_args, **_kwargs):
    _pending()
