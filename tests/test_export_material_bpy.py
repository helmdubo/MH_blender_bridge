"""Blender gates for Source Protocol v4 material extraction/publication."""

from pathlib import Path
import importlib
import json
import sys
from types import SimpleNamespace

import pytest

bpy = pytest.importorskip("bpy")
REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "addon"))

from mh4blend.core.materials import (  # noqa: E402
    MaterialValueError,
    material_json_bytes,
)
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
export_material_module = importlib.import_module("mh4blend.scene.export_material")


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


class _FakeDagorArray:
    def __init__(self, values):
        self._values = values

    def to_list(self):
        return list(self._values)


def _dagor_material(
        name="wall", *, shader_class="rendinst_simple", sides=0,
        textures=None, params=None):
    dagor_textures = {f"tex{index}": "" for index in range(16)}
    dagor_textures.update(textures or {})
    return SimpleNamespace(
        name=name,
        mh4blend=SimpleNamespace(
            mode="CLASS",
            material_class="",
            twosided_override=False,
            twosided=False,
            textures=[],
            params=[],
        ),
        dagormat=SimpleNamespace(
            shader_class=shader_class,
            sides=sides,
            textures=SimpleNamespace(**dagor_textures),
            optional=params or {},
        ),
    )


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


def test_dagormat_extract_preserves_reference_proxymat_fields(tmp_path):
    texture = tmp_path / "decal_leaks_b_tex_m.tif"
    texture.write_bytes(b"source texture")
    material = _dagor_material(
        "decal_wall_leaks",
        shader_class="rendinst_deferred_modulate2x_decal",
        sides=0,
        textures={
            "tex0": (
                r"A:\Enlisted_AM\EnlistedCDK\develop\assets\gameproj"
                r"\manmade_common\textures\decals\decal_leaks_b_tex_m.tif"),
        },
        params={
            "smoothness_metalness": _FakeDagorArray((-1.0, 0.0, 0.0, 0.0)),
            "intensity": _FakeDagorArray((0.9, 0.0, 1.0, 0.0)),
        },
    )

    prepared = prepare_blender_material_export(
        material, tmp_path, source_root=tmp_path)
    document = json.loads(prepared.payload)

    assert prepared.payload == (
        b'{\n'
        b'  "class": "rendinst_deferred_modulate2x_decal",\n'
        b'  "twosided": false,\n'
        b'  "textures": {\n'
        b'    "tex0": "decal_leaks_b_tex_m"\n'
        b'  },\n'
        b'  "params": {\n'
        b'    "intensity": [\n'
        b'      0.9,\n'
        b'      0,\n'
        b'      1,\n'
        b'      0\n'
        b'    ],\n'
        b'    "smoothness_metalness": [\n'
        b'      -1,\n'
        b'      0,\n'
        b'      0,\n'
        b'      0\n'
        b'    ]\n'
        b'  }\n'
        b'}\n')

    assert document["class"] == "rendinst_deferred_modulate2x_decal"
    assert document["twosided"] is False
    assert document["textures"] == {"tex0": "decal_leaks_b_tex_m"}
    assert document["params"]["smoothness_metalness"] == [-1, 0, 0, 0]
    assert document["params"]["intensity"] == pytest.approx([0.9, 0, 1, 0])


def test_explicit_v4_rows_override_matching_dagormat_fields():
    material = _dagor_material(
        textures={"tex0": r"C:\old\wall_old.tex.blk"},
        params={"paint_details": "not representable"},
    )
    material.mh4blend.material_class = "rendinst_layered"
    material.mh4blend.twosided_override = True
    material.mh4blend.twosided = True
    material.mh4blend.textures = [SimpleNamespace(
        slot=0,
        image=SimpleNamespace(filepath=r"C:\new\wall_new.tif", name="wall_new"),
    )]
    material.mh4blend.params = [SimpleNamespace(
        name="paint_details", kind="SCALAR", scalar=0.75, vector=(),
    )]

    resource = export_material_module._extract_resource(material)

    assert resource.material_class == "rendinst_layered"
    assert resource.twosided is True
    assert resource.textures == {"tex0": "wall_new"}
    assert resource.params == {"paint_details": 0.75}


def test_dagormat_extracts_scalar_params_and_sparse_texture_slots():
    material = _dagor_material(
        "windows_aluminium",
        shader_class="rendinst_simple_painted",
        sides=0,
        textures={
            "tex0": r"D:\textures\frames_aluminium_tex_d.tif*?q0-0-1",
            "tex2": r"D:/textures/frames_aluminium_tex_n.tif",
        },
        params={
            "paint_details": 0.85,
            "palette_index": 0,
            "micro_detail_layer": 6,
            "micro_detail_layer_swap_uv": 1,
        },
    )

    resource = export_material_module._extract_resource(material)

    assert resource.twosided is False
    assert resource.textures == {
        "tex0": "frames_aluminium_tex_d",
        "tex2": "frames_aluminium_tex_n",
    }
    assert resource.params == {
        "paint_details": 0.85,
        "palette_index": 0,
        "micro_detail_layer": 6,
        "micro_detail_layer_swap_uv": 1,
    }


def test_dagormat_real_two_sided_fails_instead_of_losing_semantics():
    with pytest.raises(MaterialValueError) as excinfo:
        export_material_module._extract_resource(_dagor_material(sides=2))
    assert excinfo.value.code == "MH_E_MATERIAL_NOT_ROUNDTRIPPABLE"
    assert excinfo.value.path == "dagormat.sides"


@pytest.mark.parametrize("value", [False, 0.0, "0", None])
def test_dagormat_sides_requires_exact_integer(value):
    with pytest.raises(MaterialValueError) as excinfo:
        export_material_module._extract_resource(
            _dagor_material(sides=value))
    assert excinfo.value.code == "MH_E_MATERIAL_NOT_ROUNDTRIPPABLE"
    assert excinfo.value.path == "dagormat.sides"


def test_dagormat_texture_rejects_non_image_extension_with_exact_slot_path():
    with pytest.raises(MaterialValueError) as excinfo:
        export_material_module._extract_resource(_dagor_material(
            textures={"tex2": r"D:\textures\wall_n.tex.blk"}))
    assert excinfo.value.code == "MH_E_NONCANONICAL_TEXTURE_REFERENCE"
    assert excinfo.value.path == "dagormat.textures.tex2"


@pytest.mark.parametrize("value", [True, "opaque", [1, 2, 3]])
def test_unrepresentable_dagormat_param_fails_closed(value):
    with pytest.raises(MaterialValueError) as excinfo:
        export_material_module._extract_resource(
            _dagor_material(params={"unsupported": value}))
    assert excinfo.value.code == "MH_E_MATERIAL_NOT_ROUNDTRIPPABLE"
    assert excinfo.value.path == "dagormat.optional.unsupported"


def test_loaded_proxy_flag_does_not_hide_resolved_dagormat_content():
    material = _dagor_material(params={"roughness": 0.25})
    material.dagormat.is_proxy = True
    material.dagormat.proxy_path = r"D:\proxymats"
    resource = export_material_module._extract_resource(material)
    assert resource.material_class == "rendinst_simple"
    assert resource.params == {"roughness": 0.25}


def test_unresolved_proxy_shader_reference_fails_class_validation():
    resource = export_material_module._extract_resource(
        _dagor_material(shader_class="wall:proxymat"))
    with pytest.raises(MaterialValueError) as excinfo:
        material_json_bytes(resource)
    assert excinfo.value.code == "MH_E_MATERIAL_GRAMMAR"
    assert excinfo.value.path == "class"


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
