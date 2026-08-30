# Квитанция: параллельный bulk-импорт UE

Дата: 2026-08-30  
Ветка: `perf/bulk-import-async`  
База: `451bf2576eec092028dae4f9c9f9ea9e937822e4`  
Engine: stock UE 5.7, без правок и без fork Engine  
Scope реализации: `ue/MimirComposite`

## 1. Результат

`Import Changed` переведён на три прохода:

1. все texture/material/static-mesh/composite UObject создаются или обновляются;
   texture import tasks сначала ставятся в очередь одним вызовом с `bAsync=true`,
   затем читаются их результаты; per-asset compilation wait отсутствует;
2. один `FAssetCompilingManager::FinishAllCompilation()` завершает builds всего
   изменённого батча; static-mesh build errors, physics и `PostEditChange` завершаются
   после этой общей точки;
3. все успешно подготовленные пакеты сохраняются одним
   `UEditorLoadingAndSavingUtils::SavePackages`.

Project-index projection и placement notifications выполняются только после
успешного общего save. Перед save повторно сверяются raw hashes всех source
payloads, включая inlined placement profiles. При расхождении весь save
fail-closed отменяется; устаревший receipt не попадает на диск.

Один plugin-owned `FScopedSlowTask` создаётся на операцию и получает один tick
на каждый реально обработанный ресурс. Порядок стадий остался
`placement_profile -> texture -> material -> static_mesh -> composite`.

## 2. Инструментирование и red-first

Отдельный коммит `e159232` (`Instrument bulk source import stages`) добавил
несериализуемую матрицу метрик
`resource type x {Create, BuildWait, SavePackage}`, progress-счётчики и два
Automation-теста. Оптимизации в этом коммите не было.

Изолированная фикстура создаёт 70 уникальных PNG и 30 уникальных `.mesh.fbx`
из `golden/fixtures/axis/axis_probe.fbx`. Generated packages перед каждым
первым импортом отсутствуют; owner-проект и его сцены не используются.

Red-артефакт:
`E:\MimirComposite_S65_DefinitionPool_20260829\BulkImportRedReport3\index.json`.

Красный вывод acceptance:

```text
Expected 'one compilation wait for the whole batch' to be 1, but it was 100.
Expected 'one package save call for the whole batch' to be 1, but it was 170.
Expected 'one progress scope' to be 1, but it was 0.
Expected 'one progress tick per resource' to be 100, but it was 0.
Expected 'interruption before save leaves package bytes unchanged' to be true.
Expected 'unfinished resource is reimported on retry' to be true.
```

Итого red: **0/2 passed, 2 failed**, 0 not run. Первый импорт: **12.210 s**;
повторный NO_CHANGE: **0.734 s**.

## 3. Профиль до/после

Время — inclusive game-thread milliseconds; в скобках для `Create` указано
exclusive время. После оптимизации общие wait/save честно отнесены к `Batch`,
а не размножены по типам.

| Resource / stage | До: calls / ms | После: calls / ms |
|---|---:|---:|
| Texture Create | 70 / 7587.317 (6180.913 excl.) | 70 / 21.450 |
| Texture BuildWait | 70 / 2.909 | 0 / 0 |
| Texture SavePackage | 140 / 1403.495 | 0 / 0 |
| StaticMesh Create | 30 / 3916.357 (3577.067 excl.) | 30 / 400.353 |
| StaticMesh BuildWait | 30 / 0.028 | 0 / 0 |
| StaticMesh SavePackage | 30 / 339.262 | 0 / 0 |
| Material (в этой фикстуре) | 0 | 0 |
| Composite (в этой фикстуре) | 0 | 0 |
| Batch Create | 0 | 1 / 82.954 |
| Batch BuildWait | 0 | **1 / 0.046** |
| Batch SavePackage | 0 | **1 / 931.588** |

Финальный green-артефакт:
`E:\MimirComposite_S65_DefinitionPool_20260829\BulkImportGreenReport4\index.json`.

| Метрика | До | После | Итог |
|---|---:|---:|---:|
| Первый import, 100 ресурсов | 12.210 s | **2.256 s** | **18.48%** baseline, 5.41x быстрее |
| NO_CHANGE | 0.734 s | **0.707 s** | быстрый no-op сохранён |
| Compilation waits | 100 | **1** | PASS |
| Save calls | 170 | **1** | PASS |
| Progress scope / resource ticks | 0 / 0 | **1 / 100** | PASS |

Требование `<= 40%` выполнено с запасом: `2.256 / 12.210 = 18.48%`.

### Загрузка CPU

`UnrealEditor-Cmd` измерялся раз в секунду через прирост process CPU time,
нормализованный на 24 logical CPU. Измерение включает запуск/завершение
commandlet и оба Automation-теста, поэтому оно консервативнее чистого окна
первого импорта.

| Профиль | Средняя доля всей CPU-машины | Пик | Эквивалент logical cores, avg / peak |
|---|---:|---:|---:|
| До, 25 samples | 4.05% | 21.80% | 0.97 / 5.23 |
| После, 16 samples | **5.23%** | **22.50%** | **1.26 / 5.40** |

На миниатюрной warm-DDC фикстуре `BuildWait` не доминировал: до оптимизации
основные затраты были в последовательных texture tasks, mesh create и
170 сохранениях. Поэтому результат не выдаётся за замер cold-DDC GAZ53;
кэш/ускорение самого DDC в scope не добавлялись.

## 4. Acceptance red -> green

1. **70 textures + 30 meshes.** Green `Mimir.V4.BulkImport.LargeBatch`:
   100 scoped rows без resource errors, 2.256 s, один wait, один save.
2. **NO_CHANGE.** Второй проход 0.707 s, `bExecuted=false`, 0 waits,
   0 saves.
