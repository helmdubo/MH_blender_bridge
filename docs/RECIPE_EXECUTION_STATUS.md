> Status: NORMATIVE · Architecture version: Recipe Model v2.1 · Supersedes: — (created in D0b)

# RECIPE_EXECUTION_STATUS — фактическая точка продолжения

Архитектурный порядок задаёт `KICKOFF_PROMPT.md` (§5) и `docs/16_recipe_model.md`
(§8). Этот документ задаёт **фактическую** точку продолжения программы: какие
срезы реально смержены в `origin/main`, какой срез следующий, и какие срезы
разблокированы для параллельной работы. Перед началом любого среза исполнитель
читает эту таблицу и начинает **только** срез со статусом NEXT/READY.

| Срез | Статус | PR / примечание |
|---|---|---|
| D0a | MERGED | #62 |
| M0 | MERGED | #63 |
| R0a | MERGED | #64 |
| R0b | MERGED | #65 |
| R1 | MERGED | #66 (acceptance по формулировке v2; П6 закрывает R1.1) |
| D0b | MERGED | #67 |
| R1.1 | MERGED | #69 (внешний исполнитель, приёмка близнеца); квитанция `docs/receipts/recipe_r1_1.md` |
| R0c | MERGED | #71 (внешний исполнитель, приёмка близнеца); реализация `7811eab` попала в `main` fast-forward-push'ем до PR, проверена близнецом постфактум (полный suite 176/176 на host близнеца); квитанция `docs/receipts/recipe_r0c.md` |
| S0 | MERGED | #73 (внешний исполнитель, приёмка близнеца; возврат 1 — racy fingerprint); квитанция `docs/receipts/source_s0.md` |
| S1 | CLOSED (без кода) | OPEN-S-1 закрыт owner'ом, вариант c: S0 достаточен; полевой замер холодного скана — по протоколу M0 §6 |
| S2 | MERGED | #77 (внешний исполнитель, приёмка близнеца; возвраты OPEN-S2-1/S2-2 закрыты нормативно); квитанция `docs/receipts/source_s2.md`. Линия S закрыта: S0 #73, S1 CLOSED (вариант c), S2 #77 |
| R2a | MERGED (близнец) | два PR: **R2a-1** фазовое разделение resolver'а — #79 (`docs/receipts/recipe_r2a_phases.md`); **R2a-2** `FMHCompiledRecipe` + реестр + `RecipeShadowParity` как CI-гейт — #81 (`docs/receipts/recipe_r2a_compiled_recipe.md`). OPEN-R2A-1 закрыт owner 2026-09-02 (docs/16 §9). **Полевой тест owner 2026-09-02 на портфолио (main `4782082`) пройден** — `docs/receipts/field_recipe_r2a_20260902.md`. Preview-путь ещё не production → R2b |
| R2b | MERGED (близнец) | три PR: **R2b-1** `MHMaterializeLayout` — #84; **R2b-2** актор на рецептах — #86; **R2b-3** удаления proof-состояния актора и definition-кэша — #90 (owner: удаления одобрены 2026-09-03; квитанция `docs/receipts/recipe_r2b_proof_state_deletions.md` §6). ADR docs/16: `Status: NORMATIVE` (D0a: «PROPOSED → NORMATIVE после R2b») |
| R2c | MERGED | #88 (внешний исполнитель, приёмка близнеца; возвраты OPEN-R2C-1…4 закрыты нормативно в `docs/contracts/recipe_r2c.md`); квитанция `docs/receipts/recipe_r2c.md`; `Proof.BuildPreflightFullClosure` зелёный → гейт R2b-3 открыт |
| R3 | NEXT (внешний исполнитель, контракт близнеца) | пять хэшей/ревизий интерфейса меша (docs/16 §2.2 П4, восьмая строка §7.2 — код); контракт `docs/contracts/recipe_r3.md` |
| RS-1 | MERGED (внешний агент, ресёрч; owner залил в `main` коммитом `28e898d`) | `docs/reference_notes/dagor_composite_build_break_20260903.md` — daEditor: «Split composites» снимает один слой, undo хранит записи `(asset, tm, seeds)`, «Export as composit» = файл на диск без изменения сцены; сводка для R4-pre в §6 |
| R4-pre | MERGED (внешний исполнитель, приёмка близнеца) | #93 (`e8256b6`): Break = один слой рецепта в preview-плоскости (без proof/tag-запросов), дети-композиты остаются `AMHCompositeActor` с сидами родителя, plan-view компоненты не транзакционны, `PostEditUndo` восстанавливает из записи; близнец: 194/194 на generic-хосте; owner на портфолио: дубли после Undo не воспроизводятся (2026-09-03); квитанция `docs/receipts/recipe_r4_pre.md`; OPEN-R4P-1 открыт (fail-closed: сиды родителя) |
| R4-pre-2 | READY FOR REVIEW (внешний исполнитель) | `codex/recipe-r4-pre2-build-break-preserve` от контрактной ветки `401d73f`, red `511d0ba`, реализация `9221626`: mesh Break переносит appearance, Build предупреждает о непредставимом состоянии через чистый preflight; 13/13 focused, full NullRHI 198 reported Success / 0 Fail (три условных NOT RUN отмечены в квитанции), force-unity и StrictIncludes PASS; квитанция `docs/receipts/recipe_r4_pre2.md`; merge — близнец, полевой smoke — owner после merge |
| R4 | PLANNED | — |
| R5 | PLANNED | — |
| R6 | PLANNED | — |
| R7 | PLANNED | — |
| R8 | OPTIONAL | — |

Обновление этой таблицы — обязательная строка acceptance каждого среза.

Правило исполнения (owner 2026-09-02): срезы, которые близнец не делает сам, выполняет внешний исполнитель по контракту из `docs/contracts/`; близнец пишет контракт (ветка + red-тест уже в ветке) и проверяет результат. Каждый срез — отдельная ветка и PR.
