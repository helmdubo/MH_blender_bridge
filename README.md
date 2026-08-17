# MH_blender_bridge

DCC-driven composite asset pipeline: Blender → versioned source files → UE5.
Философия — DagorEngine composites; спеки и Decision Log — в `docs/`.
Финальная standalone Source Schema v1:
`docs/05_source_schema_v1.md`,
авторские workflows: `docs/04_source_workflows.md`, план реализации:
`docs/06_final_v1_plan.md`. Post-freeze `Export Materials`/single-tab amendment
меняет только UX и orchestration standalone writers; JSON schema и canonical
bytes v1 не изменены. Старые bundle-разделы документов 01–02 сохранены только
как история и помечены superseded.

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

Текущие `golden/expected_diffs/*.json` и их `mh.diff_report` описаны в
историческом docs/01 §7.3. До регенерации в gates G1/G4 это pre-freeze
артефакты, а не нормативная Source Schema v1; `.md` рядом — сгенерированное
представление (`tools/render_expected_diffs.py`), руками не редактировать.

То же относится к `golden/expected_errors/*.json` (`mh.validation_report`,
исторический docs/01 §6.2) для сцен-«вредителей» (`duplicate_uid`,
`parent_uid_dangling`) и bundle-фикстуры цикла
(`golden/fixtures/composite_cycle/`, генерируется `tools/make_cycle_fixture.py`).

## Blender-аддон

```bash
python tools/build_addon_zip.py
```

Установите `dist/mh4blend-<version>-windows-x64.zip` через Blender
**Get Extensions → Install from Disk**. Это self-contained Extension: закреплённый
wheel `xxhash` уже входит в ZIP, вручную запускать `pip install` не нужно.
Перед переходом с прежнего script-addon отключите и удалите его в Preferences,
перезапустите Blender и только затем установите Extension ZIP: одновременно
включённые legacy-addon и Extension регистрируют одинаковые `mh.*` операторы.
Целевой UX v1 — одна N-panel вкладка **MH** без общей кнопки Bundle/Export
Sources:

- **FBX Export**: Collection + Directory + `Export Materials` (default ON) +
  Export. ON обновляет каждый уникальный используемый материал: существующий
  UID — in place у найденного owner, новый UID — рядом с выбранным FBX;
  OFF не трогает material payload/rows;
- **Composites / Import**: `.composite` File Path + Import;
- **Composites / Export**: Collection + Directory + Export;
- **Materials**: Material + Folder + Export; существующие payload и owning
  manifest по UID всегда обновляются in place.

Dagor LOD authoring: выберите Collection с точным именем `<base>.lods`.
Её непосредственный child `<base>.lod00` экспортируется как primary
`.mesh.fbx`, а `<base>.lod01+` — отдельными `.lod<level>.mesh.fbx` в `lods[]`
той же `static_mesh` manifest-row. Каждый уровень имеет собственный hash-skip и
участвует в recovery; packed FBX/Empty nodes не создаются. Только структурный
суффикс `.lods` снимается с логического имени ресурса — произвольные точки в
resource name остаются невалидными.

Материальные metadata читаются из `Material.dagormat`, поэтому для полного
экспорта включите dag4blend. Обычный Blender-материал без dagormat остаётся
валидным: он экспортируется как пустая заглушка `rendinst_simple`. Пути
`tex0…tex15` берутся непосредственно из материала; Blender-пути `//`
разворачиваются относительно `.blend`. Файл внутри project `source_root`
пишется относительным forward-slash путём, снаружи — нормализованным абсолютным
путём и диагностикой согласно `texture_policy`. Отдельного Texture Root нет;
текстуры не копируются и не изменяются.

Каждый standalone writer обновляет только свой ресурс и owning
`export_manifest.json`. FBX с `Export Materials=ON` оркестрирует несколько таких
material upserts вместе с mesh upsert, но не создаёт bundle-транзакцию и не
экспортирует иной dependency closure. Манифест — квитанция собственных ресурсов,
а не bundle, не граф зависимостей и не список владения каталогом; несвязанные
записи и файлы сохраняются. Resolver ищет payload по UID во всём `source_root` и
всегда подтверждает единственного владельца-манифест. Текстуры не копируются.
Полный контракт и обратный импорт: `docs/04_source_workflows.md`.

Текущая реализация в рабочей ветке приводится к замороженным документам 04–06;
полевой приёмкой считается только ZIP после прохождения соответствующих gates.

Совместимость с реальным RNA из vendored dag4blend проверяется отдельно:

```bash
blender -b --factory-startup --python-exit-code 1 \
  -P tools/check_dag4blend_compat.py
```
