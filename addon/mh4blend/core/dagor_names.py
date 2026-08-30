"""Narrow projection from external Dagor names to MH resource tokens."""

from __future__ import annotations

import re

from .canonical import validate_resource_name

__all__ = ["project_dagor_material_name", "project_dagor_resource_name"]


_ASCII_WHITESPACE = " \t\r\n\f\v"
_DAGOR_ASCII_RESOURCE_NAME_RE = re.compile(
    rf"^[A-Za-z0-9_{_ASCII_WHITESPACE}]+$")
_WHITESPACE_NEXT_TO_SEPARATOR_RE = re.compile(
    rf"[{_ASCII_WHITESPACE}]*_[{_ASCII_WHITESPACE}]*")
_WHITESPACE_RE = re.compile(rf"[{_ASCII_WHITESPACE}]+")
_DAGOR_PRINTABLE_ASCII_NAME_RE = re.compile(r"^[\t\r\n\f\v\x20-\x7e]+$")
_SEPARATOR_NEXT_TO_UNDERSCORE_RE = re.compile(
    r"[^A-Za-z0-9_]*_[^A-Za-z0-9_]*", re.ASCII)
_NON_TOKEN_ASCII_RE = re.compile(r"[^A-Za-z0-9_]+", re.ASCII)
_TOKEN_CONTENT_RE = re.compile(r"[A-Za-z0-9_]", re.ASCII)


def project_dagor_material_name(name: str) -> str:
    """Return a canonical MH token for one external Dagor material name.

    This is an input-adapter rule, not a relaxation of Source Protocol
    identity. ASCII letter case is lowered and each run of external ASCII
    separators/punctuation becomes ``_``. Unicode, control characters and
    names without any token content still fail closed. Existing underscores
    are preserved, including repeated ones.
    """
    if not isinstance(name, str):
        validate_resource_name(name)
    stripped = name.strip(_ASCII_WHITESPACE)
    if (_DAGOR_PRINTABLE_ASCII_NAME_RE.fullmatch(name) is None
            or _TOKEN_CONTENT_RE.search(stripped) is None):
        validate_resource_name(name)
    projected = _SEPARATOR_NEXT_TO_UNDERSCORE_RE.sub("_", stripped)
    projected = _NON_TOKEN_ASCII_RE.sub("_", projected).lower()
    validate_resource_name(projected)
    return projected


def project_dagor_resource_name(name: str) -> str:
    """Return the canonical lowercase MH token for one Dagor resource name.

    Resource filenames keep the narrower established contract: only ASCII
    letter case and whitespace separators are projected. Punctuation and
    Unicode fail closed. Existing underscores, including repeats, survive.
    """
    if not isinstance(name, str):
        validate_resource_name(name)
    if _DAGOR_ASCII_RESOURCE_NAME_RE.fullmatch(name) is None:
        validate_resource_name(name)
    projected = name.strip(_ASCII_WHITESPACE)
    projected = _WHITESPACE_NEXT_TO_SEPARATOR_RE.sub("_", projected)
    projected = _WHITESPACE_RE.sub("_", projected).lower()
    validate_resource_name(projected)
    return projected
