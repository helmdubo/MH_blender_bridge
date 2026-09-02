> Status: REFERENCE · External audit of Recipe Model v1 (2026-09-02) · Not normative; its 12 requirements are incorporated into KICKOFF_PROMPT.md v2 and docs/16_recipe_model.md

Итоговая оценка

dagor_composit_research.md — сильное инженерное исследование. Его главная модель подтверждается и исходниками, и официальной документацией Dagor: composite — это редакторский рецепт, а не самостоятельная runtime-сущность; в уровень уходят отдельные листья. 

KICKOFF_PROMPT.md — уже не исследование, а архитектурная директива. Направление в целом правильное, но документ пока слишком агрессивно превращает удачные наблюдения о Dagor в обязательные решения для UE5. В нём есть несколько внутренних противоречий и несколько UE-специфических рисков, которые могут создать новую волну регрессий.

Моя позиция:

Переход на «рецепт + исполнитель» нужно принимать.
KICKOFF_PROMPT.md в текущем виде не следует отдавать исполнителям как единственный норматив без правок.

Самая важная корректировка:

Нужно убрать applied state, receipt-проверки и подписи из горячего пути загрузки карты, но не удалять доказательную инфраструктуру из системы вообще.

Подпись сама по себе не является проблемой. Проблема — когда карта должна синхронно построить и проверить её до первого кадра.

⸻

1. Что исследование Dagor разобрало действительно хорошо

Компиляция на ассет, а не на placement

Исследователь правильно выделил три уровня:

service
→ CompositEntityPool на один composit-ассет
→ лёгкий CompositEntity на одно размещение

Пул хранит скомпилированный рецепт, а placement — только матрицу, два сида, индекс собственного диапазона листьев и несколько флагов. Листья лежат не внутри composite entity, а в общем плоском пуле. dagor_composit_research.md

Это именно тот инвариант, которого не хватало ранним версиям MimirComposite:

один и тот же composite используется 100 раз
→ рецепт разбирается и допускается один раз
→ 100 раз выполняется только дешёвое разрешение сидов и трансформов

Плоская программа вместо рекурсивного UObject-графа

Dagor разворачивает BLK-дерево в DFS-массив компонентов и кодирует иерархию интервалами. Во время материализации идёт линейный проход со стеком родительских матриц, а не рекурсивное создание объектов. dagor_composit_research.md

Это хороший референс для FMHCompiledRecipe.

Но переносить нужно сам принцип:

flat arrays
integer indices
resource handles
contiguous subtree ranges

а не буквально все структуры Dagor.

Сиды задаются до построения вложенного composite

Очень ценная находка — PendingCloneSeeds. Dagor специально передаёт сид вложенному composite до клонирования, чтобы не построить поддерево с дефолтным сидом и затем не пересобрать его ещё раз. dagor_composit_research.md

Mimir здесь уже движется правильно: layout seed передаётся сверху вниз во время resolve. Это нельзя потерять при введении compiled recipe.

Локальное обновление ресурсов

Исследование правильно показывает две разные операции:

изменился recipe composite
→ перестраиваются его собственные instances
изменился rendInst-лист
→ ресурс заменяется in place
→ composites не пересобираются

dagor_composit_research.md

Это непосредственно связано с вашим восьмиминутным reimport: Mimir сейчас реагирует на изменение mesh как на изменение всей композиционной структуры, хотя чаще всего изменился только payload уже существующего endpoint.

Ленивая materialization листьев

У rendInst ресурс и тяжёлое render-представление создаются лениво, вплоть до попадания в видимую область. dagor_composit_research.md

Mimir не обязан копировать visibility-rect механику Dagor, но обязан забрать принцип:

невыбранная random-ветка
→ не загружается
выбранный, но ещё не готовый endpoint
→ не блокирует map load

Разделение editor и runtime

Исследование не смешивает editor composite с daNet ECS-вариантом. В editor composite — рецепт для расстановки; в daNet существует отдельный runtime executor, который также спавнит дочерние сущности из рецепта. dagor_composit_research.md

Это подтверждает, что ваша текущая идея runtime bridge не противоречит модели Dagor.

⸻

2. Где исследование немного переобобщает Dagor

Поведение rendInst нельзя автоматически переносить на все UE Actor-листья

Исследование доказывает дешёвое in-place обновление для rendInst. Но из этого не следует, что произвольный:

Blueprint Actor
Light
PostProcessVolume
interactive gameplay actor

