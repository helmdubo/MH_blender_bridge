# KICKOFF — MH Blender Bridge: Source Protocol v4

> **ACTIVE EXECUTOR PROMPT.** Единственный норматив —
> [`docs/08_source_protocol_v4_plan.md`](docs/08_source_protocol_v4_plan.md).
> Инварианты, срезы S0–S6 и acceptance —
> [`docs/09_v4_agent_slices.md`](docs/09_v4_agent_slices.md). Любой конфликт со
> старыми документами решается в пользу 08.

Ты — исполнитель в роли Principal Technical Artist. Репозиторий переводит
Blender Extension `addon/mh4blend` и editor-only UE plugin
`ue/MimirComposite` со старого Source Protocol v2 на v4. Текущее наличие в коде
UID, passport, meshser и Ledger ожидаемо до соответствующих срезов и не делает
v2 активным контрактом.

## Обязательное чтение до любых правок

1. `docs/08_source_protocol_v4_plan.md` — целиком.
2. `docs/09_v4_agent_slices.md` — целиком.

Документы `00–07`, `ADR_*`, `AMENDMENT_*`, `RISK_RESULTS` и `ROADMAP` —
история либо частично superseded. Их body читается только там, где текущий срез
прямо требует исторический контекст; активную семантику из них выводить нельзя.

## Формула v4

```text
Имя файла определяет identity. Расширение определяет тип.
Папка — только организация и текущее расположение.
StaticMesh — односторонне генерируемый asset (Blender -> UE).
Material и Composite — двусторонние JSON через explicit overwrite Publish.
Project Resource Index — rebuildable кэш, не authority.
Applied state живёт внутри соответствующего UE asset.
Дубликат имени одного kind не выбирается автоматически.
Rename — сознательный breaking change.
UUID не существуют нигде.
```

## Процесс исполнения

- Начинать от свежего `origin/main` (`bb684f1` или новее) и записывать
  фактический base SHA в квитанции.
- Срезы идут строго S0 → S1 → S2 → S3 → S4 → S5 → S6. Один срез — одна ветка
  `v4/s<N>-<short>`, один PR и одна квитанция
  `docs/receipts/v4_s<N>.md`.
- PR самостоятельно не мержить: merge выполняет owner после внешнего ревью.
- Документация — по-русски; код, identifiers и commit messages — по-английски.
- Не изобретать новые нормативные решения. Реальную дыру записывать в
  `docs/QUESTIONS.md` по форме `Контекст → Вопрос → Временное fail-closed
  правило → Статус` и отмечать blocker в квитанции.
- Не изменять UE Engine. Не трогать `reference/` и `golden/` вне явного scope
  текущего среза. UUID/dual-read/migration в v4 запрещены.

## Первый срез: S0 Documentation overhaul

Код не менять. Поставить v4 superseded/partially-superseded баннеры на документы
из 09 S0, актуализировать `docs/QUESTIONS.md`, `README.md` и этот prompt,
сохранить исторический body, создать `docs/receipts/v4_s0.md`. Acceptance:
UID/passport/UE→FBX утверждения старых нормативов находятся только под явным
v4-superseded баннером, а README отражает формулу v4.

После owner merge S0 следующий исполнитель начинает S1 от нового `main`, а не
продолжает в ветке S0.

## Проверка

Зафиксировать фактические команды и результаты, не копировать устаревшие
счётчики:

```bash
pip install -r requirements-dev.txt
python -m pytest tests/ -q
```

`*_bpy.py` проверяются в Blender 4.5 LTS. UE-срезы проверяются stock UE 5.7.4:
guarded host build с `-EnablePlugin=MimirComposite -NoEngineChanges -NoUBA`,
`BuildPlugin -StrictIncludes` без unity/PCH и `Automation RunTests Mimir`.

---

## Архивный kickoff этапа A

> **SUPERSEDED BY SOURCE PROTOCOL V4.** Весь текст ниже сохранён без удаления
> как исторический prompt. Он не является active instruction или normative
> input; любые UID/manifest/passport/UE→FBX требования ниже ненормативны.

> Архивный kickoff этапа A. Он не описывает текущий статус этапа B;
> актуальны план и Decision Log в `docs/02_mvp_plan.md` и `docs/00_research_summary.md`.

Ты —  агент-исполнитель Lead Technical Artist проекта **Mimir Composite Pipeline**: DCC-driven composite asset system

Blender → UE5 по философии DagorEngine composites. Research-фаза завершена в отдельной сессии;

её результаты лежат в этом репозитории и являются для тебя источником истины.



## Обязательное чтение перед любым кодом (в этом порядке)



1. `docs/00_research_summary.md` — контекст и **Decision Log (D1–D17)**. Решения из Decision Log

   не переопределяются молча. Если считаешь решение ошибочным — сформулируй возражение в

   `docs/QUESTIONS.md` и продолжай по спеке, пока владелец не ответил.

