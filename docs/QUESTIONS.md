# QUESTIONS — открытые вопросы Source Protocol v5

Статус: на ветке V5-S0 freeze candidate — `docs/10_source_protocol_v5_plan.md`
и `docs/11_v5_agent_slices.md`; они становятся единственной authority только
после owner merge/ratification V5-S0. До этого production-код v5 запрещён.
После ratification открытый `OPEN-V5-*` не ослабляет 10: затронутая часть
остаётся fail-closed STOP.

Активны `OPEN-V5-1`…`OPEN-V5-7` ниже. Решённые V4-вопросы — история;
`OPEN-V4-1` перенесён в `OPEN-V5-7`, а `OPEN-V4-24` document-world прямо
superseded parent-local контрактом v5.

## OPEN-V5-1 — bit contract `mh.random_stream:1` и weighted selection

**Контекст.** Owner зафиксировал один cross-host stream, int32 placement Seed,
draw-order и запрет `FMath::Rand`/`FRandomStream`, но не выбрал PRNG algorithm,
state width/constants, отображение signed int32 Seed в state, raw draw width,
преобразование draw в `[0,1)` и правило границ weighted interval. Без этого два
независимых корректных implementation дадут разные choices.

**Вопрос.** Какой точный битовый алгоритм и wire/reference pseudocode имеет tag
`mh.random_stream:1`? После Dagor parity probe owner выбирает A — bit-for-bit
Dagor RNG либо B — собственный RNG с behavioral compatibility; нужны также
правила zero-weight/endpoint и consumption при единственной positive option.

**Временное fail-closed правило.** V5-S1 может построить fixture, извлечь
Dagor observations и подготовить API, но не объявляет Python reference или seed
expected vectors принятыми. C++ RNG и V5-S2 не начинаются.

**Статус.** ОТКРЫТ; блокирует accepted random reference и весь V5-S2.

## OPEN-V5-2 — binding и application contract `.placement`

**Контекст.** Заданы resource `<name>.placement`, `kind=placement_profile`,
`"v":1` и пары `[base,deviation]` для offset/rotation/uniform/vertical, но не
задано поле ссылки из composite/random node. Также не определены порядок
composition с authored Local T/R/S, rotation order/convention, vertical axis,
умножение uniform×vertical, admission диапазона scale, пересекающего zero, и
место `.placement` в batch publish order.

**Вопрос.** Как называется и где разрешено profile-reference поле? Как
математически из sample строится Local transform, какие profile ranges
невалидны до sampling и когда profile публикуется относительно meshes/leaf
composites/root?

**Временное fail-closed правило.** Reader не принимает выдуманное reference
field; Dagor `include`, profile binding, sampling и Publish с profile получают
STOP. Отдельный `.placement` допускается только как документационный fixture,
не как production-managed resource.

**Статус.** ОТКРЫТ; блокирует profile-часть V5-S1/V5-S2/V5-S3/V5-S4.

## OPEN-V5-3 — NodePath, closure hash и ResolvedSignature bytes

**Контекст.** `FMHResolvedCompositePlan` обязан содержать NodePath/trace и
`ResolvedSignature = hash(closure hash + seed + indices + samples + resolver
version)`, но не заданы NodePath encoding через nested composites/options,
canonical closure serialization, hash algorithm/domain tags, sample byte form
и resolver-version token.

**Вопрос.** Каковы exact byte preimage, ordering и self-describing output tag
ResolvedSignature? Входит ли display-only name, raw source path или только
ResourceKey/canonical payload hash?

**Временное fail-closed правило.** До решения trace может существовать только
как отладочная in-memory структура; signature не persist/cook/cache authority,
cross-host signature golden не утверждается.

**Статус.** ОТКРЫТ; блокирует accepted V5-S1 signatures и V5-S5+ cache/parity claims.

## OPEN-V5-4 — UE carrier и applied state для `placement_profile`

**Контекст.** Индекс v4 и шесть Asset Registry tags объявлены выжившими без
изменений, но новый source kind должен участвовать в dependency/cook closure.
Не указано, получает ли он UAsset/receipt/GeneratedAssets row и какой generated
path, либо resolver читает canonical source через index без managed carrier.

**Вопрос.** Каков UE carrier placement profile, его generated path, applied
receipt/hash policy и место в import order? Если carrier отсутствует, как
package/cook получает immutable profile data без source-tree runtime access?

**Временное fail-closed правило.** Не добавлять новый UAsset, седьмой tag,
generated path или source-at-runtime fallback. Index может документировать
candidate kind/edge, но production import/compile profile STOP.

**Статус.** ОТКРЫТ; блокирует UE `.placement` path V5-S2+.

## OPEN-V5-5 — exact admission predicate для host TRS/shear

**Контекст.** `MH_E_UNREPRESENTABLE_TRANSFORM` фиксирован на Dagor import,
Blender export и UE compile; silent approximation запрещена. Однако host
matrices имеют float precision, а owner не задал алгоритм/predicate и допуск,
который отличает round-trip noise от shear. v4 tolerance относился к иному
document-world контракту и не может быть молча унаследован.

**Вопрос.** Какой exact/float32 predicate и сравниваемые величины одинаково
реализуются Python и C++? Допускаются ли negative scales/reflections и какой
canonical decomposition выбирается при нескольких эквивалентных T/R/S?

**Временное fail-closed правило.** Production conversion/compile boundary с
не-identity parent scale/rotation не реализуется; никакой epsilon/decompose
repair не выбирается исполнителем. Fixture может только объявить expected
reject.

**Статус.** ОТКРЫТ; блокирует transform admission в
V5-S2/V5-S3/V5-S5/V5-S7.

## OPEN-V5-6 — authoritative GAZ-53 option resources и transform/profile data

