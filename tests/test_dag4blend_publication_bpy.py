"""Real-writer gates for the read-only dag4blend DTO publication planner."""

from pathlib import Path
import sys

import pytest

bpy = pytest.importorskip("bpy")
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "addon"))

from mh4blend.core.batch_publish import BatchPartialPublishError  # noqa: E402
from mh4blend.core.composites import composite_json_bytes  # noqa: E402
from mh4blend.core.model import Composite, Node, PlacementProfile, RandomOption  # noqa: E402
from mh4blend.core.placements import placement_json_bytes  # noqa: E402
from mh4blend.core.source_closure import ResourceKey  # noqa: E402
from mh4blend.scene.export_closure import (  # noqa: E402
    CLOSURE_MODE_ROOT,
    CLOSURE_MODE_COMPOSITES,
    CLOSURE_MODE_INCLUDE_ALL,
    export_composite_closure_collection,
    publish_composite_closure_export,
    stage_composite_closure_export,
)
from mh4blend.scene.export_material import prepare_blender_material_export  # noqa: E402
from mh4blend.scene.import_fbx import parse_mesh_fbx  # noqa: E402
from mh4blend.scene.resource_markers import (  # noqa: E402
    COLLECTION_KIND_KEY,
    COLLECTION_RESOURCE_KEY,
    INCOMPLETE_IMPORT_KEY,
)
from mh4blend.ui import composite_authoring, ops  # noqa: E402


@pytest.fixture(autouse=True)
def properties():
    own_material = not hasattr(bpy.types.Material, "mh4blend")
    own_object = not hasattr(bpy.types.Object, "mh4blend")
    if own_material:
        ops.register()
    if own_object:
        composite_authoring.register()
    bpy.ops.wm.read_factory_settings(use_empty=True)
    try:
        yield
    finally:
        if own_object:
            composite_authoring.unregister()
        if own_material:
            ops.unregister()


def _collection(name):
    collection = bpy.data.collections.new(name)
    bpy.context.scene.collection.children.link(collection)
    return collection


def _fixture(tmp_path, *, lods=False):
    source = tmp_path / "source"
    source.mkdir()
    material = bpy.data.materials.new("bridge_surface")
    material.mh4blend.material_class = "bridge_shader"
    mesh = _collection("bridge_mesh.lods" if lods else "bridge_mesh")
    mesh["type"] = "rendinst"
    mesh["name"] = "bridge_mesh"
    for level in range(2 if lods else 1):
        target = mesh
        if lods:
            target = bpy.data.collections.new(f"bridge_mesh.lod{level:02d}")
            mesh.children.link(target)
        data = bpy.data.meshes.new(f"triangle{level}_data")
        data.from_pydata([(0, 0, 0), (1, 0, 0), (0, 1, 0)], [], [(0, 1, 2)])
        data.materials.append(material)
        target.objects.link(bpy.data.objects.new(f"triangle{level}", data))
    documents = {
        "bridge_root": Composite("bridge_root", [
            Node("composite", resource="bridge_child")]),
        "bridge_child": Composite("bridge_child", [
            Node("mesh", resource="bridge_mesh")]),
    }
    inputs = {("mesh", "bridge_mesh"): mesh}
    legacy = _collection("bridge_root")
    legacy["type"] = "composit"
    legacy["name"] = "bridge_root"
    return source, documents, inputs, mesh, material, legacy


def _prepare(source, documents, inputs, **kwargs):
    # Local import lets the public-command regression run unchanged on main
    # 5f566c7, which does not contain the new DTO planner module.
    from mh4blend.scene.dag4blend_publication import prepare_dag4blend_publication
    return prepare_dag4blend_publication(
        documents, inputs, root_name="bridge_root", source_root=source,
        output_dir=source, **kwargs)


def _files(root):
    return {str(path.relative_to(root)): path.read_bytes()
            for path in root.rglob("*") if path.is_file()}


def _frozen(value):
    if hasattr(value, "items"):
        return tuple(sorted((key, _frozen(item)) for key, item in value.items()))
    if isinstance(value, (tuple, list)) or hasattr(value, "to_list"):
        return tuple(_frozen(item) for item in value)
    return value


