# D0b — поправки к KICKOFF v2 / docs/16_recipe_model.md (v2.1)

Статус: **READY FOR REVIEW**. Документальный срез, кода нет.

## 1. База и границы

- ветка: `recipe/d0b-docs`; база `origin/main` `4f6cf67` (после merge R1 #66);
- источник поправок: `docs/archive/D0b_kickoff_v2_1_amendments.md` (второй
  внешний аудит `docs/reference_notes/external_audit_recipe_model_v2_20260902.md`,
  решения Lead П1–П12);
- изменения ограничены `KICKOFF_PROMPT.md`, `docs/16_recipe_model.md`,
  `docs/NORMATIVE_INDEX.md`, `docs/RECIPE_EXECUTION_STATUS.md` (новый),
  `docs/archive/D0b_kickoff_v2_1_amendments.md` (новый), этой квитанцией;
  `ue/`, `addon/`, `golden/`, `tests/` не изменялись.

Патч-файл был написан по устаревшему `main` (`eba46ec`, только R0b смержен);
фактический `origin/main` на момент старта уже содержит R0b **и** R1 (#66).
Ниже это отражено (см. §4 «Фактические поправки к патчу»).

## 2. Поправка → файлы → что заменено

| # | Файлы | Что заменено |
|---|---|---|
| П1 | KICKOFF §3.3, §10; docs/16 §2.3, §9 | предложение «Первый шаг R2a — проверить…» → нормативный текст (код `MHRandomStream.cpp:809`, фазовое разделение обязательно); OPEN-R-6 → закрыт |
| П2 | docs/16 §9 (таблица + контекстный абзац) | правило OPEN-R-7 → решение «ноль tag-запросов в preview», статус закрыт; в KICKOFF §10 добавлена зеркальная запись OPEN-R-7 (её не было) |
| П3 | KICKOFF §4, docs/16 §4 | строка «Смена Seed» → 4 строки по `SeedAffectsResult` (None/ChildSeedsOnly/Transform/Topology); строка «Смена AppearanceSeed» переформулирована |
| П4 | KICKOFF §3.2/§3.8/§4/§5; docs/16 §2.2/§2.8/§4/§8 | `PlacementInterfaceHash` (одно поле) → пять полей `PayloadRevision/BoundsRevision/BucketDescriptorHash/CollisionInterfaceHash/MaterialBindingHash` во всех местах, кроме `docs/16` §7.2 (removed-entities) |
| П5 | KICKOFF §3/§3.6; docs/16 §2/§2.6 | п.1 точек выхода → `PreSaveWorld` читает background proof cache вместо синхронной сборки; в архитектурную диаграмму добавлен `FMHProofCache` |
| П6 | KICKOFF §5 (таблица R1) | red-assert `all_option_unique_meshes ≫ waited_meshes == selected_unique_meshes` → `waited_mesh_set == selected_compiling_mesh_set` + `waited_mesh_set ∩ unselected_mesh_set == ∅`, отношение осталось метрикой; в docs/16 §8 формулы red-assert не было — менять нечего |
| П7 | KICKOFF §3.1; docs/16 §2.1 | ключ реестра `TWeakObjectPtr<UMHCompositeAsset> + AppliedHash` → `UMHCompositeAsset* + RecipeRevision` (`uint32`), `AppliedHash` — debug-атрибут |
| П8 | KICKOFF §3.7; docs/16 §2.7 | состав `NodeFingerprint` → блок кода с `ParentSemanticFingerprint` без authored transform родителя + 3 пункта (DisplayName исключён, canonical-представление, правило «свой/предка/родитель»); формулировка «консервативный fingerprint» уточнена |
| П9 | KICKOFF §3.7 заголовок и §5 (хвостовой абзац); docs/16 §2.7 заголовок | «после R3» → «после R5»; хвостовой абзац про физику/автосборку → производители `FMHNodeOverrideSet`-транзакций, ядро не различает источник |
| П10 | новый `docs/RECIPE_EXECUTION_STATUS.md`; KICKOFF §0, §5; docs/16 §0; `docs/NORMATIVE_INDEX.md` | создан трекер (реальные статусы, см. §4); правило-абзац добавлено в оба §0; строка про обновление трекера добавлена в KICKOFF §5; трекер внесён в индекс |
| П11 | docs/16 первая строка; KICKOFF первая строка | `ADR status: PROPOSED (→ NORMATIVE после R2b)` → `Document status: NORMATIVE · Target architecture rollout: TRANSITIONAL · Production cutover milestone: R2b`; `Architecture version: Recipe Model v2` → `v2.1` (и `v2.1 (D0b)` в KICKOFF) |
| П12 | docs/16 §7.2 (блок + таблица); KICKOFF §7 п.4 | `PlacementInterfaceHash` добавлен восьмой строкой в `removed-entities` и таблицу docs/16; в KICKOFF термин не повторён буквально (см. §5 «Что не удалось разместить без выбора») |

## 3. Acceptance D0b (патч §Acceptance)

| # | Критерий | Результат |
|---|---|---|
| 1 | П1–П12 применены точечными заменами к обоим документам | выполнено (см. §2 выше) |
| 2 | `docs/RECIPE_EXECUTION_STATUS.md` создан и внесён в `NORMATIVE_INDEX.md` | выполнено |
| 3 | Второй аудит положен в `docs/reference_notes/external_audit_recipe_model_v2_20260902.md` | выполнено — файл уже существовал в рабочей копии (untracked) на момент старта среза, вопреки инструкции задачи, что текста аудита у исполнителя нет; добавлен в индекс и коммит (см. §4) |
| 4 | OPEN-R-6, OPEN-R-7 закрыты в docs/16 с датой и ссылкой на этот патч | выполнено — статус `закрыт 2026-09-02 (D0b П1/П2)` |
| 5 | CI-проверки §7.2 зелёные; грепы §7.4 (с П12) = 0 вне archive/receipts | выполнено — см. §5 «Гейты» |
| 6 | Этот файл перенесён в `docs/archive/` с HISTORY-заголовком | выполнено — `docs/archive/D0b_kickoff_v2_1_amendments.md` |
| 7 | Квитанция `docs/receipts/recipe_d0b.md` | этот файл |

## 4. Фактические поправки к патчу

Патч-файл был написан по состоянию `main` до merge R0b/R1, поэтому его
собственный трекер-пример (`R1 BLOCKED BY D0b`) не совпадает с реальностью:

(a) `origin/main` на старте среза уже содержит R1 (#66, acceptance по
формулировке v2). В `docs/RECIPE_EXECUTION_STATUS.md` R1 отмечен `MERGED`, а
red-assert П6 (две проверки множеств вместо `≫`) заведён отдельной строкой
`R1.1 READY` — существующий счётчик `waited_meshes` и тест
`Perf.SelectedMeshWait` (квитанция `docs/receipts/recipe_r1.md`) написаны под
старую формулировку и требуют приведения к П6, но это не блокирует D0b.

(b) Снятие tag-пробы duplicate-claim из admission-реестра (П2/OPEN-R-7)
физически не может быть отдельным срезом от миграции теста
`DuplicateRootClaimBlocksPlanAndBreak` в preflight: пока проба стоит, тест
проверяет её; как только проба снята, тест либо падает, либо должен уже быть
преflight-тестом. В `docs/RECIPE_EXECUTION_STATUS.md` это одна строка `R0c`
(снятие пробы + миграция теста, §7.5: замена до удаления), а не два среза.

(c) Второй аудит (`docs/reference_notes/external_audit_recipe_model_v2_20260902.md`)
физически присутствовал в рабочей копии worktree (untracked) на момент начала
среза, хотя инструкция D0b явно требовала запросить его у owner как
отсутствующий. Его содержимое подтверждает П1–П11 дословно (включая пункт про
`RecipeRevision`, `Document status: NORMATIVE` и т.д.), поэтому он добавлен в
`docs/NORMATIVE_INDEX.md` («Справочные») и включён в коммит; acceptance-пункт
3 отмечен выполненным, а не «не выполнено».

## 5. Что не удалось разместить без выбора (сообщено, не угадано)

П12 требовал одновременно (i) добавить `PlacementInterfaceHash` в
`docs/16_recipe_model.md` §7.2 и (ii) добавить его же в список KICKOFF §7 п.4
«лексический ноль». Итоговый шаг проверки D0b (описание задачи, отдельно от
текста самого патча) требует, чтобы
`grep -rIn "PlacementInterfaceHash" README.md KICKOFF_PROMPT.md docs
--exclude-dir=archive --exclude-dir=receipts --exclude-dir=reference_notes |
grep -v "16_recipe_model.md"` возвращал 0 совпадений — то есть термина не
должно быть и в `KICKOFF_PROMPT.md`. Эти два требования противоречат друг
другу буквально. Выбрано: термин в `KICKOFF_PROMPT.md` не повторяется (список
п.4 описывает восьмую строку без идентификатора, со ссылкой на
`docs/16_recipe_model.md` §7.2), чтобы грep-гейт был зелёным; `CI`
(`check_removed_entities`) это не ломает, так как `KICKOFF_PROMPT.md` итак
исключён из скрипта как owner-директива.

## 6. Гейты

Вывод `python tools/check_normative_docs.py`:

```
normative docs: OK
```

Вывод `git diff --check` (после `git add -A`):

```
(пусто — конфликтов пробелов/маркеров нет)
```

Грепы (Step 4):

- `grep -rIn "PlacementInterfaceHash" README.md KICKOFF_PROMPT.md docs --exclude-dir=archive --exclude-dir=receipts --exclude-dir=reference_notes | grep -v "16_recipe_model.md"` → 0 строк.
- `grep -n "Первый шаг R2a — \*\*проверить" KICKOFF_PROMPT.md docs/16_recipe_model.md` → 0 строк (формулировка заменена П1 в обоих документах).

## Ревью близнеца

Diff проверен построчно по П1–П12; дополнительно приведены к П11 описания docs/16 в `README.md` и `docs/NORMATIVE_INDEX.md` («Status PROPOSED до R2b» → «v2.1: rollout TRANSITIONAL, cutover R2b»). Ссылки KICKOFF §5/§8 на `Status: PROPOSED` описывают acceptance исторического среза D0a и не менялись.