можно безопасно:

* пулить по классу;
* переиспользовать;
* менять in place;
* спавнить как чистый endpoint без побочных эффектов.

UE Actor может иметь:

* Construction Script;
* editor callbacks;
* BeginPlay;
* компоненты с собственным состоянием;
* уникальные параметры;
* ссылки на уровень;
* сетевую репликацию;
* latent actions.

Поэтому static-mesh backend и actor backend должны оставаться разными.

Parent composite в UE не сможет обновиться «магически», как в Dagor

В Dagor родительский composite хранит leaf-ссылку на живой дочерний CompositEntity. Дочерний пул перестраивает своё поддерево, а parent pointer остаётся валиден, поэтому родители не уведомляются. dagor_composit_research.md

В предлагаемом Mimir world pool родитель в конечном счёте хранит плоский набор leaf handles. Если вложенный recipe изменился, старые листья сами собой не заменятся.

Следовательно, для UE допустимо:

перекомпилировать только изменившийся child recipe
→ найти placements, которые его инстанцируют
→ rematerialize только соответствующий subtree

Но не нужно:

перекомпилировать все parent recipes
→ полностью rebuild всех parent actors

Исследование верно обнаружило инвариант локальности, но предлагаемый им fork пока не доводит эту локальность до конца.

«Сотни полных обходов на 240 акторах» зависит от числа уникальных roots

В текущем Mimir уже существует shared definition cache. Поэтому 240 placements одного root не должны строить closure 240 раз. Стоимость ближе к:

число уникальных root composite definitions

а не:

число AMHCompositeActor

Проблема всё равно серьёзная: один и тот же leaf может повторно admission-иться в closure разных roots. Но формулировка исследования немного завышает worst case.

⸻

3. Сильные стороны KICKOFF_PROMPT.md

Правильное минимальное сохраняемое состояние актора

Идея оставить в actor только:

CompositeAsset
Seed
AppearanceSeed
NodeOverrides

а материализацию держать transient — хорошая. KICKOFF_PROMPT.md

Это намного ближе к Dagor, где уровень хранит имя composite, матрицу и два сида, а листья при загрузке создаются заново. dagor_composit_research.md

Prototype registry по детерминированному пути

Запрет на:

полный Asset Registry tag scan для каждого endpoint
FAssetData(&Object)
live GetAssetRegistryTags в hot path

правильный.

Разрешение по каноническому object path и однократная проверка embedded receipt — хороший путь. KICKOFF_PROMPT.md

Placeholder вместо блокировки всего composite

Для editor preview подход:

endpoint missing/loading/invalid
→ placeholder
→ warning

намного удобнее текущего:

один отсутствующий leaf
→ весь composite actor не материализуется

Build preflight при этом может оставаться строгим.

Это верное разделение editor availability и build correctness.

Отделение Source-линии от Recipe-линии

S0–S2 хорошо сформулированы:

* metadata filter по size/mtime;
* не парсить FBX при обычном скане;
* targeted reimport не должен делать FullScan.

KICKOFF_PROMPT.md

Это независимая от recipe model оптимизация, и её действительно можно вести параллельно.

Red-first и receipts

Требование отдельных PR, red/green evidence и полевых замеров полезно. Для такого рефакторинга оно особенно важно.

⸻

4. Главные противоречия внутри KICKOFF

4.1 Resolver «не трогать», но RawHashes получать только на выходе

Документ одновременно утверждает:

1. MHResolveCompositePlan и MHBuildRandomSourceClosure остаются неизменными. KICKOFF_PROMPT.md
2. FMHRandomSourceGraph по-прежнему собирается для resolver.
3. RawHashes будут заполняться лениво из prototype receipts «только для точек выхода». KICKOFF_PROMPT.md
4. MHMaterialize использует прежний resolver. KICKOFF_PROMPT.md

Но текущий resolver строит full source closure до обхода layout. Closure требует hashes всего замыкания, включая невыбранные варианты.

Получается невозможный цикл:

editor materialization вызывает resolver
→ resolver требует full closure hashes
→ hashes разрешаются только в exit points
→ editor materialization не является exit point

Как исправить

Нужно фазово разделить resolver, не меняя его доказанную семантику:

MHResolveCompositeLayout(...)
MHResolveCompositeAppearance(...)
MHBuildCompositeProof(...)

Существующий публичный:

MHResolveCompositePlan(...)

становится wrapper:

