"""Semantic model of a bundle and its on-disk writers (schema §1-§3, §8.1).

bpy-free by design: the Blender extraction layer (B8) produces these
dataclasses, the exporter (B10) feeds them to the writers. Transforms are
stored as quantized integers (§8.2) — the writers render decimals q / 10^p;
hashes are computed via the canonical library from the exact structures
written to disk, so file, canon form and hash cannot diverge.
"""

from dataclasses import dataclass, field

from .canonical import (
    P_ROTATION_QUAT,
    P_SCALE,
    P_TRANSLATION_CM,
    composite_content_hash,
    nfc,
    resource_filename,
)
from .materials import material_content_hash, material_disk_payload

__all__ = [
    "QuantizedTransform",
    "Node",
    "Composite",
    "MaterialSlot",
    "MeshResource",
    "MaterialResource",
    "Manifest",
    "composite_disk_dict",
    "manifest_disk_dict",
    "composite_hash",
    "IDENTITY_TRANSFORM",
    "NODE_KINDS",
    "RESERVED_NODE_KINDS",
]

NODE_KINDS = ("group", "mesh", "composite_ref")
RESERVED_NODE_KINDS = ("variant_set", "variant", "actor")


def _dec(q: int, p: int) -> float:
    """Quantized integer -> on-disk decimal (q / 10^p)."""
    return q / 10 ** p


@dataclass(frozen=True)
class QuantizedTransform:
    """local_transform in canonical integers (§8.2)."""
    translation: tuple  # 3 x int, p=3 (cm)
    rotation: tuple     # 4 x int, p=6, sign-canonicalized (x, y, z, w)
    scale: tuple        # 3 x int, p=6

    def disk_dict(self) -> dict:
        return {
            "translation_cm": [_dec(q, P_TRANSLATION_CM) for q in self.translation],
            "rotation_quat": [_dec(q, P_ROTATION_QUAT) for q in self.rotation],
            "scale": [_dec(q, P_SCALE) for q in self.scale],
        }


IDENTITY_TRANSFORM = QuantizedTransform(
    translation=(0, 0, 0),
    rotation=(0, 0, 0, 10 ** P_ROTATION_QUAT),
    scale=(10 ** P_SCALE,) * 3,
)


@dataclass
class Node:
    node_uid: str
    parent_uid: str | None
    kind: str
    display_name: str
    resource_uid: str | None
    local_transform: QuantizedTransform
    properties: dict = field(default_factory=dict)

    def disk_dict(self) -> dict:
        # Fixed on-disk key order (§8.4); group nodes carry no resource_uid.
        out = {
            "node_uid": self.node_uid,
            "parent_uid": self.parent_uid,
            "kind": self.kind,
        }
        out["display_name"] = nfc(self.display_name)
        if self.resource_uid is not None:
            out["resource_uid"] = self.resource_uid
        out["local_transform"] = self.local_transform.disk_dict()
        out["properties"] = _nfc_tree(self.properties)
        return out


@dataclass
class Composite:
    uid: str
    name: str
    nodes: list  # list[Node]
    # Asset-level bag. mh.composite v2 stores it in the payload itself; it is
    # never inherited by nodes. Frozen v1 readers obtain it from the owning
    # manifest row instead.
    properties: dict = field(default_factory=dict)

    def filename(self) -> str:
        return resource_filename(self.name, self.uid, ".composite")


@dataclass(frozen=True)
class MaterialSlot:
    slot_name: str
    material_uid: str


@dataclass
class MeshResource:
    uid: str
    name: str
    content_hash: str  # xxh3:... over the §9 stream
    material_slots: list = field(default_factory=list)  # list[MaterialSlot]
    properties: dict = field(default_factory=dict)  # asset-level bag (§2, Q9)

    def filename(self) -> str:
        return resource_filename(self.name, self.uid, ".mesh.fbx")

    def source(self) -> str:
        return f"meshes/{self.filename()}"


@dataclass
class MaterialResource:
    uid: str
    name: str
    shader_class: str
    params: dict = field(default_factory=dict)
    # slot -> authored texture path. Blender-relative ``//`` paths are
    # resolved by the host adapter; files are referenced in place, not copied.
    textures: dict = field(default_factory=dict)

    @property
    def content_hash(self) -> str:
        """Canonical fingerprint of the UE material payload (not identity)."""
        return material_content_hash(self.shader_class, self.params, self.textures)

    def disk_payload(self) -> dict:
        """Normalized semantic payload used by the manifest writer."""
        return material_disk_payload(self.shader_class, self.params, self.textures)


