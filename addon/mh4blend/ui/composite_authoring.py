"""Typed Blender authoring data for Source Protocol v5 composites.

The PropertyGroup is the only authority.  ID custom properties are maintained
as a one-way diagnostic projection for tools which do not know mh4blend's RNA
types; no reader in this module consults those mirrors.
"""

import math

import bpy


KIND_MIRROR_KEY = "mh_composite_kind"
WEIGHT_MIRROR_KEY = "mh_random_weight"
OPTION_INDEX_MIRROR_KEY = "mh_random_option_index"
PROFILE_MIRROR_KEY = "mh_composite_profile"

_OPTION_KINDS = frozenset({"mesh", "actor", "composite", "empty", "gameobj"})


def _set_or_remove_mirror(owner, key, value, *, present=True):
    if present:
        owner[key] = value
    elif key in owner:
        del owner[key]


def _is_random_option(owner):
    parent = getattr(owner, "parent", None)
    settings = owner.mh4blend
    checker = getattr(settings, "is_property_set", None)
    has_explicit_index = (
        checker is None or checker("option_index"))
    return (parent is not None and parent.type == "EMPTY"
            and parent.mh4blend.kind == "random" and has_explicit_index)


def _update_kind(self, _context):
    owner = self.id_data
    value = self.kind
    _set_or_remove_mirror(
        owner, KIND_MIRROR_KEY, value, present=value != "unset")
    if not _is_random_option(owner):
        _set_or_remove_mirror(owner, WEIGHT_MIRROR_KEY, None, present=False)
        _set_or_remove_mirror(
            owner, OPTION_INDEX_MIRROR_KEY, None, present=False)


def _update_weight(self, _context):
    owner = self.id_data
    _set_or_remove_mirror(
        owner, WEIGHT_MIRROR_KEY, float(self.weight),
        present=_is_random_option(owner))


def _update_option_index(self, _context):
    owner = self.id_data
    value = int(self.option_index)
    is_option = _is_random_option(owner)
    _set_or_remove_mirror(
        owner, OPTION_INDEX_MIRROR_KEY, value,
        present=is_option)
    _set_or_remove_mirror(
        owner, WEIGHT_MIRROR_KEY, float(self.weight), present=is_option)


def _update_profile(self, _context):
    owner = self.id_data
    value = self.profile
    _set_or_remove_mirror(
        owner, PROFILE_MIRROR_KEY, value, present=bool(value))


class MHCompositeObjectProperties(bpy.types.PropertyGroup):
    """Authoritative per-object composite authoring state.

    Deliberately none of the numeric properties has an RNA min/max.  Invalid
    imported or hand-edited values must remain observable and fail closed at
    validation/export instead of being silently clamped into valid data.
    """

    kind: bpy.props.EnumProperty(
        name="Kind",
        description="Source Protocol v5 node or random-option kind",
        items=(
            ("unset", "Unset", "Not an MH composite node"),
            ("mesh", "Mesh", "Static mesh resource"),
            ("actor", "Actor", "Actor placeholder resource"),
            ("composite", "Composite", "Nested composite resource"),
            ("empty", "Empty", "Option which resolves to no resource"),
            ("group", "Group", "Transform-bearing structural node"),
            ("random", "Random", "Weighted random selection node"),
            ("gameobj", "GameObj", "Named non-executable Dagor gameObj; no resource asset"),
        ),
        default="unset",
        update=_update_kind,
    )
    weight: bpy.props.FloatProperty(
        name="Weight",
        description="Finite non-negative random option weight",
        default=1.0,
        update=_update_weight,
    )
    option_index: bpy.props.IntProperty(
        name="Option Index",
        description="Explicit semantic order of an option",
        default=-1,
        update=_update_option_index,
    )
    profile: bpy.props.StringProperty(
        name="Placement Profile",
        description=(
            "Canonical Source Protocol placement-profile logical name; "
            "empty means no profile"),
        default="",
        update=_update_profile,
    )
    # Source provenance carriers ratified by OPEN-V5-23. They are written to
    # the wire and hashed, never executed, and have no ID mirror: a mirror
    # would be a second, non-authoritative claim on the same statement.
    place_type: bpy.props.IntProperty(
        name="Place Type",
        description=(
            "Dagor source place_type carried as provenance; -1 means the "
            "source never stated one, which is not the same as zero"),
        default=-1,
    )
    appearance_seed_boundary: bpy.props.BoolProperty(
        name="Appearance Seed Boundary",
        description=(
            "Dagor source ignoreParentInstSeed carried for V5-S6.3; this "
            "slice only stores it and never derives anything from it"),
        default=False,
    )


def sync_typed_mirror(obj):
    """Project authoritative typed state to diagnostic ID mirrors."""

    settings = obj.mh4blend
    _set_or_remove_mirror(
        obj, KIND_MIRROR_KEY, settings.kind,
        present=settings.kind != "unset")
    is_option = _is_random_option(obj)
    _set_or_remove_mirror(
        obj, WEIGHT_MIRROR_KEY, float(settings.weight), present=is_option)
    _set_or_remove_mirror(
        obj, OPTION_INDEX_MIRROR_KEY, int(settings.option_index),
        present=is_option)
    _set_or_remove_mirror(
        obj, PROFILE_MIRROR_KEY, settings.profile,
        present=bool(settings.profile))


