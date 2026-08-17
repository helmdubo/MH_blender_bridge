"""Blender adapter tests for B4 dagormat material extraction."""

import sys
import json
from pathlib import Path
import warnings

import pytest

bpy = pytest.importorskip("bpy")

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "addon"))

from mh4blend.scene.material_extract import (  # noqa: E402
    DEFAULT_SHADER_CLASS,
    PROP_IMPORTED_MATERIAL_PAYLOAD,
    extract_collection_materials,
    normalize_texture_path,
)


class _TestTextures(bpy.types.PropertyGroup):
    tex0: bpy.props.StringProperty(default="", subtype="FILE_PATH")
    tex1: bpy.props.StringProperty(default="", subtype="FILE_PATH")
    tex2: bpy.props.StringProperty(default="", subtype="FILE_PATH")


class _TestOptional(bpy.types.PropertyGroup):
    pass


class _TestDagormat(bpy.types.PropertyGroup):
    shader_class: bpy.props.StringProperty(default="")
    sides: bpy.props.IntProperty(default=0, min=0, max=2)
    optional: bpy.props.PointerProperty(type=_TestOptional)
    textures: bpy.props.PointerProperty(type=_TestTextures)


@pytest.fixture(scope="module", autouse=True)
def dagormat_rna():
    if hasattr(bpy.types.Material, "dagormat"):
        pytest.skip("test dagormat RNA conflicts with an already enabled addon")
    for cls in (_TestTextures, _TestOptional, _TestDagormat):
        bpy.utils.register_class(cls)
    bpy.types.Material.dagormat = bpy.props.PointerProperty(type=_TestDagormat)
    yield
    del bpy.types.Material.dagormat
    for cls in reversed((_TestTextures, _TestOptional, _TestDagormat)):
        bpy.utils.unregister_class(cls)


def _resource_with_object(name="Resource", object_uid="10000000-0000-0000-0000-000000000001"):
    scene = bpy.data.scenes.new("GEOMETRY")
    collection = bpy.data.collections.new(name)
    scene.collection.children.link(collection)
    mesh = bpy.data.meshes.new(f"{name}Mesh")
    mesh.from_pydata([(0, 0, 0), (1, 0, 0), (0, 1, 0)], [], [(0, 1, 2)])
    obj = bpy.data.objects.new(f"{name}Object", mesh)
    obj["mh_uid"] = object_uid
    collection.objects.link(obj)
    return collection, obj


