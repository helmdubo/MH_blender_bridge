# R3a — пять значений интерфейса mesh endpoint и классификация дельты

## 1. База и границы

- Дата: 2026-09-05. Ветка: `codex/recipe-r3a-endpoint-interface-hashes`.
- Исходный red/API: `4fbb032`; исходная база среза: `c5a951b`.
- База реализации после `git fetch` и fast-forward: `5ea9a27`.
  Owner/близнец закрыл `OPEN-R3A-1` вариантом 1: пути материалов входят
  только в `MaterialBindingHash`. Секция F red-теста усилена самим близнецом.
- Реализация: `2c9f19d`. PR: [#102](https://github.com/helmdubo/MH_blender_bridge/pull/102).
- До первой правки прочитаны целиком контракт `docs/contracts/recipe_r3a.md`,
  `docs/16_recipe_model.md` §2.2 и §4. Публичный API и тесты относительно
  `5ea9a27` не изменены.
- Изменены только два файла реестра, эта квитанция и строка R3a в tracker.
  Новых helper-файлов и кодов диагностики нет. Исторические нормативные
  изменения до `5ea9a27` остаются в истории PR.
- `MHNotifyGeneratedResourceChanged`, актор, placement compiler, resolver,
  рецепты, proof и пулы не менялись: реимпорт по-прежнему перестраивает актор.
  R3b и `Loading`/асинхронная готовность R4 здесь не реализуются.

## 2. Реализованные входы

Потоки имеют собственный строковый domain. Строка кодируется как `uint32`
длины UTF-8 в байтах, затем UTF-8 без BOM и завершающего NUL. Целые числа
записываются little-endian с явной шириной; bool — один байт 0/1.
Хэш — `CityHash64`, результат 0 заменяется на 1. Адреса объектов, padding,
индексы FName и порядок обхода TMap в поток не попадают.

| Поле | Реализованный вход и порядок |
|---|---|
| `PayloadRevision` | Строка `UMHStaticMeshImportData::SourceHash` и `int32 ImporterVersion` сравниваются непосредственно с предыдущим Ready-снимком. Первая ревизия 0; любое отличие пары добавляет 1. |
| `BoundsRevision` / `Bounds` | Domain `mh.endpoint.bounds:1`, затем `GetExtendedBounds().Origin` XYZ, `BoxExtent` XYZ, `SphereRadius`, positive extension XYZ, negative extension XYZ: 13 IEEE-754 binary64, little-endian. Сравниваются точные байты, без хэширования, округления или допуска. Первая ревизия 0; отличие добавляет 1. `Bounds = Extended.GetBox()`; только при точно нулевых origin, extent и radius: Min уменьшается на negative extension, Max увеличивается на positive extension. |
| `BucketDescriptorHash` | Domain `mh.endpoint.bucket:1`; `uint32` числа слотов, имена `MaterialSlotName.ToString()` в порядке массива; `uint32 GetNumSourceModels()`. Если RenderData существует: `uint32` числа LODResources, для каждого LOD `uint32` числа секций, затем для каждой секции `uint32 MaterialIndex`, байты `bEnableCollision`, `bCastShadow`. При отсутствии RenderData этот хвост отсутствует. Путей default/overlay материалов нет. |
| `CollisionInterfaceHash` | Domain `mh.endpoint.collision:1`; байт наличия BodySetup. При наличии: байт CollisionTraceFlag, байт bDoubleSidedGeometry; девять `uint32` счётчиков AggGeom в порядке Sphere, Box, Sphyl, Convex, TaperedCapsule, LevelSet, SkinnedLevelSet, MLLevelSet, SkinnedTriangleMesh; строка `DefaultInstance.GetCollisionProfileName().ToString()`. |
| `MaterialBindingHash` | Domain `mh.endpoint.binding:1`; `uint32` числа слотов; для каждого слота в порядке массива: `MaterialSlotName.ToString()`, путь default MaterialInterface, путь OverlayMaterialInterface. Пути читаются через `TObjectPtr::GetPathName()` без разрешения unloaded handle; null кодируется строкой `None`. |

Все входы берутся из живого endpoint UStaticMesh и его embedded receipt.
Новых Asset Registry/tag-запросов, explicit `FinishCompilation` и загрузок
материалов или иных зависимостей не добавлено. Используются предписанные
контрактом mesh accessors; их внутренние Engine async-property guards
не являются доказательством решения будущей задачи R4 о готовности.

## 3. Жизненный цикл и дельта

Приватная value-only таблица `ReadyMeshInterfaces` хранит последнюю Ready
admission каждого mesh key, не удерживая UObject. `Invalidate` и
`InvalidateAll` очищают текущий prototype до Unresolved и увеличивают его
Revision, но сохраняют этот снимок. Неудачная admission также не стирает
последний Ready-снимок. В реестре нет отдельного удаления записи ключа;
при `Deinitialize` обе таблицы очищаются.

Первый Ready даёт только `bFirstAdmission`; следующие сравнивают payload,
bounds и три хэша независимо. Повторная admission без изменений возвращает
пустую дельту. Для non-mesh, Invalid, Unresolved, неизвестного ключа или
недействительного weak object дельта пустая. Поля интерфейса для non-mesh
и Invalid нулевые, Bounds невалиден.

## 4. Host и воспроизведение

Свежий host создан `tools/setup_s6_runtime_host.ps1`:
`E:/MimirComposite_R3a_20260905/MimirCompositeV5S6.uproject`.
`Plugins/MimirComposite` — junction к plugin этой ветки. Engine:
`D:/PersonalProjects/UE5/UE_5.7`, stock **5.7.4, CL 51494982**.
Все команды ниже выполнялись последовательно. Интерактивных шагов не было;
portfolio-проект, установленный plugin и Engine не менялись.

Non-unity RED/GREEN:

```text
Build.bat MimirCompositeV5S6Editor Win64 Development
 -Project=E:/MimirComposite_R3a_20260905/MimirCompositeV5S6.uproject
 -NoEngineChanges -NoHotReloadFromIDE -NoUBA -MaxParallelActions=2
 -DisableUnity -NoPCH -NoSharedPCH -WarningsAsErrors -WaitMutex
```

Automation — `UnrealEditor-Cmd.exe` с этим uproject и параметрами:

```text
-nullrhi -unattended -nop4 -nosplash -nosound -NoAssetRegistryCache
-MHGoldenRoot=E:/GITHUB/Mimirhead_UE5Exporter/MH_blender_bridge_r3a_executor/golden
"-ExecCmds=Automation RunTests <filter>"
"-TestExit=Automation Test Queue Empty"
-ReportExportPath=<host>/<Report> -abslog=<host>/<Log>
-stdout -FullStdOutLogOutput
```

Focused filter (RED и GREEN):

```text
Mimir.V5.Composite.Reconcile.PrototypeInterfaceHashes+Mimir.V5.Composite.Registry+Mimir.V5.Composite.Recipe+Mimir.V5.Composite.Perf+Mimir.V5.Composite.DefinitionPool
```

Full filter: `Mimir.`. Перед ним test-generated Content/MH и Saved/Mimir*
из focused-прогона в собственном host перемещены в `BeforeFullSuite/`.
Тесты сами переключают `mh.PerfTrace`; override через консоль не задавался.

## 5. RED → GREEN и гейты

Все пути логов и отчётов в таблице относительны к
`E:/MimirComposite_R3a_20260905/`.

| Гейт | Результат | Свидетельство |
|---|---|---|
| RED non-unity/no-PCH на `5ea9a27`, до правок | Succeeded; 134 actions, 721.64 s | `R3A_RED_BUILD.log` |
| RED focused | 23 Success (18 + 5 с предупреждениями), **1 Fail**, 0 NotRun | `RedReport/index.json`, `R3A_RED_TEST.log` |
| GREEN non-unity/no-PCH | Succeeded; 19 actions, 140.80 s | `R3A_GREEN_BUILD.log` |
| GREEN focused | **24/24 Success** (18 + 6 с предупреждениями), 0 Fail, 0 NotRun; 2.3807 s | `GreenReport/index.json`, `R3A_GREEN_TEST.log` |
| Полный NullRHI suite | **197 reported Success** (134 + 63 с предупреждениями), **0 Fail**, JSON NotRun=0; 36.54037 s | `FullReport/index.json`, `R3A_FULL_TEST.log`; три условных NOT RUN ниже |
| RecipeShadowParity / RecipeShadowParityApplied | Оба Success | Full log: 3839 / 3845 |
| Guarded force-unity, adaptive unity off | Succeeded; 14 actions, 152.35 s | `R3A_FORCE_UNITY_BUILD.log` |
| BuildPlugin StrictIncludes | BUILD SUCCESSFUL; Editor 726.66 s, Game Development 39.77 s, Shipping 34.49 s; UAT 13m 49s | `R3A_STRICT_BUILD.log:161,191,220,225` |
| `git diff --check` | PASS | нет whitespace errors |
| `python tools/check_normative_docs.py` | PASS | `normative docs: OK` |
| docs/16 §7.2, восьмая строка | PASS: `PlacementInterfaceHash` отсутствует в `ue/MimirComposite/Source` | `rg` возвращает 0 совпадений |

Единственный RED Fail — `Mimir.V5.Composite.Reconcile.PrototypeInterfaceHashes`.
Первая падающая assertion в `R3A_RED_TEST.log:1035`: `Ready mesh carries a
bucket descriptor hash: The two values are equal`, строка 137 frozen-теста.
Далее нулевые collision/binding, отсутствие first-admission и дельт.
Тот же тест, включая усиленную владельцем секцию F, зелёный после реализации.
Остальные 23 focused-теста были зелёными до и после правки.

Полный отчёт требует чтения журнала, а не только exit code/JSON:

- `Mimir.Audit.MainBaseline.LoadedPlacementClickSelectsActor` и
  `Mimir.Audit.MainBaseline.RenderedNativeHitProxy`: **RHI lane NOT RUN**
  (строки 983, 990); нужен `-MHPreviewRenderSmoke` без NullRHI.
- `Mimir.V5.Composite.ISM.CottageMetrics`: **NOT RUN** (строка 3456);
  `sovmod_cottage_i_cmp` не установлен в generic host.

Эти три условных lane не заявляются выполненными визуальными/полевыми
проверками. Контрактный generic NullRHI gate: 197 результатов, 0 Fail.

## 6. Перф-регресс

`Mimir.V5.Composite.Perf.EndpointCounters` зелёный в RED, GREEN и full.
Ниже точные счётчики из собственного before/after focused-прогона:

| Trace | RED | GREEN |
|---|---|---|
| Первый `MH_PERF_MAPLOAD` | log:864; registry_lookups=2, identity_admissions=2, package_loads_sync=0, asset_registry_tag_queries=0, live_receipt_tag_reads=0 | log:861; те же значения |
| Следующий тёплый `MH_PERF_MAPLOAD` | log:865; registry_lookups=0, identity_admissions=0, package_loads_sync=0, asset_registry_tag_queries=0, live_receipt_tag_reads=0 | log:862; те же значения |
| `MH_PERF_REIMPORT` | log:912; full_scan_count_delta=0, incremental_paths=1, notified_resource_keys=1, notified_actors=1 | log:909; те же значения |

В обоих mapload: all_option_unique_meshes=2, selected_unique_meshes=1,
selected_meshes_compiling=0, waited_meshes=0. Reimport продолжает перестройку
одного затронутого актора. Это регрессионная проверка счётчиков на fixture,
а не benchmark производительности портфолио или проверка compiling-mesh R4.

## 7. Упаковка и финальная проверка

Force-unity команда соответствует non-unity выше, но вместо `-DisableUnity`
использует `-ForceUnity -DisableAdaptiveUnity`.

```text
RunUAT.bat BuildPlugin
 -Plugin=E:/GITHUB/Mimirhead_UE5Exporter/MH_blender_bridge_r3a_executor/ue/MimirComposite/MimirComposite.uplugin
 -Package=E:/MimirComposite_R3a_Strict_20260905
 -TargetPlatforms=Win64 -StrictIncludes -NoDeleteHostProject
```

StrictIncludes запускается отдельным `run-strict.ps1` в host. Временная
конфигурация UBT ограничивает MaxParallelActions=2 и выключает UBA; исходные
байты сохраняются и восстанавливаются через finally. После завершения
`strict-config-restoration.json` подтверждает Restored=true, BuildExitCode=0;
исходный, восстановленный и текущий SHA-256 конфигурации совпадают:
`85382F0E3B2029D752E3CC42EF17F2C1727E1700D55D2451F050C474BA3772FB`.

`verify-package.ps1` сравнил SHA-256 всех **185** файлов Source с обоими
деревьями: сохранённым `HostProject/Plugins/MimirComposite/Source` и
`Source` итогового пакета. В каждом 185 файлов, отличий и пропусков нет.
Результат: `final-package-parity.json` в host. Сам пакет не установлен.

Публичная часть заголовка до `private:` точно совпадает с `5ea9a27`;
diff тестового модуля и `MHCompositePlacementEvents.cpp` относительно этой
базы пустой. После квитанции повторены normative docs и whitespace gates;
закрытый список исполнителя соблюдён, tracker меняет только строку R3a.

## 8. OPEN и передача

`OPEN-R3A-1` закрыт решением owner/близнеца `5ea9a27`, вариант 1.
Новых OPEN нет. Статус: **READY FOR REVIEW**, все контрактные автоматические
гейты пройдены. Merge в main выполняет близнец после независимого ревью.
