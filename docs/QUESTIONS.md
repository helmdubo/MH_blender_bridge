# QUESTIONS — открытые вопросы Source Protocol v4

Статус: единственный действующий норматив —
[`08_source_protocol_v4_plan.md`](08_source_protocol_v4_plan.md). Открытые
вопросы не могут ослабить его инварианты; до решения действует указанное в
вопросе fail-closed правило. Все прежние UID/passport/round-trip вопросы ниже
сохранены как история и явно помечены `SUPERSEDED BY 08`.

Открыт один вопрос v4: filesystem aliases (`OPEN-V4-1`). Вопросы
`OPEN-V4-2`–`OPEN-V4-9` (включая выявленные аудитами S2 грамматику
Material, applied state, library round-trip и persisted-форму
`AppliedParent`) решены owner — нормативный текст перенесён в
08 §§2–5, §7 и 09 (S2/S4/S5/S6).

## OPEN-V4-2 — canonical texture reference и image extensions

**Статус. РЕШЕНО OWNER — нормативно в 08 §5 (этот docs-коммит).**
`.material.textures[texN]` хранит только logical name без расширения и без
path-разделителей; текстура — полноправный kind §2 с резолвом по ResourceKey
в границах source-дерева (Project Index — кэш резолва); каскад 07 §5 и
`texture_root` не выживают. Scanner allowlist: `png, tga, tif, tiff, exr,
jpg, jpeg, dds, hdr` (расширяется только поправкой owner). Same-stem файлы
разных расширений — стандартный duplicate fail-closed, без приоритета
форматов. Коды `MH_E_NONCANONICAL_TEXTURE_REFERENCE` и
`MH_E_UNRESOLVED_TEXTURE_REFERENCE` регистрируются в S2 вместе с
implementation. Временное правило ниже — история S0: отдельного окна
блокировки не требуется, S2 сразу реализует решение.

**Контекст.** 08 §2 задаёт texture identity как extensionless
`Texture + LogicalName` и показывает `<img-ext>` только у файла; примеры 08 §5
также используют extensionless значения `textures`. Одновременно 08 §5
наследует из 07 §5 каскад `exact path → unique basename → unresolved`, где не
определено, включает ли canonical basename расширение файла. Список допустимых
image extensions и классификация двух файлов с одинаковым stem, но разными
расширениями, тоже не заданы.

**Вопрос.** Что именно хранит `.material.textures[texN]`: logical name без
расширения, relative filename с расширением или обе канонические формы? Какие
image extensions допускает scanner и как классифицируется same stem у разных
расширений?

**Временное fail-closed правило.** До owner-решения любой `.material` с
непустым `textures` блокируется целиком вместе со всеми dependents. Reader и
writer не интерпретируют, не угадывают и не публикуют texture references.
Конкретный `MH_E_*` код должен быть ратифицирован owner и зарегистрирован в
implementation-срезе; S0 новый код не изобретает.

**Прежний статус.** ОТКРЫТ; блокировал соответствующую часть S2 до решения
owner.

## OPEN-V4-3 — mismatched `_lodNN` suffix в authored LOD collection

**Статус. РЕШЕНО OWNER — нормативно в 08 §4 (этот docs-коммит).**
Временное правило S1 ратифицировано как постоянное: mismatched
terminal-суффикс — fail-closed reject `MH_E_INVALID_LOD_HIERARCHY`,
exporter никогда не переписывает и не «чинит» авторские имена (имя —
identity; молчаливый repair запрещён аксиомами v4). Художник исправляет имя
или membership в Blender сам.

**Контекст.** 08 §4 требует временно добавить `_lodNN` mesh-объекту из
`.lodNN`-коллекции, если суффикса нет. Не определён случай, когда объект уже
имеет terminal suffix другого уровня, например `wheel_lod00` внутри `.lod01`.
Сохранение имени классифицирует узел в UE как неверный LOD; молчаливая замена
суффикса была бы незафиксированным repair.

**Вопрос.** Должен exporter отклонять mismatched suffix либо временно заменять
его на уровень authored collection с восстановлением исходного имени после
export?

**Временное fail-closed правило.** Export блокируется существующим
`MH_E_INVALID_LOD_HIERARCHY`; имя Blender-объекта не изменяется. Совпадающий
terminal suffix сохраняется, отсутствующий добавляется только в export context
и всегда восстанавливается в `finally`.

**Прежний статус.** ОТКРЫТ; S1 реализует временный reject и не блокируется
для каноничных ресурсов.

