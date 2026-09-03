# R2c — точки выхода proof-плоскости

Статус: **READY FOR RE-REVIEW** после возврата PR #88. Background proof cache,
non-cook save audit и синхронные proof-гейты preflight/snapshot/Break
реализованы. OPEN-R2C-3/4 исправлены; все acceptance-тесты, полный NullRHI
suite и сборочные гейты повторно прошли; открытых вопросов нет.

## 1. База, ветка и host

- ветка: `recipe/r2c-exit-points`; merge-base с `origin/main`:
  `ec1d5ffc402f31c5955fa6c2106fd3b640576e60` (#87);
- red-коммиты близнеца: `94938d7` (API, stub и
  `BuildPreflightFullClosure`), `4b38f7a` (два остальных proof-теста),
  `14a2d7d` (точные счётчики и коды diagnostic registry);
- контрактный HEAD до первого review: `713fa66`; ответ близнеца на
  OPEN-R2C-3/4: `c998ae9`;
- green implementation: `e55a049`; реализация возврата: `2bb0b45`;
- отдельный worktree:
  `E:\GITHUB\Mimirhead_UE5Exporter\MH_blender_bridge_r2c_executor`;
- собственный module-free host:
  `E:\MimirComposite_R2C_External_20260903\HostProject.uproject`;
  stock UE 5.7.4: `D:\PersonalProjects\UE5\UE_5.7`; plugin подключён
  junction'ом к R2c-worktree;
- основной checkout `main`, Engine, Blender addon, `golden/` и
  `reference/` не изменялись; `main` не пушился.

## 2. Red-first

Нетронутый HEAD `713fa66` сначала собран обязательной командой
non-unity/no-PCH:

```text
E:\MimirComposite_R2C_External_20260903\Saved\Logs\R2C_RED2_BUILD_NONUNITY.log
1967: Result: Succeeded
```

Первым тестовым действием после сборки был
`Automation RunTests Mimir.V5.Composite.Proof`:

```text
E:\MimirComposite_R2C_External_20260903\Saved\Logs\R2C_RED2_PROOF.log
1079: Test Completed. Result={Fail} Name={BuildPreflightFullClosure}
1081: Expected 'request schedules a deferred proof' to be true.
1084: Expected 'stale proof is Stale: MH_E_NOT_IMPLEMENTED: ...' to be true.
1088: Expected 'missing proof is Missing: MH_E_NOT_IMPLEMENTED: ...' to be true.
1097: Test Completed. Result={Fail} Name={SaveWarnsWithoutProof}
1099: Expected 'save schedules the missing proof' to be true.
1100: Expected 'save warned about the unproven placement' to be true.
1101: Expected 'deferred proof became Fresh' to be true.
1108: Test Completed. Result={Fail} Name={StaleSourceBlocksCookAndSnapshot}
1110: Expected 'stale source refuses cook preflight' to be false.
1112: Expected 'stale source refuses snapshot admission' to be false.
1114: Expected 'cache reports Stale' to be true.
```

Diagnostic-registry red:

```text
E:\MimirComposite_R2C_External_20260903\Saved\Logs\R2C_RED2_REGISTRY.log
1075: Test Completed. Result={Fail} Name={StreamTraceAndSignatureParity}
1077: Expected 'exact registered MH_E count' to be 54, but it was 53.
1078: Expected 'stale source code is registered' to be true.
1079: Expected 'exact registered MH_W count' to be 20, but it was 16.
1080–1083: Expected proof warning MH_W_PROOF_{UNKNOWN,PENDING,STALE,MISSING} is registered.
```

Падения совпадают с red-контрактом: три proof-теста — только на stub
`MH_E_NOT_IMPLEMENTED`, registry-тест — только на недостающих 1 error и
4 warning-кодах.

## 3. Реализация

- `UMHProofCacheSubsystem` кэширует `ProofPending/Fresh/Stale/Missing` по
  root asset, `RecipeRevision`, двум seed'ам, generation ProjectIndex,
  `ImporterVersion` и endpoint revisions полного набора зависимостей. Сдвиг
  ключа читается как `Unknown`.
- `BuildProofNow` строит applied full closure, проверяет claims каждого
  dependency, выполняет resolve и transform admission, затем сравнивает
  receipt raw hash только с `FMHProjectResourceIndex::Resolve` либо тестовым
  provider. Неизвестный индексу ресурс не считается stale; расхождение даёт
  точный `MH_E_STALE_SOURCE`.
- Deferred queue выполняет не более одного живого placement за game-thread
  tick; `FlushPendingProofs` исполняет очередь целиком. `AuditWorld` только
  читает кэш.
- Non-cook `PreSaveWorldWithContext` пишет один
  `MH_W_PROOF_{UNKNOWN,PENDING,STALE,MISSING}` на каждый non-Fresh placement
  и только планирует Unknown; proof синхронно не строится и save не блокируется.
- Cook/build preflight, runtime snapshot и Break используют
  `BuildProofNow`; resource notification инвалидирует proof cache.
- Proof-операции получают только уже открытый process-owned индекс через
  `MHPeekProjectIndex()`: accessor не создаёт, не открывает и не пересоздаёт
  SQLite. `MakeKey`, `GetProofState`, `AuditWorld` и `RequestProof` не имеют
  index-write side effect; при закрытом индексе generation равен 0, а
  неизвестный индексный хэш не считается stale.
- `MHRuntimeBridgePreflight` после подготовки транспорта независимо декодирует
  точные `GraphBytes`, выполняет resolve и transform admission, затем сравнивает
  `ResolvedSignature` и `PlacementSignature` транспортного плана с Fresh
  proof-планом из кэша. Расхождение закрывается точным
  `MH_E_INVALID_RESOURCE_SOURCE: transport graph diverges from proof for <path>`.
- Diagnostic registry содержит ровно новый `MH_E_STALE_SOURCE` и четыре
  предусмотренных контрактом warning-кода, без иных добавлений.

## 4. Green и acceptance

Повторный green proof-лог после OPEN-R2C-3/4:

```text
E:\MimirComposite_R2C_External_20260903\Saved\Logs\R2C_RETURN1_PROOF_NO_SOURCE_ROOT.log
1080: Test Completed. Result={Success} Name={BuildPreflightFullClosure}
1089: Test Completed. Result={Success} Name={SaveWarnsWithoutProof}
1098: Test Completed. Result={Success} Name={StaleSourceBlocksCookAndSnapshot}
```

Host не содержит настроенного Source Root. Перед прогоном существующий
generated cache был перемещён в восстанавливаемый каталог
`Saved\MimirBridge\R2C_RETURN1_PREEXISTING_INDEX_BACKUP\ProjectIndex.sqlite`.
Сразу после завершения proof-фильтра ручная проверка
`Test-Path E:\MimirComposite_R2C_External_20260903\Saved\MimirBridge\ProjectIndex.sqlite`
вернула `False`: чтение/сборка proof без Source Root файл не создали. Позднее
полный suite ожидаемо создал process-owned индекс в source-тестах.

| # | Критерий | Результат |
|---|---|---|
| 1 | `Proof.BuildPreflightFullClosure`: preview Unknown, deferred Fresh, Stale, Missing, read-only audit; без Source Root не создаёт SQLite | PASS — `R2C_RETURN1_PROOF_NO_SOURCE_ROOT.log:1080`; после процесса `ProjectIndex.sqlite` отсутствует |
| 2 | `Proof.SaveWarnsWithoutProof`: save строит 0 applied graphs, предупреждает и планирует proof; второй save после flush чист; без Source Root не создаёт SQLite | PASS — `R2C_RETURN1_PROOF_NO_SOURCE_ROOT.log:1089`; после процесса `ProjectIndex.sqlite` отсутствует |
| 3 | `Proof.StaleSourceBlocksCookAndSnapshot`: stale блокирует preflight/snapshot, preview остаётся рабочим, refresh лечит | PASS — `R2C_RETURN1_PROOF_NO_SOURCE_ROOT.log:1098` |
| 4 | точный diagnostic registry | PASS — `R2C_IMPL_REGISTRY_01.log:1075`, `StreamTraceAndSignatureParity` Success |
| 5 | `Mimir.V5.Composite.*`, включая AppliedAdmission/Recipe/Break | PASS — 86/86, 0 Fail (`R2C_IMPL_COMPOSITE_02.log`); `RecipeShadowParity` — строка 1750 |
| 6 | `Mimir.V5.Runtime.*`, включая восстановленный decode/resolve/transform transport preflight | PASS — 15/15, 0 Fail (`R2C_RETURN1_RUNTIME.log`, completed строки 1092–1193) |
| 7 | `Mimir.Audit.MainBaseline.BuildPreflight*` | PASS — `R2C_IMPL_BUILD_PREFLIGHT_01.log:1109` |
| 8 | полный `Automation RunTests Mimir` после возврата | PASS — **191/191 Success, 0 Fail** (`R2C_RETURN1_FULL.log`); proof — строки 4010/4019/4028, registry — 4572, последний completed — 4782, `TEST COMPLETE` — 4794 |
| 9 | число тестов не уменьшилось | PASS — R2b baseline 188; три red proof-теста R2c дают 191; удалений нет |

Предварительный `Mimir.V5.Composite` прогон выявил единственную совместимую
регрессию текста `LevelOperations`: старый fail-closed сценарий без resident
preview ожидает фразу `resolved plan`. Формулировка восстановлена в production
коде без изменения теста; повторный полный composite-прогон — 86/86.

## 5. Сборочные и статические гейты

| Gate | Результат |
|---|---|
| final non-unity/no-PCH editor build, `-NoEngineChanges -WarningsAsErrors` | PASS — `R2C_RETURN1_BUILD_NONUNITY_02.log:2004`, `Result: Succeeded` |
| guarded force-unity, `-ForceUnity -DisableAdaptiveUnity -NoPCH -NoSharedPCH` | PASS — `R2C_RETURN1_BUILD_FORCEUNITY.log:1989`, `Result: Succeeded` |
| `BuildPlugin -StrictIncludes -DisableUnity -NoPCH -NoSharedPCH` | PASS — `R2C_RETURN1_STRICTINCLUDES.log:1775`, `BUILD SUCCESSFUL`; строка 1777 — `ExitCode=0`; отдельный package `E:\MimirComposite_R2C_Return1_Strict_20260903_1454` |
| `git diff --check` | PASS |
| `python tools/check_normative_docs.py` | PASS — `normative docs: OK` |

## 6. OPEN-вопросы

- **OPEN-R2C-1 — закрыт близнецом 2026-09-03**, коммиты `4b38f7a` и
  `7648e72`: обязательные red-тесты и наблюдаемый save-audit counter добавил
  близнец; исполнитель тесты не менял.
- **OPEN-R2C-2 — закрыт близнецом 2026-09-03**, коммиты `14a2d7d` и
  `713fa66`: red-тест закрепил 54 errors / 20 warnings и пять точных кодов;
  Runtime diagnostic registry добавлен в закрытый список.
- **OPEN-R2C-3 — закрыт близнецом 2026-09-03**, коммит `c998ae9`: добавлен
  read-only `MHPeekProjectIndex()`, второй SQLite handle и `OpenProjectIndex`
  удалены; ручная проверка без Source Root подтвердила отсутствие нового
  `ProjectIndex.sqlite`.
- **OPEN-R2C-4 — закрыт близнецом 2026-09-03**, коммит `c998ae9`:
  `MHRuntimeBridgePreflight` снова проверяет точные транспортные байты и
  сравнивает обе подписи с proof-планом.

Открытых вопросов нет.

## 7. Изменённые файлы

Production и нормативная реализация (`e55a049`):

- `ue/MimirComposite/Source/MimirCompositeEditor/Private/Composite/MHProofCache.cpp`;
- `ue/MimirComposite/Source/MimirCompositeEditor/Public/Composite/MHProofCache.h`
  — только private implementation hooks/state;
- `ue/MimirComposite/Source/MimirCompositeEditor/Private/Composite/MHCompositeRuntimeBridge.cpp`;
- `ue/MimirComposite/Source/MimirCompositeEditor/Private/Composite/MHCompositeLevelSubsystem.cpp`;
- `ue/MimirComposite/Source/MimirCompositeEditor/Private/Composite/MHCompositePlacementEvents.cpp`;
- `ue/MimirComposite/Source/MimirCompositeRuntime/Private/Diagnostics/MHDiagnosticRegistry.cpp`;
- `docs/16_recipe_model.md` — только §2.6 и §9.

Исправления возврата (`2bb0b45`):

- `ue/MimirComposite/Source/MimirCompositeEditor/Public/Source/MHSourceComposition.h`
  — только accessor `MHPeekProjectIndex()`;
- `ue/MimirComposite/Source/MimirCompositeEditor/Private/Source/MHSourceComposition.cpp`
  — только реализация accessor;
- `ue/MimirComposite/Source/MimirCompositeEditor/Private/Composite/MHProofCache.cpp`;
- `ue/MimirComposite/Source/MimirCompositeEditor/Private/Composite/MHCompositeRuntimeBridge.cpp`.

Квитанция/статус:

- `docs/receipts/recipe_r2c.md`;
- `docs/RECIPE_EXECUTION_STATUS.md` — R2c → `IN REVIEW`.

Все файлы входят в закрытый список контракта. Тесты не менялись и не
удалялись; resolver, actor, compiled recipe, materialize, Blender addon,
Engine, `golden/`, `reference/` и форматы не тронуты.

## 8. Трекер и PR

Строка трекера: `R2c | IN REVIEW (внешний исполнитель)`. PR #88 открыт из
`recipe/r2c-exit-points` в `main`; merge выполняет только близнец после
независимой проверки.
