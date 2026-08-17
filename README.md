# MH_blender_bridge

DCC-driven composite asset pipeline: Blender → versioned source files → UE5.
Философия — DagorEngine composites; спеки и Decision Log — в `docs/`.
Кандидат на финальную standalone source schema v1:
`docs/05_source_schema_v1.md`,
авторские workflows: `docs/04_source_workflows.md`, план реализации после
freeze: `docs/06_final_v1_plan.md`. Тот же commit становится окончательно
замороженным после приёмки внешним ревьювером. Старые bundle-разделы документов
01–02 сохранены только как история и помечены superseded.

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

Установите `dist/mh4blend-<version>.zip` через Blender **Install from Disk**.
Для XXH3-хешей пакет `xxhash` должен быть установлен в Python этой версии
Blender. Целевой UX v1 состоит из трёх отдельных вкладок N-панели; общей
кнопки Bundle/Export Sources нет:

- **FBX Export**: Collection + Directory + Export;
- **Composites / Import**: `.composite` File Path + Import;
- **Composites / Export**: Collection + Directory + Export.
- **MH Material**: Material + Folder + Export; при повторном экспорте
  существующие payload и owning manifest по UID обновляются in place.

Материальные metadata читаются из `Material.dagormat`, поэтому для полного
экспорта включите dag4blend. Обычный Blender-материал без dagormat остаётся
валидным: он экспортируется как пустая заглушка `rendinst_simple`. Пути
`tex0…tex15` берутся непосредственно из материала; Blender-пути `//`
разворачиваются относительно `.blend`. Файл внутри project `source_root`
пишется относительным forward-slash путём, снаружи — нормализованным абсолютным
путём и диагностикой согласно `texture_policy`. Отдельного Texture Root нет;
текстуры не копируются и не изменяются.

Каждая операция обновляет только выбранный ресурс и его owning
`export_manifest.json`. Манифест — квитанция собственных ресурсов, а не bundle,
не граф зависимостей и не список владения каталогом; несвязанные записи и файлы
сохраняются. Resolver ищет payload по UID во всём `source_root` и всегда
подтверждает единственного владельца-манифест. Полный контракт и обратный
импорт: `docs/04_source_workflows.md`.

Текущая реализация в рабочей ветке предшествует финальному schema freeze и
должна быть приведена к документам 04–06 отдельным кодовым срезом.

Совместимость с реальным RNA из vendored dag4blend проверяется отдельно:

```bash
blender -b --factory-startup --python-exit-code 1 \
  -P tools/check_dag4blend_compat.py
```