def test_extracts_dagormat_contract_and_keeps_authored_texture_paths(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    blend_path = tmp_path / "authoring.blend"
    bpy.ops.wm.save_as_mainfile(filepath=str(blend_path))
    texture_root = tmp_path / "textures"
    texture_root.mkdir()

    collection, obj = _resource_with_object()
    material = bpy.data.materials.new("Metal")
    material.dagormat.shader_class = "rendinst_layered"
    material.dagormat.sides = 2
    material.dagormat.optional["roughness"] = 0.25
    material.dagormat.optional["tint"] = [0.1, 0.2, 0.3, 1.0]
    material.dagormat.textures.tex0 = str(texture_root / "metal_d.tif")
    material.dagormat.textures.tex1 = ""
    obj.data.materials.append(material)

    materials, slots = extract_collection_materials(collection)

    assert len(materials) == 1
    extracted = materials[0]
    assert material.get("mh_uid") == extracted.uid
    assert extracted.shader_class == "rendinst_layered"
    assert extracted.params["sides"] == 2
    assert extracted.params["roughness"] == 0.25
    assert extracted.params["tint"] == [0.1, 0.2, 0.3, 1.0]
    assert extracted.textures == {"tex0": str(texture_root / "metal_d.tif")}
    assert normalize_texture_path("//textures/metal_d.tif") == \
        bpy.path.abspath("//textures/metal_d.tif")
    assert [(slot.slot_name, slot.material_uid) for slot in slots] == [
        ("Metal", extracted.uid)
    ]


def test_empty_shader_uses_minimal_placeholder(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection, obj = _resource_with_object()
    material = bpy.data.materials.new("Placeholder")
    material.dagormat.shader_class = ""
    material.dagormat.sides = 2
    material.dagormat.optional["stale"] = 123
    material.dagormat.textures.tex0 = str(tmp_path / "stale.tif")
    obj.data.materials.append(material)

    materials, _slots = extract_collection_materials(collection)

    extracted = materials[0]
    assert extracted.shader_class == DEFAULT_SHADER_CLASS
    assert extracted.params == {}
    assert extracted.textures == {}


def test_imported_manifest_payload_roundtrips_without_configured_dagormat():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection, obj = _resource_with_object()
    material = bpy.data.materials.new("ImportedMetal")
    material["mh_uid"] = "30000000-0000-0000-0000-000000000003"
    material[PROP_IMPORTED_MATERIAL_PAYLOAD] = json.dumps({
        "uid": material["mh_uid"],
        "kind": "material",
        "name": "ImportedMetal",
        "shader_class": "rendinst_layered",
        "params": {"roughness": 0.25, "sides": 2},
        "textures": {"tex0": r"A:\\assets\\metal_d.tif"},
        "content_hash": "xxh3:ignored-on-read",
    })
    obj.data.materials.append(material)

    extracted = extract_collection_materials(collection)[0][0]
    assert extracted.uid == material["mh_uid"]
    assert extracted.shader_class == "rendinst_layered"
    assert extracted.params == {"roughness": 0.25, "sides": 2}
    assert extracted.textures == {"tex0": r"A:\\assets\\metal_d.tif"}


def test_invalid_imported_material_payload_is_machine_error():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection, obj = _resource_with_object()
    material = bpy.data.materials.new("BrokenImportedMaterial")
    material[PROP_IMPORTED_MATERIAL_PAYLOAD] = "{not-json"
    obj.data.materials.append(material)

    with pytest.raises(ValueError, match="MH_E_INVALID_MATERIAL_VALUE"):
        extract_collection_materials(collection)


def test_blender_relative_texture_requires_saved_blend():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection, obj = _resource_with_object()
    material = bpy.data.materials.new("RelativeTexture")
    material.dagormat.shader_class = "rendinst_simple"
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", RuntimeWarning)
        material.dagormat.textures.tex0 = "//textures/a.tif"
    obj.data.materials.append(material)

    with pytest.raises(ValueError, match="MH_E_INVALID_MATERIAL_VALUE"):
        extract_collection_materials(collection)


def test_unsupported_optional_value_is_machine_error():
    class Unsupported:
        pass

    class FakeDagormat:
        shader_class = "rendinst_simple"
        sides = 0
        optional = {"bad": Unsupported()}
        textures = {}

    class FakeMaterial(dict):
        name = "InvalidOptional"
        dagormat = FakeDagormat()

    class FakeSlot:
        name = "InvalidOptional"
        material = FakeMaterial()

    class FakeObject(dict):
        type = "MESH"
        name = "InvalidObject"
        material_slots = [FakeSlot()]

    obj = FakeObject(
        mh_uid="10000000-0000-0000-0000-000000000001")
    collection = type("FakeCollection", (), {"objects": [obj]})()

    with pytest.raises(ValueError, match="MH_E_INVALID_MATERIAL_VALUE"):
        extract_collection_materials(collection)


def test_multi_object_order_is_uid_then_slot_and_material_uid_is_unique(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection, later = _resource_with_object(
        object_uid="20000000-0000-0000-0000-000000000002")
    mesh = bpy.data.meshes.new("EarlierMesh")
    mesh.from_pydata([(0, 0, 0), (1, 0, 0), (0, 1, 0)], [], [(0, 1, 2)])
    earlier = bpy.data.objects.new("Earlier", mesh)
    earlier["mh_uid"] = "10000000-0000-0000-0000-000000000001"
    collection.objects.link(earlier)

    first = bpy.data.materials.new("First")
    first.dagormat.shader_class = "rendinst_simple"
    second = bpy.data.materials.new("Second")
    second.dagormat.shader_class = "rendinst_simple"
    earlier.data.materials.append(first)
    earlier.data.materials.append(second)
    later.data.materials.append(second)  # repeated MaterialUID: first wins

    materials, slots = extract_collection_materials(collection)

    assert [material.name for material in materials] == ["First", "Second"]
    assert [slot.slot_name for slot in slots] == ["First", "Second"]


def test_empty_slot_is_error_and_texture_path_is_not_root_validated(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection, obj = _resource_with_object()
    material = bpy.data.materials.new("EmptySlot")
    obj.data.materials.append(material)
    obj.material_slots[0].material = None

    with pytest.raises(ValueError, match="MH_E_EMPTY_MATERIAL_SLOT"):
        extract_collection_materials(collection)

    obj.material_slots[0].material = material
    material.dagormat.shader_class = "rendinst_simple"
    authored = str(tmp_path.parent / "outside.tif")
    material.dagormat.textures.tex0 = authored
    materials, _slots = extract_collection_materials(collection)
    assert materials[0].textures == {"tex0": authored}


def test_same_slot_name_cannot_point_to_different_material_uids():
    class FakeMaterial(dict):
        def __init__(self, name, uid):
            super().__init__(mh_uid=uid)
            self.name = name

    class FakeSlot:
        def __init__(self, name, material):
            self.name = name
            self.material = material

    class FakeObject(dict):
        type = "MESH"

        def __init__(self, uid, slot):
            super().__init__(mh_uid=uid)
            self.name = uid
            self.material_slots = [slot]

    first = FakeMaterial("First", "aaaaaaaa-0000-0000-0000-000000000001")
    second = FakeMaterial("Second", "bbbbbbbb-0000-0000-0000-000000000002")
    collection = type("FakeCollection", (), {"objects": [
        FakeObject("10000000-0000-0000-0000-000000000001", FakeSlot("Shared", first)),
        FakeObject("20000000-0000-0000-0000-000000000002", FakeSlot("Shared", second)),
    ]})()

    with pytest.raises(ValueError, match="MH_E_MATERIAL_SLOT_CONFLICT"):
        extract_collection_materials(collection)
