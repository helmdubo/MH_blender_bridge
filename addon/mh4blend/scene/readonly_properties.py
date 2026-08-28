"""Read optional RNA PropertyGroups without allocating their ID backing."""

import bpy


def existing_property_group(owner, attribute):
    # PointerProperty access allocates an empty group lazily. Absence means
    # default settings, not permission for an export/preflight to author them.
    if isinstance(owner, (bpy.types.ID, bpy.types.PropertyGroup)):
        if attribute not in owner.keys():
            return None
        value = getattr(owner, attribute, None)
        if value is None:
            raise ValueError(
                f"MH_E_INVALID_RESOURCE_SOURCE: saved {attribute} carrier is present "
                f"on {getattr(owner, 'name', type(owner).__name__)} but its RNA is not registered")
        return value
    return getattr(owner, attribute, None)
