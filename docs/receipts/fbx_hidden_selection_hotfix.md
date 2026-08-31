# Квитанция — hotfix пустых mesh FBX (скрытая/изолированная геометрия)

Дата: 2026-08-31
Ветка: `fix/dagor-uppercase-projection`
Исполнитель среза: Lead (по слову owner «сделай сам»)
Статус: **READY FOR OWNER FIELD TEST (переэкспорт mesh payload)**

## 1. Полевой дефект

Полный `Export Composite All Stuff` сцены
`E:\portfolio\sovmod_cottage_i_cmp_source_lod00.blend` опубликовал в
Source Root 732 mesh-файла, из которых 731 — структурно валидные FBX
по 4140 байт **без единого Model-узла** (без геометрии). UE-импорт
заблокировал их с диагностикой `mesh FBX contains no Model nodes` и
каскадно заблокировал зависящие композиты. Единственный полный mesh —
`sovmod_chair_c` (73 452 байта).

## 2. Диагноз (подтверждён репро)

Механизм — **молчаливый no-op `select_set(True)` на объекте, скрытом в
активном view layer**:

1. Экспортёр выделяет транспортируемые объекты и пишет FBX c
   `use_selection=True`.
2. Blender превращает `select_set(True)` для скрытого (H / isolate
   вокруг другого меша) объекта в silent no-op. Существующие проверки
   («объект есть в view layer», `hide_select`) этого не ловят.
3. Read-back проверял только парсинг и наличие секций
   `Objects`/`Connections` — пустой FBX проходил и публиковался.

Репро на реальной сцене owner (staging в temp, без публикации):

- свежеоткрытый файл — **732/732** staged FBX с геометрией;
- скрыты все меши, кроме `sovmod_chair_c_lod00` — **ok=1, empty=731**,
  побайтово полевая картина (пустышки по 4140 байт).

Проверка main (`52bfe93`): тот же `select_set` без верификации и тот же
read-back без Model-подсчёта. Это **латентный дефект всех предыдущих
версий**, а не регрессия batch-scene оптимизации `3eb46a4`;
полевым триггером стало изолированное состояние геометрии в сессии.
Batch-scene hoisting не откатывался.

Отдельный подтверждённый факт: Local View (`/`) сам по себе геометрию
из экспорта не выбрасывает (select работает); опасен именно hide-канал.
Auto-exit из Local View всё равно реализован по прямому решению owner:
публикация полного пакета не должна выполняться в изолированном виде.

## 3. Реализация (red-first)

`addon/mh4blend/scene/export_fbx.py`:

- `_temporary_selection_context`: после выделения — верификация
  `select_get()` каждого объекта; расхождение — fail-closed
  `MH_E_INVALID_RESOURCE_SOURCE` (существующий код реестра, новых E/W
  нет) с перечнем скрытых объектов до любой записи.
- `stage_prepared_fbx`: read-back усилен — число `Model`-узлов в
  staged FBX обязано равняться числу транспортируемых объектов; иначе
  RuntimeError, staged файл удаляется, публикация невозможна.
- `_exit_local_view_isolation()` + вызов в `stage_prepared_fbx`:
  перед staging каждый 3D viewport в Local View выводится из изоляции
  (fail-open no-op в безоконных сессиях; решение owner 2026-08-31).
- `blender_manifest.toml`: 0.8.0 → 0.9.0 (инвариант установки).

Семантика «скрытые объекты не выгружаются» НЕ вводилась (owner:
не критично сейчас); скрытая геометрия теперь даёт явный отказ, а не
пустой опубликованный файл.

## 4. Red → green

`tests/test_export_fbx_bpy.py`, три новых теста:

| Тест | RED (до фикса) | GREEN |
|---|---|---|
| `test_hidden_export_object_fails_closed_before_any_write` | DID NOT RAISE | pass |
| `test_staged_fbx_must_transport_every_export_object` | DID NOT RAISE | pass |
| `test_stage_leaves_local_view_isolation` | 1 failed (local view остаётся) | pass |

RED воспроизводился на точном коде ветки; для local view — временным
отключением вызова хелпера (ретроспективная red-проверка в том же
прогоне, лог `mh_lead\junit`).

## 5. Гейты

| Гейт | Результат |
|---|---|
| Pure `python -m pytest tests/ -q` | **309 passed / 14 skipped** |
| Blender-hosted, 12 модулей, изолированные factory-startup процессы | **366/366, 0 failed** (363 ветки + 3 новых) |
| Полевое staging-репро сцены owner (после фикса, без изоляции) | **732/732 Model-nodes OK** |

`golden/`, `reference/`, реестры кодов, форматы, UE-файлы этим срезом
не менялись.

## 6. Сопутствующий диагноз UE-импорта (не этот срез)

91 `MH_E_MATERIAL_GRAMMAR` (`lighting`, `real_two_sided`,
`mask_gamma`, `smoothness_metalness`) — version skew: аддон ветки
пишет string/bool opaque provenance, установленный UE-плагин = main их
отвергает. Поддержка уже в этой ветке
(`MHMaterialProtocol.cpp`, тест `Mimir.V4.Material.StringProvenance`);
лечится merge + переустановкой плагина. Два реально битых авторских
значения CDK останутся opaque string и не материализуются:
`mask_gamma="0.2,1.2.1,4.1,0.2"` и
`smoothness_metalness="-0.4, 0.0, 0.0, 0.0, 0.0"`.

## 7. Полевой протокол owner

1. Перезапустить Blender (аддон 0.9.0 установлен из этой ветки).
2. Открыть сцену, снять любую изоляцию/скрытие (или не снимать —
   Local View снимется сам, скрытое даст явный отказ с перечнем).
3. `Export Composite All Stuff` с прежними параметрами — переэкспорт
   перезапишет 731 пустышку настоящими FBX.
4. UE: Import Changed после установки веточного плагина (merge).
