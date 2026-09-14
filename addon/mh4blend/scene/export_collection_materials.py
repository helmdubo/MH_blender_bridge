"""Export loaded collection materials without publishing mesh/composite files."""

from pathlib import Path
import os

import bpy

from ..core.source_inventory import scan_source_inventory
from .export_material import (
    _inside, _proxy_dagormat, _resolved_root,
    is_technical_material, material_export_session,
    prepare_blender_material_export, resolve_material_binding,
    write_prepared_material,
)


def export_collection_materials(collection, output_dir, *, source_root):
    """Walk every child/instance, including inactive options, and overwrite materials.

    Only loaded mesh material slots are authority. Geometry and placement
    transforms are deliberately not validated or exported by this operation.
    All material payloads are prepared before the first file is replaced.
    """
    if collection is None:
        raise ValueError("Choose a collection to export materials from")
    root = _resolved_root(source_root)
    if not str(output_dir).strip():
        raise ValueError("Choose an output folder")
    output = Path(bpy.path.abspath(os.fspath(output_dir))).resolve(strict=False)
    if not _inside(root, output):
        raise ValueError("Material output folder must be inside Project Source Root")

    # Instance targets start a resource context; child/LOD collections inherit
    # it so asset-dependent proxymats use the same name across all LODs.
    def resource_name(owner):
        from .export_fbx import _dagor_lod_structure
        lods = _dagor_lod_structure(owner)
        return lods["resource_name"] if lods is not None else owner.name

    with material_export_session(inventory=scan_source_inventory(root)) as session:
        pending = [(collection, collection)]
        visited = set()
        bindings = {}
        seen_materials = set()
        while pending:
            current, owner = pending.pop()
            identity = (current.as_pointer(), owner.as_pointer())
            if identity in visited:
                continue
            visited.add(identity)
            pending.extend((child, owner) for child in current.children)
            for obj in current.objects:
                if obj.instance_collection is not None:
                    target = obj.instance_collection
                    pending.append((target, target))
                if obj.type != "MESH":
                    continue
                from .export_fbx import _is_dagor_collision_object, _is_static_mesh_aux
                if _is_dagor_collision_object(obj) or _is_static_mesh_aux(obj):
                    continue
                for slot in obj.material_slots:
                    material = slot.material
                    if material is None or is_technical_material(material):
                        continue
                    macro_context = (
                        resource_name(owner) if _proxy_dagormat(material) is not None
                        else None)
                    material_key = (material.as_pointer(), macro_context)
                    if material_key in seen_materials:
                        continue
                    seen_materials.add(material_key)
                    binding = resolve_material_binding(material, asset_name=macro_context)
                    bindings[binding.name] = binding
        prepared = [
            prepare_blender_material_export(bindings[name], output, source_root=root)
            for name in sorted(bindings)
        ]
        session.validate_sources()
        updates = [write_prepared_material(row, source_root=root) for row in prepared]
        return {
            "ok": True,
            "collection": collection.name,
            "material_updates": updates,
            "materials_exported": len(updates),
            "material_export_metrics": session.metrics_snapshot(),
        }
