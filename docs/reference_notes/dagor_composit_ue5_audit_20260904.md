> Status: REFERENCE · исследование и аудит (2026-09-04) · не норматив; предложения требуют отдельного решения и не запускают срезы реализации.

**Composit в DagorEngine и MimirComposite: устройство, проектирование и аудит**

Дата: 2026-09-04. Исследование и предложения; не изменение нормативов проекта.

Проверены публичный DagorEngine main **75723669297e48e200a0dc67b18c1629e0975daf** и локальный MH_blender_bridge main **c5a951ba6ea1db3e8a430388b8c9c1b46f11ab3a**. SHA Dagor получен через GitHub API в этом исследовании; ключевые исходники скачаны с этого SHA и прочитаны. Исследование не меняло код и архитектурные нормативы MH; при публикации отчёт добавлен в справочный раздел docs/NORMATIVE_INDEX.md. Анализ UE относится к исходникам этого checkout, а не к установленному в проекте бинарному плагину.

Главный вывод: направление нашего Recipe Model правильное. Переписывать систему целиком не требуется. Но наличие FMHCompiledRecipe ещё не делает инстанс дешёвым, а сохранение структуры при Break ещё не обеспечивает сохранение результата. Первоочередные проблемы — перенос состояния через Build/Break, гранулярность обновлений и различие editor/runtime материализации.

**1. Что представляет собой Dagor composit.**

Авторский источник — текстовый .composit.blk. Он описывает ссылки на ресурсы, узлы, матрицы или диапазоны размещения, вероятности вариантов, вложенные композиты и дополнительные свойства. Это рецепт многократного размещения. В обычном daEditor-пайплайне в игровой уровень экспортируются конечные сущности; сам авторский BLK не обязан существовать в игре. [D1]

Устройство редакторского исполнителя:

| Уровень | Реализация | Ответственность |
|---|---|---|
| Asset manager | DagorAssetMgr | Имена и типы ресурсов, загрузка BLK, изменение файлов и include, уведомления |
| Фабрика | CompositEntityManagementService | Создать/клонировать сущность, найти или создать пул соответствующего ассета, обнаружить цикл |
| Рецепт и пул | CompositEntityPool | Разобранные компоненты, диапазоны иерархии, варианты-прототипы, размещение и живые экземпляры |
| Экземпляр | CompositEntity | Матрица, два сида, флаги, индекс своего диапазона дочерних сущностей |
| Исполнители типов | rendInst/prefab/gameObj/spline и другие сервисы | Реальные ресурсы, рендер, коллизия, специализированное поведение |

Пул создаётся на ассет. Иерархия исходного документа кодируется плоскими компонентами и диапазонами beginInd/endInd. Варианты ссылаются на виртуальные сущности соответствующих сервисов. Вложенный composit остаётся обращением к собственному пулу. Это принципиально: плоское внутреннее представление документа не означает потерю границ между вложенными рецептами. [D2]

Загрузка нормализует веса вариантов; создаёт прототипы; разбирает параметры трансформа и размещения. Инстанцирование выбирает варианты, клонирует сущности, передаёт сиды, dataBlockId и другие свойства. Недостающий ресурс в ряде путей даёт NULL с диагностикой, а не отказ всего документа. Эту более мягкую политику не следует автоматически переносить в наш строгий source protocol. [D2]

Преимущество пулов не означает O(1) для огромного дома. Сама материализация и изменение матриц оплачиваются числом затронутых сущностей; преимущество в повторном использовании разобранного рецепта, ресурсов и лёгких записей. Формула «любой composit создаётся бесплатно» неверна.

**2. Как создаётся, редактируется и обновляется Dagor composit.**

Есть несколько разных операций; их нельзя объединять под одним словом Build:

