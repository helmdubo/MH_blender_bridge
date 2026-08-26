"""Blender-hosted gates for V5-S3 typed random authoring."""

from pathlib import Path
import sys
from types import SimpleNamespace

import pytest

bpy = pytest.importorskip("bpy")
from mathutils import Matrix

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "addon"))

from mh4blend.core.composites import composite_json_bytes, parse_composite  # noqa: E402
from mh4blend.core.model import Composite, Node, RandomOption  # noqa: E402
from mh4blend.scene.export_composite import (  # noqa: E402
    NODE_KIND_KEY,
    export_composite_collection,
)
from mh4blend.scene import import_composite as import_module  # noqa: E402
from mh4blend.scene.import_composite import (  # noqa: E402
    import_composite_file,
    materialize_composite_documents,
)
from mh4blend.scene.import_fbx import LOAD_MODE_STRUCTURE_ONLY  # noqa: E402
from mh4blend.scene.resource_markers import (  # noqa: E402
    INCOMPLETE_IMPORT_KEY,
    stamp_resource_collection,
)
from mh4blend.scene.service_scenes import (  # noqa: E402
    SERVICE_SCENE_NAMES,
    ensure_service_scenes,
)
from mh4blend.ui.composite_authoring import (  # noqa: E402
    OPTION_INDEX_MIRROR_KEY,
    WEIGHT_MIRROR_KEY,
    sync_typed_mirror,
    validate_random_options,
)
from mh4blend.ui import composite_authoring  # noqa: E402


@pytest.fixture(autouse=True)
def registered_addon():
    owned = not hasattr(bpy.types.Object, "mh4blend")
    if owned:
        composite_authoring.register()
    try:
        yield
    finally:
        if owned:
            composite_authoring.unregister()


def _write(path, composite):
    path.write_bytes(composite_json_bytes(composite))
    return path


def _option_objects(random_node):
    return tuple(validate_random_options(random_node))


