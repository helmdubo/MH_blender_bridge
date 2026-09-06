# MH Composite Edit Mode — пакет для архитектурного согласования и разработки

**Статус: PROPOSED, не норматив репозитория и не разрешение начинать кодирование.**  
Дата исследования: 2026-09-06.  
MH baseline: `19b75153eb2d690ba4fc3999e5a8b4aedf77e040`.  
Dagor baseline: `75723669297e48e200a0dc67b18c1629e0975daf`.  
Целевая сборка MH: stock UE **5.7.4**, согласно текущему README проекта.

Изученный C++ референс Unreal находится в публичной копии исходников, идентифицирующей себя как UE **5.8.2**, commit `acba8e55e81c70351d961ebbb71a492ac80fea50`. Прямой доступ через подключение к `EpicGames/UnrealEngine` вернул 404. Полного совпадения этой копии с Epic upstream и с установленной UE 5.7.4 исследование не доказывает. Проверка конкретных API и lifecycle на лицензированной локальной 5.7.4 является обязательным CE-0. Код Unreal в пакет не включён.

Это статическое архитектурное исследование. Автор пакета не собирал MH, не запускал Unreal/Dagor и не выдаёт существование тестов за их прохождение. Внешние агенты должны принести сборочные логи и полевые доказательства.

## Архитектурное решение

Сохранить «рецепт + исполнитель + общие пулы», но заменить редактирование через поллинг хэндлов на **полноценную сессию авторского редактирования и собственный UE editor mode**. Заимствовать у Packed Level Blueprint разделение исходного содержимого и упакованного результата, а не его обязательный UWorld/Blueprint-контейнер.

`AMHCompositeActor` остаётся размещением. `.composite` остаётся источником. Draft — единственная редактируемая модель. Временные edit-node actors и pool instances — проекции модели, не альтернативные источники.

## Что читать

1. `SPEC.md`: решения, пользовательский контракт, состояние, владение данными, интеграция, ограничения.
2. `AGENT_TASKS.md`: предлагаемые срезы CE-0…CE-6, зависимости, закрытые области изменений, ожидаемые результаты.
3. `ACCEPTANCE.md`: воспроизводимые сценарии, отрицательные тесты и критерии приёмки.
4. `SOURCES.md`: зафиксированные исходники, функция/роль, границы достоверности.

## Как передать в работу

Сначала owner и архитектурный ревьювер согласуют отличия этого предложения от текущего норматива: точный выбор inline-узлов, поведение Escape, Undo внутри сессии, границы истории Undo, сохранение draft при ошибке и новый владелец edit-state. После этого архитектор оформляет отдельное нормативное дополнение и обновляет tracker. Не переименовывать существующие MERGED R6-срезы и не выдавать их за невыполненные.

Каждый исполнитель получает этот пакет, конкретную карточку CE и зафиксированную baseline-ветку. Перед изменениями он проверяет актуальный `docs/RECIPE_EXECUTION_STATUS.md`, активные документы и diff относительно baseline: этот пакет может устареть после дальнейших merge. Ревьювер готовит red-тесты в ветке; исполнитель не меняет ожидания тестов ради зелёного результата. Один срез — один PR. Мержит owner. Работа CE не даёт разрешения менять Engine, Blender, wire-формат, RNG или runtime backend.

Пример вводного задания агенту:

> Реализуй только CE-2 из приложенного пакета после принятого CE-1. Прочитай SPEC.md и ACCEPTANCE.md, затем актуальные нормативы проекта. Соблюдай API CE-1 и закрытый список файлов карточки. Не меняй source v5, RNG, production pool editing permissions или опубликованные семантики Unique/Bake. Доставь red→green логи, strict/non-unity и force-unity сборки, полный Mimir suite, проверку отсутствия edit-объектов в сохранённой карте, git diff --check и receipt. Не мержи PR самостоятельно.

## Уже принятое в проекте, не пересматриваемое этим пакетом

Blender остаётся источником истины; последующий экспорт из Blender может перезаписать результат UE без двустороннего контроля ревизий — это явно принято owner 2026-09-06. Source Protocol v5 и path-derived RNG сохраняются. Persistent overrides R6-O, произвольные actor-leaves R7, полный World Partition и иной runtime backend не входят в CE.


---

# Спецификация CE: редактирование MH Composite в контексте Level Editor

**PROPOSED · 2026-09-06 · baseline MH 19b7515 · target stock UE 5.7.4**

Ссылки [D*], [U*], [M*], [API*] раскрыты в SOURCES.md. Имена новых классов и интерфейсов ниже — проектные, а не уже существующий API. Точные сигнатуры UE, экспорт символов и регистрация режима проверяются в CE-0 на целевой сборке.

## 1. Цель и не-цели

Художник нажимает Edit на размещённом MH Composite и редактирует его авторское содержимое там, где оно стоит в уровне: выбор геометрии, W/E/R, численные поля, групповые операции, добавление и удаление ссылок, вложенные определения, Undo/Redo, сохранение или отмена. После выхода снова работает компактное представление в существующих пулах. Break не является способом входа в Edit.

Не делать MH-подклассом APackedLevelActor только ради кнопки Edit. Не превращать .composite в .umap и Blueprint. Не редактировать напрямую ISM-бакеты, не сериализовать произвольные UObject-properties и не восстанавливать рецепт из видимых листьев. Не менять RNG, appearance, cook-мост, физику проекта, формат v5 и authority Blender. Никакого обязательного v6/NodeUID для этой задачи.

Полный аналог BPP по поддержанному авторскому словарю не означает поддержку любых сущностей Unreal. Source v5 определяет, какие данные можно сохранять. Скрыто отброшенные свойства не допускаются.

## 2. Что подтверждено исходниками

### 2.1 Dagor

`CompositEntity` — реальная редакторская сущность со своим transform/seeds; `CompositEntityPool` содержит данные определения, варианты и внутреннюю иерархию. Плоское хранение внутреннего дерева не отменяет ссылки на дочерние composite assets и рекурсивные вызовы их исполнителей. Не утверждать, что весь Dagor-композит — чистая функция без мира: placement в setTm обращается к коллизии, bounds и воде. [D1]

Composite Editor изменяет дерево параметров исходного определения, связывая viewport-попадание с `dataBlockId`. Эти IDs назначаются заново по порядку обхода и не являются стабильными межверсионными UUID. Gizmo переводит world-transform в local относительно непосредственного родителя; выбранные потомки выбранного предка исключаются из повторного преобразования. [D2–D4]

Есть вход в дочернее определение с окружением и камерой в согласованных координатах, общий Save, Revert и Save as Unique с перенаправлением ссылки родительского определения. Это не равнозначно override только одного root-placement. [D2]

`CompositeEditorTreeDataNode::canTransform()` разрешает обычный gizmo только для матричного узла без перечисленных p2-параметров. Оригинал не пытается неявно трактовать случайный результат как авторскую матрицу. [D5]

### 2.2 Packed Level Blueprint

`APackedLevelActor` использует инфраструктуру Level Instance и связан с source WorldAsset. `ULevelStreamingLevelInstanceEditor::Load` подгружает исходный уровень для редактирования под трансформом размещения с учётом pivot. Это не восстановление исходных акторов из packed ISM. [U1, U3]

`ULevelInstanceSubsystem::EditLevelInstanceInternal` проверяет возможность входа, разрешает незавершённую текущую сессию, запоминает контекст выбора, управляет Actor Editor Context и editor streaming, уведомляет предков. В изученной реализации активная writable edit-сессия одна; переход к другому scope завершает предыдущую. Не приписывать BPP стек независимых несохранённых документов. [U2]

`ULevelInstanceEditorMode` ограничивает выделение/редактирование по контексту, регистрирует команды и input behavior, управляет редакторскими show flags и выходом при PIE. Toolkit добавляет viewport overlay Save/Cancel. Его Cancel зависит от `CanExitEdit`. [U4, U5]

Во время собственного/дочернего Edit packed actor скрывает packed-представление. `OnCommit`/`OnCommitChild` могут вызвать builder и Blueprint reinstancing; subsystem сохраняет IDs и повторно разрешает актор после таких границ. Builder собирает source-level components через специализированные builders/кластеры. [U1, U2, U7]

`CommitLevelInstanceInternal` при неудачном сохранении и `!bDiscardOnFailure` возвращается до закрытия сессии. `ULevelInstanceEditorObject` следит за сохранениями и перемещениями акторов между уровнями; после некоторых cross-level операций Discard запрещён. Вход в Edit сбрасывает общую историю транзакций. Поэтому нельзя обещать «BPP сохраняет весь общий Undo» или «Cancel всегда откатывает любые уже сохранённые данные». [U2, U6]

### 2.3 Текущее MH, а не состояние прошлого ревью

R5-пулы/selection и R6-D0, D1a, D1b, D2, U1, U2, UX1, UX2a, UX2b уже MERGED на baseline. Повторная их разработка не нужна. [M0]

