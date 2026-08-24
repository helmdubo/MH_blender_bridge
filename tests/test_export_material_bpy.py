"""Blender gates for Source Protocol v4 material extraction/publication."""

from pathlib import Path
import importlib
import sys
from types import SimpleNamespace

import pytest

bpy = pytest.importorskip("bpy")
REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "addon"))

from mh4blend.core.materials import MaterialValueError  # noqa: E402
from mh4blend.scene.export_material import (  # noqa: E402
    apply_material_resource,
    material_class_for_export,
    prepare_blender_material_export,
    read_material_file,
    write_prepared_material,
)
from mh4blend.scene.export_fbx import export_fbx_collection  # noqa: E402
from mh4blend.core.model import MaterialResource  # noqa: E402
from mh4blend.ui import ops  # noqa: E402

export_fbx_module = importlib.import_module("mh4blend.scene.export_fbx")


@pytest.fixture(autouse=True)
def registered_material_properties():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    owned = not hasattr(bpy.types.Material, "mh4blend")
    if owned:
        ops.register()
    try:
        yield
    finally:
        if owned:
            ops.unregister()


def _class_material(name="wall"):
    material = bpy.data.materials.new(name)
    material.mh4blend.mode = "CLASS"
    material.mh4blend.material_class = "simple"
    return material


def _mesh(name, collection, material):
    mesh = bpy.data.meshes.new(name + "Mesh")
    mesh.from_pydata([(0, 0, 0), (1, 0, 0), (0, 1, 0)], [], [(0, 1, 2)])
    mesh.materials.append(material)
    obj = bpy.data.objects.new(name, mesh)
    collection.objects.link(obj)
    return obj


def test_material_class_uses_dagor_shader_when_v4_override_is_empty():
    material = SimpleNamespace(
        mh4blend=SimpleNamespace(material_class=""),
        dagormat=SimpleNamespace(shader_class="rendinst_simple_glass"),
    )
    assert material_class_for_export(material) == "rendinst_simple_glass"


def test_material_class_explicit_v4_override_wins_over_dagor_shader():
    material = SimpleNamespace(
        mh4blend=SimpleNamespace(material_class="rendinst_layered"),
        dagormat=SimpleNamespace(shader_class="rendinst_simple_glass"),
    )
    assert material_class_for_export(material) == "rendinst_layered"


@pytest.mark.parametrize("dagor_value", [None, "", "None"])
def test_material_class_treats_dagor_unset_sentinels_as_missing(dagor_value):
    material = SimpleNamespace(
        mh4blend=SimpleNamespace(material_class=""),
        dagormat=SimpleNamespace(shader_class=dagor_value),
    )
    assert material_class_for_export(material) == ""


def test_class_material_extracts_texture_stem_and_publishes_canonical_bytes(
        tmp_path):
    texture_path = tmp_path / "textures" / "wall_d.png"
    texture_path.parent.mkdir()
    texture_path.write_bytes(b"not decoded by this gate")
    image = bpy.data.images.new("wall_d.png", width=1, height=1)
    image.filepath = str(texture_path)

    material = _class_material()
    material.mh4blend.twosided_override = True
    material.mh4blend.twosided = False
    texture = material.mh4blend.textures.add()
    texture.slot = 2
    texture.image = image
    parameter = material.mh4blend.params.add()
    parameter.name = "roughness"
    parameter.kind = "SCALAR"
    parameter.scalar = 0.25

    prepared = prepare_blender_material_export(
        material, tmp_path, source_root=tmp_path)
    report = write_prepared_material(prepared, source_root=tmp_path)

    assert report["written"] is True
    assert (tmp_path / "wall.material").read_bytes() == prepared.payload
    assert b'"tex2": "wall_d"' in prepared.payload
    assert read_material_file(tmp_path / "wall.material").name == "wall"
    assert not list(tmp_path.glob(".wall.material.mh-tmp-*"))


def test_prepare_rejects_noncanonical_material_name_with_name_code(tmp_path):
    material = _class_material("Wall")
    with pytest.raises(ValueError, match="MH_E_NONCANONICAL_RESOURCE_NAME"):
        prepare_blender_material_export(material, tmp_path, source_root=tmp_path)


@pytest.mark.parametrize("filename", ["wall.MATERIAL", "wall.mat"])
def test_reader_rejects_noncanonical_material_suffix_with_name_code(
        tmp_path, filename):
    path = tmp_path / filename
    path.write_text('{"class":"simple"}', encoding="utf-8")
    with pytest.raises(ValueError, match="MH_E_NONCANONICAL_RESOURCE_NAME"):
        read_material_file(path)


def test_prepare_resolves_all_textures_before_any_publish(tmp_path):
    material = _class_material()
    image = bpy.data.images.new("missing.png", width=1, height=1)
    texture = material.mh4blend.textures.add()
    texture.slot = 0
    texture.image = image
    with pytest.raises(MaterialValueError) as excinfo:
        prepare_blender_material_export(material, tmp_path, source_root=tmp_path)
    assert excinfo.value.code == "MH_E_UNRESOLVED_TEXTURE_REFERENCE"
    assert not (tmp_path / "wall.material").exists()