def _snapshot():
    def pointer(value):
        return None if value is None else value.as_pointer()
    bpy.context.view_layer.update()
    return (
        tuple((row.name, row.as_pointer()) for row in bpy.data.scenes),
        tuple((row.name, row.as_pointer(), _frozen(row),
               tuple(obj.as_pointer() for obj in row.objects),
               tuple(child.as_pointer() for child in row.children))
              for row in bpy.data.collections),
        tuple((row.name, row.as_pointer(), _frozen(row), pointer(row.parent),
               pointer(row.instance_collection), pointer(row.data),
               tuple(tuple(axis) for axis in row.matrix_world))
              for row in bpy.data.objects),
        tuple((row.name, row.as_pointer(), _frozen(row))
              for row in bpy.data.materials),
        tuple(row.as_pointer() for row in bpy.context.selected_objects),
        pointer(bpy.context.view_layer.objects.active),
        bpy.context.scene.as_pointer(),
    )


def _publish(plan, tmp_path, name="stage", boundary=None):
    staging = tmp_path / name
    staging.mkdir()
    staged = stage_composite_closure_export(plan, staging_dir=staging)
    return publish_composite_closure_export(
        plan, staged, lock_root=tmp_path / "locks", _boundary_hook=boundary)


def _closed_prefix(source):
    if (source / "bridge_mesh.mesh.fbx").exists():
        assert (source / "bridge_surface.material").exists()
    if (source / "bridge_child.composite").exists():
        assert (source / "bridge_mesh.mesh.fbx").exists()
    if (source / "bridge_root.composite").exists():
        assert (source / "bridge_child.composite").exists()


def test_public_command_exports_dagor_mesh_without_mutating_scene(tmp_path):
    source, _documents, _inputs, mesh, _material, legacy = _fixture(tmp_path)
    placement = bpy.data.objects.new("mesh_placement", None)
    legacy.objects.link(placement)
    placement.instance_type = "COLLECTION"
    placement.instance_collection = mesh
    placement.location.x = 2.0
    placement.select_set(True)
    bpy.context.view_layer.objects.active = placement
    before = _snapshot()
    report = export_composite_closure_collection(
        legacy, source, source_root=source, mode=CLOSURE_MODE_INCLUDE_ALL)
    assert report["published"] == [
        "material:bridge_surface", "static_mesh:bridge_mesh", "composite:bridge_root"]
    assert _snapshot() == before


@pytest.mark.parametrize("lods", [False, True])
def test_first_real_batch_writes_files_without_scene_mutation(tmp_path, lods):
    source, documents, inputs, mesh, _material, _legacy = _fixture(tmp_path, lods=lods)
    before = _snapshot()
    plan = _prepare(source, documents, inputs)
    assert _snapshot() == before
    assert _files(source) == {}
    assert all(row.prepared is None for row in plan.payloads
               if row.key.kind == "composite")
    events = []

    def observe(event, item, _published):
        if event == "after_replace":
            events.append(item.identity)
            _closed_prefix(source)

    _publish(plan, tmp_path, boundary=observe)
    assert events == [
        "material:bridge_surface", "static_mesh:bridge_mesh",
        "composite:bridge_child", "composite:bridge_root"]
    assert _snapshot() == before
    assert COLLECTION_KIND_KEY not in mesh
    parsed = parse_mesh_fbx(source / "bridge_mesh.mesh.fbx")
    assert parsed.resource_name == "bridge_mesh"
    assert parsed.material_names == ("bridge_surface",)


