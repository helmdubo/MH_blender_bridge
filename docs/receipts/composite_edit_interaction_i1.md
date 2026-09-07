# CE-I1 — выбор и трансформы авторских узлов

Статус: **REVIEW / READY FOR FIELD**. Ветка `codex/ce-interaction-nodes`, база `0f46f95`.
Исполнение разрешено owner 2026-09-07 после внешнего interaction-аудита.
Контракт: `docs/contracts/composite_edit_interaction_i1.md`.

## Изменения

Клик по листу вложенного композита выбирает узел-ссылку текущего определения.
Все его меши получают обычную активную подсветку UE, pivot берётся из рамки
авторского узла. GUID selection принадлежит сессии; viewport и Composite
Outliner отображают один выбор. Вложенные визуальные строки Outliner выбирают
своего авторского владельца, а строки вне активного определения остаются
доступны для контекстного Edit Contents.

Один жест меняет минимальный набор выбранных корней одной пакетной командой:
выбранный потомок не получает движение второй раз. Полная матрица проверяется
до декомпозиции; недопустимый локальный TRS или shear у потомков не принимается.
Один жест оставляет один Undo, включая малые изменения ниже обычного допуска
`FTransform::Equals`. Esc возвращает исходные transforms и поглощает остаток
жеста до отпускания мыши; следующие Esc снимают выбор и предлагают выход.

CE больше не обслуживается глобальным Slate-перехватчиком Enter/Esc. W/E/R и
навигацию обслуживает viewport; наведение на gizmo не должно превращать RMB
навигацию в перемещение объекта. Автоматических команд фокусировки камеры нет.
Явный F использует bounds выбранных узлов. Native Duplicate/Delete/clipboard
временных объектов блокируются; существующие команды draft в Outliner остаются.

При замене геометрии и закрытии сессии editor selection освобождается до
уничтожения компонентов. Closed устанавливается до teardown, чтобы callback
выделения не вернул в него уничтожаемую проекцию. `UEdMode::PostUndo` повторяет
зеркало выбора после восстановления engine selection snapshot; этот callback
движок вызывает и для Redo. Отдельный regression проверяет replacement + Undo,
валидность всех native selected components и их принадлежность текущей проекции.

## Область изменений

- Модель и команды (6 production-файлов): Document, Session и Projection,
  каждый в `.h` и `.cpp`.
- Интеграция (4 production-файла): EditorMode `.h/.cpp`, Outliner `.cpp`,
  MHEditSessionKeys `.cpp`.
- Тесты: новые NodeFrame, Commands, Interaction; обновление характеризации
  пустого выбора в ModeTransform; очистка выделения в fixture Break; исправление
  устаревшего baseline DefinitionMetrics и семантическая проверка метаданных FBX.
- Документы: уточнение CE-0, контракт CE-I1, execution status, индекс аудита,
  референс Dagor и эта квитанция.

Кандидат объединяет два связанных участка 6 + 4 production-файла: перенос
selection authority требует одновременного подключения viewport и дерева.
Суммарно это больше обычного лимита шести файлов на срез; это отклонение
показано явно для ревью. Формат, resolver/RNG, Source, runtime и Engine не менялись.

## Проверки и артефакты

Хост исполнителя: `E:\temp\MH_CEI1_host_20260907`, UE 5.7.4 CL 51494982,
Win64 Development. Плагин хоста указывает на отдельный worktree
`E:\temp\MH_CEI1_red_20260907`; открытый portfolio-проект не изменялся.
Логи ниже находятся в каталоге хоста.
Финальный полный набор выполнялся также в retained BuildPlugin HostProject:
`E:\temp\MH_CEI1_package_20260907\HostProject`, по его strict DLL.

| Проверка | Результат |
|---|---|
| RED на `0f46f95` | `CEI1_RED_BUILD.log`: сборка успешна; `CEI1_RED_TEST.log`: вложенный leaf записывает неверный local ссылки, вся её геометрия получает неверный world transform |
| Guarded non-unity/no-PCH | `CEI1_UNDO_BUILD.log` и финальный `CEI1_STRICT_RELEASE.log`: Succeeded |
| NullRHI EditMode | `CEI1_GREEN_FINAL.log`: 46 passed, 0 failed (до финальных lifecycle/tiny-gesture дополнений) |
| RHI selection smoke | `CEI1_RHI.log`: 2 passed, 0 failed; реальные render proxies, IsSelected/IsIndividuallySelected |
| Финальный RHI EditMode | `CEI1_RHI_RELEASE.log` / `RHIReleaseReport/index.json`: 46 passed, 0 failed, D3D12; включает tiny gesture и replacement + Undo |
| Полный NullRHI + golden | `CEI1_FULL_RELEASE.log` / `FullReleaseReport/index.json`: 278 passed, 0 failed, 0 skipped (187 Success + 91 Success with fixture warnings) |
| RecipeShadowParity / Applied | оба Success в финальном полном наборе |
| BuildPlugin StrictIncludes | `CEI1_STRICT.log`: ExitCode=0; последние изменения — `CEI1_STRICT_RELEASE.log`: Succeeded, non-unity/no-PCH/no-shared-PCH, без Engine changes |
| Force-unity adaptive-off | `CEI1_FORCE_UNITY.log`: Succeeded, финальные production/test sources |
| Документальный гейт / diff-check | `python tools/check_normative_docs.py`: OK; `git diff --check`: чисто |

