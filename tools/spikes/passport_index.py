"""Pure G3 spike for a passport-first, disposable local index.

This module intentionally has no Blender dependency and is not imported by the
add-on.  The caller supplies passport carrier copies already extracted from a
payload; parsing an FBX container belongs to G1/the host adapters.
"""

from __future__ import annotations

from copy import deepcopy
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import sys
import unicodedata
import uuid


INDEX_SCHEMA = "mh.local_index"
INDEX_SCHEMA_VERSION = 0  # implementation detail, deliberately not protocol v2
PASSPORT_SCHEMA = "mh.fbx_passport"
PASSPORT_SCHEMA_VERSION = 1
PROVISIONAL_EMPTY_REBUILD_NOTE = (
    "No prior resolution existed; identical copies were provisionally resolved "
    "by normalized path order. A host policy must replace this before v2 freeze."
)


@dataclass(frozen=True)
class PayloadObservation:
    """A payload path plus zero or more carrier copies extracted by a host."""

    path: Path
    passport_copies: tuple[str, ...] = ()

    @classmethod
    def from_text(
        cls, path: str | Path, passport_text: str | None
    ) -> "PayloadObservation":
        copies = () if passport_text is None else (passport_text,)
        return cls(Path(path), copies)


def canonical_path(path: str | Path) -> str:
    """Return the stable local spelling used as an index key."""

    return os.path.normcase(str(Path(path).resolve(strict=False)))


def local_index_path(
    project_uid: str,
    *,
    platform_name: str | None = None,
    local_appdata: str | Path | None = None,
    home: str | Path | None = None,
    xdg_cache_home: str | Path | None = None,
) -> Path:
    """Inventory the proposed per-host cache location outside source trees."""

    try:
        normalized_uid = str(uuid.UUID(project_uid))
    except (ValueError, AttributeError, TypeError) as exc:
        raise ValueError("project_uid must be a UUID") from exc

    platform_name = platform_name or sys.platform
    user_home = Path(home) if home is not None else Path.home()
    if platform_name.startswith("win"):
        base = Path(
            local_appdata
            if local_appdata is not None
            else os.environ.get("LOCALAPPDATA", user_home / "AppData" / "Local")
        )
    elif platform_name == "darwin":
        base = user_home / "Library" / "Caches"
    else:
        base = Path(
            xdg_cache_home
            if xdg_cache_home is not None
            else os.environ.get("XDG_CACHE_HOME", user_home / ".cache")
        )
    return base / "MimirHead" / "MHBridge" / normalized_uid / "index.json"


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return f"sha256:{digest.hexdigest()}"


def exact_payload_fingerprint(
    path: str | Path,
    *,
    cached_row: dict | None = None,
    force_hash: bool = False,
) -> dict:
    """Return an exact byte hash, using size+mtime only to reuse a cached hash.

    Size and mtime are never themselves called the fingerprint.  They are only
    a fast-path admission for a previously computed exact byte hash.
    """

    payload = Path(path)
    stat = payload.stat()
    fast_path = bool(
        not force_hash
        and cached_row
        and cached_row.get("size") == stat.st_size
        and cached_row.get("mtime_ns") == stat.st_mtime_ns
        and isinstance(cached_row.get("payload_fingerprint"), str)
        and cached_row["payload_fingerprint"].startswith("sha256:")
    )
    byte_hash = (
        cached_row["payload_fingerprint"] if fast_path else _sha256_file(payload)
    )
    return {
        "size": stat.st_size,
        "mtime_ns": stat.st_mtime_ns,
        "payload_fingerprint": byte_hash,
        "fingerprint_fast_path": fast_path,
    }


def _canonical_json(value: object) -> str:
    def nfc(item):
        if isinstance(item, str):
            return unicodedata.normalize("NFC", item)
        if isinstance(item, list):
            return [nfc(child) for child in item]
        if isinstance(item, dict):
            normalized = {nfc(key): nfc(child) for key, child in item.items()}
            if len(normalized) != len(item):
                raise ValueError("keys collide after Unicode NFC")
            return normalized
        return item

    return json.dumps(
        nfc(value), ensure_ascii=False, allow_nan=False, sort_keys=True,
        separators=(",", ":"),
    )