1. Создать пустой ассет в AssetViewer: записать файл с className=composit, дать asset manager обнаружить его. [D3]
2. Export as composit в HeightmapLand: собрать выбранные сущности и сплайны и записать новый BLK. Pivot берётся из позиции первой выбранной сущности; дочерние матрицы получают соответствующий сдвиг. Можно предварительно развернуть вложенные композиты во временном наборе. Исходная сцена не заменяется одним объектом, поэтому эта команда сама по себе не является аналогом нашего Build-and-replace. [D4]
3. Save selected nodes as a new composite в AssetViewer: выделенные sibling-узлы копируются в новый рецепт с локальными матрицами. При replace-selection они заменяются ссылкой на новый ассет. Это редактирование дерева рецепта. [D5]

Изменение дерева AssetViewer преобразуется обратно в DataBlock и проходит через уведомление об изменении ассета. Внешние изменения BLK/include тоже идут через asset manager. Undo дерева хранит сериализованный DataBlock и выбор узлов; рендерное представление пересоздаётся. [D5][D6][D7]

При изменении composit перестраиваются экземпляры соответствующего пула. Родительская сущность продолжает ссылаться на дочернюю сущность. При изменении rendInst работу выполняет сервис ресурса: его onAssetChanged вызывает init0/init1 и callback обновления; это не повод заново выбирать варианты всех композитов. Нельзя обещать, что реимпорт любого листа всегда равен ровно одной дешёвой операции: он может обновлять рендерные и коллизионные ресурсы. [D2][D8]

Во время интерактивного перемещения Dagor откладывает часть обновлений riExtra grid; placement зависит от режима gizmo. Иначе большой объект платил бы за повторную перестройку пространственных структур на каждое движение мыши. [D2]

**3. Как работает Split.**

Базовая операция проходит по фактическим дочерним сущностям ICompositObj. Для каждой создаёт запись уровня с именем ассета, текущей world-матрицей, свойствами размещения и фактическими сидами. Вложенный composit становится самостоятельным composit. Структурные группы без собственной сущности не обязаны становиться объектами уровня. Поэтому «один слой» означает границу вложенного composit-ассета, а не только непосредственных XML/BLK-детей корня. [D9]

Random уже разрешён: split берёт существующую выбранную сущность. Если ребёнок поддерживает IRandomSeedHolder, используются его getSeed/getPerInstanceSeed; сиды родителя — запасной путь для типов без этого интерфейса. В UI есть отдельная опция Recursive, которая повторяет split до конечных сущностей. Без неё вложенные рецепты сохраняются. [D9]

Внутренний composit получает layout-сид при клонировании после выбора варианта родительского компонента. Передача до построения поддерева предотвращает лишнюю первоначальную генерацию. Пара сидов ребёнка после split соответствует его контексту внутри родителя. [D2]

Split не просто «переподвешивает старые указатели»: код создаёт LandscapeEntityObject, удаляет родительские объекты и добавляет новые через undo-систему. onRemove уничтожает исполняемую сущность; onAdd восстанавливает её из записи. Быстрота объясняется лёгкими записями и пулами, а не отсутствием пересоздания вообще. [D9]

**4. Runtime-исключение.**

Фраза «движок не знает composit» описывает обычный экспорт из редактора. В этом же репозитории есть отдельная daNetGameLibs/composite_entity ECS-система: shared nodes, select_one, nested, spawn дочерних entity, обновление их transform и удаление с родителем. Это другой исполнитель того же общего принципа «данные + генератор», а не доказательство идентичности BLK- и ECS-семантик. [D10]

**5. Как я проектировал бы такую сущность в UE5.**

Я сохранил бы пользовательскую сущность «рецепт + размещение» и дал ей специализированный исполнитель. Blueprint/Level Instance могут участвовать как содержимое или форма результата, но не заменяют контракт случайных вариантов, стабильных узлов, reimport и внешнего source-пайплайна. Epic описывает Level Instance как повторяемый набор Actors, а Packed Level Blueprint как оптимизированную сборку Static Mesh. Это полезные соседние механизмы. [E1]

Предлагаемая цепочка:

    Авторский документ
        -> общий неизменяемый скомпилированный рецепт
        -> экземпляр (recipe, transform, random context, overrides)
        -> вычисление выбранного результата
        -> ISM-пулы / адаптеры Actors / служебные узлы

    Импорт и проверка источников -> обновление рецептов и прототипов
    Proof/cook -> проверенная игровая форма