Подтверждённые ограничения текущей реализации:

| Место | Факт в коде | Что меняем |
|---|---|---|
| `AMHCompositeActor::FindSessionHandleForNodePath` | Глубокая строка возвращает top-level handle предка текущего определения | Разделяем точное inline-node selection и явную границу дочернего определения |
| `SyncEditScopeHandles` | Хэндлы только для непосредственных детей invocation; Billboard-компоненты на root actor | Авторская проекция всех inline-узлов активного определения |
| `AMHCompositeActor::Tick` | Поллинг transform хэндлов, правка Nodes[Index], resolve preview и full placement compile на изменении | Авторские команды, события interaction, инкрементальная проекция |
| Actor + LevelSubsystem | Оба содержат EditingDocument; actor дополнительно EditingGraph и режим | Единственный session draft; старые APIs — фасад |
| `PostEditUndo` | Завершает Edit и восстанавливает обычное размещение | Undo шага оставляет сессию открытой |
| `CommitNestedEditComposite` | Закрывает Edit/ResetEditSession до публикации | Recoverable preflight/I/O-failure не теряет draft |
| `MHEditSessionKeys.cpp` | Глобальный Slate preprocessor, фильтр по имени виджета SViewport, Esc сразу Cancel | Контекстный CommandList/режим, проверка конкретного viewport/world |
| Отложенный Enter/Apply | Lambda следующего tick не несёт ID исходной сессии | SessionId + Epoch; отменённая команда не может опубликовать другую сессию |

Последний пункт — подтверждённое отсутствие guard в коде, а не заявление о воспроизведённой у owner потере данных. Для него предусмотрен отрицательный тест. Стоимость full placement compile — статически видимая работа, не измеренная в этом исследовании длительность кадра. [M2–M5]

## 3. Зафиксированные решения

### CE-ADR-1. Источник, draft, occurrence и представление раздельны

```
.composite v5 → UMHCompositeAsset → compiled recipe → sealed placement pools
                         │
                         └→ UMHCompositeEditSession
                               ├→ transactional authoring draft
                               ├→ selected invocation / frozen seeds / context
                               ├→ exact-node edit objects
                               └→ derived edit preview

Save: draft → preflight → existing source publisher → targeted reconcile
Cancel: retire edit projection → release suppression → sealed view
```

Никакого authoritative transform в ISM или в временном edit actor. Значения UI и геометрии вычисляются из draft; UI-команда сначала правит draft и только затем обновляет представление. Native transform callback может быть входом команды, но не фоновым наблюдением за произвольными компонентами.

### CE-ADR-2. Собственный режим, а не наследование private LevelInstanceEditor

Проектный `UMHCompositeEditorMode : UEdMode` использует публичный UnrealEd API, собственный `FModeToolkit`, `TCommands`/`FUICommandList`, native actor-selection и штатный gizmo для временных edit actors. Публичные UEdMode hooks включают selection/edit restrictions и ProcessEdit*; точные 5.7.4 hooks фиксирует CE-0. [API1]

`ULevelInstanceEditorMode` находится в private implementation модуля. Не подключать `Engine/.../Private` и не подменять его типы. Не реализовывать фиктивный ILevelInstanceInterface ради получения кнопки: subsystem ожидает source UWorld, loaded level, package/save и instance membership.

Изменение инфраструктуры рендера ради полного цветового совпадения с BPP не блокирует логически корректный Edit. В v1 обязательны viewport overlay, рамка, outline и недоступность внешнего scope для изменений. Точное native dimming — отдельная проверяемая возможность CE-0/CE-3, без Engine patch и без изменения asset materials.

### CE-ADR-3. Одна writable definition-сессия

Один активный writable draft на редактор, привязанный к конкретному Editor World и root-placement. Вокруг него можно показывать весь уровень и другие occurrences. Breadcrumbs хранят навигационный контекст, не пачку скрыто изменённых файлов.

Переход на другой definition при dirty draft: **Save and Open / Discard and Open / Stay**. Если сохранение не удалось, перехода нет. Возврат к предку после успешного сохранения не обещает отменить этот уже выполненный Save.

### CE-ADR-4. Shared Edit — режим по умолчанию

Edit открывает общее определение. Перед Save показываются LogicalName, source file и число **загруженных** затронутых placements; число live consumers нельзя объявлять числом всех использований проекта. Unloaded placements не загружаются ради UI или fan-out, при следующем входе получают актуальный рецепт.

До Save draft показывается в выбранном occurrence. Остальные instances, в том числе повторные вызовы того же definition внутри того же root, по умолчанию остаются на опубликованной версии. Это осознанно выбранная визуальная изоляция; UI прямо говорит «локальный preview, общее сохранение». При публикации меняются все потребители общего definition.

Не модифицировать shared graph по ResourceKey для live-preview выбранного вызова: это может затронуть все его вызовы внутри root. Для активного edit-preview используется отдельный scoped evaluation в сохранённом invocation context.

### CE-ADR-5. Unique/Bake остаются явными отдельными операциями

Переиспользовать существующие `DescribeSaveUnique`, `SaveEditAsUnique` и две области: InParentDefinition и ForThisPlacement. Сохранить procedural-vs-bake различие и предупреждения о path-derived reroll. Тот же Seed не гарантирует прежний результат после смены имён/путей. CE не расширяет обещания точного appearance-паритета Bake при внутренних boundary.

Persistent overrides R6-O — не часть этой сессии. Session-local GUID ниже не является wire NodeUID и не меняет RNG.

## 4. Пользовательский контракт

### 4.1 Вход и выбор

В обычном режиме клик по pooled leaf по-прежнему выбирает root composite через существующий selection adapter. Edit доступен из Details, контекстного меню viewport и Outliner; открытый Composite Outliner не обязателен.

В Edit весь UI показывает **активное определение**. Inline group и inline descendant — полноценные доступные узлы. Ссылка на другое `.composite` — атомарный узел текущего definition: клик по его геометрии выбирает именно эту ссылку. Двойной клик/Enter Contents открывает child-definition. Нельзя одним и тем же кликом незаметно перейти от перемещения child-reference к изменению общего child-source.

Random-node выбранного occurrence остаётся random-node, даже если сейчас он показывает один mesh или пустоту. Его варианты отображаются в Details/дереве как authoring-data, а не как одновременно существующие world actors.

### 4.2 Команды v1

Для fixed-transform узлов: W/E/R, численные T/R/S, local/world basis, снаппинг, F/focus, multiselect. Для структуры: добавить ссылку на уже зарегистрированный mesh/composite, добавить group, удалить авторский node, дублировать, изменить parent/порядок, заменить resource, изменить список/веса random options. Сохранение отрицательного/нулевого scale и composed-transform admission подчиняется действующим проверкам v5, не новым догадкам.

Для p2/profile-generated transform обычный «перетащить итоговый лист» в v1 отключён с понятной причиной. Диапазоны редактируются в авторских Details; результат пересчитывается тем же resolver. Автоматическое превращение результата в матрицу запрещено. Явный Bake Current Result остаётся отдельной операцией с предупреждением потери процедурной структуры. Такой минимальный контракт безопаснее и соответствует разграничению матрицы/диапазонов в Dagor. [D5]

Добавление нового unmanaged mesh, редактирование геометрии mesh asset, смена материалов вне поддержанного wire-поля, запуск произвольного Blueprint construction script и импорт world-акторов с произвольными properties не входят в CE. UI должен сообщать ограничение до изменения source, а не после.

### 4.3 UI и клавиатура

Viewport overlay:

```
Composite Edit  | garage > room > cupboard
Editing: cupboard.composite   | Shared definition | Modified
[Save & Exit] [Discard] [Save As Unique…] [Context: Locked]
```

Контекст по умолчанию заблокирован для selection/edit вне writable definition. Navigation, camera и просмотр источников разрешены. Переключатель показа/изоляции окружения не должен менять persistent visibility акторов карты.

Escape: сначала отменить активную transform/tool-operation; иначе снять текущий выбор, если это действующая штатная SelectNone-команда; затем RequestExit с Save/Discard/Stay для dirty-сессии. Кнопка Discard явно отбрасывает draft. Enter в текстовом поле подтверждает поле и не публикует source. Для Save — штатная кнопка и отдельная команда режима; при сохранении текущего Enter-shortcut он работает только без активного текстового/gizmo/modal interaction и захватывает session epoch.

Это **предлагаемое изменение** нынешнего R6-UX2a, а не утверждение, что оно уже ратифицировано. Новые ожидания клавиатурных тестов принимает ревьювер до реализации.

## 5. Владение данными и новые типы

### 5.1 UMHCompositeEditSession

Единственный владелец состояния: SessionId, Epoch, EditorWorld, RootPlacement, EditedResourceKey, InvocationAddress, FrozenSeed, FrozenAppearanceSeed, CallContext, OriginalDocument, Draft, source relative path, наблюдаемая revision рецепта, state, selection, навигационный контекст, projection/suppression leases.