def _parse_passport(copies: tuple[str, ...]) -> tuple[str, dict | None, str | None]:
    if not copies:
        return "legacy_missing_passport", None, "MH_W_LEGACY_PAYLOAD_NO_PASSPORT"
    if any(not isinstance(copy, str) for copy in copies):
        return "malformed", None, "MH_E_PASSPORT_INVALID: carrier is not text"
    if len(set(copies)) != 1:
        return (
            "malformed",
            None,
            "MH_E_PASSPORT_INVALID: MESH carrier copies differ byte-for-byte",
        )
    try:
        document = json.loads(copies[0])
    except (json.JSONDecodeError, UnicodeError) as exc:
        return "malformed", None, f"MH_E_PASSPORT_INVALID: {exc}"
    if not isinstance(document, dict):
        return "malformed", None, "MH_E_PASSPORT_INVALID: root must be an object"
    if document.get("schema") != PASSPORT_SCHEMA:
        return "malformed", None, "MH_E_PASSPORT_INVALID: unknown schema"
    if document.get("schema_version") != PASSPORT_SCHEMA_VERSION:
        return (
            "unsupported_version",
            None,
            "MH_E_PASSPORT_INVALID: unsupported schema_version",
        )
    try:
        resource_uid = str(uuid.UUID(document["resource_uid"]))
    except (KeyError, ValueError, AttributeError, TypeError) as exc:
        return "malformed", None, f"MH_E_PASSPORT_INVALID: resource_uid: {exc}"
    if document.get("kind") != "static_mesh":
        return "malformed", None, "MH_E_PASSPORT_INVALID: kind must be static_mesh"
    if "lod_level" in document:
        return (
            "deprecated_per_lod_passport",
            None,
            "MH_W_DEPRECATED_PER_LOD_PASSPORT_MIGRATION_REQUIRED",
        )
    lod_levels = document.get("lod_levels")
    if (
        not isinstance(lod_levels, list)
        or not lod_levels
        or any(
            isinstance(level, bool) or not isinstance(level, int) or level < 0
            for level in lod_levels
        )
        or lod_levels != list(range(len(lod_levels)))
    ):
        return (
            "malformed",
            None,
            "MH_E_PASSPORT_INVALID: lod_levels must be contiguous [0..N]",
        )
    slots = document.get("material_slots", [])
    if not isinstance(slots, list):
        return "malformed", None, "MH_E_PASSPORT_INVALID: material_slots must be a list"
    for slot in slots:
        if not isinstance(slot, dict):
            return "malformed", None, "MH_E_PASSPORT_INVALID: invalid material slot"
        try:
            str(uuid.UUID(slot["material_uid"]))
        except (KeyError, ValueError, AttributeError, TypeError) as exc:
            return "malformed", None, f"MH_E_PASSPORT_INVALID: material_uid: {exc}"

    document = deepcopy(document)
    document["resource_uid"] = resource_uid
    return "ok", document, None


def _prior_resolved(previous_index: dict | None, uid: str) -> str | None:
    if not previous_index:
        return None
    row = previous_index.get("uids", {}).get(uid, {})
    return row.get("resolved_path")


def _known_previous_uid(previous_index: dict | None, uid: str) -> bool:
    return bool(previous_index and uid in previous_index.get("uids", {}))


