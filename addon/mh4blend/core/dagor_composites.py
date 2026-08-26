"""Strict, Blender-free reader for Dagor ``*.composit.blk`` sources.

The reader deliberately stops at the lossless conversion boundary.  It keeps
Dagor's source-order hierarchy, typed resource tokens, and raw 3x4 transform
columns; it does not decompose matrices.  Admitted node ``include`` directives
retain exact provenance, while their closed ``p2`` grammar is decoded into
placement-profile DTOs by this bpy-free module.
"""

from __future__ import annotations

from dataclasses import dataclass
import math
from pathlib import Path
import re
from typing import Iterator

from .model import PlacementProfile, PlacementRange
from .placements import parse_placement_profile, placement_json_bytes

__all__ = [
    "DagorComposite",
    "DagorCompositeError",
    "DagorInclude",
    "DagorMatrix3x4",
    "DagorNode",
    "DagorOption",
    "DagorProvenance",
    "DagorResourceToken",
    "iter_resource_tokens",
    "parse_dagor_composite",
    "parse_dagor_placement_include",
    "read_dagor_composite",
]


_GRAMMAR_CODE = "MH_E_COMPOSITE_GRAMMAR"
_SOURCE_CODE = "MH_E_INVALID_RESOURCE_SOURCE"
_IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
_RESOURCE_NAME = re.compile(r"[A-Za-z0-9_]+")
_PROFILE_NAME = re.compile(r"[a-z0-9_]+")
_NUMBER = re.compile(
    r"[+-]?(?:(?:[0-9]+(?:\.[0-9]*)?)|(?:\.[0-9]+))(?:[eE][+-]?[0-9]+)?"
)
_TYPE_TO_KIND = {
    "composit": "composite",
    "rendinst": "mesh",
    "prefab": "mesh",
    "gameobj": "actor",
}


@dataclass(frozen=True)
class DagorProvenance:
    """Exact source location and structural path of one parsed construct."""

    source: str
    line: int
    column: int
    path: str

    def render(self) -> str:
        return f"{self.source}:{self.line}:{self.column} ({self.path})"


class DagorCompositeError(ValueError):
    """A Dagor source cannot be converted without guessing or data loss."""

    def __init__(
        self,
        code: str,
        provenance: DagorProvenance,
        message: str,
    ):
        self.code = code
        self.provenance = provenance
        self.message = message
        super().__init__(f"{code}: {provenance.render()}: {message}")


@dataclass(frozen=True)
class DagorMatrix3x4:
    """Raw Dagor transform as four source-order columns of three numbers."""

    columns: tuple[
        tuple[float, float, float],
        tuple[float, float, float],
        tuple[float, float, float],
        tuple[float, float, float],
    ]
    provenance: DagorProvenance


@dataclass(frozen=True)
class DagorResourceToken:
    """An explicit ``name:type`` Dagor asset identity."""

    raw: str
    name: str
    dagor_type: str
    kind: str
    provenance: DagorProvenance


@dataclass(frozen=True)
class DagorOption:
    resource: DagorResourceToken
    weight: float
    provenance: DagorProvenance


@dataclass(frozen=True)
class DagorInclude:
    """One node-scoped placement include, before filesystem resolution."""

    path: str
    provenance: DagorProvenance


@dataclass(frozen=True)
class DagorNode:
    """One node with interleaved source-order node/ent members preserved."""

    kind: str
    resource: DagorResourceToken | None
    transform: DagorMatrix3x4 | None
    members: tuple[DagorNode | DagorOption, ...]
    provenance: DagorProvenance
    include: DagorInclude | None = None

    @property
    def children(self) -> tuple[DagorNode, ...]:
        return tuple(member for member in self.members if isinstance(member, DagorNode))

    @property
    def options(self) -> tuple[DagorOption, ...]:
        return tuple(member for member in self.members if isinstance(member, DagorOption))


