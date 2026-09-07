# MH Composite Edit: аудит взаимодействия и корректирующая программа CE-I

Дата: 7 сентября 2026 года.
База: `helmdubo/MH_blender_bridge @ d3ec99927c28fff1960e1f6e4efb0a2caca1938b`.
Статус: PROPOSED REVIEW — не ратифицированное изменение норматива, не patch.

## 0. Границы проверки

Выполнено чтение текущего C++ Edit Mode, projection, document, session, Composite Outliner, клавиатурного роутинга, регистрации модуля, контрактов и тестов. Unreal/Blender не запускались; сборки, реальные клики и визуальное соответствие не проверялись. Числа тестов в квитанциях принадлежат авторам репозитория, а не этому аудиту.

Native reference: API Epic и повторно прочитанные LevelInstanceEditorLevelStreaming.cpp, LevelInstanceEditorMode.cpp, PackedLevelActor.cpp из публичной копии Arkfall-Development/UnrealEngine @ acba8e55e81c70351d961ebbb71a492ac80fea50. Build.version этой копии — 5.8.2. Подключение к EpicGames/UnrealEngine вернуло 404. Контракт проекта CE-0 содержит собственную сверку установленной UE 5.7.4, CL 51494982; она не выдаётся здесь за наш независимый прогон.

## 1. Вердикт

CE — уже реальный режим с сессией, reflected draft, проекцией, suppression, Save/Cancel и Undo. Возвращаться к legacy Edit и переписывать recipe/pool/publisher не требуется.

Однако текущий interaction layer — редактор одного выбранного SceneComponent, а не полноценная работа с набором авторских узлов внутри одного edit scope. Включение режима по умолчанию в CE-6b1 не доказывает паритет с BPP. CE-6b2 (удаление legacy) следует проводить после исправлений и полевой приёмки, а не использовать как способ устранения этих дефектов.

Native BPP/Level Instance Edit загружает исходный WorldAsset с его акторами и ограничивает существующие редакторские операции текущей иерархией. Это НЕ редактирование всех исходных объектов как компонентов единственного служебного актора. Один projection-актор — допустимое собственное решение MH, но за ним необходим полноценный слой логических selection, commands, transforms и Details.

Сохраняем утверждённое ограничение проекта: один transient projection-актор с компонентами, Composite Outliner, один writable definition, существующие Shared/Unique/Bake, wire v5, resolver и random namespace, BPP-подобная граница Undo. Не вводим actor на каждый leaf, постоянные NodeUID, новый runtime backend, двусторонние блокировки Blender или private Engine dependencies.

## 2. Подтверждённые расхождения

### F01 — Single selection вместо обычного multi-object editing

`MHCompositeEditorMode.cpp::SelectComponent()` безусловно делает `DeselectAll()` для выбранных компонентов. `HandleClick()` игнорирует `FViewportClick`, поэтому этот путь не интерпретирует Ctrl/Shift, кнопку мыши и double click. `SelectedProjectionComponent()` возвращает nullptr при нескольких компонентах. `MHCompositeOutliner.cpp` задаёт `ESelectionMode::Single`.

Следствие: смена SelectionMode одного дерева недостаточна. Весь transform path и selection API построены вокруг одного компонента.

### F02 — ID узла и его frame берутся от разных объектов

`GetNodeIdForComponent()` обрезает origin по `>`: leaf внутри вложенного определения принадлежит узлу-ссылке активного draft. Но `GetParentWorldForComponent()` не выполняет такую же нормализацию, а `InputDelta()` читает world-transform задетого mesh component.

Команда способна записать локаль внутреннего leaf в transform внешней ссылки. Для простого мысленного примера: ссылка N при X=100; mesh внутри child с local X=20, world X=120; drag +10. Требуется N=110 и mesh=130. Если вычислить local задетого leaf относительно N, получится 30, и запись 30 в N даст mesh=50. Это иллюстрация несовпадающих frames по коду, не выполненный тест Unreal.

Исправление: hit — только обнаружение авторского NodeId. И world, и parent world, и pivot берутся из единого edit-frame этого NodeId. Ни одного transform-read от произвольного visual leaf для авторской команды.

### F03 — Взаимодействие нестабильно при структурных изменениях