Подсистема держит session UObject сильной GC-ссылкой. Слабые ссылки на world/actor проверяются на каждой асинхронной границе. C++ вспомогательные объекты с UObject-ссылками регистрируют их в GC либо используют reflected владение. Не удерживать голые указатели через publish/reimport/reinstancing.

`UMHCompositeLevelSubsystem` остаётся фасадом совместимости для существующих публичных Begin/Commit/Cancel/Unique. Он делегирует сессии, а не хранит вторую writable копию. `AMHCompositeActor` перестаёт владеть EditingDocument/EditingGraph и авторским поллингом; его sealed lifecycle и runtime transport сохраняются.

### 5.2 Transactional draft

Проектный `UMHCompositeEditDocument` — UObject с transaction-aware снимком авторской модели. Предлагаемая реализация v1: reflected `TArray<uint8> DraftSnapshot` с versioned **внутренним** session codec и reflected/session-ID map; снимок включает дерево, необязательные поля, options и порядок, selection при необходимости. Это НЕ новый wire-формат; конечный документ по-прежнему пишет `MHWriteCanonicalCompositeV5`.

Альтернатива explicit Serialize допускается только в CE-1 контракте, если тестирует эквивалентную полноту. Просто положить нерефлектируемый `FMHCompositeDocument` в UObject и вызывать Modify недостаточно: его поля должны действительно участвовать в transaction serialization. Чтение snapshot в typed DTO кэшируется по revision; не десериализовать весь документ для каждого draw-call.

### 5.3 Три адреса

`SessionNodeId` — новый при создании узла, неизменный внутри сессии/Undo, не имя объекта и не индекс массива.

`OccurrenceAddress` — путь конкретного вызова + адрес узла в выбранном authoring definition. Несколько вызовов одного definition не смешиваются.

`FMHInstanceHandle` — только адрес производной pool-материализации. Его Bucket/Slot/Generation сохраняют прежнюю семантику. Это не NodeUID. [M6]

SessionNodeId никогда не пишется в v5 и не используется как seed. При сериализации порядок остаётся авторским. Reorder/insert в индексно-адресуемом v5 может поменять path-derived random results; preview должен показать это, UI предупредить. CE не вводит обещание «структурное редактирование не перебрасывает соседние варианты».

### 5.4 AMHCompositeEditNodeActor: временная проекция

Создаётся только для узлов активного authoring definition, включая inline-children. Nested definitions не раскрываются в отдельные editable actors рекурсивно без входа в их scope. Класс native, NotPlaceable/Transient/editor-only; без произвольных construction scripts, tick-поллинга, физических тел, overlap, gameplay и nav contribution.

Fixed mesh-node может отображаться через reference-only UStaticMeshComponent с теми же render/appearance-данными. Group и empty — handle; composite-reference и random-reference используют производную визуализацию поддерева и связывают её hits с атомарным авторским node. Не создавать одновременно solid proxy mesh и его дубликат в edit pool.

Обычные свойства mesh component не становятся publishable автоматически. Details показывает модель узла, а не полный unrestricted список свойств temporary actor.

### 5.5 Контейнер временных объектов

Предлагаемый v1: временные native actors в Editor World с явным `SpawnActor` target ULevel исходного root-placement, без новых source .umap/Blueprint assets и без внешних actor-packages. Их принадлежность session определяется SessionId/registry, а не world folder или отображаемым именем.

Flags сами по себе НЕ являются доказательством отсутствия попадания в сохранение. CE-0 обязан проверить выбранный spawn/outer/flags путь на 5.7.4: обычный Save, autosave, world cleanup, PIE duplication, actor-copy и cook. Если путь создаёт persistent actor packages или записи в карте, этот adapter не допускается к CE-2. Меняется только способ размещения временной проекции, не источник .composite. Полный World Partition не заявляется: unsupported host-context отклоняется до входа с причиной.

## 6. Переход представлений и изоляция

### 6.1 Suppression lease

В существующем пуле есть HideOwner/ShowOwner, но scope внутри root требует более точной операции. Ввести проектную пару AcquireSuppression(handles, SessionId) / ReleaseSuppression(lease). У каждого slot учитывается owner visibility отдельно от счётчика временного suppression. Эффективная видимость: owner-visible AND suppression-count==0.

Нельзя делать SetVisibility на общем ISM: так исчезают чужие owners. Нельзя удалять logical slot ради временного скрытия. Migration и swap-remove сохраняют suppression. Release просроченного handle не показывает новый объект, занявший этот slot с другим Generation. [M6]

Открытие: сначала prepare valid draft/context; создать projection скрытой; после готовности первого корректного представления атомарно на game thread переключить original occurrence на projection. При ошибке открытия original остаётся видимым. Допускается явный loading placeholder, но не потеря исходной сборки.

Закрытие: закончить interaction; отвязать selection; удалить session projection; восстановить normal materialization при необходимости; снять suppression; восстановить editor context. Повторный cleanup безопасен.

### 6.2 Render parity

Проекция не меняет mesh asset, default materials, appearance-channel layout или настройки source. Для SMC-проекции используется существующий appearance transport; сопоставление ISM per-instance custom data и SMC custom primitive data проверяется отдельным RHI-тестом. NullRHI недостаточен для обещания одинакового изображения.

В одном shared ISM могут жить активный и неактивный scope, поэтому нельзя считать, что primitive-level «editing» tint автоматически различит owners. Отдельные active projection primitives дают такую границу. Native `EditingLevelInstance`/`PushLevelInstanceEditingStateToProxies` путь допускается только после проверки экспортируемого публичного API и фактического shading на stock 5.7.4; фиктивное LevelInstance membership запрещено. При недоступности — честный locked context + frame/outline; pixel-identical dimming не считается выполненным.

## 7. Команды, transforms, incremental preview

Проектная командная модель:

```
BeginTransform(selection, basis, pivot)
UpdateTransform(inputDelta)
EndTransform(Commit | Cancel)
SetNodeTransform(SessionNodeId, authoredLocal)
AddNode / RemoveNode / DuplicateNode / ReparentNode / SetResource
SetRandomOption / SetWeight / SetPlacementRanges
RequestSave / RequestDiscard / RequestOpenChild
```

Все entry points сходятся сюда: viewport, Details, Outliner, keyboard, automation. Session command запрещает изменение не своего scope даже если UI-ограничение обошли.

Для fixed-transform узла в column-vector нотации:

`newLocal = inverse(effectiveParentWorld) * newWorld`.

UE row-vector эквивалент используется согласованно с существующим кодом. Матрица берётся от непосредственного авторского родителя, не всегда от root-placement. Перед decomposition проверяются finite, invertibility, determinant и действующий TRS-reconstruction predicate. Не применять безопасный fallback inverse(identity) к singular-parent. Scale/shear не чинятся молча.

На BeginTransform фиксируются baseline transforms и effective parent matrices. Если выбран ancestor и descendant, операция применяется только selected roots; descendant наследует результат. Update вычисляется от baseline, не многократно накапливает quaternion/scale ошибки. Cancel возвращает baseline draft/preview/selection.

Resolved transform может включать placement/jitter; его нельзя просто записать в authored Transform и повторно оставить placement активным. CE v1 не даёт обычный gizmo на generated узлах. Details меняет сами диапазоны. Для fixed descendants под сгенерированным ancestor используется фактический frozen effective-parent frame из того же invocation/seed-контекста. Корректность этого пути проверяется against resolver, не вычислением «на глаз».

Каждый изменённый operation формирует dirty-subtrees. Для простого fixed-node движения без изменения choices: обновление transforms проекции/хэндлов затронутых листьев. Для структурной команды или ranges: Layout+Appearance выбранного scope, затем diff; full root rebuild не обязателен и не запускается для unrelated owners. Первую scope-resolve реализацию допускается оставить целиком по активному definition, но не скрывать её стоимость и не называть уже оптимизированным subtree evaluator.

RNG математика и canonical order не меняются. Любая оптимизация сравнивается с существующим reference resolver. Источники, full closure, source hashes и Asset Registry tag queries не участвуют в drag. Proof остаётся в опубликованных exit-point контрактах.

## 8. Undo и границы сессии

Внутри открытого Edit каждая законченная операция имеет Undo/Redo. Drag из сотни промежуточных событий — одна undo-запись. Undo не закрывает сессию и не правит .composite на диске. Обратный ход меняет transactional draft, затем восстанавливает projection по session IDs. Production pool и root-placement не являются записью изменения authored node.

**Предлагаемая политика v1 для согласования:** BPP-подобная явная граница истории транзакций при входе и терминальном выходе из режима. Вход/выход не отменяются; старая общая история UE очищается с явным уведомлением. Внутри сессии — обычные UE-транзакции draft, без nested transaction на каждый Tick. Терминальное закрытие чистит ссылки транзакций на временную сцену. Это компромисс для надёжного v1, не обязательное свойство любой возможной реализации; он должен быть принят owner до кода. Изученный native BPP тоже ResetTransaction на входе. [U2]

