"""Blender adapter gates for project texture copy and path remapping."""

from pathlib import Path
import sys
from types import SimpleNamespace

import pytest

bpy = pytest.importorskip("bpy")
REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "addon"))

from mh4blend.core.project_textures import ProjectTextureError  # noqa: E402
from mh4blend.scene.project_textures import (  # noqa: E402
    collect_dagor_texture_bindings,
    copy_all_dagor_textures_to_project,
    remap_all_dagor_textures_to_project,
)


def _material(name, slots):
    textures = {f"tex{index}": "" for index in range(16)}
    textures.update(slots)
    return SimpleNamespace(
        name=name,
        dagormat=SimpleNamespace(
            textures=SimpleNamespace(**textures),
        ),
    )


class _CallbackTextures:
    """Mimics dag4blend RNA textures: attribute writes fire the update
    callback (a full shader node tree rebuild per slot in dag4blend), item
    writes hit the IDProperty storage directly."""

    def __init__(self, slots):
        object.__setattr__(
            self, "_values",
            {f"tex{index}": slots.get(f"tex{index}", "")
             for index in range(16)})
        object.__setattr__(self, "rebuilds", 0)

    def __getattr__(self, name):
        try:
            return object.__getattribute__(self, "_values")[name]
        except KeyError:
            raise AttributeError(name)

    def __setattr__(self, name, value):
        self._values[name] = value
        object.__setattr__(self, "rebuilds", self.rebuilds + 1)

    def __setitem__(self, name, value):
        self._values[name] = value


class _FailOnceTextures:
    def __init__(self, slots, fail_slot):
        object.__setattr__(
            self, "_values",
            {f"tex{index}": slots.get(f"tex{index}", "")
             for index in range(16)})
        object.__setattr__(self, "_fail_slot", fail_slot)
        object.__setattr__(self, "_failed", False)

    def __getattr__(self, name):
        return self._values[name]

    def __setattr__(self, name, value):
        self._values[name] = value
        if name == self._fail_slot and not self._failed:
            object.__setattr__(self, "_failed", True)
            raise RuntimeError("injected texture setter failure")


def _external_texture(tmp_path, name="car_glass_c_tex_d.tif"):
    path = (
        tmp_path / "external" / "develop" / "assets" / "gameproj"
        / "manmade_common" / "textures" / "tile_textures" / name)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"texture payload")
    return path


def test_copy_all_materials_preserves_assets_tree_and_deduplicates(tmp_path):
    source = _external_texture(tmp_path)
    project = tmp_path / "project"
    project.mkdir()
    materials = [
        _material("glass_a", {"tex0": str(source)}),
        _material("glass_b", {"tex2": str(source)}),
    ]

    report = copy_all_dagor_textures_to_project(
        source_root=project, materials=materials)
    destination = (
        project / "assets" / "gameproj" / "manmade_common" / "textures"
        / "tile_textures" / source.name)

    assert report["referenced_slots"] == 2
    assert report["unique_files"] == 1 and report["copied"] == 1
    assert destination.read_bytes() == source.read_bytes()
    assert materials[0].dagormat.textures.tex0 == str(source)
    assert materials[1].dagormat.textures.tex2 == str(source)


def test_copy_and_remap_project_external_dagor_case_to_lowercase(tmp_path):
    source = _external_texture(tmp_path, "Sovmod_bag_tex_d.TGA")
    project = tmp_path / "project"
    project.mkdir()
    material = _material("Sovmod_bag_leather", {"tex0": str(source)})

    copy_report = copy_all_dagor_textures_to_project(
        source_root=project, materials=[material])
    destination = (
        project / "assets" / "gameproj" / "manmade_common" / "textures"
        / "tile_textures" / "sovmod_bag_tex_d.tga")

    assert copy_report["copied"] == 1
    assert destination.read_bytes() == source.read_bytes()

    remap_report = remap_all_dagor_textures_to_project(
        source_root=project, materials=[material])
    assert remap_report["remapped"] == 1
    assert material.dagormat.textures.tex0 == str(destination)


