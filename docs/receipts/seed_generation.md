# Квитанция — seed-кнопки в Details

Дата: 2026-08-30

Статус: **READY FOR OWNER FIELD TEST — автоматические гейты зелёные; интерактивный Details-протокол не заявлен как выполненный**

Ветка: `feat/seed-generation-ux`

База: `3f3258c4f4a18473508d41fb24489d5c814cf9f7` (`main`, новее требуемого `8399be0`)

Кодовый коммит: `e70659e` (`Add composite seed actions to actor Details`)

## 1. Ратифицированная граница

Последнее решение owner заменяет часть контракта о даговском диапазоне:
в этом срезе реализованы **только кнопки Details**. Поэтому:

- `AMHCompositeActor::GenerateAutoSeed` не менялся;
- `Seed`, `AppearanceSeed`, их `UPROPERTY` и сериализация не менялись;
- диапазон, nonzero-семантика и все существующие потребители генератора
  остались ровно прежними;
- отрицательные и нулевые авторские значения не мигрируют и не клампятся.

Работа выполнена только в `ue/MimirComposite` и этой квитанции. Engine,
runtime/cook, resolver, lifecycle, format/plan/signatures, `golden/` и
`reference/` не менялись.

Tree object до/после:

- `golden`: `71b30ebf65ca3cc8473f50305990c2bf2b332727`;
- `reference`: `12e25b76b19aa824458221cf23f77236a17382cd`.

## 2. Реализация

Для `AMHCompositeActor` зарегистрирован editor-only
`IDetailCustomization`. Две уже существующие строки категории
`Mimir|Random` дополнены кнопкой `Generate`:

- `Layout Seed` вызывает штатный `AMHCompositeActor::Reseed()`;
- `Appearance Seed` вызывает штатный
  `AMHCompositeActor::ReseedAppearance()`.

Customization получает полный список объектов текущего Details view.
Одно нажатие сначала fail-closed проверяет весь snapshot, затем создаёт
одну `FScopedTransaction` и применяет действие ко всем выбранным
`AMHCompositeActor`. Это один Undo-шаг для мультивыделения. После действия
Details запрашивает refresh, editor viewport — redraw.

Кнопка выключена, если snapshot пуст/протух или хотя бы один actor находится
в Placement Edit Mode. Проверка повторяется непосредственно перед
транзакцией: частичного reseed при рассинхроне selection не возникает.

`AMHRuntimeCompositeActor` не регистрировался и не менялся.

## 3. Red-first

Добавлен тест
`Mimir.V5.Composite.Seed.DetailsActions`: два actor, отдельные layout и
appearance действия, проверка изменения обоих значений и восстановления
обоих одним Undo.

### RED

Собрана намеренная runtime-заглушка общего Details action. Результат:

- отчёт:
  `E:\MimirComposite_S65_DefinitionPool_20260829\SeedUxRedReport\index.json`;
- **0/1 passed, 1 failed, 0 not run**;
- `Layout Generate executes for the edited selection` — false;
- layout seeds обоих actor не изменились;
- `one Undo reverses the multi-select layout action` — false;
- `Appearance Generate executes for the edited selection` — false;
- appearance seeds обоих actor не изменились;
- `one Undo reverses the multi-select appearance action` — false.

Это runtime RED Automation-теста, а не ошибка линковки/компиляции.

### GREEN

После транзакционной реализации:

- отчёт:
  `E:\MimirComposite_S65_DefinitionPool_20260829\SeedUxGreenReport\index.json`;
- **1/1 passed, 0 failed, 0 not run**;
- финальный exact-tree отчёт:
  `E:\MimirComposite_S65_DefinitionPool_20260829\SeedUxDetailsFinalReport\index.json`;
- **1/1 passed, 0 failed, 0 not run**.

Первая green-сборка также выявила пропущенный прямой include
`DetailWidgetRow.h`; после его добавления отдельный translation unit и оба
unity-режима собираются с warnings-as-errors.

## 4. Ручной протокол Details

Статус: **NOT RUN — требуется owner field test**. Исполнитель не открывал
GUI и не перехватывал управление ПК; автоматический тест подтверждает
команду/транзакцию, но не подменяет визуальную проверку расположения Slate
controls.

Протокол для owner:

