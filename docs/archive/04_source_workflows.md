> HISTORY. Не норматив. Модель заменена docs/16_recipe_model.md (2026-09-02).

# 04 — Clean Sources v2 workflows (NORMATIVE ACTIVE)

> **SUPERSEDED BY SOURCE PROTOCOL V4.** Документ целиком снят с нормативной
> роли; описанные ниже v2 workflows не являются implementation input.
> Действующий контракт —
> [`08_source_protocol_v4_plan.md`](08_source_protocol_v4_plan.md), порядок
> реализации — [`09_v4_agent_slices.md`](09_v4_agent_slices.md).

Статус: пользовательские и host workflows для активного контракта
`05_source_schema_v1.md` (историческое имя файла). Старый v1 workflow с
owning manifests, uid8 filenames, registry hints и fail-closed manifest marker
superseded. Manifest участвует только в **Migrate v1 Sources**.

## 1. Одна панель, независимые разделы

В Blender существует одна N-panel вкладка **MH**. В ней раскрываемые разделы:

- **FBX Export**;
- **Composites / Import**;
- **Composites / Export**;
- **Materials**;
- **Source Tools**.

Общих Bundle Export/Export Sources нет. Ресурсные writers независимы;
dependency closure экспортируется только там, где workflow явно это обещает.

## 2. Project settings

- `source_root` — recursive scope static mesh/composite/material resolution;
- `project_uid` — ключ UE Ledger/project state, не source identity;
- `texture_root` — scope texture basename lookup; default `source_root`;
- `texture_policy` — `transitional` (default) или `strict`;
- optional Blender import cache не показывается художнику и строится лениво.

Source tree остаётся стерильным: settings, caches, Ledger, locks и reports не создают в
нём manifest/sidecar/marker.

## 3. FBX Export

Поля: **Collection**, **Directory**, Boolean **Export Materials** (default ON),
кнопка **Export FBX**.

### 3.1 Preflight

1. Получить/назначить stable ResourceUID Collection.
2. Если выбрана `<base>.lods`, извлечь direct `<base>.lodNN`; иначе использовать
   Collection как single LOD0.
3. Проверить hierarchy, dense LOD set, slots subset, scale/UID diagnostics.
4. Собрать `mh.meshser:2` geometry hash и полный FBX passport.
5. Requested target всегда равен
   `<Directory>/<sanitized_name>.mesh.fbx`.
6. Если target существует, прочитать только его embedded UID: другой UID
   блокирует export и показывает Rename mine / Fork existing / Cancel; тот же
   UID разрешает replace.

### 3.2 Publication

Exporter временно назначает `mh_lod_level` и Carrier B passport properties,
пишет sibling temporary FBX и публикует его atomic replace под коротким
per-file lock. Blender state восстанавливается в `finally`, затем writer
завершается. Он не сканирует source root, не читает Ledger/cache, не строит diff
и не обновляет index.

Каждый explicit Export всегда пишет requested FBX, включая semantic no-op.
Diff и решение о UE reimport принимает только reader по сравнению с Ledger.

### 3.3 Export Materials

При **ON** exporter дедуплицирует MaterialUID всех затронутых mesh objects:

- каждый затронутый material записывается в requested FBX Directory как
  `<sanitized_name>.material`;
- target с тем же UID заменяется, target с другим UID блокируется collision
  guard;
- conflict/clean-name collision конкретного материала выдаётся в отчёте;
- texture files не копируются.

FBX publication и material publications являются отдельными resource writes,
не bundle transaction. Ошибка material write не откатывает уже опубликованную
geometry; итоговый report перечисляет failed/missing UID и действие
**Export materials…**.

При **OFF** записывается только FBX. Passport всё равно содержит MaterialUID
slots. Missing material payload даёт warning со списком, но не блокирует
geometry export. Ни `.material`, ни texture не меняются.

## 4. Material Export

Поля: **Material**, **Directory**, **Export Material**.

Каждый Export пишет requested
`<Directory>/<sanitized_name>.material`. Target с тем же UID заменяется; target
с другим UID блокируется. Writer намеренно не ищет тот же UID в других folders:
экспорт туда легален, а duplicates/divergence классифицирует reader scan.

Material без dagormat экспортируется как `rendinst_simple` с пустыми `params`
и `textures`. При включённом dag4blend читаются authored dagormat shader,
parameters и `tex0…tex15`. Node tree не является альтернативным metadata source.

Rename display name не меняет UID. Requested clean target следует текущему
name; старый файл в другом path writer не удаляет и не объявляет owner.
Reader увидит MOVE только если старого кандидата больше нет; иначе duplicate или
divergent revision. Collision target всегда использует общий диалог.

## 5. Composite Export

Поля: **Collection**, **Directory**, **Export Composite**. Экспортируется ровно
одна definition, не весь dependency closure.

Каждая direct child Collection definition представляется placement Empty с
Collection Instance. Empty несёт NodeUID, target ResourceUID, local transform и
`mh_p_*` Custom Properties; resource Collection несёт свой UID/properties.
Физическая вложенность Collections не заменяет graph nodes.

Writer создаёт `mh.composite` schema_version 2 с top-level resource
`properties` и всегда пишет requested
`<Directory>/<sanitized_name>.composite`. Проверяется только target collision;
global resolver/cache не участвует в export.