Разделение ответственности:

| Часть | Что она должна хранить/делать |
|---|---|
| Recipe asset | Семантические узлы, стабильные NodeId, порядок, ссылки, варианты, диапазоны, вложенные вызовы |
| Compiled recipe | Разобрать один раз на ревизию; общие узлы и ссылки на другие рецепты |
| Instance record | RecipeId, transform, layout/appearance context, локальные overrides |
| Evaluator | Детерминированно выбрать результат без source scan, загрузок UObject и создания компонентов |
| Resource registry | Общие ресурсы и состояния Unresolved/Loading/Ready/Invalid, ревизии интерфейсов |
| Materializer | Превратить выбранные листья в данные ISM или реальные Actors через адаптеры |
| Editor projection | Outliner, hit selection, gizmo и Undo через соответствие handle ↔ instance/node |
| Cook backend | Для статической сцены — bake выбранного результата; для динамического runtime — общий cooked recipe + компактные записи экземпляров |

Обычные меши группировать по полному совместимому дескриптору, не только по указателю UStaticMesh: материалы, collision, mobility, shadow/render flags, layout custom data. У ISM ряд свойств общий на компонент; appearance передавать через per-instance custom data. ISM/HISM выбирать под движение, Nanite и профиль нагрузки, а не объявлять HISM универсально быстрее. [E2]

Граница пула сначала ULevel. Объединение через World Partition cell/Data Layers потребует отдельного решения о времени жизни, streaming, видимости и выборе. Не следует создавать один глобальный мегапул на весь World.

Стабильный пользовательский handle должен переживать уплотнение массивов ISM. Числовой InstanceIndex — адрес текущего представления, а не identity узла. В новой модели NodeId также отделён от порядкового индекса и от отображаемого имени.

Обновления должны иметь разные пути: transform меняет матрицы; appearance меняет custom data; layout seed пересчитывает решения; geometry refresh обновляет ресурс; изменение collision/material interface затрагивает соответствующие backend-объекты; изменение рецепта — его экземпляры или поддеревья. Undo хранит авторские записи, а не сгенерированную геометрию.

Для Build/Break я ввёл бы главный инвариант: **структурная операция сохраняет наблюдаемый результат, пока пользователь явно не просит reroll или bake с потерей семантики**. Сюда входят выбранные ресурсы, world-матрицы, appearance, поддерживаемые свойства и служебные узлы. Непредставимое состояние надо отклонять до замены объектов либо явно фиксировать как отдельный режим.

Это предложение для новой модели. В действующем MH нельзя просто заменить RNG или грамматику NodePath: текущие байты закреплены контрактом. Для совместимости потребуется версионированный контекст вызова или явные overrides; старые ассеты должны воспроизводиться прежним resolver. [M10]

**6. Что уже сделано у нас правильно.**

Фактическая цепочка: Dagor BLK/dag4blend collections -> преобразование в MH .composite/.placement -> импорт в UMHCompositeAsset -> UMHCompiledRecipeRegistry -> MHMaterializeLayout -> AMHCompositeActor и компоненты preview. Для PIE/cook существует отдельный snapshot/runtime bridge. [M1][M2][M3][M11][M15]

На проверенном main уже есть:

- Разделение source, preview и proof; preview не строит старый полный applied-proof граф.
- Общий реестр скомпилированных рецептов и ссылки на вложенные рецепты.
- Реестр endpoint с identity admission и детерминированными путями.
- Раздельные layout и appearance сиды.
- ISM-группировка мешей внутри одного composite actor и частичный reseed-diff.
- Break по текущему preview-плану с сохранением вложенных composite actors.
- Исправление Undo: derived plan-view components не транзакционны, восстановление идёт из записи актора.
- Проверка непредставимых world-преобразований до материализации.
- Отдельная строгая проверка полного замыкания на выходах в build/snapshot/cook.

Это действующие улучшения. Старое замечание «Break всегда flatten-ит всё и дублирует после Undo» к этому main не применимо. Статус R4-pre уже MERGED; owner-проверка Undo записана в текущем tracker. [M14]

**7. Проблемы: подтверждённые потери результата.**

