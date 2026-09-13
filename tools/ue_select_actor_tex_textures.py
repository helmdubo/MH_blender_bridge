"""Select textures bound to texN / texN_suffix parameters in the Content Browser.

Run in Unreal Editor with actors selected, using Tools > Execute Python Script.
Reads explicit global overrides in MaterialInstanceConstant and parent instances.
Master-material textures are excluded, even when repeated in an MI override.
Does not modify or save assets. Material Layers parameters are outside
this helper's scope. Static-switch activity is not used to filter parameters.
"""

import re

import unreal


# Include meshes owned by attached actors (for composite/child actor hierarchies).
INCLUDE_ATTACHED_ACTORS = True
PARAMETER_PATTERN = re.compile(r"^tex\d+(?:_.*)?$", re.IGNORECASE)


def instance_overrides(material):
    """Walk child first; do not resolve missing overrides through the master."""
    overrides = {}
    visited = set()
    current = material
    while isinstance(current, unreal.MaterialInstanceConstant):
        path = current.get_path_name()
        if path in visited:
            unreal.log_warning("MH: Cyclic material parent chain: " + path)
            return {}, None
        visited.add(path)
        for value in current.get_editor_property("texture_parameter_values"):
            info = value.get_editor_property("parameter_info")
            if info.get_editor_property("association") != unreal.MaterialParameterAssociation.GLOBAL_PARAMETER:
                continue
            name = str(info.get_editor_property("name"))
            key = name.casefold()
            if PARAMETER_PATTERN.fullmatch(name) and key not in overrides:
                overrides[key] = (name, value.get_editor_property("parameter_value"))
        current = current.get_editor_property("parent")
    return overrides, current


def select_actor_tex_textures():
    subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    pending = list(subsystem.get_selected_level_actors())
    if not pending:
        unreal.log_warning("MH: Select an actor in the level first.")
        return []

    actors_seen = set()
    materials = {}
    while pending:
        actor = pending.pop()
        actor_path = actor.get_path_name()
        if actor_path in actors_seen:
            continue
        actors_seen.add(actor_path)
        for component in actor.get_components_by_class(unreal.MeshComponent):
            for material in component.get_materials():
                if isinstance(material, unreal.MaterialInstanceConstant):
                    materials[material.get_path_name()] = material
        if INCLUDE_ATTACHED_ACTORS:
            pending.extend(actor.get_attached_actors())

    library = unreal.MaterialEditingLibrary
    candidates = []
    masters = {}
    for material_path, material in sorted(materials.items()):
        overrides, master = instance_overrides(material)
        if isinstance(master, unreal.Material):
            masters[master.get_path_name()] = master
        valid_names = {str(name).casefold() for name in library.get_texture_parameter_names(material)}
        for key, (name, texture) in sorted(overrides.items()):
            if key in valid_names and texture is not None:
                candidates.append((material_path, name, texture))

    # Protect textures used by any selected MI's master, including fixed samples
    # and parameter defaults that an MI may explicitly repeat as placeholders.
    master_texture_paths = set()
    for master in masters.values():
        master_textures = list(library.get_used_textures(master))
        for name in library.get_texture_parameter_names(master):
            master_textures.append(library.get_material_default_texture_parameter_value(master, name))
        master_texture_paths.update(texture.get_path_name() for texture in master_textures if texture)

    textures = {}
    for material_path, name, texture in candidates:
        path = texture.get_path_name()
        if path in master_texture_paths:
            unreal.log("MH: Skipped master texture: " + path)
            continue
        if not path.startswith("/") or path.startswith(("/Engine/Transient", "/Temp/")):
            continue
        textures[path] = texture
        unreal.log("MH: {} [{}] -> {}".format(material_path, name, path))

    paths = sorted(textures)
    if paths:
        unreal.EditorAssetLibrary.sync_browser_to_objects(paths)
        unreal.log("MH: Selected {} textures from {} material instances.".format(
            len(paths), len(materials)
        ))
    else:
        unreal.log_warning(
            "MH: No MI override textures remain after excluding master textures. "
            "Content Browser selection was left unchanged; do not treat the old selection as a result."
        )
    return paths


if __name__ == "__main__":
    select_actor_tex_textures()
