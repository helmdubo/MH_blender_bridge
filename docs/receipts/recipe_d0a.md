# D0a — документальный срез программы Recipe Model v2

Статус: **READY FOR REVIEW**. Кода плагина нет; добавлен только
документальный CI-скрипт `tools/check_normative_docs.py`.

## 1. База и границы

- ветка: `recipe/d0a-docs` (заменяет `recipe/d0-docs` / PR #61, сделанный по
  KICKOFF v1 до аудита; PR #61 закрывается без merge);
- база: `origin/main` `70342e2` (KICKOFF v2 + внешний аудит);
- изменения ограничены `README.md`, `docs/`, `tools/check_normative_docs.py`,
  ссылками в `reference/dag4blend_patches/README.md`; `ue/`, `addon/`,
  `golden/`, `tests/` не изменялись;
- норматив после среза — `docs/NORMATIVE_INDEX.md`: `KICKOFF_PROMPT.md`,
  `docs/16_recipe_model.md` (ADR, Status PROPOSED), `README.md`,
  `docs/10_source_protocol_v5_plan.md`, сам индекс.

Разделение труда (матрица Lead 2026-09-02): таблица «что мёртво / что
переписать» (§3), ADR 16, README, вырезание в 10, квитанция — близнец;
механика (архив KICKOFF v1, переименование аудита, HISTORY-шапки,
`NORMATIVE_INDEX.md`, CI-скрипт) — Sonnet 5 по закрытому списку файлов.

## 2. Acceptance KICKOFF v2 §8

| # | Критерий | Результат |
|---|---|---|
| 1 | `docs/16_recipe_model.md`: §0–§4 KICKOFF как ADR, `Status: PROPOSED`, список удалённых сущностей кода, список запрещённых утверждений, OPEN-вопросы §10 | выполнено: 16 §0–§4, шапка `ADR status: PROPOSED (→ NORMATIVE после R2b)`, §7.2 (блок `removed-entities`, читается CI), §7.1, §9 (OPEN-R-1…6) |
| 2 | `docs/NORMATIVE_INDEX.md` создан; README описывает три плоскости и политику §7 | выполнено |
| 3 | 10/11/12/14/15: wire-формат, сиды, runtime-мост остаются в переписанном документе со status-шапкой либо перенесены в 16; остальное — архив с HISTORY-шапкой | 10 переписан со status-шапкой (§3); 11, 12a, 12b, 14, 15 — `docs/archive/`; appearance seed из 12b — 10 §6.9 |
| 4 | `KICKOFF_PROMPT.md` заменён v2; v1 — в `docs/archive/` | v2 положен owner в `70342e2`; v1 — `docs/archive/KICKOFF_PROMPT_v1_20260902.md` |
| 5 | Внешний аудит — `docs/reference_notes/external_audit_recipe_model_20260902.md` | переименован из `auditor_feedback Dagor and KICKOFF.ma`, шапка `Status: REFERENCE`, тело без изменений |
| 6 | CI-скрипт §7.2 добавлен и зелёный | `tools/check_normative_docs.py` → `normative docs: OK`, exit 0 (§4) |
| 7 | Квитанция с таблицей «документ → действие → почему» | этот файл, §3 |

## 3. Документ → действие → почему

Решения близнеца (что мёртво / что переписать):

| Документ | Действие | Почему |
|---|---|---|
| `KICKOFF_PROMPT.md` | без изменений (v2 owner) | активный промпт |
| `docs/16_recipe_model.md` | переписан как ADR v2 | v1-редакция D0 несла лексический бан на подписи и OPEN-вопрос о хэше плана; v2 оставляет `ClosureHash`/`ResolvedSignature` proof-артефактами, вводит три плоскости, два уровня admission, `PlacementInterfaceHash`, стабильные хэндлы, домен пула `ULevel`, fingerprint для overrides, preview-акторы editor-only, сырые веса; §7 — статус-политика вместо лексики |
| `docs/NORMATIVE_INDEX.md` | создан | KICKOFF §7.1–7.2 |
| `README.md` | переписан | три плоскости, политика §7, индекс, CI-команда; убраны superseded-баннеры и ссылки на несуществующие файлы (`tools/ue_s6_host/README.md`, `tools/check_dag4blend_compat.py`, меню «Mimir FBX Dump») |
| `docs/10_source_protocol_v5_plan.md` | переписан (вырезание) + status-шапка | **живой wire-формат**: §2–§6.5 (identity, индекс, FBX, материалы, `.composite`/`.placement`, Blender authoring, closure export), §6.6/§13.1/§13.8 (layout seed), новый §6.9 (appearance seed, перенос из 12b §3–4), §7 receipt (два уровня чтения по 16 §2.4, шесть тегов как `MH.<Name>`), §8 пути, §13.3 `ClosureHash`/`ResolvedSignature` как proof-артефакты. **Мёртво и вырезано**: freeze-статус и ссылки на 11/QUESTIONS как норматив; «applied state» как модель актора (→ receipt); §6.7 «actor хранит derived `ResolvedSignature`, dependency notify пересобирает plan» (→ состояние 16 §2.10, preview/proof); §12 таблица судьбы v4-документов (→ указатель на архив) |
| `docs/archive/11_v5_agent_slices.md` | HISTORY | порядок работ v5 завершён; активная программа — KICKOFF §5 |
| `docs/archive/12_v5_s6_1_s6_2_slices.md`, `12_v5_s6_2_s6_3_slices.md` | HISTORY | подписи как состояние актора, admission на инстансе, интерфейс мирового размещения; выживший контракт appearance seed — 10 §6.9 |
| `docs/archive/13_v5_s6_1_dag4blend_bridge.md` | HISTORY | S6.1 bridge; нормативный остаток — 10 §13.13 |
| `docs/archive/14_v5_ue_editor_program.md` | HISTORY | shared definition cache, перф-программа U0–U7 |
| `docs/archive/15_v5_s6_1_1_hardening.md` | HISTORY | S6.1.1/S6.1.2 |
| `docs/archive/00…09`, `ADR_*`, `AMENDMENT_*`, `C0/C1_*`, `RISK_RESULTS`, `ROADMAP`, `QUESTIONS.md`, `proposals/`, `spikes/` | HISTORY | история v1–v4, решённые OPEN-V4/V5 (остаток — 10 §13), основа старого definition cache |
| `docs/archive/README_pre_d0.md`, `docs/archive/KICKOFF_PROMPT_v1_20260902.md` | HISTORY | прежние README (v5/v4/v2) и KICKOFF v1 |
| `docs/reference_notes/dagor_composit_research.md` | шапка `Status: REFERENCE`, тело без изменений | обязательное чтение по фиксированному пути; §2 — старая модель как улика, §3.5 — черновой порядок срезов |
| `docs/reference_notes/external_audit_recipe_model_20260902.md` | переименован, шапка `Status: REFERENCE` | KICKOFF §1.4, §8.5 |
| `docs/reference_notes/*` (остальные) | без изменений | исследования dag4blend/корпуса |
| `docs/receipts/recipe_d0.md` | удалён (заменён этой квитанцией) | квитанция v1-среза, снятого аудитом |
| `reference/dag4blend_patches/README.md` | ссылки на 13/15 → `docs/archive/` | документы перемещены |
| `tools/check_normative_docs.py` | создан | KICKOFF §7.2 |

## 4. CI и гейты

```bash
python tools/check_normative_docs.py
```

Вывод: `normative docs: OK`, exit 0. Проверки: покрытие индексом
(`README.md`, `KICKOFF_PROMPT.md`, `docs/**` вне archive/receipts); шапка
нормативного статуса у нормативных; шапка HISTORY у всех 31 файлов
`docs/archive/`; отсутствие markdown-ссылок из нормативных в `archive/`;
receipts без шапки нормативного статуса; лексический ноль удалённых сущностей кода
(блок `removed-entities` в 16 §7.2) в `README.md` и `docs/**` — **кроме**
`KICKOFF_PROMPT.md` (директива owner, называющая удаляемое) и
`docs/reference_notes/` (исследование и аудит цитируют старый код как улику).
`git diff --check` — PASS.

## 5. Решения исполнителя, требующие взгляда owner

1. **Область лексического гейта** сужена до нормативной прозы (`README.md`,
   `docs/**` без archive/receipts/reference_notes); `KICKOFF_PROMPT.md`
   исключён — иначе гейт не может быть зелёным в принципе.
2. **Список удалённых сущностей** = стартовый список KICKOFF §7.4 плюс
   `MHResolveCompositeDefinitionEndpoint` (KICKOFF §5 R0 называет его
   заменяемым; в коде — 3 файла).
3. **16 §3** — стаб с указателями на §2.4/§2.6 (нумерация §0–§4 KICKOFF
   сохранена ради сквозных ссылок; содержательно точки выхода описаны в §2.6).
4. **Doc 10 оставлен активным справочником**, не архивирован (KICKOFF §8.3
   допускает «остаются в переписанном документе со status-шапкой»).
5. **Шесть тегов** в 10 §7 записаны как `MH.<Name>` с запретом резолва по
   тегам в preview-плоскости.

## 6. Изменённые файлы

- `README.md`, `docs/16_recipe_model.md`, `docs/NORMATIVE_INDEX.md`,
  `docs/10_source_protocol_v5_plan.md`, `docs/receipts/recipe_d0a.md`;
- `docs/archive/` — 27 перемещённых `docs/*.md`, `proposals/`, `spikes/`,
  `README_pre_d0.md`, `KICKOFF_PROMPT_v1_20260902.md`;
- `docs/reference_notes/dagor_composit_research.md` (шапка),
  `docs/reference_notes/external_audit_recipe_model_20260902.md` (переименование + шапка);
- `reference/dag4blend_patches/README.md` (две ссылки);
- `tools/check_normative_docs.py`.

## 7. Следующие срезы

M0 (счётчики `registry_lookups`, `package_loads`, `identity_admissions` —
Sonnet 5), затем R0 `UMHEndpointPrototypeRegistry` с identity-admission
(Opus 5; red-тест и acceptance пишет Lead до выдачи). R2a (фазовый resolver +
shadow parity) — близнец.