**A. Break вложенного random-композита может изменить и геометрию, и appearance. Приоритет высокий.**

MHCollectBreakSpecs присваивает каждому ребёнку Seed и AppearanceSeed корневого актора. Однако поток MH определяется парой Seed + hash(NodePath), а appearance — также boundary path. Превращение вложенного ребёнка в корень меняет эти пути. Равенство двух int не обеспечивает равенства результата. Это известное OPEN-R4P-1; исполнение R4-pre соответствует временно принятому правилу, но продуктовый дефект остаётся. [M4][M5][M14]

Минимальная проверка существующим Python reference resolver:

| Параметр | До Break, ребёнок внутри root | После переноса ребёнка в корень |
|---|---|---|
| Layout Seed | 0 | 0 |
| Appearance Seed | 34 | 34 |
| Random node path | root_cmp:nodes[0]>child_cmp:nodes[0] | child_cmp:nodes[0] |
| Выбранный ресурс при двух равных весах | mesh_b | mesh_a |
| Raw draw | 4224114907 | 646050620 |
| Appearance boundary | root_cmp | child_cmp |
| Appearance raw u32 | 3015928452, 1791264275, 189237491, 1236852751 | 4167548070, 997018091, 3672239497, 573612910 |

Это выполненная проверка алгоритма, не запуск Break в UE Editor. Связь с UE подтверждается чтением MHMakeNodeRandomStream, WalkNode и MHCollectBreakSpecs. [M4][M5]

Простое предложение «взять child seed как в Dagor» недостаточно: у нас нет аналогичного единого передаваемого running seed; действует path-derived RNG. Нужен сохраняемый контекст исходного namespace и appearance boundary либо фиксированные решения/overrides. Для нового формата возможны независимые потоки вложенных вызовов со стабильными NodeId.

**B. Build не сохраняет индивидуальное состояние выбранных Actors. Приоритет высокий.**

MHBuildNodeForActor берёт transform, label, mesh resource / composite resource / зарегистрированный actor token. Для composite не копируются его Seed/AppearanceSeed. Для StaticMeshActor не копируются OverrideMaterials, custom data и другие instance properties; проверки на их отсутствие в этой функции также нет. Для ActorClassRegistry сохраняется класс-токен, а не значения instance properties. Затем исходные Actors удаляются. [M4]

Примеры: два экземпляра одного дочернего recipe с разными сидами; меш с material override; actor с изменённым параметром относительно CDO. Build принимает такие объекты, но документ содержит недостаточно данных для воспроизведения выбранного состояния. Для случайного ребёнка сама смена namespace уже показана проверкой A. Для остальных свойств это доказанная неполнота переноса по коду; визуальные сценарии в UE в этом исследовании не запускались.

Исправление по смыслу: определить сериализуемый contract каждого поддерживаемого типа и либо сохранять его состояние, либо отказать до замены. «Recipe from selection» и «reroll selected recipes inside new parent» — разные пользовательские действия.

**C. Break в обычный меш теряет appearance-каналы. Приоритет высокий для материалов, использующих эти каналы.**

FMHBreakSpawnSpec не несёт вычисленные четыре appearance channel. Ветка Mesh после AddActor выполняет SetStaticMesh и не применяет MHApplyLeafAppearanceCustomData. Сиды в Spec этого не исправляют: AStaticMeshActor их не вычисляет. Выбранная геометрия сохраняется, цвет/вариация материала могут измениться. Нужен перенос вычисленных каналов на новый StaticMeshComponent. [M4]

Это отдельный дефект от A: воспроизводимая семантическая причина существует даже у композита без случайного выбора геометрии.

**8. Проблемы: стоимость исполнения.**

**D. Общий плоский рецепт снова превращается в отдельное дерево на каждый rebuild актора.**

MHMaterializeLayout создаёт новый Graph и Plan. MHBuildRecipeGraph через RecipeNodes восстанавливает рекурсивное дерево из Components, затем Gather обходит все ссылки на дочерние recipes, включая варианты, которые могут не быть выбраны. Актор хранит такой граф и resident plan. Следовательно, кэш компиляции не равен общему исполняемому графу без копирования. [M2][M3]