Проекция переиспользует компоненты по строковому origin (`ComponentsByOrigin`). NodeId вычисляется заново из текущего индексного selector. После удаления/reorder тот же origin и тот же pointer могут обозначать другой узел. `FindComponentForNodeId()` возвращает первый подходящий компонент из массива, собранного из TMap; для composite-reference подходят и её frame, и разные visual leaves.

Исправление: session GUID — identity выделения и редактирования; origin — адрес текущего результата resolver. Для авторского узла один явный frame; visuals — отдельный набор. Смена origin не должна незаметно переносить selection на другой GUID.

### F04 — Обратная синхронизация viewport → Composite Outliner осталась от старой модели

`EditorSelectionChanged()` принимает компонент только при `Component->GetOwner() == CurrentActor`. CurrentActor — основной AMHCompositeActor; компоненты CE принадлежат AMHCompositeEditProjectionActor. Обработчик typed selection ниже занимается SMInstance, а не component-selection проекции.

Направление tree → viewport новое; обратное CE-направление в этом обработчике отсутствует. `RefreshModel()` дополнительно восстанавливает строку по PreviousPath, не по DraftNodeId.

### F05 — Esc перехватывается до команд mode

Модуль продолжает регистрировать `FMHEditSessionInputProcessor`. Под CE он игнорирует Enter, но перехватывает Esc при фокусе любого виджета типа SViewport и вызывает/откладывает `RequestCancel()`.

Это обходит `BindCommands()` режима, который пытался уступить Esc штатному SelectNone. Epoch-guard защищает от чужой новой сессии, но не исправляет приоритет gesture → selection → exit.

Исправление: global processor не обслуживает CE вообще. Единый controller реализует Escape для viewport, tree и кнопок; focus/world/host подтверждаются. Текстовое поле сначала обслуживает свой ввод. Escape во время gesture восстанавливает snapshot, не закрывает session.

### F06 — Авторские операции есть в API/дереве, но не заведены в стандартный editor command path

В классе mode нет переопределений GetActionEdit*/ProcessEditDelete/Duplicate/Copy/Cut/Paste или обработки drag-duplicate. Add/Drop реализованы на строках Composite Outliner. Это не доказательство корректной работы Delete, Ctrl+D, Alt-drag и drop в viewport.

Не утверждаем без RHI-проверки, что каждая клавиша обязательно удалит projection actor: фактический fallback зависит от движка. Утверждаем, что отсутствует необходимый авторский маршрут и такая команда не защищена полноценным CE-контрактом.

### F07 — Details не равнозначны авторскому Details

Числовой transform и placement controls исключены из CE-4b3 по tracker. `RebuildDetails()` выводит Fixed TRS как текст. Mode разрешает редактирование projection actor, но projection actor не имеет authoring callbacks; SMC создаётся обычного engine-класса. Авторский SetNodeTransform приходит из custom InputDelta, не из общего property bridge.

Следовательно, обычное изменение доступного component property не следует считать сохраняемой правкой draft. Все поддерживаемые поля надо перенаправить в authoring commands, остальные закрыть/показать read-only. Не разрешать «передвинул через Details, Save сохранил старое».

### F08 — Нет полноценного transform admission и procedural gate

`UMHCompositeEditDocument::SetNodeTransform()` проверяет только наличие ID, после чего Modify и запись FTransform. `InputDelta()` не проверяет authored fixed/procedural режим. Для p2/profile это может записать sampled result в authored transform при сохранённом генераторе.

`World.GetRelativeTransform(ParentWorld)` выполняется до какой-либо проверки матричной представимости. Если shear уже потерян, поздняя проверка FTransform исходную ошибку не обнаружит.

Session игнорирует ошибку Refresh после записи. `IsDirty()` выражением `CanonicalBytes(...) && Bytes != OriginalBytes` объявляет failed canonicalization чистым состоянием. Недопустимое значение не должно становиться основанием для silent discard.

Исправление: candidate → finite/singular/matrix/TRS/procedural admission → атомарная запись. Ошибка preview — явное состояние с диагностикой, не потеря draft. Ошибка canonicalization означает dirty/invalid, а не clean.