Первый полный прогон упал при GC в DefinitionPool после Break: тестовый мир
уничтожался с четырьмя выбранными output-акторами. Парный прогон воспроизвёл
externally referenced typed-element handles. Fixture Break теперь снимает
editor selection до DestroyWorld. Следующий полный прогон завершился без этого
падения. Тесты не удалялись.

`CEI1_FULL_FINAL.log` завершил 278 тестов с 5 failed: два набора метрик
предполагали регистрацию static leaf на каждое placement, хотя после ISM
pooling регистрируются actor-owned structural components (300 вместо 400 и
4 вместо 6); FBX-тест искал произвольные байты `MH_`, не свойства модели.
Теперь он переоткрывает FBX и проверяет отсутствие user-defined `mh_` свойств
узлов. Точная позиция прежнего совпадения не сохранена после fixture cleanup;
включение имени каталога хоста в метаданные FBX — вероятное объяснение.
Ещё два perf-теста требуют начальный `mh.PerfTrace=0`, поэтому полный финальный
набор запускается без принудительного глобального `mh.PerfTrace 1`. Сами
инструментальные сценарии включают trace внутри своих scopes.

Первый расширенный RHI-прогон (`CEI1_RHI_FINAL.log`, 45/1) выявил восстановление
устаревшего component selection после Undo замены reference. Это закрывается
финальным PostUndo mirror и атомарной очисткой typed selection. Малый жест в
regression задан двумя принятыми дельтами с итогом 0.00005 cm, чтобы проверить
границу Undo, а не отбрасывание почти нулевого InputDelta движком.

При подготовке isolated host обнаружилась также ошибка staging: Copy-Item
сохраняет старые timestamps, поэтому UBT первоначально оставил test object с
прежней раскладкой Session. После принудительного обновления timestamps
изменённых заголовков и пересборки зависимых translation units ошибки исчезли.
Итоговые гейты выполняются по синхронизированным файлам с актуальными timestamps.

## Загрузка и представление мешей

Проверено по коду: normal scene использует общий `UInstancedStaticMeshComponent`
пул уровня (`MHInstancePool.cpp::PoolNewBucketComponent`). Геометрия остаётся
ассетами `UStaticMesh` в `.uasset` по `/Game/MH/Generated/Meshes`; ISM — способ
размещения этих ассетов. В Edit отдельные временные StaticMeshComponent дают
независимый picking, подсветку и трансформы. После выхода lease возвращает
видимость материализованному occurrence в пуле.

Кубы — штатная заглушка состояния Loading в
`MHEndpointPrototypeRegistry.cpp::ResolveMeshForPreview`, пока идёт асинхронная
загрузка выбранных endpoint-ассетов. В текущем логе portfolio от 2026-09-07
есть 9 622 DDC hits, 3 298 shader-job cache hits и `Shaders Compiled: 1`.
Гипотеза о полной повторной компиляции материалов каждую сессию этим логом
не подтверждается. Для вывода о времени загрузки текстур нужны отдельные
замеры пакетов, streaming и повторной материализации; в CE-I1 оптимизация
холодной загрузки не выполнялась.

Asset Viewer Dagor изучен на фиксированном commit; ссылки и различия shared/
unique сохранены в `docs/reference_notes/dagor_assetviewer_composite_edit_20260907.md`.
Его camera remap при смене координат ассета не переносится в UE world.

## Полевая проверка

Пакет: `E:\temp\MH_CEI1_field_20260907\MimirComposite_CE-I1_UE5.7.4_Win64.zip`.
В архиве — плагин с DLL/PDB, source и precompiled runtime products, инструкция
`FIELD_CHECK.md` и `manifest.json` с revision и SHA-256 исходников/DLL.
Рядом с архивом — `SHA256.txt`. Это кандидат для ревью, не owner acceptance.

Собранный плагин устанавливается после закрытия UE, заменой `Plugins/MimirComposite`.
Предыдущую сборку можно сохранить вне каталога Plugins. В Project Settings →
Plugins → Mimir Composite должен быть включён Composite Edit Mode (session +
projection), по умолчанию включённый с CE-6b1.

1. Поставить два occurrence одного композита. Оставить камеру сбоку, Edit через
   контекстное меню актора. Камера сохраняет позицию и ориентацию.
2. Нажать на разные меши одного вложенного композита: весь узел активен,
   соседние узлы не подсвечены, gizmo находится у pivot ссылки.
3. Проверить W/E/R, world/local, snapping, Ctrl/Shift и выбор рамкой. Перемещение
   вложенного узла сохраняет его внутреннюю расстановку; Undo/Redo работают внутри сессии.
4. Во время drag нажать Esc, продолжить движение мыши и отпустить: исходный
   transform восстановлен. Следующий Esc снимает выделение. Enter не сохраняет.
   RMB/WASD и Alt-навигация, в том числе около gizmo, сохраняют привычное управление.
5. В Composite Outliner открыть Edit Contents и переставить внутренние узлы.
   Save обновляет общее определение; Cancel возвращает исходную расстановку.
   Повторно открыть Edit и проверить сохранённые значения. На переходах камера не меняется.
6. Сменить resource выбранного узла или выполнить структурную команду Outliner,
   затем Undo/Redo. Выбор остаётся на корректном узле; закрыть сессию без остатков
   активной подсветки и временных компонентов.

Живое ощущение mouse/keyboard, отображение outline и камера требуют проверки
owner в реальном viewport. Автоматический RHI smoke проверяет флаги render proxy,
но не подтверждает субъективный паритет с BPP. Числовые TRS-поля, клавиатурное
дублирование и профилирование холодной загрузки остаются дальнейшими работами.