Не сбрасывать историю на selection, каждом gizmo Update, обычном property change или failed validation. Существующая политика irreversible source publication сохраняется. Если owner требует сохранения общей истории карты через Edit, нужен отдельный контракт локального undo-journal/транзакционного adapter, а не обещание, что transient-флаги решат историю автоматически.

Legacy `AMHCompositeActor::PostEditUndo` для обычных placements сохраняет нужный rebuild. Новые authored edit-команды не должны вызывать этот путь как нормальный Undo. Его реакция на внешнее уничтожение/Undo root рассматривается отдельно как session host-loss, с безопасным завершением.

## 9. Состояния и lifecycle

```
Idle → Opening → EditingClean ↔ EditingDirty
                       ↕             ↕
                    TransformActive
Editing* → Validating → Publishing → Closing → Idle
                    ↘ EditingDirty  (pre-write failure)
Publishing → RecoveryRequired      (partial external side effects)
Editing* → Discarding → Closing → Idle
```

Dirty основан на authoring changes, не сравнении positions в кадре. Для точного no-op Save можно сравнить canonical bytes на exit boundary; не вычислять hash дерева каждый viewport tick. Полное возвращение к исходному draft через Undo снимает dirty.

Каждый deferred callback содержит `(SessionId, Epoch, ExpectedState)`. Root/World weak refs, source/key и текущая projection revision перепроверяются. При Cancel/Close/BeginNew epoch меняется; stale load completion и queued Apply становятся no-op. Задача асинхронного endpoint load не имеет права автоматически сохранять source.

Обязательные выходы: toolbar Discard, RequestClose режима, смена scope, закрытие уровня/редактора, pre-PIE/SIE, удаление root, reimport/reinstancing asset/actor, plugin shutdown. Подписки удаляются симметрично. После начала world cleanup нельзя создавать actors/components для «восстановления» уже уничтожаемого мира.

Обычный mesh payload reimport разрешается через existing endpoint reconcile. Изменение редактируемого recipe или invocation context при открытом draft не должно молча переписать draft. Сессия показывает устаревший локальный контекст и предлагает сохранить отдельно/открыть заново/завершить по согласованной политике. Это защита согласованности внутри editor session, **не** новый двусторонний CAS с Blender и не попытка запрещать принятый owner overwrite.

PIE не исполняет draft: сначала явный Save/Discard/Stay. Если конкретный pre-PIE hook не позволяет отменить вход, CE-0 обязан зафиксировать поддержанный cancel/defer механизм; нельзя начинать PIE и затем обещать безопасно спросить. Forced shutdown сохраняет recovery-draft при возможности без auto-publish.

## 10. Сохранение

### 10.1 Shared Save

1. Завершить/зафиксировать активную interaction; проверить session ID, current state и владелец.
2. Получить один immutable snapshot draft для validation/publish.
3. `MHWriteCanonicalCompositeV5` + действующий preflight; ошибки возвращают EditingDirty без закрытия и потери результата.
4. Показать имя source, область Shared, loaded-consumers, предупреждения; проверить доступность публикации без побочных эффектов.
5. No-op: закрыть без source write, уведомлений и recompile.
6. Выполнить существующий publisher через facade; источник меняется по существующей политике. Все новые irreversible side effects явно отделены от native transaction.
7. Успех: уведомить изменённое definition через existing targeted reconcile; не перекомпилировать неизменённые parent recipes и не загружать весь мир.
8. Закрыть projection и восстановить актуальное sealed placement, selection/context.

Не пытаться отменить file-write через `FScopedTransaction`. До записи корректный draft должен оставаться восстановимым. Если publisher применяет Asset до записи, его live mutation и notifications должны находиться под guard; на ошибке восстанавливаются source/asset средствами существующего RestoreDefinition, а authored draft сохраняется в recoverable session/snapshot.

### 10.2 Ошибки после побочных эффектов

«Не записан файл», «файл записан, но import/reconcile не завершился» и «часть Unique-chain создана» — разные результаты. Ввести внутренний publish result с `NoExternalChange / SourceCommitted / PartialBatch` и отдельными warnings, committed targets и draft recovery. Не выдавать `SourceCommitted` за полностью отменённый Save.

После source-write границы сохранение общего Undo не обещается. Даже если session UI остаётся открытым для retry/recovery, original snapshot и подсказка Discard должны соответствовать фактическому source; нельзя предложить Discard как отмену уже записанного файла.

### 10.3 Unique batch

Переиспользовать утверждённый план двух scope. Validate все имена/targets до первых записей. Создавать inner→outer определения; root-placement/single parent binding менять последним, после успешной доступности всех нужных assets. Несколько atomic file replace не являются атомарной транзакцией всей цепочки. При частичном сбое явно перечислить новые файлы/assets; не удалять ранее существовавшие ресурсы, не перенаправлять actor на недостроенный root. История cleanup может быть отдельным отчётом.

### 10.4 Blender authority

Не вводить новую блокировку последующего Blender export. В UI Save оставить короткое напоминание: «UE публикует .composite; следующий экспорт из Blender может его перезаписать». Это действующее решение, не открытый архитектурный вопрос. [M0]

## 11. Интеграция в текущие файлы

| Existing seam | Ответственность после CE |
|---|---|
| `Composite/MHCompositeLevelSubsystem.*` | Совместимый facade Begin/Commit/Cancel/Unique, publisher orchestration; не вторая модель |
| `Composite/MHCompositeActor.*` | Sealed placement, seeds/call context, normal lifecycle; без authoring Tick/EditingGraph |
| `Composite/MHCompositeSelectionAdapter.*` | Sealed owner selection; Edit hit routed through session occurrence mapping |
| `Composite/MHInstancePool.*` | Stable handles, existing readonly permissions; suppression leases/reconcile preservation |
| `Composite/MHCompositePlacementCompiler.*` | Existing backend; consume candidate plan; не скрытый источник авторских правок |
| `UI/MHCompositeOutliner.*`, `MHCompositeOutlinerModel.*` | Session tree/selection/view, не источник состояния и не prerequisite Edit |
| `UI/MHEditSessionKeys.*` | Тонкая совместимость/удаление глобального processor; команды идут в mode/session |
| `UI/MHCompositeActorDetails.*`, `MHSourceToolMenus.*` | Одинаковые commands и Save-scope summary |
| `MimirCompositeEditorModule.*`, `.Build.cs` | Публичная регистрация режима/команд и симметричный lifecycle |

Новые проектные файлы располагаются в `Public/Editing` и `Private/Editing`: MHCompositeEditSession, MHCompositeEditDocument, MHCompositeEditProjection, MHCompositeEditNodeActor, MHCompositeEditorMode, MHCompositeEditorModeToolkit, MHCompositeEditCommands, MHCompositeEditInteraction, MHCompositeEditPublisher. Делить файлы только по ответственностям; не создавать framework уровня всего DCC.

## 12. Производительность и границы выпуска

Не использовать фиктивные миллисекунды без замера. Обязательные structural budgets: ноль source I/O/proof/tag scans во время drag; ноль изменения чужих owners; ноль unconditional authored-polled Tick; одна undo-команда на gesture; editable actors пропорциональны active authored nodes, а не сумме всех leaves всех placements; physics/nav не пересоздаются для edit-projection на каждом кадре; render refresh батчится по затронутым представлениям.

Цифровые baseline/report: cold/warm Enter, 100 fixed-node drag updates, structural change, nested switch, Save Shared на двух и ста loaded consumers, Cancel, peak actor/component count, source/proof calls и memory after repeated open/close. Сравнить до/после на одном host и том же контенте. Отдельно RHI/viewport-тесты: hit selection, outline, Game View, appearance parity, no z-fighting. Headless green не равен завершённому UX.

Первая вертикаль для acceptance: два placements одного root; в каждом два вызова одного child; child содержит mesh, inline group с потомком и random-node с empty option. Edit второго child первого root → выбрать именно inline потомка → move → Undo/Redo → Cancel. Повторить Shared Save и две Unique-области. Это одновременно проверяет model, scope, transform, lifetime и смысл сохранения.


---

# CE — карточки работ внешним агентам

**PROPOSED.** Пакет не заменяет `docs/RECIPE_EXECUTION_STATUS.md`. Baseline: MH `19b75153eb2d690ba4fc3999e5a8b4aedf77e040`; stock UE 5.7.4. Общий контракт — SPEC.md; источники — SOURCES.md. Все новые API/файлы здесь проектные.

## Общие правила

Перед каждым PR: прочитать актуальный tracker и норматив, проверить diff от baseline, не работать по устаревшему порядку MERGED-срезов. Действующий production-маршрут сохраняется до явного CE cutover. В каждый момент одна сессия имеет ровно одного writable владельца; feature flag выбирает целиком старый или новый session backend, а не двух писателей одного draft.

