# MH_blender_bridge

DCC-driven composite asset pipeline: Blender → versioned source files → UE5.
Философия — DagorEngine composites; спеки и Decision Log — в `docs/` (читать в
порядке 00 → 01 → 02 → 03).

## Структура

```
docs/            # спеки, Decision Log, QUESTIONS.md
reference/       # dag4blend 2.12.0, read-only (лицензия Gaijin — только паттерны)
tools/           # golden scene, мутации, каноническая библиотека
golden/          # expected_diffs (спека тестов B/C), canonical_vectors.json
                 # *.blend генерируются, в git не хранятся
addon/mh4blend/  # этап B
ue/MimirComposite/  # этапы C–D
```

## Этап A: запуск

Требования: Python 3.11 + `pip install -r requirements-dev.txt`, Blender 4.5+
(бинарник — или pip-модуль `bpy`, тогда Blender не нужен вовсе).

```bash
# всё разом: golden.blend, 7 мутаций, md-рендер диффов, pytest
python3 tools/run_all.py                      # через pip bpy
MH_BLENDER=/path/to/blender python3 tools/run_all.py   # через blender -b -P

# по отдельности
python3 tools/make_golden_scene.py
python3 tools/mutations/rename_object.py
python3 -m pytest tests -q
```

`golden/expected_diffs/*.json` — первичная спецификация диффов (формат
`mh.diff_report`, docs/01 §7.3); `.md` рядом — сгенерированное представление
(`tools/render_expected_diffs.py`), руками не редактировать.

`golden/expected_errors/*.json` — спецификация негативных тестов (формат
`mh.validation_report`, docs/01 §6.2) для сцен-«вредителей» (`duplicate_uid`,
`parent_uid_dangling`) и bundle-фикстуры цикла
(`golden/fixtures/composite_cycle/`, генерируется `tools/make_cycle_fixture.py`).

## Blender-аддон

```bash
python tools/build_addon_zip.py
```

Установите `dist/mh4blend-<version>.zip` через Blender **Install from Disk**.
Для XXH3-хешей пакет `xxhash` должен быть установлен в Python этой
версии Blender. В preferences `mh4blend` задаются `Source Root`,
`Texture Root` и опциональный `registry.json`; в N-панели **MH** доступны
**Export Sources** и **Validate**.

Материальные metadata читаются из `Material.dagormat`, поэтому для
полного экспорта включите dag4blend. Обычный Blender-материал без
dagormat остаётся валидным: он экспортируется как пустая заглушка
`rendinst_simple`. На диске пишутся только `*.composite`, `meshes/*.mesh.fbx`
и служебный `export_manifest.json`; отдельного `materials.json` нет.

Для однократного переноса legacy-путей задайте **Old Texture Root (Remap)**
и новый **Texture Root**, затем нажмите **Remap Old Texture Root**. Оператор
с подтверждением меняет только абсолютные пути внутри старого корня; операция
поддерживает Undo и пишет счётчики в `mh_export_log`.

Совместимость с реальным RNA из vendored dag4blend проверяется отдельно:

```bash
blender -b --factory-startup --python-exit-code 1 \
  -P tools/check_dag4blend_compat.py
```
