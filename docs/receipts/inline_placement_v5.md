# Квитанция — inline placement в грамматике `.composite` (ревизия OPEN-V5-15)

Дата: 2026-08-31
Ветка: `feat/inline-placement-v5`
Исполнитель среза: Lead (по слову owner «сделай сам»)
Статус: **READY FOR OWNER FIELD TEST (переэкспорт композитов)**

## 1. Ратификация

Owner 2026-08-31 пересмотрел собственное решение OPEN-V5-15: производные
content-addressed файлы `dagor_p2_*.placement` признаны ошибкой; выбран
вариант (b) — **inline-профиль в wire-грамматике `.composite`, как в
Dagor**. Нормативный текст — docs/10 §6.1 (дополнение) и
docs/QUESTIONS.md OPEN-V5-15 «Ревизия owner 2026-08-31».

Семантика:

- новое опциональное поле узла `placement` — полный canonical placement-v1
  документ inline; канонический порядок: kind, resource, name, transform,
  profile, **placement**, place_type, appearance_seed_boundary, options,
  children. Append-only: документы без поля — байт-в-байт прежние;
- `profile` и `placement` взаимоисключимы (`MH_E_COMPOSITE_GRAMMAR` в
  ридерах и врайтерах обеих сторон);
- тело inline подчиняется полной закрытой placement-v1 грамматике со
  своими кодами (`MH_E_PLACEMENT_PROFILE_GRAMMAR`,
  `MH_E_UNKNOWN_SCHEMA_VERSION`, `MH_E_NAN_INF_VALUE`); прежние
  adapter-инварианты (abs-нормализация deviation, neutral-оси, identity
  authored transform без double-base) сохранены;
- inline не является ресурсом: не входит в profile references,
  dependency-граф, selected resources и project index; UE-резолвер сэмплит
  его напрямую из узла теми же draw-ролями и тем же node stream, что и
  внешний профиль (паритет закреплён тестом);
- адаптер dag4blend больше не порождает производные `.placement`;
- MH-сценовый импорт композита с inline-узлом — fail-closed
  (`MH_E_UNREPRESENTABLE_SCENE_OBJECT`) до появления сценового carrier'а —
  follow-up;
- внутренний runtime тини-транспорт — append-only версия 3 (v1/v2
  payload'ы читаются без изменений); wire/stream/signature домены не
  менялись.

Новых E/W-кодов нет. `reference/` не тронут. `golden/v5/
source_protocol_v5_codec_vectors.json` расширен **строго append-only**
(19 insertions, 0 deletions по diff): позитивный вектор `inline_placement`
и негативные `profile_and_placement_conflict`,
`inline_placement_negative_deviation`, `inline_placement_wrong_version`.

## 2. Red → green

| Слой | RED | GREEN |
|---|---|---|
| Pure codec (`test_composites.py`, 4 новых теста) | 4 failed | 37/37 |
| UE `Mimir.V5.Composite.CanonicalGoldenVectors` на расширенном golden | Fail | Success |
| Адаптер (`test_dag4blend_direct_export_bpy.py`, переписанные inline-тесты) | 3 failed | 83/83 |
| MH-сценовый импорт fail-closed (`test_composite_blender_bpy.py`) | red-first | 23/23 |

Тест `test_generated_p2_hash_collision_never_overwrites_existing_profile`
удалён: его предмет (коллизия content-addressed имён производных файлов)
упразднён ревизией; замена —
`test_inline_p2_reexport_ignores_foreign_placement_files`.
UE-тесты `Mimir.V5.Random.InlinePlacementParity` (паритет draw/transform
inline против named профиля + mutual-exclusion отказ резолвера) и
обновлённый `NodeMetadata.ClosedGrammar` добавлены после общего RED
golden-вектора; отдельный red-прогон для них не выполнялся.

## 3. Изменённые файлы

Blender: `core/model.py`, `core/composites.py`,
`scene/import_dagor_composite.py`, `scene/import_composite.py`,
`blender_manifest.toml` (0.10.0), `tools/mh_v5_codec_fixture.py`,
тесты `test_composites.py`, `test_dag4blend_direct_export_bpy.py`,
`test_composite_blender_bpy.py`.

UE: `MHCompositeProtocol.h/.cpp` (парсер/врайтер/flatten/extract,
общий `ParsePlacementProfileObject`), `MHCompositeAsset.h`
(`FMHCompositeAssetNode.bHasInlinePlacement/InlinePlacement`),
`MHCompositeResolvedPlan.cpp`, `MHRandomStream.h/.cpp`,
`MHRuntimeCompositeInput.cpp` (транспорт v3),
тесты `MHRandomStreamV5Test.cpp`, `MHCompositeNodeMetadataTest.cpp`.

Доки: `10_source_protocol_v5_plan.md` §6.1 (+исправлена опечатка
`placement`→`place_type` в старом перечне порядка полей),
`QUESTIONS.md` OPEN-V5-15, эта квитанция.

## 4. Гейты

| Гейт | Результат |
|---|---|
| Pure `python -m pytest tests/ -q` | **313 passed / 14 skipped** |
| Blender-hosted, 12 модулей | **367/367, 0 failed** |
| Guarded UE editor build | **Succeeded** |
| Полный NullRHI `Mimir` + `-MHGoldenRoot` | **162/162, 0 failed** (120 clean + 42 warn) |
| Полевое staging-репро cottage_i (prepare+stage, без публикации) | **732/732 мешей, 0 `.placement`, 239 композитов, 64 с inline** |

Payload'ов плана стало 1522 вместо 1595 — исчезли 73 производных профиля.

## 5. Полевой протокол owner

1. Перезапустить Blender (аддон 0.10.0), UE-плагин переустановлен.
2. `Export Composite All Stuff` — переэкспорт перепишет все `.composite`
   с p2-узлами на inline-форму; новые `dagor_p2_*` файлы не появятся.
3. UE Import Changed: композиты с inline перечитаются; разброс узлов
   проверяется рероллом Layout Seed.
4. После успешного переэкспорта старые `dagor_p2_*.placement` в Source
   Root — сироты; удалить их (родные композиты на них больше не
   ссылаются). До переэкспорта не удалять.

## 6. Follow-ups

- Сценовый MH-carrier для inline placement (сейчас импорт fail-closed).
- 14 `MH_E_UNREPRESENTABLE_TRANSFORM` полевого импорта — отдельное
  расследование (не связано с этим срезом).