## OPEN-V4-4 — diagnostic code для noncanonical logical name

**Статус. РЕШЕНО OWNER — нормативно в 08 §2 (этот docs-коммит).**
В S2 вводится `MH_E_NONCANONICAL_RESOURCE_NAME` — единый код любого
нарушения каноничного имени файла ресурса (stem вне `[a-z0-9_]+` ИЛИ
не-lowercase каноничный суффикс). Он заменяет legacy
`MH_E_NON_ASCII_RESOURCE_NAME` во всех call sites (включая suffix-case
reject S1, использующий `MH_E_SOURCE_INDEX_INVALID`), в реестре и в golden
counts. До S2 действует временное правило S1.

**Контекст.** 08 §2 требует exact `[a-z0-9_]+` и fail-closed reject без
lowercase/sanitize repair, но не называет машинный код общего нарушения.
Существующий `MH_E_NON_ASCII_RESOURCE_NAME` зарегистрирован в Blender/UE, однако
его имя уже семантики v4: uppercase, пробел и точка тоже неканоничны, оставаясь
ASCII.

**Вопрос.** Ратифицировать новый v4-код для любого нарушения canonical logical
name либо официально расширить смысл/переименовать legacy-код?

**Временное fail-closed правило.** Blender exporter и UE scanner валидируют
exact `[a-z0-9_]+`, ничего не нормализуют и для любого нарушения возвращают уже
зарегистрированный `MH_E_NON_ASCII_RESOURCE_NAME`. Новый код в S1 не вводится.

**Прежний статус.** ОТКРЫТ; diagnostic naming требовал owner-решения, strict
поведение S1 не блокировалось.

## OPEN-V4-5 — версии v4 diagnostic JSON

**Статус. РЕШЕНО OWNER — нормативно в 09 S4/S5/S6 (этот docs-коммит).**
v2-теги `:1` мертвы навсегда и никогда не переносят v4-байты; временный
режим S1 (console Analyze/Plan, `-report`/`-writeledger` → exit 2 без
файла) ратифицирован до соответствующих срезов. Структурированные
diagnostics возвращаются по срезам: S4 удаляет `-writeledger` вместе с
Ledger; S5 возвращает mapper-facing FBX dump поверх `FMHSceneIR` с тегом
`mh.fbxdump:4`; S6 вводит JSON-отчёт с тегом `mh.analyze_sources:4`.
Точные поля задаются в своих срезах, наследование v2-полей запрещено.

**Контекст.** В S1 `MHAnalyzeSources` перешёл с UID и доменных semantic hashes
на `ResourceKey` и raw hash, а FBX-контракт — с passport/`mh_lod_level` на
классификацию по именам узлов. Старые теги `mh.analyze_sources:1` и
`mh.fbxdump:1` описывали несовместимые v2-поля. 08 утверждает Message Log и
commandlets как идею, но не ратифицирует форму или номер diagnostic JSON;
старый FBX dump contract уже явно superseded в `UE-QUESTION-14`.

**Вопрос.** Какие tag/version и точные поля должны иметь v4 AnalyzeSources
report и mapper-facing FBX dump? Нужен ли FBX dump уже до S5, либо его следует
вернуть вместе с `FMHSceneIR`/StaticMesh importer acceptance?

**Временное fail-closed правило.** S1 не переиспользует старые теги для новых
байтов. `MHAnalyzeSources` сохраняет console Analyze/Plan, но `-report`
отклоняется с exit code 2 и не пишет файл. Старый FBX dump commandlet/UI и его
`mh.fbxdump:1` удалены; v4 dump не вводится без owner-решения. Новых кодов
ошибок для этого временного правила не добавляется: report preflight использует
зарегистрированный `MH_E_SOURCE_INDEX_INVALID`.

**Прежний статус.** ОТКРЫТ; не блокировал name-keyed
scan/resolve/analyze/plan и FBX transport S1, но блокировал
структурированные diagnostic artifacts до решения owner.

## OPEN-V4-6 — top-level bool и точная грамматика Material v4

**Статус. РЕШЕНО OWNER — нормативно в 08 §5 (этот docs-коммит).**
Грамматика закрыта: class-форма = `class` + опциональные `twosided`,
`textures` (`tex0`–`tex15`), `params` (число или массив ровно из 4);
library-форма = ровно одно поле `library`. `tex16support` — артефакт
черновика, удалён из примера; `twosided` — единственный top-level флаг
(MI Base Property Override, не static switch), пишется только при
override. Любое неизвестное поле/тип/ключ — новый `MH_E_MATERIAL_GRAMMAR`
(регистрируется в S2), reader ничего не игнорирует. Static bools
по-прежнему не сериализуются (№7).