@dataclass(frozen=True)
class DagorComposite:
    name: str
    nodes: tuple[DagorNode, ...]
    provenance: DagorProvenance


@dataclass(frozen=True)
class _Token:
    kind: str
    value: str
    line: int
    column: int


class _Lexer:
    def __init__(self, text: str, source: str):
        self._text = text
        self._source = source
        self._offset = 0
        self._line = 1
        self._column = 1

    def _provenance(self, path: str) -> DagorProvenance:
        return DagorProvenance(self._source, self._line, self._column, path)

    def _error(self, message: str) -> DagorCompositeError:
        return DagorCompositeError(_GRAMMAR_CODE, self._provenance("$"), message)

    def _advance(self) -> str:
        character = self._text[self._offset]
        self._offset += 1
        if character == "\n":
            self._line += 1
            self._column = 1
        else:
            self._column += 1
        return character

    def _skip_trivia(self) -> None:
        while self._offset < len(self._text):
            if self._text[self._offset].isspace():
                self._advance()
                continue
            if self._text.startswith("//", self._offset):
                while self._offset < len(self._text) and self._advance() != "\n":
                    pass
                continue
            if self._text.startswith("/*", self._offset):
                start = self._provenance("$")
                self._advance()
                self._advance()
                while (
                    self._offset < len(self._text)
                    and not self._text.startswith("*/", self._offset)
                ):
                    self._advance()
                if self._offset == len(self._text):
                    raise DagorCompositeError(
                        _GRAMMAR_CODE, start, "unterminated block comment")
                self._advance()
                self._advance()
                continue
            return

    def tokens(self) -> tuple[_Token, ...]:
        tokens: list[_Token] = []
        punctuation = frozenset("{}[]=:,;")
        while True:
            self._skip_trivia()
            if self._offset == len(self._text):
                tokens.append(_Token("EOF", "", self._line, self._column))
                return tuple(tokens)
            line, column = self._line, self._column
            character = self._text[self._offset]
            if character in punctuation:
                tokens.append(_Token(character, self._advance(), line, column))
                continue
            if character == '"':
                tokens.append(_Token("STRING", self._string(), line, column))
                continue
            identifier = _IDENTIFIER.match(self._text, self._offset)
            if identifier is not None:
                value = identifier.group(0)
                for _ in value:
                    self._advance()
                tokens.append(_Token("IDENT", value, line, column))
                continue
            number = _NUMBER.match(self._text, self._offset)
            if number is not None:
                value = number.group(0)
                for _ in value:
                    self._advance()
                tokens.append(_Token("NUMBER", value, line, column))
                continue
            raise self._error(f"unexpected character {character!r}")

    def _string(self) -> str:
        self._advance()
        characters: list[str] = []
        escapes = {'"': '"', "\\": "\\", "n": "\n", "r": "\r", "t": "\t"}
        while self._offset < len(self._text):
            character = self._advance()
            if character == '"':
                return "".join(characters)
            if character == "\n":
                raise self._error("newline in string literal")
            if character != "\\":
                characters.append(character)
                continue
            if self._offset == len(self._text):
                break
            escaped = self._advance()
            if escaped not in escapes:
                raise self._error(f"unsupported string escape \\{escaped}")
            characters.append(escapes[escaped])
        raise self._error("unterminated string literal")