@pytest.mark.parametrize("boundary", ["before_replace", "after_replace"])
@pytest.mark.parametrize("failure_index", range(4))
def test_each_publish_boundary_preserves_scene_and_reports_exact_prefix(
        tmp_path, boundary, failure_index):
    source, documents, inputs, _mesh, _material, _legacy = _fixture(tmp_path)
    before = _snapshot()
    plan = _prepare(source, documents, inputs)
    identities = tuple(str(row.key) for row in plan.to_publish)

    def inject(event, item, _published):
        _closed_prefix(source)
        if event == boundary and item.identity == identities[failure_index]:
            raise RuntimeError("injected publication boundary")

    count = failure_index + (boundary == "after_replace")
    error_type = BatchPartialPublishError if count else RuntimeError
    with pytest.raises(error_type, match="injected publication boundary") as caught:
        _publish(plan, tmp_path, boundary=inject)
    assert _snapshot() == before
    _closed_prefix(source)
    if count:
        assert caught.value.published == identities[:count]
        assert caught.value.unpublished == identities[count:]

    if count < 2:
        retry = _prepare(source, documents, inputs)
    else:
        # Honest temporary STOP: repeat-loaded-FBX comparison is unresolved.
        files = _files(source)
        with pytest.raises(ValueError, match="proven comparison"):
            _prepare(source, documents, inputs)
        assert _files(source) == files
        # Explicit narrower command reuses published geometry without guessing.
        retry = _prepare(source, documents, inputs, mode=CLOSURE_MODE_COMPOSITES)
    report = _publish(retry, tmp_path, "retry")
    assert report["published"] == list(identities[count:])
    assert _snapshot() == before
    _closed_prefix(source)


@pytest.mark.parametrize("failure", [
    "material", "texture", "partial_stamp", "mixed_stamps",
    "incomplete", "wrong_identity",
])
def test_preflight_failure_has_no_files_or_scene_delta(tmp_path, failure):
    source, documents, inputs, mesh, material, _legacy = _fixture(tmp_path)
    if failure == "material":
        material.mh4blend.material_class = ""
    elif failure == "texture":
        texture = bpy.data.images.new("missing_image", 1, 1)
        texture.filepath = str(source / "missing.png")
        row = material.mh4blend.textures.add()
        row.slot = 0
        row.image = texture
    elif failure == "partial_stamp":
        mesh[COLLECTION_KIND_KEY] = "mesh"
    elif failure == "mixed_stamps":
        mesh[COLLECTION_KIND_KEY] = "mesh"
        mesh[COLLECTION_RESOURCE_KEY] = "bridge_mesh"
    elif failure == "incomplete":
        mesh[INCOMPLETE_IMPORT_KEY] = True
    elif failure == "wrong_identity":
        mesh.name = "different_identity"
    before = _snapshot()
    with pytest.raises((ValueError, TypeError)):
        _prepare(source, documents, inputs)
    assert _files(source) == {}
    assert _snapshot() == before


def test_unrelated_blender_ids_are_not_future_adoption_targets(tmp_path):
    source, documents, inputs, _mesh, _material, _legacy = _fixture(tmp_path)
    _collection("bridge_root.composite")
    bpy.data.objects.new("bridge_root__node_0000", None)
    twin = _collection("unrelated_mh")
    twin[COLLECTION_KIND_KEY] = "mesh"
    twin[COLLECTION_RESOURCE_KEY] = "bridge_mesh"
    malformed = _collection("malformed_claim")
    malformed[COLLECTION_KIND_KEY] = {"broken": 1}
    malformed[COLLECTION_RESOURCE_KEY] = "bridge_mesh"
    before = _snapshot()
    assert len(_prepare(source, documents, inputs).to_publish) == 4
    assert _snapshot() == before
    assert _files(source) == {}


@pytest.mark.parametrize("mode", [CLOSURE_MODE_ROOT, CLOSURE_MODE_COMPOSITES])
def test_excluded_missing_mesh_names_owner_and_include_all_command(tmp_path, mode):
    source, documents, inputs, _mesh, _material, _legacy = _fixture(tmp_path)
    if mode == CLOSURE_MODE_ROOT:
        (source / "bridge_child.composite").write_bytes(
            composite_json_bytes(documents["bridge_child"]))
    before = _files(source)
    with pytest.raises(ValueError, match="MH_E_RESOURCE_NOT_FOUND") as caught:
        _prepare(source, documents, inputs, mode=mode)
    assert "static_mesh:bridge_mesh" in str(caught.value)
    assert "composite:bridge_child" in str(caught.value)
    assert "Export Composite Include All Stuff" in str(caught.value)
    assert _files(source) == before