def rebuild_index(
    observations: list[PayloadObservation] | tuple[PayloadObservation, ...],
    *,
    previous_index: dict | None = None,
    known_material_uids: set[str] | frozenset[str] = frozenset(),
) -> dict:
    """Rebuild a disposable index and exercise the ADR conflict matrix."""

    known_materials = {str(uuid.UUID(value)) for value in known_material_uids}
    previous_paths = (previous_index or {}).get("paths", {})
    path_rows: dict[str, dict] = {}
    grouped: dict[str, list[str]] = {}
    quarantine: list[str] = []
    legacy: list[str] = []

    for observation in observations:
        key = canonical_path(observation.path)
        if key in path_rows:
            raise ValueError(f"duplicate observation for canonical path: {key}")
        fingerprint = exact_payload_fingerprint(
            observation.path, cached_row=previous_paths.get(key))
        parse_status, passport, diagnostic = _parse_passport(
            observation.passport_copies)
        row = {
            "path": key,
            **fingerprint,
            "parse_status": parse_status,
            "parsed_passport": passport,
            "diagnostics": [diagnostic] if diagnostic else [],
            "quarantined": parse_status in {"malformed", "unsupported_version"},
        }
        if passport is not None:
            descriptor = deepcopy(passport)
            descriptor.pop("descriptor_hash", None)
            row["descriptor_hash"] = "sha256:" + hashlib.sha256(
                _canonical_json(descriptor).encode("utf-8")
            ).hexdigest()
            uid = passport["resource_uid"]
            grouped.setdefault(uid, []).append(key)
        elif row["quarantined"]:
            quarantine.append(key)
        else:
            legacy.append(key)

        old = previous_paths.get(key)
        if (
            old
            and passport is not None
            and old.get("parsed_passport") == passport
            and old.get("payload_fingerprint") != row["payload_fingerprint"]
        ):
            row["diagnostics"].append("MH_W_PAYLOAD_EXTERNAL_MODIFIED")
        path_rows[key] = row

    uid_rows: dict[str, dict] = {}
    provisional_resolutions: list[dict] = []
    for uid, candidate_paths in sorted(grouped.items()):
        issues: list[str] = []
        known_before = _known_previous_uid(previous_index, uid)
        moved = False
        candidates = sorted(candidate_paths, key=lambda value: (value.casefold(), value))
        fingerprints = {
            path_rows[path]["payload_fingerprint"] for path in candidates
        }
        prior_path = _prior_resolved(previous_index, uid)
        chosen: str | None = None
        basis: str | None = None
        if len(candidates) == 1:
            chosen = candidates[0]
            if prior_path and prior_path != chosen and prior_path not in path_rows:
                moved = True
                basis = "single_candidate_after_prior_disappeared"
            else:
                basis = "single_candidate"
            resolution_status = "moved" if moved else (
                "adopt_new" if not known_before else "resolved"
            )
        elif len(fingerprints) == 1:
            if prior_path in candidates:
                chosen = prior_path
                basis = "preserved_prior_resolved_path"
            else:
                chosen = candidates[0]
                basis = "provisional_normalized_path"
                provisional_resolutions.append({
                    "resource_uid": uid,
                    "chosen_path": chosen,
                    "note": PROVISIONAL_EMPTY_REBUILD_NOTE,
                })
            resolution_status = "duplicate_copy_warning"
            issues.append(
                "MH_W_DUPLICATE_IDENTICAL_PAYLOAD: " + ", ".join(candidates)
            )
        else:
            basis = "divergent_revisions"
            resolution_status = "hard_conflict"
            issues.append(
                "MH_E_DIVERGENT_REVISIONS: " + ", ".join(candidates)
            )

        material_uids: set[str] = set()
        for path in candidates:
            passport = path_rows[path]["parsed_passport"]
            material_uids.update(
                str(uuid.UUID(slot["material_uid"]))
                for slot in passport.get("material_slots", [])
            )
        missing_materials = sorted(material_uids - known_materials)
        if missing_materials:
            issues.append(
                "MH_W_MISSING_MATERIAL: " + ", ".join(missing_materials)
            )

        descriptor_passport = path_rows[chosen or candidates[0]]["parsed_passport"]
        uid_rows[uid] = {
            "candidate_paths": candidates,
            "candidate_fingerprints": sorted(fingerprints),
            "resolved_path": chosen,
            "resolution_basis": basis,
            "resolution_status": resolution_status,
            "lod_levels": descriptor_passport["lod_levels"],
            "model_lod_verification": "carrier_reader_responsibility",
            "material_status": "missing" if missing_materials else "complete",
            "missing_material_uids": missing_materials,
            "diagnostics": issues,
        }

    return {
        "schema": INDEX_SCHEMA,
        "schema_version": INDEX_SCHEMA_VERSION,
        "cache_only": True,
        "paths": dict(sorted(path_rows.items())),
        "uids": uid_rows,
        "legacy_paths": sorted(legacy),
        "quarantine_paths": sorted(quarantine),
        "provisional_resolutions": provisional_resolutions,
    }


def fork_document(
    document: dict,
    *,
    new_resource_uid: str | None = None,
    dependency_uid_replacements: dict[str, str] | None = None,
) -> dict:
    """Fork a pure passport/composite document without mutating the original."""

    forked = deepcopy(document)
    replacement_uid = str(uuid.UUID(new_resource_uid or str(uuid.uuid4())))
    identity_key = "resource_uid" if "resource_uid" in forked else "uid"
    if identity_key not in forked:
        raise ValueError("document has no resource_uid or uid")
    forked[identity_key] = replacement_uid
    replacements = {
        str(uuid.UUID(old)): str(uuid.UUID(new))
        for old, new in (dependency_uid_replacements or {}).items()
    }

    def rewrite_nodes(value):
        if isinstance(value, list):
            for child in value:
                rewrite_nodes(child)
        elif isinstance(value, dict):
            if "resource_uid" in value:
                try:
                    current = str(uuid.UUID(value["resource_uid"]))
                except (ValueError, AttributeError, TypeError):
                    current = None
                if current in replacements:
                    value["resource_uid"] = replacements[current]
            for key, child in value.items():
                if key != "resource_uid":
                    rewrite_nodes(child)

    if forked.get("kind") == "composite" or forked.get("schema") == "mh.composite":
        rewrite_nodes(forked.get("nodes", []))
    return forked


def serialize_index(index: dict) -> bytes:
    """Human-readable cache serialization; byte parity across hosts is not required."""

    return (json.dumps(index, ensure_ascii=False, indent=2, sort_keys=True) + "\n").encode(
        "utf-8"
    )


def load_index(path: str | Path) -> dict:
    return json.loads(Path(path).read_text(encoding="utf-8"))
