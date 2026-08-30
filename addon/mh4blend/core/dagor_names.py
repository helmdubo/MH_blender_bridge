"""Case-only projection from external Dagor names to MH resource tokens."""

from __future__ import annotations

import re

from .canonical import validate_resource_name

__all__ = ["project_dagor_resource_name"]


_DAGOR_ASCII_RESOURCE_NAME_RE = re.compile(r"^[A-Za-z0-9_]+$")


def project_dagor_resource_name(name: str) -> str:
    """Return the canonical lowercase MH token for one Dagor name.

    This is an input-adapter rule, not a relaxation of Source Protocol
    identity. Only ASCII letter case is projected; punctuation, whitespace,
    Unicode and every other noncanonical spelling still fail closed.
    """
    if not isinstance(name, str):
        validate_resource_name(name)
    if _DAGOR_ASCII_RESOURCE_NAME_RE.fullmatch(name) is None:
        validate_resource_name(name)
    projected = name.lower()
    validate_resource_name(projected)
    return projected