def test_root_only_requires_nested_source_even_when_dto_is_loaded(tmp_path):
    source, documents, inputs, _mesh, _material, _legacy = _fixture(tmp_path)
    with pytest.raises(ValueError, match="MH_E_RESOURCE_NOT_FOUND") as caught:
        _prepare(source, documents, inputs, mode=CLOSURE_MODE_ROOT)
    assert "composite:bridge_child" in str(caught.value)
    assert "composite:bridge_root" in str(caught.value)
    assert "Export Composite Include All Stuff" in str(caught.value)
    assert _files(source) == {}


def _json_documents():
    return {
        "bridge_root": Composite("bridge_root", [
            Node("composite", resource="bridge_child")]),
        "bridge_child": Composite("bridge_child", [Node("group")]),
    }


@pytest.mark.parametrize("mode", [CLOSURE_MODE_ROOT, CLOSURE_MODE_COMPOSITES,
                                  CLOSURE_MODE_INCLUDE_ALL])
def test_three_modes_reuse_canonical_json_against_source_without_stamps(tmp_path, mode):
    source = tmp_path / "source"
    source.mkdir()
    documents = _json_documents()
    _publish(_prepare(source, documents, {}), tmp_path)
    before = _files(source)
    mtimes = {path.name: path.stat().st_mtime_ns for path in source.iterdir()}
    scene = _snapshot()
    retry = _prepare(source, documents, {}, mode=mode)
    assert retry.mode == mode and retry.to_publish == ()
    report = _publish(retry, tmp_path, "retry")
    assert report["published"] == []
    assert report["reused"] == ["composite:bridge_child", "composite:bridge_root"]
    assert _files(source) == before
    assert {path.name: path.stat().st_mtime_ns for path in source.iterdir()} == mtimes
    assert _snapshot() == scene


def test_changed_child_json_publishes_with_unchanged_root_reused(tmp_path):
    source = tmp_path / "source"
    source.mkdir()
    documents = _json_documents()
    _publish(_prepare(source, documents, {}), tmp_path)
    before = (source / "bridge_root.composite").stat().st_mtime_ns
    documents["bridge_child"].nodes[0].name = "changed"
    plan = _prepare(source, documents, {})
    assert [str(row.key) for row in plan.to_publish] == ["composite:bridge_child"]
    _publish(plan, tmp_path, "retry")
    assert (source / "bridge_root.composite").stat().st_mtime_ns == before


def test_loaded_material_with_identical_source_is_not_rewritten(tmp_path):
    source, documents, inputs, _mesh, material, _legacy = _fixture(tmp_path)
    prepared = prepare_blender_material_export(material, source, source_root=source)
    prepared.target.write_bytes(prepared.payload)
    before = prepared.target.stat().st_mtime_ns
    plan = _prepare(source, documents, inputs)
    assert plan.row_for(ResourceKey("material", "bridge_surface")).action == "reuse"
    _publish(plan, tmp_path)
    assert prepared.target.stat().st_mtime_ns == before


def test_nonselected_option_and_profile_are_full_closure_members(tmp_path):
    source, documents, inputs, _mesh, _material, _legacy = _fixture(tmp_path)
    documents["bridge_root"].nodes = [Node(
        "random", profile="scatter", options=[
            RandomOption("empty", 1.0),
            RandomOption("composite", 0.0, "bridge_child")])]
    path = source / "scatter.placement"
    payload = placement_json_bytes(PlacementProfile("scatter"))
    path.write_bytes(payload)
    before = path.stat().st_mtime_ns
    plan = _prepare(source, documents, inputs)
    assert plan.payloads[0].key == ResourceKey("placement_profile", "scatter")
    assert plan.payloads[0].action == "reuse"
    assert ResourceKey("static_mesh", "bridge_mesh") in plan.full_closure_keys
    _publish(plan, tmp_path)
    assert path.read_bytes() == payload
    assert path.stat().st_mtime_ns == before


def test_source_only_mesh_and_material_are_reused_without_rewriting(tmp_path):
    source, documents, inputs, _mesh, material, _legacy = _fixture(tmp_path)
    _publish(_prepare(source, documents, inputs), tmp_path)
    material.mh4blend.material_class = "unrelated_loaded_change"
    before = _files(source)
    mtimes = {path.name: path.stat().st_mtime_ns for path in source.iterdir()}
    scene = _snapshot()
    retry = _prepare(source, documents, {})
    assert retry.to_publish == ()
    _publish(retry, tmp_path, "retry")
    assert _files(source) == before
    assert {path.name: path.stat().st_mtime_ns for path in source.iterdir()} == mtimes
    assert _snapshot() == scene