Дерево `MimirCompositeEditor` ниже сокращено как `E`, `MimirCompositeTests/Private` как `T`; оба находятся под `ue/MimirComposite/Source/`. `Public/Editing/X.h` и `Private/Editing/X.cpp` — предлагаемые новые файлы. Существующие файлы нельзя переименовывать по догадке; их точное наличие проверяется перед изменением.

Каждый PR предоставляет red→green логи, команду и SHA сборки, `git diff --check`, `python tools/check_normative_docs.py`, полный `Automation RunTests Mimir` согласно README/KICKOFF, включая RecipeShadowParity, и отсутствие регрессий existing selection/pool/Unique/Bake. Обязательны stock guarded build, `BuildPlugin -StrictIncludes` без unity/PCH и force-unity без adaptive unity по уже принятому проектом протоколу. NullRHI не заменяет viewport/RHI acceptance. Тесты со skip должны перечисляться отдельно; «suite зелёный» не скрывает пропущенные критические тесты.

Архитектор/ревьювер создаёт fixture и red assertions; исполнитель не меняет ожидания или golden ради прохождения. Изменение public API, существующей нормативной политики, source v5, RNG, diagnostics registry, Engine или области вне карточки — STOP+OPEN. Локальные решения внутри закрытой области — DECIDED в receipt, с обоснованием и тестом. PR не мержится исполнителем.

Изменения `MHCompositeLevelSubsystem`, `MHCompositeActor`, `MHInstancePool`, модуля и Build.cs интегрируются последовательно; два агента не редактируют эти общие файлы одновременно.

## CE-0 — проверка stock API и вертикальный red-fixture

**Зависимости:** owner согласовал предложенные UX/Undo boundaries. До этого — только исследование и тест-план.

**Цель:** доказать, что подход реализуем плагином на фактической UE 5.7.4, а не предположить совпадение с прочитанной копией 5.8.2.

**Разрешённые файлы:** новый `docs/contracts/composite_edit_ce0.md`; `docs/receipts/composite_edit_ce0.md`; `T/MHCompositeEditModeCharacterizationTest.cpp`; `T/MHCompositeEditFixture.h`. Минимальный тестовый plugin adapter допускается только после фиксации его точного пути контрактом. Никаких изменений production Engine/asset formats.

**Проверить локальные исходники и записать file+line:** PackedLevelActor.cpp, PackedLevelActorBuilder.cpp, LevelInstanceSubsystem.cpp, LevelInstanceEditorLevelStreaming.cpp, LevelInstanceEditorObject.cpp, LevelInstanceEditorMode.cpp/Toolkit.cpp, публичные Tools/UEdMode.h и actor editor context API. Установить доступные способы регистрации UEdMode, mode-scoped command routing, native transform Begin/Update/End, selection restrictions, block/defer PIE, save hooks и удаления временных actors.

**Spike обязан ответить:** как temporary native actor с OverrideLevel/flags избегает actor packages, сохранения/PIE/cook; можно ли использовать native editing tint без private headers; как один native gesture записывает draft в UE transaction; какие изменения по сравнению с5.8.2 существенны. Не объявлять классы/сигнатуры экспортированными только потому, что они встречаются в HTML API.

**Red fixture:** root дважды размещён, child дважды вызван в каждом root, inline grandchild, random+empty, разные parent transforms, foreign ISM того же mesh. Воспроизвести текущий выбор ancestor и семантику Undo. Отдельный red для queued Apply A → Cancel A → Begin B → next tick.

**Доставка:** таблица verified5.7.4 API, выбранный spawn/transaction adapter, отсутствующие возможности, воспроизводимые commands, red tests. Нельзя продолжать к production cutover при недоказанном сохранении/cleanup. Если exact native tint недоступен, закрепить locked-context+outline как v1 и явно отметить отсутствие pixel parity, а не менять Engine.

## CE-1 — единый session owner и transactional draft

**Зависимости:** CE-0.

**Разрешённые production файлы:** `E/Public/Editing/MHCompositeEditSession.h`, `E/Private/Editing/MHCompositeEditSession.cpp`, `E/Public/Editing/MHCompositeEditDocument.h`, `E/Private/Editing/MHCompositeEditDocument.cpp`, `E/Public/Editing/MHCompositeEditCommands.h`, `E/Private/Editing/MHCompositeEditCommands.cpp`; минимальный facade в `E/Public/Composite/MHCompositeLevelSubsystem.h` и соответствующем cpp.

**Тесты ревьювера:** `T/MHCompositeEditSessionTest.cpp`, `T/MHCompositeEditDocumentTest.cpp`, общий fixture.

**Сделать:** один сильный GC-owned session UObject, immutable original, mutable transaction-serializable snapshot, session-local IDs, exact node addressing для всех inline descendants/options, state transitions, session epoch, создание typed DTO cache из snapshot по revision. GetEditingDraft должен читать текущий draft, а не оставшийся initial subsystem snapshot. Не вводить persistent NodeUID или новый wire codec.

**Инварианты:** no-op begin/discard не пишет source; имя и UUID proxy не влияют на serialize/RNG; все исходные options/empty/zero-weight/метаданные сохраняются; структурный порядок source не сортируется. Closed session отказывает всем queued commands по ID/epoch.

**Acceptance:** A01–A04, A11, A16 из ACCEPTANCE. Snapshot реально попадает в native transaction; одиночный Modify на нерефлектируемом DTO не принимается.

## CE-2 — авторская проекция и suppression выбранного occurrence

**Зависимости:** CE-1; успешный temporary-actor probe CE-0.

**Production:** `E/Public/Editing/MHCompositeEditProjection.h`, cpp; `MHCompositeEditNodeActor.h/.cpp`; `E/Public/Composite/MHInstancePool.h`, cpp; согласованные facade additions в session header/cpp. Любое public pool extension заранее утверждается контрактом.

**Тесты:** `T/MHCompositeEditProjectionTest.cpp`, `T/MHCompositeEditSuppressionTest.cpp`.

**Сделать:** exact-node temporary actors активного definition; atomic nested-reference proxies; mixed mesh/group/random/empty presentation; owner-independent suppression leases по stable handles с отдельными owner visibility и suppression count. Переключение представлений prepare→show/hide, симметричный cleanup. По возможности переиспользовать existing appearance transport, не копировать материалы/геометрию assets.

**Запрещено:** HideOwner(root) для редактирования одного child без сохранения остального root, SetVisibility общего ISM, Remove logical slot вместо suppression, рекурсивно распаковывать все root leaves в editable actors, менять asset-key чтобы искусственно поменять RNG namespace.

**Acceptance:** A02–A08, A19–A21. Два вызова child внутри одного root не меняются одновременно в локальном preview. Смена bucket при reimport сохраняет suppression и reverse lookup. Чужой ISM/owner не исчезает. Отдельный RHI test appearance parity.

## CE-3 — editor mode, команды и точное selection

**Зависимости:** CE-2. UI может готовиться параллельно CE-2 только через утверждённые session interfaces, без изменений общих файлов.

**Production:** `MHCompositeEditorMode.h/.cpp`, `MHCompositeEditorModeToolkit.h/.cpp` в Editing; согласованный `MHCompositeEditCommands`; `E/Private/Composite/MHCompositeSelectionAdapter.cpp` и header; `E/Private/UI/MHCompositeOutliner.cpp`, `MHCompositeOutlinerModel.cpp`, их headers; `E/Private/UI/MHEditSessionKeys.cpp` и header; editor module header/cpp и Build.cs — через интегратора.

**Тесты:** `T/MHCompositeEditSelectionTest.cpp`, `T/MHCompositeEditCommandRoutingTest.cpp`.

**Сделать:** зарегистрировать собственный UEdMode, viewport toolbar, scope label/breadcrumbs, exact inline-node selection, explicit nested scope entry, mode-scoped commands, focus/F, locked context. Sealed route остаётся current owner selection. Все ProcessEdit* сначала идут в модель. Глобальный Slate processor старого режима больше не является production input authority.

**Escape priority:** active tool/gesture → selection → RequestExit. Enter в Details не вызывает Save. Любое отложенное modal действие проверяет captured epoch. Outliner закрыт — все обязательные viewport операции продолжают работать. Stock PLB/LI Edit и MH Edit одновременно writable не допускаются.

**Acceptance:** A03–A06, A12, A16, A23. Переключение нескольких viewport/asset editor окон не крадёт команды. Состояние toolbar соответствует session, а не текущему выбранному bucket.

## CE-4 a — event-driven transforms и Undo внутри Edit

**Зависимости:** CE-3.

**Production:** `MHCompositeEditInteraction.h/.cpp`, session/document/commands/projection согласованные реализации; `E/Private/Composite/MHCompositeActor.cpp` и header — интегратор удаляет authoring Tick в новом пути. Старое sealed PostEditUndo не ломать.

