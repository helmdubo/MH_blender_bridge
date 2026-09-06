# CE-pre (Composite Edit Mode) — отложенный Apply привязан к сессии

Статус: **MERGED** (близнец, #138). Дыра из внешнего аудита
(`docs/reference_notes/MH_Composite_Edit_Mode_Spec_19b7515.md` §9): Apply,
отложенный на следующий тик для сессии A, срабатывал на любой активной к тому
моменту сессии.

## 1. Что сделано

- `UMHCompositeLevelSubsystem::GetEditSessionEpoch()` — растёт на каждом
  Begin (корневом и вложенном) и при закрытии сессии (`ResetEditSession`).
- `MHRunDeferredEditSessionApply(epoch)` — выполняет интерактивный commit
  только пока активна сессия с этим epoch; иначе no-op, `false`. Enter
  захватывает epoch в момент постановки в очередь.
- `docs/contracts/composite_edit_ce0.md` — решения owner 2026-09-06 и
  верифицированный на локальной UE 5.7.4 API (file+line) для CE-программы.

## 2. Тесты (red `76c75d8`)

| Тест | Что проверяет |
|---|---|
| `Mimir.V5.Composite.EditContext.QueuedApplyIgnoresLaterSession` | сессия A ставит Apply (epoch захвачен), A отменена, начата B на другом размещении: устаревший callback — no-op (B редактируется, подтверждения нет, публикации нет, источник прежний); текущий epoch применяет B один раз |

## 3. Гейты

| Gate | Результат |
|---|---|
| RED (`76c75d8`) | `CEPRE_RED_TEST.log`: Fail |
| GREEN non-unity/no-PCH build | `CEPRE_GREEN_BUILD.log`: Succeeded |
| `Mimir.V5.Composite.EditContext` | `CEPRE_GREEN_TEST.log`: 11/0 |
| полный NullRHI suite | `CEPRE_FULL.log`: `Success=232 Fail=0` (231 + 1) |
| force-unity | `CEPRE_FORCE_UNITY.log`: Succeeded |
| `BuildPlugin -StrictIncludes` | `CEPRE_STRICT.log`: ExitCode=0 (Success) |
| `git diff --check`, `check_normative_docs.py` | чисто / OK |

## 4. Изменённые файлы

Editor: `Public/Composite/MHCompositeLevelSubsystem.h`,
`Private/Composite/MHCompositeLevelSubsystem.cpp`, `Public|Private/UI/MHEditSessionKeys.{h,cpp}`.
Tests: `MHCompositeEditContextTest.cpp`. Docs: `docs/contracts/composite_edit_ce0.md`,
`docs/RECIPE_EXECUTION_STATUS.md`, эта квитанция.

## 5. Вопросы

Открытых нет. Следующий — CE-0 spike в редакторе (сохранение/PIE/dimming) и
red-fixture, затем CE-1.
