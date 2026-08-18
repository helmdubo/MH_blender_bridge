"""Source Protocol v2 tests for the self-contained material codec."""

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
    prepare_material_export,
    validate_material_document,
    write_material_payload_atomic,
)
from mh4blend.core import material_source as material_source_module  # noqa: E402


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
    prepared_a = prepare_material_export(
        **{key: first[key] for key in (
            "uid", "name", "shader_class", "params", "textures")},
        source_root=tmp_path, output_dir=tmp_path / "a")
    prepared_b = prepare_material_export(
        **{key: second[key] for key in (
            "uid", "name", "shader_class", "params", "textures")},
        source_root=tmp_path, output_dir=tmp_path / "b")
    assert prepared_a.content_hash == prepared_b.content_hash


def test_first_create_uses_clean_human_filename(tmp_path):
    prepared = prepare_material_export(
        uid=UID, name="My Material", shader_class="rendinst_simple",
        params={}, textures={}, source_root=tmp_path, output_dir=tmp_path)
    assert Path(prepared.payload_path).name == \
        "my_material.material"


def test_uid_resolved_existing_location_can_update_in_place(tmp_path):
    folder = tmp_path / "common" / "materials"
    payload = folder / "old_name.material"
    existing = prepare_material_export(
        uid=UID, name="Old Name", shader_class="rendinst_simple",
        params={}, textures={}, source_root=tmp_path,
        target_payload_path=payload)
    write_material_payload_atomic(existing, source_root=tmp_path)
    prepared = prepare_material_export(
        uid=UID, name="New Name", shader_class="rendinst_simple",
        params={}, textures={}, source_root=tmp_path,
        output_dir=tmp_path / "ignored",
        target_payload_path=payload)
    assert prepared.payload_path == str(payload)
    assert prepared.document["name"] == "New Name"


def test_clean_name_collision_with_other_uid_blocks(tmp_path):
    path = tmp_path / "material.material"
    first = prepare_material_export(
        uid="aaaaaaaa-0000-0000-0000-000000000001", name="Material",
        shader_class="rendinst_simple", params={}, textures={},
        source_root=tmp_path, target_payload_path=path)
    write_material_payload_atomic(first, source_root=tmp_path)
    with pytest.raises(
            MaterialSourceError, match="MH_E_NAME_COLLISION_DIFFERENT_UID"):
        prepare_material_export(
            uid=UID, name="Material", shader_class="rendinst_simple",
            params={}, textures={}, source_root=tmp_path,
            target_payload_path=path)


def test_collision_appearing_after_prepare_is_rejected_under_writer_lock(
        tmp_path):
    path = tmp_path / "material.material"
    ours = prepare_material_export(
        uid=UID, name="Material", shader_class="rendinst_simple",
        params={}, textures={}, source_root=tmp_path,
        target_payload_path=path)
    foreign = prepare_material_export(
        uid="aaaaaaaa-0000-0000-0000-000000000001", name="Material",
        shader_class="rendinst_simple", params={}, textures={},
        source_root=tmp_path, target_payload_path=path)
    write_material_payload_atomic(foreign, source_root=tmp_path)

    with pytest.raises(
            MaterialSourceError, match="MH_E_NAME_COLLISION_DIFFERENT_UID"):
        write_material_payload_atomic(ours, source_root=tmp_path)
    assert json.loads(path.read_text(encoding="utf-8"))["uid"] != UID


def test_explicit_material_export_always_rewrites_atomically(
        tmp_path, monkeypatch):
    prepared = prepare_material_export(
        uid=UID, name="Material", shader_class="rendinst_simple",
        params={}, textures={}, source_root=tmp_path, output_dir=tmp_path)
    calls = []
    original = material_source_module.atomic_publish_bytes

    def tracked(*args, **kwargs):
        calls.append(os.fspath(args[0]))
        return original(*args, **kwargs)

    monkeypatch.setattr(material_source_module, "atomic_publish_bytes", tracked)
    assert write_material_payload_atomic(
        prepared, source_root=tmp_path) is True
    first_stat = os.stat(prepared.payload_path).st_mtime_ns
    assert write_material_payload_atomic(
        prepared, source_root=tmp_path) is True
    assert os.stat(prepared.payload_path).st_mtime_ns >= first_stat
    assert calls == [prepared.payload_path, prepared.payload_path]

    loaded = json.loads(Path(prepared.payload_path).read_text(encoding="utf-8"))
    assert loaded == prepared.document
    assert sorted(path.name for path in tmp_path.iterdir()) == [
        "material.material"]


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
