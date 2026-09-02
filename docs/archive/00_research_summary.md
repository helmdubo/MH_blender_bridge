> Status: HISTORY · Do not use for implementation · Superseded by docs/16_recipe_model.md

# 00 — Research summary and Decision Log

> **SUPERSEDED BY SOURCE PROTOCOL V4.** Документ целиком снят с роли
> нормативного input. Действующий контракт и решения находятся только в
> [`08_source_protocol_v4_plan.md`](08_source_protocol_v4_plan.md); body ниже
> сохранён как история исследования и решений прежних протоколов.

Статус: Decision Log проекта. Активный source contract — MH Source Protocol v2
Clean Sources (`05_source_schema_v1.md`; filename historical). Freeze Source
Schema v1 по SHA `d52520c47544a6e36b3bac32b16237ad670abb20`
сохранён как migration baseline и не является production runtime.

Нумерация решений принадлежит репозиторию. Директивы/ревью ссылаются на
решения по slug; свободный номер назначается при внесении в этот файл.

## 1. Исследовательский итог

Dagor-style authoring подтверждает полезность двух разных сущностей:

- resource definition Collection содержит geometry или composite definition;
- placement Empty/Collection Instance содержит transform, random/offset и
  остальные node properties.

Из этого следует неизменный принцип: `.composite` задаёт graph, а FBX только
наполняет target Collection. Попытка восстановить nodes из FBX возвращает
хрупкую семантику-в-именах, от которой проект сознательно отказался.

Исследование v1 manifests доказало работоспособность UID/dependency модели, но
также выявило operational цену distributed ownership rows, uid8 filenames и
payload+manifest transactions. Владелец выбрал v2 passport-first: identity
внутри primary payload, stateless explicit writer и reader-owned state вне
стерильного source tree.

Combined-LOD pivot отдельно принят осознанно: один mesh resource — один FBX;
цена полного rewrite при правке любого уровня предпочтительнее per-file LOD
transport и повторного связывания уровней.

## 2. Активные решения, не изменённые v2

### D1 `[composite-is-graph]`

`.composite` и только он задаёт NodeUID, target ResourceUID, parent, transform и
node metadata. Resolver находит payload по UID. FBX никогда не является
источником composite nodes.

### D3 `[stable-resource-and-node-uids]`

ResourceUID идентифицирует definition; NodeUID идентифицирует placement внутри
одного composite. Rename/path MOVE не меняет UID. Duplicate authored UID
является validation/conflict case, а не основанием выбрать первый найденный.

### D4 `[deterministic-random]`

Random/variant semantics, когда реализованы, используют явный seed policy и
stable UID inputs. Порядок Blender Outliner/JSON arrays не является seed.

### D10 `[cycle-policy]`

Blender import: composite back-edge → unresolved placeholder и
`MH_W_COMPOSITE_CYCLE`, остальной graph продолжается. Blender export и UE
import: `MH_E_COMPOSITE_CYCLE`, операция блокируется.

### D12 `[transform-contract]`

Composite transform хранится как translation cm, canonical quaternion xyzw и
positive scale; UE coordinate convention является transport convention.
Zero/negative scale запрещён.

### D19 `[collection-resource-identity]`

Definition Collection имеет ResourceUID и kind. Placement Empty имеет NodeUID,
target ResourceUID и `instance_collection`. Definitions являются siblings под
`GEOMETRY`; физическая Collection nesting не заменяет graph.

### D20 `[names-are-display]`

Identity не выводится из имени. Authoring resource name проходит ASCII
валидацию; Unicode остаётся допустимым в display/properties data. Canonical
strings нормализуются NFC. V2 filename clean/lowercase, но resolve идёт по UID.

### D23 `[texture-identity-is-path]`

Texture dependency остаётся path-based. V2 добавляет переносимый fallback по
basename и явную actualization, но не создаёт TextureUID и не копирует файл.

### D32 `[single-panel-standalone-ux]`

Одна N-panel вкладка **MH**, отдельные sections FBX/Composites/Materials/Source
Tools. Bundle Export отсутствует. Export Materials — Boolean FBX workflow.

### D35 `[recursive-composite-import]`

Recursive composite resolution и found geometry import всегда ON. Missing UID
сохраняет identity в placeholder; Resolve Missing наполняет те же data-blocks.

### D38 `[fbx-export-materials-toggle]`

FBX Export `Export Materials=ON` всегда пишет затронутые materials в requested
Directory; `OFF` не пишет материалы. Это orchestration независимых explicit
resource writes, не bundle/closure transaction. Texture files не копируются.

### D40 `[combined-lod-fbx]`

Dag authoring `<base>.lods`/`<base>.lodNN` экспортируется в один FBX. Level —
integer `mh_lod_level` на mesh node, не имя. Passport заявляет `lod_levels`.
`mh.meshser:2` покрывает все levels и auxiliary UCX/SOCKET. Explicit Export
всегда переписывает общий payload; правка любого level меняет geometry hash.

## 3. Clean Sources v2 decisions

### D41 `[clean-sources-v2-authority]`

Принято владельцем немедленно: source tree содержит только primary
`*.mesh.fbx`, `*.composite`, `*.material`. `export_manifest.json`, registry,
sidecars, markers и cache в дереве отменены. Embedded identity/passport —
authority; UE Ledger и optional Blender Import Composite cache принадлежат
readers и находятся вне дерева.