**Тесты:** `T/MHCompositeEditTransformTest.cpp`, `T/MHCompositeEditUndoTest.cpp`.

**Сделать:** native Begin/Update/End hooks, baseline transforms, selected-roots policy, parent-local conversion, exact TRS admission, один transaction на gesture, numeric Details path через те же команды. Undo/Redo применяются к draft, затем projection; не к root placement. Не считывать world component transforms каждый Tick как первичный канал данных.

**Generated transforms:** ordinary gizmo запрещён по capability; fixed child под generated ancestor корректно использует frozen effective parent. Фиксированный вариант не превращает случайный родитель в fixed. Явный authored/placed separation обязателен.

**Acceptance:** A07–A11, A13–A15, A22. Особенно no-op Enter/Save, transform с глубоким rotated hierarchy, ancestor+descendant multiselect, cancel-mid-drag, Undo после100 Updates оставляет session active и возвращает именно один шаг.

## CE-4 b — структурные команды и procedural Details

**Зависимости:** CE-4 a.

**Production:** document/commands/projection; новый `E/Private/Editing/MHCompositeEditNodeDetails.cpp` и `E/Public/Editing/MHCompositeEditNodeDetails.h`; Outliner отображение только через согласованный API.

**Тесты:** `T/MHCompositeEditStructureTest.cpp`, `T/MHCompositeEditProceduralTest.cpp`.

**Сделать:** Add reference/group, Delete, Duplicate, Reparent, reorder, replacement managed resource, random option/weight и placement range editing. Mapping session IDs ↔ draft selectors обновляется как часть атомарной authoring-команды. Undo восстанавливает options, order, metadata и IDs. Parent transform сохраняется согласно документированному режиму world-preserving reparent; невозможный TRS отказывает до изменения.

**Запрещено:** Break+Build как Edit, serialize только selected random leaves, нормализация RNG/weights как «оптимизация», silent loss неподдержанных полей, исполнение arbitrary Blueprint properties.

**Acceptance:** A09, A10, A14, A15, A17, A18. Структурное изменение может reroll path-derived RNG: expected results задаёт unchanged reference resolver, а не обещание прежней картинки.

## CE-5 — publish/unique и восстановление после ошибки

**Зависимости:** CE-4 b.

**Production:** `MHCompositeEditPublisher.h/.cpp`, session/commands; `MHCompositeLevelSubsystem.h/.cpp` (facade/shared existing publisher), UI/ToolMenus/Details только адаптация к result/state. `MHPublishCompositeV5`/source import semantics не переписываются; изменение их API — отдельный утверждённый seam.

**Тесты:** `T/MHCompositeEditPublishTest.cpp`, `T/MHCompositeEditUniqueFailureTest.cpp`.

**Сделать:** preflight без закрытия session, no-op Save, единый immutable snapshot на publish, distinguish NoExternalChange/SourceCommitted/PartialBatch, сохранить recovery draft, reconcile точного definition. Failed pre-write save оставляет активный корректный draft и возможность исправить/повторить. Post-write failure честно показывает committed source state. Unique batch связывает owner последним и перечисляет orphaned created targets при частичном сбое.

**Не делать:** новое Blender version-lock/CAS; transaction rollback файлов; полную перекомпиляцию parents; force-load всех consumers; автоматический Bake; исчезновение shared/placement unique scope distinction.

**Acceptance:** A16–A18, A24–A29. Проверить Error от записи файла, Error после успешной записи при import, failure на второй копии Unique-chain. Source snapshots сравниваются фактически, не по одному UI success flag.

## CE-6 — lifecycle, production cutover, удаления и полевая приёмка

**Зависимости:** CE-5.

**Production:** session/mode/projection cleanup; module lifecycle; минимальные согласованные guards в runtime bridge для отказа входа в PIE/cook с неразрешённой сессией без изменения runtime data/algorithm. При необходимости новый editor-only lifecycle adapter. Удаление старых actor EditingDocument/EditingGraph/поллинга и global input authority после перевода callers.

**Тесты:** `T/MHCompositeEditLifecycleTest.cpp`, `T/MHCompositeEditPerfTest.cpp`; обновление approved tracker/ADR/receipts.

**Сделать:** OnRequestClose, world/host deletion, shutdown, active LI-mode conflict, pre-PIE handshake, stale async callbacks, save/autosave/cook invisibility, zero leftover delegates/proxies/leases после повторных циклов. Документировать фактический temporary actor container и поддержанный уровень world contexts. Поддержка произвольного WP/LI edit nesting не подразумевается.

**Полевая acceptance:** две сцены — маленький fixture и реальная portfolio-сборка owner. Outliner закрыт/открыт, W/E/R, precise leaf selection, drag+Undo/Redo+Cancel, Shared и Unique, Game View, появление endpoint после async-load. Сравнить изображение sealed/edit после no-op и пары операций.

**Метрики:** cold/warm Enter/Exit,100 Updates, peak actors/components, source/proof invocations, unrelated owner handle preservation, steady memory после50 Open/Close. Латентность p50/p95 — измерить на одном host; численные пороги согласовать по baseline, не выдумывать.

**Definition of Done:** A01–A29 выполнены или явно обозначенные out-of-scope contexts дают проверенный отказ до входа. Нет второго writable state. Новый mode — единственный production Edit entry. Старые R6 операции доступны через facade, wire/RNG/runtime golden без изменений. Каждый remaining gap перечислен; частичная visual parity не названа полной.

## Порядок интеграции

`CE-0 → CE-1 → CE-2 → CE-3 → CE-4 a → CE-4 b → CE-5 → CE-6`.

Параллельно разрешены лишь независимые UI layouts, source notes и тестовые fixtures после фиксации интерфейсов. Не параллелить изменение session ownership, pool suppression и публикации в одном большом cpp без отдельного интегратора.


---

# Приёмка CE

**PROPOSED.** Это задания на тесты, не отчёт об уже выполненных проверках.

## Эталонный fixture

Два разных `AMHCompositeActor` A и B ссылаются на root R. В R два узла-ссылки X и Y вызывают child C. В C находятся mesh M, inline group G → mesh N, random-node Q с вариантами mesh/composite/empty и нулевым весом одного варианта, fixed node под parent с placement. У A/B разные basis и seeds. Рядом foreign native ISM с тем же mesh и MH placement другого определения в том же compatible бакете. Использовать не только identity transforms: translation, rotation вокруг нескольких осей, uniform scale, допустимый non-uniform scale и отдельно отказной shear/singular-parent.

Операция Edit выбранного occurrence A/Y открывает общий C, но draft preview локален A/Y до Save. A/X, B/X, B/Y продолжают опубликованный preview. После Shared Save все потребители C получают изменение. После ForThisPlacementUnique только A переключает root согласно прежнему контракту; процедурный reroll отражён в expected result.

## Модель и выбор

| ID | Действие | Обязательное ожидание |
|---|---|---|
| A01 | Открыть/закрыть без изменений; повторить50раз | Source bytes прежние; нет лишних proxies, leases, delegates; sealed appearance и transform прежние |
| A02 | Edit A/Y при4вызовах C | Локально меняется только A/Y; остальные occurrences не получают draft автоматически |
| A03 | Клик mesh N внутри inline group G | Выбран именно N; перемещение не меняет G/M; адрес не подменён top-level ancestor |
| A04 | Клик геометрии вложенного composite-reference | Выбран reference-node текущего definition; изменение child-source только после явного Enter Contents |
| A05 | Composite Outliner закрыт | Edit, viewport selection, W/E/R, F, Save/Discard доступны; нет зависимости от Slate-дерева |
| A06 | В Edit выбрать внешний actor/foreignISM через viewport/Outliner | Изменение отклонено mode и command layer; камера/просмотр не заблокированы |

## Transforms и Undo

| ID | Действие | Обязательное ожидание |
|---|---|---|
| A07 | Изменить fixed node под глубокими rotated parents | Новый local восстанавливает целевой world в пределах действующего admission; Save/reimport не даёт второго transform |
| A08 | Одновременно выделить group и его descendant; rotate/scale | Descendant получает одну inherited трансформацию, не две |
| A09 | Reparent preserving world под допустимый parent, затем Undo | World сохранён; восстановлены исходные parent, local, order и SessionNodeIds |
| A10 | Reparent/scale создаёт shear или singular-frame | Отказ до commit draft; last-good projection остаётся; source не меняется |
| A11 |100 drag Updates → End → Undo → Redo | Один Undo-шаг; режим не закрывается; восстанавливаются именно draft и его проекция |
| A12 | Esc внутри drag, затем без drag с selection, затем без selection | Последовательно cancel gesture, SelectNone, RequestExit; нет внезапного publish/discard при вводе чисел |
| A13 | Изменить несколько полей Details без editor Tick между ними | Draft и preview актуальны событийно; Save не требует `Actor->Tick(0)` для сбора авторских правок |
| A14 | Generated p2/profile node | Ordinary result gizmo недоступен с причиной; Details редактирует ranges; no-op не записывает sampled result в authored Transform |
| A15 | Fixed child под generated ancestor; same frozen seed | Правильный effective-parent frame; parent range не заменён sampled matrix; unchanged reference resolver даёт совпадающий preview |