def _grammar(message):
    return ValueError(f"MH_E_COMPOSITE_GRAMMAR: {message}")


def _is_property_set(settings, name):
    checker = getattr(settings, "is_property_set", None)
    return checker is None or checker(name)


def _indexed_options(random_node, *, require_nonempty=True,
                     require_positive=True):
    if random_node is None or random_node.type != "EMPTY":
        raise _grammar("random authoring requires an Empty node")
    if random_node.mh4blend.kind != "random":
        raise _grammar(
            f"{random_node.name!r} is not a typed random node")

    by_index = {}
    positive = False
    for option in random_node.children:
        settings = option.mh4blend
        if not _is_property_set(settings, "option_index"):
            # Random nodes may also own ordinary wire children.  An explicit
            # typed option_index, never parentage or an ID mirror, is the
            # authored-option discriminator.
            continue
        if option.type != "EMPTY":
            raise _grammar(
                f"random option {option.name!r} must be an Empty")
        index = settings.option_index
        if isinstance(index, bool) or not isinstance(index, int) or index < 0:
            raise _grammar(
                f"random option {option.name!r} has invalid option_index")
        if index in by_index:
            raise ValueError(
                "MH_E_DUPLICATE_RANDOM_OPTION_INDEX: random node "
                f"{random_node.name!r} has duplicate option_index {index}")

        kind = settings.kind
        if kind not in _OPTION_KINDS:
            raise _grammar(
                f"random option {option.name!r} has invalid kind {kind!r}")
        if settings.profile:
            raise _grammar(
                f"random option {option.name!r} cannot carry placement "
                "profile authority")
        resource = option.instance_collection
        if kind == "empty" and resource is not None:
            raise _grammar(
                f"empty random option {option.name!r} forbids a resource")
        if (kind not in {"empty", "gameobj"} and resource is None
                and not bool(option.get("mh_unresolved_placement", False))):
            raise _grammar(
                f"random option {option.name!r} requires a resource")

        weight = settings.weight
        if isinstance(weight, bool) or not isinstance(weight, (int, float)):
            raise _grammar(
                f"random option {option.name!r} has a non-number weight")
        if not math.isfinite(float(weight)) or weight < 0.0:
            raise _grammar(
                f"random option {option.name!r} has invalid weight")
        positive = positive or weight > 0.0
        by_index[index] = option

    if require_nonempty and not by_index:
        raise _grammar(f"random node {random_node.name!r} has no options")
    if require_positive and by_index and not positive:
        raise _grammar(
            f"random node {random_node.name!r} has no positive option weight")
    return by_index


def validate_random_options(random_node):
    """Return options in their explicit semantic order or fail closed."""

    return tuple(
        option for _index, option in sorted(
            _indexed_options(random_node).items()))


def _active_random(context):
    node = context.active_object
    if node is None:
        raise _grammar("select a random Empty")
    if node.type != "EMPTY" or node.mh4blend.kind != "random":
        raise _grammar("active object must be a typed random Empty")
    return node


def _link_collection_for(context, random_node):
    context_collection = getattr(context, "collection", None)
    if (context_collection is not None
            and context_collection in random_node.users_collection):
        return context_collection
    collections = tuple(random_node.users_collection)
    if len(collections) == 1:
        return collections[0]
    if not collections:
        raise _grammar(
            f"random node {random_node.name!r} is not linked to a collection")
    raise _grammar(
        f"random node {random_node.name!r} is linked to multiple collections; "
        "make one of them active")


def _report_failure(operator, exc):
    operator.report({"ERROR"}, str(exc))
    return {"CANCELLED"}


class _MHRandomOptionOperator:
    @classmethod
    def poll(cls, context):
        node = getattr(context, "active_object", None)
        return (node is not None and node.type == "EMPTY"
                and node.mh4blend.kind == "random")


class MH_OT_random_option_add(_MHRandomOptionOperator, bpy.types.Operator):
    bl_idname = "mh.random_option_add"
    bl_label = "Add Random Option"
    bl_description = "Add an explicitly indexed empty option"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        option = None
        try:
            random_node = _active_random(context)
            current = _indexed_options(
                random_node, require_nonempty=False, require_positive=False)
            index = max(current, default=-1) + 1
            option = bpy.data.objects.new(
                f"{random_node.name}_option_{index}", None)
            _link_collection_for(context, random_node).objects.link(option)
            option.parent = random_node
            option.instance_type = "COLLECTION"
            settings = option.mh4blend
            settings.kind = "empty"
            settings.weight = 1.0
            settings.option_index = index
            sync_typed_mirror(option)
        except (RuntimeError, ValueError) as exc:
            if option is not None and option.name in bpy.data.objects:
                bpy.data.objects.remove(option, do_unlink=True)
            return _report_failure(self, exc)
        return {"FINISHED"}


