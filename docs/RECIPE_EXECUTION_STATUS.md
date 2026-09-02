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
| R0c | IN REVIEW (внешний исполнитель, контракт `docs/contracts/recipe_r0c.md`) | убрать из кода валидацию applied-root в горячем пути (docs/16 §7.2, шестая строка removed-entities); снять tag-пробу duplicate-claim из admission реестра **вместе** с миграцией `DuplicateRootClaimBlocksPlanAndBreak` в preflight-тест (§7.5) |
| S0 | READY (внешний исполнитель, контракт `docs/contracts/source_s0.md`) | параллельно, линия S |
| S1 | PLANNED | — |
| S2 | PLANNED | — |
| R2a | PLANNED (близнец) | первый шаг — реализация фазового разделения, П1 |
| R2b | PLANNED | — |
| R2c | PLANNED | — |
| R3 | PLANNED | — |
| R4 | PLANNED | — |
| R5 | PLANNED | — |
| R6 | PLANNED | — |
| R7 | PLANNED | — |
| R8 | OPTIONAL | — |

Обновление этой таблицы — обязательная строка acceptance каждого среза.

Правило исполнения (owner 2026-09-02): срезы, которые близнец не делает сам, выполняет внешний исполнитель по контракту из `docs/contracts/`; близнец пишет контракт (ветка + red-тест уже в ветке) и проверяет результат. Каждый срез — отдельная ветка и PR.
