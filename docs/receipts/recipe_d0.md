# D0 — документальный срез программы Recipe Model

Статус: **READY FOR REVIEW**. Кода в срезе нет; только документы.

## 1. База и границы

- ветка: `recipe/d0-docs`;
- база: `origin/main` `038ccc8` (Adopt recipe model: replace kickoff prompt,
  add Dagor research);
- изменения ограничены `README.md`, `docs/`, ссылками в
  `reference/dag4blend_patches/README.md`; `ue/`, `addon/`, `golden/`,
  `tests/`, `tools/` не изменялись;
- норматив после среза — ровно три файла: `KICKOFF_PROMPT.md`,
  `docs/16_recipe_model.md`, `README.md` (KICKOFF §7.1).

## 2. Acceptance KICKOFF §8

| # | Критерий | Результат |
|---|---|---|
| 1 | `docs/16_recipe_model.md` создан: §3–§4 KICKOFF как норматив, таблица удалённых терминов, точки выхода, OPEN-вопросы | выполнено: 16 §2, §4 (норматив), §7 (термины + греп-гейт), §3 (точки выхода), §9 (OPEN-R-1…5) |
| 2 | 10/11/12/14/15: разделы про applied state, подписи, definition cache, admission на инстансе, U0–U7 — переписаны или документ в `docs/archive/` | 10 переписан (§3 ниже); 11, 12a, 12b, 14, 15 — архив с шапкой §7.1; wire-формат, сиды, runtime-мост сохранены в 10 (§6.1–6.9, §7, §8, §13) |
| 3 | `KICKOFF_PROMPT.md` заменён текстом Recipe Model | уже выполнено owner в `038ccc8`; в D0 не менялся, проверено совпадение (§8 acceptance, OPEN-R-4, `MH_W_ORPHAN_OVERRIDE`, `recipe_d0`) |
| 4 | `README.md` обновлён: норматив = три файла; receipts = история | выполнено; прежнее тело README (v5/v4/v2) — `docs/archive/README_pre_d0.md` |
| 5 | Грепы §7.3 = 0 вне архива и receipts | выполнено, команды и вывод — §4 |
| 6 | Проверено, смержен ли `codex/m0-perf-instrumentation` | смержен: PR #60 (`5e6024d`) в `main` до базы D0; R0 базируется на нём и добавляет счётчики `registry_lookups`, `package_loads`, `receipt_validations` (16 §8) |
| 7 | Квитанция с таблицей §7.4 | этот файл, §3 |

## 3. Документ → действие → почему (KICKOFF §7.4)

| Документ | Действие | Почему |
|---|---|---|
| `KICKOFF_PROMPT.md` | без изменений | заменён owner в базе `038ccc8` |
| `README.md` | переписан | прежний README нёс три вложенных «архивных README» с superseded-баннерами (запрещено §7.1) и упоминал applied state; новый: банер трёх нормативов, модель в одном экране, карта документов, состояние реализации, полевые команды. Убраны ссылки на несуществующие `tools/ue_s6_host/README.md`, `tools/check_dag4blend_compat.py` и меню «Mimir FBX Dump» (в коде только commandlet) |
| `docs/16_recipe_model.md` | создан | норматив модели |
| `docs/10_source_protocol_v5_plan.md` | переписан (вырезание) | wire-формат, identity, индекс, receipt, сиды остаются нормативом протокола (KICKOFF §8.2); удалены/заменены: статус-шапка (freeze-кандидат, ссылки на 11 и `QUESTIONS.md`), «applied state» → «receipt» (§1, §3, §5, §6, §7, §11, §13.4, §13.12), подпись актора в §6.6/§6.7/§7/§13.3/§13.8/§13.12, §6.7 переписан под модель 16 и runtime-мост, §7 получил правило «где читается receipt», шесть тегов записаны как `MH.<Name>`, §12 заменён указателем на архив, добавлен §6.9 (appearance seed — перенесён из архивируемого 12b §3–4, чтобы контракт `mh.appearance:1` не остался только в истории) |
| `docs/11_v5_agent_slices.md` | архив | порядок работ v5 завершён; активная программа — KICKOFF §5 |
| `docs/12_v5_s6_1_s6_2_slices.md`, `docs/12_v5_s6_2_s6_3_slices.md` | архив | подписи, три уровня хэшей, admission на инстансе; выживший контракт appearance seed перенесён в 10 §6.9 |
| `docs/13_v5_s6_1_dag4blend_bridge.md` | архив | история S6.1 (упоминает подпись и definition cache); нормативный остаток уже в 10 §13.13 |
| `docs/14_v5_ue_editor_program.md` | архив | shared definition cache, перф-программа U0–U7 |
| `docs/15_v5_s6_1_1_hardening.md` | архив | история S6.1.1/S6.1.2 (KICKOFF §1) |
| `docs/00…09`, `ADR_*`, `AMENDMENT_*`, `C0/C1_*`, `RISK_RESULTS`, `ROADMAP` | архив | история v1–v4, уже superseded; §7.1 не оставляет в `docs/*.md` документов вне трёх нормативов и справочника 10 |
| `docs/QUESTIONS.md` | архив | история вопросов; решённый остаток — 10 §13; активные OPEN — 16 §9 |
| `docs/proposals/*`, `docs/spikes/*` | архив | основа старого definition cache; passport-спайки v2 |
| `docs/reference_notes/dagor_composit_research.md` | шапка RESEARCH, тело без изменений | обязательное чтение KICKOFF §1.3 по фиксированному пути; §2 описывает старую модель как улику, §3.5 — черновой порядок срезов, отличающийся от KICKOFF §5 |
| `docs/reference_notes/*` (остальные) | без изменений | исследования dag4blend/корпуса, терминов не содержат |
| `docs/receipts/*` | без изменений | история исполнения |
| `reference/dag4blend_patches/README.md` | ссылки на 13/15 → `docs/archive/` | документы перемещены |