class _Parser:
    def __init__(self, tokens: tuple[_Token, ...], source: str, name: str):
        self._tokens = tokens
        self._source = source
        self._name = name
        self._cursor = 0

    def _peek(self, kind: str | None = None) -> _Token | bool:
        token = self._tokens[self._cursor]
        return token if kind is None else token.kind == kind

    def _take(self, kind: str, path: str, message: str | None = None) -> _Token:
        token = self._tokens[self._cursor]
        if token.kind != kind:
            raise self._error(token, path, message or f"expected {kind}, got {token.kind}")
        self._cursor += 1
        return token

    def _accept(self, kind: str) -> _Token | None:
        if self._peek(kind):
            token = self._tokens[self._cursor]
            self._cursor += 1
            return token
        return None

    def _provenance(self, token: _Token, path: str) -> DagorProvenance:
        return DagorProvenance(self._source, token.line, token.column, path)

    def _error(self, token: _Token, path: str, message: str) -> DagorCompositeError:
        return DagorCompositeError(_GRAMMAR_CODE, self._provenance(token, path), message)

    def _lossless_stop(self, token: _Token, path: str, construct: str) -> DagorCompositeError:
        return self._error(
            token,
            path,
            f"lossless conversion error: {construct} has no admitted "
            "placement-profile carrier",
        )

    def parse(self) -> DagorComposite:
        class_token: _Token | None = None
        nodes: list[DagorNode] = []
        while not self._peek("EOF"):
            self._accept(";")
            if self._peek("EOF"):
                break
            key = self._take("IDENT", "$", "expected root statement")
            folded = key.value.casefold()
            if folded == "include":
                raise self._lossless_stop(key, "$", "root Dagor include")
            if folded == "node" and self._peek("{"):
                nodes.append(self._node(key, f"$.nodes[{len(nodes)}]"))
                continue
            if self._next_type_is("p2"):
                raise self._lossless_stop(key, f"$.{key.value}", "Dagor p2 range")
            if key.value != "className":
                raise self._error(key, "$", f"unsupported root construct {key.value!r}")
            if class_token is not None:
                raise self._error(key, "$.className", "duplicate className")
            value = self._typed_string(key, "t", "$.className")
            if value.casefold() != "composit":
                raise self._error(key, "$.className", "must equal 'composit'")
            class_token = key
            self._accept(";")
        eof = self._take("EOF", "$")
        if class_token is None:
            raise self._error(eof, "$.className", "missing className:t='composit'")
        return DagorComposite(
            self._name,
            tuple(nodes),
            self._provenance(class_token, "$"),
        )

    def _node(self, start: _Token, path: str) -> DagorNode:
        self._take("{", path)
        resource: DagorResourceToken | None = None
        transform: DagorMatrix3x4 | None = None
        include: DagorInclude | None = None
        members: list[DagorNode | DagorOption] = []
        while not self._peek("}"):
            self._accept(";")
            if self._peek("}"):
                break
            if self._peek("EOF"):
                raise self._error(start, path, "unterminated node block")
            key = self._take("IDENT", path, "expected node statement")
            folded = key.value.casefold()
            if folded == "include":
                if include is not None:
                    raise self._error(
                        key, f"{path}.profile", "duplicate Dagor include")
                include_path = self._take(
                    "STRING", f"{path}.profile",
                    "Dagor include requires one quoted path").value
                if not include_path:
                    raise self._error(
                        key, f"{path}.profile", "Dagor include path is empty")
                include = DagorInclude(
                    include_path,
                    self._provenance(key, f"{path}.profile"),
                )
                self._accept(";")
                continue
            if folded == "node" and self._peek("{"):
                child_index = sum(isinstance(item, DagorNode) for item in members)
                members.append(self._node(key, f"{path}.children[{child_index}]"))
                continue
            if folded == "ent" and self._peek("{"):
                option_index = sum(isinstance(item, DagorOption) for item in members)
                members.append(self._option(key, f"{path}.options[{option_index}]"))
                continue
            if key.value == "name":
                if resource is not None:
                    raise self._error(key, f"{path}.name", "duplicate name")
                value = self._typed_string(key, "t", f"{path}.name")
                resource = self._resource(value, key, f"{path}.name")
                self._accept(";")
                continue
            if key.value == "tm":
                if transform is not None:
                    raise self._error(key, f"{path}.tm", "duplicate tm")
                self._typed_prefix(key, "m", f"{path}.tm")
                transform = self._matrix(key, f"{path}.tm")
                self._accept(";")
                continue
            if self._next_type_is("p2"):
                raise self._lossless_stop(key, f"{path}.{key.value}", "Dagor p2 range")
            raise self._error(key, path, f"unsupported node construct {key.value!r}")
        self._take("}", path)
        self._accept(";")
        options = tuple(item for item in members if isinstance(item, DagorOption))
        if resource is not None and options:
            raise self._error(
                start, path, "resource node cannot also contain random options")
        if options:
            try:
                total_weight = math.fsum(option.weight for option in options)
            except OverflowError as exc:
                raise self._error(
                    start, f"{path}.options", "random option total must be finite") from exc
            if not math.isfinite(total_weight):
                raise self._error(
                    start, f"{path}.options", "random option total must be finite")
            if total_weight <= 0.0:
                raise self._error(
                    start, f"{path}.options", "random option total must be positive")
        kind = "random" if options else (resource.kind if resource is not None else "group")
        return DagorNode(
            kind, resource, transform, tuple(members),
            self._provenance(start, path), include)

    def _option(self, start: _Token, path: str) -> DagorOption:
        self._take("{", path)
        resource: DagorResourceToken | None = None
        weight = 1.0
        saw_weight = False
        while not self._peek("}"):
            self._accept(";")
            if self._peek("}"):
                break
            if self._peek("EOF"):
                raise self._error(start, path, "unterminated ent block")
            key = self._take("IDENT", path, "expected ent statement")
            if key.value.casefold() == "include":
                raise self._lossless_stop(
                    key, path, "random-option Dagor include")
            if key.value == "name":
                if resource is not None:
                    raise self._error(key, f"{path}.name", "duplicate name")
                value = self._typed_string(key, "t", f"{path}.name")
                resource = self._resource(value, key, f"{path}.name")
            elif key.value == "weight":
                if saw_weight:
                    raise self._error(key, f"{path}.weight", "duplicate weight")
                self._typed_prefix(key, "r", f"{path}.weight")
                number = self._take("NUMBER", f"{path}.weight", "weight must be a number")
                weight = self._finite(number, f"{path}.weight")
                if weight < 0.0:
                    raise self._error(number, f"{path}.weight", "weight must be non-negative")
                saw_weight = True
            elif self._next_type_is("p2"):
                raise self._lossless_stop(key, f"{path}.{key.value}", "Dagor p2 range")
            else:
                raise self._error(key, path, f"unsupported ent construct {key.value!r}")
            self._accept(";")
        self._take("}", path)
        self._accept(";")
        if resource is None:
            raise self._error(start, f"{path}.name", "ent requires explicit name:type")
        return DagorOption(resource, weight, self._provenance(start, path))

    def _typed_prefix(self, key: _Token, expected_type: str, path: str) -> None:
        self._take(":", path)
        type_token = self._take("IDENT", path, "missing Dagor field type")
        if type_token.value.casefold() == "p2":
            raise self._lossless_stop(key, path, "Dagor p2 range")
        if type_token.value.casefold() != expected_type.casefold():
            raise self._error(
                type_token,
                path,
                f"field {key.value!r} requires :{expected_type}, got :{type_token.value}",
            )
        self._take("=", path)

    def _next_type_is(self, expected_type: str) -> bool:
        return (
            bool(self._peek(":"))
            and self._cursor + 1 < len(self._tokens)
            and self._tokens[self._cursor + 1].kind == "IDENT"
            and self._tokens[self._cursor + 1].value.casefold()
            == expected_type.casefold()
        )

    def _typed_string(self, key: _Token, expected_type: str, path: str) -> str:
        self._typed_prefix(key, expected_type, path)
        return self._take("STRING", path, "field value must be a string").value

    def _resource(self, raw: str, token: _Token, path: str) -> DagorResourceToken:
        if raw.count(":") != 1:
            raise self._error(token, path, "resource requires explicit name:type token")
        name, type_name = raw.split(":", 1)
        if _RESOURCE_NAME.fullmatch(name) is None:
            raise self._error(token, path, "resource name must match [A-Za-z0-9_]+")
        normalized_type = type_name.casefold()
        kind = _TYPE_TO_KIND.get(normalized_type)
        if kind is None:
            raise self._error(token, path, f"unsupported Dagor resource type {type_name!r}")
        return DagorResourceToken(
            raw,
            name,
            normalized_type,
            kind,
            self._provenance(token, path),
        )

    def _finite(self, token: _Token, path: str) -> float:
        try:
            value = float(token.value)
        except ValueError as exc:
            raise self._error(token, path, "invalid number") from exc
        if not math.isfinite(value):
            raise self._error(token, path, "number must be finite")
        return value

    def _matrix(self, start: _Token, path: str) -> DagorMatrix3x4:
        self._take("[", path, "tm must be a 3x4 matrix")
        columns: list[tuple[float, float, float]] = []
        for column_index in range(4):
            self._accept(",")
            self._take("[", path, "tm must contain four [x, y, z] columns")
            values: list[float] = []
            for component_index in range(3):
                if component_index:
                    self._take(",", path, "tm column components require commas")
                number = self._take(
                    "NUMBER",
                    f"{path}[{column_index}][{component_index}]",
                    "tm component must be a number",
                )
                values.append(self._finite(
                    number, f"{path}[{column_index}][{component_index}]"))
            self._take("]", path, "tm column must contain exactly three numbers")
            columns.append((values[0], values[1], values[2]))
        self._take("]", path, "tm must contain exactly four columns")
        return DagorMatrix3x4(
            (columns[0], columns[1], columns[2], columns[3]),
            self._provenance(start, path),
        )


