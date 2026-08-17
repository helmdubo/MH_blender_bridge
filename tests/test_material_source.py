"""Frozen Source Schema v1 tests for the Blender-free material codec."""

import json
import os
from pathlib import Path
import sys

import pytest

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "addon"))

from mh4blend.core.material_source import (  # noqa: E402
    MaterialSourceError,
    build_material_document,
    canonicalize_texture_path,
    material_payload_bytes,
    material_resource_row,
    prepare_material_export,
    stage_material_payload,
    validate_material_document,
    validate_material_resource_row,
    write_material_payload_atomic,
)


UID = "7d995e54-d084-4466-a613-a1cd8f3248b2"


def build(root, **overrides):
    values = {
        "uid": UID,
        "name": "m_stucco_concrete",
        "shader_class": "rendinst_simple",
        "params": {"roughness": 0.2500004, "sides": 0},
        "textures": {},
        "source_root": root,
    }
    values.update(overrides)
    return build_material_document(**values)


def test_writer_emits_exact_v1_shape_and_quantized_payload(tmp_path):
    document, diagnostics = build(tmp_path)
    assert list(document) == [
        "schema", "schema_version", "uid", "name", "shader_class",
        "params", "textures",
    ]
    assert document["schema"] == "mh.material"
    assert document["schema_version"] == 1
    assert document["params"] == {"roughness": 0.25, "sides": 0}
    assert diagnostics == ()
    assert material_payload_bytes(document).endswith(b"\n")
    assert b"\r\n" not in material_payload_bytes(document)


def test_absolute_texture_inside_root_becomes_portable_relative(tmp_path):
    texture = tmp_path / "textures" / "wall_d.tif"
    document, diagnostics = build(
        tmp_path, textures={"tex0": str(texture)})
    assert document["textures"] == {"tex0": "textures/wall_d.tif"}
    assert diagnostics == ()


def test_relative_escape_becomes_external_absolute_and_warns(tmp_path):
    document, diagnostics = build(
        tmp_path, textures={"tex2": "../library/wall_n.tif"})
    assert os.path.isabs(document["textures"]["tex2"])
    assert document["textures"]["tex2"].endswith("/library/wall_n.tif")
    assert [row.code for row in diagnostics] == [
        "MH_W_TEXTURE_OUTSIDE_ROOT"]


def test_external_texture_is_error_under_strict_policy(tmp_path):
    outside = tmp_path.parent / "library" / "wall_n.tif"
    with pytest.raises(
            MaterialSourceError, match="MH_E_TEXTURE_OUTSIDE_ROOT"):
        build(tmp_path, textures={"tex2": str(outside)},
              texture_policy="strict")


@pytest.mark.parametrize("slot", ["tex16", "albedo", "Tex0", "tex-1"])
def test_texture_slot_vocabulary_is_exact(tmp_path, slot):
    with pytest.raises(MaterialSourceError, match="tex0 through tex15"):
        build(tmp_path, textures={slot: "textures/a.tif"})


def test_writer_omits_empty_authored_slots_but_reader_rejects_them(tmp_path):
    document, _ = build(tmp_path, textures={"tex0": ""})
    assert document["textures"] == {}
    document["textures"] = {"tex0": ""}
    with pytest.raises(MaterialSourceError, match="non-empty"):
        validate_material_document(document, source_root=tmp_path)


def test_reader_rejects_unknown_top_level_field(tmp_path):
    document, _ = build(tmp_path)
    document["external_path"] = False
    with pytest.raises(MaterialSourceError, match="unknown field"):
        validate_material_document(document, source_root=tmp_path)


@pytest.mark.parametrize("path", [
    "./textures/a.tif",
    "textures/../a.tif",
    r"textures\a.tif",
])
def test_reader_does_not_repair_malformed_relative_texture_paths(
        tmp_path, path):
    document, _ = build(tmp_path)
    document["textures"] = {"tex0": path}
    with pytest.raises(MaterialSourceError, match="texture path"):
        validate_material_document(document, source_root=tmp_path)


def test_reader_rejects_absolute_inside_root_instead_of_storing_alias(tmp_path):
    document, _ = build(tmp_path)
    document["textures"] = {
        "tex0": str(tmp_path / "textures" / "a.tif").replace("\\", "/")}
    with pytest.raises(MaterialSourceError, match="must be relative"):
        validate_material_document(document, source_root=tmp_path)


def test_material_hash_excludes_uid_and_name(tmp_path):
    first, _ = build(tmp_path)
    second, _ = build(
        tmp_path,
        uid="aaaaaaaa-0000-0000-0000-000000000001",
        name="renamed_material",
    )
    row_a = material_resource_row(first, f"m__{UID[:8]}.material")
    row_b = material_resource_row(
        second, "m__aaaaaaaa.material")
    assert row_a["content_hash"] == row_b["content_hash"]


