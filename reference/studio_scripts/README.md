# studio_scripts — рабочие скрипты владельца (read-only референс)

Источники правды для портирования в аддон/плагин (D21–D28); в них НЕ вносятся правки.

| Файл | Оригинальное имя | Что портируется |
|---|---|---|
| `blender_export_matdata.py` | `add_matdata_to_meshdata_v0.75_with_collapse_v5_decal_split_fixed_v5_transform_shell.py` | контекст-менеджер `_temporary_ue_centimeter_export_state` (м→см ×100 с откатом — канон вместо `global_scale=100`), канон-настройки `export_scene.fbx` (`axis_forward='X'`, `axis_up='Z'`, `apply_scale_options='FBX_SCALE_UNITS'`, `use_space_transform=True`, `bake_space_transform=False`), схема материальных метаданных (`shader_class`, params, textures, `sides`), decal-split |
| `ue5_postprocess_materials.py` | `ue5_process_blueprints_and_actors_materials_v12_no_scale_reset.py` | суффикс-правила текстур (`_tex_d\|_d` → sRGB+TC_DEFAULT, `_tex_n\|_n` → linear+TC_BC7, `_tex_m\|_m` → sRGB+TC_DEFAULT), Finalize-правила (decal, Lumen Mesh Cards, UCX), семантика «MI существует, но не MIC / неправильный parent» |

SKELETAL-ветки (`only_deform` и пр.) при портировании сохраняются за флагом,
но в MVP не активируются (D29).