class _PlacementIncludeParser(_Parser):
    """Closed reader for the owner-admitted Dagor ``p2`` subset."""

    _FIELDS = frozenset({
        "offset_x", "offset_y", "offset_z",
        "rot_x", "rot_y", "rot_z",
        "scale", "yScale",
    })

    def parse_profile(self) -> PlacementProfile:
        values: dict[str, PlacementRange] = {}
        provenance: dict[str, DagorProvenance] = {}
        while not self._peek("EOF"):
            self._accept(";")
            if self._peek("EOF"):
                break
            key = self._take("IDENT", "$", "expected Dagor p2 assignment")
            path = f"$.{key.value}"
            if key.value not in self._FIELDS:
                raise self._error(
                    key, path,
                    f"unsupported Dagor placement parameter {key.value!r}")
            if key.value in values:
                raise self._error(key, path, "duplicate Dagor placement parameter")
            self._take(":", path)
            type_token = self._take(
                "IDENT", path, "missing Dagor placement parameter type")
            if type_token.value.casefold() != "p2":
                raise self._error(
                    type_token, path,
                    f"placement parameter {key.value!r} requires :p2")
            self._take("=", path)
            bracketed = self._accept("[") is not None
            base_token = self._take(
                "NUMBER", f"{path}[0]", "p2 base must be a number")
            self._take(",", path, "p2 requires base,deviation")
            deviation_token = self._take(
                "NUMBER", f"{path}[1]", "p2 deviation must be a number")
            if bracketed:
                self._take("]", path, "bracketed p2 requires closing ]")
            values[key.value] = PlacementRange(
                self._finite(base_token, f"{path}[0]"),
                self._finite(deviation_token, f"{path}[1]"),
            )
            provenance[key.value] = self._provenance(key, path)
            self._accept(";")
        self._take("EOF", "$")

        def triple(prefix: str):
            names = tuple(f"{prefix}_{axis}" for axis in "xyz")
            present = tuple(name in values for name in names)
            if any(present) and not all(present):
                first = next(name for name in names if name in values)
                raise DagorCompositeError(
                    _GRAMMAR_CODE,
                    provenance[first],
                    f"lossless conversion requires the complete {prefix}_x/"
                    f"{prefix}_y/{prefix}_z p2 triple",
                )
            return tuple(values[name] for name in names) if all(present) else None

        profile = PlacementProfile(
            self._name,
            offset_cm=triple("offset"),
            rotation_deg=triple("rot"),
            uniform_scale=values.get("scale"),
            vertical_scale=values.get("yScale"),
        )
        try:
            # Round-trip through the normative codec once: this both validates
            # scale ranges and fixes the returned values to canonical float32.
            return parse_placement_profile(
                placement_json_bytes(profile), name=self._name)
        except ValueError as exc:
            token = next(iter(provenance.values()), DagorProvenance(
                self._source, 1, 1, "$"))
            code = getattr(exc, "code", None) or "MH_E_PLACEMENT_PROFILE_GRAMMAR"
            raise DagorCompositeError(
                code, token,
                f"Dagor placement include cannot be represented losslessly: {exc}",
            ) from exc


