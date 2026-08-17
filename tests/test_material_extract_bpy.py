"""Blender adapter tests for B4 dagormat material extraction."""

import sys
import json
import importlib
from pathlib import Path

import pytest

bpy = pytest.importorskip("bpy")

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "addon"))

from mh4blend.scene.material_extract import (  # noqa: E402
    DEFAULT_SHADER_CLASS,
    extract_collection_materials,
    normalize_texture_path,
    remap_absolute_texture_path,
    remap_dagormat_texture_paths,
)
from mh4blend.core.canonical import composite_content_hash  # noqa: E402
from mh4blend.scene.export_bundle import (  # noqa: E402
    _collect_duplicate_errors,
    _gather,
    export_bundle,
)
export_module = importlib.import_module(  # noqa: E402
    "mh4blend.scene.export_bundle")


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


def test_extracts_only_dagormat_contract_and_normalizes_textures(tmp_path):
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

    materials, slots = extract_collection_materials(
        collection, str(texture_root))

    assert len(materials) == 1
    extracted = materials[0]
    assert material.get("mh_uid") == extracted.uid
    assert extracted.shader_class == "rendinst_layered"
    assert extracted.params["sides"] == 2
    assert extracted.params["roughness"] == 0.25
    assert extracted.params["tint"] == [0.1, 0.2, 0.3, 1.0]
    assert extracted.textures == {"tex0": "metal_d.tif"}
    assert normalize_texture_path(
        "//textures/metal_d.tif", str(texture_root)) == "metal_d.tif"
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

    materials, _slots = extract_collection_materials(collection, "")

    extracted = materials[0]
    assert extracted.shader_class == DEFAULT_SHADER_CLASS
    assert extracted.params == {}
    assert extracted.textures == {}


@pytest.mark.parametrize(
    "source, expected, changed",
    [
        (r"C:\Legacy\Textures\brick_d.tif",
         r"D:\Project\Textures\brick_d.tif", True),
        (r"c:\legacy\textures\Sub\..\brick_n.tif",
         r"D:\Project\Textures\brick_n.tif", True),
        (r"C:\Legacy\TexturesExtra\foreign.tif",
         r"C:\Legacy\TexturesExtra\foreign.tif", False),
        (r"C:\Legacy\Textures\..\outside.tif",
         r"C:\Legacy\Textures\..\outside.tif", False),
        (r"Z:\Library\foreign.tif", r"Z:\Library\foreign.tif", False),
        ("/mnt/foreign/brick.tif", "/mnt/foreign/brick.tif", False),
        (r"relative\brick.tif", r"relative\brick.tif", False),
        ("//textures/brick.tif", "//textures/brick.tif", False),
    ],
)
def test_one_shot_absolute_texture_remap_is_containment_safe(
        source, expected, changed):
    got, did_change = remap_absolute_texture_path(
        source, r"C:\Legacy\Textures", r"D:\Project\Textures")
    assert got == expected
    assert did_change is changed


def test_dagormat_texture_remap_rewrites_actual_property_group_once():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    material = bpy.data.materials.new("LegacyPaths")
    material.dagormat.textures.tex0 = \
        r"C:\Legacy\Textures\building\wall_d.tif"
    material.dagormat.textures.tex1 = r"textures\relative_n.tif"
    material.dagormat.textures.tex2 = r"Z:\Foreign\wall_n.tif"

    counts = remap_dagormat_texture_paths(
        r"C:\Legacy\Textures", r"D:\Project\Textures",
        materials=[material])

    assert material.dagormat.textures.tex0 == \
        r"D:\Project\Textures\building\wall_d.tif"
    assert material.dagormat.textures.tex1 == r"textures\relative_n.tif"
    assert material.dagormat.textures.tex2 == r"Z:\Foreign\wall_n.tif"
    assert counts == {
        "materials_scanned": 1,
        "dagormat_materials": 1,
        "materials_skipped_read_only": 0,
        "materials_changed": 1,
        "texture_paths_scanned": 3,
        "texture_paths_rewritten": 1,
    }

    second_pass = remap_dagormat_texture_paths(
        r"C:\Legacy\Textures", r"D:\Project\Textures",
        materials=[material])
    assert second_pass["materials_changed"] == 0
    assert second_pass["texture_paths_rewritten"] == 0


