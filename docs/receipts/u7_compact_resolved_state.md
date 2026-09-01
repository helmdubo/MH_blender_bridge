# Квитанция U7 — compact resolved state + lazy debug trace

Дата: 2026-09-01  
Ветка: `codex/u7-compact-resolved-state`  
База: `origin/main` `8ca913c` (merge U5, PR #55)  
Статус: **READY FOR REVIEW**

## 1. Границы

- работа выполнена только в `ue/MimirComposite` и этой квитанции;
- stock Unreal Engine 5.7, Engine не менялся и fork Engine не создавался;
- использован собственный host
  `E:\MimirComposite_U5_20260901`; audit-host Lead не открывался и не
  модифицировался;
- runtime tiny transport, wire, resolver, формат, подписи, enum/binary,
  реестры E/W-кодов, `golden/` и `reference/` не менялись.

## 2. Red-first

Отдельный коммит `a01477e` добавил тест
`Mimir.V5.Composite.CompactResolvedState.LazyDebugPlan` и измерение
resident resolved-state в существующий cottage probe.

До оптимизации тест ожидаемо упал:

```text
Test Completed. Result={Fail} Name={LazyDebugPlan}
Expected 'ordinary placement retains no full debug plan' to be false.
Expected 'compact state retains no draws, decisions, or preimages' to be false.
```

Полный вывод:
`E:\MimirComposite_U5_20260901\u7_red.log`, строки 785–790.

Baseline на реальном `sovmod_cottage_i_cmp`, 849 resolved leaves:

```text
phase=cold wall_ms=151361.837600 resolved_state_bytes=4247217 debug_resident=1
phase=warm wall_ms=108.899500 resolved_state_bytes=4247217 debug_resident=1
```

Лог: `E:\MimirComposite_U5_20260901\u7_baseline_cottage.log`.

## 3. Реализация

### 3.1 Постоянное compact-состояние

`AMHCompositeActor` после успешной материализации больше не сохраняет
`FMHResolvedCompositePlan`. Editor-only `FMHCompactResolvedState` содержит:

- layout/appearance seeds;
- интернированный массив resource names и `ResourceIndex` у листа;
- полный `FMatrix` листа, kind, root/resolved node indices;
- четыре appearance-канала CPD;
- выбранные option indices;
- `ResolvedSignature`, `AppearanceSignature`, `PlacementSignature`.

В compact-state отсутствуют `Closure`, `Nodes`, `Decisions`, `Draws`, оба
`SignaturePreimage`, appearance draws, `SelectedDependencies`, display names,
NodePath и diagnostic TRS.

Существующий U5 mapping `LeafMaterializations` остаётся plan-aligned и хранит
`(component, instance index) -> ResolvedNodeIndex/NodePath`. Допустимая
контрактом замена NodePath на отдельную lazy path table здесь сознательно не
выполнена: это расширило бы изменение U5 typed-selection/Edit Mode mapping при
уже достигнутом снижении resident plan на 95.91%.

### 3.2 Lazy debug plan

`GetResolvedPlan()` теперь лениво повторяет тот же frozen resolver над
immutable definition и сохранёнными seeds. Admission требует точного совпадения
всех трёх подписей с compact-state и повторной transform-валидации. Любое
расхождение возвращает `nullptr` и пишет существующий
`MH_E_PLACEMENT_STATE_DESYNC`; устаревший plan не выдаётся.

Outliner удерживает plan lease только пока открыта панель и выбран actor.
Переключение actor/закрытие панели освобождает trace. Команды просмотра в
Message Log и Level operations используют парные lease/release. Rebuild
инвалидирует debug cache; открытый Outliner восстанавливает его по событию на
следующем refresh без polling.

Basis-update и предыдущая сторона incremental reseed получают полный plan
временно из того же resolver и освобождают его после операции. Это сохраняет
точную проверку всех per-node world matrices; приближённый `FTransform` или
ослабленная leaf-only admission не вводились.

## 4. Green и паритет

Фокусный тест после реализации:

```text
Test Completed. Result={Success} Name={LazyDebugPlan}
```

Лог: `E:\MimirComposite_U5_20260901\u7_green_focus_final.log`.

Тест доказывает:

- сразу после placement полный debug plan отсутствует;
- compact-state содержит 12 листьев и 12 выбранных option indices, но не
  содержит trace/preimage;
- первый inspection восстанавливает Decisions, layout draws, appearance draws
  и оба preimage;
- lazy `ResolvedSignature`, `AppearanceSignature`, `PlacementSignature`
  совпадают с подписанным compact-state;
- освобождение и повторное разрешение дают байт-идентичные layout/appearance
  preimage, NodePath решений и выбранные option indices;
- последний Outliner-style lease освобождает полный plan.

Весь `Mimir.V5.Composite` после реализации: **68/68, 0 failed**. В него входят
Lifecycle, DefinitionPool, IncrementalReseed, U5 ISM selection/Edit Mode и
Outliner model.

## 5. Cottage до/после

Один и тот же asset, seeds `1729/2718`, stock UE, один host:

| Метрика | До U7 | После U7 | После/до |
|---|---:|---:|---:|
| resolved leaves | 849 | 849 | 100% |
| resident resolved-state на actor | 4,247,217 B | 173,524 B | **4.09%** |
| экономия resident state | — | 4,073,693 B | **95.91%** |
| full debug plan сразу после placement | да | нет | — |
| cold wall | 151,361.838 ms | 150,902.921 ms | 99.70% |
| warm wall | 108.900 ms | 109.079 ms | 100.16% |
| warm ResolveCompositePlan | 27.886 ms | 27.055 ms | 97.02% |
| warm CompilePlacement | 23.083 ms | 19.202 ms | 83.19% |

Cold wall по-прежнему доминируется BuildAppliedGraph/первым endpoint load и не
является целью U7. Warm wall приведён информационно; измеримого ухудшения нет.
После U7 `debug_resident=0` и на cold, и на warm actor.

После-лог:
`E:\MimirComposite_U5_20260901\u7_after_cottage.log`, строки 816–819.
Для замера `Content` собственного host временно заменялся junction на portfolio
`Content`; после теста junction проверен как junction, удалён, локальный каталог
восстановлен. Пакеты portfolio не сохранялись и не изменялись.

## 6. Гейты

| Gate | Результат |
|---|---|
| полный NullRHI `Mimir` с `-MHGoldenRoot=<repo>/golden` | **169/169, 0 failed** |
| `Mimir.V5.Composite` | **68/68, 0 failed** |
| guarded force-unity `-NoEngineChanges -ForceUnity -DisableAdaptiveUnity -NoPCH -NoSharedPCH -WarningsAsErrors` | **Succeeded**, 22/22 actions |
| `BuildPlugin -StrictIncludes -DisableUnity -NoPCH -NoSharedPCH` | **BUILD SUCCESSFUL**; Editor 111/111, Runtime Development 11/11, Runtime Shipping 11/11 |
| `git diff --check` | **PASS** |

Полный NullRHI лог:
`E:\MimirComposite_U5_20260901\u7_full_mimir.log`.
Все 169 Automation-тестов выполнены. Три informational `NOT RUN` внутри
успешных тестов ожидаемы: два RHI smoke требуют отдельный non-NullRHI флаг,
cottage probe выполнялся отдельным реальным lane из §5.

## 7. Изменённые файлы

Production:

- `MHCompositeActor.h/.cpp`;
- `MHCompositeLevelSubsystem.cpp`;
- `MHCompositeOutliner.cpp`;
- `MHSourceToolMenus.cpp`.

Tests/metrics:

- `MHCompositeISMMaterializationTest.cpp`;
- `MHCompositeISMCottageMetricsTest.cpp`.

Documentation:

- `docs/receipts/u7_compact_resolved_state.md`.

## 8. Вопросы Lead

Нет. Временных семантических допущений и новых authority не введено.