def parse_dagor_placement_include(
    payload: bytes | str,
    *,
    source: str = "<memory>",
    name: str,
) -> PlacementProfile:
    """Parse one admitted Dagor include into a canonical placement profile."""

    if _PROFILE_NAME.fullmatch(name) is None:
        raise DagorCompositeError(
            "MH_E_NONCANONICAL_RESOURCE_NAME",
            DagorProvenance(source, 1, 1, "$"),
            "Dagor include stem must match [a-z0-9_]+ exactly",
        )
    if isinstance(payload, bytes):
        try:
            text = payload.decode("utf-8-sig")
        except UnicodeDecodeError as exc:
            raise DagorCompositeError(
                _SOURCE_CODE,
                DagorProvenance(source, 1, 1, "$"),
                "Dagor placement include must be UTF-8",
            ) from exc
    elif isinstance(payload, str):
        text = payload
    else:
        raise TypeError("Dagor placement include payload must be bytes or str")
    tokens = _Lexer(text, source).tokens()
    return _PlacementIncludeParser(tokens, source, name).parse_profile()


def parse_dagor_composite(
    payload: bytes | str,
    *,
    source: str = "<memory>",
    name: str = "",
) -> DagorComposite:
    """Parse one source payload into a closed, immutable Dagor graph."""

    if isinstance(payload, bytes):
        try:
            text = payload.decode("utf-8-sig")
        except UnicodeDecodeError as exc:
            provenance = DagorProvenance(source, 1, 1, "$")
            raise DagorCompositeError(
                _SOURCE_CODE, provenance, "Dagor composite must be UTF-8") from exc
    elif isinstance(payload, str):
        text = payload.removeprefix("\ufeff")
    else:
        provenance = DagorProvenance(source, 1, 1, "$")
        raise DagorCompositeError(
            _SOURCE_CODE, provenance, "Dagor composite payload must be bytes or str")
    tokens = _Lexer(text, source).tokens()
    return _Parser(tokens, source, name).parse()


def read_dagor_composite(path: str | Path) -> DagorComposite:
    """Read one exact-lowercase ``*.composit.blk`` source file."""

    source = Path(path)
    if not source.name.endswith(".composit.blk"):
        provenance = DagorProvenance(str(source), 1, 1, "$")
        raise DagorCompositeError(
            _SOURCE_CODE,
            provenance,
            "Dagor composite filename must end with exact .composit.blk",
        )
    name = source.name.removesuffix(".composit.blk")
    try:
        payload = source.read_bytes()
    except OSError as exc:
        provenance = DagorProvenance(str(source), 1, 1, "$")
        raise DagorCompositeError(_SOURCE_CODE, provenance, str(exc)) from exc
    return parse_dagor_composite(payload, source=str(source), name=name)


def iter_resource_tokens(composite: DagorComposite) -> Iterator[DagorResourceToken]:
    """Yield the full source frontier in depth-first source order."""

    def walk(node: DagorNode) -> Iterator[DagorResourceToken]:
        if node.resource is not None:
            yield node.resource
        for member in node.members:
            if isinstance(member, DagorOption):
                yield member.resource
            else:
                yield from walk(member)

    for root in composite.nodes:
        yield from walk(root)