**Контекст.** Owner задал имена трёх composite-файлов, topology root→body+random
и три options weight 1, но не передал authoritative option resource tokens,
Dagor source fixture, authored transforms или placement profile values.
Поиск в репозитории и локальных project docs этих файлов не обнаружил.

**Вопрос.** Какие exact option resources и Dagor исходники являются oracle для
первого end-to-end golden? Какие profile values должны давать ожидаемые
transforms для seed set?

**Временное fail-closed правило.** V5-S0 хранит topology-only fixture с явно
synthetic canonical option tokens; он не является production GAZ content и не
фиксирует seed expected traces/signatures. V5-S1 parity acceptance ждёт
owner-provided/confirmed oracle.

**Статус.** ОТКРЫТ; не блокирует documentation freeze, блокирует GAZ field
parity и финальный V5-S1 golden.

## OPEN-V5-7 — filesystem aliases на reader paths

**Контекст.** Лексическая path-канонизация не доказывает, что junction/symlink
под разрешённым каталогом физически не ведёт обратно в `source_root`. Это
создаёт обход запрета записи source-файлов через report output и риск сканирования
payload за физической границей root. V5 сохраняет v4 index/source-root contract,
поэтому прежний `OPEN-V4-1` переносится без изменения сути.

**Вопрос.** Разрешать ли явно настроенный alias самого `source_root` после
physical-root canonicalization, либо aliases остаются полностью запрещены для
source scan и diagnostic report output?

**Временное fail-closed правило.** Diagnostic/commandlet report output разрешён
только под `Saved/Mimir`; любой symlink/junction-компонент output или source path
отклоняется. Scan отклоняет alias ниже границы настроенного `source_root`; сам
root пока определяет boundary. `Saved/MimirBridge/ProjectIndex.sqlite` задан
отдельно и этим ограничением report output не переопределяется.

**Статус.** ОТКРЫТ; действует прежнее fail-closed правило. Не блокирует
V5-S0/V5-S1 pure-contract work, блокирует ослабление path admission.

---

## OPEN-V4-23 — ручной file-drop `.composite` вне `source_root`

