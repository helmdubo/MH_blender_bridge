# Квитанция: настройка подтверждения overwrite источника

Дата: 2026-08-30
Ветка: `feat/source-overwrite-confirmation-setting`
База: `69793b852be9ba849926e95100e373f99d7ef4c0`
Engine: stock UE 5.7.4, changelist `51494982`, без правок и без fork Engine
Scope: `ue/MimirComposite` и эта квитанция

## 1. Результат

В `UMHCompositeSettings` добавлена editor-config настройка:

```cpp
UPROPERTY(EditAnywhere, config, Category = "Mimir Composite|Source")
bool bConfirmSourceOverwrite = true;
```

Значение по умолчанию `true` сохраняет прежнее поведение команды
`Apply Edited Transforms to Source`: перед необратимой записью показывается
существующий `Yes/No` dialog; отказ не запускает publish.

При `false` modal API вообще не вызывается. Publish выполняется сразу, а после
успешной записи обязательно создаются два независимых следа с одинаковым
текстом:

```text
<file> overwritten from edited transforms
```

- fire-and-forget Slate notification с длительностью 5 секунд;
- строка `Info` на новой странице `Source overwrite` в `Mimir` Message Log.

Неуспешная запись не получает ложного success-аудита. Существующий итоговый
`NotifyOperation` команды и его error/warning routing не менялись.

## 2. Инвентарь publish-в-source UI

Поиск всех editor call sites дал одну точку modal-подтверждения именно
перезаписи source: `ExecuteCommitEditComposite` в `MHSourceToolMenus.cpp`.

- `Publish Material to MH Source` и `Publish Composite to MH Source` modal
  overwrite prompt не имели и не получили: default `true` не должен добавлять
  новый UX. После каждой операции они уже вызывают `NotifyOperation`, который
  пишет результат в `Mimir` Message Log и показывает non-modal notification.
- material/composite Adopt dialogs выбирают новый target для unmanaged asset и
  не являются подтверждением overwrite; настройка их не подавляет.
- delete `Yes/No/Cancel` отвечает за break/delete policy, а не за overwrite, и
  остаётся без изменений.

Policy вынесен в `MHExecuteSourceOverwrite`. Команда передаёт operation как
callback, поэтому `Cancelled` отличается от выполненной, но неуспешной попытки,
а audit появляется только при фактическом success.

## 3. Red-first

Коммит `beae6d0` (`Add source overwrite confirmation policy tests`) добавил
настройку, behavior-preserving seam «всегда спросить» и два Automation-теста.
Поддержка `false` в этом коммите намеренно отсутствовала.

RED:

- report:
  `E:\MimirComposite_S65_DefinitionPool_20260829\SourceOverwritePolicyRedReport\index.json`;
- log:
  `E:\MimirComposite_S65_DefinitionPool_20260829\source_overwrite_policy_red.log`;
- результат: **1/2 passed, 1 failed, 0 not run**.

Существенный вывод:

```text
disabled policy attempts overwrite without modal: values are not equal
Expected 'disabled policy never invokes confirmation UI' to be 0, but it was 1
Expected 'disabled policy runs one write' to be 1, but it was 0
Expected 'disabled policy writes the source file' to be true
Expected 'disabled policy emits one non-modal notification' to be 1, but it was 0
Expected 'disabled policy emits one Message Log line' to be 1, but it was 0
```

Коммит `ab231d8` (`Allow audited source overwrites without confirmation`)
реализовал настройку и двойной audit.

GREEN:

- report:
  `E:\MimirComposite_S65_DefinitionPool_20260829\SourceOverwritePolicyGreenReport\index.json`;
- log:
  `E:\MimirComposite_S65_DefinitionPool_20260829\source_overwrite_policy_green.log`;
- результат: **2/2 passed, 0 failed, 0 not run**.

`ConfirmEnabled` проверяет default `true`, вызов dialog seam, отмену без записи и
запись только после согласия. `ConfirmDisabled` записывает реальный временный
`.composite`, доказывает ноль обращений к modal seam, точный текст обоих audit
каналов и отсутствие success-аудита при ошибке callback.

## 4. Гейты

| Гейт | Результат |
|---|---|
| `Mimir.V4.SourceOverwritePolicy` | **2/2**, 0 failed, 0 not run |
| полный NullRHI `Mimir` с `-MHGoldenRoot=<repo>/golden` | **155/155**, 0 failed, 0 not run = база 153 + 2 новых |
| Guarded force-unity: `-NoEngineChanges -ForceUnity -DisableAdaptiveUnity -NoPCH -NoSharedPCH`, warnings-as-errors | **Succeeded**; production compile 7/7 actions, финальный exact target up to date |
| `BuildPlugin -StrictIncludes`, `-NoPCH -NoSharedPCH -DisableUnity` | **BUILD SUCCESSFUL**; Editor 103/103, Runtime Development 11/11, Runtime Shipping 11/11 |
| `git diff --check` | **PASS** |

Артефакты:

- `E:\MimirComposite_S65_DefinitionPool_20260829\SourceOverwritePolicyFullMimirReport\index.json`;
- `E:\MimirComposite_S65_DefinitionPool_20260829\source_overwrite_policy_full_mimir.log`;
- `E:\MimirComposite_S65_DefinitionPool_20260829\source_overwrite_green_build.log`;
- `E:\MimirComposite_S65_DefinitionPool_20260829\source_overwrite_policy_final_guarded_force_unity.log`;
- `E:\MimirComposite_S65_DefinitionPool_20260829\source_overwrite_policy_strictincludes.log`;
- StrictIncludes package:
  `E:\MimirComposite_SourceOverwrite_StrictIncludes_20260830_1`.

UAT создал шаблон `ue/MimirComposite/Config/FilterPlugin.ini`. Точный путь был
проверен; только этот generated-файл удалён и в коммиты не входит.

## 5. Frozen-инварианты

- Engine и runtime/cook-модуль не менялись.
- Форматы source, canonical bytes, планы, подписи и importer versions не
  менялись.
- Новых E/W-кодов нет; diagnostic registries не менялись.
- `golden` tree до/после:
  `71b30ebf65ca3cc8473f50305990c2bf2b332727`.
- `reference` tree до/после:
  `12e25b76b19aa824458221cf23f77236a17382cd`.
- UI policy не является новой source authority и не меняет publish admission.

## 6. Изменённые файлы

- `ue/MimirComposite/Source/MimirCompositeEditor/Public/Settings/MHCompositeSettings.h`;
- `ue/MimirComposite/Source/MimirCompositeEditor/Public/UI/MHSourceOverwritePolicy.h`;
- `ue/MimirComposite/Source/MimirCompositeEditor/Private/UI/MHSourceOverwritePolicy.cpp`;
- `ue/MimirComposite/Source/MimirCompositeEditor/Public/Composite/MHCompositeLevelSubsystem.h`;
- `ue/MimirComposite/Source/MimirCompositeEditor/Private/Composite/MHCompositeLevelSubsystem.cpp`;
- `ue/MimirComposite/Source/MimirCompositeEditor/Private/UI/MHSourceToolMenus.cpp`;
- `ue/MimirComposite/Source/MimirCompositeTests/Private/MHSourceOverwritePolicyTest.cpp`;
- `docs/receipts/source_overwrite_confirmation.md`.

## 7. Вопросы

Открытых архитектурных вопросов нет.