def test_dagormat_texture_remap_rolls_back_if_apply_fails():
    class FailingTextures(dict):
        fail = False

        def __setitem__(self, key, value):
            if self.fail and key == "tex1" and value.startswith("D:"):
                raise RuntimeError("simulated read-only property")
            super().__setitem__(key, value)

    textures = FailingTextures({
        "tex0": r"C:\Legacy\Textures\a.tif",
        "tex1": r"C:\Legacy\Textures\b.tif",
    })
    textures.fail = True
    dagormat = type("FakeDagormat", (), {"textures": textures})()
    material = type("FakeMaterial", (), {
        "dagormat": dagormat,
        "is_editable": True,
    })()

    with pytest.raises(RuntimeError, match="simulated read-only"):
        remap_dagormat_texture_paths(
            r"C:\Legacy\Textures", r"D:\Project\Textures",
            materials=[material])

    assert textures == {
        "tex0": r"C:\Legacy\Textures\a.tif",
        "tex1": r"C:\Legacy\Textures\b.tif",
    }


def test_dagormat_texture_remap_skips_read_only_material():
    textures = {"tex0": r"C:\Legacy\Textures\a.tif"}
    dagormat = type("FakeDagormat", (), {"textures": textures})()
    material = type("FakeMaterial", (), {
        "dagormat": dagormat,
        "is_editable": False,
    })()

    counts = remap_dagormat_texture_paths(
        r"C:\Legacy\Textures", r"D:\Project\Textures",
        materials=[material])

    assert textures["tex0"] == r"C:\Legacy\Textures\a.tif"
    assert counts["materials_skipped_read_only"] == 1
    assert counts["texture_paths_rewritten"] == 0


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

    materials, slots = extract_collection_materials(collection, str(tmp_path))

    assert [material.name for material in materials] == ["First", "Second"]
    assert [slot.slot_name for slot in slots] == ["First", "Second"]


