"""Real-writer gates for the explicit dag4blend publication planner."""

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
from mh4blend.scene.dag4blend_publication import prepare_dag4blend_publication  # noqa: E402
from mh4blend.scene.export_closure import (  # noqa: E402
    CLOSURE_MODE_INCLUDE_ALL,
    _finalize_published_blender_state,
    prepare_composite_closure_export,
    publish_composite_closure_export,
    stage_composite_closure_export,
)
from mh4blend.scene.import_composite import (  # noqa: E402
    _placement_name,
    materialize_composite_documents,
)
from mh4blend.scene.import_fbx import parse_mesh_fbx  # noqa: E402
from mh4blend.scene.resource_markers import (  # noqa: E402
    COLLECTION_KIND_KEY,
    COLLECTION_RESOURCE_KEY,
    INCOMPLETE_IMPORT_KEY,
    stamp_resource_collection,
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


def _triangle(collection, name, material):
    data = bpy.data.meshes.new(name + "_data")
    data.from_pydata([(0, 0, 0), (1, 0, 0), (0, 1, 0)], [], [(0, 1, 2)])
    data.materials.append(material)
    obj = bpy.data.objects.new(name, data)
    collection.objects.link(obj)
    return obj


def _fixture(tmp_path, *, lods=False):
    source = tmp_path / "source"
    source.mkdir()
    material = bpy.data.materials.new("bridge_surface")
    material.mh4blend.material_class = "bridge_shader"
    mesh = _collection("bridge_mesh.lods" if lods else "bridge_mesh")
    mesh["type"] = "rendinst"
    mesh["name"] = "bridge_mesh"
    if lods:
        for level in range(2):
            child = bpy.data.collections.new(f"bridge_mesh.lod{level:02d}")
            mesh.children.link(child)
            _triangle(child, f"triangle{level}", material)
    else:
        _triangle(mesh, "triangle", material)
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
    return prepare_dag4blend_publication(
        documents, inputs, root_name="bridge_root", source_root=source,
        output_dir=source, **kwargs)


def _files(root):
    return {str(path.relative_to(root)): path.read_bytes()
            for path in root.rglob("*") if path.is_file()}


def _closed_prefix(source):
    if (source / "bridge_mesh.mesh.fbx").exists():
        assert (source / "bridge_surface.material").exists()
    if (source / "bridge_child.composite").exists():
        assert (source / "bridge_mesh.mesh.fbx").exists()
    if (source / "bridge_root.composite").exists():
        assert (source / "bridge_child.composite").exists()


@pytest.mark.parametrize("lods", [False, True])
def test_real_batch_then_mesh_override_avoids_occupied_legacy_ids(tmp_path, lods):
    source, documents, inputs, mesh, _material, legacy = _fixture(
        tmp_path, lods=lods)
    names = tuple(obj.name for obj in mesh.all_objects)
    data = tuple(obj.data for obj in mesh.all_objects)
    legacy_properties = dict(legacy.items())
    plan = _prepare(source, documents, inputs)
    assert _files(source) == {}
    assert COLLECTION_KIND_KEY not in mesh
    assert all(row.prepared is None for row in plan.payloads
               if row.key.kind == "composite")
    staging = tmp_path / "stage"
    staging.mkdir()
    staged = stage_composite_closure_export(plan, staging_dir=staging)
    assert _files(source) == {}
    assert COLLECTION_KIND_KEY not in mesh
    events = []

    def observe(event, item, _published):
        if event == "after_replace":
            events.append(item.identity)
            _closed_prefix(source)

    report = publish_composite_closure_export(
        plan, staged, lock_root=tmp_path / "locks", _boundary_hook=observe)
    assert events == [
        "material:bridge_surface", "static_mesh:bridge_mesh",
        "composite:bridge_child", "composite:bridge_root"]
    assert COLLECTION_KIND_KEY not in mesh  # The low-level API never stamps.
    parsed = parse_mesh_fbx(source / "bridge_mesh.mesh.fbx")
    assert parsed.resource_name == "bridge_mesh"
    assert parsed.material_names == ("bridge_surface",)

    # Model the caller's post-success adoption, then use the unchanged importer.
    _finalize_published_blender_state(plan, report["published"])
    materialized = materialize_composite_documents(
        documents, root_name="bridge_root", source_root=source,
        resource_overrides=inputs)
    assert bpy.data.collections.get(mesh.name) is mesh
    assert tuple(obj.name for obj in mesh.all_objects) == names
    assert tuple(obj.data for obj in mesh.all_objects) == data
    assert dict(legacy.items()) == legacy_properties
    assert bpy.data.collections.get("bridge_root") is legacy
    child = bpy.data.collections["bridge_child.composite"]
    assert next(iter(child.objects)).instance_collection is mesh
    assert _prepare(source, documents, inputs).closure == plan.closure
    strict = prepare_composite_closure_export(
        materialized["collection"], source, source_root=source,
        mode=CLOSURE_MODE_INCLUDE_ALL)
    assert strict.closure == plan.closure


@pytest.mark.parametrize("boundary", ["before_replace", "after_replace"])
@pytest.mark.parametrize("failure_index", range(4))
def test_every_publish_boundary_reports_closed_prefix_and_allows_retry(
        tmp_path, boundary, failure_index):
    source, documents, inputs, mesh, _material, _legacy = _fixture(tmp_path)
    plan = _prepare(source, documents, inputs)
    identities = tuple(str(row.key) for row in plan.to_publish)
    staging = tmp_path / "stage"
    staging.mkdir()
    staged = stage_composite_closure_export(plan, staging_dir=staging)

    def inject(event, item, _published):
        _closed_prefix(source)
        if event == boundary and item.identity == identities[failure_index]:
            raise RuntimeError("injected publication boundary")

    count = failure_index + (boundary == "after_replace")
    error_type = BatchPartialPublishError if count else RuntimeError
    with pytest.raises(error_type, match="injected publication boundary") as caught:
        publish_composite_closure_export(
            plan, staged, lock_root=tmp_path / "locks", _boundary_hook=inject)
    _closed_prefix(source)
    assert COLLECTION_KIND_KEY not in mesh
    if count:
        assert caught.value.published == identities[:count]
        assert caught.value.unpublished == identities[count:]
        _finalize_published_blender_state(plan, caught.value.published)
    assert (COLLECTION_KIND_KEY in mesh) == (count >= 2)

    retry = _prepare(source, documents, inputs)
    retry_dir = tmp_path / "retry_stage"
    retry_dir.mkdir()
    retry_staged = stage_composite_closure_export(retry, staging_dir=retry_dir)
    report = publish_composite_closure_export(
        retry, retry_staged, lock_root=tmp_path / "locks")
    assert report["published"] == list(identities)
    _closed_prefix(source)
    assert (source / "bridge_root.composite").read_bytes() == (
        composite_json_bytes(documents["bridge_root"]))


@pytest.mark.parametrize("failure", [
    "material", "texture", "partial_stamp", "foreign_partial", "incomplete", "managed_twin",
    "wrong_identity", "occupied_composite", "occupied_object", "reuse_differs",
])
def test_preflight_failure_writes_no_files_or_mesh_stamps(tmp_path, failure):
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
    elif failure == "foreign_partial":
        _collection("partial_claim")[COLLECTION_RESOURCE_KEY] = "bridge_mesh"
    elif failure == "incomplete":
        mesh[INCOMPLETE_IMPORT_KEY] = True
    elif failure == "managed_twin":
        twin = _collection("twin")
        stamp_resource_collection(twin, "mesh", "bridge_mesh")
    elif failure == "wrong_identity":
        mesh.name = "different_identity"
    elif failure == "occupied_composite":
        _collection("bridge_root.composite")
    elif failure == "occupied_object":
        bpy.data.objects.new(_placement_name("bridge_root", 0), None)
    elif failure == "reuse_differs":
        existing = _collection("bridge_root.composite")
        stamp_resource_collection(existing, "composite", "bridge_root")
    before_properties = dict(mesh.items())
    before_names = tuple(row.name for row in bpy.data.collections)
    with pytest.raises((ValueError, TypeError)):
        _prepare(source, documents, inputs)
    assert _files(source) == {}
    assert dict(mesh.items()) == before_properties
    assert tuple(row.name for row in bpy.data.collections) == before_names


def test_nonselected_option_and_profile_are_full_closure_members(tmp_path):
    source, documents, inputs, _mesh, _material, _legacy = _fixture(tmp_path)
    documents["bridge_root"].nodes = [Node(
        "random", profile="scatter", options=[
            RandomOption("empty", 1.0),
            RandomOption("composite", 0.0, "bridge_child")])]
    profile_path = source / "scatter.placement"
    payload = placement_json_bytes(PlacementProfile("scatter"))
    profile_path.write_bytes(payload)
    before = profile_path.stat().st_mtime_ns
    plan = _prepare(source, documents, inputs)
    assert plan.payloads[0].key == ResourceKey("placement_profile", "scatter")
    assert plan.payloads[0].action == "reuse"
    assert ResourceKey("static_mesh", "bridge_mesh") in plan.full_closure_keys
    staging = tmp_path / "stage"
    staging.mkdir()
    publish_composite_closure_export(
        plan, stage_composite_closure_export(plan, staging_dir=staging),
        lock_root=tmp_path / "locks")
    assert profile_path.read_bytes() == payload
    assert profile_path.stat().st_mtime_ns == before


def test_source_only_mesh_is_reused_without_rewriting_payload(tmp_path):
    source, documents, inputs, mesh, _material, _legacy = _fixture(tmp_path)
    plan = _prepare(source, documents, inputs)
    staging = tmp_path / "stage"
    staging.mkdir()
    publish_composite_closure_export(
        plan, stage_composite_closure_export(plan, staging_dir=staging),
        lock_root=tmp_path / "locks")
    # Leave no loaded mesh definition occupying the ordinary import target.
    mesh_objects = list(mesh.all_objects)
    bpy.data.batch_remove([*mesh_objects, mesh])
    before = _files(source)
    retry = _prepare(source, documents, {})
    row = retry.row_for(ResourceKey("static_mesh", "bridge_mesh"))
    assert row.action == "reuse"
    assert row.payload == before["bridge_mesh.mesh.fbx"]
    assert _files(source) == before


@pytest.mark.parametrize("zero_weight_option", [False, True])
def test_actor_publication_stops_at_open_v5_19(tmp_path, zero_weight_option):
    source = tmp_path / "source"
    source.mkdir()
    node = (Node("random", options=[
        RandomOption("empty", 1.0), RandomOption("actor", 0.0, "dummy_pivot")])
        if zero_weight_option else Node("actor", resource="dummy_pivot"))
    documents = {"bridge_root": Composite("bridge_root", [node])}
    with pytest.raises(ValueError, match="OPEN-V5-19"):
        _prepare(source, documents, {})
    assert _files(source) == {}


def test_refresh_is_pending_without_changing_canonical_import_policy(tmp_path):
    source, documents, inputs, _mesh, _material, _legacy = _fixture(tmp_path)
    with pytest.raises(ValueError, match="refresh"):
        _prepare(source, documents, inputs, definition_policy="refresh")
    assert _files(source) == {}


def test_missing_mesh_input_and_source_fail_before_publication(tmp_path):
    source, documents, _inputs, _mesh, _material, _legacy = _fixture(tmp_path)
    with pytest.raises(ValueError, match="MH_E_RESOURCE_NOT_FOUND"):
        _prepare(source, documents, {})
    assert _files(source) == {}


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


@pytest.mark.parametrize("malformed_kind", [{"broken": 1}, [1, 2]])
def test_malformed_competing_kind_has_a_named_diagnostic(tmp_path, malformed_kind):
    source, documents, inputs, mesh, _material, _legacy = _fixture(tmp_path)
    malformed = _collection("malformed_claim")
    malformed[COLLECTION_KIND_KEY] = malformed_kind
    malformed[COLLECTION_RESOURCE_KEY] = "bridge_mesh"
    with pytest.raises(ValueError, match="MH_E_IMPORT_TARGET_OCCUPIED"):
        _prepare(source, documents, inputs)
    assert _files(source) == {}
    assert COLLECTION_KIND_KEY not in mesh


def test_reused_composite_cannot_hide_unmanaged_inner_composite_binding(tmp_path):
    source, documents, inputs, mesh, _material, _legacy = _fixture(tmp_path)
    old_child = _collection("bridge_child")
    old_child["type"] = "composit"
    old_child["name"] = "bridge_child"
    existing = _collection("bridge_root.composite")
    stamp_resource_collection(existing, "composite", "bridge_root")
    obj = bpy.data.objects.new("existing_placement", None)
    existing.objects.link(obj)
    obj.mh4blend.kind = "composite"
    obj.instance_type = "COLLECTION"
    obj.instance_collection = old_child
    with pytest.raises(ValueError, match="unmanaged definition"):
        _prepare(source, documents, inputs)
    assert _files(source) == {}
    assert COLLECTION_KIND_KEY not in mesh
    assert obj.instance_collection is old_child