## Commands и сохранение полной структуры

| ID | Действие | Обязательное ожидание |
|---|---|---|
| A16 | Queue Apply A; Cancel A; открыть B до next tick | Отложенная команда A не сохраняет B; аналогично stale endpoint callbacks после close/reopen |
| A17 | Delete/Duplicate/reorder random-node; Undo/Redo | Сохранены hidden/nonselected/empty/zero-weight options, metadata, order; никакого Bake без команды |
| A18 | Изменить display label/создать новые session IDs | Ни RNG namespace, ни wire identity не выводятся из proxy names/IDs; структурный reroll проверяется reference, не скрывается |

## Пулы и представления

| ID | Действие | Обязательное ожидание |
|---|---|---|
| A19 | Скрыть A/Y для projection, owner B в том же ISM | B и foreign geometry не исчезают; original logical slots A/Y не уничтожены |
| A20 | Во время Edit mesh bucket migration + swap-remove другого slot | Suppression сохраняется; stable handles/reverse lookup корректны; stale lease не показывает новую generation slot |
| A21 | Hidden owner до Enter; затем Cancel | Исходная hidden policy сохраняется; cleanup не вызывает ShowOwner безусловно |
| A22 |100 fixed-drag Updates |0 source I/O,0 closure proof,0 AssetRegistry-tag queries,0 unrelated owner writes; нет per-frame physics/nav у edit projection |
| A23 | Sealed/edit no-op под RHI; Game View; второй viewport | Нет doubled geometry, z-fighting, missing appearance/selection; source materials не dirty от visual isolation; dimming claim соответствует фактическому adapter |

## Commit и lifecycle

| ID | Действие | Обязательное ожидание |
|---|---|---|
| A24 | Shared Save C | Пишется C, а не весь root из resident leaves;4 потребителя обновляются; unchanged parent recipes не перекомпилированы; unloaded world не загружен |
| A25 | Source file readonly/no directory/write-failure до записи | Сессия остаётся пригодной; draft не исчезает; исправить путь/право и Retry возможно; source byte-for-byte прежний |
| A26 | Файл записан, import/reconcile отказал | UI сообщает SourceCommitted, сохраняет recovery-draft; не обещает, что Cancel вернёт старый source |
| A27 | Unique scopes, procedural/Bake, сбой на второй созданной копии | Правильная область изменений; binding перенесён последним; partial-files перечислены; прежние ресурсы не удалены; RNG/appearance предупреждения сохранены |
| A28 | Save/autosave карты, PIE/SIE/cook, root delete, world change, shutdown | Temporary entities не сохранены/не исполняются; cleanups идемпотентны; no late spawn при teardown; dirty session разрешена до PIE или вход остановлен поддержанным механизмом |
| A29 | Save/Discard при невалидном/stale host или reimport recipe | Нет use-after-free или source-публикации не того definition; восстановлен context/selection; accepted Blender overwrite-policy не заменена скрытым CAS |

## Дополнительные guards

No-op Save: никакого нового file mtime, asset notification, parent recompile или source write только из-за входа в Edit. Ошибка входа не оставляет скрытый sealed root. Existing native Packed Level/Level Instance Edit не ломается после регистрации/деактивации MH mode. Переключение native/MH modes сначала разрешает dirty draft, не активирует два writable scope.

Поддержка внешних ресурсов/типов ограничена Source v5 и действующими capabilities. Unsupported actor class, unmanaged mesh, world partition context вне подтверждённой области, reference-cycle — диагностируемый отказ до разрушительных действий. Не заменять отсутствующий resource на empty.

## Доказательства, которые приносит PR

Автоматизированный журнал с version/SHA и именами тестов, фактические before/after source bytes, dirty package list, actor/component/handle counts и счётчики work. Для UI/RHI — короткая запись взаимодействий или набор screenshots с шагами и expected state; сами картинки не заменяют backend assertions. Измерения выполняются на указанном host, отдельно cold и warm, до/после на одной сцене. Existing receipts проекта не считаются свежим результатом CE.

## Недопустимые критерии приёмки

«Кнопка появилась», «на кубах двигается», «ничего не упало», «NullRHI зелёный», «используем тот же Seed», «объекты transient — значит не сохраняются», «каждый файл atomic — значит batch atomic» и «у меня закрытый Outliner, поэтому selection не обязан работать» не доказывают выполнение этой задачи.


---

# Карта источников и границы исследования

**Статическое чтение C++; не runtime/compile verification.** Проверено актуальное состояние default branch MH и Dagor и зафиксированы SHA. Ниже указаны прочитанные релевантные участки, а не заявление о полном аудите многомиллионного репозитория. Все ссылки закреплены на конкретной версии.

## Важное ограничение Unreal

Подключение к `EpicGames/UnrealEngine` вернуло 404. C++ механизма изучен через доступную публичную копию `Arkfall-Development/UnrealEngine`. Её `Build.version` сообщает 5.8.2; соответствие каждого файла неизменённому Epic upstream не удостоверялось. Официальная документация Epic использована для независимой сверки назначений и публичных API. Локальная лицензированная UE 5.7.4 остаётся окончательным основанием для конкретной реализации: обязательный CE-0 проверяет файлы, сигнатуры, экспорт символов и поведение. Пакет не содержит перепечатки исходников Epic.

## C++ и проектные документы