@dataclass
class Manifest:
    bundle_uid: str
    bundle_name: str
    blend_file: str
    exporter_version: str
    meshes: list = field(default_factory=list)       # list[MeshResource]
    composites: list = field(default_factory=list)   # list[Composite]
    materials: list = field(default_factory=list)    # list[MaterialResource]
    external_dependencies: list = field(default_factory=list)  # list[dict]


def _nfc_tree(value):
    """NFC-normalize every string in a JSON-ish tree (§8.4, writer side)."""
    if isinstance(value, str):
        return nfc(value)
    if isinstance(value, dict):
        return {nfc(k): _nfc_tree(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [_nfc_tree(v) for v in value]
    return value


def composite_disk_dict(composite: Composite, *, schema_version: int = 2) -> dict:
    """Build one ``mh.composite`` payload with deterministic key/node order.

    Production writers emit v2, where the payload owns resource-level
    ``properties``. ``schema_version=1`` remains only as a frozen-fixture and
    migration seam; it reproduces the accepted v1 bytes without rewriting the
    old golden corpus.
    """
    if schema_version not in {1, 2} or isinstance(schema_version, bool):
        raise ValueError("composite schema_version must be integer 1 or 2")
    nodes = sorted(composite.nodes, key=lambda n: n.node_uid.encode("utf-8"))
    document = {
        "schema": "mh.composite",
        "schema_version": schema_version,
        "uid": composite.uid,
        "name": nfc(composite.name),
    }
    if schema_version == 2:
        # Present even when empty: this is the v2 authority boundary, not an
        # optional manifest-style decoration.
        document["properties"] = _nfc_tree(composite.properties)
    document["nodes"] = [node.disk_dict() for node in nodes]
    return document


def manifest_disk_dict(manifest: Manifest, composite_hashes: dict) -> dict:
    """On-disk export_manifest.json (§2). `composite_hashes` maps
    composite uid -> content_hash (computed by the exporter from the exact
    disk dicts it wrote); mesh hashes live on the MeshResource entries."""
    resources = []
    for mesh in manifest.meshes:
        entry = {
            "uid": mesh.uid,
            "kind": "static_mesh",
            "name": nfc(mesh.name),
            "source": mesh.source(),
            "content_hash": mesh.content_hash,
        }
        if mesh.material_slots:
            entry["material_slots"] = [
                {"slot_name": nfc(s.slot_name), "material_uid": s.material_uid}
                for s in mesh.material_slots
            ]
        if mesh.properties:
            entry["properties"] = _nfc_tree(mesh.properties)
        resources.append(entry)
    for composite in manifest.composites:
        entry = {
            "uid": composite.uid,
            "kind": "composite",
            "name": nfc(composite.name),
            "source": composite.filename(),
            "content_hash": composite_hashes[composite.uid],
        }
        resources.append(entry)
    resources.sort(key=lambda r: r["uid"].encode("utf-8"))

    materials = []
    for material in manifest.materials:
        payload = material.disk_payload()
        materials.append({
            "uid": material.uid,
            "kind": "material",
            "name": nfc(material.name),
            "shader_class": payload["shader_class"],
            "params": payload["params"],
            "textures": payload["textures"],
            "content_hash": material.content_hash,
        })
    materials.sort(key=lambda m: m["uid"].encode("utf-8"))

    external = sorted(
        (dict(e) for e in manifest.external_dependencies),
        key=lambda e: e["uid"].encode("utf-8"))

    return {
        "schema": "mh.bundle_manifest",
        "schema_version": 2,
        "exporter_version": manifest.exporter_version,
        "bundle_uid": manifest.bundle_uid,
        "bundle_name": nfc(manifest.bundle_name),
        "source": {"blend_file": nfc(manifest.blend_file)},
        "resources": resources,
        "materials": materials,
        "external_dependencies": external,
    }


def composite_hash(composite: Composite, *, schema_version: int = 2) -> str:
    """content_hash of a composite — over the exact disk dict (§8)."""
    return composite_content_hash(composite_disk_dict(
        composite, schema_version=schema_version))
