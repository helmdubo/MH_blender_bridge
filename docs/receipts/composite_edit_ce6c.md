# CE-6c (Composite Edit Mode) — baseline-метрики CE-backend на хосте

Статус: **REVIEW** (близнец). Часть карточки CE-6 «Метрики»: cold/warm
Enter/Exit, 100 Updates, peak components, 50 циклов. Числа фиксируются как
baseline хоста `E:\MimirComposite_R_M0_20260902` (NullRHI); пороги — по
согласованию с owner на этом baseline, не выдуманы.

## 1. Что сделано

- `Mimir.V5.Composite.EditMode.Perf.BaselineNumbersAreReported`
  (`MHCompositeEditPerfTest.cpp`): на fixture CE-0 (root ×2, child ×2,
  7 листьев) измеряет cold Enter/Exit вложенной сессии, warm Enter,
  100 `InputDelta` на листе через режим (жест CE-4a), warm Exit, 50 циклов
  Begin/Cancel; проверяет отсутствие projection-акторов после циклов;
  числа — в `AddInfo` (`PERF …`) лога, тест не судит.
- Baseline (`CE6C_GREEN_TEST.log`, NullRHI, non-unity/no-PCH Development):
  cold Enter 15.2–15.9 ms, cold Exit 0.9–1.1 ms, warm Enter 13.8–14.2 ms, 100 gizmo-updates 16.8–17.0 ms (≈0.17 ms/update), warm Exit 0.9–1.3 ms, 50 циклов Begin/Cancel 790–801 ms (≈16 ms/цикл), компонентов проекции 5–6 (два прогона `CE6C_RED_TEST.log`/`CE6C_GREEN_TEST.log`)

## 2. Тесты (red `3c00a4c`)

Срез измерительный: тест зелёный с первого прогона (нет контракта, который
он переворачивает); red-этап — компиляция/прогон теста без изменений
production-кода (`CE6C_RED_TEST.log`: 1/0 (измерительный тест, зелёный сразу)).

## 3. Гейты

| Gate | Результат |
|---|---|
| RED (`3c00a4c`) | `CE6C_RED_TEST.log`: 1/0 (измерительный тест, зелёный сразу) |
| GREEN non-unity/no-PCH build | `CE6C_GREEN_BUILD.log`: Succeeded |
| `Mimir.V5.Composite.EditMode` | `CE6C_GREEN_TEST.log`: 37/0 |
| полный NullRHI suite | `CE6C_FULL.log`: `Success=269 Fail=0 (268 + 1)` |
| force-unity | `CE6C_FORCE_UNITY.log`: Succeeded |
| `BuildPlugin -StrictIncludes` | `CE6C_STRICT.log`: ExitCode=0 (Success) |
| `git diff --check`, `check_normative_docs.py` | чисто / OK |

## 4. Изменённые файлы

Editor: нет. Tests: `MHCompositeEditPerfTest.cpp` (новый). Docs:
`docs/RECIPE_EXECUTION_STATUS.md`, эта квитанция.

## 5. Вопросы

- p50/p95 латентность на реальной сцене owner'а (portfolio) — при полевой
  приёмке; fixture слишком мал для порогов.
- Память после 50 циклов: тест проверяет отсутствие акторов; объём в байтах
  (`FPlatformMemory::GetStats`) добавим, если owner попросит порог.
- CE-6b (cutover) — после полевой приёмки owner'ом.