Writer валидирует только authoring graph, UID/parent/transform/cycle и target
collision. Он не сканирует disk dependencies: missing mesh/composite/material
обнаруживает reader resolver при import. Dependency paths или ownership lists
не записываются.

## 6. Recursive Composite Import

Поля: `.composite` **File Path**, кнопка **Import Composite**. Recursive import
и geometry import всегда ON; отдельных `Recursive`/`With dags` toggles нет.

1. Reader валидирует root `mh.composite` v2 и embedded UID.
2. Resolver на первом Import Composite молча строит optional lazy cache; cache
   miss/stale вызывает payload scan без artist action.
3. Каждый ResourceUID создаёт одну sibling definition Collection под
   `GEOMETRY`, даже если payload пока отсутствует или пуст.
4. Static mesh FBX импортируется в target Collection; combined levels сохраняют
   metadata, необходимую для повторного export.
5. Material slots разрешаются по MaterialUID и применяются к geometry.
6. Composite hierarchy собирается Empty Collection Instances; transforms,
   offsets/random и остальные properties записываются в Custom Properties Empty.

`.composite`, а не FBX, является источником nodes. Обратный порядок допустим
как implementation scheduling, но FBX никогда не восстанавливает graph.

Missing resource сохраняет NodeUID/ResourceUID в красном unresolved Empty и
unresolved target Collection. **Resolve Missing** повторяет resolution,
наполняет те же Blender data-blocks и не меняет identity.

Back-edge cycle на Blender import создаёт placeholder и
`MH_W_COMPOSITE_CYCLE`; остальной graph импортируется. UE import и Blender
export блокируются `MH_E_COMPOSITE_CYCLE`.

## 7. Loose payload, conflicts и Fork

После копирования loose file UE startup/watcher либо Blender Import Composite
scan читает embedded identity:

- новый UID — **Adopt**;
- прежний path исчез, новый unique — auto MOVE + log;
- две byte-identical copies — duplicate warning;
- две divergent copies — `MH_E_DIVERGENT_REVISIONS`, без mtime winner;
- missing/malformed identity — quarantine.

При ручном выборе loose file UI предлагает **Update existing**, **Fork as New
Resource**, **Cancel**. Fork выдаёт новый UID и переписывает payload identity;
для composite отдельно выбирается fork closure. Художник никогда не получает
чужое имя или revision автоматически.

## 8. Textures и Actualize

Texture slot resolution:

```text
exact path -> unique basename under texture_root -> unresolved
```

Unique basename при material import актуализирует `.material` и пишет log.
Ambiguous basename не выбирается автоматически. Раздел **Source Tools** содержит
**Actualize Texture Paths** с отчётом fixed/ambiguous/missing. Та же операция
доступна UE commandlet.

Внутренний path записывается относительно `texture_root`; внешний — normalized
absolute. Transitional policy предупреждает, strict блокирует. Ни один workflow
не копирует texture files.

## 9. Blender lazy Import Composite cache

Первый **Import Composite** молча сканирует три primary extensions и может
сохранить optional lazy cache вне source tree. Последующие imports используют
его только после проверки текущего fingerprint/passport; stale/missing cache
перестраивается автоматически.

Artist-facing **Rebuild Index** отсутствует. Cache никогда не обновляется
export writer'ом и не вычисляет geometry hash из FBX. Сторонняя byte-правка при
неизменных semantic hashes отмечается reader diagnostic.

## 10. Migrate v1 Sources

**Migrate v1 Sources** — единственный workflow, читающий
`export_manifest.json`, uid8 filenames и legacy `lods[]`.

Оператор сначала делает полный no-write preflight, затем:

- переносит mesh identity/metadata в FBX passport;
- re-export'ит per-file LOD как один Combined-LOD FBX;
- повышает composite до schema_version 2 и переносит resource properties;
- переименовывает payload в clean names;
- после валидации удаляет legacy manifests;
- выдаёт migrated/renamed/conflicted/failed receipt и rollback instructions.

Collision/ambiguity не получает автоматического победителя. Production import,
resolver и watcher никогда не вызывают legacy codec.

## 11. UE startup, watcher и reimport

При запуске UE scanner сравнивает source payloads с Ledger. Default — silent
auto-import; optional project preference включает prompt. Watcher затем
стабилизирует изменившийся payload per-file и выполняет то же Ledger comparison.
Import batch:

```text
textures -> materials -> geometry -> composites -> finalize -> ledger
```

Combined FBX обновляет все authored SourceModels одного существующего
StaticMesh in place. Composite graph компилируется из `.composite` v2. Diff
существует только на reader side: Blender writer всегда пишет и ничего не
сравнивает. Ledger не использует manifests.

## 12. Field acceptance

Owner проверяет на реальном проекте:

1. clean export без sidecars и UID suffixes;
2. no-op FBX export физически переписывает target, но UE Ledger даёт
   `NO_CHANGE`; metadata-only export даёт descriptor/property update;
3. Combined-LOD с двумя levels и multi-object LOD1;
4. ON/OFF ветки Export Materials и отсутствие texture copies;
5. recursive composite import, missing → Resolve Missing с той же identity;
6. loose MOVE, identical duplicate и divergent conflict без mtime winner;
7. Actualize unique/ambiguous/missing texture paths;
8. UE reimport-in-place того же asset.

Крупные gates до field acceptance принимает внешний аудитор.
