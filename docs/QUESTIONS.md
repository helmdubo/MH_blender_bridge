# QUESTIONS — implementation questions after Clean Sources v2 freeze

Статус: открытые вопросы здесь не могут ослабить normative invariants 05/ADR/
Combined-LOD. До решения используется fail-closed вариант, указанный в каждом
пункте. Вопросы frozen v1 ниже закрыты или superseded и не создают dual-read.

## OPEN-V2-1 — Provisioning `project_uid`

**Контекст.** UE хранит Ledger вне source tree и ему нужен стабильный ключ
проекта. Blender writer state не имеет; optional lazy Import Composite cache
может использовать тот же project key только как implementation detail.

**Вопрос.** Где студия хранит/раздаёт project UID: в существующем project
configuration, environment variable или явной UE project setting?

**Предложение.** Один UUID в UE project configuration вне source payload tree.
Не выводить его из абсолютного path: перенос проекта не должен создавать новый
Ledger namespace. Blender importer может читать его для cache namespace, но
writer не зависит от него.

**Временное правило.** Только explicit valid UUID; автоматическая генерация или
path hash запрещены до решения. Это implementation setting, не on-disk schema.

## OPEN-V2-2 — Срок migration warning

**Контекст.** `MH_W_LEGACY_PAYLOAD_NO_PASSPORT` разрешён только migration scan,
после переходного окна факт должен стать ошибкой даже в studio tooling.

**Вопрос.** Какая дата/релиз закрывает окно предупреждения?

**Предложение.** Первый общий Blender Extension + UE plugin release, прошедший
owner field acceptance и миграцию активных source roots. Receipt релиза хранит
список failed roots; только после нуля failed code повышается до E.

**Временное правило.** Production runtime уже quarantines missing identity.
Warning существует только в migrator; срок не разрешает dual-read.

## OPEN-V2-3 — Rename-to-match UX

**Контекст.** Filename display-only. Explicit Export пишет clean target в
выбранный Directory и не удаляет старый path того же UID. После rename старый
file может остаться duplicate/divergent candidate. MOVE возникает только когда
старого path больше нет.

**Вопрос.** Нужен ли отдельный operator **Rename file to match** в ближайшем
Blender slice или достаточно ручного move + watcher?

**Предложение.** ROADMAP operator с collision preflight и atomic move/delete-old
transaction. Обычный Export остаётся stateless и не переносит старый resource.

**Временное правило.** Export пишет requested target; reader честно показывает
duplicate/divergent state, пока художник не удалит/переместит старый path.

## CLOSED — frozen v1 questions

| Старый вопрос | Итог |
|---|---|
| Unicode canonical strings | NFC принято и остаётся в v2 |
| Mesh hash coverage | v2 использует `mh.meshser:2`; export-affecting data и UCX/SOCKET обязательны |
| Zero/negative scale | `scale <= 0` запрещён |
| Multi-object hash order | deterministic `(lod_level, mh_uid)`; UID repair может дать честный rewrite |
| LOD strategy | superseded Combined-LOD D40; per-file rows migration-only |
| Blender properties | `mh_p_<key>`; resource и placement bags раздельны |
| Negative golden scenes | duplicate UID, cycle, dangling parent и v2 passport/conflict fixtures обязательны |
| Cyrillic resource filename | authoring name ASCII; filename clean lowercase; Unicode разрешён в display/properties |
| Resource properties transport | FBX passport / composite v2 top-level properties; material semantics в params |
| Manifest owner/registry/source | superseded embedded identity + UE Ledger/scan; Blender cache import-only |
| uid8 disambiguation | superseded clean filename + collision/Fork UX |

## Non-negotiable while questions are open

- source tree содержит только three primary payload types;
- runtime не читает manifest и не выбирает revision по mtime;
- FBX без valid consensus passport quarantined;
- один mesh UID имеет один Combined-LOD FBX;
- Blender writer не имеет cache/diff; Import Composite cache строится молча;
- UE startup/watcher сравнивает scan с Ledger;
- legacy reader существует только в migration utility.

## UE plugin questions (этап C)

### UE-QUESTION-13 — passport carrier property key

**Статус.** РЕШЕНО фактом v2 writer: production код
`addon/mh4blend/core/fbx_passport.py` пишет carrier custom property
`mh_fbx_passport` (underscore) со schema `mh.fbx_passport`. UE reader обязан
читать это имя. Внимание ревьювера: §4.2 `05_source_schema_v1.md` словами
называет property `mh.fbx_passport` — это расхождение формулировки с writer;
требуется одно-строчная правка документа, байты payload'ов не меняются.

### UE-QUESTION-14 — объём `mh.fbxdump --full`

**Контекст.** v2 §7.2 контракта 07: dump печатает passport, Model graph,
`mh_lod_level`, slots, axis/units, counts. Не ратифицировано, обязан ли
`--full` уже в теге 1 содержать mapper-facing layer arrays
(normals/smoothing/UV/colors) или topology-only формы C0 достаточно до C2.

**Временное правило.** Тег `mh.fbxdump:1` сохраняет topology-only `--full`;
расширение формата выполняется bump'ом тега вместе с fixtures C2.

**Статус.** ОТКРЫТ.