Для A экземпляров с одним графом размера G это даёт вклад порядка A×G по копированию/хранению структуры, помимо неизбежных индивидуальных выбранных листьев. Материализатор ещё строит Result.Placements, но данный путь AMHCompositeActor использует Graph/Plan и затем заново проходит Plan в placement compiler. Это дополнительный временный массив. [M1][M3]

Приоритет: уменьшить дублирование и исполнять общий граф/рецепт напрямую, сохранив shadow parity. Не удалять нужные Outliner и reseed-diff данные только ради формального уменьшения структуры.

**E. Реимпорт меша вызывает полный rebuild зависимых root Actors.**

MHNotifyGeneratedResourceChanged перебирает все живые AMHCompositeActor. DependsOnResource заново собирает набор из resident graph, включая невыбранные mesh options. Для совпадения вызывается RebuildComposite. Это не перекомпилирует все рецепты, но повторно материализует план и представление подходящих Actors. [M6][M1][M3]

В полном compile пути повторно используются совместимые компоненты, однако ISM получает ClearInstances и AddInstance для всего bucket. Нельзя описывать это как точечный refresh геометрии. Кроме того, proof cache инвалидируется целиком. [M7][M6]

Нужны отдельные причины изменения и подписки resource -> реально затронутые buckets/instances. В текущем endpoint есть одна Revision; пять ревизий/хэшей уже запланированы в R3. Это незавершённая архитектура, а не пропущенная реализация в срезе R4-pre. [M8][M14]

**F. Пулы editor-preview ограничены одним actor, а runtime создаёт компонент на лист.**

MHCompileCompositePlacementV5 собирает локальный Buckets и принимает previous bucket только с GetOwner()==Target. Поэтому A домов с B совместимыми категориями дают приблизительно A×B ISM-компонентов, а не B на допустимый общий уровень. Это расчёт структуры объектов, не измерение draw calls. [M7]

Поиск bucket через IndexOfByPredicate и поиск предыдущих bucket линейные. В худшем случае при множестве разных дескрипторов группировка приближается к квадратичной; скорость на типичном небольшом наборе дескрипторов надо измерять отдельно. [M7]

AMHRuntimeCompositeActor::Materialize создаёт UStaticMeshComponent на каждый mesh leaf и UChildActorComponent на actor leaf. В BeginPlay запускается rebuild; BuildCandidate декодирует GraphBytes, валидирует bindings и повторно вызывает resolver с proof-этапами. Следовательно, editor ISM-оптимизация не переносится автоматически в игру. [M9]

Для статического наполнения я предпочёл бы cook-bake в оптимизированные группы. Если runtime reseed необходим, нужен общий runtime recipe и тот же принцип эффективной материализации, с тестом component count отдельно от visual parity.

**G. Синхронная загрузка и ожидание mesh compilation остаются.**

Endpoint Admit использует LoadObject; GetCompositeAsset — LoadSynchronous. PlanViewWaitSelectedMeshes вызывает FinishCompilation для реально компилирующихся выбранных мешей. R1.1 уже сузил ожидание правильно, но первый холодный load/reseed может блокировать поток редактора. Это причина возможных пауз, а не измеренная в этом исследовании задержка. Async-состояние Loading объявлено, но зарезервировано для R4. [M8][M7][M1]

**9. Ограничения продукта и авторинга.**