### F09 — Полный rebuild графа на каждый delta

SetNodeTransform → Projection.Refresh → BuildDraftGraph: canonical serialization, MHRawPayloadHash, MHApplyCompositeV5, Invalidate/Compile draft asset, сбор графов и MHResolvePreviewGraph всего root. Затем обновляются компоненты occurrence. IsDirty также сериализует меняющийся draft.

Это не source I/O и не full-closure proof; не путать виды работы. Но fixed-node gesture оплачивает компиляцию/разрешение существенно шире затронутого subtree. Алгоритмическая избыточность подтверждена; latency в миллисекундах не измерялась.

### F10 — Режимы и lifecycle ещё не образуют единого контракта

IsCompatibleWith исключает только Foliage/Landscape. Активный LI Edit явно не исключён. Show flags переписываются на всех level viewports и при Exit безусловно выключаются. External Exit отменяет dirty session напрямую. PreBeginPIE вызывает RequestCancel; комментарии о Save/Discard/Stay не равнозначны фактическому коду.

Нужны guards до открытия, владение show-state, порядок завершения gesture, explicit exit reasons. Не обещать возможность отменить PIE только из PreBeginPIE — проверить реальный hook 5.7.4.

## 3. Новая граница авторского элемента

Вводим/выделяем не новый source-формат, а session-level модели:

```
FEditNodeFrame:
  SessionNodeId
  ParentSessionNodeId
  NodeKind
  AuthoredLocal
  EffectiveWorldMatrix
  EffectiveParentWorldMatrix
  TransformCapability

FEditVisualBinding:
  VisualOrigin
  AuthoringNodeId
  Component
  Role = Geometry | Locator | ScopeFrame

FEditSelection:
  OrderedNodeIds
  ActiveNodeId
```

`VisualOrigin` продолжает участвовать в существующей random/provenance модели. SessionNodeId никогда не заменяет его в RNG. При reorder random может легально измениться в рамках текущего path-derived контракта.

Правила:

- fixed mesh: geometry hit → его node;
- nested composite reference: любой внутренний visual hit → reference-node текущего definition;
- random: выбранный вариант визуализируется, но selection относится к owner random-node;
- inline group: отдельный редактируемый node, descendants остаются доступными;
- group/empty без меша: locator с логическим hit binding;
- внешний контекст: не editable;
- frame всего scope: визуальная граница, не случайный transform target.

Не выводить CurrentSelection из GEditor SelectedComponents как из авторского источника. Engine selection — зеркало/интеграция; session selection определяет semantic targets. При таком одноакторном представлении технически полезны также паттерны Fracture Editor: логические элементы, свой selection set, box/frustum и custom focus при одном агрегированном представлении.

## 4. Контракт BPP-подобного взаимодействия внутри scope

### Selection

Один клик, add/toggle selection с модификаторами, прямоугольное/фрустумное выделение, Select All, Select None, active node, right-click without destroying the existing multi-selection. Точные chord rules фиксируются по локальному BPP 5.7.4, не по памяти исполнителя. Camera navigation/right-drag не поглощается как обычное редактирование.

Tree ↔ viewport ↔ Details подписаны на один selection event. Tree поддерживает multi-selection, хранит expansion/selection по session GUID. Component lifetime не определяет lifetime выбранного узла.

### Gizmo

Собственный widget frame по NodeId/selection, native W/E/R, world/local basis, grid/angle/scale snapping и pivot policy. Реализовать публичные widget/select hooks (или UBaseLegacyWidgetEdMode helper) вместо надежды на стандартный gizmo одного выбранного SMC.

На Begin сохраняются все исходные authored locals, effective worlds, parent frames, selection roots и pivot. Если выбран предок и потомок, world-op применяется только минимальному набору выбранных roots; потомок наследует результат один раз.

На Update вычисляются candidate world matrices согласно нормализованному gesture. В принятой MH row-vector convention:

`NewLocalMatrix = NewWorldMatrix * inverse(EffectiveParentWorldMatrix)`.

Admission до преобразования в FTransform. Для mixed/procedural selection v1 — fail-closed с причиной, без частичного молчаливого применения. Сам random choice не запрещает gizmo: запрещает именно generated transform. Fixed child под generated parent допустим при корректном effective parent.