1. Открыть уровень с размещённым `AMHCompositeActor`, выделить actor.
2. В `Details → Mimir|Random` проверить кнопку `Generate` в строке
   `Layout Seed` и отдельную кнопку в строке `Appearance Seed`.
3. Записать оба значения. Нажать layout `Generate`: layout должен
   измениться, appearance — остаться прежним. Один Ctrl+Z должен вернуть
   layout к записанному значению.
4. Нажать appearance `Generate`: appearance должен измениться, layout —
   остаться прежним. Один Ctrl+Z должен вернуть appearance.
5. Выделить два размещённых composite actor. Нажать каждую кнопку по
   отдельности: соответствующий seed должен измениться у обоих; один Ctrl+Z
   должен восстановить оба прежних значения.
6. На actor в Placement Edit Mode кнопки должны быть выключены.
7. Выделить `AMHRuntimeCompositeActor`: MH seed-кнопок этого customization
   быть не должно.

После фактического прогона в этот раздел нужно дописать actor/map и числа
до/после. До этого статус квитанции намеренно не повышается до полного
`READY`.

## 5. Гейты

### База до правок

На точном `main` `3f3258c`:

- NullRHI `Mimir` с `-MHGoldenRoot=<repo>/golden`:
  **159/159 passed, 0 failed, 0 not run**;
- отчёт:
  `E:\MimirComposite_S65_DefinitionPool_20260829\SeedUxBaselineReport\index.json`.

### Финальный exact tree

| Гейт | Результат |
|---|---:|
| `Mimir.V5.Composite.Seed.DetailsActions` | **1/1**, 0 failed, 0 not run |
| `Mimir.V5.Composite.Lifecycle` | **7/7**, 0 failed, 0 not run |
| `Mimir.V5.Composite.DefinitionPool` | **8/8**, 0 failed, 0 not run |
| `Mimir.V5.Composite.IncrementalReseed` | **6/6**, 0 failed, 0 not run |
| полный NullRHI `Mimir` + `-MHGoldenRoot` | **160/160**, 0 failed, 0 not run |

Полный отчёт:
`E:\MimirComposite_S65_DefinitionPool_20260829\SeedUxFullMimirReport\index.json`
(118 clean success + 42 существующих success-with-warnings).

Guarded force-unity:

`-NoEngineChanges -ForceUnity -DisableAdaptiveUnity -NoPCH -NoSharedPCH
-NoUBA -MaxParallelActions=2` — **PASS**, exit `0`, 17/17 actions.

Лог:
`E:\MimirComposite_S65_DefinitionPool_20260829\seed_ux_guarded_force_unity.log`.

BuildPlugin StrictIncludes / no-unity / no-PCH:

`BuildPlugin -StrictIncludes -DisableUnity -NoPCH -NoSharedPCH
-NoEngineChanges -NoUBA` — **BUILD SUCCESSFUL**, exit `0`; отдельно
собраны UnrealEditor, UnrealGame Development и UnrealGame Shipping.

Лог:
`E:\MimirComposite_S65_DefinitionPool_20260829\seed_ux_strictincludes_no_unity.log`.

UAT создал известный comment-only
`ue/MimirComposite/Config/FilterPlugin.ini`. После проверки точного пути
удалены только этот untracked-файл и пустой каталог; в ветку они не входят.
Stock Engine не является git-checkout, поэтому Engine git-status не
заявляется; все сборки шли с `-NoEngineChanges`, пути Engine не входили в
diff. Рабочее дерево репозитория после гейтов чистое.

## 6. Изменённые файлы

- `ue/MimirComposite/Source/MimirCompositeEditor/MimirCompositeEditor.Build.cs`;
- `ue/MimirComposite/Source/MimirCompositeEditor/Private/MimirCompositeEditorModule.cpp`;
- `ue/MimirComposite/Source/MimirCompositeEditor/Private/UI/MHCompositeActorDetails.cpp`;
- `ue/MimirComposite/Source/MimirCompositeEditor/Public/UI/MHCompositeActorDetails.h`;
- `ue/MimirComposite/Source/MimirCompositeTests/Private/MHCompositeActorDetailsTest.cpp`;
- `docs/receipts/seed_generation.md`.

## 7. Вопросы

Нет открытых архитектурных вопросов. Временное fail-closed поведение для
Placement Edit Mode и протухшего мультиселекционного snapshot описано в §2.