- **Build смешивает создание ресурса и замену сцены.** Он требует Source Root, создаёт анализ/индекс, проверяет closure, публикует .composite и импортирует его, затем открывает UE-транзакцию замены Actors. Это больше работы, чем Dagor Export. При отказе после публикации внешний файл уже может существовать; UE Undo замены сцены не отменяет его публикацию. Нужны явная граница Prepare/Publish/Replace и определённое поведение при частичном отказе. Нельзя ускорять этот путь простым удалением source-проверок. [M4]
- **Commit Edit очищает общую UE-историю Undo.** ResetTransaction вызывается до публикации. При неудаче есть попытка reconciliation из авторитетного source, но прежняя Undo-история уже сброшена. Это сознательная граница текущего source-пайплайна и серьёзная UX-цена. Для нового редактора предпочтительна staged authoring session с отдельно определённым сохранением во внешний источник. [M4]
- **Identity привязана к позиционному NodePath.** Вставка перед узлом, смена родителя или root name меняют stream. NodeFingerprint пока равен нулю, NodeOverrides запланированы на R6. Стабильный идентификатор узла и порядок следует разделять в следующей версии, сохраняя прежнее воспроизведение для старой. [M2][M5][M14]
- **gameobj и place_type не равны полному Dagor-поведению.** GameObj сохраняется в семантическом плане, но не создаёт исполняемый leaf; Break его пропускает. PlaceType в asset отмечен как provenance. Импорт метаданных нельзя выдавать за поддержку игровых сущностей и размещения на поверхности. Это принятые границы, а не новая регрессия. [M4][M5][M16]
- **World Partition runtime пока закрыт явно.** Runtime bridge возвращает ошибку для cook/PIE с composite placements в WP. Это честный отказ, но он ограничивает применение в больших мирах; общие пулы сами по себе его не устранят. [M11]

**10. В каком порядке двигаться.**

1. Сначала закрепить сохранение результата через Build/Break: nested random, appearance, material overrides и actor instance state. Выбрать contract контекста вызова, не менять замороженный RNG скрыто.
2. Закрыть R3: причины изменения ресурсов, адресный refresh, отсутствие полной materialization при замене только геометрии.
3. Закрыть R4: async-загрузка, отмена устаревших запросов, placeholders без ожидания FinishCompilation в интерактивном пути.
4. R5: общие пулы по ULevel, стабильные handles и пакетные обновления. Отдельно измерить повторение одного recipe много раз.
5. Отдельно закрыть runtime: компонентная стоимость и выбор bake/shared recipe. Проверить WP до обещаний поддержки больших карт.
6. R6/R7: overrides, полноценное редактирование и capability-contract actor-листьев. Оптимизация shared graph/evaluator идёт под существующим parity gate.

Проверки при будущей реализации: Build -> Break сохраняет выбранные ресурсы, матрицы и appearance; Undo/Redo сохраняет количество сущностей; geometry-only reimport не вызывает resolve/materialize; изменение невыбранного mesh option не пересобирает текущую геометрию; 1000 экземпляров одного recipe не содержат 1000 копий его структуры; runtime component count соответствует backend-политике.

**11. Что проверено в этом исследовании.**

- Прочитаны Dagor manager/pools, split/export, AssetViewer authoring/undo, resource reload, reference provider и ECS runtime.
- Прочитаны MH compiled recipe, materialization, actor lifecycle, events, build/break/edit, endpoint registry, runtime bridge и runtime materializer; сверены действующие контракты/статусы.
- Выполнен минимальный пример смены random namespace, результат в таблице A.
- Выполнено: python -B -m pytest -q -p no:cacheprovider tests/test_random_reference.py tests/test_appearance_reference.py tests/test_dagor_random_parity_probe.py — **50 passed in 0.51s**.
- Сборка UE, viewport/PIE/cook и профилирование портфолио не запускались. Численные оценки A×G/A×B — анализ алгоритмов, не замеры производительности.
- Исследование выполнено на чистом checkout указанного SHA. Коммит публикации содержит только этот отчёт и запись в справочном индексе.

Результаты прежних исследований использованы как навигация и проверены по коду. Особое уточнение: у нас **поведенческая совместимость с Dagor, собственный RNG**, а не bit-for-bit паритет. Это прямо закреплено актуальным golden/v5/dagor_random_probe/README.md. В прежнем исследовании формулировку про паритет потока нельзя переносить в новые выводы. [M10]

**Воспроизведение проверки A на зафиксированном MH SHA.**

Из корня репозитория, в Python-окружении проекта:

