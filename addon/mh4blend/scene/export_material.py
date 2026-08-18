"""Blender adapter for self-contained v2 material exports."""

from __future__ import annotations

import os

import bpy

from ..core.material_source import (
    PreparedMaterialExport,
    prepare_material_export,
    write_material_payload_atomic,
)
from ..core.model import MaterialResource
from .material_extract import _material_resource

__all__ = [
    "prepare_blender_material_export",
    "prepare_material_resource_export",
    "prepare_material_resource_exports",
    "write_prepared_material",
]


def _resolved_path(value):
    if value is None:
        return None
    return os.path.abspath(bpy.path.abspath(os.fspath(value)))


def prepare_material_resource_export(
        resource: MaterialResource, output_dir, *, source_root,
        texture_policy="transitional", target_payload_path=None,
        **_legacy_ignored) -> PreparedMaterialExport:
    """Prepare one already-extracted material (the FBX checkbox seam)."""
    if not isinstance(resource, MaterialResource):
        raise TypeError("resource must be MaterialResource")
    return prepare_material_export(
        uid=resource.uid,
        name=resource.name,
        shader_class=resource.shader_class,
        params=resource.params,
        textures=resource.textures,
        source_root=_resolved_path(source_root),
        texture_policy=texture_policy,
        output_dir=_resolved_path(output_dir),
        target_payload_path=_resolved_path(target_payload_path),
    )


def prepare_blender_material_export(
        material, output_dir, *, source_root, texture_policy="transitional",
        target_payload_path=None, **_legacy_ignored) -> PreparedMaterialExport:
    """Extract one Blender Material and prepare its clean v2 source output."""
    if material is None:
        raise ValueError("material is required")
    return prepare_material_resource_export(
        _material_resource(material), output_dir,
        source_root=source_root,
        texture_policy=texture_policy,
        target_payload_path=target_payload_path,
    )


def prepare_material_resource_exports(
        resources, output_dir, *, source_root,
        texture_policy="transitional") -> list[PreparedMaterialExport]:
    """Prepare the de-duplicated material set touched by one FBX export.

    The explicit writer never resolves or relocates prior copies. Every
    touched material targets its clean filename in the requested FBX folder.
    """
    prepared = []
    seen = set()
    for resource in resources:
        if not isinstance(resource, MaterialResource):
            raise TypeError("resources must contain MaterialResource values")
        if resource.uid in seen:
            continue
        seen.add(resource.uid)
        prepared.append(prepare_material_resource_export(
            resource, output_dir, source_root=source_root,
            texture_policy=texture_policy,
            target_payload_path=None,
        ))
    return prepared


def write_prepared_material(
        prepared: PreparedMaterialExport, *, source_root,
        texture_policy="transitional", force=False) -> bool:
    """Write one self-contained material payload."""
    return write_material_payload_atomic(
        prepared, force=force, source_root=_resolved_path(source_root),
        texture_policy=texture_policy)
