"""Blender host seam tests for standalone/FBX-triggered material output."""

from pathlib import Path
import sys

import pytest

bpy = pytest.importorskip("bpy")

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "addon"))

from mh4blend.scene.export_material import (  # noqa: E402
    prepare_blender_material_export,
    prepare_material_resource_export,
    prepare_material_resource_exports,
)
from mh4blend.scene.material_extract import DEFAULT_SHADER_CLASS  # noqa: E402


UID = "7d995e54-d084-4466-a613-a1cd8f3248b2"


class _TestTextures(bpy.types.PropertyGroup):
    tex0: bpy.props.StringProperty(default="", subtype="FILE_PATH")
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


def test_prepares_dagormat_material_for_first_export(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    blend = tmp_path / "authoring.blend"
    bpy.ops.wm.save_as_mainfile(filepath=str(blend))
    material = bpy.data.materials.new("Metal")
    material["mh_uid"] = UID
    material.dagormat.shader_class = "rendinst_layered"
    material.dagormat.sides = 2
    material.dagormat.optional["roughness"] = 0.25
    material.dagormat.textures.tex0 = str(
        tmp_path / "textures" / "metal_d.tif")

    prepared = prepare_blender_material_export(
        material, tmp_path / "materials", source_root=tmp_path)

    assert prepared.document["shader_class"] == "rendinst_layered"
    assert prepared.document["params"] == {"roughness": 0.25, "sides": 2}
    assert prepared.document["textures"] == {
        "tex0": "textures/metal_d.tif"}
    assert prepared.resource_row["kind"] == "material"
    assert Path(prepared.payload_path).name == f"metal__{UID[:8]}.material"


def test_empty_dagormat_is_minimal_placeholder(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    material = bpy.data.materials.new("Placeholder")
    material["mh_uid"] = UID
    material.dagormat.sides = 2
    material.dagormat.optional["stale"] = 7
    material.dagormat.textures.tex0 = str(tmp_path / "stale.tif")

    prepared = prepare_blender_material_export(
        material, tmp_path, source_root=tmp_path)
    assert prepared.document["shader_class"] == DEFAULT_SHADER_CLASS
    assert prepared.document["params"] == {}
    assert prepared.document["textures"] == {}


def test_fbx_seam_accepts_extracted_resource_and_resolved_owner(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    material = bpy.data.materials.new("Renamed")
    material["mh_uid"] = UID
    material.dagormat.shader_class = "rendinst_simple"
    from mh4blend.scene.material_extract import _material_resource

    resource = _material_resource(material)
    payload = tmp_path / "common" / f"old_name__{UID[:8]}.material"
    manifest = tmp_path / "export_manifest.json"
    prepared = prepare_material_resource_export(
        resource, tmp_path / "ignored", source_root=tmp_path,
        target_payload_path=payload, owning_manifest_path=manifest,
        existing_source=f"common/{payload.name}")
    assert prepared.payload_path == str(payload)
    assert prepared.resource_row["source"] == f"common/{payload.name}"


def test_external_texture_reports_warning_or_blocks_by_policy(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    material = bpy.data.materials.new("External")
    material["mh_uid"] = UID
    material.dagormat.shader_class = "rendinst_simple"
    material.dagormat.textures.tex2 = str(
        tmp_path.parent / "dagor_library" / "metal_n.tif")

    prepared = prepare_blender_material_export(
        material, tmp_path, source_root=tmp_path,
        texture_policy="transitional")
    assert prepared.diagnostics[0].code == "MH_W_TEXTURE_OUTSIDE_ROOT"

    with pytest.raises(ValueError, match="MH_E_TEXTURE_OUTSIDE_ROOT"):
        prepare_blender_material_export(
            material, tmp_path, source_root=tmp_path,
            texture_policy="strict")


def test_fbx_batch_deduplicates_and_routes_existing_owner(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    existing = bpy.data.materials.new("Existing")
    existing["mh_uid"] = UID
    existing.dagormat.shader_class = "rendinst_simple"
    fresh = bpy.data.materials.new("Fresh")
    fresh["mh_uid"] = "aaaaaaaa-0000-0000-0000-000000000001"
    fresh.dagormat.shader_class = "rendinst_simple"
    from mh4blend.scene.material_extract import _material_resource

    old_payload = tmp_path / "common" / f"old_name__{UID[:8]}.material"
    owner_manifest = tmp_path / "export_manifest.json"
    existing_resource = _material_resource(existing)
    fresh_resource = _material_resource(fresh)
    prepared = prepare_material_resource_exports(
        [existing_resource, fresh_resource, existing_resource],
        tmp_path / "fbx", source_root=tmp_path,
        owners_by_uid={UID: {
            "payload_path": old_payload,
            "owning_manifest_path": owner_manifest,
            "manifest_row": {"source": f"common/{old_payload.name}"},
        }})

    assert [item.document["uid"] for item in prepared] == [
        UID, fresh_resource.uid]
    assert prepared[0].payload_path == str(old_payload)
    assert Path(prepared[1].payload_path).parent == tmp_path / "fbx"