@pytest.mark.parametrize("option", [False, True])
def test_marker_token_never_requires_own_source_payload(tmp_path, option):
    source = tmp_path / "source"
    source.mkdir()
    node = (Node("random", options=[
        RandomOption("empty", 1.0), RandomOption("marker", 0.0, "dummy_pivot")])
        if option else Node("marker", resource="dummy_pivot"))
    documents = {"bridge_root": Composite("bridge_root", [node])}
    plan = _prepare(source, documents, {})
    assert plan.full_closure_keys == (ResourceKey("composite", "bridge_root"),)
    _publish(plan, tmp_path)
    assert set(_files(source)) == {"bridge_root.composite"}


def test_duplicate_source_identity_blocks_without_overwriting_either_file(tmp_path):
    source, documents, inputs, mesh, _material, _legacy = _fixture(tmp_path)
    (source / "nested").mkdir()
    (source / "bridge_mesh.mesh.fbx").write_bytes(b"first-existing-source")
    (source / "nested" / "bridge_mesh.mesh.fbx").write_bytes(b"second-existing-source")
    before = _files(source)
    with pytest.raises(ValueError, match="MH_E_AMBIGUOUS_RESOURCE_NAME"):
        _prepare(source, documents, inputs)
    assert _files(source) == before
    assert COLLECTION_KIND_KEY not in mesh


def test_missing_mesh_input_and_source_fails_before_publication(tmp_path):
    source, documents, _inputs, _mesh, _material, _legacy = _fixture(tmp_path)
    with pytest.raises(ValueError, match="MH_E_RESOURCE_NOT_FOUND") as caught:
        _prepare(source, documents, {})
    assert "composite:bridge_child" in str(caught.value)
    assert "Export Composite Include All Stuff" not in str(caught.value)
    assert _files(source) == {}


def test_invalid_mode_fails_without_side_effects(tmp_path):
    source, documents, inputs, _mesh, _material, _legacy = _fixture(tmp_path)
    before = _snapshot()
    with pytest.raises(ValueError, match="unsupported closure export mode"):
        _prepare(source, documents, inputs, mode="all_blender_data")
    assert _snapshot() == before
    assert _files(source) == {}


@pytest.mark.parametrize("mode", [CLOSURE_MODE_ROOT, CLOSURE_MODE_COMPOSITES])
def test_excluded_missing_material_names_composite_owner(tmp_path, mode):
    source, documents, inputs, _mesh, _material, _legacy = _fixture(tmp_path)
    _publish(_prepare(source, documents, inputs), tmp_path)
    (source / "bridge_surface.material").unlink()
    before = _files(source)
    scene = _snapshot()
    with pytest.raises(ValueError, match="MH_E_RESOURCE_NOT_FOUND") as caught:
        _prepare(source, documents, inputs, mode=mode)
    assert "material:bridge_surface" in str(caught.value)
    assert "composite:bridge_child" in str(caught.value)
    assert "Export Composite Include All Stuff" in str(caught.value)
    assert _files(source) == before
    assert _snapshot() == scene


def test_root_only_uses_published_child_not_excluded_loaded_changes(tmp_path):
    source = tmp_path / "source"
    source.mkdir()
    documents = _json_documents()
    _publish(_prepare(source, documents, {}), tmp_path)
    documents["bridge_child"].nodes = [Node("mesh", resource="not_published")]
    documents["bridge_root"].nodes[0].name = "explicit_root_change"
    before = (source / "bridge_child.composite").read_bytes()
    plan = _prepare(source, documents, {}, mode=CLOSURE_MODE_ROOT)
    assert plan.closure.static_meshes == ()
    assert [str(row.key) for row in plan.to_publish] == ["composite:bridge_root"]
    _publish(plan, tmp_path, "retry")
    assert (source / "bridge_child.composite").read_bytes() == before