Layout
→ Appearance
→ Full Closure
→ Signatures

Editor preview вызывает только:

Layout + Appearance

Build preflight, runtime snapshot и export вызывают полный wrapper.

Так сохраняются:

* golden parity;
* единая выборка random;
* единая transform math;
* существующие signatures;

но map load больше не зависит от full closure proof.

Это лучше, чем писать второй независимый random resolver.

⸻

4.2 Receipt проверяется «только на выходе» и одновременно «в prototype registry»

В §3.2 написано, что registry один раз за сессию проверяет:

LogicalName
SourceRelativePath
SourceHash
object path

KICKOFF_PROMPT.md

Но §3.6 говорит:

Только в exit points читаются SourceHash/AppliedHash.

KICKOFF_PROMPT.md

Оба утверждения одновременно неверны.

Правильное разделение

Нужно различать два уровня доказательства.

Endpoint identity admission — при первом использовании prototype:

объект находится по каноническому пути
embedded receipt существует
LogicalName совпадает
ImporterVersion поддерживается
receipt structurally valid

Source freshness proof — только в exit points/background audit:

embedded SourceHash
vs
актуальный ProjectIndex / source payload

Тогда editor гарантирует, что не показывает случайный чужой asset, но не блокирует работу из-за того, что внешний FBX недавно изменился.

⸻

4.3 Изменение child composite: KICKOFF снова предлагает перестраивать родителей

KICKOFF предписывает:

reimport nested composite
→ recompile recipe этого asset
→ recompile recipes, которые на него ссылаются
→ rebuild placements этих recipes

KICKOFF_PROMPT.md

Это противоречит главному инварианту, найденному в Dagor: child pool обновляется локально, parent recipe не пересобирается. dagor_composit_research.md

Для Mimir лучше

child recipe Revision++
→ parent compiled recipes остаются прежними и продолжают ссылаться на child handle
→ SeedAffectsResult upstream cache инвалидируется
→ placements с конкретной child invocation rematerialize её subtree

Полный root rematerialize допустим как первая реализация, но parent recipe recompilation не нужна.

⸻

4.4 Во время drag KICKOFF предлагает не двигать листья

В исследовании Dagor тяжёлые действия действительно откладываются во время gizmo drag, но сами transforms листьев продолжают обновляться. Откладываются:

* collision placement;
* tiled-scene/grid update;
* тяжёлый flush.

dagor_composit_research.md

KICKOFF говорит:

drag gizmo
→ накапливать
→ применять только в PostEditMove(bFinished)

KICKOFF_PROMPT.md

При world-level ISM это означает, что composite actor будет двигаться, а его геометрия визуально останется на старом месте до отпускания мыши.

Нужная семантика

каждый editor frame во время drag:
    обновить instance transforms
    без collision/nav/snapping rebuild
    без отдельного MarkRenderStateDirty на каждый instance
в конце drag:
    один physics/nav/grid refresh
    snapping pass
    bounds finalization

То есть нужен аналог Dagor BeginBulk/EndBulk, а не заморозка визуального движения.

⸻

4.5 Исследование предлагает pool per level/cell, KICKOFF — один на world

Исследование осторожно предлагает:

один HISM/ISM bucket на level
или на World Partition cell

dagor_composit_research.md

KICKOFF уже жёстко фиксирует:

один UInstancedStaticMeshComponent на descriptor на весь world

KICKOFF_PROMPT.md

Это риск для:

* World Partition streaming;
* Data Layers;
* level visibility;
* per-level save/unload;
* actor hidden state;
* editor isolation;
* HLOD;
* copy/paste между levels.

Нужен pool domain

Минимум:

FPoolDomainKey
{
    UWorld* World;
    ULevel* OwningLevel;
    WorldPartitionCellId;
    DataLayerSet;
    FISMDescriptor;
}

На первом этапе достаточно pool per ULevel, а WP-cell и Data Layers добавить после полевого теста.

⸻

4.6 Blanket-удаление ClosureHash и signatures противоречит frozen resolver

KICKOFF требует удалить из активных документов и модели:

ResolvedSignature
CompactResolvedState
PlacementDependencies
ClosureHash
AppliedGraph

и даже вводит grep-гейт на ноль упоминаний ClosureHash. KICKOFF_PROMPT.md

Одновременно документ объявляет:

* random resolver неизменным;
* runtime bridge неизменным;
* runtime snapshot admission неизменным.

KICKOFF_PROMPT.md

