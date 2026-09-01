# Квитанция: пропуск свежей пересборки MH Composite Outliner

Статус: **READY FOR REVIEW**

Дата: 2026-09-01

База: `origin/main` `f651725`

Ветка: `codex/perf-outliner-skip-stale-rebuild`

## 1. Дефект и границы

После исправления typed-element selection из PR #57 каждый клик по ISM-листу
повторно входил в цепочку
`RefreshSelectedActor -> RefreshModel -> FMHCompositeOutlinerModel::BuildFromActor`.
Для неизменившегося актора это заново строило source tree и ленивый resolved
overlay, хотя U7 compact-state уже предоставляет достаточный freshness stamp.

Изменение ограничено selection-refresh MH Composite Outliner. Не менялись:

- Details customization и Details-иерархия;
- сбор typed-element selection и owner-fallback PR #57;
- безусловный путь `ComponentsEdited -> DeferredRefresh -> RefreshModel`;
- U7 `RetainResolvedDebugPlan`/`ReleaseResolvedDebugPlan`;
- runtime/cook, wire/schema, enum, подписи и resolver;
- E/W-реестры, `golden/`, `reference/` и Blender-addon.

Использован собственный host
`E:\MimirComposite_OutlinerFix_20260901`. Audit-host ревьювера не
использовался.

## 2. Реализация

### 2.1 Freshness stamp

Тестируемый non-Slate `FMHCompositeOutlinerFreshness` снимает только уже
публичные данные `AMHCompositeActor`:

- `Seed`;
- `AppearanceSeed`;
- `GetCompactResolvedSignature()`;
- `GetCompactAppearanceSignature()`;
- `GetCompactPlacementSignature()`.

Совпадение точное по всем пяти осям. Stamp считается complete только при трёх
непустых подписях. Два пустых stamp не считаются совпавшими: отсутствие
доказательства свежести означает rebuild.

### 2.2 Fail-closed gate

`FMHCompositeOutlinerRefreshState` хранит weak identity актора и stamp
последнего успешно построенного overlay. `RefreshSelectedActor` возвращается
до `RefreshModel` только когда одновременно:

1. текущий selection резолвится в тот же живой `AMHCompositeActor`;
2. сохранённый и текущий stamp complete;
3. оба seed и все три подписи совпадают побайтово.

Смена актора, изменение любой оси, пустой state или неуспешная пересборка
сбрасывают право на skip. Admission дополнительно требует, чтобы
`BuildFromActor` действительно восстановил resident U7 debug plan. Поэтому
source-only/error view не кэширует старые complete-подписи и не может
«залипнуть».

Lease-блок U7 оставлен без изменений. Fresh click того же актора не вызывает
ни lease churn, ни `RefreshModel`. `ComponentsEdited` по-прежнему вызывает
полную пересборку без обращения к gate и после неё обновляет stamp.

## 3. Red-first

Коммит `7701d97` (`Add red-first Outliner stale rebuild coverage`) добавил
freshness predicate, production wiring с прежним `always rebuild` и тест
`Mimir.V5.Composite.Outliner.StaleRebuildSkip`.

RED:

```text
Found 1 automation tests
Test Completed. Result={Fail} Name={StaleRebuildSkip}
Expected 'two unchanged selection refreshes rebuild once' to be 1, but it was 2.
EXIT=255
```

Все проверки пяти freshness-осей и пустого state в этом запуске прошли;
упал только интеграционный счётчик selection-refresh.

Лог:
`E:\MimirComposite_OutlinerFix_20260901\Saved\Logs\OutlinerStaleRebuild_RED.log`.

Минимальная реализация predicate в `NeedsRebuild` и fail-closed admission
зафиксированы коммитом `ff0fd98`
(`Skip fresh Composite Outliner selection rebuilds`).

GREEN:

```text
Found 1 automation tests
Test Completed. Result={Success} Name={StaleRebuildSkip}
EXIT=0
```

Интеграционный счётчик изменился с `2` rebuild requests в RED до `1` в
GREEN.

Тест закрепляет:

- несовпадение layout seed;
- несовпадение appearance seed;
- независимое несовпадение каждой из трёх подписей;
- empty/incomplete state;
- смену actor identity;
- сброс stamp после неуспешного rebuild;
- два последовательных refresh без изменений дают одну пересборку.

Финальный фокусный лог:
`E:\MimirComposite_OutlinerFix_20260901\Saved\Logs\OutlinerStaleRebuild_GREEN_FINAL.log`.

## 4. Гейты

### Automation

Полный NullRHI с
`-MHGoldenRoot=E:\GITHUB\Mimirhead_UE5Exporter\MH_blender_bridge\golden`:

```text
Found 171 automation tests based on 'Mimir'
171 passed / 0 failed / 0 not run
EXIT=0
```

Это база `170/170` плюс новый `StaleRebuildSkip`. В том же прогоне зелёные:

- `Mimir.V5.Composite.Outliner.InstanceSelectionRetention`;
- `Mimir.V5.Composite.CompactResolvedState.LazyDebugPlan`.

Лог:
`E:\MimirComposite_OutlinerFix_20260901\Saved\Logs\OutlinerStaleRebuild_FULL.log`.

### Build

- Guarded force-unity:
  `-NoEngineChanges -ForceUnity -DisableAdaptiveUnity -NoPCH -NoSharedPCH -WarningsAsErrors`
  — **Succeeded**, 22 actions, `106.55 s`. Один action был штатно
  перепоставлен UBA после memory-pressure threshold и успешно завершился.
- `BuildPlugin -StrictIncludes -DisableUnity -NoPCH -NoSharedPCH`, Win64 —
  **BUILD SUCCESSFUL**, `0:18:17`:
  Editor Development `113/113`, Runtime Development `11/11`, Runtime Shipping
  `11/11`.
- `git diff --check` — PASS.
- Служебный `ue/MimirComposite/Config/FilterPlugin.ini`, созданный UAT,
  удалён до сдачи.

## 5. Полевой протокол

1. Выделить `AMHCompositeActor`, открыть MH Composite Outliner и дождаться
   resolved overlay.
2. Несколько раз выбирать разные ISM-листья этого актора строкой дерева и
   прямым кликом во viewport. Дерево не схлопывается; повторный selection
   refresh не должен давать видимого rebuild-лага.
3. Выполнить layout reseed. Header, Decisions и выбранные строки обязаны
   обновиться, а не остаться на прежнем stamp.
4. Выполнить appearance reseed. Appearance overlay обязан обновиться.
5. Выполнить `Rebuild Composite` либо точечный reimport зависимости. Путь
   `ComponentsEdited` обязан полностью пересобрать дерево даже при совпавших
   seed.
6. Проверить actor с placement error/отсутствующим plan: incomplete state не
   должен разрешать skip; причина ошибки остаётся актуальной в шапке.

## 6. Изменённые файлы

- `ue/MimirComposite/Source/MimirCompositeEditor/Public/UI/MHCompositeOutlinerModel.h`
- `ue/MimirComposite/Source/MimirCompositeEditor/Private/UI/MHCompositeOutlinerModel.cpp`
- `ue/MimirComposite/Source/MimirCompositeEditor/Private/UI/MHCompositeOutliner.cpp`
- `ue/MimirComposite/Source/MimirCompositeTests/Private/MHCompositeOutlinerStaleRebuildTest.cpp`
- `docs/receipts/outliner_stale_rebuild_skip.md`

## 7. Вопросы

Открытых архитектурных вопросов нет. Любая неуверенность freshness gate
трактуется как необходимость полной пересборки.
