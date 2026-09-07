# Загрузка композитов и Packed Level Actor — исследование 2026-09-07

Статус: RESEARCH (снимок до реализации). Owner принял A/B 2026-09-07;
реализация и уточнение Save — `docs/contracts/composite_loading.md`.
Основание: полевой скриншот
owner с кубами вместо листьев композитов. Проверен checkout `c6faeae`, UE 5.7.4
CL 51494982. Срез загрузки ещё не реализован; приведённые ниже причины работы
кода не являются замером доли времени CPU, I/O, DDC или texture streaming.

## 1. Что делает UE

Epic описывает Packed Level Blueprint как оптимизированное представление
набора static meshes: [Level Instancing](https://dev.epicgames.com/documentation/en-us/unreal-engine/level-instancing-in-unreal-engine).
Детали ниже проверены в локальном исходнике именно UE 5.7.
Корень: `D:/PersonalProjects/UE5/UE_5.7/Engine/Source/`.

- `Runtime/Engine/Private/PackedLevelActor/PackedLevelActorBuilder.cpp:298`:
  исходный level загружается для packing в отдельный временный LevelInstance.
- `PackedLevelActorISMBuilder.cpp:26,69,94` в той же папке: реальные SMC/ISM
  группируются по mesh/material/render descriptor. HISM выбирается при
  количестве instances > 1 и non-Nanite mesh; иначе используется ISM.
- `Runtime/Engine/Public/ISMPartition/ISMComponentDescriptor.h:283`:
  используемый descriptor имеет жёсткие UObject-ссылки на меш и материалы.
- `PackedLevelActorBuilder.cpp:452` и
  `Editor/UnrealEd/Private/Kismet2/Kismet2.cpp:1185`: packed components
  переносятся в сохраняемые Blueprint SCS component templates.
- `Runtime/Engine/Private/InstancedStaticMesh.cpp:3505,3561`: instances
  Blueprint наследуют массивы transforms/custom data от archetype.
- `Runtime/Engine/Public/PackedLevelActor/PackedLevelActor.h:95`:
  исходный world не нужен для cook geometry самого Packed Blueprint.

Таким образом, при открытии карты UE уже имеет ссылки на настоящие меши,
материалы и сохранённое представление instances. Это обычная загрузка
зависимостей Unreal. В проверенном packed-пути нет подстановки default cube.
Это не обещание мгновенной загрузки: packages, render data, shaders и texture
mips всё равно должны оказаться в памяти.

## 2. Что делает MimirComposite сейчас

Пути ниже относительно `ue/MimirComposite/Source/MimirCompositeEditor/`.

| Этап | Подтверждение |
|---|---|
| На акторе сохранены soft reference определения, seeds и call context; представление временное | `Public/Composite/MHCompositeActor.h:242,277` |
| После загрузки актора запускается построение | `Private/Composite/MHCompositeActor.cpp:1073,1093` — PostLoad помечает, PostRegisterAllComponents вызывает RebuildComposite |
| Каждый выбранный нерезидентный mesh запрашивается асинхронно | `Private/Composite/MHEndpointPrototypeRegistry.cpp:475,486`; PendingLoads дедуплицирует один ресурс между placements |
| Пока mesh Loading, возвращается настоящий UStaticMesh куба | тот же файл `:351,360`; это не low LOD исходного меша |
| В normal scene всё уже размещено через ISM | `Private/Composite/MHInstancePool.cpp:64,119`: transient pool components; исходная геометрия — отдельные UStaticMesh `.uasset` |
| Каждый mesh completion вызывает уведомление реимпорта | `MHEndpointPrototypeRegistry.cpp:371` → `MHNotifyGeneratedResourceChanged(Key)` |
| Уведомление перебирает MH actors и проверяет зависимости; первое появление mesh также инвалидирует proof cache | `Private/Composite/MHCompositePlacementEvents.cpp:45,66,126` |
| Первая Ready admission вызывает полную пересборку зависимого выбранного placement | `Private/Composite/MHCompositeActor.cpp:674,681` |
| Полная пересборка повторно получает план и перевыдаёт все pool instances владельца | `MHCompositeActor.cpp:766,829,971`; `MHCompositePlacementCompiler.cpp:914` RemoveOwner, затем Add для всех листьев |

Асинхронные запросы не являются последовательными дисковыми загрузками.
Проблема здесь — отдельная цепочка повторной работы на completion каждого
ресурса. Для большого числа уникальных мешей и повторённых placements это
может многократно пересобирать один и тот же объект. Величину ускорения нужно
измерить, а саму повторную работу видно непосредственно в коде.

## 3. Материалы, текстуры и кэш

Generated meshes/materials/textures сохраняются в `.uasset`:
`MHStaticMeshImporter.cpp:64,1202`, `MHMaterialImporter.cpp:692,722`,
`MHTextureImporter.cpp:81,94,171` в соответствующих Private-папках модуля.
Неизменившиеся receipts позволяют пропускать импорт. Mesh slots удерживают
материалы, а параметры MIC — текстуры обычными UObject-ссылками.

Endpoint registry живёт весь editor process, но хранит weak object pointers.
Пока сцена использует меш через ISM/SMC, компоненты удерживают его. После
выгрузки/GC без потребителей возможна повторная загрузка; это не повторный
импорт и не признак утраты дискового DDC. Пример намеренного dead-cache reload
проверяется в `MHCompositeDefinitionMetricsTest.cpp:646`.

Свежий `Saved/Logs/MimirHead_portfolio.log` не содержит mesh/texture build или
shader compile рядом с CE-входами (строки 3382–3448). На старте есть flush
367 async packages (2440), но без attribution их нельзя приписывать MH.
Ранее проверенный backup содержит 9622 DDC hits, 3298 shader-job cache hits
и один скомпилированный shader. Полная повторная компиляция материалов при
каждом Edit этими данными не подтверждается; времена texture streaming
по ним не установлены.

Отдельная проверяемая точка: `MHEndpointPrototypeRegistry.cpp:553` при первой
admission может включать `MATUSAGE_InstancedStaticMeshes` у base material,
если флаг ещё отсутствует. Это способно менять материал/запускать shader
cache при загрузке. Нужно проверить сохранение этого флага на этапе импорта;
его наличие нельзя считать доказательством перекомпиляции каждого материала.

## 4. Предлагаемое изменение

Цель: при открытии сохранённой сцены получать настоящие зависимости через
обычную загрузку Unreal, а при асинхронном появлении нового композита
материализовать готовый набор один раз. Общий ISM-пул уровня сохраняется.

**Часть A — готовность загрузки отдельно от изменения содержимого.**

- Собрать уникальные выбранные mesh dependencies из уже вычисленного плана;
  сохранить дедупликацию между placements и удерживать load handles до передачи
  объектов компонентам, устойчиво к GC, удалению актора и смене seeds.
- Loading → Ready обрабатывать отдельным путём, не как reimport исходного
  ассета. Группировать завершения; готовый placement материализовать один раз,
  без повторного resolver и RemoveOwner на каждый mesh.
- Убрать кубы на каждом листе. Для нового объекта показывать состояние загрузки
  в Outliner; при перестройке уже видимого сохранять предыдущую геометрию до
  готовности замены. Настоящие Missing/Error отделить от Loading.
- Ready UObject не тождественен готовым shaders/texture mips; ожидания и
  визуальные проверки должны различать эти стадии.

**Часть B — сохраняемые ссылки на выбранные меши размещённого актора.**

- Хранить производный набор hard references на выбранные UStaticMesh рядом
  с сохранённым placement. Материалы и текстуры следуют обычным зависимостям
  этих ассетов. Не загружать все невыбранные random-варианты.
- Авторская модель и seeds остаются источником истины; список зависимостей
  обновляется при изменении структуры/seed, Save Edit и reimport.
- У старых карт и устаревших производных данных должен работать путь A.
  Persistent validity нельзя строить только на сбрасываемом при перезапуске
  счётчике in-memory recipe revision.
- Начать со списка зависимостей. Сохранение ещё и полного render layout имеет
  смысл, только если замер покажет существенную цену однократного resolver.
  Для этого не требуется сохранять общий pool actor или выпускать BPP на
  каждый procedural вариант.

Часть B переносит часть ожидания в штатное открытие карты: hard references
не делают I/O бесплатным. Она даёт Unreal заранее известные зависимости,
вместо их позднего обнаружения после регистрации актора.

## 5. Приёмка

Одинаковая карта/набор мешей: отдельный процесс с тёплым DDC и нерезидентными
UObjects; повторное открытие внутри процесса; дополнительный тест после GC.
Замерять от запроса карты до готовой геометрии, отдельно package load,
mesh render data, shaders и texture mips; не только время PostRegister.

- Ноль per-leaf cubes в штатной загрузке; ошибка ресурса видна отдельно.
- Один запрос на уникальный нерезидентный mesh, общие запросы между placements.
- Одна первоначальная материализация готового placement; нет N полных
  пересборок по числу пришедших meshes.
- Нет новых import/build/save material операций при неизменённой сцене.
- Seeds, transforms, appearance, ISM grouping, выбор/Edit/Save/Cancel и
  shadow parity сохраняют поведение. Reimport остаётся отдельным проверяемым
  сценарием настоящего изменения ассета.
- Сравнить весь интервал загрузки и editor responsiveness с равным набором
  мешей в Packed Level Blueprint. Численный выигрыш до замера не заявляется.

Текущий контракт `docs/16_recipe_model.md:130` прямо предписывает куб при
Loading. Исполнение части A должно обновить этот пункт и старый R4-тест
в соответствии с новым запросом owner, а не оставлять противоречащие ожидания.