Это чрезмерная реакция.

Что действительно надо удалить

Удалить нужно:

signature как authority actor preview
signature как условие загрузки карты
signature как причина rebuild placement

Но сохранить:

ClosureHash в reference resolver
ResolvedSignature в golden/parity тестах
proof в runtime snapshot
proof в build preflight
proof в export

Правильная формулировка:

Signatures remain proof artifacts, but cease to be editor placement state.

Не следует запрещать сам термин ClosureHash в активной документации.

⸻

4.7 AppearanceSeed и position-derived instSeed не согласованы

Actor должен хранить:

AppearanceSeed
bAutoAppearanceSeed

KICKOFF_PROMPT.md

Но materializer затем должен вычислять InstSeed через FNV от позиции composite, если seed не задан явно. KICKOFF_PROMPT.md

Не определено:

* считается ли bAutoAppearanceSeed=true отсутствием явного seed;
* будет ли appearance меняться при перемещении actor;
* сохраняется ли текущая Mimir-семантика независимого appearance reroll;
* что происходит с существующими уровнями.

Dagor действительно использует position-derived default instSeed, но Mimir уже имеет собственный explicit dual-seed контракт. Переносить этот аспект автоматически нельзя.

Я бы сохранил текущую Mimir-семантику:

AppearanceSeed — явное сохраняемое значение
перемещение actor его не меняет

А Dagor-compatible position seed оформить как отдельную opt-in policy позже.

⸻

5. Критические UE-риски, которых в обоих документах почти нет

NodeOverrides по одному NodePath небезопасны

Текущий NodePath индексный:

nodes[2]/children[1]/options[0]

При вставке нового sibling все следующие paths сдвигаются.

Документ предусматривает только случай:

path исчез
→ orphan warning

Но более опасный сценарий:

path всё ещё существует
→ теперь это другой узел
→ старый override молча применяется не туда

Минимальная защита

Ключ override должен содержать:

struct FMHNodeOverrideKey
{
    FString NodePath;
    uint64 SourceNodeFingerprint;
};

Fingerprint минимум покрывает:

semantic kind
resource key
display label
authored local transform
parent fingerprint

Если path найден, но fingerprint не совпал:

override не применять
→ MH_W_ORPHAN_OVERRIDE_IDENTITY_CHANGED

Без этой защиты NodeOverrides нельзя считать production-ready.

Generic Actor pooling слишком рискованный

В KICKOFF Actor-листья должны спавниться через pool по классу и иметь:

RF_Transient
bIsEditorOnlyActor = false

KICKOFF_PROMPT.md

Для editor preview это почти наверняка неверная политика.

Preview actor должен быть:

editor-only
transient
duplicate-transient
не попадать в cook
не переноситься автоматически в PIE

Runtime actor должен создаваться отдельно runtime bridge.

Кроме того, arbitrary Blueprint actor нельзя по умолчанию пулить. Начальная политика должна быть:

StaticMesh leaves:
    pool
Actor leaves:
    spawn one transient editor-preview actor per selected leaf
    только из whitelist / ActorClassRegistry
    без reuse между placements

Позже можно добавить интерфейс:

IMHCompositePoolableActor

для классов, которые явно подтверждают безопасный reset/reuse.

Mesh reimport всё равно требует bucket-level reconcile

Правильно не делать:

mesh reimport
→ full Materialize
→ full actor rebuild

Но недостаточно просто увеличить Prototype.Revision.

Reimport может изменить:

* material slot count/order;
* default materials;
* LOD count;
* section count;
* section flags;
* collision;
* BodySetup;
* trace companion;
* bounds.

Поэтому нужен PlacementInterfaceHash:

material slots
section policies
collision contract
trace companion identity
descriptor-relevant properties

Поведение:

geometry payload changed, interface same
    → никакого placement rebuild
    → render resource refresh only
interface changed
    → migrate только buckets этого mesh
collision changed
    → recreate physics state только соответствующих buckets

Материалы и текстуры отсутствуют в update protocol

Документы подробно описывают reimport:

* composite;
* mesh;
* seed.

Но почти не описывают:

* Material Instance reimport;
* texture reimport;
* Physical Material change;
* изменение parent material;
* изменение per-section phmat mapping.

Для вашего проекта это нельзя оставить неоговорённым.

Базовая семантика должна быть:

texture payload reimport in place
    → no recipe rebuild
    → no placement rematerialization
