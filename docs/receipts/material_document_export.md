# Квитанция: экспорт canonical `.material` из Content Browser

Статус: **READY FOR REVIEW**

Дата: 2026-09-01

База: `origin/main` `813dd70`

Ветка: `codex/feat-material-document-export`

## 1. Результат и границы

В контекстное меню `UMaterialInstanceConstant` в Content Browser, рядом с
`Publish Material to MH Source`, добавлена команда
`Export Material Document...`. Она создаёт внешнюю canonical-копию текущего
Material Instance, не публикует source authority и не меняет asset, receipt,
project index либо freshness.

Срез ограничен `ue/MimirComposite`. Не менялись runtime/cook, wire/schema,
enum, resolver, E/W-реестры, `golden/`, `reference/` и Blender-addon.

## 2. Реализация

### 2.1 Non-Slate ядро

`MHPrepareMaterialDocumentExport` выполняет полностью read-only preflight:

1. проверяет точный суффикс `.material` и destination;
2. блокирует весь batch, если хотя бы один destination находится внутри
   настроенного Source Root;
3. извлекает документ существующим `MHExtractMaterialV4`;
4. получает точные bytes существующим `MHWriteCanonicalMaterialV4`;
5. фиксирует canonical hash и его совпадение с `AppliedHash` managed receipt;
6. собирает единый отсортированный список существующих target-файлов;
7. плохой материал записывает в `Skipped`, не удаляя корректные peers из
   плана.

Имя файла для multi-export берётся из `UMHMaterialSourceData::LogicalName`;
при отсутствии/пустом receipt используется имя UE-ассета.

`MHCommitMaterialDocumentExport` ничего не пишет, если в плане есть коллизии
и пользователь не разрешил overwrite. При подтверждении каждый файл пишется
через sibling temporary file, canonical read-back и atomic replace. Файл,
появившийся после preflight и отсутствовавший в списке подтверждения,
fail-closed отклоняется как внешняя модификация.

### 2.2 Content Browser UX

- один выбранный материал открывает `SaveFileDialog` с предложенным
  `<logical-name>.material`; пользователь может выбрать новый путь либо
  существующий `.material`;
- несколько материалов открывают один directory dialog;
- перед multi/single overwrite показывается один диалог с полным списком
  коллизий; `No` означает ноль записей всего batch;
- ошибки извлечения отдельных материалов и ошибки записи выводятся с asset и
  destination в отдельную страницу Mimir Message Log;
- итоговая Slate notification показывает exported/skipped counts;
- несериализуемые overrides MI (имена вне грамматики, UE-only параметры,
  base/static overrides) не блокируют экспорт: документ содержит
  сериализуемую часть, отброшенное перечислено предупреждениями в Message Log
  (решение owner 2026-09-03; до этого — `MH_E_MATERIAL_NOT_ROUNDTRIPPABLE`);
- destination внутри Source Root **разрешён** (решение owner 2026-09-03:
  материал можно экспортировать в любой исходник материалов в любой
  ситуации); запись логируется как обычная внешняя правка источника, receipt не
  трогается — `Publish Material to MH Source` остаётся отдельным путём. До
  2026-09-03 такой destination отклонялся с `MH_E_INVALID_RESOURCE_SOURCE`.

## 3. Red-first

Коммит `171f72b` (`Add red-first material document export coverage`) добавил
публичный non-Slate seam, намеренно пустую реализацию и тест
`Mimir.V5.Material.DocumentExport`.

RED на отдельном HostProject, без управления открытым editor owner:

```text
Found 1 automation tests based on 'Mimir.V5.Material.DocumentExport'
Test Completed. Result={Fail} Name={DocumentExport}
Expected 'receipt logical name wins' to be "managed_leaf", but it was "ManagedAssetName".
Expected 'round-trip preflight succeeds' to be true.
round-trip plan missing ready item: RED: material document export is not implemented
Expected 'one overwrite is reported' to be 1, but it was 0.
Expected 'Source Root refusal points to Publish' to be true.
Expected 'one valid material remains ready' to be 1, but it was 0.
```

При RED collision-cancel тест дополнительно подтвердил, что старый файл не
изменился и fresh peer не появился.

Лог:
`E:\MimirComposite_MaterialDoc_Build_20260901\HostProject\Saved\Logs\MaterialDocumentExport_RED.log`.

