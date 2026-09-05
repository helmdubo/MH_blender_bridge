> Status: NORMATIVE · Architecture version: Recipe Model v2.1 · Supersedes: — · Контракт среза R3a для внешнего исполнителя (близнец, 2026-09-04)

# Контракт R3a — пять хэшей/ревизий интерфейса меша в реестре endpoint'ов

Основание: KICKOFF §5 (R3: «семантический reconcile §4 по пяти хэшам/ревизиям
П4»; ограничение owner ≤6 изменяемых файлов — R3 разделён на R3a и R3b),
`docs/16_recipe_model.md` §2.2 (определение пяти хэшей/ревизий: когда
считаются, из чего), §4 (протокол обновлений — потребитель), §7.2 восьмая
строка (`PlacementInterfaceHash` — код; в `ue/MimirComposite/Source`
идентификатора уже нет, лексический гейт зелёный; в квитанции это
фиксируется).

R3a даёт **данные**: реестр считает интерфейс меша при `Ready` и при каждом
re-admission после `Revision++`, и классифицирует, что изменилось. R3b (следующий
контракт) заменяет полный `RebuildComposite` в `MHNotifyGeneratedResourceChanged`
на действия §4 по этой классификации. В R3a поведение реимпорта **не меняется**:
акторы по-прежнему перестраиваются целиком.

## Что уже есть в ветке (не переписывать)

Ветка `recipe/r3a-endpoint-interface-hashes` от `origin/main` (`c5a951b`). Red-коммит близнеца `4fbb032`:

- `Public/Composite/MHEndpointPrototypeRegistry.h` — API-контракт (менять
  только по STOP+OPEN): в `FMHEndpointPrototype` поля `FBox Bounds`,
  `uint32 PayloadRevision`, `uint32 BoundsRevision`, `uint64 BucketDescriptorHash`,
  `uint64 CollisionInterfaceHash`, `uint64 MaterialBindingHash`; структура
  `FMHEndpointInterfaceDelta { bFirstAdmission, bPayload, bBounds,
  bBucketDescriptor, bCollisionInterface, bMaterialBinding; Any() }`; метод
  `UMHEndpointPrototypeRegistry::GetLastInterfaceDelta(Key)`.
- `Private/Composite/MHEndpointPrototypeRegistry.cpp` — fail-closed заглушка
  `GetLastInterfaceDelta` (пустая дельта); поля прототипа никто не заполняет.
- тест `Mimir.V5.Composite.Reconcile.PrototypeInterfaceHashes`
  (`MimirCompositeTests/Private/MHEndpointInterfaceHashTest.cpp`) — **это и есть
  acceptance**, секции A–J: первая admission (хэши ненулевые, дельта = только
  `bFirstAdmission`); детерминизм (re-admission без изменений — те же значения,
  пустая дельта); только payload (receipt `SourceHash`) → `PayloadRevision+1`,
  `bPayload`; только bounds (`SetPositiveBoundsExtension`) → `BoundsRevision+1`,
  `bBounds`, `Bounds` прототипа отражает расширение; добавлен material slot →
  `BucketDescriptorHash` и `MaterialBindingHash` меняются, `CollisionInterfaceHash`
  нет; сменён default material в слоте → только `MaterialBindingHash`;
  `CollisionTraceFlag` → только `CollisionInterfaceHash`; `AddSourceModel` (LOD
  count) → только `BucketDescriptorHash`; Invalid-прототип — нули и пустая
  дельта; ноль tag-запросов и live receipt reads при вычислении.

Тест — норма среза. Его не редактируют; блокирует — STOP + OPEN.

## Норма

### 1. Когда считается

В `Admit`, в момент перехода в `Ready`, **только** для `EMHResourceKind::StaticMesh`.
Для других kinds и для `Invalid` — поля остаются нулевыми, `Bounds` невалиден,
дельта пустая. Реестр хранит предыдущий Ready-снимок пяти значений на ключ
(переживает `Invalidate`), чтобы при следующем `Ready` посчитать дельту.
`InvalidateAll` снимки **не** стирает (иначе каждый реимпорт после
`InvalidateAll` выглядел бы первой admission). Снимок стирается только с
удалением записи ключа (если такое есть) — иначе живёт всю сессию.

### 2. Из чего считается (детерминированно, без Asset Registry)