MI scalar/vector/texture parameters changed in place
    → no recipe rebuild
material object identity / slot binding changed
    → bucket descriptor reconcile
physical material mapping changed
    → collision/trace material interface reconcile

InstanceIndex не может быть публичным стабильным handle

При удалении ISM instance Unreal может переместить последний instance в освободившийся индекс.

Следовательно:

FMHInstanceHandle = (Component*, InstanceIndex)

не является стабильным handle.

Нужна индирекция:

FMHInstanceHandle
{
    BucketId;
    SlotId;
    Generation;
}

А bucket хранит:

SlotId → current ISM index
ISM index → SlotId

При swap-remove reverse mapping обновляется.

Без этого после удаления одного leaf:

* Outliner выберет чужой узел;
* undo восстановит неправильный instance;
* actor handles начнут указывать не туда.

World pool должен учитывать visibility ownership

Один ISM содержит instances от многих composite actors. Но пользователь может:

* скрыть один actor;
* выключить Data Layer;
* unloaded одну cell;
* изолировать selection;
* сделать actor editor-only;
* удалить один level.

У pool subsystem должна быть group operation:

HideOwner
ShowOwner
RemoveOwner
MoveOwner
SetOwnerEditorVisibility

Обычный SetVisibility() на ISM-компоненте для этого непригоден: он скроет все placements bucket.

⸻

6. Риск для golden parity: нормализация весов и схлопывание матриц

KICKOFF переносит из Dagor:

* нормализацию weights при компиляции;
* схлопывание узлов без deviation в matrix.

KICKOFF_PROMPT.md

Это выглядит разумно, но текущий Mimir random resolver имеет замороженные float/double boundaries и golden vectors.

Математически:

1, 2, 3

и:

1/6, 2/6, 3/6

эквивалентны.

В floating point они не обязаны давать одинаковую границу выбора для каждого RawU32.

То же касается предварительной matrix composition: она может изменить округление world transform и signatures.

Поэтому для первой версии FMHCompiledRecipe я бы хранил:

исходные weights без нормализации
canonical TRS без предварительного схлопывания

А оптимизации добавлял только после exhaustive parity test:

старый reference resolver
vs
compiled executor

на:

* всех golden vectors;
* extreme weights;
* очень малых weights;
* длинной вложенности;
* non-uniform scale;
* отрицательных rotation values;
* почти граничных random draws.

⸻

7. Как я бы скорректировал целевую архитектуру

UMHCompositeAsset
    authoring recipe
        │
        ▼
FMHCompiledRecipeRegistry
    one flat recipe program per asset
    no StaticMesh package loads
    no closure proof
        │
        ├──────────────────────┐
        ▼                      ▼
MHMaterializeLayout       MHBuildCompositeProof
fast editor path          preflight/runtime/export
no hashes required        full closure + receipts + signatures
        │
        ▼
UMHEndpointPrototypeRegistry
    exact path
    embedded identity receipt
    async selected endpoint loading
        │
        ▼
UMHInstancePoolSubsystem
    scoped by Level/WP domain
    stable generation handles
        │
        ▼
AMHCompositeActor
    asset + seeds + safe overrides

Ключевой принцип:

Preview plane

быстрый
доступный
placeholder-friendly
не блокирует карту
не сравнивает Source Root

Proof plane

строгий
full closure
SourceHash / AppliedHash
build-blocking
runtime snapshot admission

Source plane

incremental filesystem index
background freshness
targeted reimport

Сейчас KICKOFF пытается частично смешать preview и proof planes, поэтому появляются противоречия вокруг RawHashes и resolver.

⸻

8. Как изменить программу срезов

Я бы не начинал с полного уничтожения прежнего норматива и тестов.

D0a — новый активный ADR, без семантической чистки кода

Создать docs/16_recipe_model.md и явно написать:

Status: Proposed / implementation in progress
Active architecture direction
Legacy placement implementation remains until R2

Старые документы можно сразу переместить в archive и снабдить шапкой HISTORY. Это решит проблему внешних агентов.

Но не нужно вводить lexical ban на:

ClosureHash
ResolvedSignature

Нужно запрещать только утверждения вида:

actor placement freshness определяется ResolvedSignature
map load обязан построить full closure proof

M0 — instrumentation

Оставить первым кодовым срезом.

R0 — prototype registry, но с двумя уровнями admission

embedded identity receipt
source freshness proof

Не смешивать их.

R1 — убрать all-options compile wait