class _MHIndexedRandomOptionOperator(_MHRandomOptionOperator):
    option_index: bpy.props.IntProperty(
        name="Option Index", options={"HIDDEN"}, default=-1)

    def resolve_options(self, context):
        random_node = _active_random(context)
        options = _indexed_options(
            random_node, require_nonempty=True, require_positive=False)
        if self.option_index not in options:
            raise _grammar(
                f"random option index {self.option_index} does not exist")
        return random_node, options


class MH_OT_random_option_remove(
        _MHIndexedRandomOptionOperator, bpy.types.Operator):
    bl_idname = "mh.random_option_remove"
    bl_label = "Remove Random Option"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        try:
            _random_node, options = self.resolve_options(context)
            bpy.data.objects.remove(
                options[self.option_index], do_unlink=True)
        except (RuntimeError, ValueError) as exc:
            return _report_failure(self, exc)
        return {"FINISHED"}


class _MHMoveRandomOptionOperator(_MHIndexedRandomOptionOperator):
    direction = 0

    def execute(self, context):
        try:
            _random_node, options = self.resolve_options(context)
            ordered_indices = sorted(options)
            position = ordered_indices.index(self.option_index)
            other_position = position + self.direction
            if not 0 <= other_position < len(ordered_indices):
                raise _grammar(
                    f"random option index {self.option_index} cannot move "
                    f"{'up' if self.direction < 0 else 'down'}")
            other_index = ordered_indices[other_position]
            option = options[self.option_index]
            other = options[other_index]
            option.mh4blend.option_index = other_index
            other.mh4blend.option_index = self.option_index
            sync_typed_mirror(option)
            sync_typed_mirror(other)
        except (RuntimeError, ValueError) as exc:
            return _report_failure(self, exc)
        return {"FINISHED"}


class MH_OT_random_option_up(
        _MHMoveRandomOptionOperator, bpy.types.Operator):
    bl_idname = "mh.random_option_up"
    bl_label = "Move Random Option Up"
    bl_options = {"REGISTER", "UNDO"}
    direction = -1


class MH_OT_random_option_down(
        _MHMoveRandomOptionOperator, bpy.types.Operator):
    bl_idname = "mh.random_option_down"
    bl_label = "Move Random Option Down"
    bl_options = {"REGISTER", "UNDO"}
    direction = 1


def draw_random_options(layout, context):
    """Draw the Options authoring box for the active typed random Empty."""

    random_node = getattr(context, "active_object", None)
    if random_node is None or random_node.type != "EMPTY":
        return

    node = layout.row(align=True)
    node.label(text="Selected Node")
    node.prop(random_node.mh4blend, "kind", text="")
    if not _is_random_option(random_node):
        profile = layout.row(align=True)
        profile.label(text="Placement Profile")
        profile.prop(random_node.mh4blend, "profile", text="")
    if random_node.mh4blend.kind != "random":
        return

    box = layout.box()
    header = box.row(align=True)
    header.label(text="Random Options", icon="OUTLINER_OB_EMPTY")
    header.operator("mh.random_option_add", text="", icon="ADD")

    try:
        indexed = _indexed_options(
            random_node, require_nonempty=False, require_positive=False)
        rows = tuple(sorted(indexed.items()))
    except ValueError as exc:
        warning = box.row()
        warning.alert = True
        warning.label(text=str(exc), icon="ERROR")
        rows = tuple(
            (option.mh4blend.option_index, option)
            for option in random_node.children
            if _is_property_set(option.mh4blend, "option_index"))

    if not rows:
        box.label(text="No options", icon="INFO")
        return

    for position, (index, option) in enumerate(rows):
        row = box.row(align=True)
        row.label(text=str(index))
        row.prop(option.mh4blend, "kind", text="")
        row.prop(option, "instance_collection", text="")
        weight = row.row(align=True)
        weight.alert = (
            not math.isfinite(float(option.mh4blend.weight))
            or option.mh4blend.weight < 0.0)
        weight.prop(option.mh4blend, "weight", text="W")
        up = row.row(align=True)
        up.enabled = position > 0
        op = up.operator("mh.random_option_up", text="", icon="TRIA_UP")
        op.option_index = index
        down = row.row(align=True)
        down.enabled = position + 1 < len(rows)
        op = down.operator(
            "mh.random_option_down", text="", icon="TRIA_DOWN")
        op.option_index = index
        op = row.operator(
            "mh.random_option_remove", text="", icon="REMOVE")
        op.option_index = index


CLASSES = (
    MHCompositeObjectProperties,
    MH_OT_random_option_add,
    MH_OT_random_option_remove,
    MH_OT_random_option_up,
    MH_OT_random_option_down,
)


def register():
    for cls in CLASSES:
        bpy.utils.register_class(cls)
    bpy.types.Object.mh4blend = bpy.props.PointerProperty(
        name="MH Composite", type=MHCompositeObjectProperties)


def unregister():
    if hasattr(bpy.types.Object, "mh4blend"):
        delattr(bpy.types.Object, "mh4blend")
    for cls in reversed(CLASSES):
        bpy.utils.unregister_class(cls)
