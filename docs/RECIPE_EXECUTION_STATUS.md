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
| R2b | IN PROGRESS (близнец) | три PR: **R2b-1** `MHMaterializeLayout` — IN REVIEW, ветка `recipe/r2b-materialize-layout`, квитанция `docs/receipts/recipe_r2b_materialize_layout.md`, OPEN-R2B-1 (гейт удалений) — ждёт Lead; **R2b-2** актор на рецептах (без Layout на PostEditMove, без Proof на загрузке) — NEXT; **R2b-3** удаления proof-состояния — после ответа Lead. Полевые находки Undo/reseed — вход для R2b-2/R4 |
| R2c | PLANNED | — |
| R3 | PLANNED | — |
| R4 | PLANNED | — |
| R5 | PLANNED | — |
| R6 | PLANNED | — |
| R7 | PLANNED | — |
| R8 | OPTIONAL | — |

Обновление этой таблицы — обязательная строка acceptance каждого среза.

Правило исполнения (owner 2026-09-02): срезы, которые близнец не делает сам, выполняет внешний исполнитель по контракту из `docs/contracts/`; близнец пишет контракт (ветка + red-тест уже в ветке) и проверяет результат. Каждый срез — отдельная ветка и PR.