End закрывает одну transaction; no-op не оставляет запись. Cancel возвращает snapshot всех изменений жеста. Failure не теряет прежнее preview и не объявляет документ clean.

### Commands

Все entry points вызывают SessionCommandService: Delete, Duplicate, Copy/Paste, Alt-drag duplicate, Add managed mesh/composite/group, reparent/reorder, Rename, SetResource, SetRandomOptions, SetAuthoredTransform.

Для batch-команды: валидировать всех targets → один Modify/batch mutation → одно событие изменения → один update проекции → обновить selection GUIDs. Не делать цикл Session.DeleteNode с полным Refresh на каждом элементе.

Неподдержанная команда явно блокируется; engine fallback не должен менять projection actor/компоненты или создавать обычные map actors. Copy/Paste сериализует авторские узлы и все options/metadata, не transient UObject pointers.

### Details

Selection-aware UObject/view-model с разрешёнными авторскими полями. Числовой TRS, reset/copy-paste/multi-edit должны вызывать те же команды, что gizmo. Component-native fields без wire-представления read-only. Resource picker ограничивает admitted managed resources. Ranges/profile UI не изменяет sampled transform.

### Add / viewport DnD

Проверка drop до создания обычного world actor. Drop на tree использует parent-local intent; drop в viewport — world placement под подтверждённый authoring parent. Collision query для placement не превращает projection в физические тела. Placement создаёт draft-node, project components, новую selection и одну Undo запись. При отказе нет чужого map actor/dirty map package.

### Scope / Exit

Один writable definition. Root и nested edit используют одинаковый interaction controller. В текущем root scope nested reference атомарна; открыть её definition — отдельная команда, а не side-effect обычного клика.

Toolbar сохраняет утверждённую форму breadcrumbs | Save | Cancel. Unique остаётся существующей context command. Локальный preview / общее сохранение явно подписаны. Cancel и Save — не fallback после разрушения сессии.

## 5. Проекция: incremental update без второго resolver

Два пути:

1. Full Evaluate — открытие, структурная правка, смена ресурсного binding, profiles/ranges или необходимая regeneration.
2. Transform Update — fixed-node command: обновить frame и зависимые descendant matrices; только затронутые components. При appearance, зависящем от world position по существующему контракту, обновить соответствующие каналы; нельзя обещать transform-only до проверки реальной зависимости.

Оба пути математически сверяются с существующим full resolver. Новый frontend не вводит другое случайное распределение или другое умножение матриц. Golden/shadow parity сохраняется.

Changed-set event различает Transform, Structure, ResourceBinding, Appearance, Metadata. Rename не обязан полностью регенерировать геометрию; structural batch обновляется один раз. Geometry/material reimport обслуживается endpoint refresh, с lease и logical selection intact.

## 6. Корректирующие срезы

Срезы CE-I — дополнение к выполненному CE, а не утверждение, что CE-1…6 отсутствуют. До ratification старый tracker не редактировать автоматически.

### CE-I0 — Interaction baseline и red tests

Сопоставить одинаковую fixture в native BPP 5.7.4 и MH, записать операции и эквивалентные editor options. Добавить failing tests для F01/F02/F04/F05, viewport-command routes и invalid transform. Доставить видео/скриншоты и логи host; unit tests не заменяют эту lane.

### CE-I1 — Identity и единый node frame (P0)

Production: Editing/MHCompositeEditProjection.{h,cpp}, при необходимости отдельный EditNodeModel.{h,cpp}, Session.{h,cpp}. Устранить обратное вычисление authoring target по component pointer+текущему selector. Nested reference и random visuals получают один owning frame. Reparent использует этот frame, а не first component.

Приёмка: клик по двум разным meshes одной nested reference даёт одну selection и одинаковый transform outcome; reordering не переносит selection на соседа; scaled occurrence keep-world сохраняет полный frame.

### CE-I2 — Session selection / frontend bridges

Production: новый SelectionModel.{h,cpp}, Mode.{h,cpp}, Outliner.cpp и узкий model adapter. Multi-select, active ID, box/frustum, exact hit mapping, обратная синхронизация дерева. Native selection не используется для выбора первого удобного leaf. Отдельная подсветка scope и selection, focus по logical bounds.