def test_copy_all_reads_proxy_file_instead_of_stale_scene_slots(tmp_path):
    source = _external_texture(tmp_path, "Tree_Leaf_D.TGA")
    project = tmp_path / "project"
    project.mkdir()
    proxy_dir = tmp_path / "proxymats"
    proxy_dir.mkdir()
    (proxy_dir / "Tree_Leaf.proxymat.blk").write_text(
        'class:t="rendinst_tree_colored"\n'
        f'tex0:t="{source}"\n',
        encoding="utf-8",
    )
    material = _material("Tree_Leaf", {"tex0": "stale_cache.tif"})
    material.dagormat.is_proxy = True
    material.dagormat.proxy_path = str(proxy_dir)

    report = copy_all_dagor_textures_to_project(
        source_root=project, materials=[material])
    destination = (
        project / "assets" / "gameproj" / "manmade_common" / "textures"
        / "tile_textures" / "tree_leaf_d.tga")

    assert report["copied"] == 1
    assert report["referenced_slots"] == 1
    assert destination.read_bytes() == source.read_bytes()
    assert material.dagormat.textures.tex0 == "stale_cache.tif"

    remap_report = remap_all_dagor_textures_to_project(
        source_root=project, materials=[material])
    assert remap_report["remapped"] == 0
    assert remap_report["read_only_proxy_slots"] == 1


def test_basename_slot_resolves_loaded_dagor_image_path(tmp_path):
    source = _external_texture(tmp_path, "bush_tree_cotton_branch_a_tex_d.tif")
    project = tmp_path / "project"
    project.mkdir()
    image = bpy.data.images.new(
        "bush_tree_cotton_branch_a_tex_d", width=1, height=1)
    image.filepath = str(source)
    material = _material("bush_tree_cotton_branch", {
        "tex0": "bush_tree_cotton_branch_a_tex_d.tif",
    })

    report = copy_all_dagor_textures_to_project(
        source_root=project, materials=[material])
    destination = (
        project / "assets" / "gameproj" / "manmade_common" / "textures"
        / "tile_textures" / "bush_tree_cotton_branch_a_tex_d.tif")

    assert report["copied"] == 1
    assert destination.read_bytes() == source.read_bytes()
    assert material.dagormat.textures.tex0 == (
        "bush_tree_cotton_branch_a_tex_d.tif")


def test_basename_slot_with_multiple_loaded_sources_fails_before_copy(
        tmp_path):
    first = _external_texture(
        tmp_path, "ambiguous_branch_tex_d.tif")
    second = (
        tmp_path / "other" / "develop" / "assets" / "gameproj"
        / "manmade_common" / "textures" / "tile_textures"
        / "ambiguous_branch_tex_d.tif")
    second.parent.mkdir(parents=True)
    second.write_bytes(b"different texture")
    for source in (first, second):
        image = bpy.data.images.new(
            "ambiguous_branch_tex_d", width=1, height=1)
        image.filepath = str(source)
    project = tmp_path / "project"
    project.mkdir()
    material = _material("ambiguous_branch", {
        "tex0": "ambiguous_branch_tex_d.tif",
    })

    with pytest.raises(ProjectTextureError) as excinfo:
        copy_all_dagor_textures_to_project(
            source_root=project, materials=[material])

    assert excinfo.value.code == "MH_E_AMBIGUOUS_RESOURCE_NAME"
    assert str(first) in str(excinfo.value)
    assert str(second) in str(excinfo.value)
    assert not (project / "assets").exists()