def test_empty_slot_and_outside_texture_are_machine_errors(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection, obj = _resource_with_object()
    material = bpy.data.materials.new("EmptySlot")
    obj.data.materials.append(material)
    obj.material_slots[0].material = None

    with pytest.raises(ValueError, match="MH_E_EMPTY_MATERIAL_SLOT"):
        extract_collection_materials(collection, str(tmp_path))

    obj.material_slots[0].material = material
    material.dagormat.shader_class = "rendinst_simple"
    material.dagormat.textures.tex0 = str(tmp_path.parent / "outside.tif")
    with pytest.raises(ValueError, match="MH_E_TEXTURE_OUTSIDE_ROOT"):
        extract_collection_materials(collection, str(tmp_path))


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


def test_gather_wires_materials_slots_and_registry_warning(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection, obj = _resource_with_object()
    geo_scene = next(scene for scene in bpy.data.scenes
                     if scene.name == "GEOMETRY")
    cmp_scene = bpy.data.scenes.new("COMPOSITS")
    bpy.context.window.scene = geo_scene

    material = bpy.data.materials.new("KnownShape")
    material.dagormat.shader_class = "rendinst_layered"
    obj.data.materials.append(material)

    manifest, _composites, _objects, errors = _gather(
        geo_scene, cmp_scene, "MaterialTest", texture_root=str(tmp_path))
    assert errors == []
    assert len(manifest.materials) == 1
    assert manifest.meshes[0].material_slots[0].material_uid \
        == manifest.materials[0].uid

    registry = tmp_path / "registry.json"
    registry.write_text(
        '{"schema":"mh.registry","schema_version":1,'
        '"shader_classes":["rendinst_simple"]}',
        encoding="utf-8",
    )
    report = export_bundle(
        "", geo_scene=geo_scene, cmp_scene=cmp_scene,
        bundle_name="MaterialTest", dry_run=True,
        texture_root=str(tmp_path), registry_path=str(registry),
    )
    assert report["ok"]
    assert [row["code"] for row in report["validation"]["warnings"]] == [
        "MH_W_UNKNOWN_SHADER_CLASS"
    ]

    unreadable = export_bundle(
        "", geo_scene=geo_scene, cmp_scene=cmp_scene,
        bundle_name="MaterialTest", dry_run=True,
        texture_root=str(tmp_path),
        registry_path=str(tmp_path / "missing-registry.json"),
    )
    assert unreadable["ok"]
    assert [row["code"] for row in unreadable["validation"]["warnings"]] == [
        "MH_W_REGISTRY_INVALID"
    ]


def test_duplicate_material_uids_are_detected_before_gather():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection, first_obj = _resource_with_object()
    geo_scene = next(scene for scene in bpy.data.scenes
                     if scene.name == "GEOMETRY")
    cmp_scene = bpy.data.scenes.new("COMPOSITS")

    mesh = bpy.data.meshes.new("SecondMesh")
    mesh.from_pydata([(0, 0, 0), (1, 0, 0), (0, 1, 0)], [], [(0, 1, 2)])
    second_obj = bpy.data.objects.new("SecondObject", mesh)
    second_obj["mh_uid"] = "20000000-0000-0000-0000-000000000002"
    collection.objects.link(second_obj)

    shared_uid = "aaaaaaaa-0000-0000-0000-000000000001"
    first = bpy.data.materials.new("First")
    first["mh_uid"] = shared_uid
    second = bpy.data.materials.new("Second")
    second["mh_uid"] = shared_uid
    first_obj.data.materials.append(first)
    second_obj.data.materials.append(second)

    errors = _collect_duplicate_errors(geo_scene, cmp_scene)
    assert [(row.code, row.subjects) for row in errors] == [
        ("MH_E_DUPLICATE_RESOURCE_UID", [shared_uid])
    ]


def test_shared_mesh_datablock_is_not_a_duplicate_resource_uid():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection, first_obj = _resource_with_object()
    geo_scene = next(scene for scene in bpy.data.scenes
                     if scene.name == "GEOMETRY")
    cmp_scene = bpy.data.scenes.new("COMPOSITS")
    first_obj.data["mh_uid"] = "aaaaaaaa-0000-0000-0000-000000000001"

    second_obj = bpy.data.objects.new("LinkedObject", first_obj.data)
    second_obj["mh_uid"] = "20000000-0000-0000-0000-000000000002"
    collection.objects.link(second_obj)

    assert _collect_duplicate_errors(geo_scene, cmp_scene) == []


def test_nan_material_value_blocks_before_destination_is_created(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection, obj = _resource_with_object()
    geo_scene = next(scene for scene in bpy.data.scenes
                     if scene.name == "GEOMETRY")
    cmp_scene = bpy.data.scenes.new("COMPOSITS")
    material = bpy.data.materials.new("InvalidNumber")
    material.dagormat.shader_class = "rendinst_simple"
    material.dagormat.optional["bad_number"] = float("nan")
    obj.data.materials.append(material)
    destination = tmp_path / "must_not_exist"

    report = export_bundle(
        str(destination), geo_scene=geo_scene, cmp_scene=cmp_scene,
        bundle_name="InvalidMaterial", texture_root=str(tmp_path))

    assert not report["ok"]
    assert [row["code"] for row in report["validation"]["errors"]] == [
        "MH_E_NAN_INF_VALUE"
    ]
    assert report["manifest_changed"] is False
    assert not destination.exists()


def test_pending_manifest_is_fail_closed_and_next_export_recovers(
        tmp_path, monkeypatch):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    _collection, obj = _resource_with_object()
    geo_scene = next(scene for scene in bpy.data.scenes
                     if scene.name == "GEOMETRY")
    cmp_scene = bpy.data.scenes.new("COMPOSITS")
    destination = tmp_path / "source_set"

    first = export_bundle(
        str(destination), geo_scene=geo_scene, cmp_scene=cmp_scene,
        bundle_name="CrashRecovery", texture_root=str(tmp_path))
    stable_manifest = destination / "export_manifest.json"
    pending_manifest = destination / "export_manifest.json.tmp"
    first_doc = json.loads(stable_manifest.read_text(encoding="utf-8"))
    assert first["manifest_written"]
    assert not pending_manifest.exists()

    obj.data.vertices[0].co.x += 0.25

    def simulated_fbx_failure(*_args, **_kwargs):
        raise RuntimeError("simulated FBX failure")

    monkeypatch.setattr(
        export_module, "_export_collection_fbx", simulated_fbx_failure)
    with pytest.raises(RuntimeError, match="simulated FBX failure"):
        export_bundle(
            str(destination), geo_scene=geo_scene, cmp_scene=cmp_scene,
            bundle_name="CrashRecovery", texture_root=str(tmp_path))

    assert pending_manifest.exists()
    assert json.loads(stable_manifest.read_text(encoding="utf-8")) == first_doc

    monkeypatch.undo()
    recovered = export_bundle(
        str(destination), geo_scene=geo_scene, cmp_scene=cmp_scene,
        bundle_name="CrashRecovery", texture_root=str(tmp_path))
    assert recovered["manifest_changed"]
    assert recovered["manifest_written"]
    assert not pending_manifest.exists()
    assert json.loads(stable_manifest.read_text(encoding="utf-8")) != first_doc


def test_stale_pending_manifest_is_committed_even_when_semantics_match(
        tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    _collection, _obj = _resource_with_object()
    geo_scene = next(scene for scene in bpy.data.scenes
                     if scene.name == "GEOMETRY")
    cmp_scene = bpy.data.scenes.new("COMPOSITS")
    destination = tmp_path / "source_set"
    first = export_bundle(
        str(destination), geo_scene=geo_scene, cmp_scene=cmp_scene,
        bundle_name="PendingRecovery", texture_root=str(tmp_path))
    assert first["manifest_written"]
    stable_manifest = destination / "export_manifest.json"
    pending_manifest = destination / "export_manifest.json.tmp"
    pending_manifest.write_bytes(stable_manifest.read_bytes())

    recovered = export_bundle(
        str(destination), geo_scene=geo_scene, cmp_scene=cmp_scene,
        bundle_name="PendingRecovery", texture_root=str(tmp_path))

    assert not recovered["manifest_changed"]
    assert recovered["manifest_written"]
    assert recovered["written"] == [
        next(entry["source"] for entry in
             json.loads(stable_manifest.read_text(encoding="utf-8"))["resources"]
             if entry["kind"] == "static_mesh")
    ]
    assert not pending_manifest.exists()


def test_recovery_rewrites_payload_replaced_before_failed_commit(
        tmp_path, monkeypatch):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    _collection, _obj = _resource_with_object()
    geo_scene = next(scene for scene in bpy.data.scenes
                     if scene.name == "GEOMETRY")
    cmp_scene = bpy.data.scenes.new("COMPOSITS")
    composite = bpy.data.collections.new("CrashComposite")
    composite["mh_uid"] = "30000000-0000-0000-0000-000000000003"
    cmp_scene.collection.children.link(composite)
    node = bpy.data.objects.new("CrashNode", None)
    node["mh_uid"] = "40000000-0000-0000-0000-000000000004"
    composite.objects.link(node)
    destination = tmp_path / "source_set"

    first = export_bundle(
        str(destination), geo_scene=geo_scene, cmp_scene=cmp_scene,
        bundle_name="CrashRevert", texture_root=str(tmp_path))
    assert first["ok"]
    stable_manifest = destination / "export_manifest.json"
    pending_manifest = destination / "export_manifest.json.tmp"
    node["mh_p_crash_probe"] = "state_b"

    def fail_final_commit(_source, _target):
        raise RuntimeError("simulated commit failure")

    monkeypatch.setattr(
        export_module, "_promote_pending_manifest", fail_final_commit)
    with pytest.raises(RuntimeError, match="simulated commit failure"):
        export_bundle(
            str(destination), geo_scene=geo_scene, cmp_scene=cmp_scene,
            bundle_name="CrashRevert", texture_root=str(tmp_path))
    assert pending_manifest.exists()

    # Authoring returns to A while the on-disk composite is still B. Recovery
    # must not trust A's stable manifest for hash-skip decisions.
    del node["mh_p_crash_probe"]
    monkeypatch.undo()
    recovered = export_bundle(
        str(destination), geo_scene=geo_scene, cmp_scene=cmp_scene,
        bundle_name="CrashRevert", texture_root=str(tmp_path))
    stable_doc = json.loads(stable_manifest.read_text(encoding="utf-8"))
    composite_entry = next(
        entry for entry in stable_doc["resources"]
        if entry["kind"] == "composite")
    composite_doc = json.loads(
        (destination / composite_entry["source"]).read_text(encoding="utf-8"))

    assert not recovered["manifest_changed"]
    assert recovered["manifest_written"]
    assert len(recovered["written"]) == 2  # one mesh + one composite
    assert not pending_manifest.exists()
    assert composite_content_hash(composite_doc) == \
        composite_entry["content_hash"]
