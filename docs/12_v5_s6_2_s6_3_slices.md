# 12 — Срезы V5-S6.2 и V5-S6.3 (pre-S7)

Статус: **owner freeze, редакция 3**. Аудитория — исполнитель без контекста сессии
и внешний ai-аудитор. До любых правок прочитать целиком
`docs/10_source_protocol_v5_plan.md`, `docs/11_v5_agent_slices.md` и `README.md`.
Все инварианты `11 §Инварианты для всех срезов` действуют без изменений; ниже
они не повторяются, а только уточняются там, где срез добавляет частный случай.

Редакция 3 отличается от редакции 2 в трёх местах, все — следствия
owner-решений, зафиксированных в документе 13: перенумерация срезов;
`placement` объявлен провенансным полем, а не отложенной функцией; из §6 убран
интерфейс мирового размещения. Owner-решения: **обратной записи в Dagor не
будет никогда**, **прижатия в UE5 не будет**, **UE5 — сцена для рендера
портфолио, игровых акторов нет**.

Предшественник — **V5-S6.0** (`docs/receipts/v5_s6_0.md`, PR #29): три
базовых дефекта превью. Shared definition cache вынесен в отдельный кандидат
(`docs/proposals/shared_composite_preview_cache.md`) и в эти срезы не входит.

> **Поправка 2026-08-29.** Canonical-поле узла `placement` заменено
> скалярным `place_type` (док 13, «Поправки owner 2026-08-29» п.2); позиция
> в ратифицированном порядке полей сохраняется. V5-S7 Cook flattening
> отложен owner'ом к v2.0 с обязательными фундамент-инвариантами — см.
> `docs/14_v5_ue_editor_program.md`. Нумерация и содержание S6.2/S6.3 не
> меняются.

Редакция 2 отличается от переданного ранее проекта в четырёх местах, все —
вокруг `place_type`: одноуровневое наследование, приоритет легаси-флагов,
политика «сохранять, но не исполнять» и снятое предложение реализовать
`pivot`-снап внутри S6.2. Owner-решение, на котором это основано: **целевое
состояние — `default` у всех узлов; ненулевые режимы остаются редкими
исключениями.**

Два среза. Они независимы по содержанию, но выполняются последовательно по
инварианту 11 §2: одна ветка, один PR, одна квитанция на срез, мержит owner.

```text
V5-S6.1  dag4blend direct export   документ 13
V5-S6.2  Placement lifecycle       ветка v5/s6.2-placement-lifecycle
V5-S6.3  Pre-S7 freeze             ветка v5/s6.3-pre-s7-freeze
V5-S7    Cook flattening (существующий, не меняется этим документом)
```

---

## 0. Провенанс фактов о Dagor

Все утверждения о поведении Dagor ниже проверены по публичному источнику,
запиненному в `tools/dagor_random_parity_probe.py`
(`DAGOR_SOURCE_COMMIT = 75723669297e48e200a0dc67b18c1629e0975daf`). Исходный код
Dagor в репозиторий не копируется. Якорь — имя функции, не номер строки.

| Факт | Файл | Функция |
| --- | --- | --- |
| PRNG и веса | `prog/gameLibs/publicInclude/gameMath/objgenPrng.h` | `rnd`, `frnd` |
| Прижатие и его распространение на поддерево | `prog/tools/sceneTools/daEditorX/services/compositMgr/compositMgrService.cpp` | `CompositEntityPool::setTm` |
| Семантика `aboveHt` | `prog/tools/sceneTools/daEditorX/include/de3_genObjUtil.h` | `place_on_plane`, `place_on_ground`, `dist_to_ground` |
| Наследование instSeed | `compositMgrService.cpp` | `getSubEntInstSeed`, `loadAssetData` |
| Квантование | `compositMgrService.cpp` | `CompositEntity::setTm`, `getQuantizeTm` |

Два факта, на которых держится V5-S6.2, и их точная формулировка:

1. **Прижатие принадлежит узлу и распространяется на всё его поддерево.**
   В `CompositEntityPool::setTm` мировая матрица `stm` — одна локальная
   переменная на весь обход. Прижатие пишет прямо в неё (`stm.setcol(3, p)`,
   а для нормали и три-точечного режима переписываются и столбцы ориентации),
   после чего та же переменная уходит в `HierIter::iterate` следующей итерации
   как родительский фрейм. Дети считаются от уже прижатой матрицы родителя.
   Дельт, pre/post-снимков и отдельного прохода в Dagor нет.

2. **`aboveHt` — высота старта трассировки, а не смещение.** В
   `place_on_plane` параметр называется `start_above`: точка поднимается на
   `start_above` вдоль нормали, луч идёт вниз на `maxTraceDistance + start_above`,
   и **при промахе точка возвращается ровно на исходное место** — никакого
   fallback-смещения. Дефолт в композите: `aboveHt = placeType ? 5 : 0` метров.

---

## V5-S6.2 — Placement lifecycle

Gate: V5-S6.1 owner-accepted (документ 13, прямой маршрут dag4blend).

Срез **не меняет** формат, грамматику, план, подпись, golden-векторы и коды
диагностики, кроме одного нового `MH_E_*` в п.4. Цель — исправить жизненный
цикл компонентов размещения и две квадратичные операции.

### 1. Регистрация компонентов вне `PostLoad`

Симптом подтверждён полевой проверкой owner: у размещённого композита клик во
вьюпорте не выбирает актор.

Причина: `AMHCompositeActor::PostLoad()` вызывает `RebuildComposite()`, который
доходит до `PlanViewNew` → `RegisterComponent()`. На стадии `PostLoad` актор не
обязан быть добавлен в мир. `OnConstruction` затем уходит в ветку `else`
(условие `!ResolvedPlan.IsValid()` уже не выполняется) и только двигает basis,
не перестраивая. То есть на загрузке уровня живут компоненты, зарегистрированные
в `PostLoad`.

**Инвариант среза.** Компоненты размещения создаются и регистрируются ровно в
одной точке жизненного цикла, и эта точка находится после того, как актор
добавлен в мир и его собственные компоненты зарегистрированы.

Реализация:

1. Убрать `RebuildComposite()` из `PostLoad()`. `PostLoad` оставляет только
   `AttachRootTransformHook()` и пометку «нужна первичная сборка».
2. Первичную сборку выполнять в `PostRegisterAllComponents()`. Если полевой
   тест покажет, что этого недостаточно (актор в World Partition/OFPA,
   отложенная загрузка), допустимый fallback — отложенный тик редактора; выбор
   фиксируется в квитанции с обоснованием.
3. `OnConstruction` **не** делать точкой полной пересборки: он выполняется при
   каждом `PostEditMove`/`PostEditChangeProperty`, и полная пересборка там
   вернёт лаг на каждое перемещение. Он остаётся тем, чем является сейчас, —
   обновлением basis.
4. В `PlanViewNew` порядок довести до канонического: `NewObject` →
   `AddInstanceComponent` → `SetupAttachment` → `SetAbsolute` → конфигурация
   (`SetStaticMesh` / `SetChildActorClass`) → `RegisterComponent`. Сейчас
   `SetStaticMesh` выполняется в `MHCompileCompositePlacementV5` уже после
   `RegisterComponent`. Это не обязательно причина бага, но lifecycle обязан
   быть однозначным.

STOP-условие: если после п.1–п.4 клик по листу по-прежнему не выбирает актор,
срез останавливается и в `docs/QUESTIONS.md` заводится `OPEN-V5-*` с полевым
протоколом (класс компонента, `IsRegistered`, `IsVisible`,
`IsVisualizationComponent`, `bSelectable`, тип и актор hit proxy, флаги объекта)
в сравнении с обычным `UStaticMeshComponent` на тестовом акторе. Кастомный hit
proxy этим срезом **не вводится**: если причина в тайминге регистрации, hit
proxy её не лечит.

### 2. `PlanViewFind` — O(N²)

`MHCompilePlacementV5` для каждого листа линейно сканирует `PreviousComponents`
и сравнивает `ComponentTags`. Композит на 500 листов даёт ~250 000 сравнений
на каждую пересборку.

Построить `TMap<FName, UActorComponent*>` один раз перед циклами (handles и
leaves используют одну карту, ключи уже различаются префиксом `MH.Handle:` /
`MH.Leaf:`). Семантика поиска не меняется: совпадение по тегу **и** по классу.

### 3. `DestroyMHRetiredComponents` — O(N²)

`Current.Contains(Previous[Index])` внутри цикла по `Previous`. Построить
`TSet<TObjectPtr<UActorComponent>>` из `Current` один раз.

### 4. Fail-closed вместо тихого усечения

`MHUpdateCompositePlacementBasis` обновляет массивы по пересечению длин:

```cpp
Index < Handles.Num() && Index < RootDefinition.Nodes.Num()
Index < Leaves.Num()  && Index < Plan.Leaves.Num()
```

При рассинхронизации лишние элементы просто остаются необновлёнными, а функция
возвращает `true`. Это единственное место в плагине, где несогласованность
проходит молча, и оно противоречит инварианту 11 §3.

Требовать точного равенства. При несовпадении — не частичное обновление, а
`MH_E_PLACEMENT_STATE_DESYNC` (новый код; регистрируется в
`addon/mh4blend/core/canonical.py::ERROR_CODES`, в зеркальном C++ registry и в
golden counts тем же срезом) и полная пересборка размещения.

### Acceptance V5-S6.3

1. **Функциональный тест выбора**, не только automation-проверка наличия
   компонента: клик по листу размещённого композита во вьюпорте редактора после
   открытия уровня → `GEditor` selection содержит `AMHCompositeActor`. Протокол
   и результат — в квитанции.
2. Компоненты не создаются и не регистрируются во время `PostLoad`; охранный
   тест это фиксирует.
3. Перемещение актора не вызывает полную пересборку (счётчик пересборок).
4. Рассинхрон массивов даёт `MH_E_PLACEMENT_STATE_DESYNC` и полную пересборку,
   а не частично обновлённую сцену.
5. Число обращений к поиску предыдущих компонентов линейно по числу листьев
   (инструментированный счётчик в тесте).
6. `ResolvedSignature`, choices, samples, world transforms и golden-векторы
   идентичны до и после среза: **байтовое равенство**.
7. Двойной unity gate по 11 §9.

---

## V5-S6.3 — Pre-S7 freeze

Gate: V5-S6.2 owner-accepted.

Цель: заморозить всё, что V5-S7 будет материализовывать. Если формат плана,
сидов или placement-политик изменится после S7, S7 придётся переделывать.
Срез **вводит формат и метаданные, но не реализует мировой снап и не трогает
cook**.

### 1. Грамматика `.composite` v5: два новых опциональных поля узла

Корень `.composite` не меняется: `v` + `nodes`. Порядок полей узла становится:

```text
kind → resource → name → transform → placement → appearance_seed_boundary
     → options → children
```

**`placement`** — опциональный объект узла, ровно с полями в этом порядке:

```json
"placement": { "mode": "three_point", "trace_start_above_cm": 500 }
```

- `mode` обязателен внутри объекта и принадлежит закрытому множеству:
  `pivot | pivot_normal | three_point | foundation | water | pivot_collision`.
  Отсутствие блока `placement` означает «не прижимать» и является дефолтом;
  отдельного значения `none` в грамматике нет.
- `trace_start_above_cm` опционален, конечен, `>= 0`. Дефолт при наличии
  `placement` — `500` (даговские 5 метров), в канонических байтах опускается,
  когда равен дефолту.
- Запрещён у `kind: random`? **Нет.** `placement` разрешён на любом обычном
  узле и на `random`: в Dagor политика принадлежит узлу, а не варианту, и
  выбранный вариант материализуется в трансформ узла (10 §6.1). У `option`
  блок `placement` запрещён, как и `transform`.

**`appearance_seed_boundary`** — опциональный bool узла, дефолт `false`,
опускается когда `false`. Разрешён на любом обычном узле и на `random`;
запрещён у `option`.

Нарушения (неизвестный `mode`, лишнее поле, неверный тип, блок у `option`,
отрицательный `trace_start_above_cm`) блокируют `MH_E_COMPOSITE_GRAMMAR`.
Отдельного семейства кодов не вводится (11 §4).

**Совместимость.** Оба поля опциональны и опускаются при дефолте, поэтому все
существующие `.composite` остаются байт-идентичными и валидными. Массовая
миграция source-дерева не требуется. Legacy-generation правило (`"v": 5`
обязателен, dual-read нет) не меняется.

### 2. Нормативная семантика placement

Записывается в `10` новым подразделом §6.9 и является authority:

1. **Placement — свойство узла, не листа.** Узел может одновременно порождать
   сущность и иметь `children`.
2. **Скорректированная матрица узла становится родительским фреймом всего его
   поддерева.** Если у потомка есть собственный `placement`, он применяется
   поверх уже скорректированной родительской матрицы. Это поведение Dagor
   (`CompositEntityPool::setTm`, см. §0).
3. **`placement` — провенансное поле. Исполнителя у него нет и не будет.**
   Owner-решения: прижатия в UE5 не будет; обратной записи в Dagor не будет.
   Значит поле фиксирует исходное намерение и предотвращает тихую потерю
   данных, но потребителя не получит. Носитель вводится в V5-S6.1
   (документ 13 §7.1); здесь он только описан как часть грамматики.
   Предупреждения на каждое размещение **нет** — постоянное состояние не
   является поводом для шума в редакторе. Ограничение называется один раз при
   импорте, в отчёте о совместимости.
4. **`trace_start_above_cm` — высота старта трассировки вниз**, не смещение.
   Луч идёт из `world_position + start_above * axis` на расстояние
   `project_max_trace_distance + start_above`. **При промахе узел остаётся
   ровно на исходном месте.**
5. **Override размещения.** `AMHCompositeActor` получает
   `TOptional<EMHPlacementMode> PlacementModeOverride` (`EditInstanceOnly`),
   который перебивает `mode` всего поддерева размещения. Аналог
   `placeTypeOverride`. `trace_start_above_cm` override не получает.
6. **Во время интерактивного перемещения снап не выполняется.** Начало
   перетаскивания → отложенный режим; во время драга обновляется только basis;
   на дропе — один проход снапа, одна транзакция. Аналог `gizmoEnabled` в
   Dagor.
7. **Снап не входит ни в план, ни в `ResolvedSignature`.** См. §4.
8. `three_point` и `foundation` требуют bbox листа; источник bbox —
   `UStaticMesh::GetBoundingBox()`. Для узла без геометрии (group, random без
   выбранного меша) эти режимы деградируют до `pivot`; деградация
   фиксируется `MH_W_UNRESOLVED_PLACEMENT` с NodePath.
9. **Одноуровневое наследование в источнике Dagor.** В `compositMgrService.cpp`
   разбор выглядит так:

   ```cpp
   c.p.placeType = b.getInt("place_type", blk.getInt("place_type", c.p.placeType));
   c.p.aboveHt   = b.getReal("aboveHt",   blk.getReal("aboveHt", c.p.placeType ? 5 : 0));
   ```

   `b` — блок узла, `blk` — **объемлющий** блок. Значит `place_type`, `aboveHt`
   и легаси-флаги наследуются ровно на **один текстовый уровень вверх**: узел
   без своего значения берёт значение объемлющего блока, а узел верхнего уровня
   — значение, объявленное в корне файла. Наследование **не сквозное**: если
   промежуточный узел параметр не объявил, внук значения деда не увидит.
   Грамматика `.composite` наследования не имеет и иметь не будет — импортёр
   обязан разрешить это правило и записать в каждый узел **эффективное**
   значение явно.
10. **Приоритет легаси-флагов.** Порядок разбора в Dagor: сначала
    `placeOnCollision` / `place_on_collision` → `pivot`, затем
    `useCollisionNormal` / `use_collision_norm` → `pivot_normal`, затем явный
    `place_type` **перебивает оба**. Оба флага также наследуются на один
    уровень. Импортёр воспроизводит этот порядок.

### 3. Дуальный сид

**Placement.** `AMHCompositeActor` и `AMHRuntimeCompositeActor` получают:

```cpp
UPROPERTY(EditInstanceOnly, Category = "Mimir|Random")
int32 Seed;                 // serialized-имя НЕ меняется; в UI — "Layout Seed"

UPROPERTY(EditInstanceOnly, Category = "Mimir|Random")
int32 AppearanceSeed;

UPROPERTY(EditInstanceOnly, Category = "Mimir|Random")
bool bAutoAppearanceSeed = true;
```

Serialized-имя `Seed` сохраняется ради совместимости уровней; переименование
происходит только в отображаемом имени и в документации.

**Миграция — критическая деталь.** Для размещения, у которого `AppearanceSeed`
ещё не сериализован, значение вычисляется **один раз** при загрузке как
детерминированный `Mix(Seed, "appearance")`, записывается в свойство и пакет
помечается dirty. Это **не** вычисляемый дефолт и **не** fallback в геттере:
иначе каждый реролл `Seed` молча переролил бы покраску, и второй сид не решал
бы задачу, ради которой вводится. Охранный тест обязателен.

**Модель границ вместо флагов наследования.**

```text
Boundary(node) = ближайший предок (включая сам узел) с
                 appearance_seed_boundary = true,
                 иначе корень размещения

AppearanceStream(node) = MHMakeNodeRandomStream(AppearanceSeed,
                                                NodePath(Boundary(node)))
```

Пять состояний наследования не вводятся. Даговские `ignoreParentInstSeed` /
`useParentInstSeed` / `forceInstSeed0` / `autoInstSeed` существуют только
потому, что там дефолтный instSeed — хэш позиции; здесь сид явный и хранится на
размещении, поэтому достаточно одного булева поля.

Покрываемые сценарии (проверяются тестами):

- дом с согласованными окнами — ни одной границы, всё поддерево берёт сид
  корня размещения;
- магазин тканей — граница на каждом листе ткани, у каждого свой путь и свой
  поток;
- вложенный композит с общей вариативностью — граница на composite-узле.

**Каналы вместо транспорта сида.** Материалу передаётся не сид, а значения.
Каждый лист получает `MH_APPEARANCE_CHANNELS` розыгрышей из своего
`AppearanceStream`, каждый — `NextUnit()` в `[0,1)`, с ролями
`appearance[0] … appearance[N-1]`. Это снимает проблему round-trip: `float32`
точно хранит только 24 значащих бита целого, и `uint32`-сид через
`PerInstanceCustomData` не проходит, а `unit` проходит без потерь и без
контракта на битовую упаковку.

**Битовый контракт.** Задаётся на том же уровне строгости, что и layout-поток
в 10 §13.1:

- поток листа: `MHMakeNodeRandomStream(AppearanceSeed, NodePath(Boundary(leaf)))`,
  алгоритм потока не меняется;
- порядок розыгрышей на лист: строго каналы `0 … MH_APPEARANCE_CHANNELS-1`,
  по одному `NextU32()` на канал, пропусков нет;
- **в прообраз подписи идёт `RawU32`**, как в существующих
  `FMHResolvedCompositeDraw`. `Unit` и значение канала — производные,
  сериализуются тем же float32-shortest, но авторитетом не являются;
- два листа с одной и той же `Boundary` получают один и тот же поток и
  одинаковые значения каналов. Ключ потока — путь границы, не путь листа.
  Это и есть «дом с едиными окнами».

`MH_APPEARANCE_CHANNELS` — ратифицированная константа этого среза.
Предлагаемое значение **4**. Оно **обязано** входить в прообраз
`AppearanceSignature`, иначе изменение константы молча разошло бы подписи.

Транспорт значений в материал (`PerInstanceCustomData` / `SetCustomPrimitiveDataFloat`)
реализуется в V5-S7 вместе с ISM policy. Этот срез фиксирует формат и кладёт
значения в лист плана. Транспорт для actor-листьев **вне scope**: розыгрыши для
них выполняются (чтобы паритет был стабилен), значения лежат в плане, потребитель
появится позже.

### 4. Три уровня хэшей и подписей

Записывается в `10 §13.3` как уточнение.

| Уровень | Что включает | Cross-host |
| --- | --- | --- |
| Source / closure hash | `placement`, `appearance_seed_boundary` и все прочие source-поля | да |
| `ResolvedSignature` | choices, samples, **pre-snap** world matrices, selected dependencies, `Seed` | да |
| `AppearanceSignature` | `AppearanceSeed`, `MH_APPEARANCE_CHANNELS`, границы, per-leaf каналы | да |
| World placement result | trace hits, нормали, уровень воды, post-snap матрицы | **нет** |

`PlacementSignature = Hash(ResolvedSignature, AppearanceSignature)` — то, что
проверяет пятисторонний parity smoke.

**Экономия golden-векторов.** Appearance-розыгрыши кладутся в **отдельный**
массив `Plan.Appearance.Draws`, а не дописываются в существующий `Plan.Draws`.
Layout-часть плана — choices, draws, samples, pre-snap матрицы — остаётся
байт-идентичной, и существующие golden-векторы layout **не регенерируются**.
Регенерируется только итоговая `PlacementSignature`.

Тег резолвера остаётся `mh.random_resolver:2`: layout-поток и его инициализация
не менялись. Appearance-стадия получает собственный тег `mh.appearance:1` в
прообразе `AppearanceSignature`.

Мировой результат при необходимости может считать `WorldPlacementDebugHash`, но
только как диагностику одного конкретного мира. Он не является protocol
authority и не участвует ни в одном acceptance.

### 5. Метаданные плана

`FMHResolvedCompositeNode` и `FMHResolvedCompositeLeaf` получают производные
поля. Все они **исключены из прообраза подписи** (как и существующие
traversal-метаданные), поэтому паритет не затрагивают:

```cpp
struct FMHResolvedCompositeNode
{
    // существующие поля без изменений
    int32 ParentResolvedNodeIndex = INDEX_NONE;
    int32 SelectedOptionIndex     = INDEX_NONE;   // только для random
};

struct FMHResolvedCompositeLeaf
{
    // существующие поля без изменений
    int32 OwningResolvedNodeIndex = INDEX_NONE;
    float AppearanceChannels[MH_APPEARANCE_CHANNELS];
};
```

Правила, которые обязаны выполняться и проверяться:

1. **Pre-order инвариант.** `ParentResolvedNodeIndex < ResolvedNodeIndex` для
   любого узла. `OutPlan.Nodes` уже заполняется в pre-order (`WalkNode`
   добавляет узел до раскрытия composite и до цикла по `children`), включая
   переходы через границу вложенного композита. Инвариант проверяется
   `checkf` в резолвере и тестом.
2. **Граница вложенного композита.** Для узла внутри раскрытого
   `kind: composite` родителем является узел-ссылка на композит, а не
   последний узел предыдущего поддерева.
3. **Random.** Для листа, материализованного из выбранного option, владеющим
   узлом является сам random-узел. Для узла внутри выбранного composite-option
   родителем является random-узел.
4. `SelectedOptionIndex` заполняется только у `kind: random` и совпадает с
   `Decisions[i].OptionIndex` для того же NodePath.

Зачем это нужно помимо UI: снап по §2.2 обязан идти по семантической иерархии.
Достаточно одного прохода по `Plan.Nodes` в порядке массива с массивом
накопленных мировых матриц — дельты и pre/post-снимки не нужны, потому что
потомки считаются от уже скорректированной матрицы родителя, как в Dagor.

### 6. Интерфейс мирового размещения — снят

Редакция 2 предусматривала `IMHWorldPlacementProvider` с identity-реализацией,
чтобы заморозить интерфейс до включения прижатия в V5-S7. **Owner-решение
отменяет прижатие целиком**, поэтому интерфейс не вводится: замораживать нечего.

Порядок стадий остаётся нормативным и без провайдера:

```text
applied graph
  → MHResolveCompositePlan (layout, детерминированный, подписанный)
  → appearance stage (детерминированная, подписанная)
  → basis (трансформ актора)
  → MHValidateResolvedPlacementTransforms
  → материализация компонентов
```

**Что это значит для видимости результата.** Ранее я предупреждал, что этот
срез почти не даёт результата, который можно потрогать. После снятия провайдера
это по-прежнему верно: появится поле `AppearanceSeed`, картинка не изменится до
V5-S7. Осязаемый результат для маршрута из CDK даёт V5-S6.1, а не этот срез.
Идти в него нужно с этим знанием.

### 7. Compatibility matrix даговской грамматики

Строгий читатель `addon/mh4blend/core/dagor_composites.py` сейчас принимает в
узле ровно `node{}`, `ent{}`, `name:t`, `tm:m`; всё остальное падает как
`unsupported node construct`. `place_type` даговский редактор проставляет при
экспорте рутинно, поэтому в текущем виде реальные production-композиты не
импортируются.

Каждый конструкт получает **ровно один** статус. Молчаливого отбрасывания нет
(11 §3): drop сопровождается `MH_W_DAGOR_CONSTRUCT_DROPPED` с полным NodePath и
попадает в квитанцию импорта.

| Конструкт | Статус | Действие |
| --- | --- | --- |
| `place_type:i=0` | executed | легальное «не прижимать»: блок `placement` не создаётся, `aboveHt` при нулевом типе отбрасывается |
| `place_type:i` (1…6) | executed | → `placement.mode` (`1→pivot`, `2→pivot_normal`, `3→three_point`, `4→foundation`, `5→water`, `6→pivot_collision`); блокируются только значения `< 0` и `> 6` |
| `aboveHt:r` | executed | при ненулевом `place_type` → `placement.trace_start_above_cm`, метры → сантиметры; при `place_type=0` отбрасывается |
| `placeOnCollision:b`, `place_on_collision:b` | executed | legacy alias → `mode: pivot`; перебивается явным `place_type` |
| `useCollisionNormal:b`, `use_collision_norm:b` | executed | legacy alias → `mode: pivot_normal`; перебивается явным `place_type` |
| наследование от объемлющего блока | executed | `place_type`, `aboveHt` и оба легаси-флага: **один** текстовый уровень вверх, разрешается импортёром, в узел пишется эффективное значение (§2.9) |
| `gameObj` любого имени | executed | → kind `marker` (V5-S6.1 §6.4): имя и трансформ сохраняются, в UE не порождается ничего. Реестр ролей не нужен |
| `ignoreParentInstSeed:b=yes` | executed | → `appearance_seed_boundary: true` |
| `useParentInstSeed:b=yes` | executed | → `appearance_seed_boundary: false` (явно) |
| `label:t` | dropped + warning | потребителя нет, пока `require` заблокирован |
| `colors{}` | dropped + warning | кандидат на будущий appearance-профиль; отдельный ADR |
| `quantizeTm:b` | dropped + warning | **не исполнять**: квантует позицию до 1/32 м = 3.125 см, компенсируя точность легаси 12×32-битного инстанс-формата Dagor. В UE такого рассогласования нет, исполнение только испортило бы точность |
| `require{}` | **blocked, permanently** | topology semantics: исполнение потребовало бы входа в резолвер и в подпись. `MH_E_INVALID_RESOURCE_SOURCE` с сообщением «перевыразите через random options» |
| inline `p2` в узле | **blocked, см. OPEN-V5-15** | текущий `_lossless_stop` сохраняется до решения owner |
| `prefab` в `name:t="x:prefab"` | **blocked без registry** | см. §8 |

Статус «preserved but not executed» не используется ни для одного конструкта:
сохранённая мёртвая конструкция однажды будет включена по ошибке.

### 8. Реестр ролей даговских endpoint'ов

Сейчас `dagor_composites.py` делает безусловно `"prefab": "mesh"`. В Dagor
prefab часто несёт коллизию, а не видимую геометрию — параметр `quantizeTm`
существует ровно для пары «геометрия rendInst + коллизия prefab». Безусловная
конверсия делает коллизионный прокси видимым мешем.

Та же болезнь у `gameObj`. В реальных композитах встречаются служебные
маркеры — например узел `name:t="dummy_pivot:gameObj"` с единичной матрицей,
который несёт `place_type`, но ничего не спавнит. Текущее безусловное
`"gameobj": "actor"` превратит его в actor-лист с ресурсом `dummy_pivot`, для
которого в UE нет класса: импорт либо упадёт, либо породит пустой актор.

- Дефолт становится fail-closed: `prefab` и не распознанный `gameObj` без
  явного маппинга блокируют импорт.
- Вводится project-настройка `DagorEndpointPolicy`:
  `TMap<FString, EMHDagorEndpointRole>` со значениями
  `CollisionProxy | StaticVisual | Marker | ActorClass | Unsupported`.
  Ключ — `<dagor_type>:<logical_name>`.
- `CollisionProxy` и `Marker` означают «узел сохраняется как обычный узел с
  трансформом и `placement`, но **листа не порождает**». Для маркера это ровно
  нужное поведение: его `placement` продолжает действовать на поддерево
  (§2.1–2.2), мусорного актора не появляется.
- Привязка коллизии `CollisionProxy` к парному мешу — отдельный вопрос вне
  scope.

### 9. Открытые вопросы для `docs/QUESTIONS.md`

Оформляются по 11 §1 (Контекст → Вопрос → Временное fail-closed правило →
Статус) **до** реализации:

- **OPEN-V5-15 — inline `p2` в даговском узле.** Даговская документация
  использует inline-параметры в собственном примере иерархии (стол + кружка).
  Варианты: (a) генерировать производный `.placement` с детерминированным
  именем; (b) разрешить inline-профиль в грамматике `.composite`; (c) оставить
  блокировку. Временное правило: (c), текущий `_lossless_stop` сохраняется.
- **OPEN-V5-16 — значение `MH_APPEARANCE_CHANNELS`.** Предложение 4. Временное
  правило: 4, зафиксировано в прообразе `AppearanceSignature`; изменение
  требует отдельного ADR и регенерации appearance-goldens.
- **OPEN-V5-18 — роль служебных `gameObj`-маркеров.** Узел
  `dummy_pivot:gameObj` встречается в production-композитах и, по описанию
  owner, служит точкой привязки, а не спавнящейся сущностью. Вопрос: какие
  именно логические имена `gameObj` являются маркерами, и должен ли маркер
  когда-либо порождать что-то в UE (например пустой `USceneComponent` для
  выбора во вьюпорте). Временное правило: маркеров в реестре нет, любой
  нераспознанный `gameObj` блокирует импорт с указанием имени и NodePath —
  то есть первый же реальный композит назовёт нужные имена сам.
- **OPEN-V5-17 — `project_max_trace_distance`.** Аналог
  `EDITORCORE->getMaxTraceDistance()`. Временное правило: настройка проекта,
  дефолт согласовать с owner; не входит ни в один cross-host хэш.

### Acceptance V5-S6.2

1. **Грамматика.** Существующие `.composite` из `golden/` читаются и
   записываются байт-идентично. Новые поля при дефолте опускаются. Все
   перечисленные нарушения дают `MH_E_COMPOSITE_GRAMMAR`.
2. **Layout не тронут.** `Plan.Draws`, `Plan.Decisions`, pre-snap world
   matrices, `SelectedDependencies` и `ResolvedSignature` **байт-идентичны**
   значениям до среза на всём ратифицированном seed-наборе. Тег резолвера
   остаётся `mh.random_resolver:2`.
3. **Appearance паритет.** Python reference = UE Automation = Editor preview =
   PIE = packaged по `Plan.Appearance.Draws`, per-leaf каналам и
   `AppearanceSignature`.
4. **Независимость сидов.** Реролл `AppearanceSeed` сохраняет
   `ResolvedSignature` байт-идентичной и меняет `AppearanceSignature`.
   Реролл `Seed` меняет `ResolvedSignature`; `AppearanceSignature` меняется
   только через изменение набора листьев. Для каждого `NodePath`, присутствующего
   в обоих планах, значения appearance-каналов **байт-идентичны**: переживший
   topology-смену лист сохраняет свои каналы.
5. **Миграция.** Уровень со старым размещением после загрузки имеет
   материализованный `AppearanceSeed`; последующие рероллы `Seed` его не
   меняют. Охранный тест против реализации через вычисляемый дефолт.
6. **Границы сида.** Три сценария из §3 воспроизводимы: дом без границ — один
   сид на поддерево; ткани с границей на листе — разные; граница на
   composite-узле — общий сид его поддерева.
7. **Метаданные плана.** `ParentResolvedNodeIndex < ResolvedNodeIndex` для всех
   узлов; корректность через границу вложенного композита и через random option
   проверена на GAZ-53; поля отсутствуют в прообразах обеих подписей.
8. **Placement identity.** С identity-провайдером мировые матрицы и все
   компоненты байт-идентичны состоянию до среза.
9. **Наследование и приоритет.** Композит с `place_type` только в корне даёт
   эффективное значение у всех узлов верхнего уровня и **не** даёт его у
   внуков через необъявивший промежуточный узел. Композит с
   `placeOnCollision:b=yes` и `place_type:i=3` на одном узле даёт
   `three_point`. Оба случая — отдельные фикстуры.
10. **Ненулевой режим сохраняется, но не исполняется.** Композит с
    `place_type:i=1` импортируется, `placement.mode = "pivot"` присутствует в
    канонических байтах, мировые матрицы **байт-идентичны** identity-случаю,
    и выдаётся ровно одно `MH_W_PLACEMENT_NOT_APPLIED` на размещение.
11. **Round-trip.** Dagor → MH → Dagor для фикстуры с `place_type` и `aboveHt`
    восстанавливает эффективные значения на тех же узлах.
12. **Dagor import.** Фикстуры из **реальных** production-композитов (не
   синтетических) с `place_type`, `aboveHt`, `ignoreParentInstSeed`:
   импортируются lossless. Композит с `require` даёт конкретный блок с NodePath,
   а не generic grammar wall. Композит с `quantizeTm` импортируется с warning и
   **без** квантования координат. `prefab` без записи в `DagorPrefabPolicy`
   блокирует импорт.
13. **Маркеры.** Композит с `dummy_pivot:gameObj` без записи в
    `DagorEndpointPolicy` блокирует импорт с именем и NodePath. С записью
    `Marker` — узел присутствует в плане, листа не порождает, его `placement`
    доезжает до потомков.
14. Новые коды `MH_W_DAGOR_CONSTRUCT_DROPPED`, `MH_W_PLACEMENT_NOT_APPLIED`
    (и `MH_E_PLACEMENT_STATE_DESYNC` из S6.2) присутствуют в обоих registry и
    в golden counts.
15. Двойной unity gate по 11 §9 и packaged smoke.

---

## Вне scope обоих срезов

Перечислено явно, чтобы исполнитель не расширял границы:

- **Composite Outliner** (Slate-инструмент, semantic hierarchy, навигация по
  вложенным композитам, show choices/weights) — отдельный ADR. Дешёвый патч с
  индексами из S6.2 §5 к нему не относится и его не заменяет.
- **Shared definition cache** (`UMHCompositeDefinitionSubsystem`). Избыточность
  реальна: `MHBuildAppliedCompositeGraph` вызывается на каждый актор, и полный
  обход closure с валидацией рецептов повторяется на каждое размещение. Но это
  чистая производительность редактора: формат, план, подпись и cook не
  затрагиваются, значит S7 это не гейтит. Делать после инструментирования
  (`BuildAppliedGraph`, `ResolveCompositePlan`, `LoadEndpoints`,
  `WaitStaticMeshCompilation`, `CompilePlacement`, `RegisterComponents`,
  `DestroyRetiredComponents`), чтобы не оптимизировать не то.
- **Cook flattening, ISM/HISM policy, World Partition/OFPA** — существующий
  V5-S7, не переопределяется этим документом.
- **Мировая реализация режимов прижатия** — все шесть, V5-S7 или сразу после.
  Реализация одного режима `pivot` внутри S6.2 рассмотрена и **отклонена**
  (§6).
- **Gameplay attachment pass** для actor-листьев.
- **Транспорт appearance-каналов в материал** — V5-S7 вместе с ISM policy.
- **Привязка коллизионного prefab к парному мешу.**
- **V5-S9 / V5-S10** (packing, physics settle) — parked, не затрагиваются.

## Что V5-S7 получает на входе после этих срезов

```text
frozen:  грамматика placement и appearance_seed_boundary
frozen:  Seed + AppearanceSeed на размещении, модель границ, N каналов
frozen:  ParentResolvedNodeIndex / OwningResolvedNodeIndex / SelectedOptionIndex
frozen:  три уровня хэшей и граница «снап вне подписи»
frozen:  IMHWorldPlacementProvider и порядок стадий
frozen:  compatibility matrix даговской грамматики
```

S7 не изобретает эти модели, а материализует уже определённый
layout + appearance + world-placement result в cook output.