### [D1] `prog/tools/sceneTools/daEditorX/services/compositMgr/compositMgrService.cpp`
CompositEntity / CompositEntityPool; loadAssetData, setTm, seed handoff, asset change, interactive move.
[Зафиксированный источник](https://github.com/GaijinEntertainment/DagorEngine/blob/75723669297e48e200a0dc67b18c1629e0975daf/prog/tools/sceneTools/daEditorX/services/compositMgr/compositMgrService.cpp)

### [D2] `prog/tools/AssetViewer/Entity/compositeEditor.cpp`
begin/end, enterSubCompositeEditing, Save Shared, saveSubCompositeAsUnique, pending reference swap, context/camera; просмотрены 1–505.
[Зафиксированный источник](https://github.com/GaijinEntertainment/DagorEngine/blob/75723669297e48e200a0dc67b18c1629e0975daf/prog/tools/AssetViewer/Entity/compositeEditor.cpp)

### [D3] `prog/tools/AssetViewer/Entity/compositeEditorTreeData.cpp`
DataBlock↔tree; последовательные dataBlockId и соответствие loader.
[Зафиксированный источник](https://github.com/GaijinEntertainment/DagorEngine/blob/75723669297e48e200a0dc67b18c1629e0975daf/prog/tools/AssetViewer/Entity/compositeEditorTreeData.cpp)

### [D4] `prog/tools/AssetViewer/Entity/compositeEditorGizmoClient.cpp`
world/local/parent frame; multiselect ancestor filtering; changed() и inverse parent; просмотрены 1–270.
[Зафиксированный источник](https://github.com/GaijinEntertainment/DagorEngine/blob/75723669297e48e200a0dc67b18c1629e0975daf/prog/tools/AssetViewer/Entity/compositeEditorGizmoClient.cpp)

### [D5] `prog/tools/AssetViewer/Entity/compositeEditorTreeDataNode.cpp`
canTransform, matrix-vs-p2 capability; создание node/ent.
[Зафиксированный источник](https://github.com/GaijinEntertainment/DagorEngine/blob/75723669297e48e200a0dc67b18c1629e0975daf/prog/tools/AssetViewer/Entity/compositeEditorTreeDataNode.cpp)

### [D6] `prog/tools/AssetViewer/Entity/compositeEditorViewport.cpp`
pixel-perfect hits, entity→dataBlockId→tree, double-click child entry.
[Зафиксированный источник](https://github.com/GaijinEntertainment/DagorEngine/blob/75723669297e48e200a0dc67b18c1629e0975daf/prog/tools/AssetViewer/Entity/compositeEditorViewport.cpp)

### [D7] `prog/tools/sceneTools/daEditorX/include/de3_composit.h`
Редакторский интерфейс composit, placement policies.
[Зафиксированный источник](https://github.com/GaijinEntertainment/DagorEngine/blob/75723669297e48e200a0dc67b18c1629e0975daf/prog/tools/sceneTools/daEditorX/include/de3_composit.h)

### [U0] `Engine/Build/Build.version`
Публичная копия идентифицирует себя как UE 5.8.2, не 5.7.4.
[Зафиксированный источник](https://github.com/Arkfall-Development/UnrealEngine/blob/acba8e55e81c70351d961ebbb71a492ac80fea50/Engine/Build/Build.version)

### [U1] `Engine/Source/Runtime/Engine/Private/PackedLevelActor/PackedLevelActor.cpp`
OnEdit, OnCommit, OnEditChild, OnCommitChild, IsHiddenEd, WorldAsset и builder; просмотрены 1–290.
[Зафиксированный источник](https://github.com/Arkfall-Development/UnrealEngine/blob/acba8e55e81c70351d961ebbb71a492ac80fea50/Engine/Source/Runtime/Engine/Private/PackedLevelActor/PackedLevelActor.cpp)

### [U2] `Engine/Source/Runtime/Engine/Private/LevelInstance/LevelInstanceSubsystem.cpp`
EditLevelInstanceInternal, CommitLevelInstanceInternal, FLevelInstanceEdit, save-failure, ID reacquire, context Push/Pop; изучены соответствующие участки 1–230,1300–1650,1850–2230,2330–2900.
[Зафиксированный источник](https://github.com/Arkfall-Development/UnrealEngine/blob/acba8e55e81c70351d961ebbb71a492ac80fea50/Engine/Source/Runtime/Engine/Private/LevelInstance/LevelInstanceSubsystem.cpp)

### [U3] `Engine/Source/Runtime/Engine/Private/LevelInstance/LevelInstanceEditorLevelStreaming.cpp`
Load/Unload source level, transform/pivot, actor added events, editing proxies.
[Зафиксированный источник](https://github.com/Arkfall-Development/UnrealEngine/blob/acba8e55e81c70351d961ebbb71a492ac80fea50/Engine/Source/Runtime/Engine/Private/LevelInstance/LevelInstanceEditorLevelStreaming.cpp)

### [U4] `Engine/Source/Editor/LevelInstanceEditor/Private/LevelInstanceEditorMode.cpp`
UEdMode, Enter/Exit, command routing, selection/edit restrictions, native flags; просмотрены 1–430.
[Зафиксированный источник](https://github.com/Arkfall-Development/UnrealEngine/blob/acba8e55e81c70351d961ebbb71a492ac80fea50/Engine/Source/Editor/LevelInstanceEditor/Private/LevelInstanceEditorMode.cpp)

### [U5] `Engine/Source/Editor/LevelInstanceEditor/Private/LevelInstanceEditorModeToolkit.cpp`
FModeToolkit viewport overlay Save/Cancel, CanExitEdit.
[Зафиксированный источник](https://github.com/Arkfall-Development/UnrealEngine/blob/acba8e55e81c70351d961ebbb71a492ac80fea50/Engine/Source/Editor/LevelInstanceEditor/Private/LevelInstanceEditorModeToolkit.cpp)

### [U6] `Engine/Source/Runtime/Engine/Private/LevelInstance/LevelInstanceEditorObject.cpp`
CanDiscard, OnMoveActorsToLevel, save notifications и committed changes.
[Зафиксированный источник](https://github.com/Arkfall-Development/UnrealEngine/blob/acba8e55e81c70351d961ebbb71a492ac80fea50/Engine/Source/Runtime/Engine/Private/LevelInstance/LevelInstanceEditorObject.cpp)

### [U7] `Engine/Source/Runtime/Engine/Private/PackedLevelActor/PackedLevelActorBuilder.cpp`
source level→clusters→builders; WorldAsset, relative pivot, packing; просмотрены 1–260.
[Зафиксированный источник](https://github.com/Arkfall-Development/UnrealEngine/blob/acba8e55e81c70351d961ebbb71a492ac80fea50/Engine/Source/Runtime/Engine/Private/PackedLevelActor/PackedLevelActorBuilder.cpp)

### [M0] `docs/RECIPE_EXECUTION_STATUS.md`
Актуальные MERGED R5/R6, owner overwrite policy 2026-09-06, PLANNED R6-O/R7.
[Зафиксированный источник](https://github.com/helmdubo/MH_blender_bridge/blob/19b75153eb2d690ba4fc3999e5a8b4aedf77e040/docs/RECIPE_EXECUTION_STATUS.md)

### [M1] `ue/MimirComposite/Source/MimirCompositeEditor/Public/Composite/MHCompositeLevelSubsystem.h`
Текущий session facade, EditingDocument, GetEditingDraft, nested/shared/Unique APIs.
[Зафиксированный источник](https://github.com/helmdubo/MH_blender_bridge/blob/19b75153eb2d690ba4fc3999e5a8b4aedf77e040/ue/MimirComposite/Source/MimirCompositeEditor/Public/Composite/MHCompositeLevelSubsystem.h)

### [M2] `ue/MimirComposite/Source/MimirCompositeEditor/Private/Composite/MHCompositeActor.cpp`
FindSessionHandleForNodePath, SyncEditScopeHandles, SetPlacementEditMode, Tick, PostEditUndo; просмотрены 1–630 и1130–конец.
[Зафиксированный источник](https://github.com/helmdubo/MH_blender_bridge/blob/19b75153eb2d690ba4fc3999e5a8b4aedf77e040/ue/MimirComposite/Source/MimirCompositeEditor/Private/Composite/MHCompositeActor.cpp)

### [M3] `ue/MimirComposite/Source/MimirCompositeEditor/Private/Composite/MHCompositeLevelSubsystem.cpp`
Begin/Commit и nested Commit; PublishDefinition/RestoreDefinition; Unique selectors/Bake; просмотрены650–1230.
[Зафиксированный источник](https://github.com/helmdubo/MH_blender_bridge/blob/19b75153eb2d690ba4fc3999e5a8b4aedf77e040/ue/MimirComposite/Source/MimirCompositeEditor/Private/Composite/MHCompositeLevelSubsystem.cpp)

### [M4] `ue/MimirComposite/Source/MimirCompositeEditor/Private/MimirCompositeEditorModule.cpp`
Selection adapter startup, global edit key processor registration; просмотрены1–235.
[Зафиксированный источник](https://github.com/helmdubo/MH_blender_bridge/blob/19b75153eb2d690ba4fc3999e5a8b4aedf77e040/ue/MimirComposite/Source/MimirCompositeEditor/Private/MimirCompositeEditorModule.cpp)

### [M5] `ue/MimirComposite/Source/MimirCompositeEditor/Private/UI/MHEditSessionKeys.cpp`
SViewport-based global preprocessor, Escape Cancel, next-tick Apply без captured session.
[Зафиксированный источник](https://github.com/helmdubo/MH_blender_bridge/blob/19b75153eb2d690ba4fc3999e5a8b4aedf77e040/ue/MimirComposite/Source/MimirCompositeEditor/Private/UI/MHEditSessionKeys.cpp)

### [M6] `ue/MimirComposite/Source/MimirCompositeEditor/Public/Composite/MHInstancePool.h`
Readonly ISM manager, handles, owner visibility, pool lifecycle и отсутствие per-scope suppression API.
[Зафиксированный источник](https://github.com/helmdubo/MH_blender_bridge/blob/19b75153eb2d690ba4fc3999e5a8b4aedf77e040/ue/MimirComposite/Source/MimirCompositeEditor/Public/Composite/MHInstancePool.h)

### [M7] `README.md`
Target stock UE 5.7.4, три плоскости, агентские build/test gates; README имеет старые summary-абзацы, статус работ берётся из tracker и кода.
[Зафиксированный источник](https://github.com/helmdubo/MH_blender_bridge/blob/19b75153eb2d690ba4fc3999e5a8b4aedf77e040/README.md)

## Официальная документация Epic

HTML API может переключаться на актуальную версию сайта; это не frozen header целевой 5.7.4.

- **[API1]** UEdMode, публичные selection/edit restrictions и ProcessEdit*: [документация Epic](https://dev.epicgames.com/documentation/unreal-engine/API/Editor/UnrealEd/UEdMode).
- **[API2]** Level Instancing: source level и Packed Level Blueprint; общий характер правок: [документация Epic](https://dev.epicgames.com/documentation/en-us/unreal-engine/level-instancing-in-unreal-engine).
- **[API3]** IActorEditorContextClient; интерфейс контекстного UI/action: [документация Epic](https://dev.epicgames.com/documentation/unreal-engine/API/Editor/UnrealEd/IActorEditorContextClient).
- **[API4]** FPackedLevelActorBuilder; интерфейс packing/update: [документация Epic](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/FPackedLevelActorBuilder).
- **[API5]** ULevelInstanceSubsystem; edit/commit и ancestor APIs: [документация Epic](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/ULevelInstanceSubsystem).

## Как классифицировать утверждения

Факты об имеющемся MH подтверждены исходниками M1–M6 и tracker M0. Факты об Unreal ограничены версией публичной копии U0 и перечисленными участками. Имена `UMHCompositeEditSession`, `AMHCompositeEditNodeActor`, suppression leases, новые CE-тесты и срезы — предлагаемая архитектура, а не найденные в baseline реализации. Проблема неправильного позднего Apply описана как отсутствие guard плюс сценарий для воспроизведения; она не выдаётся за уже воспроизведённую ошибку. Численные performance-результаты в исследовании не измерялись.
