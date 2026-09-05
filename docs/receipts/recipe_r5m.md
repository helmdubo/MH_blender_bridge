# R5-M (Recipe Model v2.1) — ISM-usage материалов при admission меша

Статус: **MERGED** (близнец, #124 `70a89a4`). Полевой дефект owner 2026-09-05: минутные
фризы с `LogMaterial: Material … needed to have new flag set
bUsedWithInstancedStaticMeshes`. С R5b-1 каждый static-лист рендерится через
ISM, и движок лениво ставит usage-флаг базовому материалу в момент создания
render proxy — синхронно, посреди кадра.

## 1. Что сделано

- `UMHEndpointPrototypeRegistry`: при Ready-admission static-меша для каждого
  привязанного слота берётся базовый `UMaterial`; без
  `MATUSAGE_InstancedStaticMeshes` → `SetMaterialUsage` (кэш шейдеров
  асинхронно, пакет материала помечается dirty). Ленивый путь
  `CheckMaterialUsage` в кадре больше не срабатывает; после сохранения
  материалов (Save All) флаг постоянен и перекомпиляция не повторяется в
  следующих сессиях. Один файл.

## 2. Тесты (red `e7a2837`)

| Тест | Что проверяет |
|---|---|
| `Mimir.V5.Composite.Reconcile.AdmissionSetsInstancedMaterialUsage` | новый `UMaterial` без флага, привязан к мешу A, reimport-уведомление → реестр re-admit'ит меш (Ready) и флаг выставлен; размещение без ошибки |

## 3. Гейты

| Gate | Результат |
|---|---|
| RED (`e7a2837`) | `R5M_RED_TEST.log`: Fail |
| GREEN non-unity/no-PCH build | `R5M_GREEN_BUILD.log`: Succeeded |
| `Mimir.V5.Composite.Reconcile` | `R5M_GREEN_TEST.log`: 5/0 |
| полный NullRHI suite | `R5M_GREEN_FULL.log`: `Success=220 Fail=0` (219 + 1) |
| force-unity | `R5M_FORCE_UNITY.log`: Succeeded |
| `BuildPlugin -StrictIncludes` | `R5M_STRICT.log`: ExitCode=0 (Success) |
| `git diff --check`, `check_normative_docs.py` | чисто / OK |

## 4. Изменённые файлы

Editor: `Private/Composite/MHEndpointPrototypeRegistry.cpp`. Tests:
`MHResourceReconcileTest.cpp`. Docs: `docs/RECIPE_EXECUTION_STATUS.md`, эта
квитанция.

## 5. Вопросы и полевая проверка

Открытых нет. Owner: открыть карту портфолио, дождаться admission, **Save All**
(материалы помечены dirty) — в следующей сессии фризов компиляции по
usage-флагу быть не должно. Мелкие фризы `Using fallback RTPSO` — ray tracing
PSO, вне плагина.
