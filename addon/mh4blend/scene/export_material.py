"""Blender adapter for standalone and FBX-triggered material exports.

This module intentionally stops at the manifest transaction seam.  The global
resolver supplies an existing material owner when one exists; the final writer
must stage that owning manifest before calling ``write_prepared_material`` and
promote it afterwards.
"""

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
        owning_manifest_path=None, existing_source=None) -> PreparedMaterialExport:
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
        owning_manifest_path=_resolved_path(owning_manifest_path),
        existing_source=existing_source,
    )


def prepare_blender_material_export(
        material, output_dir, *, source_root, texture_policy="transitional",
        target_payload_path=None, owning_manifest_path=None,
        existing_source=None) -> PreparedMaterialExport:
    """Extract one Blender Material and prepare its frozen-v1 source output."""
    if material is None:
        raise ValueError("material is required")
    return prepare_material_resource_export(
        _material_resource(material), output_dir,
        source_root=source_root,
        texture_policy=texture_policy,
        target_payload_path=target_payload_path,
        owning_manifest_path=owning_manifest_path,
        existing_source=existing_source,
    )


def prepare_material_resource_exports(
        resources, output_dir, *, source_root, texture_policy="transitional",
        owners_by_uid=None) -> list[PreparedMaterialExport]:
    """Prepare the de-duplicated material set touched by one FBX export.

    ``owners_by_uid`` accepts the resolver's honest result shape:
    ``{uid: {payload_path, owning_manifest_path, manifest_row}}``.  Missing
    UIDs are first-created beside the FBX output; resolved UIDs always retain
    their existing location and filename.
    """
    owners = owners_by_uid or {}
    prepared = []
    seen = set()
    for resource in resources:
        if not isinstance(resource, MaterialResource):
            raise TypeError("resources must contain MaterialResource values")
        if resource.uid in seen:
            continue
        seen.add(resource.uid)
        owner = owners.get(resource.uid)
        if owner is None:
            target = manifest = source = None
        else:
            try:
                target = owner["payload_path"]
                manifest = owner["owning_manifest_path"]
                source = owner["manifest_row"]["source"]
            except (KeyError, TypeError) as exc:
                raise ValueError(
                    f"invalid resolver owner for material {resource.uid}") from exc
        prepared.append(prepare_material_resource_export(
            resource, output_dir, source_root=source_root,
            texture_policy=texture_policy,
            target_payload_path=target,
            owning_manifest_path=manifest,
            existing_source=source,
        ))
    return prepared


def write_prepared_material(
        prepared: PreparedMaterialExport, *, source_root,
        texture_policy="transitional", force=False) -> bool:
    """Write payload after the caller has staged its owning manifest marker."""
    return write_material_payload_atomic(
        prepared, force=force, source_root=_resolved_path(source_root),
        texture_policy=texture_policy)