Ссылки внутри архивированных документов на `10_source_protocol_v5_plan.md`
перенаправлены на `../10_…`; остальные относительные ссылки между
архивированными файлами сохраняют работоспособность (перемещены вместе).

## 4. Греп-гейт

Команды из 16 §7, база `docs/`, `README.md`:

```bash
T='ResolvedSignature|CompactResolvedState|PlacementDependencies|ClosureHash|AppliedDefinition|AppliedGraph|definition cache|definition-кэш|MHLoadAppliedResource|AppliedPlanReceipt|FinalizeDeferredMeshes|applied[ -]state|MH\.Managed|MH\.Kind'
grep -rIil -E "$T" README.md docs --exclude-dir=archive --exclude-dir=receipts --exclude-dir=reference_notes --exclude=16_recipe_model.md
sed -n '/^## 7\./,/^## 8\./!p' docs/16_recipe_model.md | grep -i -E "$T"
```

Вывод обеих команд пуст. До среза те же термины встречались в 15 активных
файлах (`README.md`, 03, 08, 09, 10, 11, 12a, 12b, 13, 14, ADR_V3,
C1_AUDIT_REPORT, C1_SLICE_NOTES, QUESTIONS, proposals).

Область гейта уточнена исполнителем (16 §7): исключены `docs/reference_notes/`
(исследования, KICKOFF §1.3 требует читать их по фиксированному пути) и
таблица терминов самого 16 §7. `git diff --check` — PASS.

## 5. Решения исполнителя, требующие взгляда owner

1. **Хэш плана резолвера — OPEN-R-5 (16 §9).** Resolver в runtime-модуле,
   `AMHRuntimeCompositeActor`, отчёт плана и `golden/v5/*` хранят
   `resolved_signature` / `appearance_signature` / `placement_signature`;
   KICKOFF §2 их замораживает, §3.5 удаляет подпись «из всех документов».
   Временное правило: удаляется только свойство редакторского актора и его
   гейт-семантика (R2b); в документах хэш плана назван по имени поля golden
   (`resolved_signature`) как артефакт паритета. Ответ нужен до R2b.
2. **Doc 10 оставлен активным справочником**, а не архивирован: KICKOFF §8.2
   допускает «оставлены в 10 после вырезания», а линия S и Blender-сторона
   нуждаются в protocol-контрактах по стабильному пути. Норматив модели
   актора в нём отсутствует — только указатели на 16.
3. **Шесть тегов** в 10 §7 записаны как `MH.<Name>` (Kind, LogicalName,
   SourcePath, SourceHash, AppliedHash, Managed) с явным запретом резолва по
   тегам: имена тегов — часть receipt-контракта индекса, который линия S не
   меняет.
4. **Все документы вне трёх нормативов и 10 перемещены в архив**, включая
   00–09/ADR/amendments (были superseded ранее) и Blender-документы 13/15:
   §7.1 не предусматривает третьего состояния «оставлен без изменений».

## 6. Изменённые файлы

- `README.md`, `docs/16_recipe_model.md` (новый),
  `docs/10_source_protocol_v5_plan.md`, `docs/receipts/recipe_d0.md` (этот);
- `docs/archive/` — 27 перемещённых `docs/*.md`, `proposals/`, `spikes/`,
  `README_pre_d0.md`;
- `docs/reference_notes/dagor_composit_research.md` (шапка);
- `reference/dag4blend_patches/README.md` (две ссылки).

## 7. Следующий срез

R0 — `UMHLeafPrototypeRegistry` (KICKOFF §5): ветка `recipe/r0-…` от
`origin/main` после owner merge D0; red-тест — загрузка карты с двумя
акторами одного ассета: `registry_lookups == uniqueKeys`, вызовов
`IAssetRegistry::GetAssets` = 0, `FAssetData(&Object)` = 0.
