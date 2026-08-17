"""Model-level validation (schema §6) producing mh.validation_report.

Covers every check computable from the semantic model alone. Scene-level
checks that need Blender data (empty resource collection, missing
collection uid, nested composite collections) live in the extraction
layer (B8) and report through the same builder.

Report contract (§6.2): stable machine codes from the §6.1 registry,
subjects sorted bytewise; messages are human-only and never compared.
"""

from .canonical import ERROR_CODES, validate_resource_name
from .materials import (
    MaterialRegistry,
    MaterialValueError,
    RegistryValidationError,
    material_disk_payload,
    parse_registry,
)

__all__ = ["ValidationError", "ValidationWarning", "MHValidationError",
           "validate_manifest", "build_report"]


class MHValidationError(ValueError):
    """Raised where extraction cannot continue; carries the machine code and
    subject UIDs so the caller can turn it into a report row."""

    def __init__(self, code, subjects, message=""):
        assert code in ERROR_CODES, f"unknown error code {code}"
        self.code = code
        self.subjects = sorted(subjects)
        super().__init__(f"{code}: {message}")

    def as_row(self):
        return ValidationError(self.code, self.subjects, str(self))


class ValidationError:
    """One (code, subjects, message) row; sortable and comparable."""

    def __init__(self, code, subjects, message=""):
        assert code in ERROR_CODES, f"unknown error code {code}"
        assert code.startswith("MH_E_"), f"non-blocking code in errors: {code}"
        self.code = code
        self.subjects = sorted(subjects)
        self.message = message

    def disk_dict(self):
        out = {"code": self.code, "subjects": self.subjects}
        if self.message:
            out["message"] = self.message
        return out


class ValidationWarning:
    """One non-blocking warning row, transported separately from errors."""

    def __init__(self, code, subjects, message=""):
        assert code in ERROR_CODES, f"unknown warning code {code}"
        assert code.startswith("MH_W_"), f"blocking code in warnings: {code}"
        self.code = code
        self.subjects = sorted(subjects)
        self.message = message

    def disk_dict(self):
        out = {"code": self.code, "subjects": self.subjects}
        if self.message:
            out["message"] = self.message
        return out


def build_report(errors, warnings=()):
    rows = sorted(errors, key=lambda e: (e.code, e.subjects))
    warning_rows = sorted(warnings, key=lambda w: (w.code, w.subjects))
    return {
        "schema": "mh.validation_report",
        "schema_version": 2,
        "errors": [e.disk_dict() for e in rows],
        "warnings": [w.disk_dict() for w in warning_rows],
    }


def _check_names(manifest, errors):
    named = [(manifest.bundle_name, manifest.bundle_uid)]
    named += [(m.name, m.uid) for m in manifest.meshes]
    named += [(c.name, c.uid) for c in manifest.composites]
    named += [(m.name, m.uid) for m in manifest.materials]
    for name, uid in named:
        try:
            validate_resource_name(name)
        except ValueError as exc:
            errors.append(ValidationError(
                "MH_E_NON_ASCII_RESOURCE_NAME", [uid], str(exc)))


def _check_duplicate_resource_uids(manifest, errors):
    seen = {}
    for res in (list(manifest.meshes) + list(manifest.composites)
                + list(manifest.materials)):
        seen.setdefault(res.uid, []).append(res)
    for uid, owners in seen.items():
        if len(owners) > 1:
            errors.append(ValidationError(
                "MH_E_DUPLICATE_RESOURCE_UID", [uid],
                f"{len(owners)} resources share this uid"))


def _check_uid8_collisions(manifest, errors):
    by_uid8 = {}
    for res in list(manifest.meshes) + list(manifest.composites):
        by_uid8.setdefault(res.uid[:8], set()).add(res.uid)
    for uid8, uids in by_uid8.items():
        if len(uids) > 1:
            errors.append(ValidationError(
                "MH_E_UID8_COLLISION", sorted(uids),
                f"uid8 prefix '{uid8}' collides; regenerate one UID"))