### CE-I3 — Transform controller и admission

Production: TransformController.{h,cpp}, Mode.{h,cpp}, Document.cpp, Session.cpp. Snapshot, native pivot/basis, ancestor filtering, batch updates, rollback gesture, validation. Исправить dirty-on-invalid. Procedural transform policy explicit. Существующую Undo boundary не менять.

### CE-I4 — Editor commands и Escape

Production: Mode.{h,cpp}, CommandController.{h,cpp}, Outliner.cpp, UI/MHEditSessionKeys.cpp. GetAction/ProcessEdit/drag duplicate adapters. Global processor игнорирует CE. RMB context-menu и keyboard routes вызывают те же batch authoring commands. Clipboard не несёт runtime objects.

### CE-I5 — Authoring Details

Production: selection proxy/customization, module registration, session command seam. Числовой transform, reset, multi-edit, typed resource и procedural properties. Все unsupported engine fields защищены. Работает без открытого Composite Outliner.

### CE-I6 — Viewport asset placement

Изучить конкретный публичный pre-spawn drop hook 5.7.4 и доказать его в spike. Не заменять проверку созданием map actor с последующим best-effort удалением. Managed-resource whitelist; destination frame; одна транзакция; ноль посторонних actors после success/cancel/failure.

### CE-I7 — Incremental projection

Production: Projection, change-set/interaction adapter; no wire/RNG changes. Счётчики canonical writes, payload hashes, recompiles, full root resolve, touched components на 100 gesture deltas. Fixed-node drag не компилирует весь draft при каждом delta. Результат совпадает с эталонным полным resolve.

### CE-I8 — Lifecycle и пользовательская приёмка

Pre-entry конфликт LI/MH scopes; show-state ownership; new viewports; focus/modal; external mode switch; PIE; owner deletion/world cleanup; dirty publish recovery; source committed outcome. Только после RHI interaction lane и полевого подтверждения — CE-6b2 cleanup. Legacy deletion сам по себе ничего из F01–F10 не исправляет.

## 7. Обязательные сценарии приёмки

| ID | Сценарий | Критерий |
|---|---|---|
| I01 | Открыть root, затем отдельной сессией child | Один и тот же frontend без обходных жестов |
| I02 | Ctrl/Shift click 3 разных узла | Selection set, active, tree и gizmo согласованы |
| I03 | Box/frustum через несколько owners | Только узлы текущего edit definition |
| I04 | Клик по разным leaves одной вложенной ссылки | Тот же NodeId, pivot и parent frame |
| I05 | Ссылка X=100, leaf offset=20, drag +10 | Ссылка 110, leaf 130; не 30/50 |
| I06 | Select group + descendant, rotate/scale | Один inherited transform, без двойного движения |
| I07 | Удалить preceding sibling при выбранном B | Выбран B с прежним GUID, не следующий индекс |
| I08 | Delete/Ctrl+D/Alt-drag из viewport, Outliner закрыт | Меняется draft; source и чужие map actors нетронуты |
| I09 | Tree selection ↔ viewport selection | Двусторонне, включая multi и Undo |
| I10 | TRS в обычном авторском Details → Save | Сохраняется та же правка без Tick-сбора |
| I11 | Fixed random choice / generated transform / fixed child под generated parent | Разные корректные capability-режимы, no hidden Bake |
| I12 | Shear, singular parent, non-finite, недопустимый scale | Отказ до mutation либо явный invalid draft; не clean |
| I13 | 100 deltas → Undo → Redo | Одна запись, живой режим, точное восстановление |
| I14 | Esc в gesture, затем при selection, затем без selection | Cancel gesture → SelectNone → RequestCancel |
| I15 | Drop managed asset в viewport; drop unsupported asset | Новый draft-node либо чистый отказ; нет orphan map actors |
| I16 | Reimport mesh с slot migration во время Edit | Lease, selection, frames и representation согласованы |
| I17 | Save failure / частичный Unique | Draft доступен; точный publish outcome без ложного rollback |
| I18 | Native LI Edit активен, затем MH Edit | Явный конфликт до mutation; tint не повреждён |
| I19 | PIE / mode switch / root deleted / world cleanup | Ни orphan projection, ни застрявшего suppression/transaction |
| I20 | Сохранить карту и перезапустить редактор | Transient edit objects не стали содержимым карты |