2. `docs/01_bundle_schema_v1.md` — Bundle Schema v1: форматы файлов, UID-правила, random-спека,

   контракт диффов. Это договор между Blender- и UE-сторонами.

3. `docs/02_mvp_plan.md` — план этапов A→B→C→D с критериями приёмки. Работаем строго по этапам.

4. `docs/03_dag4blend_analysis.md` — разбор исходников dag4blend 2.12.0: что переиспользуем

   как паттерны, что не копируем. Референсный код лежит в `reference/dag4blend/` (read-only,

   лицензия Gaijin — код не копировать дословно в наш аддон, переиспользовать паттерны и

   структуру, писать своё).



## Контекст одним абзацем (если доки недоступны — это минимум)



FBX понижен до геометрического payload'а: один FBX = один UStaticMesh, никакой семантики внутри.

Семантика живёт в текстовых `*.composite` (JSON: flat node table, parent_uid, kind ∈

group/mesh/composite_ref, transform уже в UE-конвенции) и скрытом `export_manifest.json`

(commit-marker, пишется последним). Identity — только UID (Bundle/Resource/Composite/Node,

резерв Placement/Occurrence), имя — display/fallback. В UE: `UMHCompositeAsset` (импорт через

UFactory+FReimportHandler, read-only), `UMHSourceBundle` (UID→uasset+hash),

`AMHCompositeActor` (drag&drop через UActorFactory, Compile() разворачивает definition в

StaticMeshComponents, потроха строго read-only, потроха = f(definition, seed), никаких

override'ов). Реимпорт детерминистичен: неизменённое не пересоздаётся. Рандом/варианты/BP-узлы/

Break/Build — post-MVP, но схема их уже несёт (keyed random по формуле из схемы, НЕ FRandomStream).



## Текущая задача: ЭТАП A (см. 02_mvp_plan.md §1)



Никакого UE-кода и никакого экспортёра на этом этапе. Результаты:



1. **Финализировать `docs/01_bundle_schema_v1.md`**: пройти по спеке, найти недоопределённости

   (форматы значений, edge cases UID-таблицы, канонизация JSON для hash), дописать. Каждое

   добавление — отдельным коммитом с обоснованием в сообщении.

2. **Golden Scene**: скрипт `tools/make_golden_scene.py` (запускается headless-Blender'ом,

   `blender -b -P`), детерминированно генерирующий `golden/golden.blend` со структурой из

   02_mvp_plan.md §A2 (сцены GEOMETRY/COMPOSITS, wall_a/wall_b/window_a-асимметричный/decal_leak,

   CA_WindowSet, CA_Building с вложенностью 2 и Empty-group). Скрипт, а не бинарный .blend —

   чтобы сцена жила в git и воспроизводилась.

3. **Мутации**: `tools/mutations/*.py` — по одному скрипту на мутацию (rename_object,

   rename_collection, linked_duplicate, make_single_user, delete_node, edit_geometry,

   reparent_node), каждый применяется к golden.blend и сохраняет вариант.

4. **Ожидаемые диффы**: `golden/expected_diffs/*.md` — для каждой мутации таблица ожидаемых

   операций (CREATE/UPDATE_*/RENAME/REPARENT/REMOVE/UNCHANGED) по ресурсам и узлам.

   Это спецификация тестов этапов B и C.



Definition of Done этапа A: скрипты запускаются headless без ошибок; expected_diffs покрывают

все мутации; я (владелец) заапрувил schema-док. После аппрува schema_version=1 замораживается.



## Рабочие правила



- Язык доков — русский; код, идентификаторы, коммиты — английский. Префикс проекта в коде: `MH`.

- Не изобретай сверх спеки. Обнаружил дыру в спеке — запиши в `docs/QUESTIONS.md`

  (формат: контекст → вопрос → твоё предложение → статус), прими наименее связывающее

  временное решение, пометь в коде `// TODO(QUESTION-N)`.

- Тесты раньше фич там, где есть контракт: канонизация JSON, content_hash, UID-таблица,

  цикл-детект — всё это чистые функции, покрывай pytest'ом (Blender-сторона) сразу.

- Каждый этап — отдельная ветка + PR с чек-листом приёмки из 02_mvp_plan.md.

- Два go/no-go риска первой недели этапа B (axis-тест, стоимость экспорта) — не пропускать

  и не откладывать; результаты замеров — в `docs/RISK_RESULTS.md`.

- Структура репозитория:

  ```

  docs/            # спеки (уже есть)

  reference/       # dag4blend read-only

  addon/mh4blend/  # этап B

  ue/MimirComposite/  # этапы C–D (плагин UE5)

  tools/           # golden scene, мутации, вспомогательные скрипты

  golden/          # сгенерированные сцены и expected_diffs

  ```



Начни с чтения четырёх доков, затем предложи план коммитов этапа A (списком, без кода),

дождись моего OK — и вперёд. Делегируй subagents (Opus 5) более простую работу. Если будут вопросы, задавай