**Контекст.** Первый пример 08 §5 содержит top-level поля
`"tex16support": true` и `"twosided": false`. Ниже тот же §5 утверждает,
что статические bool-переключатели в JSON не сериализуются, а 09 S2 ещё раз
ограничивает writer/reader полями `class|library`, `textures`, `params` и
явно говорит «БЕЗ ... static bools». Для обязательного golden codec нельзя
самостоятельно выбрать между reject, ignore и serialize: все три варианта
дают разные нормативные bytes и разное Publish-поведение.

**Вопрос.** Являются ли `tex16support`/`twosided` устаревшими полями примера,
которые reader обязан отклонять как неизвестные, или это отдельные допустимые
не-static bool-поля? Зафиксировать точный allowed-field set для class-mode и
library-mode, включая правило unknown fields.

**Временное fail-closed правило.** S2 material reader/writer не принимает и
не публикует `.material`, пока exact grammar не ратифицирована. Сохраняется
S1-заглушка; старый v2 codec не возвращается и dual-read не вводится.

**Прежний статус.** ОТКРЫТ; блокировал material codec, golden-векторы и
весь S2.

## OPEN-V4-7 — семантика `UMHMaterialSourceData.AppliedHash`

**Статус. РЕШЕНО OWNER — нормативно в 08 §§5, 7 (этот docs-коммит).**
Хранятся ОБА хэша (`blake3-160:<40 hex>`, §3): `SourceHash` — raw bytes
применённого `.material`; `AppliedHash` — hash канонического JSON,
извлечённого из MI сразу после apply тем же extractor'ом, что использует
Publish — одна канонизация на импорт, publish и детект. Локальная правка:
re-extract сейчас → hash ≠ `AppliedHash` (или extract не-roundtrippable) →
`MH_W_MANAGED_ASSET_LOCALLY_MODIFIED`. Каноническая байт-форма
(UTF-8/LF/отступ 2/порядок полей/float shortest round-trip) закреплена в
08 §5 и фиксируется общими golden-векторами pytest + UE Automation.

**Контекст.** 08 §5 требует перед source-wins overwrite обнаруживать локально
изменённый managed MI и выдавать `MH_W_MANAGED_ASSET_LOCALLY_MODIFIED`. 08 §7
перечисляет у `UMHMaterialSourceData` только `LogicalName`, path, `applied
hash` и parent class, но не определяет, хэширует ли `AppliedHash` raw source
bytes или семантическое состояние MI. Для второго варианта не заданы состав
доменов MI (parent/scalar/vector/texture/static overrides), канонизация и
алгоритм. Raw source hash сам по себе не позволяет отличить сохранённую
локальную правку MI от неизменённого applied asset.

**Вопрос.** Что именно означает `UMHMaterialSourceData.AppliedHash`? Если это
asset-state hash, какие домены MI входят в него, как они канонизируются и какой
алгоритм/tag используется? Если это source raw hash, где хранится baseline,
необходимый для детекта локальной правки?

**Временное fail-closed правило.** UE не мутирует и не публикует managed MI и
не записывает предположительный applied state. Scan/resolve/Analyze остаются
reader-only; ложный warning или молчаливое затирание локальной правки
запрещены.

**Прежний статус.** ОТКРЫТ; блокировал `UMHMaterialSourceData`, MI import и
Publish.

## OPEN-V4-8 — library-mode round-trip и Blender authoring mapping

**Статус. РЕШЕНО OWNER — нормативно в 08 §5 (этот docs-коммит).**
Подтверждено: любой локальный override у MI c library-parent делает Publish
`MH_E_MATERIAL_NOT_ROUNDTRIPPABLE`; молчаливый discard и расширение JSON
запрещены. Импорт library-формы — полный apply (reparent + очистка
overrides, source побеждает, детект локальной правки до перезаписи).
Blender: v4-модель — собственная property group mh4blend; dag4blend
`is_proxy`/`proxy_path` не читаются и не конвертируются (proxymat
superseded library-формой), `sides` не сериализуется — двусторонность
выражается только `twosided`.

**Контекст.** Library payload по 08 §5 содержит только `{ "library":
"<name>" }`. При этом S2 требует Publish существующего MI и Blender
writer/reader. Активный норматив не определяет:

