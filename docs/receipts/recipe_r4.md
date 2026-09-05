# R4 — async-загрузка выбранных endpoint'ов, заглушки, снятие остатка ожидания R1

Статус: **MERGED** (близнец; вся реализация среза выполнена близнецом на его
generic host `E:/MimirComposite_R_M0_20260902`, 2026-09-05, executor в этот
срез не привлекался).

## 1. Red-коммит и что доказано

Red-коммит `c1d9c8a` (API contract, без реализации): дефолт
`UMHCompositeSettings::PlaceholderMesh` = `/Engine/BasicShapes/Cube`;
`UMHEndpointPrototypeRegistry::ResolveMeshForPreview` / `HasPrototype` /
`FlushAsyncLoadsForTests` добавлены как stubs, сохраняющие сегодняшний
синхронный путь; новый тест
`Mimir.V5.Composite.Async.ColdEndpointLoadsWithoutSyncLoad`; ожидание R1 в
`Mimir.V5.Composite.Perf.SelectedMeshWait` переписано на контракт R4
(`waited_mesh_set` пуст, `WaitStaticMeshCompilationMs == 0`).

`R4_RED_TEST.log` воспроизводит ровно KICKOFF §5 R4: `ColdEndpointLoadsWithoutSyncLoad`
падает на «cold endpoint performs no synchronous package load» (было 1),
«selected cold endpoint is Loading» и «first frame renders the placeholder
mesh» — то есть до реализации холодный endpoint грузился синхронно, минуя
async-путь и заглушку. `SelectedMeshWait` на red-сборке уже проходил: меши
фикстуры не компилировались в момент проверки, так что старое и новое
условие («ожидал только выбранные компилирующиеся» vs «ожидания нет вовсе»)
не расходились на этом прогоне; это не доказательство контракта R4 —
доказательство даёт только зелёный `ColdEndpointLoadsWithoutSyncLoad`.

## 2. Реализация

Файлы: `MHEndpointPrototypeRegistry.{h,cpp}`,
`MHCompositePlacementCompiler.cpp` (`MimirCompositeEditor`).

- **`Admit`**: для `static_mesh`-ключа, чей объект не резидентен, но
  канонический пакет существует на диске (`FPackageName::DoesPackageExist`),
  прототип получает `State = Loading` и стартует
  `UAssetManager::GetStreamableManager().RequestAsyncLoad(...)` на
  `FSoftObjectPath(Path)` с callback'ом `OnAsyncLoadComplete(Key)`;
  дескриптор хранится в `TMap<FMHResourceKey, TSharedPtr<FStreamableHandle>>
  PendingLoads`. Если streamable manager отказал (`Handle` невалиден) —
  единственный запасной путь: синхронный `LoadObject`, засчитанный в
  `MHRecordEndpointPackageLoadSync()`, прототип помечается `Invalid` (не
  `Loading`) для этого отказа. Отсутствующий на диске пакет — прежний путь,
  без изменений.
- **`Resolve`**: ключ с активной записью в `PendingLoads` возвращается как
  есть (`Loading` не в счёт хита/переадмиссии); остальные состояния
  переадмитятся как раньше.
- **`ResolveMeshForPreview`**: `Ready` → сам объект; `Loading` → грузит и
  возвращает `Settings.PlaceholderMesh`, `bOutPlaceholder = true` (заглушка
  никогда не проходит по счётчикам endpoint'а — `LoadSynchronous()` заглушки
  не вызывает `MHRecordEndpointPackageLoadSync`); иначе — `AdmissionError`.
- **`OnAsyncLoadComplete`**: снимает ключ из `PendingLoads` и вызывает только
  `MHNotifyGeneratedResourceChanged(Key)` — саму первую `Ready`-admission
  выполняет протокол реимпорта R3b (`Invalidate → Resolve`), см. §6.
- **`FlushAsyncLoadsForTests`**: ждёт каждый незавершённый handle
  (`WaitUntilComplete`), при необходимости завершает admission вручную,
  возвращает `false`, если после этого какой-то ключ остался `Loading`.
- **Compiler**: `PlanViewWaitSelectedMeshes` — тело пустое
  (`static_cast<void>`), `FStaticMeshCompilingManager::FinishCompilation`
  больше не вызывается в интерактивном пути; движок сам подменяет render
  data по готовности компиляции. Новый `PlanViewResolveLeafMesh(Resource,
  Settings, OutError)` заменяет пять инлайновых мест
  `UMHEndpointPrototypeRegistry::ResolveEndpoint(Key, Error)` для
  mesh-листьев, делегируя в `ResolveMeshForPreview`.
- Невыбранные варианты по-прежнему не резолвятся (обход рецепта их не
  трогает; тест это отдельно проверяет через `HasPrototype`).

## 3. Тесты