Переходный безопасный вариант:

recipe строится без тяжёлых endpoint loads
resolve seed
загрузить selected meshes
wait только selected meshes

Это быстро покажет реальную разницу:

517 all-options
vs
N selected

S0–S2 — сразу параллельно

Это уже даёт измеримый выигрыш около 100 секунд и не зависит от recipe refactor.

R2a — compiled recipe + shadow parity

Новый executor пока не становится production path.

На тестах и diagnostic command:

reference resolver
vs
compiled materializer

Сравниваются:

* decisions;
* leaves;
* matrices;
* appearance channels;
* selected dependencies.

R2b — переключение editor preview на fast materializer

Reference resolver остаётся:

* в preflight;
* runtime snapshot;
* export;
* golden tests.

Это не «два production path»: один — preview executor, второй — proof/reference executor.

R3 — semantic resource reconciliation

Не только mesh:

mesh
material
texture
physical material
composite recipe

И не blanket skip, а PlacementInterfaceHash.

R4 — настоящая async-загрузка selected endpoints

Только теперь гарантируется:

cold DDC
→ первый кадр без ожидания package/mesh compilation

R5 — pool per domain

Не сразу один pool на весь world.

Начать с:

one pool subsystem
one pool actor per ULevel

Затем отдельно доказать WP/Data Layer модель.

NodeOverrides — после стабильного locator

Я бы вынес их из R2b.

Сначала закончить:

recipe
fast materialize
endpoint registry
reimport locality

Потом добавить:

NodePath + fingerprint overrides

Иначе performance refactor одновременно становится новой authoring feature.

Actor leaves — после отдельного capability contract

Не generic class pooling.

⸻

9. Что делать с документальной политикой

Мотивация аудитора понятна: старые документы заставляют внешних агентов возвращать устаревшую applied-state модель.

Но правило:

удалить все упоминания термина и тесты старого концепта за один срез

слишком механическое.

Лучше проверять не слова, а нормативный статус.

Например, каждый active document должен иметь:

Status: NORMATIVE
Architecture version: Recipe Model R1
Supersedes: ...

Каждый архивный:

Status: HISTORY
Do not use for implementation
Superseded by docs/16_recipe_model.md

CI может проверять:

* нет ссылок из active docs на archive как на норматив;
* каждый active doc перечислен в docs/NORMATIVE_INDEX.md;
* каждый archive doc имеет HISTORY header;
* новые PR не добавляют нормативные требования в receipts.

Удалять старые тесты следует только после появления replacement tests. Например:

AppliedPlanAdmissionTest удаляется
только когда
PrototypeRegistryIdentityAdmissionTest
+
BuildPreflightFullClosureTest
уже зелёные

Иначе можно убрать концепт вместе с единственной проверкой полезного инварианта.

⸻

Мой окончательный вердикт

dagor_composit_research.md

Принимаю как сильный архитектурный референс.

Особенно ценны:

* recipe pool на ассет;
* плоская compiled representation;
* лёгкий placement state;
* seed-before-construction;
* lazy endpoint materialization;
* in-place leaf update;
* локальность изменений;
* отсутствие composite wrapper в финальном игровом представлении.

Но это исследование Dagor, а не готовая спецификация Unreal. RendInst pool нельзя буквально отождествить с arbitrary Blueprint Actor pool.

KICKOFF_PROMPT.md

Принимаю направление, но не текущую нормативную формулировку.

До выдачи исполнителям нужно исправить как минимум:

1. Разделить fast layout resolver и full proof/signature stage.
2. Разделить embedded receipt identity и source freshness.
3. Не перекомпилировать parent recipes при изменении child.
4. Обновлять visual transforms во время drag, откладывая только тяжёлые side effects.
5. Scope pool по level/WP domain, а не на весь world.
6. Оставить signatures как proof artifacts.
7. Защитить NodeOverrides fingerprint’ом.
8. Не пулить arbitrary Actor classes без capability contract.
9. Сделать preview actors editor-only.
10. Добавить update protocol для materials, textures, phmat и collision interface.
11. Ввести стабильные generation handles для ISM instances.
12. Не нормализовать веса и не схлопывать transforms до доказательства golden parity.

Главная формула целевой системы должна быть такой:

Dagor-подобный быстрый preview executor + Mimir-подобный строгий proof на границах.

Не нужно выбирать между скоростью Dagor и доказуемостью Mimir. Их нужно развести по разным стадиям.