```python
from tools.mh_random_reference import (
    Composite, Node, RandomOption, ResourceKey, raw_payload_hash,
    resolve_composite, resolve_appearance,
)

child = Composite("child_cmp", (Node("random", options=(
    RandomOption("mesh", 1, "mesh_a"),
    RandomOption("mesh", 1, "mesh_b"),
)),))
root = Composite("root_cmp", (Node("composite", resource="child_cmp"),))
composites = {"root_cmp": root, "child_cmp": child}
hashes = {
    ResourceKey(kind, name): raw_payload_hash((kind + ":" + name).encode())
    for kind, name in (
        ("composite", "root_cmp"), ("composite", "child_cmp"),
        ("static_mesh", "mesh_a"), ("static_mesh", "mesh_b"),
    )
}
for name in ("root_cmp", "child_cmp"):
    plan = resolve_composite(name, 0, composites, {}, hashes)
    appearance = resolve_appearance(plan, 34).leaves[0]
    print(name, plan.decisions[0].path, plan.leaves[0].resource,
          plan.decisions[0].raw_u32, appearance.boundary, appearance.raw_u32)
```

Ожидаются две строки: root_cmp выбирает mesh_b, child_cmp выбирает mesh_a; raw draw и appearance соответствуют таблице A. Проверка моделирует смену корня, которую производит текущий Break, и не подменяет будущий интеграционный тест операции в UE Editor.

**Исходники и точные точки входа.**