def test_random_import_roundtrip_uses_typed_authority_and_all_options(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    assert all(bpy.data.scenes.get(name) is None for name in SERVICE_SCENE_NAMES)

    source = Composite("random_root", [Node(
        "random",
        name="all_variants",
        options=[
            RandomOption("composite", 1.0, "variant_a"),
            RandomOption("mesh", 2.5, "variant_mesh"),
            RandomOption("actor", 0.0, "variant_actor"),
            RandomOption("empty", 0.25),
        ],
        children=[Node("actor", resource="always_spawned")],
    )])
    path = _write(tmp_path / "random_root.composite", source)
    expected = path.read_bytes()

    report = import_composite_file(path, source_root=tmp_path)
    assert report["service_scenes"] == list(SERVICE_SCENE_NAMES)
    assert all(bpy.data.scenes.get(name) is not None for name in SERVICE_SCENE_NAMES)
    assert len(bpy.data.scenes["TECH"].collection.children) == 0

    random_node = next(
        obj for obj in report["collection"].objects
        if obj.mh4blend.kind == "random")
    assert random_node[NODE_KIND_KEY] == "random"
    options = _option_objects(random_node)
    assert [option.mh4blend.option_index for option in options] == [0, 1, 2, 3]
    assert [option.mh4blend.kind for option in options] == [
        "composite", "mesh", "actor", "empty"]
    assert [option.mh4blend.weight for option in options] == pytest.approx(
        [1.0, 2.5, 0.0, 0.25])
    assert [option[OPTION_INDEX_MIRROR_KEY] for option in options] == [0, 1, 2, 3]
    assert [option[WEIGHT_MIRROR_KEY] for option in options] == pytest.approx(
        [1.0, 2.5, 0.0, 0.25])

    ordinary_children = [
        child for child in random_node.children
        if not child.mh4blend.is_property_set("option_index")]
    assert len(ordinary_children) == 1
    assert ordinary_children[0].mh4blend.kind == "actor"

    actor_option = options[2]
    assert actor_option.instance_collection is not None
    assert actor_option.instance_collection.name == "variant_actor.actor"
    for resource in (
            option.instance_collection for option in options
            if option.instance_collection is not None):
        assert WEIGHT_MIRROR_KEY not in resource
        assert OPTION_INDEX_MIRROR_KEY not in resource

    exported = export_composite_collection(
        report["collection"], tmp_path, source_root=tmp_path)
    assert Path(exported["filepath"]).read_bytes() == expected

    # Option matrices are display-only and diagnostic mirrors are projections.
    options[0].matrix_basis = Matrix.Translation((123.0, -9.0, 4.0))
    options[1][WEIGHT_MIRROR_KEY] = 999.0
    options[1][OPTION_INDEX_MIRROR_KEY] = 99
    exported = export_composite_collection(
        report["collection"], tmp_path, source_root=tmp_path)
    assert Path(exported["filepath"]).read_bytes() == expected
    assert options[1].mh4blend.weight == pytest.approx(2.5)
    assert options[1].mh4blend.option_index == 1
    assert options[1][WEIGHT_MIRROR_KEY] == pytest.approx(2.5)
    assert options[1][OPTION_INDEX_MIRROR_KEY] == 1


def _make_random_collection(name="authored_random"):
    collection = bpy.data.collections.new(name)
    bpy.context.scene.collection.children.link(collection)
    random_node = bpy.data.objects.new("random_node", None)
    collection.objects.link(random_node)
    random_node.mh4blend.kind = "random"
    sync_typed_mirror(random_node)
    return collection, random_node


def _add_empty_option(collection, parent, name, index, weight):
    option = bpy.data.objects.new(name, None)
    collection.objects.link(option)
    option.parent = parent
    option.mh4blend.kind = "empty"
    option.mh4blend.weight = weight
    option.mh4blend.option_index = index
    sync_typed_mirror(option)
    return option


def test_explicit_indices_control_order_and_duplicate_fails_closed(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection, random_node = _make_random_collection()
    first = _add_empty_option(collection, random_node, "z_name", 0, 1.0)
    second = _add_empty_option(collection, random_node, "a_name", 1, 2.0)

    initial = export_composite_collection(
        collection, tmp_path, source_root=tmp_path)
    decoded = parse_composite(Path(initial["filepath"]).read_bytes())
    assert [option.weight for option in decoded.nodes[0].options] == [1.0, 2.0]

    first.mh4blend.option_index = 1
    second.mh4blend.option_index = 0
    reordered = export_composite_collection(
        collection, tmp_path, source_root=tmp_path)
    decoded = parse_composite(Path(reordered["filepath"]).read_bytes())
    assert [option.weight for option in decoded.nodes[0].options] == [2.0, 1.0]

    first.mh4blend.option_index = 0
    first[WEIGHT_MIRROR_KEY] = 123.0
    with pytest.raises(
            ValueError, match="MH_E_DUPLICATE_RANDOM_OPTION_INDEX"):
        export_composite_collection(
            collection, tmp_path, source_root=tmp_path)
    assert first[WEIGHT_MIRROR_KEY] == pytest.approx(123.0)


def test_invalid_option_data_is_not_repaired(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection, random_node = _make_random_collection("invalid_random")
    option = _add_empty_option(collection, random_node, "bad", -1, 1.0)
    assert option.mh4blend.option_index == -1
    with pytest.raises(ValueError, match="invalid option_index"):
        export_composite_collection(
            collection, tmp_path, source_root=tmp_path)

    option.mh4blend.option_index = 0
    option.mh4blend.weight = -1.0
    assert option.mh4blend.weight == pytest.approx(-1.0)
    with pytest.raises(ValueError, match="invalid weight"):
        export_composite_collection(
            collection, tmp_path, source_root=tmp_path)


def test_options_operators_swap_only_explicit_indices_and_do_not_renumber():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    _collection, random_node = _make_random_collection("operator_random")
    random_node.select_set(True)
    bpy.context.view_layer.objects.active = random_node

    assert bpy.ops.mh.random_option_add() == {"FINISHED"}
    assert bpy.ops.mh.random_option_add() == {"FINISHED"}
    assert [row.mh4blend.option_index for row in _option_objects(random_node)] == [0, 1]

    assert bpy.ops.mh.random_option_up(option_index=1) == {"FINISHED"}
    by_name = {row.name: row.mh4blend.option_index for row in random_node.children}
    assert sorted(by_name.values()) == [0, 1]

    assert bpy.ops.mh.random_option_remove(option_index=0) == {"FINISHED"}
    remaining = list(random_node.children)
    assert len(remaining) == 1
    assert remaining[0].mh4blend.option_index == 1


def test_dag4blend_resource_override_is_explicit_and_routed_after_commit(
        tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    resource = bpy.data.collections.new("ArtistFacingResource")
    resource["type"] = "rendinst"
    resource["name"] = "variant_mesh"
    bpy.context.scene.collection.children.link(resource)
    source = Composite("override_root", [Node(
        "random", options=[RandomOption("mesh", 1.0, "variant_mesh")])])

    report = materialize_composite_documents(
        {source.name: source},
        root_name=source.name,
        source_root=None,
        resource_overrides={("mesh", "variant_mesh"): resource},
        load_mode=LOAD_MODE_STRUCTURE_ONLY,
    )
    random_node = next(
        obj for obj in report["collection"].objects
        if obj.mh4blend.kind == "random")
    option = _option_objects(random_node)[0]
    managed = option.instance_collection
    assert managed is not resource
    assert managed.get(INCOMPLETE_IMPORT_KEY) is True
    assert len(managed.objects) == 0
    assert bpy.data.scenes["MESH"].collection.children.get(managed.name) is managed

    decoded_path = export_composite_collection(
        report["collection"], tmp_path, source_root=tmp_path)
    decoded = parse_composite(Path(decoded_path["filepath"]).read_bytes())
    assert decoded.nodes[0].options == [
        RandomOption("mesh", 1.0, "variant_mesh")]


def test_failed_materialization_does_not_link_existing_override():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    resource = bpy.data.collections.new("ExistingDagorResource")
    resource["type"] = "rendinst"
    resource["name"] = "variant_mesh"
    bpy.context.scene.collection.children.link(resource)
    source = Composite("occupied", [Node("mesh", resource="variant_mesh")])
    bpy.data.collections.new("occupied.composite")

    with pytest.raises(ValueError, match="MH_E_IMPORT_TARGET_OCCUPIED"):
        materialize_composite_documents(
            {source.name: source},
            root_name=source.name,
            source_root=None,
            resource_overrides={("mesh", "variant_mesh"): resource},
            load_mode=LOAD_MODE_STRUCTURE_ONLY,
        )
    assert bpy.data.scenes.get("MESH") is None


def test_resource_override_without_explicit_identity_fails_before_mutation():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    resource = bpy.data.collections.new("UnmarkedArtistCollection")
    bpy.context.scene.collection.children.link(resource)
    source = Composite("unmarked_override", [
        Node("mesh", resource="variant_mesh"),
    ])

    with pytest.raises(
            ValueError, match="MH_E_INVALID_RESOURCE_SOURCE"):
        materialize_composite_documents(
            {source.name: source},
            root_name=source.name,
            source_root=None,
            resource_overrides={("mesh", "variant_mesh"): resource},
        )
    assert bpy.data.scenes.get("MESH") is None
    assert bpy.data.collections.get("unmarked_override.composite") is None


def test_resource_collection_random_properties_block_export(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection, random_node = _make_random_collection("leaked_resource_state")
    resource = bpy.data.collections.new("resource")
    resource["type"] = "rendinst"
    resource["name"] = "resource"
    resource[WEIGHT_MIRROR_KEY] = 2.0
    option = bpy.data.objects.new("option", None)
    collection.objects.link(option)
    option.parent = random_node
    option.instance_type = "COLLECTION"
    option.instance_collection = resource
    option.mh4blend.kind = "mesh"
    option.mh4blend.weight = 1.0
    option.mh4blend.option_index = 0

    with pytest.raises(ValueError, match="must not carry random-option"):
        export_composite_collection(
            collection, tmp_path, source_root=tmp_path)


def test_random_option_outside_selected_collection_fails_closed(tmp_path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    collection, random_node = _make_random_collection("external_option")
    other = bpy.data.collections.new("foreign_authority")
    bpy.context.scene.collection.children.link(other)
    option = bpy.data.objects.new("foreign_option", None)
    other.objects.link(option)
    option.parent = random_node
    option.mh4blend.kind = "empty"
    option.mh4blend.weight = 1.0
    option.mh4blend.option_index = 0

    with pytest.raises(ValueError, match="MH_E_PARENT_OUTSIDE_RESOURCE"):
        export_composite_collection(
            collection, tmp_path, source_root=tmp_path)


def test_external_resource_routes_roll_back_lifo_on_second_link_failure(
        monkeypatch):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    service_scenes = ensure_service_scenes()
    overrides = {}
    for name in ("mesh_a", "mesh_b"):
        collection = bpy.data.collections.new(f"legacy_{name}")
        mesh = bpy.data.meshes.new(f"{name}_geometry")
        collection.objects.link(bpy.data.objects.new(f"{name}_render", mesh))
        stamp_resource_collection(collection, "mesh", name)
        bpy.context.scene.collection.children.link(collection)
        overrides[("mesh", name)] = collection
    source = Composite("rollback_root", [Node(
        "random",
        options=[
            RandomOption("mesh", 1.0, "mesh_a"),
            RandomOption("mesh", 1.0, "mesh_b"),
        ],
    )])
    real_children = service_scenes["MESH"].collection.children

    class FailingChildren:
        def __init__(self):
            self.links = 0

        def get(self, name):
            return real_children.get(name)

        def link(self, collection):
            self.links += 1
            if self.links == 2:
                raise RuntimeError("injected second route failure")
            real_children.link(collection)

        def unlink(self, collection):
            real_children.unlink(collection)

    patched = dict(service_scenes)
    patched["MESH"] = SimpleNamespace(
        collection=SimpleNamespace(children=FailingChildren()))
    monkeypatch.setattr(
        import_module, "ensure_service_scenes", lambda: patched)

    with pytest.raises(RuntimeError, match="injected second route failure"):
        materialize_composite_documents(
            {source.name: source},
            root_name=source.name,
            source_root=None,
            resource_overrides=overrides,
        )
    assert len(real_children) == 0
    assert bpy.data.collections.get("rollback_root.composite") is None
    assert all(bpy.data.scenes.get(name) is service_scenes[name]
               for name in SERVICE_SCENE_NAMES)


def test_full_lod_rejects_unmanaged_dag4blend_override():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    resource = bpy.data.collections.new("legacy_mesh")
    resource["type"] = "rendinst"
    resource["name"] = "variant_mesh"
    source = Composite("unmanaged_full", [
        Node("mesh", resource="variant_mesh"),
    ])

    with pytest.raises(ValueError, match="MH_E_INVALID_RESOURCE_SOURCE"):
        materialize_composite_documents(
            {source.name: source}, root_name=source.name,
            source_root=None,
            resource_overrides={("mesh", "variant_mesh"): resource})
    assert bpy.data.collections.get("unmanaged_full.composite") is None


def test_mixed_refresh_create_before_commit_failure_restores_exact_snapshot():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    child = Composite("refresh_child", [Node("empty")])
    root_document = Composite("refresh_root", [
        Node("composite", resource="refresh_child"),
    ])
    documents = {
        root_document.name: root_document,
        child.name: child,
    }
    first = materialize_composite_documents(
        documents, root_name=root_document.name, source_root=None)
    root = first["collection"]
    root["artist_state"] = "keep"
    pointer = root.as_pointer()
    external = bpy.data.objects.new("external_root_user", None)
    external.instance_type = "COLLECTION"
    external.instance_collection = root
    bpy.context.scene.collection.objects.link(external)

    child_collection = bpy.data.collections["refresh_child.composite"]
    bpy.data.batch_remove([*tuple(child_collection.objects), child_collection])
    assert bpy.data.collections.get("refresh_child.composite") is None
    old_objects = tuple(
        (obj.name, obj.as_pointer(), obj.instance_collection)
        for obj in root.objects)
    old_children = tuple(
        (collection.name, collection.as_pointer())
        for collection in root.children)
    old_properties = dict(root.items())

    def fail_publication(context):
        def fail_after_refresh_swap():
            raise RuntimeError("injected before-commit failure")

        context["transaction"].add_finalize(fail_after_refresh_swap)

    with pytest.raises(RuntimeError, match="injected before-commit failure"):
        materialize_composite_documents(
            documents, root_name=root_document.name, source_root=None,
            definition_policy="refresh", before_commit=fail_publication)

    assert root.as_pointer() == pointer
    assert external.instance_collection is root
    assert dict(root.items()) == old_properties
    assert tuple(
        (obj.name, obj.as_pointer(), obj.instance_collection)
        for obj in root.objects) == old_objects
    assert tuple(
        (collection.name, collection.as_pointer())
        for collection in root.children) == old_children
    assert bpy.data.collections.get("refresh_child.composite") is None
    assert not any(
        collection.name.startswith(".__mh_refresh")
        for collection in bpy.data.collections)