**Статус. РЕШЕНО OWNER — нормативно в 08 §6.1 (этот docs-коммит).**
Drop внешнего payload-файла открывает **Adopt-диалог** (папка внутри
`source_root` + имя, предзаполненное stem'ом): каноничная валидация имени,
duplicate — fail-closed reject без overwrite, копирование sibling-tmp →
atomic replace, затем ШТАТНЫЙ импорт. Отмена диалога = полный отказ без
следов. Content Browser target НИКОГДА не влияет на generated path —
identity определяет путь (`/Game/MH/Generated/<Kind>/<name>`),
подтверждено. Семантика единообразна для всех payload-kinds по мере
реализации их file-drop UX. «Молчаливого копирования» нет — диалог делает
выбор явным; это то же Adopt-прецедентное правило, что у материалов 08 §5.

**Контекст.** S6 требует UX импорта и единственную source-authority, но 08/09
не задают поведение стандартного Content Browser file-drop для файла,
находящегося вне настроенного `source_root`. Молчаливое создание произвольного
`UMHCompositeAsset` нарушило бы identity и детерминированный generated path;
молчаливое копирование файла выбрало бы новую source-папку без явно
ратифицированной Adopt-семантики.

**Вопрос.** Должен ли drop внешнего `.composite` открывать Adopt-диалог и
атомарно копировать payload в выбранную папку внутри `source_root`, либо такой
файл всегда отклоняется и пользователь сначала сам помещает его в source-tree?
Подтвердить также, что Content Browser target не меняет канонический путь
`/Game/MH/Generated/Composites/<logical_name>`.

**Временное fail-closed правило.** File-drop принимает только exact
`.composite`, уже находящийся внутри `source_root`, совпадающий с единственным
resolved candidate `composite:<logical_name>`, и только в каноническом
generated package. Outside-root, duplicate/ambiguous и произвольный Content
Browser target отклоняются с абсолютным путём и `MH_E_*`; копирование и repair
не выполняются. Это не блокирует ручной импорт канонического project-source
файла и размещение уже импортированного `UMHCompositeAsset` в уровне.

**Прежний статус.** ОТКРЫТ; блокировал только Adopt/copy внешнего файла, но
не fail-closed ручной импорт внутри source-tree.

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

**Ревизия owner (полевое решение, PR #16).** Часть первоначального решения
о dag4blend пересмотрена owner'ом в поле: для class-формы writer теперь
АВТОМАТИЧЕСКИ извлекает семантику из заполненного `dagormat`
(`shader_class`, `textures.tex0–tex15`, `optional`, `sides 0|1 → явный
twosided`); mh4blend property group — приоритетные точечные overrides.
Непредставимое (`sides=2`, типы вне number/vector4) — fail-closed
`MH_E_MATERIAL_NOT_ROUNDTRIPPABLE`, без потери данных. НЕ пересмотрено:
`is_proxy`/`proxy_path` не читаются, proxymat superseded library-формой,
`tex16support` не существует. Нормативный текст — 08 §5 (правка PR #16).

**Прежний статус (первоначальное решение). РЕШЕНО OWNER — нормативно в 08
§5 (этот docs-коммит).**
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

## OPEN-V4-10 — закрытая грамматика и transform contract Composite v4

**Статус. РЕШЕНО OWNER — нормативно в 08 §§6, 7 (этот docs-коммит).**
Грамматика закрыта по фактической форме §6 (`nodes`/`resource`/вложенный
`transform`): точные field sets по kind, `children` у любого kind,
identity-дефолты, значимый порядок узлов, duplicate-key detection,
unknown → `MH_E_COMPOSITE_GRAMMAR`; reuse pre-registered
`MH_E_NAN_INF_VALUE`/`MH_E_INVALID_SCALE`/`MH_E_UNSUPPORTED_NODE_KIND`.
Канонические байты — режим §5 (общее ядро, float32 shortest, опускание
дефолтов, общие golden-векторы). `AppliedHash` — зеркало материального:
hash канонического extract применённого `UMHCompositeAsset` (08 §7).
Transform: каноническое пространство — конвенция UE (cm, оси UE,
`[x,y,z,w]` FQuat); определяющее свойство — равенство мировому трансформу
FBX-пути §4, с обязательным parity-гейтом; `core/transforms.py` не
authority. Подтверждено: actor token в Blender хранится lossless без
валидации (валидирует только UE), source-wins warning относится только к
managed UE asset.

**Контекст.** 08 §6 задаёт пример JSON-дерева и называет kinds
`mesh|actor|composite|group`, поля transform и optional `name`, но не закрывает
грамматику так, как это сделано для Material в §5. Одновременно S3 требует
круговой Blender/UE round-trip, Publish, applied-state hash и компиляцию
размещений. Действующий `addon/mh4blend/core/transforms.py` ссылается на старый
schema §11; в активном 08 §11 описана миграция, а прежний schema-документ
superseded. Использование старого mapping без решения owner означало бы тихое
возвращение снятого норматива.

Не определены:

- точные required/allowed fields для каждого kind, право владеть `children`,
  обязательность `transform`, семантика порядка `nodes`/`children`, duplicate
  keys и unknown-field policy;
- числовой домен и canonical bytes: порядок полей, spelling чисел, финальный
  LF, subject `AppliedHash`;
- Blender ↔ UE mapping осей/handedness, порядок quaternion, normalize/sign
  canonicalization, precision/quantization и правила scale;
- должен ли Blender только сохранять actor token (registry существует только
  в UE), и относится ли source-wins warning только к `UMHCompositeAsset` либо
  также к рабочей Blender-сцене.

**Вопрос.** Зафиксировать закрытый Composite v4 contract: field set и nesting
для каждого kind; обязательность и точную форму transform; order semantics;
unknown/duplicate policy и diagnostic codes; canonical byte/hash форму;
полный Blender↔UE transform mapping. Отдельно подтвердить, что actor registry
validation выполняется UE, Blender losslessly хранит token, а applied-state
warning относится к managed UE asset.

**Временное fail-closed правило.** Composite codec, Blender import/export и UE
asset/compiler/import/publish остаются заблокированы существующим
`MH_E_INVALID_COMPOSITE`; старый transform codec не используется как authority.
Информация о материалах и UID не принимается ни при каких условиях.
Независимые S3 cleanup и mesh-import §4.1 не интерпретируют Composite bytes и
могут выполняться, но весь S3 не готов к acceptance до ответа owner.

**Прежний статус. ОТКРЫТ.** Блокировал Composite-часть и готовность S3.

## OPEN-V4-11 — конфликт префикса `UCX_` и collision suffix

**Статус. РЕШЕНО OWNER — нормативно в 08 §4 (этот docs-коммит).**
Никакого precedence: любой двойной маркер запрещён, включая семантически
совпадающий `UCX_*_cls_both` (закрытое правило без спец-случаев). Заодно
закрыты соседние ловушки того же семейства: mesh с `SOCKET_`, null с
`UCX_`/`_cls_*`, null с терминальным `_lodNN`, `SOCKET_`-узел с детьми.
Единый код `MH_E_INVALID_NODE_MARKERS` (регистрируется в S3; Blender
export/§4.1-импорт, зеркалирует S5). Repair не существует.

**Контекст.** Таблица 08 §4 распознаёт collision по префиксу `UCX_` ИЛИ по
суффиксу `_cls_phys|trace|both`: `UCX_` означает QueryAndPhysics, а суффиксы
задают PhysicsOnly/QueryOnly/QueryAndPhysics. Имя вроде
`UCX_body_cls_phys` одновременно требует двух разных режимов; precedence или
reject не заданы. Общий classifier S3 должен одинаково обслуживать Blender
export/import и будущий UE importer S5.

**Вопрос.** Запретить любой двойной marker, разрешить только семантически
совпадающий `UCX_*_cls_both`, либо задать точный precedence префикса/суффикса?

**Временное fail-closed правило.** Конфликтующий двойной marker блокируется
зарегистрированным `MH_E_INVALID_RESOURCE_SOURCE`; никакой режим collision не
выбирается автоматически. Имена без двойного marker и семантически однозначные
правила таблицы §4 реализуются без repair.

**Прежний статус. ОТКРЫТ.** Блокировал только конфликтующие collision names
и финальный общий classifier acceptance S3/S5.

## OPEN-V4-12 — state machine и rebuild-контракт Project Index

**Статус. РЕШЕНО OWNER — нормативно в 08 §3 (этот docs-коммит).**
Индекс — чистая проекция без tombstones/history; meta-tag
`mh.project_index:4`; любой mismatch/коррупция → удалить файл и полный
rebuild, миграций кэша нет. Словари: `parse_status ∈ {ok, noncanonical,
unreadable, invalid_payload}` (mesh до S5 — ok при читаемом каноничном
файле); `resolution_status` — чистая функция кандидатов и референсов,
ambiguous ВСЕГДА побеждает invalid; `missing` живёт ровно пока жив
референс; `GeneratedAssets.status ∈ {applied, stale, orphan,
invalid_receipt, duplicate_claim}`; двойная заявка ключа ассетами —
`MH_E_AMBIGUOUS_GENERATED_ASSET` (S4), плохие managed-строки блокируют
только свой ключ, не rebuild. «Идентичный индекс» = равенство
нормализованного логического дампа без volatile-полей + resolver
outcomes, не байты .sqlite; in-memory tokens в дамп не входят.

**Контекст.** 08 §3 перечисляет таблицы и часть полей, а 09 S4 требует
идентичного восстановления после удаления SQLite. Для детерминированной схемы
и acceptance не определены:

- множество ключей, для которых хранится `ResourceKeys.missing` (только
  dependency targets, generated assets, исчезнувшие candidates или их union),
  срок жизни missing-строки и точный смысл `invalid`;
- precedence `ambiguous`/`invalid`, если у одного key несколько candidates и
  один либо несколько payload не парсятся;
- словари/переходы `ResourceCandidates.parse_status`,
  `GeneratedAssets.status` и семантика `generation`;
- представление неканоничного filename, из которого нельзя получить валидные
  `(kind, name)`, и конфликт нескольких UE assets, заявляющих один ResourceKey;
- rebuild или блок при corrupt/unsupported schema;
- означает ли «идентичный индекс» равенство упорядоченных typed rows и resolver
  outcomes либо физически идентичные SQLite bytes, и входят ли transient event
  diagnostics/tokens в это равенство.

**Вопрос.** Зафиксировать exact state vocabulary/precedence/retention,
generation и schema-mismatch policy для пяти таблиц §3, а также точный subject
acceptance «delete `.sqlite` → identical index». Должны ли malformed/duplicate
managed Asset Registry rows блокировать соответствующий key либо весь rebuild?

**Временное fail-closed правило.** До решения resolver не считает key
разрешённым без ровно одного каноничного и валидного candidate; persistent
SQLite state machine и Ledger replacement не реализуются. Corrupt cache не
интерпретируется и не мигрируется.

**Прежний статус. ОТКРЫТ; блокировал S4 schema, rebuild и rename/orphan
acceptance.**

## OPEN-V4-13 — applied raw state и покрытие GeneratedAssets

**Статус. РЕШЕНО OWNER — нормативно в 08 §7 (этот docs-коммит).**
Вводится ШЕСТОЙ Asset Registry tag `MH.SourceHash` (raw, blake3-160) —
поправка owner: фиксация «ровно пять» предшествовала удалению Ledger;
без raw hash в тегах детектор грузил бы каждый ассет на каждом скане.
Receipts в ассетах — authority, теги — проекция; расхождение →
`invalid_receipt`. Покрытие S4: material, composite и texture (вводится
минимальный `UMHTextureSourceData` + теги); StaticMesh-строки — только
с S5; tagged-ассет без carrier — `invalid_receipt`, rebuild не блокирует.
Детектор: candidate raw_hash vs `MH.SourceHash` (equal → NO_CHANGE,
differs → stale/REIMPORT, нет строки → CREATE). S3-тест «exact five
tags» обновляется в S4 на шесть.

**Контекст.** 08 §7 фиксирует ровно пять Asset Registry tags:
`MH.Kind`, `MH.LogicalName`, `MH.SourcePath`, `MH.AppliedHash`, `MH.Managed`;
расширять их нельзя. `GeneratedAssets` строится из этих tags, но raw
`SourceHash`, необходимый после удаления Ledger для честного
`REIMPORT/NO_CHANGE`, среди них отсутствует. Material/Composite имеют raw
receipt внутри UObject, StaticMesh carrier появляется только в S5, а для
генерируемой Texture §7 вообще не задаёт applied-state carrier/tags, хотя
texture имеет ResourceKey и generated path §§2/8.

**Вопрос.** Откуда S4 change detector получает last-successful raw source hash:
загружает UObject по `ue_object_path`, читает receipt и пишет отдельную derived
колонку, либо использует иной owner-пинованный источник без шестого AR tag?
Какие kinds обязаны входить в `GeneratedAssets` уже в S4; должен ли S4 вводить
texture carrier, а mesh-row откладывать до S5? Как обрабатывается tagged asset,
для kind которого carrier ещё не существует?

**Временное fail-closed правило.** Не выводить applied raw state из
`AppliedHash` другого semantic domain, не добавлять шестой tag и не записывать
предположительные Texture/StaticMesh receipts. До решения Ledger/change
detector не заменяется неполной моделью.

**Прежний статус. ОТКРЫТ; блокировал Ledger purge, GeneratedAssets и change
analysis.**

## OPEN-V4-14 — домен и роли Dependencies в S4

**Статус. РЕШЕНО OWNER — нормативно в 08 §3 (этот docs-коммит).**
Только ResourceKey→ResourceKey рёбра; закрытые роли `texture`,
`placement_mesh`, `placement_composite`, с S5 — `slot` (до S5 slot-рёбер
нет: dependents материалов временно не включают меши — принятый
interim). Registry-tokens (actor/class/library) рёбрами не являются.
Рёбра — по кандидату с provenance `owner_path`, множество ключа = union;
unreadable/invalid кандидат рёбер не даёт. Transitive blocking — на этом
графе: blocked = status ≠ unique ИЛИ ребро на blocked target; циклы
невозможны.

**Контекст.** 09 S4 требует `Dependencies из payload-ссылок` и блокировку
dependents, но не определяет exact edge set и `role`. Material v4 даёт
`material → texture`; Composite — `composite → mesh|composite` и actor token,
который не является ResourceKey. Class/library parents также являются registry
tokens, не source ResourceKeys. `mesh → material` требует FBX slot parse из
S5, которого в UE S4 ещё нет. Для ambiguous/invalid owner с несколькими
candidates таблица §3 не имеет candidate-path provenance.

**Вопрос.** Хранит ли `Dependencies` только ResourceKey→ResourceKey edges?
Зафиксировать exact role names, включение/exclusion actor/class/library,
момент появления mesh-slot edges и политику edges для duplicate/invalid owner
(union, per-candidate provenance или полное отсутствие). На каком графе S4
обязан вычислять transitive dependent blocking?

**Временное fail-closed правило.** Не сохранять заведомо неполный dependency
graph и не объявлять transitive dependents здоровыми. Ресурс с ambiguous
ResourceKey остаётся заблокирован существующим
`MH_E_AMBIGUOUS_RESOURCE_NAME`; registry tokens не маскируются под source kind.

**Прежний статус. ОТКРЫТ; блокировал Dependencies и dependent-blocking
acceptance S4.**

## OPEN-V4-15 — incremental events, self-publish и probable rename

**Статус. РЕШЕНО OWNER — нормативно в 08 §3 (этот docs-коммит).**
S4 НЕ владеет watcher/debounce/PIE (S6); S4 даёт full scan, batched
`UpsertPaths` API (точка входа S6) и Publish-интеграцию. Token —
in-memory {path, raw_hash, generation}, регистрируется после atomic
replace и до собственного upsert, single-shot; persistence нет by
design — после restart корректность обеспечивают receipts (NO_CHANGE).
Probable rename: на границе одной generation, пара Disappeared×Appeared
только при same kind и биективном совпадении raw_hash; many-to-many →
ни пары, ни warning. `MH_W_PROBABLE_RESOURCE_RENAME` —
derived-диагностика, пока условие наблюдаемо; в rebuild-identity входят
только derived-строки, сессионные события (`SELF_PUBLISHED`) в БД не
пишутся.

**Контекст.** 09 S4 одновременно требует incremental upsert
`watcher/startup/publish`, self-publish token и probable rename, тогда как
actual `DirectoryWatcher`, debounce/coalescing и PIE queue назначены S6.
Filesystem event не переносит publish token. Не определены:

- владеет ли S4 реальной watcher-подпиской или только callable batched API для
  будущего S6;
- token identity без UUID, регистрация до/после atomic replace, persistence,
  cancel при failed publish, TTL/consumption/replay и несколько одинаковых
  publish/event observations;
- batch/window и same-kind правило probable rename, many-to-many pairing при
  одинаковом raw hash, а также lifetime transition diagnostic;
- входит ли transient token/rename diagnostic в rebuild-identical contract
  `OPEN-V4-12`.

**Вопрос.** Зафиксировать границу S4/S6 и state machine pending publication →
`SELF_PUBLISHED`, включая match tuple и crash/restart semantics. Зафиксировать
deterministic probable-rename pairing и diagnostic retention для already
coalesced event batch.

**Временное fail-closed правило.** Без однозначного pending receipt событие не
классифицируется `SELF_PUBLISHED`; token не угадывается по одному path/mtime.
Actual watcher/debounce не переносится самовольно из S6. При неоднозначном
same-hash pairing alias и probable-rename warning не создаются.

**Прежний статус. ОТКРЫТ; блокировал incremental/self-publish/rename часть
S4.**

## OPEN-V4-16 — метрика orphan rebound divergence

**Статус. РЕШЕНО OWNER — нормативно в 08 §§2, 3 (этот docs-коммит).**
Отдельной rebind-операции нет: импорт ключа в детерминированный путь §8
переиспользует живущий там managed-ассет — это и есть rebind. Метрика
бинарная и в одном домене: candidate raw_hash vs receipt-`SourceHash`
сироты (`AppliedHash` с raw не сравнивается никогда). Равенство —
молчаливо; любое отличие — `MH_W_ORPHAN_REBOUND_CONTENT_DIVERGED` в
момент импорта (warning, не блок). Слово «сильно» упразднено; порогов и
метрик подобия нет. Оба W-кода регистрируются в S4.

**Контекст.** 08 §2 требует
`MH_W_ORPHAN_REBOUND_CONTENT_DIVERGED` при re-bind сироты с «сильно
разошедшимся» содержимым, но не задаёт саму rebind-операцию/момент, сравниваемые
hash domains или threshold. Fixed Asset Registry tags не содержат raw
SourceHash; `AppliedHash` Material/Composite/StaticMesh описывает разные
semantic domains и не сравним с raw candidate hash.

**Вопрос.** Что именно инициирует orphan rebind и какие baseline/current
receipts сравниваются? Зафиксировать детерминированную divergence metric и
threshold для каждого kind либо заменить «сильно» точным hash-equality
правилом. В каком S4/S6 API выдаётся warning?

**Временное fail-closed правило.** Orphan остаётся orphan; автоматический или
неподтверждённый rebind запрещён. `AppliedHash` не сравнивается с raw hash,
warning без owner-пинованной метрики не выдаётся.

**Прежний статус. ОТКРЫТ; блокировал rebound и регистрацию/call-site warning
S4.**

## OPEN-V4-17 — GeneratedAssets status при ambiguous/invalid source key

**Статус. РЕШЕНО OWNER — нормативно в 08 §3 (этот docs-коммит).**
Вводится ШЕСТОЙ status `source_blocked`: единственный валидный managed
claim при `resolution_status ∈ {ambiguous, invalid}` — ассет и receipt
здоровы, источник нездоров; hash-сравнение не выполняется
(авторитетного кандидата нет), импорт заблокирован source-диагностикой
ключа; при выздоровлении источника строка перевычисляется в
applied/stale. Обе ветви (ambiguous и invalid) — одно значение:
различие уже хранится в `ResourceKeys.resolution_status`, дублировать
его в GeneratedAssets незачем. Прецеденс закреплён:
`duplicate_claim → invalid_receipt → {orphan | source_blocked |
stale | applied}` (source-состояния взаимоисключающи). Функция стала
тотальной — rebuild/dump acceptance определён на всём домене.

**Контекст.** После решения OPEN-V4-12 словарь
`GeneratedAssets.status ∈ {applied, stale, orphan, invalid_receipt,
duplicate_claim}` закрыт, но функция в 08 §3 определена не на всём входном
домене. Для единственного managed-ассета с валидными шестью тегами:

- `applied`/`stale` определены только при `ResourceKeys = unique`;
- `orphan` определён при отсутствии source-кандидата;
- `invalid_receipt` относится к malformed/incomplete tags или kind без
  carrier;
- `duplicate_claim` требует двух заявителей одного ключа.

Если source key имеет один `invalid_payload` candidate (`invalid`) либо два и
более candidates (`ambiguous`), receipt валиден и source существует, но ни
одно значение `GeneratedAssets.status` буквальному контракту не соответствует.
Resolver/import при этом однозначно blocked, однако нормализованный dump и
delete-and-rebuild acceptance требуют детерминированной строки GeneratedAssets.

**Вопрос.** Какой `GeneratedAssets.status` присваивать валидному единственному
managed claim при `ResourceKeys.resolution_status = ambiguous` и при
`invalid`? Зафиксировать обе ветви и, если это одно из существующих значений,
уточнить его расширённый смысл; новый status исполнитель не вводит.

**Временное fail-closed правило.** Оба key остаются import-blocked. Project
Index не записывает предположительный GeneratedAssets status и не объявляет
rebuild/dump acceptance до owner-решения.

**Прежний статус. ОТКРЫТ; блокировал total GeneratedAssets projection и S4
rebuild acceptance.**

## OPEN-V4-18 — значение `MH.AppliedHash` у managed Texture

**Статус. РЕШЕНО OWNER — нормативно в 08 §7 (этот docs-коммит).**
Для БИНАРНЫХ kinds (texture; static_mesh с S5) канонического extract не
существует: applied state = применённые source-байты, поэтому
`MH.AppliedHash == MH.SourceHash` ПО ОПРЕДЕЛЕНИЮ — это нормативное
тождество одного домена, не «приравнивание догадкой». Отдельное
receipt-поле в `UMHTextureSourceData` не добавляется; тег публикуется
из `SourceHash` и никогда не пуст. Валидатор GeneratedAssets проверяет
тождество; расхождение — `invalid_receipt`. Запрет сравнения
AppliedHash с raw (V4-16) остаётся только для канонических kinds
(material/composite), где домены действительно разные. Существующий
texture-import путь с S4 прикрепляет receipt — текстуры managed.

**Контекст.** 08 §7 требует у `UMHTextureSourceData` только поля
`LogicalName`, `SourceRelativePath`, `SourceHash` (raw), но одновременно
требует у managed Texture те же ровно шесть Asset Registry tags, включая
`MH.AppliedHash`. Semantic domain и persisted source этого шестого значения
для Texture не определены. Приравнять `AppliedHash` к raw `SourceHash`,
оставить tag пустым или добавить отсутствующее receipt-поле — три разные
нормативные модели. Пустой tag дополнительно может не попасть в Asset Registry
как заявленный tag и превратить валидную Texture в `invalid_receipt`.

**Вопрос.** Зафиксировать значение и hash-domain Texture `MH.AppliedHash`:
добавляется ли `AppliedHash` в `UMHTextureSourceData`, чему равен при успешном
импорте и какой live extract/validator (если есть) его вычисляет? Если для
Texture tag является marker без semantic hash, зафиксировать точную непустую
строковую форму и соответствующую валидацию GeneratedAssets.

**Временное fail-closed правило.** Не приравнивать semantic/raw hashes, не
добавлять скрытое receipt-поле и не публиковать неполный six-tag claim.
Существующий texture import не объявляется managed S4 carrier до решения.

**Прежний статус. ОТКРЫТ; блокировал `UMHTextureSourceData`, exact-six tags
и texture GeneratedAssets acceptance S4.**

## OPEN-V4-19 — orphan-rebind warning и чистая rebuildable-проекция

**Статус. РЕШЕНО OWNER — вариант 1, нормативно в 08 §3 (этот
docs-коммит).** Приоритет у чистой проекции: warning —
СЕССИОННОЕ СОБЫТИЕ импорта (категория `SELF_PUBLISHED` из V4-15),
выдаётся по наблюдённому в живом индексе переходу `orphan → rebind` в
Message Log/отчёт операции; в Diagnostics НЕ пишется, в rebuild-identity
НЕ входит. Обычный `stale` rebind'ом не маркируется никогда. Потеря
события через «orphan → удаление SQLite → rebuild → импорт» принята by
design (историю проекция не хранит; при совпадении hash сохраняется
derived `MH_W_PROBABLE_RESOURCE_RENAME`). Варианты 2 (history-исключение)
и 3 (durable marker в receipt) отвергнуты: первый ломает аксиому кэша,
второй превращает одноразовое уведомление в вечное состояние ассета с
lifecycle-бременем. Временная пауза авто-импорта снимается: rebind — 
обычный REIMPORT без подтверждений, source побеждает.

**Прежний статус. ОТКРЫТ; блокировал финальную приёмку S4.**

**Контекст.** 08 §3 одновременно требует, чтобы:

- Project Index и каждая строка `Diagnostics` были чистой проекцией текущих
  `(source tree + Asset Registry tags)`, а удаление SQLite и полный rebuild
  давали тот же нормализованный логический dump;
- `MH_W_ORPHAN_REBOUND_CONTENT_DIVERGED` выдавался только для импорта ранее
  orphan-ассета, если новый candidate raw hash отличается от сохранённого
  `SourceHash`, и жил derived-строкой `Diagnostics` до следующего импорта.

После появления source-файла текущая проекция такого ассета совпадает с
обычным `stale`: один unique candidate, здоровый singleton claim и
`candidate.raw_hash != receipt.SourceHash`. Факт предшествующего состояния
`orphan` выводится только из перехода между generations. Persist этого факта
в `Diagnostics`/отдельном поле превращает кэш в history; после удаления SQLite
тот же факт не восстанавливается. Если считать любое `stale` orphan-rebind,
warning становится ложным для обычного редактирования source.

**Вопрос.** Какой инвариант имеет приоритет и где должен жить warning:

1. session-only warning непосредственно в import operation по наблюдённому
   переходу `orphan -> candidate`, без persisted `Diagnostics` row;
2. persisted transition-state до следующего импорта как специально
   ратифицированное исключение из rebuild-identical Project Index;
3. новый rebuildable marker в authoritative Asset Registry receipt (нужно
   точно задать поле/tag и lifecycle);
4. иная owner-форма, сохраняющая различимость без ложного warning.

**Временное fail-closed правило.** Обычный `stale` никогда не маркируется как
orphan-rebind. Переход `orphan -> candidate` может быть обнаружен и показан
только в текущей UE-сессии, но не объявляется нормативной persisted
`Diagnostics` row и не считается прошедшим rebuild-identity acceptance до
owner-решения. Импорт ресурса с таким переходом не запускается автоматически;
план остаётся `REIMPORT`, а выполнение требует повторного подтверждения после
решения вопроса. Новые schema/tag/error-code формы не вводятся.

## OPEN-V4-20 — applied fingerprint и transaction receipt StaticMesh

**Статус. РЕШЕНО OWNER — нормативно в 08 §§7, 9 (этот docs-коммит).**
Поля v3-черновика упразднены, а не определены: `SchemaVersion`,
`RecipeHash`, `AppliedAssetHash`, `LastSuccessfulTransaction` не
существуют. Receipt: LogicalName, SourceRelativePath, SourceHash (raw),
ImporterVersion (int32-константа кода; отличие → REIMPORT даже при
равном hash), bLocallyModified. Канонического fingerprint'а собранного
UStaticMesh нет (binary kind, V4-18); детект локальной правки —
editor-hook ставит persisted-флаг, предупреждение выдаётся при импорте
и в Verify (scan мешей не грузит — принято); правки при выключенном
плагине не ловятся — best-effort класс V4-19. Явный Reimport всегда
пересобирает, игнорируя NO_CHANGE.

**Контекст.** 08 §7 требует
`UMHStaticMeshImportData : UAssetImportData` с полями `SchemaVersion`,
`LogicalName`, `SourceRelativePath`, `SourceRawHash`, `RecipeHash`,
`AppliedAssetHash`, `LastSuccessfulTransaction`; 08 §9 и acceptance S5
требуют детектировать локальную правку managed `UStaticMesh` через
`AppliedAssetHash` и восстановить source следующим импортом. При этом активные
08/09 не задают:

- типы и допустимые значения `SchemaVersion`, `RecipeHash` и
  `LastSuccessfulTransaction` (UUID запрещены инвариантом v4);
- вход и версию `RecipeHash`;
- каноническую проекцию/байты `AppliedAssetHash`: какие LOD
  `MeshDescription` attributes и build settings, material slots/object paths,
  `BodySetup` shapes/modes и sockets входят; как упорядочиваются элементы и
  кодируются float; должен ли fingerprint быть тождественен после
  save/reload;
- момент проверки локальной правки: только перед source-triggered `REIMPORT`
  либо также для `NO_CHANGE` при обычном scan (последнее требует загрузить
  каждый managed mesh); как explicit reimport заставляет source победить при
  неизменном raw hash;
- lifecycle `LastSuccessfulTransaction`: что именно он идентифицирует и когда
  продвигается относительно build → async completion → первого save → save
  receipt.

Шесть Asset Registry tags вопрос не расширяет: для binary kind нормативно
`MH.AppliedHash == MH.SourceHash == SourceRawHash`; `AppliedAssetHash` —
внутренний fingerprint локального состояния, а не седьмой tag.

**Вопрос.** Какова точная типизированная модель этих полей, каноническая
проекция/алгоритм `AppliedAssetHash`, значение `RecipeHash`, семантика
`LastSuccessfulTransaction` и cadence local-edit detection/forced reimport?

**Временное fail-closed правило.** Не подменять fingerprint `LightingGuid`,
DDC key, package bytes или произвольной UObject-сериализацией; не создавать и
не продвигать managed StaticMesh receipt/claim. S5 остаётся STOP до решения;
никакие частичные assets не публикуются.

## OPEN-V4-21 — observable mapping socket и collision nodes

**Статус. РЕШЕНО OWNER — нормативно в 08 §4 (этот docs-коммит).**
Имя сокета — БЕЗ префикса `SOCKET_` (маркер — транспорт, не identity);
пустой остаток — `MH_E_INVALID_NODE_MARKERS`, дубликаты имён —
`MH_E_INVALID_RESOURCE_SOURCE`. Каждый collision-узел → ровно один
convex element (hull transformed control points); декомпозиции и
примитив-фиттинга нет, невыпуклое хуллится (стандарт UCX), дегенерат —
`MH_E_INVALID_RESOURCE_SOURCE`; per-shape `CollisionEnabled` по таблице
§4, `CTF_UseDefault`, авто-коллизии при отсутствии узлов нет.

**Контекст.** Таблица 08 §4 распознаёт null `SOCKET_*` как
`UStaticMeshSocket`, а mesh `UCX_*`/`*_cls_phys|trace|both` как BodySetup shape
с соответствующим `CollisionEnabled`. Не зафиксированы две наблюдаемые части
результата:

- `UStaticMeshSocket::SocketName`: сохраняется полный FBX token
  `SOCKET_grip` или marker снимается и публичное имя равно `grip`; допустим ли
  пустой token `SOCKET_`;
- каждый collision mesh становится отдельным convex hull из всех transformed
  control points либо `_cls_*` использует другую форму/cooking; что делать с
  невыпуклой/дегенеративной геометрией и несколькими nodes одного режима.

Это пользовательски видимое состояние `UStaticMesh` и часть
`AppliedAssetHash`; silent repair/угадывание запрещено.

**Вопрос.** Какое точное имя получает socket и какая shape/cooking policy
применяется к `UCX_*` и `_cls_*` collision nodes?

**Временное fail-closed правило.** Не материализовывать sockets/collision в
production `UStaticMesh` и не заявлять частичную приёмку S5 до owner-решения.

## OPEN-V4-22 — machine code transport-level FBX failures

**Статус. РЕШЕНО OWNER — нормативно в 08 §4 (этот docs-коммит).**
Единый код `MH_E_FBX_TRANSPORT_FAILED` (регистрируется в S5 + golden
counts) для всех transport-отказов: SDK init/чтение, corrupt scene,
axis/units mismatch, triangulation, невалидные layer indices, отказ
axis-probe. Имя probe-кода `MH_E_GEOMETRY_SOURCE_MISMATCH` ратифицируется
НЕ будет — «mismatch» вводит в заблуждение для corrupt-файлов (прецедент
V4-4); заменить во всех call sites. Блок ресурса; slot-рёбра из
неразобранного FBX не извлекаются.

**Контекст.** 08 §4 пинует canonical axis/unit transport и закрытый static-mesh
диалект. Direct FBX SDK обязан fail-closed отклонять как минимум corrupt/
нечитаемый FBX, несовпадающие axis/units, неудачную triangulation и
невалидные geometry layer indices. Node/LOD/slot нарушения уже имеют
ратифицированные коды, но общего production-кода transport/geometry failure в
реестре нет. Существующий R1 axis-probe возвращает
`MH_E_GEOMETRY_SOURCE_MISMATCH`, однако этот код отсутствует в
`canonical.py::ERROR_CODES` и golden-списке; переносить его молча нельзя.

**Вопрос.** Ратифицировать `MH_E_GEOMETRY_SOURCE_MISMATCH` как единый S5-код
transport-level mismatch/invalid geometry (с регистрацией и обновлением
счётчиков) либо назначить другой существующий/новый набор машинных кодов?

**Временное fail-closed правило.** Неразобранный или неканоничный FBX не
импортируется и не даёт slot dependencies, но production diagnostic/schema не
вводятся до owner-решения; старый незарегистрированный probe-код не считается
контрактом S5.

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

**Статус. SUPERSEDED BY OPEN-V5-7.** Этот body сохранён как история; активный
вопрос и fail-closed правило перенесены наверх без смены смысла.

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

**Прежний статус.** ОТКРЫТ; действовало временное fail-closed правило. Упоминание его
реализации в C1 является исторической квитанцией, а не приёмкой v4.

### OPEN-V4-24 — group transform: document-world или parent-local

**Статус. РЕШЕНО OWNER — вариант (а), нормативно в 08 §§6, 6.1 (этот
docs-коммит).** Document-world побеждает: это контракт, ратифицированный
V4-10, реализованный S3 и закреплённый golden-гейтом
`CompilerPreservesSourceWorldTransforms` (child 125 под group 100 остаётся
world 125). Ошибочная посылка о parent-local композиции в поправке §6.1
(«представимость композиции» на Break) ОТОЗВАНА: дети при Break сохраняют
world-значения дословно, собственный трансформ группы — организационный
pivot и отбрасывается, shear на Break невозможен (значения в файле — T/R/S
по грамматике). Временный блок Break для transform-bearing групп снимается.
Настоящий shear-рубеж перенесён на экспорт из Blender: matrix_world, не
восстанавливающаяся из T/R/S-декомпозиции в допуске float32, — fail-closed
`MH_E_INVALID_RESOURCE_SOURCE`. «Drag группы двигает потомков» — поведение
редакторов (parenting в Blender, явная дельта в UE Edit), не семантика
файла.

**Контекст.** Новая поправка `08` §6.1 требует для Break композицию
`group scale × child rotation`, проверку shear и утверждает, что компилятор
сохраняет иерархию и перемножает её без потерь. Но действующий `08` §6
определяет значения узлов как UE world transforms, а принятый Automation-гейт
`Mimir.V4.Composite.CompilerPreservesSourceWorldTransforms` прямо закрепляет:
child translation `125` под group translation `100` остаётся world `125`, а
не становится `225`. Оба UE-компилятора вызывают `SetWorldTransform`, а Break
рекурсирует с неизменным document basis; group сейчас structural-only.

**Вопрос.** Какой контракт должен победить: (а) сохранить document-world и
удалить premise о group-композиции/shear из новой поправки §6.1; либо (б)
перевести всё composite-дерево на parent-local T/R/S, что требует согласованно
изменить UE compiler/placement compiler, Blender import/export transforms,
FBX parity и принятый world-transform golden, после чего Break проверяет
матричную представимость при flatten?

**Временное правило.** Пока домен transform не выбран owner'ом, Break
fail-closed отклоняет растворение transform-bearing group с размещаемыми
потомками через `MH_E_UNREPRESENTABLE_SCENE_OBJECT` и перечисляет такие группы;
тихая document-world интерпретация либо переход на parent-local запрещены.

**Прежний статус.** ОТКРЫТ; блокировал только group-transform часть Break
follow-up.

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
