# CE-0 — контракт: stock UE 5.7.4 API для MH Composite Edit Mode

Статус: **DECIDED** (owner 2026-09-06, близнец). Основание: спецификация
`docs/reference_notes/MH_Composite_Edit_Mode_Spec_19b7515.md` (внешний аудит,
читал 5.8.2). Ниже — сверка на локальной лицензированной **UE 5.7.4**
(`Engine/Build/Build.version`: 5.7.4, CL 51494982), file+line.

Уточнение взаимодействия owner 2026-09-07 после первого полевого кандидата:
`composite_edit_interaction_i1.md` задаёт выбор целого узла, pivot, немедленный
Esc/Cancel, publish по Save и сохранение контроля камеры/Game View.

## 1. Решения owner (2026-09-06)

| Решение | Значение |
|---|---|
| Программа | CE-0 → CE-1 → CE-2 → CE-3 → CE-4a → CE-4b → CE-5 → CE-6 заменяет текущий Edit Contents (actor-хэндлы + `Tick`); backend R6-D2/U1/U2 (publish, Unique, Bake), пул и resolver сохраняются за фасадом subsystem |
| Проекция | **компоненты одного projection-актора на сессию** (не actor на узел). Актёр и компоненты имеют `RF_Transient \| RF_DuplicateTransient`, без `bIsEditorOnlyActor`: на время Edit листья — отдельные видимые `StaticMeshComponent`, после Save/Cancel — снова ISM пула |
| Undo | BPP-политика v1: `ResetTransaction` на входе и терминальном выходе, внутри — полноценный Undo/Redo, один gesture = одна запись |
| Клавиатура | Один Esc отменяет всю сессию немедленно: активный gesture сначала возвращается к исходному snapshot, затем draft отбрасывается и режим выходит без отдельного снятия выбора или подтверждения. Enter не публикует; глобальный Slate processor R6-UX2a уходит |
| Панель режима | **как у Level Instance Edit** (уточнение owner 2026-09-07 по скриншоту): `<breadcrumb> \| Save \| Cancel`, других кнопок нет. **Save** — явный Apply Shared Definition и выход без второго подтверждения перезаписи; ошибка сохраняет сессию/draft. **Cancel** — немедленно отбросить draft и выйти без вопроса, даже если draft dirty. Save As Unique Copy… — только из контекстного меню Composite Outliner / Composite Actions |
| Структура | в режиме можно **добавлять узлы**: empty/group, ссылка на composite, static mesh, actor/gameobj (CE-4b, только уже управляемые ресурсы) |
| Навигация | Composite Outliner сохраняется; правый клик по любому подкомпозиту → Edit Contents его определения (breadcrumbs), как Edit у Packed Level Actor |
| Контекст | locked context + рамка/outline обязательны. Пустой логический выбор оставляет native actor/component selection пустым; корень и draft Composite Outliner закреплены за открытой сессией. Режим не меняет камеру и состояние Game View; native dimming через show flag `EditingLevelInstance` — проверить визуально в spike (см. §3) |

## 2. Верифицированный API (5.7.4)

