# Квитанция: точечный force-reimport managed static mesh

Дата: 2026-08-30
Ветка: `feat/targeted-force-reimport`
База: `54b6f6a205aead70ac6b9f746b15be184947295e`
Engine: stock UE 5.7.4, changelist `51494982`, без правок и без fork Engine
Scope реализации: `ue/MimirComposite`

## 1. Результат

Editor-модуль регистрирует plugin-owned `FReimportHandler` на своём
`StartupModule` и снимает регистрацию в `ShutdownModule`. Handler принимает только
canonical `UStaticMesh` из `/Game/MH/Generated/Meshes`, у которого установлен
`UMHStaticMeshImportData` с canonical непустым `LogicalName` и непустым
`SourceRelativePath`, а `Source Root` настроен. Чужой mesh и копия managed receipt
на неканоническом объекте не перехватываются.

Источник вычисляется только как
`<SourceRoot>/<receipt.SourceRelativePath>`. Путь нормализуется и обязан остаться
внутри root. Receipt provenance не может быть заменён через стандартный UE file
picker. Отсутствующий файл отклоняется до создания import services и до мутации
ассета; ошибка содержит полный ожидаемый путь.

Успешный вызов идёт через существующий
`UMHSourceImporter::ReimportStaticMesh` и
`MHImportStaticMeshV4(..., bForceReimport=true)`. `force` пропускает только
`NO_CHANGE`: обычные resolver/admission, FBX/LOD/material/collision/decal checks и
штатная запись receipt сохранены. Для самостоятельного вызова создаётся один
`FMHSourceImportBatchContext`, после чего ровно по одному разу выполняются
`FinishCompilation`, `SavePackages` и `CommitProjectionAndNotifications`.
Последняя стадия использует существующую воронку
`MHNotifyGeneratedResourceChanged`; новой воронки инвалидации нет.

## 2. Red-first

Коммит `c304d2a` (`Add managed mesh reimport acceptance tests`) зарегистрировал
handler с намеренно не реализованным `Reimport` и добавил четыре теста. Коммит
`9b3af63` (`Cover placed composites in managed mesh reimport tests`) до production
усилил fixture уже размещённым `AMHCompositeActor`. Production-реализация
появилась только следующим коммитом `23a3ba6`
(`Force reimport managed meshes through source batches`).

Red-артефакты:

- `E:\MimirComposite_S65_DefinitionPool_20260829\TargetedReimportE2ERedReport\index.json`;
- `E:\MimirComposite_S65_DefinitionPool_20260829\targeted_reimport_e2e_red.log`.

Red: **1/4 passed, 3 failed, 0 not run**. Существенный вывод:

```text
Managed static-mesh force-reimport is not implemented
Expected 'changed Reimport advances receipt hash' ... old hash remained
Expected 'replacement geometry has two polygons' to be 2, but it was 1
Expected 'single force-reimport has one batch compilation wait' to be 1, but it was 0
Expected 'single force-reimport has one batch save' to be 1, but it was 0
Expected 'changed Reimport emits one placement notification' to be 1, but it was 0
changed mesh reimport rebuilds the existing placed composite: The two values are not equal
unchanged force also refreshes the existing placed composite: The two values are not equal
Expected 'missing-source error reports exact path' to be true
Expected 'two sequential single batches perform two waits' to be 2, but it was 0
Expected 'two sequential single batches perform two saves' to be 2, but it was 0
Expected 'two successful resources notify twice' to be 2, but it was 0
```

Admission уже был зелёным в red-коммите: регистрация и граница владения handler
были проверены независимо от реализации reimport.

Green-артефакты:

- `E:\MimirComposite_S65_DefinitionPool_20260829\TargetedReimportFinalGreenReport\index.json`;
- `E:\MimirComposite_S65_DefinitionPool_20260829\targeted_reimport_final_green.log`.

Green: **4/4 passed, 0 failed, 0 not run**.

## 3. Acceptance

### 3.1 Managed mesh и обновление размещений

`TargetedReimport.ForceAndNotify` вызывает не handler напрямую, а стандартный
`FReimportManager::Reimport`, то есть тот же маршрут, который обслуживает
`Asset Actions -> Reimport`. Изменённый FBX дал новый raw hash, заменил
однополигональную геометрию на двухполигональную и выполнил ровно один batch
wait, один batch save и одно уведомление с точным resource key. Созданный до
reimport размещённый `AMHCompositeActor` получил ровно один дополнительный
rebuild, сохранил static-mesh leaf и увидел пересобранный mesh без ручного вызова.

Существующие тесты `DefinitionPool.TargetedReimportInvalidatesOnce` и
`EndpointReimportInvalidatesEntry` дополнительно подтверждают: notify инвалидирует
только зависимый definition, пересобирает только зависимые placements и следующий
resolve берёт новый endpoint.

### 3.2 Force при неизменённом source

Тот же тест повторно портит локальное состояние mesh, не меняя source FBX, и
снова вызывает стандартный reimport. Вызов успешно пересобирает mesh, восстанавливает
source-authoritative состояние, выполняет ещё по одному wait/save/notify, а
геометрия и receipt hash совпадают с предыдущим импортом того же source. Тот же
уже размещённый actor получает второй и только второй дополнительный rebuild.

### 3.3 Чужие mesh

`TargetedReimport.Admission` проверяет:

- managed canonical mesh: `CanReimport=true`, возвращён точный receipt-derived
  абсолютный путь;
- `UStaticMesh` без `UMHStaticMeshImportData`: `false`;
- неканонический объект с копией managed receipt: `false`.

