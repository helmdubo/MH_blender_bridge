"""Blender-local identity stamps for resource definition collections.

These markers never enter FBX or Source Protocol payloads.  They let a
collection instance carry the identity established by a successful import or
export, so Composite authoring does not ask the artist to repeat it.
"""

from ..core.canonical import validate_resource_name

COLLECTION_KIND_KEY = "mh_resource_kind"
COLLECTION_RESOURCE_KEY = "mh_resource_name"


def stamp_resource_collection(collection, kind: str, resource_name: str) -> None:
    if kind not in {"mesh", "composite"}:
        raise ValueError(f"collection resource kind is unsupported: {kind!r}")
    validate_resource_name(resource_name)
    collection[COLLECTION_KIND_KEY] = kind
    collection[COLLECTION_RESOURCE_KEY] = resource_name
