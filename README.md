# MH_blender_bridge

DCC-driven composite asset pipeline: Blender → versioned Source Bundle → UE5.
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
