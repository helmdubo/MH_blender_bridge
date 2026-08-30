"""Read-only parser for dag4blend ``.proxymat.blk`` material sources."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import re

__all__ = ["ProxymatSource", "parse_proxymat_text", "read_proxymat"]


_SCRIPT_RE = re.compile(r'^script:t="([^"=]+)=([^"=]*)"')
_TEXTURE_RE = re.compile(r"^(tex(?:[0-9]|1[0-5])):t=(.*)$")


@dataclass(frozen=True)
class ProxymatSource:
    material_class: str
    twosided: bool
    textures: dict[str, str]
    params: dict[str, object]
    macro_textures: dict[str, str]


def _guess_type(value: str):
    """Mirror dag4blend ``guess_type_convert`` for script values."""
    if "[" in value:
        # Dagor matrices are not representable by the frozen material grammar.
        # Keep the original string so the adapter rejects it fail-closed.
        return value
    if value.lower() in ("yes", "true"):
        return True
    if value.lower() in ("no", "false"):
        return False
    if "," in value:
        constructor = float if "." in value else int
        try:
            return [constructor(component) for component in value.split(",")]
        except ValueError:
            return value
    if "." in value:
        try:
            return float(value)
        except ValueError:
            return value
    try:
        return int(value)
    except ValueError:
        return value.replace('"', "")


def parse_proxymat_text(text: str) -> ProxymatSource:
    """Parse the subset consumed by dag4blend's ``dagormat_from_text``.

    ``read_proxy_blk`` removes ASCII spaces from every source line before
    parsing. Repeated script properties replace the existing custom property,
    therefore the final occurrence is authoritative.
    """
    material_class = ""
    twosided = False
    textures = {}
    macro_textures = {}
    params = {}

    for source_line in text.splitlines():
        line = source_line.replace(" ", "")
        plain = line.replace('"', "")
        if plain == "twosided:b=yes":
            twosided = True
        elif plain.startswith("class:t="):
            material_class = plain.replace("class:t=", "", 1)
        else:
            texture = _TEXTURE_RE.fullmatch(plain)
            if texture is not None:
                slot, value = texture.groups()
                if "$(ASSET_NAME)" in value:
                    macro_textures[slot] = value
                elif value:
                    textures[slot] = value

        script = _SCRIPT_RE.match(line)
        if script is not None:
            name, value = script.groups()
            params[name] = _guess_type(value)

    return ProxymatSource(
        material_class=material_class,
        twosided=twosided,
        textures=textures,
        params=params,
        macro_textures=macro_textures,
    )


def read_proxymat(path: str | Path) -> ProxymatSource:
    """Read one proxymat without updating Blender or dagormat state."""
    return parse_proxymat_text(Path(path).read_text(encoding="utf-8"))
