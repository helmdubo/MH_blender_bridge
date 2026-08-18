"""Mesh serialization for content_hash — combined-LOD stream mh.meshser:2.

Pure byte encoding: the caller (B8 extraction layer) supplies plain arrays
pulled from evaluated meshes IN BLENDER METERS (§9: the hash is computed
before the cm export conversion). This module is the reference
implementation of the byte layout; the future C++ side mirrors it.

Layout per object (§9.3): transform (16 x q6, row-major, no count) ->
positions (count + 3 x q4 each) -> polygons (count + per-poly vert count,
vertex indices, material_index) -> custom split normals (uint8 flag +
per-loop 3 x q4, loop count implied) -> per-poly use_smooth bytes + sharp
edges (count + sorted pairs) -> UV layers (count + name + per-loop 2 x q6)
-> color attributes (count + name/domain/type + value count + q4 values)
-> material slot names (count + strings).
Encodings per §9.1: little-endian, uint32 counts/indices, int64 scaled
integers, length-prefixed UTF-8 strings. Each object is prefixed by its uint32
LOD level; objects are ordered by ``(lod_level, mh_uid)``.
"""

import struct
from dataclasses import dataclass, field

from .canonical import hash_hex, nfc, quantize

__all__ = ["MeshObjectRecord", "serialize_mesh_resource", "mesh_content_hash",
           "MESHSER_TAG"]

MESHSER_TAG = "mh.meshser:2"

P_POSITION = 4   # positions / normals / colors (§9.3)
P_TRANSFORM = 6  # object transform matrix
P_UV = 6         # UV coordinates


@dataclass
class MeshObjectRecord:
    """Everything of one object that §9.3 hashes. No names — the object and
    datablock names are structurally absent (anti-requirement §9.4)."""
    transform: tuple            # 16 floats, row-major 4x4, collection space
    positions: list             # [(x, y, z) floats, meters]
    polygons: list              # [(material_index, (vert_idx, ...)), ...]
    split_normals: list | None  # per-loop [(x, y, z)] or None
    use_smooth: list = field(default_factory=list)   # per-poly bools
    sharp_edges: list = field(default_factory=list)  # [(v0, v1), ...]
    uv_layers: dict = field(default_factory=dict)    # name -> per-loop [(u, v)]
    color_attributes: list = field(default_factory=list)
    # [(name, domain, data_type, [flat float components]), ...]
    material_slot_names: list = field(default_factory=list)


def _u32(out, value):
    out.append(struct.pack("<I", value))


def _i64q(out, value, p):
    out.append(struct.pack("<q", quantize(float(value), p)))


def _string(out, text):
    data = nfc(text).encode("utf-8")
    _u32(out, len(data))
    out.append(data)


def _serialize_object(out, record: MeshObjectRecord):
    # 1. transform, 16 x q6, no count
    assert len(record.transform) == 16
    for component in record.transform:
        _i64q(out, component, P_TRANSFORM)
    # 2. positions
    _u32(out, len(record.positions))
    for x, y, z in record.positions:
        _i64q(out, x, P_POSITION)
        _i64q(out, y, P_POSITION)
        _i64q(out, z, P_POSITION)
    # 3. polygons: vert count, indices, material_index
    _u32(out, len(record.polygons))
    loop_total = 0
    for material_index, indices in record.polygons:
        _u32(out, len(indices))
        for index in indices:
            _u32(out, index)
        _u32(out, material_index)
        loop_total += len(indices)
    # 4. custom split normals
    if record.split_normals is None:
        out.append(b"\x00")
    else:
        out.append(b"\x01")
        assert len(record.split_normals) == loop_total
        for x, y, z in record.split_normals:
            _i64q(out, x, P_POSITION)
            _i64q(out, y, P_POSITION)
            _i64q(out, z, P_POSITION)
    # 5. shading: per-poly smooth flags (poly count implied), sharp edges
    assert len(record.use_smooth) == len(record.polygons)
    out.append(bytes(1 if smooth else 0 for smooth in record.use_smooth))
    pairs = sorted(tuple(sorted(edge)) for edge in record.sharp_edges)
    _u32(out, len(pairs))
    for v0, v1 in pairs:
        _u32(out, v0)
        _u32(out, v1)
    # 6. UV layers, name-sorted (names are content, §9.3)
    names = sorted(record.uv_layers, key=lambda n: nfc(n).encode("utf-8"))
    _u32(out, len(names))
    for name in names:
        _string(out, name)
        layer = record.uv_layers[name]
        assert len(layer) == loop_total
        for u, v in layer:
            _i64q(out, u, P_UV)
            _i64q(out, v, P_UV)
    # 7. color attributes, name-sorted
    attrs = sorted(record.color_attributes,
                   key=lambda a: nfc(a[0]).encode("utf-8"))
    _u32(out, len(attrs))
    for name, domain, data_type, values in attrs:
        _string(out, name)
        _string(out, domain)
        _string(out, data_type)
        # SPEC NOTE: §9.3 does not state an explicit count for color values
        # (component count varies by type); the reference layout writes a
        # uint32 flat-component count, then the components. C++ must mirror.
        _u32(out, len(values))
        for component in values:
            _i64q(out, component, P_POSITION)
    # 8. material slot names, slot order
    _u32(out, len(record.material_slot_names))
    for name in record.material_slot_names:
        _string(out, name)


def _lod_item(item):
    """Normalize legacy level-0 pairs and explicit combined-LOD triples."""
    if len(item) == 2:
        uid, record = item
        return 0, uid, record
    if len(item) == 3:
        lod_level, uid, record = item
        if not isinstance(lod_level, int) or isinstance(lod_level, bool) \
                or lod_level < 0 or lod_level > 0xFFFFFFFF:
            raise ValueError("lod_level must be a uint32 integer")
        return lod_level, uid, record
    raise ValueError(
        "mesh object record must be (uid, record) or "
        "(lod_level, uid, record)")


def serialize_mesh_resource(objects):
    """Return the mh.meshser:2 stream for level-tagged mesh objects.

    ``objects`` accepts explicit ``(lod_level, mh_uid, MeshObjectRecord)``
    triples.  Two-tuples remain a convenience spelling for ordinary LOD0
    resources; their bytes are still meshser:2 and include a zero level.
    """
    out = []
    _string(out, MESHSER_TAG)
    ordered = sorted(
        (_lod_item(item) for item in objects),
        key=lambda item: (item[0], item[1].encode("utf-8")),
    )
    # SPEC NOTE: §9 does not name an explicit object-count field; the
    # reference layout writes uint32 count after the tag (self-delimiting
    # streams are cheaper to mirror in C++ than sentinel-terminated ones).
    _u32(out, len(ordered))
    for lod_level, _uid, record in ordered:
        _u32(out, lod_level)
        _serialize_object(out, record)
    return b"".join(out)


def mesh_content_hash(objects) -> str:
    return hash_hex(serialize_mesh_resource(objects))