def test_prepare_updates_unique_existing_material_anywhere_in_source_tree(
        tmp_path):
    existing = tmp_path / "materials" / "nested" / "wall.material"
    existing.parent.mkdir(parents=True)
    existing.write_bytes(b"old")
    chosen = tmp_path / "mesh_exports"
    material = _class_material("wall")

    prepared = prepare_blender_material_export(
        material, chosen, source_root=tmp_path)
    assert prepared.target == existing
    write_prepared_material(prepared, source_root=tmp_path)
    assert existing.read_bytes() == prepared.payload
    assert not (chosen / "wall.material").exists()


def test_prepare_blocks_duplicate_existing_material_identity(tmp_path):
    for folder in (tmp_path / "a", tmp_path / "b"):
        folder.mkdir()
        (folder / "wall.material").write_text("{}", encoding="utf-8")
    with pytest.raises(MaterialValueError) as excinfo:
        prepare_blender_material_export(
            _class_material("wall"), tmp_path, source_root=tmp_path)
    assert excinfo.value.code == "MH_E_AMBIGUOUS_RESOURCE_NAME"


@pytest.mark.parametrize("filename", ["wall.MATERIAL", "Wall.material"])
def test_prepare_blocks_case_variant_existing_material_identity(
        tmp_path, filename):
    (tmp_path / filename).write_text("{}", encoding="utf-8")
    with pytest.raises(MaterialValueError) as excinfo:
        prepare_blender_material_export(
            _class_material("wall"), tmp_path, source_root=tmp_path)
    assert excinfo.value.code == "MH_E_NONCANONICAL_RESOURCE_NAME"


def test_reader_applies_dedicated_property_group_without_legacy_proxy_fields(
        tmp_path):
    material = bpy.data.materials.new("library_mat")
    resource = MaterialResource("library_mat", library="wet_concrete")
    apply_material_resource(material, resource, source_root=tmp_path)
    assert material.mh4blend.mode == "LIBRARY"
    assert material.mh4blend.library == "wet_concrete"
    assert not material.mh4blend.textures
    assert "is_proxy" not in material and "proxy_path" not in material


def test_reader_preflights_all_texture_refs_before_property_mutation(tmp_path):
    (tmp_path / "present.png").write_bytes(b"present")
    material = _class_material("wall")
    settings = material.mh4blend
    settings.material_class = "original"
    settings.twosided_override = True
    settings.twosided = True
    old_param = settings.params.add()
    old_param.name = "old_param"
    old_param.scalar = 0.75

    resource = MaterialResource(
        "wall", material_class="replacement",
        textures={"tex0": "present", "tex1": "missing"},
        params={"new_param": 0.25})
    with pytest.raises(MaterialValueError) as excinfo:
        apply_material_resource(material, resource, source_root=tmp_path)
    assert excinfo.value.code == "MH_E_UNRESOLVED_TEXTURE_REFERENCE"
    assert settings.mode == "CLASS"
    assert settings.material_class == "original"
    assert settings.twosided_override is True and settings.twosided is True
    assert [(row.name, row.scalar) for row in settings.params] == [
        ("old_param", 0.75)]
    assert len(settings.textures) == 0


def test_fbx_export_updates_every_unique_touched_material(tmp_path, monkeypatch):
    existing_wall = tmp_path / "authored" / "wall.material"
    existing_wall.parent.mkdir()
    existing_wall.write_bytes(b"old wall")
    collection = bpy.data.collections.new("building")
    bpy.context.scene.collection.children.link(collection)
    _mesh("Wall", collection, _class_material("wall"))
    _mesh("Roof", collection, _class_material("roof"))
    monkeypatch.setattr(
        export_fbx_module, "_export_selected_fbx",
        lambda path: Path(path).write_bytes(b"staged fbx"))

    report = export_fbx_collection(
        collection, tmp_path, source_root=tmp_path, export_materials=True)

    assert [row["resource_name"] for row in report["material_updates"]] == [
        "roof", "wall"]
    assert (tmp_path / "roof.material").is_file()
    assert existing_wall.is_file() and existing_wall.read_bytes() != b"old wall"
    assert not (tmp_path / "wall.material").exists()


def test_material_rejection_happens_before_fbx_publish(tmp_path, monkeypatch):
    collection = bpy.data.collections.new("building")
    bpy.context.scene.collection.children.link(collection)
    invalid = _class_material("wall")
    invalid.mh4blend.material_class = "NotCanonical"
    _mesh("Wall", collection, invalid)
    calls = []
    monkeypatch.setattr(
        export_fbx_module, "_export_selected_fbx",
        lambda path: calls.append(path))

    with pytest.raises(MaterialValueError, match="MH_E_MATERIAL_GRAMMAR") as excinfo:
        export_fbx_collection(
            collection, tmp_path, source_root=tmp_path, export_materials=True)
    rendered = str(excinfo.value)
    assert "material 'wall' / class" in rendered
    assert "value 'NotCanonical'" in rendered
    assert calls == []
    assert not (tmp_path / "building.mesh.fbx").exists()