def _check_nodes(composite, errors):
    seen = {}
    for node in composite.nodes:
        seen.setdefault(node.node_uid, 0)
        seen[node.node_uid] += 1
    for uid, count in seen.items():
        if count > 1:
            errors.append(ValidationError(
                "MH_E_DUPLICATE_NODE_UID", [uid],
                f"{count} nodes share this uid in '{composite.name}'"))

    table = {node.node_uid: node for node in composite.nodes}
    for node in composite.nodes:
        if node.parent_uid is not None and node.parent_uid not in table:
            errors.append(ValidationError(
                "MH_E_DANGLING_PARENT", [node.node_uid],
                f"parent {node.parent_uid} is not in the node table"))

    # parent-chain cycles (nodes with resolvable parents only)
    for node in composite.nodes:
        slow = node
        visited = set()
        while slow.parent_uid is not None and slow.parent_uid in table:
            if slow.node_uid in visited:
                errors.append(ValidationError(
                    "MH_E_PARENT_CYCLE", sorted(visited),
                    f"parent chain cycle in '{composite.name}'"))
                break
            visited.add(slow.node_uid)
            slow = table[slow.parent_uid]

    for node in composite.nodes:
        if any(q <= 0 for q in node.local_transform.scale):
            errors.append(ValidationError(
                "MH_E_INVALID_SCALE", [node.node_uid],
                "mirror the geometry, not the placement"))


def _check_composite_cycles(manifest, errors):
    """DFS over composite_ref edges within the manifest (D10). Diamonds are
    legal; a back-edge to a node on the current stack is a cycle."""
    graph = {}
    local = {c.uid for c in manifest.composites}
    for composite in manifest.composites:
        refs = [n.resource_uid for n in composite.nodes
                if n.kind == "composite_ref" and n.resource_uid in local]
        graph[composite.uid] = refs

    WHITE, GRAY, BLACK = 0, 1, 2
    color = {uid: WHITE for uid in graph}
    reported = set()

    def dfs(uid, stack):
        color[uid] = GRAY
        stack.append(uid)
        for ref in graph[uid]:
            if color[ref] == GRAY:
                cycle = stack[stack.index(ref):]
                key = frozenset(cycle)
                if key not in reported:
                    reported.add(key)
                    chain = " -> ".join(cycle + [ref])
                    errors.append(ValidationError(
                        "MH_E_COMPOSITE_CYCLE", sorted(cycle), chain))
            elif color[ref] == WHITE:
                dfs(ref, stack)
        stack.pop()
        color[uid] = BLACK

    for uid in graph:
        if color[uid] == WHITE:
            dfs(uid, [])


def _check_material_values(manifest, errors):
    invalid = set()
    for material in manifest.materials:
        try:
            material_disk_payload(
                material.shader_class, material.params, material.textures)
        except MaterialValueError as exc:
            invalid.add(material.uid)
            errors.append(ValidationError(exc.code, [material.uid], str(exc)))
    return invalid


def _check_shader_classes(manifest, registry, warnings, invalid_materials):
    if registry is None:
        return
    try:
        parsed = registry if isinstance(registry, MaterialRegistry) else parse_registry(registry)
    except RegistryValidationError as exc:
        warnings.append(ValidationWarning(
            "MH_W_REGISTRY_INVALID", [manifest.bundle_uid], str(exc)))
        return
    for material in manifest.materials:
        if material.uid in invalid_materials:
            continue
        if not parsed.contains(material.shader_class):
            warnings.append(ValidationWarning(
                "MH_W_UNKNOWN_SHADER_CLASS", [material.uid],
                f"shader_class '{material.shader_class}' is absent from registry"))


def validate_manifest(manifest, registry=None):
    """Run all model-level checks; returns the mh.validation_report dict."""
    errors = []
    warnings = []
    _check_names(manifest, errors)
    _check_duplicate_resource_uids(manifest, errors)
    _check_uid8_collisions(manifest, errors)
    for composite in manifest.composites:
        _check_nodes(composite, errors)
    _check_composite_cycles(manifest, errors)
    invalid_materials = _check_material_values(manifest, errors)
    _check_shader_classes(manifest, registry, warnings, invalid_materials)
    return build_report(errors, warnings)