[D1]: https://github.com/GaijinEntertainment/DagorEngine/blob/75723669297e48e200a0dc67b18c1629e0975daf/_docs/source/assets/all-about-blk/composit_blk.md#L3-L38
[D2]: https://github.com/GaijinEntertainment/DagorEngine/blob/75723669297e48e200a0dc67b18c1629e0975daf/prog/tools/sceneTools/daEditorX/services/compositMgr/compositMgrService.cpp
[D3]: https://github.com/GaijinEntertainment/DagorEngine/blob/75723669297e48e200a0dc67b18c1629e0975daf/prog/tools/AssetViewer/compositeAssetCreator.cpp#L99-L115
[D4]: https://github.com/GaijinEntertainment/DagorEngine/blob/75723669297e48e200a0dc67b18c1629e0975daf/prog/tools/sceneTools/daEditorX/HeightmapLand/hmlImportExport.cpp#L164-L316
[D5]: https://github.com/GaijinEntertainment/DagorEngine/blob/75723669297e48e200a0dc67b18c1629e0975daf/prog/tools/AssetViewer/Entity/compositeEditor.cpp#L1037-L1137
[D6]: https://github.com/GaijinEntertainment/DagorEngine/blob/75723669297e48e200a0dc67b18c1629e0975daf/prog/tools/AssetViewer/Entity/compositeEditorUndo.cpp#L7-L75
[D7]: https://github.com/GaijinEntertainment/DagorEngine/blob/75723669297e48e200a0dc67b18c1629e0975daf/prog/tools/libTools/assetMgr/assetMgrTrackChanges.cpp
[D8]: https://github.com/GaijinEntertainment/DagorEngine/blob/75723669297e48e200a0dc67b18c1629e0975daf/prog/tools/sceneTools/daEditorX/services/riMgr/riMgrServiceAces.cpp#L1056-L1070
[D9]: https://github.com/GaijinEntertainment/DagorEngine/blob/75723669297e48e200a0dc67b18c1629e0975daf/prog/tools/sceneTools/daEditorX/HeightmapLand/hmlEntity.cpp#L1489-L1634
[D10]: https://github.com/GaijinEntertainment/DagorEngine/blob/75723669297e48e200a0dc67b18c1629e0975daf/prog/daNetGameLibs/composite_entity/main/composite_entity_es.das#L36-L130
[E1]: https://dev.epicgames.com/documentation/en-us/unreal-engine/level-instancing-in-unreal-engine
[E2]: https://dev.epicgames.com/documentation/en-us/unreal-engine/instanced-static-mesh-component-in-unreal-engine
[M1]: https://github.com/helmdubo/MH_blender_bridge/blob/c5a951ba6ea1db3e8a430388b8c9c1b46f11ab3a/ue/MimirComposite/Source/MimirCompositeEditor/Private/Composite/MHCompositeActor.cpp#L399
[M2]: https://github.com/helmdubo/MH_blender_bridge/blob/c5a951ba6ea1db3e8a430388b8c9c1b46f11ab3a/ue/MimirComposite/Source/MimirCompositeEditor/Private/Composite/MHCompiledRecipe.cpp#L116
[M3]: https://github.com/helmdubo/MH_blender_bridge/blob/c5a951ba6ea1db3e8a430388b8c9c1b46f11ab3a/ue/MimirComposite/Source/MimirCompositeEditor/Private/Composite/MHMaterializeLayout.cpp#L40
[M4]: https://github.com/helmdubo/MH_blender_bridge/blob/c5a951ba6ea1db3e8a430388b8c9c1b46f11ab3a/ue/MimirComposite/Source/MimirCompositeEditor/Private/Composite/MHCompositeLevelSubsystem.cpp#L103
[M5]: https://github.com/helmdubo/MH_blender_bridge/blob/c5a951ba6ea1db3e8a430388b8c9c1b46f11ab3a/ue/MimirComposite/Source/MimirCompositeRuntime/Private/Random/MHRandomStream.cpp#L587
[M6]: https://github.com/helmdubo/MH_blender_bridge/blob/c5a951ba6ea1db3e8a430388b8c9c1b46f11ab3a/ue/MimirComposite/Source/MimirCompositeEditor/Private/Composite/MHCompositePlacementEvents.cpp#L43
[M7]: https://github.com/helmdubo/MH_blender_bridge/blob/c5a951ba6ea1db3e8a430388b8c9c1b46f11ab3a/ue/MimirComposite/Source/MimirCompositeEditor/Private/Composite/MHCompositePlacementCompiler.cpp#L1040
[M8]: https://github.com/helmdubo/MH_blender_bridge/blob/c5a951ba6ea1db3e8a430388b8c9c1b46f11ab3a/ue/MimirComposite/Source/MimirCompositeEditor/Private/Composite/MHEndpointPrototypeRegistry.cpp#L257
[M9]: https://github.com/helmdubo/MH_blender_bridge/blob/c5a951ba6ea1db3e8a430388b8c9c1b46f11ab3a/ue/MimirComposite/Source/MimirCompositeRuntime/Private/Composite/MHRuntimeCompositeActor.cpp#L77
[M10]: https://github.com/helmdubo/MH_blender_bridge/blob/c5a951ba6ea1db3e8a430388b8c9c1b46f11ab3a/golden/v5/dagor_random_probe/README.md#L1
[M11]: https://github.com/helmdubo/MH_blender_bridge/blob/c5a951ba6ea1db3e8a430388b8c9c1b46f11ab3a/ue/MimirComposite/Source/MimirCompositeEditor/Private/Composite/MHCompositeRuntimeBridge.cpp#L280
[M14]: https://github.com/helmdubo/MH_blender_bridge/blob/c5a951ba6ea1db3e8a430388b8c9c1b46f11ab3a/docs/RECIPE_EXECUTION_STATUS.md#L27
[M15]: https://github.com/helmdubo/MH_blender_bridge/blob/c5a951ba6ea1db3e8a430388b8c9c1b46f11ab3a/addon/mh4blend/scene/import_dagor_composite.py#L329
[M16]: https://github.com/helmdubo/MH_blender_bridge/blob/c5a951ba6ea1db3e8a430388b8c9c1b46f11ab3a/ue/MimirComposite/Source/MimirCompositeEditor/Public/Composite/MHCompositeAsset.h#L151

Дополнительные точные диапазоны MH: Break сбор 187–295 / spawn 300–368; Build 388–610; Commit Edit 803–870 в MHCompositeLevelSubsystem.cpp. Runtime WP admission — MHCompositeRuntimeBridge.cpp:280; создание snapshot — :500. Копирование графа — MHCompiledRecipe.cpp:116–173; зависимости невыбранных вариантов — MHMaterializeLayout.cpp:21–37; ISM ClearInstances — MHCompositePlacementCompiler.cpp:1165; синхронное ожидание — :480–495; тесты текущего Break — MHCompositeBreakTest.cpp:226–236.
