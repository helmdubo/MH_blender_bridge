"""Narrow projection from external Dagor names to MH resource tokens."""

from __future__ import annotations

import re

from .canonical import validate_resource_name

__all__ = ["project_dagor_resource_name"]


_ASCII_WHITESPACE = " \t\r\n\f\v"
_DAGOR_ASCII_RESOURCE_NAME_RE = re.compile(
    rf"^[A-Za-z0-9_{_ASCII_WHITESPACE}]+$")
_WHITESPACE_NEXT_TO_SEPARATOR_RE = re.compile(
    rf"[{_ASCII_WHITESPACE}]*_[{_ASCII_WHITESPACE}]*")
_WHITESPACE_RE = re.compile(rf"[{_ASCII_WHITESPACE}]+")


def project_dagor_resource_name(name: str) -> str:
    """Return the canonical lowercase MH token for one Dagor name.

    This is an input-adapter rule, not a relaxation of Source Protocol
    identity. ASCII letter case and whitespace separators are projected;
    punctuation, Unicode and every other noncanonical spelling still fail
    closed. Existing underscores are preserved, including repeated ones.
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
