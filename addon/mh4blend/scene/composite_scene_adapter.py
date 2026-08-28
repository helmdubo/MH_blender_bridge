"""Read-only scene-form dispatch; selecting a reader never adopts a scene."""

import bpy

from ..core.canonical import validate_resource_name
from ..core.validate import MHValidationError
from .resource_markers import COLLECTION_KIND_KEY, COLLECTION_RESOURCE_KEY


def composite_scene_form(collection):
    if collection is None or bpy.data.collections.get(collection.name) is not collection:
        raise MHValidationError(
            "MH_E_INVALID_RESOURCE_SOURCE", [repr(collection)],
            "composite export requires a live Blender Collection")
    keys = (COLLECTION_KIND_KEY, COLLECTION_RESOURCE_KEY, "type", "name")
    found = [key for key in keys if key in collection]
    has_kind = COLLECTION_KIND_KEY in collection
    has_name = COLLECTION_RESOURCE_KEY in collection
    dagor = "type" in collection or "name" in collection

    def reject(message):
        raise MHValidationError(
            "MH_E_INVALID_RESOURCE_SOURCE", [collection.name, *found], message)

    if has_kind != has_name:
        reject("partial MH identity cannot select an export adapter")
    if has_kind and dagor:
        reject("mixed MH/dag4blend authority cannot select an export adapter")
    if dagor:
        if "type" not in collection or "name" not in collection:
            reject("partial dag4blend type/name identity")
        if collection["type"] != "composit":
            reject("dag4blend composite root requires type == 'composit'")
        try:
            validate_resource_name(collection["name"])
        except (TypeError, ValueError):
            reject("dag4blend root name must be canonical [a-z0-9_]+")
        return "dag4blend"
    return "mh"