def test_remap_all_materials_preserves_transport_suffix(tmp_path):
    source = _external_texture(tmp_path)
    project = tmp_path / "project"
    project.mkdir()
    material = _material("glass", {"tex0": str(source) + "*?q0-0-1"})
    copy_all_dagor_textures_to_project(
        source_root=project, materials=[material])

    report = remap_all_dagor_textures_to_project(
        source_root=project, materials=[material])
    expected = (
        project / "assets" / "gameproj" / "manmade_common" / "textures"
        / "tile_textures" / source.name)

    assert report["remapped"] == 1
    assert material.dagormat.textures.tex0 == str(expected) + "*?q0-0-1"
    second_report = remap_all_dagor_textures_to_project(
        source_root=project, materials=[material])
    assert second_report["remapped"] == 0


def test_remap_never_fires_the_per_slot_update_callback(tmp_path):
    # dag4blend's texN properties rebuild the whole shader node tree on every
    # attribute assignment; the remap writes through IDProperty item access
    # instead, so a full-scene remap performs zero rebuilds.
    source = _external_texture(tmp_path)
    project = tmp_path / "project"
    project.mkdir()
    textures = _CallbackTextures({"tex0": str(source), "tex3": str(source)})
    material = SimpleNamespace(
        name="glass", dagormat=SimpleNamespace(textures=textures))
    copy_all_dagor_textures_to_project(
        source_root=project, materials=[material])

    report = remap_all_dagor_textures_to_project(
        source_root=project, materials=[material])
    expected = str(
        project / "assets" / "gameproj" / "manmade_common" / "textures"
        / "tile_textures" / source.name)

    assert report["remapped"] == 2
    assert textures.tex0 == expected and textures.tex3 == expected
    assert textures.rebuilds == 0


def test_remap_preflight_failure_leaves_every_path_unchanged(tmp_path):
    first = _external_texture(tmp_path, "first_d.tif")
    second = _external_texture(tmp_path, "second_d.tif")
    project = tmp_path / "project"
    project.mkdir()
    material = _material("wall", {
        "tex0": str(first),
        "tex2": str(second),
    })
    bindings = collect_dagor_texture_bindings(
        [material], source_root=project)
    first_destination = next(
        binding.plan.destination for binding in bindings
        if binding.slot == "tex0")
    first_destination.parent.mkdir(parents=True)
    first_destination.write_bytes(b"only one copied texture")
    before = (
        material.dagormat.textures.tex0,
        material.dagormat.textures.tex2,
    )

    with pytest.raises(ProjectTextureError) as excinfo:
        remap_all_dagor_textures_to_project(
            source_root=project, materials=[material])

    assert excinfo.value.code == "MH_E_UNRESOLVED_TEXTURE_REFERENCE"
    assert (
        material.dagormat.textures.tex0,
        material.dagormat.textures.tex2,
    ) == before


def test_binding_error_names_material_slot_and_authored_path(tmp_path):
    project = tmp_path / "project"
    project.mkdir()
    bad = tmp_path / "outside" / "wall_d.tif"
    material = _material("wall_c", {"tex2": str(bad)})

    with pytest.raises(ProjectTextureError) as excinfo:
        collect_dagor_texture_bindings([material], source_root=project)

    rendered = str(excinfo.value)
    assert "material 'wall_c' / dagormat.textures.tex2" in rendered
    assert repr(str(bad)) in rendered


def test_remap_setter_failure_restores_every_original_path(tmp_path):
    first = _external_texture(tmp_path, "first_d.tif")
    second = _external_texture(tmp_path, "second_d.tif")
    project = tmp_path / "project"
    project.mkdir()
    textures = _FailOnceTextures({
        "tex0": str(first),
        "tex2": str(second),
    }, fail_slot="tex2")
    material = SimpleNamespace(
        name="wall",
        dagormat=SimpleNamespace(textures=textures),
    )
    copy_all_dagor_textures_to_project(
        source_root=project, materials=[material])
    before = (textures.tex0, textures.tex2)

    with pytest.raises(RuntimeError, match="injected texture setter failure"):
        remap_all_dagor_textures_to_project(
            source_root=project, materials=[material])

    assert (textures.tex0, textures.tex2) == before