- что делать с scalar/vector/texture overrides у MI, parent которого найден
  под `library_root` (reject, discard или расширение JSON);
- как Blender представляет library-mode: доступная RNA-поверхность dag4blend
  имеет `is_proxy`/`proxy_path`, но 08 не связывает её с `library`;
- участвует ли dag4blend `sides` в v4, тогда как static bool сериализация
  запрещена.

**Вопрос.** Подтвердить, что любой override у library-parent делает Publish
`MH_E_MATERIAL_NOT_ROUNDTRIPPABLE`, либо задать другой lossless контракт;
зафиксировать Blender mapping для `library` и судьбу `sides`.

**Временное fail-closed правило.** Library-mode Blender export/reader и UE
Publish не угадывают mapping и остаются недоступны. Никакие proxy paths или
parent asset names не преобразуются в logical name автоматически.

**Прежний статус.** ОТКРЫТ; блокировал library-mode часть S2 и круговой
acceptance.

## OPEN-V4-9 — `UMHMaterialSourceData.ParentClass` для library-form

**Статус. РЕШЕНО OWNER — нормативно в 08 §7 (этот docs-коммит).**
Поле переименовывается в `AppliedParent`; persisted-значение — tagged
logical token `class:<токен>` | `library:<имя>` (форма `tag:name`, как у
`FMHResourceKey::ToString`; алфавит `[a-z0-9_]+`). UE object path не
хранится. Поле receipt-only: import/publish/extract по нему не резолвят
(резолв — source JSON + текущие настройки, extract — live reverse-lookup
фактического parent'а). Asset Registry tags не расширяются. Library
applied state разблокирован.

**Контекст.** 08 §7 требует хранить в `UMHMaterialSourceData` поле
`ParentClass`. Для class-form 08 §5 задаёт logical token `class`, но для
library-form parent резолвится из отдельного token `library` и не
является material class. Активный норматив не определяет, должно ли
поле хранить library token, object path, пустое значение или иметь
иное имя/форму. Любой выбор меняет persisted applied-state и Asset
Registry tags.

**Вопрос.** Что точно записывается в `ParentClass` для class-form и
library-form: logical token, UE object path или другая tagged форма? Нужно
ли переименовать поле, чтобы library-parent не маскировался под class?

**Временное fail-closed правило.** UE не коммитит applied state и не
сохраняет managed MI для library-form; Publish/Adopt library-mode не
предполагает форму persisted metadata. Class-form может быть
реализована и протестирована, но S2 в целом не готов к
acceptance до ответа owner.

**Прежний статус.** ОТКРЫТ; блокировал library applied state и готовность
S2.

## OPEN-V2-1 — Provisioning `project_uid`

**Статус. SUPERSEDED BY 08 §§1, 3.** UUID не существуют нигде; Project Index
имеет заданное расположение и является rebuildable cache, а не authority.

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

**Статус. SUPERSEDED BY 08 §11.** Миграции файлов и переходного dual-read окна
нет; единственный тестовый v2-ассет пересоздаётся.

**Контекст.** `MH_W_LEGACY_PAYLOAD_NO_PASSPORT` разрешён только migration scan,
после переходного окна факт должен стать ошибкой даже в studio tooling.

**Вопрос.** Какая дата/релиз закрывает окно предупреждения?

**Предложение.** Первый общий Blender Extension + UE plugin release, прошедший
owner field acceptance и миграцию активных source roots. Receipt релиза хранит
список failed roots; только после нуля failed code повышается до E.

**Временное правило.** Production runtime уже quarantines missing identity.
Warning существует только в migrator; срок не разрешает dual-read.

## OPEN-V2-3 — Rename-to-match UX

**Статус. SUPERSEDED BY 08 §2.** Identity задаётся kind + canonical logical
name, rename является breaking DELETE+CREATE и не создаёт alias.

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

## CLOSED / SUPERSEDED BY 08 — frozen v1 questions

Вся таблица ниже — история прежних протоколов, не активные решения v4.

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

## SUPERSEDED BY 08 — прежние non-negotiable rules

Список ниже сохранён как история v2 и целиком ненормативен для v4.

- source tree содержит только three primary payload types;
- runtime не читает manifest и не выбирает revision по mtime;
- FBX без valid consensus passport quarantined;
- один mesh UID имеет один Combined-LOD FBX;
- Blender writer не имеет cache/diff; Import Composite cache строится молча;
- UE startup/watcher сравнивает scan с Ledger;
- legacy reader существует только в migration utility.

## UE plugin questions (этап C)

### UE-QUESTION-13 — passport carrier property key

**Статус. SUPERSEDED BY 08 §§4, 11.** FBX passport и MH custom properties
удаляются; production-код v2 ниже описан только исторически.

**Прежний статус.** РЕШЕНО фактом v2 writer: production код
`addon/mh4blend/core/fbx_passport.py` пишет carrier custom property
`mh_fbx_passport` (underscore) со schema `mh.fbx_passport`. UE reader обязан
читать это имя. Внимание ревьювера: §4.2 `05_source_schema_v1.md` словами
называет property `mh.fbx_passport` — это расхождение формулировки с writer;
требуется одно-строчная правка документа, байты payload'ов не меняются.

### UE-QUESTION-14 — объём `mh.fbxdump --full`

**Статус. SUPERSEDED BY 08 AS AN OLD DIAGNOSTIC CONTRACT.** Старый dump tag
опирается на passport и `mh_lod_level`; 08 не переносит этот формат в v4 и не
делает его acceptance-требованием. Если mapper-facing dump понадобится S5,
его scope требует отдельного owner-вопроса, а не наследования ответа ниже.

**Контекст.** v2 §7.2 контракта 07: dump печатает passport, Model graph,
`mh_lod_level`, slots, axis/units, counts. Не ратифицировано, обязан ли
`--full` уже в теге 1 содержать mapper-facing layer arrays
(normals/smoothing/UV/colors) или topology-only формы C0 достаточно до C2.

**Временное правило.** Тег `mh.fbxdump:1` сохраняет topology-only `--full`;
расширение формата выполняется bump'ом тега вместе с fixtures C2.

**Прежний статус.** ОТКРЫТ.

### UE-QUESTION-15 — authority applied-state: Ledger или import data ассета

**Статус. SUPERSEDED BY 08 §§3, 7.** Applied state задан в соответствующем UE
asset, Project Resource Index является rebuildable cache.

**Контекст.** Owner уточнил, что итоговая выжимка архитектурного агента является
ключевым pivot развития UE-инструмента и имеет приоритет над его первым
сообщением. Целевое решение: authoritative last-applied graph внутри каждого
`UStaticMesh` через штатный `UInterchangeAssetImportData`, а
`UMHImportLedger` — disposable dashboard/index. Вариант первого сообщения с
самостоятельным `UMHStaticMeshImportData : UAssetImportData` не считается
равноправной альтернативой. Это всё ещё формально противоречит действующему
`07` §§2,4,12 и D44, которые пока не обновлены нормативным docs-коммитом.

**Вопрос.** Каким docs-коммитом и с какого gate формально supersede-ятся
`07`/D44: `UInterchangeAssetImportData` становится applied authority, а Ledger
остаётся derived index? Нужен ли тонкий MH summary только как custom attributes
в `CachedNodeContainer` или позднее допустим subclass import data?

**Временное правило.** Текущий C1 завершается по действующему контракту, потому
что в нём ещё нет derived assets/Interchange lifecycle. Детектор изолирован за
`IMHChangeDetector`, resolver — за `IMHSourceResolver`; первый post-C1 slice
заменяет state implementation без изменения coordinator выше этих seam'ов.
Новые поля on-disk протокола без отдельного amendment не вводятся.

**Прежний статус.** РЕШЕНО — нормативный docs-коммит выполнен:
`ADR_V3_interchange_hybrid.md` supersede-ит `07` §§2,4,12 и applied-authority
часть D44. Applied authority — `UInterchangeAssetImportData` (stock, MH-state
как custom attributes в `CachedNodeContainer`); Ledger — derived index. Subclass
import data — только по триггерам ADR v3 §3, новым вопросом. Gate-маршрут —
C2.0 (ADR v3 §9).

### UE-QUESTION-16 — Interchange spike до C2

**Статус. SUPERSEDED BY 08 §10.** Stock Interchange не используется как основа;
v4 закрепляет direct FBX SDK → `IMHGeometryTranslator` → `FMHSceneIR`.

**Контекст.** Архитектурный агент предложил до C2 проверить
`UInterchangeFbxTranslator -> UInterchangeBaseNodeContainer -> UStaticMesh` и
сохранение `CachedNodeContainer`. Однако `07` §12 явно выносит Interchange из
C-scope, а текущий `FMHFbxBackend` и direct FBX SDK являются утверждённым путём
C2. Предложение дополнительно затрагивает lifecycle, module dependencies,
provenance и будущий UE->source export.

**Вопрос.** Owner подтвердил Interchange pivot. Нужно ратифицировать только его
gate-положение: является ли доказательный spike отдельным блокирующим gate
между C1 и C2 или первым под-gate нового C2?

**Временное правило.** Interchange dependencies и factories в C1 не
добавляются. После внешней приёмки C1 первым выполняется transport/applied-state
spike из итоговой архитектурной выжимки: stock translator проверяется на
production FBX, при потере обязательной семантики заменяется `UMHFbxTranslator`
над direct SDK. Над transport вводится собственный нейтральный
`FMHSceneIR`/material/composite IR; importer и будущий exporter не получают
две независимые модели данных. Direct FBX SDK/R1 остаются transport/parity
foundation и будущим deterministic writer, но не владеют UE import lifecycle.

**Прежний статус.** РЕШЕНО — ADR v3 §9: spike является первым блокирующим под-gate
нового C2 с обозначением **C2.0** и собственной квитанцией; внешний аудит
остаётся на границе C2 в целом.

### UE-QUESTION-17 — граница startup между C1 и C3

**Статус. SUPERSEDED BY 08 И S6.** Startup scan, watcher и commandlets входят в
единый срез S6 после появления builders в предыдущих срезах.

**Контекст.** Checklist `07` §13 требует в C1 `startup silent/prompt`, но `07`
§10 и тот же checklist относят startup/watcher/`MHImportSources` к C3. В C1
ещё нет builders и безопасного Execute, поэтому настоящий silent auto-import
невозможен без преждевременного начала C2/C3.

**Вопрос.** Считать ли достаточным для C1 автоматический startup
`Scan -> Resolve -> Analyze -> Plan` с silent/prompt presentation, оставив
asset mutation, watcher и `LedgerCommit` успешного импорта на C2/C3?

**Временное правило.** C1 может автоматически построить и показать/залогировать
план, но не продвигает Ledger и не мутирует assets. Оба режима используют те же
`IMHSourceResolver` и `IMHChangeDetector`; Execute появляется только вместе с
контрактными builders.

**Прежний статус.** ОТКРЫТ.

### OPEN-V4-1 (ранее UE-QUESTION-18) — filesystem aliases на reader paths

**Контекст.** Лексическая path-канонизация не доказывает, что junction/symlink
под разрешённым каталогом физически не ведёт обратно в `source_root`. Это
создаёт обход строгого запрета записи source-файлов через `-report` и риск
сканирования payload за физической границей root. Активный `08` не задаёт
отдельную policy для filesystem aliases.

**Вопрос.** Разрешать ли в будущем явно настроенный alias самого `source_root`
после physical-root canonicalization, либо aliases должны оставаться полностью
запрещены для source scan и diagnostic report output?

**Временное правило.** Diagnostic/commandlet report output, рассматриваемый
этим вопросом, разрешён только под `Saved/Mimir`; любой symlink/junction-
компонент такого output или source path отклоняется fail-closed. Scan отклоняет
alias ниже границы настроенного `source_root`; сам root пока определяет границу
и может быть заменён physical canonicalization после решения вопроса.
Обязательный v4 Project Index по пути
`Saved/MimirBridge/ProjectIndex.sqlite` задан 08 §3 отдельно и этим временным
ограничением report output не переопределяется.

**Статус.** ОТКРЫТ; действует временное fail-closed правило. Упоминание его
реализации в C1 является исторической квитанцией, а не приёмкой v4.

### UE-QUESTION-19 — организационные/групповые Empty в mesh-ресурсе

**Статус. ЧАСТИЧНО SUPERSEDED BY 08 §§4, 12.** Полная иерархия, parent closure
и резервирование костей выживают. `mh_lod_level`, Carrier B, `mh_uid` и
duplicate-node-UID repair superseded.

Field-факт (перекорневление детей невыбранного Empty с запечёнными
transforms; честность `MHFbxDump`) — в `AMENDMENT_node_hierarchy.md` §1.

**Прежний статус.** РЕШЕНО OWNER (r2 amendment'а): полная иерархия — группы + меши —
транспортируется null nodes с сохранением parenting; `mh_lod_level`/Carrier B
на группы не распространяются; замыкание по родителям fail-closed
(`MH_E_PARENT_OUTSIDE_RESOURCE`); random/variant механики в mesh FBX нет;
кости зарезервированы будущим amendment. Одновременно ратифицирован
deterministic duplicate node-UID repair (`MH_W_NODE_UID_REASSIGNED`);
material UID не чинится никогда.
