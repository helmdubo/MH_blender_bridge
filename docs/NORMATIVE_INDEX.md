> Status: NORMATIVE · Architecture version: Recipe Model v2 · Supersedes: — (created in D0a)

# NORMATIVE_INDEX — активные документы

## Нормативные

| Документ | Роль |
| --- | --- |
| `KICKOFF_PROMPT.md` | активный промпт исполнителя: роль, программа срезов D0a/M0/R0–R8, линия S, гейты, OPEN-R |
| `docs/16_recipe_model.md` | ADR модели «рецепт + исполнитель»; Status PROPOSED до R2b |
| `README.md` | карта репозитория, три плоскости, документальная политика, полевые команды |
| `docs/10_source_protocol_v5_plan.md` | протокольный справочник v5: identity, индекс, FBX, материалы, .composite/.placement, receipt, сиды, runtime-мост |
| `docs/NORMATIVE_INDEX.md` | этот индекс |

## Справочные (не норматив)

| Документ | Роль |
| --- | --- |
| `docs/reference_notes/dagor_composit_research.md` | исследование модели Dagor composite (evidence/research) |
| `docs/reference_notes/external_audit_recipe_model_20260902.md` | внешний аудит Recipe Model v1 (evidence/research) |
| `docs/reference_notes/dag4blend_random_overlay.md` | исследование dag4blend random overlay (evidence/research) |
| `docs/reference_notes/dagor_corpus_inventory_20260828.md` | инвентаризация корпуса Dagor (evidence/research) |
| `docs/reference_notes/dagor_engine_references.md` | справочные ссылки на движок Dagor (evidence/research) |
| `docs/reference_notes/dagor_phmat_registry.md` | заметки по реестру phmat Dagor (evidence/research) |
| `docs/reference_notes/evidence/` | сырые артефакты и логи проб (evidence/research), не норматив |

## История

`docs/archive/` — это HISTORY: документы, которые больше не описывают текущую модель, и их не следует использовать для реализации. `docs/receipts/` — это история исполнения срезов; квитанция фиксирует, что было сделано и проверено в моменте, но квитанция не равна owner acceptance и не содержит нормативных требований — при расхождении между квитанцией и нормативным документом действует нормативный документ.

## Проверка

Соответствие этого индекса репозиторию проверяется скриптом `python tools/check_normative_docs.py`. Код выхода 0 (`normative docs: OK`) — индекс актуален; любой другой — есть нарушения, перечисленные построчно как `VIOLATION: ...`.