def test_material_row_is_strict_and_contains_no_payload_fields(tmp_path):
    document, _ = build(tmp_path)
    source = f"materials/old_display_name__{UID[:8]}.material"
    row = material_resource_row(document, source)
    assert row == {
        "uid": UID,
        "kind": "material",
        "name": "m_stucco_concrete",
        "source": source,
        "content_hash": row["content_hash"],
    }
    assert validate_material_resource_row(row) == row
    malformed = dict(row, params={})
    with pytest.raises(MaterialSourceError, match="exactly"):
        validate_material_resource_row(malformed)


def test_first_create_uses_uid8_and_explicit_owner(tmp_path):
    prepared = prepare_material_export(
        uid=UID, name="My Material", shader_class="rendinst_simple",
        params={}, textures={}, source_root=tmp_path, output_dir=tmp_path)
    assert Path(prepared.payload_path).name == \
        f"my_material__{UID[:8]}.material"
    assert Path(prepared.owning_manifest_path).name == "export_manifest.json"
    assert prepared.resource_row["source"] == Path(prepared.payload_path).name


def test_existing_owner_location_and_stale_display_filename_win(tmp_path):
    folder = tmp_path / "common" / "materials"
    payload = folder / f"old_name__{UID[:8]}.material"
    manifest = tmp_path / "common" / "export_manifest.json"
    prepared = prepare_material_export(
        uid=UID, name="New Name", shader_class="rendinst_simple",
        params={}, textures={}, source_root=tmp_path,
        output_dir=tmp_path / "ignored",
        target_payload_path=payload,
        owning_manifest_path=manifest,
        existing_source=f"materials/{payload.name}")
    assert prepared.payload_path == str(payload)
    assert prepared.resource_row["source"] == f"materials/{payload.name}"
    assert prepared.document["name"] == "New Name"


def test_existing_target_requires_owning_manifest(tmp_path):
    with pytest.raises(ValueError, match="owning_manifest_path"):
        prepare_material_export(
            uid=UID, name="Material", shader_class="rendinst_simple",
            params={}, textures={}, source_root=tmp_path,
            target_payload_path=tmp_path / f"m__{UID[:8]}.material")


def test_payload_atomic_write_and_semantic_skip(tmp_path):
    prepared = prepare_material_export(
        uid=UID, name="Material", shader_class="rendinst_simple",
        params={}, textures={}, source_root=tmp_path, output_dir=tmp_path)
    assert write_material_payload_atomic(
        prepared, source_root=tmp_path) is True
    first_stat = os.stat(prepared.payload_path).st_mtime_ns
    assert write_material_payload_atomic(
        prepared, source_root=tmp_path) is False
    assert os.stat(prepared.payload_path).st_mtime_ns == first_stat

    loaded = json.loads(Path(prepared.payload_path).read_text(encoding="utf-8"))
    assert loaded == prepared.document


def test_staging_does_not_replace_stable_payload(tmp_path):
    prepared = prepare_material_export(
        uid=UID, name="Material", shader_class="rendinst_simple",
        params={}, textures={}, source_root=tmp_path, output_dir=tmp_path)
    Path(prepared.payload_path).write_text("stable", encoding="utf-8")
    staged = stage_material_payload(prepared)
    assert Path(prepared.payload_path).read_text(encoding="utf-8") == "stable"
    assert Path(staged).exists()


def test_windows_lexical_paths_are_machine_independent():
    internal, warnings = canonicalize_texture_path(
        r"c:\Project\Assets\textures\.\wall_d.tif",
        r"C:\Project\Assets")
    assert internal == "textures/wall_d.tif"
    assert warnings == ()

    external, warnings = canonicalize_texture_path(
        r"d:\DagorLibrary\wall_n.tif", r"C:\Project\Assets")
    assert external == "D:/DagorLibrary/wall_n.tif"
    assert warnings[0].code == "MH_W_TEXTURE_OUTSIDE_ROOT"


def test_unc_paths_preserve_unc_and_compare_case_insensitively():
    internal, warnings = canonicalize_texture_path(
        "//SERVER/Share/Project/textures/a.tif",
        "//server/share/project")
    assert internal == "textures/a.tif"
    assert warnings == ()

    external, warnings = canonicalize_texture_path(
        "//server/share/library/a.tif", "//server/share/project")
    assert external == "//server/share/library/a.tif"
    assert warnings[0].code == "MH_W_TEXTURE_OUTSIDE_ROOT"