Все входы — поля живого `UStaticMesh`; никаких `GetAssetRegistryTags`,
`FAssetData`, `IAssetRegistry`, `FinishCompilation`, загрузок других объектов.
Хэш — `CityHash64`/`FXxHash64` по каноническому байтовому потоку (порядок
полей фиксирован, строки как UTF-8 без BOM, числа little-endian); значение
`0` запрещено как результат (при коллизии с нулём — `1`).

| Поле | Входы | Изменение → |
|---|---|---|
| `PayloadRevision` | `UMHStaticMeshImportData::SourceHash` (+ `ImporterVersion`) — сравнение с предыдущим снимком; `+1` при отличии, `0` при первой admission | render refresh |
| `BoundsRevision` / `Bounds` | `GetExtendedBounds()` (origin, box extent, sphere radius) + `GetPositiveBoundsExtension()` + `GetNegativeBoundsExtension()`; `Bounds` = `GetExtendedBounds().GetBox()`, расширенный на положительное/отрицательное extension, если `GetExtendedBounds()` нулевой (меш без render data в тестах) | bounds cache |
| `BucketDescriptorHash` | структура слотов: число, порядок, `MaterialSlotName` (**без** путей материалов — они только в `MaterialBindingHash`); `GetNumSourceModels()` (LOD count); секции из `GetRenderData()` если он есть (для каждого LOD: число секций, `MaterialIndex`, `bEnableCollision`, `bCastShadow`); иначе секции не участвуют | миграция бакета |
| `CollisionInterfaceHash` | наличие `BodySetup`; `CollisionTraceFlag`; `bDoubleSidedGeometry`; число элементов `AggGeom` по типам; `DefaultInstance.GetCollisionProfileName()` | recreate physics |
| `MaterialBindingHash` | слоты: `MaterialSlotName`, путь default `MaterialInterface`, путь `OverlayMaterialInterface`, порядок | reconcile материалов |

Разделение обязательно: тест требует, чтобы смена default material в
существующем слоте меняла **только** `MaterialBindingHash`, а добавление слота —
и `BucketDescriptorHash`, и `MaterialBindingHash`; `CollisionTraceFlag` — только
`CollisionInterfaceHash`; LOD count — только `BucketDescriptorHash`.

### 3. Дельта

`GetLastInterfaceDelta(Key)`: первая Ready-admission → `bFirstAdmission = true`,
остальное false. Re-admission → флаги по различию с предыдущим снимком;
`bPayload` = `PayloadRevision` вырос; `bBounds` = `BoundsRevision` вырос;
остальные — хэш отличается. Ключ не Ready / никогда не резолвился —
default-конструированная дельта.

### 4. Не входит в R3a

Изменение поведения `MHNotifyGeneratedResourceChanged`, актора, компилятора
размещения, пулов (R5), `Loading` (R4). Перф-счётчики `recipes_recompiled` /
`parent_recipes_recompiled` — R3b.

## Закрытый список файлов

- `ue/MimirComposite/Source/MimirCompositeEditor/Public/Composite/MHEndpointPrototypeRegistry.h`
  — только приватные члены и приватные helpers (публичный API — контракт);
- `ue/MimirComposite/Source/MimirCompositeEditor/Private/Composite/MHEndpointPrototypeRegistry.cpp`;
- `docs/receipts/recipe_r3a.md` (новая), `docs/RECIPE_EXECUTION_STATUS.md`
  (строка R3a);
- при необходимости один новый приватный файл
  `Private/Composite/MHEndpointInterfaceHash.{h,cpp}` для вычисления хэшей
  (чистые функции над `const UStaticMesh&`) — допускается, чтобы не раздувать
  реестр; заголовок приватный, не Public.

## Запрещено

- менять тесты; менять публичный API реестра сверх red-коммита; менять
  `MHCompositePlacementEvents.cpp`, актор, компилятор, resolver, рецепты, proof;
- Asset Registry (`GetAssets*`, `FAssetData(&Object)`, теги), `FinishCompilation`,
  `LoadObject` чего-либо кроме самого endpoint'а (он уже грузится в `Admit`);
- новые коды `MH_E_*`/`MH_W_*` (реестр диагностик пиннут 54/20);
- `git pull`, стоя на `main`; push в `main`.

## Acceptance

1. Non-unity/no-PCH сборка хоста — Succeeded.
2. Зелёные: `Mimir.V5.Composite.Reconcile.PrototypeInterfaceHashes`,
   `Mimir.V5.Composite.Registry.*`, `Mimir.V5.Composite.Recipe.*`,
   `Mimir.V5.Composite.Perf.*`, `Mimir.V5.Composite.DefinitionPool.*`.
