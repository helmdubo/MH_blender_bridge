# CE-5a (Composite Edit Mode) — публикация без потери сессии: NoExternalChange / SourceCommitted

Статус: **REVIEW** (близнец). Первая часть карточки CE-5 спецификации
(§10.2, A25, A26): shared publish из CE-backend сессии. Unique-batch и
`PartialBatch` (A27) — CE-5b.

## 1. Что сделано

- `UMHCompositeLevelSubsystem::PublishFromSession` — Commit/Apply из сессии
  (корень и вложенный) больше не закрывает сессию до публикации: preflight
  (extract, canonical bytes, previous) при открытой сессии, снимок байтов
  файла-цели до, `PublishDefinition` (apply + publish через test seam;
  неудача восстанавливает определение из authoritative source или prev-
  документа, как в R6-D2), классификация неудачи по файлу: байты после ==
  canonical draft'а и != байтов до → **SourceCommitted**, иначе
  **NoExternalChange**. Успех: `ResetEditSession` → `ResetTransaction` →
  rebuild размещения (граница source, как раньше).
- `EMHCompositePublishOutcome {None, Succeeded, NoExternalChange,
  SourceCommitted, PartialBatch}` + `GetLastPublishOutcome()`; при неудаче
  сессия, draft, режим и проекция живы (проекция обновляется после
  восстановления определения), epoch тот же — Retry возможен.
- SourceCommitted: `UMHCompositeEditSession::RebaseOriginal(committed)` —
  original и его байты = записанный документ, dirty считается от файла,
  Cancel не обещает прежний source (spec §10.2); warning
  `MH_W_SOURCE_COMMITTED`. Определение выравнивается по файлу: если после
  `RestoreDefinition` (reconcile из файла отказал → откат на pre-publish
  документ) байты определения ≠ canonical записанного, к ассету применяется
  известный записанный документ и консьюмеры уведомляются (второй green-
  прогон `CE5A_GREEN_TEST2.log`: 31/1 именно на этом). NoExternalChange:
  warning `MH_W_NO_EXTERNAL_CHANGE`.
- UI (`ExecuteCommitEditComposite`): при неудаче с живой сессией текст
  ошибки начинается с исхода («Source committed, import failed — …» /
  «Nothing was written — …, fix and Save again»); overlay режима остаётся.
- Legacy-путь (флаг выключен) не изменён: закрывает сессию до публикации.
  Ветка `PublishFromSession` берётся только при сессии **без** legacy
  edit-mode: на legacy-пути сессия-фасад (CE-1) тоже открыта, и первый
  полный прогон (`CE5A_FULL.log`: 262/2, `EditContext.ApplySharedDefinition*`)
  показал перехват legacy Apply — поправлено (`bSessionEdit && !bLegacyEdit`).

## 2. Тесты (red `9ffad0f`)

| Тест | Что проверяет |
|---|---|
| `Mimir.V5.Composite.EditMode.Publish.PreWriteFailureKeepsTheSessionAndTheSource` | publisher-hook отказывает без записи: Commit false с причиной, outcome NoExternalChange, та же сессия (epoch), режим и проекция живы, draft dirty с правкой, определение и файл byte-for-byte как до; Retry с рабочим publisher'ом → Succeeded, опубликован child, сессия закрыта, child несёт правку |
| `…Publish.PostWriteFailureReportsSourceCommitted` | hook пишет canonical draft'а в файл-цель и отказывает: outcome SourceCommitted, файл = draft, сессия и режим живы, original сессии = committed (clean), warning `MH_W_SOURCE_COMMITTED`, определение = файлу (reconcile из authoritative source); Cancel; файл удалён |

## 3. Гейты

| Gate | Результат |
|---|---|
| RED (`9ffad0f`) | `CE5A_RED_TEST.log`: Fail ×2 (`CE5A_RED_TEST2.log`; первый red `d452bab` не собрался — UHT) |
| GREEN non-unity/no-PCH build | `CE5A_GREEN_BUILD3.log`: Succeeded (первые прогоны red/green упали в UHT: `UENUM` между `USTRUCT()` и `struct`, поправлено во втором red-коммите) |
| `Mimir.V5.Composite.EditMode` | `CE5A_GREEN_TEST3.log`: 32/0 (`CE5A_GREEN_TEST2.log`: 31/1 до выравнивания определения по файлу, см. §1) |
| полный NullRHI suite | `CE5A_FULL.log`: 262/2 (legacy Apply перехвачен session-веткой, см. §1) → `CE5A_FULL2.log`: `Success=264 Fail=0 (262 + 2)` |
| force-unity | `CE5A_FORCE_UNITY2.log`: Succeeded |
| `BuildPlugin -StrictIncludes` | `CE5A_STRICT2.log`: ExitCode=0 (Success) |
| `git diff --check`, `check_normative_docs.py` | чисто / OK |

## 4. Изменённые файлы

Editor (5): `Public|Private/Composite/MHCompositeLevelSubsystem.{h,cpp}`,
`Public|Private/Editing/MHCompositeEditSession.{h,cpp}`,
`Private/UI/MHSourceToolMenus.cpp`. Tests: `MHCompositeEditPublishTest.cpp`
(новый). Docs: `docs/RECIPE_EXECUTION_STATUS.md`, эта квитанция.

## 5. Вопросы

- Тест SourceCommitted пишет `<child>.composite` под source root
  fixture (уникальное имя) и удаляет его в конце; при падении посреди
  теста файл-сирота безвреден (имя с per-run суффиксом).
- CE-5b: Save As Unique из сессии по тем же правилам (сессия живёт при
  pre-write неудаче), `PartialBatch` с перечислением созданных копий при
  сбое на второй копии, binding переносится последним.
- CE-6: lifecycle/cutover (флаг → default on, снятие legacy-пути), save
  map/PIE/level change, полевая приёмка.