## 8. О тестовом покрытии и доказательствах

Текущий MHCompositeEditModeTransformTest.cpp вызывает StartTracking/InputDelta/EndTracking с nullptr viewport напрямую. Это полезная проверка command semantics, а не симуляция native взаимодействия. Его основные кейсы — перенос, одна транзакция, simple grouped/rotated parent, frame swallowing.

Квитанция CE-6b1 сообщает 270/0 NullRHI и успешные сборочные гейты; не объявляем их независимыми результатами данного аудита. Отдельная реальная editor lane должна проверять hit proxies, modifiers, widget pivot и focus, dropdown/Details, keyboard, drag/drop и обе поверхности selection.

## 9. Карта источников

Все MH пути ниже относительно `ue/MimirComposite/Source/`, ref d3ec99927c28fff1960e1f6e4efb0a2caca1938b:

- MimirCompositeEditor/Private/Editing/MHCompositeEditorMode.cpp: SelectComponent, HandleHitProxy/HandleClick, SelectedProjectionComponent, StartTracking/InputDelta/EndTracking, Enter/Exit, BindCommands, compatibility/show flags.
- MimirCompositeEditor/Public/Editing/MHCompositeEditorMode.h: реализованные интерфейсы и отсутствующие command/widget/select hooks.
- MimirCompositeEditor/Private/Editing/MHCompositeEditProjection.cpp: BuildDraftGraph/Refresh, ComponentsByOrigin, GetNodeIdForComponent/GetParentWorldForComponent/FindComponentForNodeId, pivot spawn.
- MimirCompositeEditor/Public/Editing/MHCompositeEditProjection.h: одноакторная проекция, shape actor и mapping API.
- MimirCompositeEditor/Private/Editing/MHCompositeEditSession.cpp: SetNodeTransform/RefreshProjection, ReparentNode, IsDirty, OnRestored, RebaseOriginal.
- MimirCompositeEditor/Private/Editing/MHCompositeEditDocument.cpp: reflected tree/IDs и SetNodeTransform admission.
- MimirCompositeEditor/Private/UI/MHCompositeOutliner.cpp: SelectionMode, TreeSelectionChanged, EditorSelectionChanged, RefreshModel, AcceptDrop, RebuildDetails.
- MimirCompositeEditor/Private/UI/MHEditSessionKeys.cpp: global input processor и CE Cancel route.
- MimirCompositeEditor/Private/MimirCompositeEditorModule.cpp: регистрация global keys и Details.
- MimirCompositeTests/Private/MHCompositeEditModeTransformTest.cpp: прямые вызовы gesture.
- docs/contracts/composite_edit_ce0.md: owner constraints и заявленная локальная API-сверка.
- docs/receipts/composite_edit_ce4a.md: scope и оставленные вопросы transform admission.
- docs/receipts/composite_edit_ce6b1.md: default-on cutover, результаты авторов и будущая полевая приёмка.
- docs/RECIPE_EXECUTION_STATUS.md: точка продолжения, merged CE и NEXT CE-6b2.


### Зафиксированные ссылки