3. Полный NullRHI suite на generic-хосте: 0 Fail, число тестов = 196 + 1.
4. `RunUAT BuildPlugin -StrictIncludes` — Success; guarded force-unity — Succeeded.
5. `git diff --check` чисто; `python tools/check_normative_docs.py` — OK.
6. Квитанция `docs/receipts/recipe_r3a.md`: RED/GREEN логи, гейты, таблица
   входов каждого хэша (как реализовано), замечание о восьмой строке §7.2
   (`PlacementInterfaceHash` в коде отсутствует, гейт зелёный), список OPEN.
7. Регресс перфа: `Mimir.V5.Composite.Perf.EndpointCounters` зелёный —
   вычисление хэшей не добавляет sync package loads и tag-запросов.

## STOP + OPEN

Остановиться и записать `OPEN-R3A-N` в этот файл (что, где, первая падающая
строка, два варианта), закоммитить только это и сообщить близнецу, если:
тест из acceptance нельзя сделать зелёным без правки теста или запрещённого
файла; существующий тест ломается; для входа хэша нужен объект, которого нет
на `UStaticMesh` (например, физматериал через `BodySetup->PhysMaterial` —
допустимо читать путь, но не грузить); нужен новый код диагностики.

## Host и правила git

Хост — свежий по `tools/setup_s6_runtime_host.ps1`, Engine `UE_5.7`, тесты
NullRHI с `-NoAssetRegistryCache -MHGoldenRoot=<repo>/golden`. Не собирать,
пока идёт прогон тестов. Только ветка `recipe/r3a-endpoint-interface-hashes`
(`git checkout --detach origin/…` или локальная ветка от неё); никогда
`git pull` на `main`, никогда push в `main`. Один PR в `main`; merge — близнец
после независимой проверки. Интерактивных шагов в редакторе нет.

## OPEN-R3A-1 — default material одновременно включён и исключён из bucket hash

Статус: **OPEN / STOP до нормативного решения близнеца или owner**, 2026-09-05.
Обнаружено при обязательном чтении контракта до первой правки реализации.
База исполнителя: `ebbf250`, red-коммит `4fbb032`.

Контекст и первая противоречащая строка: §2, строка 71 исходного контракта,
включает путь default `MaterialInterface` в `BucketDescriptorHash`;
`docs/16_recipe_model.md` §2.2 также включает дефолтные материалы. Но строки
75–78 этого контракта требуют, чтобы замена default material существующего
слота меняла **только** `MaterialBindingHash`. Включение пути в канонический
байтовый поток bucket hash нарушает второе требование, исключение — первое.

Секция F acceptance-теста (`MHEndpointInterfaceHashTest.cpp:190–198`) комментарием
обещает неизменность descriptor layout, но фактически проверяет изменение
binding и отсутствие collision delta; равенства bucket hash или отсутствия
`bBucketDescriptor` там нет. Поэтому зелёный тест не разрешит противоречие.
Runtime failure не заявляется: сборка/RED-прогон ещё не запускались, код и
acceptance-тест не менялись.

Два варианта для нормативного решения:

1. **Рекомендуемый:** bucket hash включает структуру слотов (число, порядок,
   имена), LOD/sections, а пути default/overlay материалов входят только в
   binding hash. Близнец согласует строку таблицы и ADR §2.2 с обязательным
   разделением; при необходимости усиливает свой red-тест проверкой отсутствия
   bucket delta. Исполнитель тест не редактирует.
2. Default material остаётся входом обоих хэшей. Близнец меняет требование
   «только MaterialBindingHash» и комментарий/ожидаемую семантику секции F;
   replacement материала классифицируется как descriptor + binding.

Временное правило: не выбирать смысл самостоятельно, не реализовывать R3a
и не менять публичный API, другие документы или тест. Этот STOP-коммит
содержит только данный OPEN-раздел. Независимый R4-pre-2 разрешён owner'ом
в том же поручении и не требует готового R3a.

**Ответ близнеца (2026-09-05): вариант 1.** Противоречие было в контракте
(строка таблицы для `BucketDescriptorHash` перечисляла путь default material);
норма — абзац «Разделение обязательно» и секция F теста. Исправлено в этом
коммите: строка таблицы (пути материалов только в `MaterialBindingHash`),
`docs/16_recipe_model.md` §2.2 (то же), и секция F red-теста усилена —
равенство `BucketDescriptorHash` и отсутствие `bBucketDescriptor` при смене
default material в существующем слоте. STOP снят — продолжать реализацию.