| Тест | Что проверяет |
|---|---|
| `Mimir.V5.Composite.Async.ColdEndpointLoadsWithoutSyncLoad` | Первый кадр: без ошибки, план резидентен, `package_loads_sync == 0`, `WaitStaticMeshCompilation.Calls == 0`, выбранный холодный endpoint — `Loading`, бакет рендерит заглушку по пути (не по указателю — движок content не rooted), невыбранный вариант не резолвится (`HasPrototype == false`). После `FlushAsyncLoadsForTests`: endpoint — `Ready`, `package_loads_sync` всё ещё 0, бакет рендерит реальный меш, ошибок нет, невыбранный вариант остаётся невыгруженным (`FindObject` == null) |
| `Mimir.V5.Composite.Perf.SelectedMeshWait` (переписан R4) | `waited_mesh_set` пуст, `wait_static_mesh_compilation_ms == 0`; отчёт по-прежнему называет выбранные компилирующиеся меши, но интерактивный путь не ждёт ни одного |

## 4. Гейты

| Gate | Результат |
|---|---|
| non-unity/no-PCH build | `R4_GREEN_BUILD5.log`: Succeeded |
| Полный NullRHI suite «Mimir» | `R4_GREEN_FULL.log`: Success=205, Fail=0 (205 = 204 в `main` после R4-pre-3 + 1 новый; база R3b-проверки 202 не включала два теста R4-pre-3) |
| `RunUAT BuildPlugin -StrictIncludes` | `R4_STRICT.log`: ExitCode=0 |
| force-unity | `R4_FORCE_UNITY.log`: Succeeded |
| `git diff --check` | чисто |
| `tools/check_normative_docs.py` | OK |

## 5. Изменённые файлы

`ue/MimirComposite/Source/MimirCompositeEditor/Private/Composite/MHEndpointPrototypeRegistry.cpp`,
`ue/MimirComposite/Source/MimirCompositeEditor/Public/Composite/MHEndpointPrototypeRegistry.h`,
`ue/MimirComposite/Source/MimirCompositeEditor/Private/Composite/MHCompositePlacementCompiler.cpp`
(реализация); `ue/MimirComposite/Source/MimirCompositeTests/Private/MHAsyncEndpointTest.cpp`
(новый в red-коммите `c1d9c8a`, доработан над красным: сравнение заглушки по
пути вместо указателя, `FirstBucket` обобщён с `UInstancedStaticMeshComponent`
на `UStaticMeshComponent`), `ue/MimirComposite/Source/MimirCompositeTests/Private/MHStaticMeshImporterTest.cpp`
(правка `SelectedMeshWait` — часть red-коммита); API-контракт
(`UMHCompositeSettings::PlaceholderMesh`, три метода реестра) — из red-коммита,
реализацией не менялся.

## 6. Уроки

Порядок admission vs уведомление реимпорта важен: ранний вариант в
`OnAsyncLoadComplete` сначала выполнял admission (`Resolve`/`Admit` до
`Ready`), затем уведомлял — дельта реконсиляции (R3b) сравнивала уже
одинаковые до/после снимки и получала пустую дельту, поэтому `RebuildComposite`
не вызывался и заглушка не заменялась реальным мешом. Исправление: завершение
async-загрузки **только** уведомляет
(`MHNotifyGeneratedResourceChanged`), а саму первую `Ready`-admission
(`Invalidate → Resolve`, дельта с `bFirstAdmission`) выполняет протокол R3b —
тем самым единственный владелец перехода placeholder → real mesh остаётся один.

Фикстура: холодные меши — `DuplicateObject` копии стокового куба, сохранённые
в канонический пакет, затем освобождённые (`ClearFlags` + `MarkAsGarbage`) и
собранные `CollectGarbage`; пустой `UStaticMesh` без Source Models на
перезагрузке даёт ошибку движка "no Source Models", поэтому фикстура кладёт
настоящую геометрию. Контент движка (`/Engine/...`) не rooted — заглушка после
принудительного GC может быть выгружена и перезагружена под другим
указателем, поэтому тест сравнивает её по пути (`GetPathName`), а не по
указателю.

## 7. Вопросы

Открытых нет. Вне среза (зафиксировано как факт, не решается здесь):
заглушка — обычный engine-куб без material hint; `Loading`-прототип не даёт
Break собрать дочерний актор (существующая диагностика «unavailable» до
завершения загрузки); точки выхода proof/runtime сохраняют синхронные
загрузки по контракту (§2.6) — им разрешено блокироваться. Полный NullRHI
suite вернул 205: 204 теста `main` после R4-pre-3 (`CallContextReproducesSubtree`, `Break.NestedCompositeReproducesResult`) плюс новый Async-тест; число 202 относилось к ветке R3b, отведённой до R4-pre-3.

## 8. Проверка при написании квитанции

```
python tools/check_normative_docs.py  → normative docs: OK
git diff --check                       → чисто (только предупреждения о LF→CRLF, не ошибки)
```
