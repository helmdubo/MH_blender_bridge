# Квитанция: удержание MH Composite Outliner при выборе ISM-инстанса

Статус: **READY FOR REVIEW**

Дата: 2026-09-01

База: `origin/main` `867f2cb`

Ветка: `codex/fix-outliner-ism-selection-collapse`

## 1. Дефект и границы

После U5 статический лист представлен typed-element выбором `SMInstance`.
Клик по mesh-строке MH Composite Outliner или прямой клик инстанса во
viewport заменял actor selection этим элементом. Старый
`RefreshSelectedActor` смотрел только `GEditor->GetSelectedActors()`, терял
владельца и очищал дерево. Выбор компонента через Details-иерархию не
воспроизводил дефект, потому что актор оставался в actor selection.

Исправление ограничено `ue/MimirComposite`. Wire/schema, enum, runtime/cook,
подписи, диагностические реестры, `golden/`, `reference/` и Blender-addon не
изменялись. Использовался собственный host
`E:\MimirComposite_OutlinerFix_20260901`; audit-host ревьювера не
использовался.

## 2. Реализация

- Резолв текущего актора вынесен из Slate-виджета в тестируемый helper
  `MHResolveCompositeOutlinerActor`.
- Actor selection сохраняет прежний приоритет. Если в нём есть ровно один
  уникальный `AMHCompositeActor`, он остаётся текущим независимо от typed
  selection. Несколько composite-акторов дают fail-closed `nullptr`.
- Только если composite-актора в actor selection нет, панель сканирует
  текущие typed-element handles, принимает только валидные `SMInstance` и
  передаёт их ISM-компоненты helper'у.
- Helper возвращает владельца только для одного уникального
  `AMHCompositeActor`. Несколько инстансов одного владельца допустимы;
  разные владельцы, чужой actor или отсутствие владельца не получают
  authority.
- Существующая U7 lease-логика не менялась. До и после смены способа выбора
  helper возвращает тот же указатель актора; условие
  `CurrentActor.Get() != NextActor` ложно, поэтому
  `ReleaseResolvedDebugPlan`/`RetainResolvedDebugPlan` не вызываются и lazy
  debug plan не пересобирается на клик.

## 3. Red-first

Механический вынос прежней actor-only логики и новый тест зафиксированы
коммитом `a4eb310` (`Add red-first Outliner instance retention coverage`).

Запуск:

```text
Automation RunTests Mimir.V5.Composite.Outliner.InstanceSelectionRetention
```

RED, `EXIT=255`, один тест найден, результат `Fail`:

```text
one selected SMInstance retains its composite owner: The two values are not equal.
multiple instances of one owner remain unambiguous: The two values are not equal.
```

Минимальный fallback зафиксирован коммитом `b088c15`
(`Retain Composite Outliner for selected ISM instances`). Повторный запуск:

```text
Found 1 automation tests
Test Completed. Result={Success} Name={InstanceSelectionRetention}
EXIT=0
```

Тест также закрепляет fail-closed случаи: два выбранных composite-актора,
инстансы двух разных composite-владельцев и ISM чужого обычного актора.

Логи собственного host:

- `E:\MimirComposite_OutlinerFix_20260901\Saved\Logs\OutlinerSelection_RED.log`
- `E:\MimirComposite_OutlinerFix_20260901\Saved\Logs\OutlinerSelection_GREEN.log`

## 4. Гейты

### Automation

Полный NullRHI с обязательным
`-MHGoldenRoot=E:\GITHUB\Mimirhead_UE5Exporter\MH_blender_bridge\golden`:

```text
Found 170 automation tests based on 'Mimir'
170 passed / 0 failed / 0 not run
EXIT=0
```

Это база `169/169` плюс новый
`Mimir.V5.Composite.Outliner.InstanceSelectionRetention`. В том же прогоне
зелёный U7-гейт
`Mimir.V5.Composite.CompactResolvedState.LazyDebugPlan`.

Лог: `E:\MimirComposite_OutlinerFix_20260901\Saved\Logs\Mimir_FULL.log`.

### Build

- Guarded force-unity editor build:
  `-NoEngineChanges -ForceUnity -DisableAdaptiveUnity -NoPCH -NoSharedPCH -WarningsAsErrors`
  — **Succeeded**, 22 actions, `73.46 s`.
- `BuildPlugin -StrictIncludes -DisableUnity -NoPCH -NoSharedPCH`
  (Editor Development + Game Development/Shipping, Win64) —
  **BUILD SUCCESSFUL**, `0:19:08`.
- `git diff --check` — чисто.
- Созданный UAT файл `ue/MimirComposite/Config/FilterPlugin.ini` удалён до
  сдачи.

## 5. Полевой протокол для независимого ревью

1. Выделить один `AMHCompositeActor` и открыть MH Composite Outliner — дерево
   и resolved-overlay видимы.
2. Кликнуть mesh-строку дерева, соответствующую ISM-листу — точный инстанс
   выделяется во viewport, дерево не схлопывается.
3. Кликнуть другой инстанс этого актора непосредственно во viewport —
   соответствующая строка подсвечивается, дерево остаётся открытым.
4. Выбрать компонент через Details-иерархию — прежнее поведение сохраняется.
5. Выделить инстансы двух разных composite-акторов — панель не выбирает
   произвольного владельца.
6. Повторять клики внутри одного актора — resident lazy debug plan не должен
   освобождаться и пересобираться.

## 6. Изменённые файлы

- `ue/MimirComposite/Source/MimirCompositeEditor/Public/UI/MHCompositeOutlinerModel.h`
- `ue/MimirComposite/Source/MimirCompositeEditor/Private/UI/MHCompositeOutlinerModel.cpp`
- `ue/MimirComposite/Source/MimirCompositeEditor/Private/UI/MHCompositeOutliner.cpp`
- `ue/MimirComposite/Source/MimirCompositeTests/Private/MHCompositeOutlinerSelectionTest.cpp`
- `docs/receipts/outliner_ism_selection_collapse.md`

## 7. Вопросы

Открытых архитектурных вопросов по этому срезу нет. Неоднозначный selection
остаётся fail-closed и не получает текущего актора.
