# R6-UX2a (Recipe Model v2.1) — Edit Contents: Esc отменяет, Enter применяет

Статус: **MERGED** (близнец, #135). Полевой отзыв owner 2026-09-06: выйти из сессии
или принять правку можно было только правым кликом по композиту в Outliner.

## 1. Что сделано

- `UI/MHEditSessionKeys.{h,cpp}`: `MHEditSessionKeyAction(Key, bSessionActive)`
  — чистое решение (Esc → Cancel, Enter → Apply при активной сессии, иначе
  None); `MHHandleEditSessionKey(Key, bDeferApply)` — Cancel сразу через
  `CancelEditComposite`, Apply через `MHExecuteCommitEditCompositeInteractive`
  (обычное подтверждение перезаписи; по умолчанию откладывается на следующий
  тик редактора, чтобы модал не открывался из пути ввода).
- Slate input pre-processor (`MHRegisterEditSessionKeys` при старте модуля,
  снятие при shutdown): перехватывает Esc/Enter только при активной сессии и
  только когда фокус клавиатуры в level viewport (`SViewport`); текстовые поля
  и другие панели не затрагиваются.
- Composite Outliner: `OnKeyDown` панели — те же клавиши; пункты меню
  «Cancel Edit Contents (Esc)» (Outliner и Composite Actions); статусная строка
  «Enter applies, Esc discards».

## 2. Тесты (red `ba7338a`)

| Тест | Что проверяет |
|---|---|
| `Mimir.V5.Composite.EditContext.EscapeCancelsEnterApplies` | таблица решений (Esc/Enter/другая клавиша/без сессии); без сессии клавиши не перехватываются; Esc после сдвига хэндла — сессия закрыта, лист на месте, источник не тронут; Enter (hooks подтверждения → true, seam publisher) — подтверждение спрошено один раз, опубликован ребёнок, сессия закрыта, A рендерит правку |

## 3. Гейты

| Gate | Результат |
|---|---|
| RED (`ba7338a`) | `R6UX2A_RED_TEST2.log`: Fail |
| GREEN non-unity/no-PCH build | `R6UX2A_GREEN_BUILD.log`: Succeeded |
| `Mimir.V5.Composite.EditContext` | `R6UX2A_GREEN_TEST.log`: 10/0 |
| полный NullRHI suite | `R6UX2A_GREEN_FULL.log`: `Success=231 Fail=0` (230 + 1) |
| force-unity | `R6UX2A_FORCE_UNITY.log`: Succeeded |
| `BuildPlugin -StrictIncludes` | `R6UX2A_STRICT.log`: ExitCode=0 (Success) |
| `git diff --check`, `check_normative_docs.py` | чисто / OK |

## 4. Изменённые файлы

Editor: `Public/UI/MHEditSessionKeys.h`, `Private/UI/MHEditSessionKeys.cpp`
(новые), `Private/MimirCompositeEditorModule.cpp`,
`Private/UI/MHCompositeOutliner.cpp`, `Private/UI/MHSourceToolMenus.cpp`.
Tests: `MHCompositeEditContextTest.cpp`. Docs: `docs/RECIPE_EXECUTION_STATUS.md`,
эта квитанция.

## 5. Вопросы

Открытых нет. Следующий — R6-UX2b: одна кнопка «Save As Unique Copy...» с
диалогом (область + Bake) вместо четырёх пунктов меню.
