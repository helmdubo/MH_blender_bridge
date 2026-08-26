"""Blender-local identity stamps for resource definition collections.

These markers never enter FBX or Source Protocol payloads.  They let a
collection instance carry the identity established by a successful import or
export, so Composite authoring does not ask the artist to repeat it.
"""

from ..core.canonical import validate_resource_name

COLLECTION_KIND_KEY = "mh_resource_kind"
COLLECTION_RESOURCE_KEY = "mh_resource_name"
INCOMPLETE_IMPORT_KEY = "mh_incomplete_import"

DEFINITION_REUSE = "reuse"
DEFINITION_REFRESH = "refresh"
DEFINITION_POLICIES = frozenset({DEFINITION_REUSE, DEFINITION_REFRESH})


def stamp_resource_collection(
        collection, kind: str, resource_name: str, *, incomplete=False) -> None:
    if kind not in {"mesh", "actor", "composite"}:
        raise ValueError(f"collection resource kind is unsupported: {kind!r}")
    validate_resource_name(resource_name)
    collection[COLLECTION_KIND_KEY] = kind
    collection[COLLECTION_RESOURCE_KEY] = resource_name
    if incomplete:
        collection[INCOMPLETE_IMPORT_KEY] = True
    elif INCOMPLETE_IMPORT_KEY in collection:
        del collection[INCOMPLETE_IMPORT_KEY]


def is_managed_resource_collection(collection, kind: str, resource_name: str) -> bool:
    """Return whether both exact MH identity stamps match one ResourceKey."""

    return (
        collection.get(COLLECTION_KIND_KEY) == kind
        and collection.get(COLLECTION_RESOURCE_KEY) == resource_name
    )


def managed_resource_collections(kind: str, resource_name: str) -> tuple:
    """Find exact managed definitions without treating datablock names as truth."""

    import bpy

    validate_resource_name(resource_name)
    return tuple(
        collection for collection in bpy.data.collections
        if is_managed_resource_collection(collection, kind, resource_name)
    )