| Что | Где | Вывод |
|---|---|---|
| Публичный `UEdMode` | `Editor/UnrealEd/Public/Tools/UEdMode.h`: `Enter()` :133, `Exit()` :151, `IsSelectionAllowed(AActor*, bool)` :199, `ProcessEditDelete()` :90, `ProcessEditDuplicate()` :89, `ActorSelectionChangeNotify()` :98, `UsesToolkits()` :214, `CreateToolkit()` :281, `IsCompatibleWith(FEditorModeID)` :95 | режим из plugin-модуля реализуем без private headers |
| Хуки трансформа | `Editor/UnrealEd/Public/Tools/LegacyEdModeInterfaces.h`: `ILegacyEdModeViewportInterface` :166 — `StartTracking` :199, `InputDelta` :198, `EndTracking` :200; `ILegacyEdModeSelectInterface` :33 (`BoxSelect` :43) | Begin/Update/End gesture берём отсюда (CE-4a) |
| Level Instance mode — private | `Editor/LevelInstanceEditor/Private/LevelInstanceEditorMode.{h,cpp}`, `…ModeCommands.cpp`, `…ModeToolkit.cpp` | не наследоваться; копируем паттерны |
| Регистрация/активация | `LevelInstanceEditorMode.cpp` :161 — `FEditorModeInfo(Id, Name, FSlateIcon(), /*bVisible*/ false)`; `LevelInstanceEditorModule.cpp` :1092–1098 — `FLevelEditorModule::GetFirstLevelEditor()->GetEditorModeManager().ActivateMode/DeactivateMode(Id)` | невидимый в списке режимов mode, активируется subsystem'ом |
| Ограничение контекста | `LevelInstanceEditorMode.cpp` :319 `IsSelectionDisallowed` (по editing instance, `bContextRestriction`), :314 `IsEditingDisallowed`; toggle Shift+L :391 | наш `IsSelectionAllowed`: только projection-актор и его компоненты |
| Esc-приоритет | `LevelInstanceEditorModeCommands.cpp` :14 `ExitMode` = `FInputChord(EKeys::Escape)`; `LevelInstanceEditorMode.cpp` :277 `BindCommands`: `CanExecute` = false, пока есть выделенные акторы и chord совпадает с `SelectNone` → сначала снятие выделения, потом выход; :377 `ExitModeCommand` игнорируется при открытом modal | это проверенный факт поведения UE, но acceptance MH по override owner отличается: один Esc восстанавливает активный жест и сразу отменяет всю сессию без prompt |
| PIE | `LevelInstanceEditorMode.cpp` :196 `OnPreBeginPIE` → `ExitModeCommand()` (подписка `FEditorDelegates::PreBeginPIE` в `Enter` :240) | pre-PIE handshake: Save/Discard/Stay до старта |
| Toolkit во viewport | `LevelInstanceEditorModeToolkit.cpp` :92 `Init` → inline content `SBorder{SImage, STextBlock, Save(PrimaryButton), Cancel}`; кнопки зовут `ILevelInstanceInterface::ExitEdit(bDiscard)`, `IsEnabled` по `CanExitEdit(true)` | наш `FMHCompositeEditorModeToolkit`: `<breadcrumb> \| Save \| Cancel`; Save публикует без overwrite prompt, Cancel отбрасывает draft без dirty prompt |
| Dimming | `Runtime/Engine/Public/ShowFlagsValues.inl` :265 `SHOWFLAG_FIXED_IN_SHIPPING(0, EditingLevelInstance, SFG_Transient, …)`; `LevelInstanceEditorMode.cpp` :201 `UpdateEngineShowFlags` ставит `EngineShowFlags.EditingLevelInstance` и `LastEngineShowFlags` на всех `GEditor->GetLevelViewportClients()`; `Classes/Components/PrimitiveComponent.h` :2609 `ENGINE_API void PushLevelInstanceEditingStateToProxy(bool)`, :2085 `GetLevelInstanceEditingState()`; `Renderer/Private/SceneHitProxyRendering.cpp` :1133/:1233 — proxy с `IsEditingLevelInstanceChild()` получает stencil `VisualizeLevelInstances` | **публичный путь без Engine-патча**: show flag на viewport'ах + editing state на примитивах projection-актора; всё остальное (включая shared ISM-бакеты и чужие размещения) рисуется приглушённым, как «белое здание» у BPP. Точное shading — проверить глазами в spike |
| Временный контейнер | `Runtime/Engine/Private/LevelInstance/LevelInstanceEditorInstanceActor.cpp` :43–50: `SpawnParams.OverrideLevel = LoadedLevel; bHideFromSceneOutliner = true; bCreateActorPackage = false; ObjectFlags = RF_Transient; bNoFail = true`; `GameFramework/Actor.h` :526 содержит отдельный `bIsEditorOnlyActor` | сохраняем engine reference как факт, но MH projection использует `RF_Transient \| RF_DuplicateTransient` на актёре и динамических компонентах **без** `bIsEditorOnlyActor`: карта их не пишет, Duplicate не переносит, Game View не скрывает |
| Граница Undo | `LevelInstanceSubsystem.cpp` :2537 `GEditor->ResetTransaction(LOCTEXT("LevelInstanceEditResetTrans", …))` при входе в Edit | прецедент для принятой политики |
| Commit | `LevelInstanceSubsystem.cpp` :2597 `CommitLevelInstanceInternal`: `CanDiscard()` из editor object, `SaveDirtyPackages(..., bCanBeDeclined=true)` — при отказе выход до уничтожения edit; ID сохраняется на случай reinstancing (`LevelInstanceID`) | наш publish: preflight → publisher → reconcile → закрыть проекцию; ошибка до записи оставляет draft (CE-5) |
| PLA переходы | `PackedLevelActor.cpp` :254 `OnEdit` / :260 `OnCommit` → `MarkComponentsRenderStateDirty`; :280 `IsHiddenEd()` = editing ∨ child edit ∨ loading for packing; :210/:218 `OnEditChild/OnCommitChild` | у нас вместо скрытия актора — suppression lease по хэндлам (CE-2), т.к. бакет общий |

## 3. Spike (CE-0, в редакторе, глазами)

1. Projection-актор и SMC (`RF_Transient | RF_DuplicateTransient`, без
   `bIsEditorOnlyActor`, `bHideFromSceneOutliner=true`): Save map/autosave не
   создают постоянный объект, Duplicate не копирует инфраструктуру, уже
   включённый Game View не скрывает проекцию.
2. Show flag `EditingLevelInstance` + `PushLevelInstanceEditingStateToProxy(true)` на SMC → окружение приглушено, проекция нормальная; shared ISM бакет приглушён целиком (ожидаемо).
3. `IsSelectionAllowed` → клик по чужому актору не выделяет; `ProcessEditDelete` → Delete на компоненте проекции идёт в модель.
4. `ILegacyEdModeViewportInterface::StartTracking/InputDelta/EndTracking` приходят при drag gizmo компонента проекции.
5. Enter/Edit switch/Save/Cancel не фокусируют и не перемещают камеру, не
   переключают Game View. При пустом NodeId-выборе нет native actor/component
   selection и phantom gizmo; Composite Outliner продолжает показывать корень
   и draft открытой сессии.

## 4. Fixture CE-0 (`T/MHCompositeEditFixture.h`, done)

root размещён дважды; в каждом root child вызван дважды; child = mesh +
inline group{mesh} + random{mesh, empty}; разные parent transforms; foreign
ISM того же меша. Характеризационные red'ы: точный inline-узел (сейчас —
верхний предок), Undo внутри сессии (сейчас закрывает), queued Apply A →
Cancel A → Begin B (закрыт срезом CE-pre, `QueuedApplyIgnoresLaterSession`).
Реализовано: `FCompositeEditFixture` и `Mimir.V5.Composite.EditMode.Characterization.*`
(4 теста, квитанция `docs/receipts/composite_edit_ce0.md`).