Минимальная реализация и Content Browser wiring зафиксированы коммитом
`6a293f9` (`Export canonical material documents from Content Browser`).

GREEN:

```text
Found 1 automation tests based on 'Mimir.V5.Material.DocumentExport'
Test Completed. Result={Success} Name={DocumentExport}
EXIT=0
```

Тест закрепляет:

- receipt logical name и fallback asset name;
- byte-identical canonical output и равенство hash с `AppliedHash`;
- один collision-list, полную отмену без записей и подтверждённый overwrite;
- destination внутри Source Root: preflight и commit проходят, документ
  записан canonical-байтами (до 2026-09-03 — полный запрет);
- skip неэкспортируемого материала при успешной записи корректного peer.

Лог:
`E:\MimirComposite_MaterialDoc_Build_20260901\HostProject\Saved\Logs\MaterialDocumentExport_GREEN.log`.

## 4. Гейты

### Automation

Полный NullRHI с
`-MHGoldenRoot=E:\GITHUB\Mimirhead_UE5Exporter\MH_blender_bridge\golden`:

```text
Found 172 automation tests based on 'Mimir'
172 passed / 0 failed
EXIT=0
```

Это база `171/171` на merged `813dd70` плюс новый `DocumentExport`. Внутренние
optional RHI/field lanes сохранили штатный статус success-with-warnings;
ни один Automation test не завершился `Fail`.

Лог:
`E:\MimirComposite_MaterialDoc_Build_20260901\HostProject\Saved\Logs\MaterialDocumentExport_FULL.log`.

### Build

- guarded force-unity:
  `-NoEngineChanges -ForceUnity -DisableAdaptiveUnity -NoPCH -NoSharedPCH -WarningsAsErrors`
  — **Succeeded**, 17/17 actions, `207.09 s`;
- `BuildPlugin -StrictIncludes -DisableUnity -NoPCH -NoSharedPCH`, Win64 —
  **BUILD SUCCESSFUL**, `19m 52s`: Editor Development `115/115`
  (`1063.66 s`), Runtime Development `11/11` (`63.66 s`), Runtime Shipping
  `11/11` (`58.32 s`);
- `git diff --check` — PASS.

Все сборки выполнялись против stock UE 5.7.4. Engine не менялся. Открытый UE
owner не закрывался и не использовался: build/test шли через отдельный
`E:\MimirComposite_MaterialDoc_Build_20260901\HostProject`.

## 5. Полевой протокол UI

1. В Content Browser выбрать один managed Material Instance, открыть
   `Asset Actions -> Export Material Document...`, сохранить вне Source Root.
   Проверить предложенное logical name и canonical `.material`.
2. Повторить в тот же файл: появляется один overwrite prompt; `No` сохраняет
   прежние bytes, `Yes` заменяет их.
3. Выбрать несколько managed/unmanaged MI, выбрать папку. Проверить имена из
   receipt/fallback, один общий список коллизий и exported/skipped в Mimir Log.
4. Добавить в selection MI с неподдерживаемым parent/override: он получает
   строку с причиной в Message Log, остальные файлы создаются.
5. Выбрать destination внутри Source Root: документ записывается как обычный
   экспорт; индекс источника подхватывает файл как внешнюю правку.

Commandlet-гейты покрывают ядро; native dialogs и визуальное расположение
пункта меню требуют этого короткого owner field test.

## 6. Изменённые файлы

- `ue/MimirComposite/Source/MimirCompositeEditor/Public/Material/MHMaterialDocumentExport.h`
- `ue/MimirComposite/Source/MimirCompositeEditor/Private/Material/MHMaterialDocumentExport.cpp`
- `ue/MimirComposite/Source/MimirCompositeEditor/Private/UI/MHSourceToolMenus.cpp`
- `ue/MimirComposite/Source/MimirCompositeEditor/MimirCompositeEditor.Build.cs`
- `ue/MimirComposite/Source/MimirCompositeTests/Private/MHMaterialDocumentExportTest.cpp`
- `docs/receipts/material_document_export.md`

## 7. Вопросы

Открытых архитектурных вопросов нет. С 2026-09-03 export разрешён и внутри
Source Root по решению owner; publish-путь (receipt, index) им не подменяется —
экспортированный документ живёт как внешняя правка источника.
