"""UID subsystem (schema §4, §4.1): lazy assignment, duplicate detection,
manifest-based arbitration.

Carriers are anything with dict-style custom-prop access (`.get(key)`,
`[key] = value`) — Blender ID datablocks qualify, so do plain dicts in
tests. The module stays bpy-free; the extraction layer (B8) feeds it real
datablocks.
"""

import uuid

PROP_UID = "mh_uid"

__all__ = [
    "PROP_UID",
    "ensure_uid",
    "peek_uid",
    "find_duplicate_uids",
    "ArbitrationResult",
    "arbitrate_resource_duplicates",
]


def peek_uid(carrier):
    """Return the carrier's UID or None (no assignment)."""
    value = carrier.get(PROP_UID)
    return value if isinstance(value, str) and value else None


def ensure_uid(carrier):
    """Lazy assignment (§4): return the existing UID or assign a fresh
    UUID4 (lowercase, dashes). Never overwrites an existing value."""
    existing = peek_uid(carrier)
    if existing is not None:
        return existing
    fresh = str(uuid.uuid4())
    carrier[PROP_UID] = fresh
    return fresh


def find_duplicate_uids(carriers):
    """Map uid -> list of carriers for every UID held by more than one
    carrier. Carriers without a UID are ignored (lazy assignment will
    handle them)."""
    by_uid = {}
    for carrier in carriers:
        uid = peek_uid(carrier)
        if uid is not None:
            by_uid.setdefault(uid, []).append(carrier)
    return {uid: owners for uid, owners in by_uid.items() if len(owners) > 1}


class ArbitrationResult:
    """Outcome of §4.1 resource-UID arbitration.

    resolved: True -> `keeper` retains the UID, `reassign` get fresh ones
              (the caller performs the reassignment via ensure_uid after
              clearing, or assigns explicit UIDs — test tooling does).
    resolved: False -> manual choice required ('which one is the
              original'); `candidates` lists the contenders.
    """

    def __init__(self, resolved, keeper=None, reassign=(), candidates=()):
        self.resolved = resolved
        self.keeper = keeper
        self.reassign = list(reassign)
        self.candidates = list(candidates)


def arbitrate_resource_duplicates(uid, owners, manifest_hash, hash_of):
    """§4.1: decide which duplicate keeps a resource UID.

    uid            duplicated resource uid
    owners         carriers sharing it (>= 2)
    manifest_hash  content_hash recorded for this uid in the LAST export
                   manifest, or None when no manifest exists
    hash_of        callable(carrier) -> current content_hash

    The owner whose current hash equals the recorded one is the legitimate
    keeper; everyone else is reassigned. No manifest, or nobody matches
    (both changed), or several match identically -> manual choice.
    """
    if manifest_hash is None:
        return ArbitrationResult(False, candidates=owners)
    matches = [o for o in owners if hash_of(o) == manifest_hash]
    if len(matches) == 1:
        keeper = matches[0]
        return ArbitrationResult(
            True, keeper=keeper,
            reassign=[o for o in owners if o is not keeper])
    return ArbitrationResult(False, candidates=owners)