Следствие: parallel production v1, manifest fallback и dual-read запрещены.
Frozen v1 codec разрешён только migration utility.

### D42 `[clean-human-filenames]`

Filename: `<sanitized_name>.mesh.fbx|.composite|.material`, без UID suffix.
Filename display-only; same names в разных folders легальны. В одном folder
clean-name collision разных UID блокируется
`MH_E_NAME_COLLISION_DIFFERENT_UID` и требует Rename mine / Fork existing /
Cancel. Writer проверяет только requested target; тот же UID можно явно
экспортировать в другой folder. Duplicate/divergent state классифицирует reader.
Silent overwrite/mtime winner отклонены.

### D43 `[passport-carrier-and-reader-hashes]`

FBX использует `mh.fbx_passport:1`. Carrier B — одинаковая canonical JSON
property на каждой Model node с обязательным byte consensus. Решение final, но
transport proof является блокирующим implementation gate.

Разведены `geometry_hash`, `descriptor_hash`, `payload_fingerprint`. Writer
только вычисляет embedded `geometry_hash`; UE startup/watcher сравнивает
semantic hashes/fingerprint с Ledger. Hashes не управляют explicit Export.

### D44 `[reader-ledger-and-conflict-matrix]`

UE startup/watcher scan сравнивает payloads с Ledger; default auto-import
silent, prompt optional. Blender lazy cache существует только для Import
Composite и строится молча. Passport auto-wins только для MOVE (old gone, new
unique). Два живых divergent payload одного UID дают
`MH_E_DIVERGENT_REVISIONS`; mtime не выбирает победителя. Fork выдаёт новый UID.

### D45 `[migration-only-legacy-reader]`

Legacy manifests/uid8/per-file LOD читаются только one-shot migration operator.
Migration имеет full no-write preflight, exact backup/rollback receipts,
переносит metadata внутрь payload и удаляет manifests после validation.
Production reader не содержит legacy branch.

### D46 `[texture-basename-actualization]`

Texture resolve: exact normalized path → unique basename под `texture_root` →
unresolved. Unique candidate актуализирует `.material`; ambiguous выдаёт
`MH_W_TEXTURE_BASENAME_AMBIGUOUS`. Blender operator и UE commandlet формируют
fixed/ambiguous/missing report. Texture copy остаётся запрещён.

### D47 `[stateless-explicit-writer]`

Каждый Blender Export всегда пишет requested payload(s): target collision guard,
temporary sibling, atomic replace, exit. Writer не выполняет hash-skip/diff,
source-root scan, cache/Ledger read или index update. Per-file lock защищает
только target. Diff полностью reader-side; semantic no-op физически пишет
source, но UE Ledger классифицирует `NO_CHANGE`.

### D48 `[composite-v2-resource-properties]`

`mh.composite` schema_version 2 содержит required top-level `properties` для
asset-level semantics. Node `properties` остаётся placement-level. Bags не
наследуются и не дублируются. Material asset semantics живут в `.material.params`.

## 4. Исторические/superseded v1 решения

Ни одна строка этой таблицы не разрешает production behavior. Она объясняет
миграцию и старые artifacts.

| Историческое решение | v2 disposition |
|---|---|
| D2 bundle/manifest identity | superseded D41; bundle и manifest runtime удалены |
| D13/D39 per-file LOD rows | superseded D40; one-shot migration only |
| D15 manifest-last transaction | superseded D47; stateless atomic explicit writer |
| D17 uid8 filename disambiguation | superseded D42; clean names + collision UI |
| D21 owning manifest | superseded D41/D44; embedded identity + reader candidate conflict |
| D22 registry hint + confirming scan | superseded D44; UE Ledger/scan and Blender import-only lazy cache |
| D27 source-root-relative texture-only model | refined D46; texture_root basename actualization |
| D31 manifest material rows | superseded; `.material` self-identifies |
| D33 manifest snapshot/global pending block | superseded D47; per-file stability |
| D34 transitional external texture diagnostics | retained only as path policy inside D46/05 §8 |
| Frozen `mh.composite` v1 | migration input; active writer emits v2 |

## 5. Rejected alternatives

- **«Последний файл по mtime победил»** — теряет revision и присваивает
  художнику решение без согласия.
- **UID в filename** — визуальный шум и не решает divergent identity; UID живёт
  внутри payload.
- **Manifest alongside payload** — возвращает distributed transaction и stale
  ownership.
- **Central `.mh/` in source tree** — нарушает sterile-tree invariant.
- **Per-file LOD** — ломает one-resource/one-file decision и добавляет связывание.
- **LOD by node name** — семантика-в-именах и FBX mangling.
- **Texture copying on export** — меняет ownership и выходит за DCC source
  workflow.
- **`.mhpack` for internal loose exchange** — closure packaging переложено на
  машину позже как Export Selection, не на ежедневного художника.

## 6. Acceptance model

Контракт принят немедленно, implementation доказывается gates:

1. Carrier B transport proof;
2. clean writers/passports/meshser2/no manifests;
3. Blender Import Composite lazy resolver/Fork/Actualize;
4. UE startup/watcher Ledger diff and conflicts;
5. migration-only legacy reader;
6. crash/concurrency and vertical slice.

Внешний аудитор принимает крупные slices. Owner делает финальную field
acceptance; до неё результат не объявляется полностью завершённым.