Таким образом native UE reimport чужих static mesh не перехватывается.

### 3.4 Отсутствующий source

`TargetedReimport.MissingSourceDoesNotMutate` переносит FBX после исходного
успешного импорта и вызывает production source-importer. Получен отказ с полным
ожидаемым путём. До/после совпали:

- байты сохранённого `.uasset`;
- live geometry;
- receipt raw hash.

### 3.5 Мультивыделение

Выбран разрешённый контрактом вариант: **N последовательных завершённых
single-batch**. `FReimportManager::ReimportMultiple` вызывает handler отдельно для
каждого UObject и предоставляет только `void PostImportCleanUp()`. Перенос save в
cleanup лишил бы конкретный элемент честного результата `Succeeded/Failed`, если
общий compilation/save упадёт. Поэтому каждый ресурс завершает собственные
wait/save/notify до возврата результата.

Автотест на двух реальных managed meshes через `ReimportMultiple`:

```text
TARGETED_REIMPORT_MULTI resources=2 wall_ms=440.880400 policy=sequential_single_batches
```

Фактически: **2 waits, 2 saves, 2 notifications**, оба результата успешны.
Цена 440,88 мс для двух ресурсов принята как точечный UX; общая bulk-операция
остаётся за `Import Changed` и его shared batch.

## 4. Гейты

| Гейт | Результат |
|---|---|
| `Mimir.V4.StaticMesh.TargetedReimport` | **4/4**, 0 failed, 0 not run |
| `Mimir.V5.Composite.Lifecycle` | **7/7**, 0 failed |
| `Mimir.V5.Composite.DefinitionPool` | **8/8**, 0 failed |
| полный NullRHI `Mimir` с `-MHGoldenRoot=<repo>/golden` | **147/147**, 0 failed, 0 not run = база 143 + 4 новых |
| Guarded force-unity: `-NoEngineChanges -ForceUnity -DisableAdaptiveUnity -NoPCH -NoSharedPCH`, UHT warnings-as-errors | **Succeeded**; production compile 4/4 actions, финальный exact target up to date |
| `BuildPlugin -StrictIncludes`, `-NoPCH -NoSharedPCH -DisableUnity` | **BUILD SUCCESSFUL**; Editor 100/100, Runtime Development 11/11, Runtime Shipping 11/11 |
| `git diff --check` | **PASS** |

Артефакты:

- `E:\MimirComposite_S65_DefinitionPool_20260829\TargetedReimportFinalLifecycleReport\index.json`;
- `E:\MimirComposite_S65_DefinitionPool_20260829\TargetedReimportFinalDefinitionPoolReport\index.json`;
- `E:\MimirComposite_S65_DefinitionPool_20260829\TargetedReimportFinalFullMimirReport\index.json`;
- `E:\MimirComposite_S65_DefinitionPool_20260829\targeted_reimport_final_guarded_force_unity.log`;
- `E:\MimirComposite_S65_DefinitionPool_20260829\targeted_reimport_final_strictincludes.log`;
- StrictIncludes package:
  `E:\MimirComposite_TargetedReimport_StrictIncludes_20260830_2`.

UAT создал шаблон `ue/MimirComposite/Config/FilterPlugin.ini`. Точный путь и
содержимое были проверены; только этот generated-файл удалён, в коммиты он не
вошёл.

## 5. Frozen-инварианты

- Engine не изменялся; fork Engine не создавался.
- Runtime/cook-модуль не менялся.
- `golden` tree до/после:
  `71b30ebf65ca3cc8473f50305990c2bf2b332727`.
- `reference` tree до/после:
  `12e25b76b19aa824458221cf23f77236a17382cd`.
- Формат, планы, подписи, canonical writers и importer version 4 не менялись.
- Новых E/W-кодов нет; diagnostic registries не менялись.
- Texture/material/composite importer и их reimport UX не менялись.
- Test-only observer встроен непосредственно в существующую notify-воронку под
  `WITH_DEV_AUTOMATION_TESTS`; production routing и порядок уведомлений не меняет.

## 6. Изменённые файлы

- `ue/MimirComposite/Source/MimirCompositeEditor/Public/StaticMesh/MHStaticMeshReimportHandler.h`;
- `ue/MimirComposite/Source/MimirCompositeEditor/Private/StaticMesh/MHStaticMeshReimportHandler.cpp`;
- `ue/MimirComposite/Source/MimirCompositeEditor/Private/MimirCompositeEditorModule.cpp`;
- `ue/MimirComposite/Source/MimirCompositeEditor/Private/Source/MHSourceImporter.cpp`;
- `ue/MimirComposite/Source/MimirCompositeEditor/Public/Composite/MHCompositePlacementEvents.h`;
- `ue/MimirComposite/Source/MimirCompositeEditor/Private/Composite/MHCompositePlacementEvents.cpp`;
- `ue/MimirComposite/Source/MimirCompositeTests/Private/MHStaticMeshImporterTest.cpp`;
- `docs/receipts/targeted_reimport.md`.

## 7. Вопросы owner/Lead

1. **Контекст:** texture importer имеет свой `bForceReimport`, но стандартный
   texture `FReimportHandler` потребует отдельной admission-границы, receipt-path
   проверки и red-first матрицы; это не бесплатное переиспользование mesh handler.
   **Вопрос:** нужен ли отдельный контракт на targeted texture reimport?
   **Временное fail-closed допущение:** texture/material/composite handler не
   добавлен и текущий native UE UX не перехватывается.
