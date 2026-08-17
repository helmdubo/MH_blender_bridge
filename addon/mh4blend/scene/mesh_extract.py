"""bpy adapter: evaluated meshes of a GEOMETRY collection -> MeshObjectRecord.

The only place §9 touches Blender. Everything is read in Blender meters
(§9: the hash is computed BEFORE the cm export conversion) from the
evaluated depsgraph state (modifiers applied). Object transforms are
matrix_world — resource-space is the GEOMETRY scene's world space (§9.3).
"""

import bpy

from ..core.meshser import MeshObjectRecord, mesh_content_hash
from ..core.uid import PROP_UID

__all__ = ["extract_mesh_records", "collection_content_hash"]


def _record_for_object(obj, depsgraph):
    eval_obj = obj.evaluated_get(depsgraph)
    mesh = eval_obj.to_mesh()
    try:
        transform = tuple(
            component for row in obj.matrix_world for component in row)
        positions = [tuple(v.co) for v in mesh.vertices]
        polygons = [
            (poly.material_index, tuple(poly.vertices)) for poly in mesh.polygons]
        split_normals = None
        if mesh.has_custom_normals:
            split_normals = [tuple(corner.vector) for corner in mesh.corner_normals]
        use_smooth = [poly.use_smooth for poly in mesh.polygons]
        sharp_edges = [
            tuple(edge.vertices) for edge in mesh.edges if edge.use_edge_sharp]
        uv_layers = {
            layer.name: [tuple(item.uv) for item in layer.data]
            for layer in mesh.uv_layers
        }
        color_attributes = []
        for attr in mesh.color_attributes:
            values = []
            for item in attr.data:
                values.extend(item.color)
            color_attributes.append(
                (attr.name, attr.domain, attr.data_type, values))
        slot_names = [
            slot.material.name if slot.material else ""
            for slot in obj.material_slots
        ]
        return MeshObjectRecord(
            transform=transform,
            positions=positions,
            polygons=polygons,
            split_normals=split_normals,
            use_smooth=use_smooth,
            sharp_edges=sharp_edges,
            uv_layers=uv_layers,
            color_attributes=color_attributes,
            material_slot_names=slot_names,
        )
    finally:
        eval_obj.to_mesh_clear()


def extract_mesh_records(collection, depsgraph=None):
    """[(object mh_uid, MeshObjectRecord)] for every MESH object of a
    GEOMETRY resource collection. Objects without mh_uid are a caller-side
    validation concern (lazy assignment runs before extraction)."""
    if depsgraph is None:
        depsgraph = bpy.context.evaluated_depsgraph_get()
    records = []
    for obj in collection.objects:
        if obj.type != "MESH":
            continue
        uid = obj.get(PROP_UID)
        if not uid:
            raise ValueError(
                f"object '{obj.name}' has no {PROP_UID}; run UID assignment "
                "before extraction")
        records.append((uid, _record_for_object(obj, depsgraph)))
    return records


def collection_content_hash(collection, depsgraph=None) -> str:
    """§9 content_hash of one GEOMETRY collection resource."""
    return mesh_content_hash(extract_mesh_records(collection, depsgraph))
