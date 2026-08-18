# ADR-V2 — Passport-first authority for Clean Sources v2

Статус: **ACCEPTED, ACTIVE NOW**. Этот ADR уточняет
`05_source_schema_v1.md`; название того файла историческое. Старые положения
этого ADR о «v1 остаётся активным», dual-read, uid8 filenames и ожидании
spike-результатов до объявления v2 полностью superseded документом
**CONTRACT — MH Source Protocol v2: Clean Sources**.

## 1. Решение authority

Source tree содержит только `*.mesh.fbx`, `*.composite`, `*.material`.
Embedded passport/self-identity payload является истиной. UE Ledger и optional
Blender Import Composite cache находятся вне дерева и принадлежат readers.
Manifest runtime, registry hints и sidecar ownership больше не существуют.

Legacy `export_manifest.json` читается только one-shot migration operator. Его
production fallback или «временный» dual-read запрещены: они создают две
конкурирующие authority-модели.

## 2. FBX passport и carrier

Нормативная схема `mh.fbx_passport:1` задана в 05 §4. Carrier B финален:
каноническая JSON-строка `mh.fbx_passport` дублируется на каждой
экспортируемой FBX Model node. Reader требует наличие и bytewise consensus всех
копий. Любое расхождение, malformed JSON или unknown version карантинирует
payload с `MH_E_PASSPORT_INVALID`.

Transport gate остаётся блокирующим implementation gate, а не выбором carrier:
до включения writer требуется доказать сохранность на двух Blender FBX путях,
в UE backend, после re-export, на shared datablocks/custom normals, длинном
JSON, Combined-LOD и без изменения pivot/hierarchy.

## 3. Geometry, descriptor и reader-side fingerprint

| Величина | Кто вычисляет | Authority/storage |
|---|---|---|
| `geometry_hash` | Blender exporter по `mh.meshser:2` | FBX passport + reader state |
| `descriptor_hash` | reader по passport без hash fields | UE Ledger / optional Blender import cache |
| `payload_fingerprint` | reader scanner по bytes | UE Ledger / optional Blender import cache |

Reader scan читает `geometry_hash` из passport и не реконструирует его из FBX.
Fingerprint mismatch при неизменных semantic hashes означает внешнюю правку:
`MH_W_PAYLOAD_EXTERNAL_MODIFIED`, подтверждение пользователем.

Нормативное правило explicit Blender writer:

```text
collision guard -> temporary sibling -> atomic replace -> exit
```

Каждый пользовательский Export **всегда** пишет requested payload(s), включая
semantic no-op. Writer не читает Ledger/cache, не строит diff и не обновляет
index. `geometry_hash` и вычисляемый `descriptor_hash` нужны UE startup/watcher,
которые сравнивают payload с Ledger: no-op → `NO_CHANGE`, geometry change →
`UPDATE_GEOMETRY`, metadata-only change → descriptor/property update.

## 4. Clean filenames

Primary paths имеют форму `<sanitized_name>.mesh.fbx`, `.composite`,
`.material`. UID suffix отсутствует. Filename — display concern; resolve идёт
только по embedded UID.

Clean-name collision в одном каталоге при разных UID блокируется
`MH_E_NAME_COLLISION_DIFFERENT_UID`; разрешённые действия: Rename mine, Fork
existing as new resource, Cancel. Silent overwrite, mtime winner и присвоение
художнику чужого имени запрещены. Одинаковые filenames в разных каталогах
легальны.

## 5. Conflict matrix

| Наблюдение | Реакция |
|---|---|
| UID отсутствует в UE Ledger, payload валиден | startup/watcher auto-import (silent default; prompt optional) |
| Старый path исчез, новый unique path имеет тот же UID | MOVE автоматически + log |
| Два существующих path одного UID, fingerprints равны | `MH_W_DUPLICATE_IDENTICAL_PAYLOAD` |
| Два path одного UID, fingerprints различны | `MH_E_DIVERGENT_REVISIONS`, ручной выбор |
| Loose file выбран вручную | Update existing / Fork as New Resource / Cancel |
| Embedded identity отсутствует | runtime quarantine; `MH_W_LEGACY_PAYLOAD_NO_PASSPORT` только в migrator |
| Passport malformed/unknown/без consensus | `MH_E_PASSPORT_INVALID`, quarantine |

Passport «всегда выигрывает» только в доказанном MOVE: прежнего кандидата нет,
новый один и валиден. При двух живых divergent revisions автоматического
победителя нет.

**Fork as New Resource** создаёт новый UID и не изменяет оригинал. Для
composite пользователь отдельно выбирает closure внутренних ссылок, который
форкается вместе с root.

## 6. Reader state: Blender lazy cache и UE Ledger

Общего project index контракта нет.

- Blender writer stateless относительно source root.
- Blender **Import Composite** при первом вызове молча строит optional lazy
  resolver cache. Cache — implementation accelerator; stale/missing состояние
  вызывает silent scan. Artist-facing Rebuild отсутствует.
- UE startup сканирует payloads и сравнивает их с Ledger. Default behavior —
  silent auto-import; project preference может включить prompt.
- UE watcher выполняет тот же per-file scan/Ledger comparison для изменений.

Reader state находится вне source tree и удаляем без потери source data. Writer
никогда его не обновляет. Atomic publication не образует транзакцию с cache или
Ledger; per-file lock относится только к requested export target.

## 7. Texture resolution

Material сохраняет texture path, а resolver использует каскад:

```text
exact normalized path -> unique basename under texture_root -> unresolved
```

Unique basename позволяет актуализировать `.material`; ambiguous basename даёт
`MH_W_TEXTURE_BASENAME_AMBIGUOUS` без автопочинки. **Actualize Texture Paths**
существует в Blender и как UE commandlet и выдаёт fixed/ambiguous/missing.
Texture files не копируются. Полная canonical/path policy — 05 §8.

## 8. Combined-LOD

Один mesh UID — один FBX. Passport хранит `lod_levels`; `mh_lod_level` является
integer property mesh Model node. Level никогда не выводится из filename или
node name. Geometry hash `mh.meshser:2` покрывает все уровни и auxiliary
UCX/SOCKET; изменение любого уровня переписывает общий payload.

Нормативные ограничения и diagnostics —
`AMENDMENT_combined_lod_fbx.md`. Старые per-file LOD rows superseded и доступны
только migration path.

## 9. Loose transfer и отклонённые варианты

Внутристудийная передача использует loose payload files. Получатель кладёт их
в source tree; resolver читает embedded identity и перечисляет отсутствующие
UID dependencies по hints. `.mhpack`, центральный `.mh/`, общий project index,
mtime-based identity и runtime manifest fallback отклонены.

Export Selection с dependency closure остаётся ROADMAP для внешней передачи и
не меняет primary payload contract.

## 10. Implementation order

Немедленное принятие v2 не отменяет gates:

1. carrier transport proof;
2. clean always-write exporters/passports и одновременное удаление manifest,
   hash-skip, diff и index-update из writer flow;
3. Blender lazy Import Composite resolver + UE startup/watcher Ledger diff,
   conflict matrix, Fork, texture actualization;
4. migration-only legacy reader и v2 importer;
5. crash/concurrency receipts;
6. UE parity и дальнейшие C/D gates.

До gate 2 UE не должен создавать новый per-file-LOD или manifest runtime path;
после gate 2 оба фронта работают только против Clean Sources v2.