- [docs/contracts/composite_edit_ce0.md](https://github.com/helmdubo/MH_blender_bridge/blob/d3ec99927c28fff1960e1f6e4efb0a2caca1938b/docs/contracts/composite_edit_ce0.md)
- [docs/RECIPE_EXECUTION_STATUS.md](https://github.com/helmdubo/MH_blender_bridge/blob/d3ec99927c28fff1960e1f6e4efb0a2caca1938b/docs/RECIPE_EXECUTION_STATUS.md)
- [ue/MimirComposite/Source/MimirCompositeEditor/Private/Editing/MHCompositeEditorMode.cpp](https://github.com/helmdubo/MH_blender_bridge/blob/d3ec99927c28fff1960e1f6e4efb0a2caca1938b/ue/MimirComposite/Source/MimirCompositeEditor/Private/Editing/MHCompositeEditorMode.cpp)
- [ue/MimirComposite/Source/MimirCompositeEditor/Private/Editing/MHCompositeEditProjection.cpp](https://github.com/helmdubo/MH_blender_bridge/blob/d3ec99927c28fff1960e1f6e4efb0a2caca1938b/ue/MimirComposite/Source/MimirCompositeEditor/Private/Editing/MHCompositeEditProjection.cpp)
- [ue/MimirComposite/Source/MimirCompositeEditor/Private/Editing/MHCompositeEditSession.cpp](https://github.com/helmdubo/MH_blender_bridge/blob/d3ec99927c28fff1960e1f6e4efb0a2caca1938b/ue/MimirComposite/Source/MimirCompositeEditor/Private/Editing/MHCompositeEditSession.cpp)
- [ue/MimirComposite/Source/MimirCompositeEditor/Private/Editing/MHCompositeEditDocument.cpp](https://github.com/helmdubo/MH_blender_bridge/blob/d3ec99927c28fff1960e1f6e4efb0a2caca1938b/ue/MimirComposite/Source/MimirCompositeEditor/Private/Editing/MHCompositeEditDocument.cpp)
- [ue/MimirComposite/Source/MimirCompositeEditor/Private/UI/MHCompositeOutliner.cpp](https://github.com/helmdubo/MH_blender_bridge/blob/d3ec99927c28fff1960e1f6e4efb0a2caca1938b/ue/MimirComposite/Source/MimirCompositeEditor/Private/UI/MHCompositeOutliner.cpp)
- [ue/MimirComposite/Source/MimirCompositeEditor/Private/UI/MHEditSessionKeys.cpp](https://github.com/helmdubo/MH_blender_bridge/blob/d3ec99927c28fff1960e1f6e4efb0a2caca1938b/ue/MimirComposite/Source/MimirCompositeEditor/Private/UI/MHEditSessionKeys.cpp)
- [ue/MimirComposite/Source/MimirCompositeTests/Private/MHCompositeEditModeTransformTest.cpp](https://github.com/helmdubo/MH_blender_bridge/blob/d3ec99927c28fff1960e1f6e4efb0a2caca1938b/ue/MimirComposite/Source/MimirCompositeTests/Private/MHCompositeEditModeTransformTest.cpp)

### Native reference

- [Engine/Build/Build.version](https://github.com/Arkfall-Development/UnrealEngine/blob/acba8e55e81c70351d961ebbb71a492ac80fea50/Engine/Build/Build.version)
- [Engine/Source/Runtime/Engine/Private/LevelInstance/LevelInstanceEditorLevelStreaming.cpp](https://github.com/Arkfall-Development/UnrealEngine/blob/acba8e55e81c70351d961ebbb71a492ac80fea50/Engine/Source/Runtime/Engine/Private/LevelInstance/LevelInstanceEditorLevelStreaming.cpp)
- [Engine/Source/Editor/LevelInstanceEditor/Private/LevelInstanceEditorMode.cpp](https://github.com/Arkfall-Development/UnrealEngine/blob/acba8e55e81c70351d961ebbb71a492ac80fea50/Engine/Source/Editor/LevelInstanceEditor/Private/LevelInstanceEditorMode.cpp)
- [Engine/Source/Runtime/Engine/Private/PackedLevelActor/PackedLevelActor.cpp](https://github.com/Arkfall-Development/UnrealEngine/blob/acba8e55e81c70351d961ebbb71a492ac80fea50/Engine/Source/Runtime/Engine/Private/PackedLevelActor/PackedLevelActor.cpp)

- [Epic: UEdMode](https://dev.epicgames.com/documentation/unreal-engine/API/Editor/UnrealEd/UEdMode)
- [Epic: ILegacyEdModeWidgetInterface](https://dev.epicgames.com/documentation/unreal-engine/API/Editor/UnrealEd/ILegacyEdModeWidgetInterface)
- [Epic: ILegacyEdModeSelectInterface](https://dev.epicgames.com/documentation/unreal-engine/API/Editor/UnrealEd/ILegacyEdModeSelectInterface)
- [Epic: UFractureEditorMode](https://dev.epicgames.com/documentation/unreal-engine/API/Plugins/FractureEditor/UFractureEditorMode)