3. **Результаты и receipts.** Texture suffix policy не менялась;
   `MHStaticMeshImporterVersion` осталась **4**. Полный `Mimir` и его golden,
   receipt, placement/signature тесты прошли 143/143. Golden не
   перегенерировались.
4. **Crash между проходами.** Red сохранял изменённый package до injected
   границы и не переимпортировал его. Green оставляет package byte-identical
   последней завершённой версии; после имитации process restart следующий
   `Import Changed` повторно импортирует незавершённый ресурс.
5. **Dependencies.** Материалы по-прежнему идут после texture; meshes после
   materials; composites после meshes. Scoped lazy texture reimport материала
   также присоединяется к активному batch.
6. **Source race.** Перед единственным save повторно проверяются root payloads
   каждого ресурса и inlined profile payloads composite. Ошибка использует
   существующий `MH_E_SOURCE_INDEX_SNAPSHOT_CHANGED`; новых E/W-кодов нет.

Green bulk: **2/2 passed, 0 failed, 0 not run**.

## 5. Гейты

| Гейт | Результат |
|---|---|
| `Mimir.V4` | **51/51**, 0 failed |
| `Mimir.V4.BulkImport` | **2/2**, 0 failed |
| `Mimir.V5.Composite.Lifecycle` | **7/7**, 0 failed |
| полный NullRHI `Mimir` с `-MHGoldenRoot=<repo>/golden` | **143/143**, 0 failed, 0 not run = актуальная база main 141 + 2 новых |
| Guarded force-unity: `-NoEngineChanges -ForceUnity -DisableAdaptiveUnity -NoPCH -NoSharedPCH` | **Succeeded**, 16/16 actions, warnings-as-errors |
| `BuildPlugin -StrictIncludes`, `-NoPCH -NoSharedPCH -DisableUnity` | **BUILD SUCCESSFUL**; Editor 99/99, Runtime Development 11/11, Runtime Shipping 11/11 |
| `git diff --check` | **PASS** |

Артефакты:

- `E:\MimirComposite_S65_DefinitionPool_20260829\BulkImportGreenReport4\index.json`;
- `E:\MimirComposite_S65_DefinitionPool_20260829\BulkImportFullMimirReport2\index.json`;
- `E:\MimirComposite_S65_DefinitionPool_20260829\BulkImportLifecycleReport1\index.json`;
- `E:\MimirComposite_S65_DefinitionPool_20260829\bulk_import_guarded_force_unity.log`;
- `E:\MimirComposite_S65_DefinitionPool_20260829\bulk_import_strictincludes.log`;
- StrictIncludes package:
  `E:\MimirComposite_BulkImport_StrictIncludes_20260830_1`.

## 6. Frozen-инварианты

- Engine не изменялся; fork Engine не создавался; работа выполнена plugin-only.
- Runtime/cook-модуль не менялся.
- `golden` tree до/после:
  `71b30ebf65ca3cc8473f50305990c2bf2b332727`.
- `reference` tree до/после:
  `12e25b76b19aa824458221cf23f77236a17382cd`.
- Формат, canonical writers, планы, подписи и importer version не менялись.
- Новых E/W-кодов нет; diagnostic registries не менялись.
- Texture suffix settings не менялись.

## 7. Изменённые файлы

- `ue/MimirComposite/Source/MimirCompositeEditor/Public/Source/MHSourceImportMetrics.h`;
- `ue/MimirComposite/Source/MimirCompositeEditor/Private/Source/MHSourceImportMetrics.cpp`;
- `ue/MimirComposite/Source/MimirCompositeEditor/Public/Source/MHSourceImportBatch.h`;
- `ue/MimirComposite/Source/MimirCompositeEditor/Private/Source/MHSourceImportBatch.cpp`;
- `ue/MimirComposite/Source/MimirCompositeEditor/Public/Source/MHSourceImporter.h`;
- `ue/MimirComposite/Source/MimirCompositeEditor/Private/Source/MHSourceImporter.cpp`;
- `ue/MimirComposite/Source/MimirCompositeEditor/Public/Texture/MHTextureImporter.h`;
- `ue/MimirComposite/Source/MimirCompositeEditor/Private/Texture/MHTextureImporter.cpp`;
- `ue/MimirComposite/Source/MimirCompositeEditor/Private/Material/MHMaterialImporter.cpp`;
- `ue/MimirComposite/Source/MimirCompositeEditor/Private/StaticMesh/MHStaticMeshImporter.cpp`;
- `ue/MimirComposite/Source/MimirCompositeEditor/Private/Composite/MHCompositeImporter.cpp`;
- `ue/MimirComposite/Source/MimirCompositeTests/Private/MHSourceBulkImportTest.cpp`;
- `docs/receipts/import_perf.md`.

## 8. Коммиты

- `e159232` — `Instrument bulk source import stages`;
- `78ec416` — `Batch source builds and package saves`;
- квитанция — отдельный documentation commit.

## 9. Вопросы

1. **Контекст:** текущий importer для non-normal textures уже не задаёт
   `CompressionSettings`, то есть оставляет штатный `TC_Default`; normal-map
   policy отдельно фиксирует `TC_BC7`. Формулировка опционального
   `TextureCompressionFast` предлагает для non-normal `TC_Default/BC1`, но BC1
   выбирается texture compressor по содержимому/alpha и не является отдельным
   значением `TextureCompressionSettings` для прямого переключения.
   **Вопрос:** какое точное наблюдаемое изменение setting/format должен включать
   этот флаг относительно уже существующего `TC_Default`?
   **Временное fail-closed допущение:** опциональный флаг не добавлен; suffix
   policy и качество оставлены byte/behavior-identical текущему main до
   ратификации точного UE setting.
