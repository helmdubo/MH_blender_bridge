> HISTORY. Не норматив. Модель заменена docs/16_recipe_model.md (2026-09-02).

# 11 — Срезы реализации Source Protocol v5 для внешнего агента

Статус: **кандидат owner freeze V5-S0**. Аудитория — исполнитель без контекста
сессии и внешний ai-аудитор. До любых правок прочитать целиком
`docs/10_source_protocol_v5_plan.md`, этот файл и `README.md`. Owner merge
V5-S0 означает ратификацию; до него production-код не меняется.

## Инварианты для всех срезов

1. **10 — единственная v5 authority после ратификации V5-S0.** Конфликт кода,
   08/09, старых goldens или исторических questions с 10 решается в пользу 10.
   Новое нормативное решение не импровизируется: реальная дыра оформляется в
   `docs/QUESTIONS.md` как `OPEN-V5-*`:
   Контекст → Вопрос → Временное fail-closed правило → Статус, после чего
   затронутая часть получает STOP.
2. **Срезы строго последовательны.** V5-S<N+1> не начинается, пока owner не
   смержил/ратифицировал V5-S<N>. Отдельный внешний review не равен owner
   acceptance. Один срез — одна ветка `v5/s<N>-<slug>`, один PR, одна квитанция
   `docs/receipts/v5_s<N>.md`. PR мержит только owner.
3. **Fail-closed.** Неоднозначность, неизвестное поле/версия, непредставимый
   transform, неполное closure или divergent source блокируют ресурс и
   dependents. Никаких tolerance-repair, snapping, mtime winner, legacy
   fallback, dual-read или silent discard.
4. **Коды диагностики.** Новый `MH_E_*`/`MH_W_*` регистрируется в
   `addon/mh4blend/core/canonical.py::ERROR_CODES`, зеркальном C++ registry и
   golden counts тем же срезом, который вводит первый call site. Фиксированный
   код v5: `MH_E_UNREPRESENTABLE_TRANSFORM`. JSON random grammar расширяет
   `MH_E_COMPOSITE_GRAMMAR`; отдельное random-grammar семейство не вводится.
5. **Canonical bytes.** Python и C++ читают общие golden-файлы. Duplicate JSON
   keys reject. Float32-shortest, field order, LF/final-LF и omission rules
   определены 10. Контракты material/mesh/texture/index/applied state v4 не
   получают version fields.
6. **Source closure != resolved plan.** Closure всегда содержит все options и
   никогда не зависит от seed. Все потребители конкретного результата получают
   один `FMHResolvedCompositePlan`; random внутри component-spawning кода
   запрещён.
7. **Blender boundary.** Seed в Blender не существует. Scene authority —
   `COMPOSITE`/`MESH`/`ACTOR_PLACEHOLDERS`; `TECH` только preview.
   `reference/` read-only; `golden/` меняется только в явном scope среза.
8. **UE boundary.** Engine не форкается и не изменяется. Плагин остаётся в
   `ue/MimirComposite`. Любое неожиданное изменение Engine — STOP/receipt.
   Level Instance random не резолвит.
9. **C++ gate.** Каждый срез с C++ проходит stock UE 5.7.4:
   guarded host/plugin build; `BuildPlugin -StrictIncludes` без unity/PCH;
   force-unity с отключённым adaptive unity; `Automation RunTests Mimir`.
   Недавно изменённые file-scope helpers имеют unique names/anonymous namespace.
10. **Квитанция честная.** Разделять automated gates, Blender/UE field checks,
    внешний audit и owner acceptance. WIP commit, focused test или STOP receipt
    не являются acceptance следующего gate.
11. **Исправление дефекта начинается с failing-теста на текущем `origin/main`.**
    Квитанция содержит его вывод до изменений. Без него изменение не является
    исправлением дефекта и требует отдельной ратификации как новая работа.

---

## V5-S0 — Freeze

Цель: получить ратифицируемый, самодостаточный норматив v5 без production-code.

Scope:

1. Создать `docs/10_source_protocol_v5_plan.md`. Все выжившие части 08
   перенести внутрь; composite/document-world часть заменить v5. Зафиксировать:
   mandatory `"v": 5`, random/empty options, parent-local T/R/S, placement
   profile, seed ownership, closure/plan separation, Blender model, export batch,
   editor/runtime/cook paths и GAZ-53 acceptance.
2. Создать этот файл с последовательностью V5-S0…S7 и parked S8.
3. Добавить в 08/09 верхние supersede-баннеры с таблицами судьбы. Исторический
   body сохранить.
4. Обновить `README.md` и `KICKOFF_PROMPT.md` на 10/11. Старую v4 часть
   оставить под явным archive/superseded banner.
5. Добавить GAZ-53 source fixtures в `golden/v5/gaz53/`:
   `gaz53_b_random_cmp.composite` → `gaz53_b_body_cmp.composite` +
   `gaz53_body_bc_random_cmp.composite`; три ordered options weight 1;
   parent-local probe 100+25=125. V5-S0 не подставляет выдуманные RNG expected
   traces/signatures.
6. Выбрать diagnostic naming. Решение freeze-candidate:
   `MH_E_COMPOSITE_LEGACY_GENERATION`,
   `MH_E_PLACEMENT_GRAMMAR`,
   `MH_E_DUPLICATE_RANDOM_OPTION_INDEX`,
   `MH_E_PARTIAL_PUBLISH` и фиксированный
   `MH_E_UNREPRESENTABLE_TRANSFORM`; random JSON violations остаются
   `MH_E_COMPOSITE_GRAMMAR`.
7. Реальные дыры owner-контракта записать как `OPEN-V5-*` со STOP; кодов и
   goldens для нератифицированной семантики не изобретать.
8. Создать `docs/receipts/v5_s0.md` с base SHA, branch, changed paths,
   проверками и явным `AWAITING OWNER RATIFICATION`.

Acceptance:

- production paths `addon/`, `ue/`, `tools/`, `reference/` не изменены;
- 10/11/README/KICKOFF согласованы, 08/09 имеют v5 fate-table banners;
- все v5 composite fixtures начинаются первым полем `"v": 5`;
- fixture graph имеет все три options, closure expectation и parent-local probe;
- JSON parse/duplicate-filename/Markdown link checks зелёные;
- Python suite прогнан как regression, но не выдаётся за v5 implementation;
- внешний auditor получил крупный freeze-срез; owner merge остаётся gate.

---

## V5-S1 — Cross-host random reference и Dagor parity probe

> **ПОПРАВКА OWNER после приёмки (10 §13.8): seeds выводятся из пути.**
> Единого сквозного stream больше нет — каждый random-узел и каждый узел с
> `profile` открывает свой поток от `mix(placement_state, hash(NodePath))`.
> Срез принят и смержен, но подлежит retrofit'у: обнови reference, регенерируй
> golden-векторы, подними токен резолвера до `mh.random_resolver:2`. Тег
> `mh.random_stream:1` не меняется. Retrofit выполняется ОТДЕЛЬНОЙ веткой
> `v5/s1-1-path-derived-seeds` и не смешивается с V5-S2.

Gate: ОТКРЫТ. V5-S0 ратифицирован, `OPEN-V5-1`/`-2`/`-3` решены в 10 §§13.1–13.3
(baseline-битконтракт, применение профиля, байты подписи) — реализуй их дословно.
Единственное ожидание было `OPEN-V5-6` — исходные GAZ `*.composit.blk`.
Owner положил их в `reference/dagor_fixtures/gaz53/`, synthetic-токены заменены
реальными, GAZ-parity acceptance закрыт (§13.6).

Python, bpy-free:

1. Реализовать `mh.random_stream:1`: state/seed mapping, fixed draw,
   weighted selection без bias, profile sampling, DFS recursion, decision trace,
   SelectedDependencies и ResolvedSignature.
2. Один API возвращает immutable semantic plan/reference result; resolver не
   зависит от dict/set iteration.
3. Golden seed set:
   `0, 1, 2, 42, 123, 1024, 2147483647`. Зафиксировать raw draws, normalized
   samples, option indices, sampled transforms, NodePath, selected deps и
   signatures.
4. Golden'ами доказать draw-order:
   selection → offset X/Y/Z → rotation X/Y/Z → uniform → vertical → children;
   отсутствующий profile field draw не потребляет.
5. Source closure builder отдельно обходит все options/cycles и не принимает
   Seed.

Параллельный **Dagor Random Parity Probe**:

- минимальный fixture с тремя равновесными options и transform ranges;
- те же seeds;
- ожидаемые choices/transforms извлекаются из reference source/наблюдаемого
  Dagor поведения с provenance (файл/версия/команда), без копирования
  лицензированного кода;
- owner выбирает A — bit-for-bit Dagor RNG, либо B — behavioral compatibility с
  собственным RNG. Выбор записывается в 10/QUESTIONS и только затем открывает
  C++.

Acceptance: bpy-free pytest + golden verifier; повторный run byte-identical;
probe receipt отделяет измеренный Dagor факт от MH-норматива; owner decision
A/B смержен. Без него V5-S2 STOP.

---

## V5-S2 — Codecs v5 и C++ random parity

Gate: V5-S1 owner-accepted. `OPEN-V5-4` и `OPEN-V5-5` решены в 10 §§13.4–13.5:
профиль инлайнится в `UMHCompositeAsset` (ни UAsset, ни седьмого тега, ни
generated path; ребро индекса `composite→placement_profile: "profile"`),
predicate представимости — 8 ULP float32 по всем 16 элементам. Новые коды
среза: `MH_E_PLACEMENT_PROFILE_GRAMMAR`, `MH_E_UNREPRESENTABLE_TRANSFORM`
(+ golden counts).

Python и C++:

1. Parser/writer `.composite` v5: first `"v": 5`, closed grammar,
   random/options/empty, parent-local T/R/S, duplicate-key reject, canonical
   bytes.
2. Missing v → `MH_E_COMPOSITE_LEGACY_GENERATION` и обязательное сообщение;
   wrong version → `MH_E_UNKNOWN_SCHEMA_VERSION`. Dual-read отсутствует.
3. Parser/writer `.placement` v1 по ратифицированному binding/profile
   контракту.
4. C++ `mh.random_stream:1` бит-идентичен Python на всех общих vectors,
   включая traces/signatures.
5. Физически удалить v4 document-world parser/writer/compiler paths,
   `composite_v4_vectors.json` и world-transform tests/goldens; заменить v5.
   OPEN-V4-24 остаётся только historical docs.
6. Cycle/dependency traversal проходит все options. Source closure и resolved
   plan имеют разные APIs/types.
7. Зарегистрировать коды и обновить точные E/W counts.

Acceptance: Python/C++ canonical bytes и RNG traces byte-identical; v4 file
без `v` отвергается без fallback; parent 100 + local 25 = world 125; shear
negative vectors; полный Python gate и двойной UE unity gate.

---

## V5-S3 — Blender random authoring и Dagor import

Контракт свойств и конвертации Dagor→MH закреплён в 10 §6.4: typed
PropertyGroup `mh4blend` как authority, неавторитетное зеркало в ID custom
properties (`mh_composite_kind` / `mh_random_weight` /
`mh_random_option_index`), два обязательных источника конвертации (прямой
разбор `.composit.blk` — авторитетный; конвертация уже импортированной
dag4blend-сцены — рабочий), два признака распознавания random-узла, подъём
вариантов из helper-коллекции `TECH_STUFF` в дети узла. Разбор референсного
overlay-патча dag4blend — `docs/reference_notes/dag4blend_random_overlay.md`.

Gate: V5-S2 owner-accepted.

1. PropertyGroup `mh4blend`: random kind, typed option weight/index/resource.
   Resource collections остаются чистыми.
2. Options panel: Add/Remove/Up/Down/weight. Reorder меняет indices явно.
   Duplicate/missing invalid index fail-closed; option transforms display-only.
3. Строго четыре scenes: `COMPOSITE`, `MESH`,
   `ACTOR_PLACEHOLDERS`, `TECH`. Preview helpers не экспортируются.
4. Dagor `ent`/`weight:r` → typed options; implicit weight=1; `include` →
   typed placement profile. Непереносимое — lossless error с provenance.
5. Recursive import грузит все variants; modes structure-only/LOD0/full-LOD;
   explicit reuse/refresh definitions.
6. Blender UI/data/fixtures не содержат seed/InstanceSeed.

Acceptance: bpy field round-trip Dagor→Blender→MH→export; option display
transforms не меняют bytes; reorder меняет только option order/index;
unselected dependency загружается; grep seed/InstanceSeed по Blender production
пуст, кроме явных запретных тестов/доков.

---

## V5-S4 — Full closure export

Gate: V5-S3 owner-accepted. `OPEN-V5-11`/`-12`/`-13`, поднятые на STOP внутри
среза, решены в 10 §§13.9–13.11 и внесены в §6.5 — реализуй их дословно.

1. Реализовать три команды 10 §6.5.
2. Walker строит полное source closure через все random options; seed parameter
   отсутствует в API.
3. Full preflight: loaded authoring resources либо existing managed source;
   missing/unmanaged/ambiguous блокируют до staging. Зависимости, которые
   команда НЕ публикует, остаются в замыкании и проверяются наравне с
   публикуемыми (10 §13.11).
4. Staging всего замыкания, read-back каждого payload, затем
   placement profiles (первыми, 10 §13.2) → materials → meshes → leaf
   composites → parents → root LAST. Textures фазы не имеют вовсе
   (10 §13.10): только preflight-зависимость, копирования батчем нет.
5. Blender токенов не выпускает и watcher его событий не подавляет
   (10 §13.9); self-publish token остаётся UE-internal. После частичного
   replace честный `MH_E_PARTIAL_PUBLISH` с published/unpublished sets;
   rollback — VCS.
6. Receipt фиксирует crash/failure injection на каждом publish boundary.

Acceptance: root не заменяется до всех dependencies; невыбранные options
попадают в closure; existing unloaded source не переписывается; preflight error
не оставляет файлов; injected partial failure сообщает точный набор; отсутствие
существующего source у непубликуемой зависимости блокирует батч и называет
ResourceKey, ссылающийся композит и команду-исправление.

---

## V5-S5 — UE Editor random

Дополнительно (из разбора Dagor-редактора, протокол не затрагивают):

- классификация definition при импорте `None | ChildSeedsOnly | Transform |
  Topology` (аналог `seedAffectsResult`): смена Seed вызывает ровно нужный
  объём работы вместо полной пересборки дерева — существенно для сцен с
  сотнями размещений;
- на мультивыделении две команды вместо copy/paste: `Individual` (каждому
  актору свой новый seed) и `Equal` (один новый seed на всё выделение).

Gate: V5-S4 owner-accepted. `OPEN-V5-14` решён в 10 §13.12: thumbnail снят со
среза целиком, custom renderer остаётся отключённым, тест
`Mimir.V5.Composite.ThumbnailRenderingDisabled` не меняется.

1. `AMHCompositeActor`: int32 `Seed`, `bAutoSeed`, derived read-only
   `ResolvedSignature`. Auto create non-zero; manual 0 legal; transform actor не
   меняет seed; duplicate default new seed, Keep Seed explicit.
2. Один resolver строит `FMHResolvedCompositePlan`. Preview, Break и
   Show Choices/Trace используют только plan. Thumbnail потребителем plan не
   является (10 §13.12).
3. UI: Reseed / Randomize Selected / Copy/Paste/Lock Seed / Keep Seed /
   Show Resolved Choices / Show Decision Trace. InstanceSeed отсутствует.
4. Nested random и dependency-notify rebuild; source closure cook/find-broken
   не зависит от current seed.
5. UE compile применяет parent-local matrices и блокирует
   `MH_E_UNREPRESENTABLE_TRANSFORM` до component mutation.
6. Level Instance не резолвит.

Acceptance: одинаковые root+closure+seed → одинаковый trace/signature; seed 100
placements совпадают, seed 200 отличается на ratified golden; move actor не
меняет resolution; dependency change rebuilds; preview/Break parity; двойной
unity gate.

---

## V5-S6 — Runtime

Gate: V5-S5 owner-accepted.

1. `AMHRuntimeCompositeActor` в runtime module использует сериализуемый вход
   того же resolver/plan; editor-only services не попадают в packaged path.
2. Spawn materializes plan leaves; random draw в spawning запрещён.
3. Cook dependency admission использует source closure, runtime result —
   resolved plan.
4. Cross-host trace/signature report доступен Automation/PIE/packaged smoke.

Acceptance: Python reference = UE Automation = Editor preview = PIE = packaged
по choices, samples, world transforms, SelectedDependencies и
ResolvedSignature на seed set; двойной unity gate и packaged smoke.

---

## V5-S6.0 — Composite preview defects

Owner-approved минимальный срез перед S6.1. База — свежий `origin/main`,
ветка `v5/s6.0-preview-defects`, квитанция `docs/receipts/v5_s6_0.md`.
Не выделяется cherry-pick'ом из доказательной ветки
`codex/fix-ue-composite-preview`; та ветка сохраняется без reset/delete.

Scope определяется тремя красными baseline-пробами:

1. Rendering flag: видимость composite leaves при Game View (`G on`).
2. Организационная привязка leaves к существующим top-level authoring handles,
   с absolute T/R/S и мировыми матрицами плана, без нового resolver metadata.
3. Build preflight до создания source payload, папки, UObject package и UAsset.

Acceptance: `BuildPreflightRejectsBeforeMutation`, `TopLevelGrouping` и
`RenderedNativeHitProxy` становятся постоянными Automation-тестами;
квитанция содержит красный вывод неизменённого main и зелёный нового среза.
RHI-проба выполняется с настоящим viewport, не под `-nullrhi`.
Полный Automation и оба C++ build-гейта §9 обязательны.

Shared cache, idempotent assignment, nonblocking pending-mesh, watcher-import
scope и per-operation lookup сюда не входят; они вынесены в
[отдельный кандидат](proposals/shared_composite_preview_cache.md).
G-off selection после загрузки уровня и `PostLoad` остаются за S6.2;
полное глубокое semantic tree этим срезом не заявляется.
Дальнейшая последовательность уточнена owner-документами 13 R2 и 12 R3.

## V5-S6.1 — Прямой экспорт dag4blend

Gate: V5-S6.0 принят, PR #29 смержен. Ветка `v5/s6.1-dag4blend-bridge`,
квитанция `docs/receipts/v5_s6_1.md`. Полный scope и acceptance —
[документ 13, редакция 2](13_v5_s6_1_dag4blend_bridge.md).

Две формы сцены → единый Composite DTO → существующие writers и S4 publisher.
Прямой экспорт не материализует, не перепривязывает и не штампует исходные
датаблоки. Три команды выбирают объём файлов; полный source closure всегда
валидируется по всем options. Повторное использование проверяется против
Source Root, не по меткам сцены. Marker — явная поправка 10 §6.1; новые
носители не исполняют прижатие и не вводят Blender seed.

Обязательны failing baseline, нулевая мутация сцены, реальный GAZ end-to-end,
повтор без перезаписи, failure injection каждого replace, Python/Blender
счётчики отдельно, оба unity gate, Automation и packaged smoke.

## V5-S6.2 / V5-S6.3 — Только после приёмки S6.1

[Документ 12, редакция 3](12_v5_s6_2_s6_3_slices.md) задаёт следующие срезы:
placement lifecycle, затем pre-S7 freeze. Их реализация не входит в S6.1.
Owner отменил прижатие и обратный экспорт в Dagor; старые пункты о провайдере,
снапе и реестре gameObj не являются заданиями исполнителю.

---

## V5-S7 — Cook flattening

Gate: V5-S6 owner-accepted.

Build commandlet:

1. Для каждого placed `AMHCompositeActor` строит plan по его Seed.
2. Static leaves → ISM/HISM/StaticMeshActor по ратифицированной policy;
   gameplay leaves → самостоятельные actors.
3. Groups и nested composites растворяются; wrapper снимается. Никакого нового
   random draw при materialization.
4. World Partition/OFPA validation, stable actor naming/ownership и idempotent
   rerun; source closure обеспечивает cook dependencies всех options.
5. Failure до commit не оставляет half-flattened level/package.

Acceptance: flattened world визуально/семантически равен plan; traces/signatures
совпадают с V5-S6 до flatten; World Partition/OFPA и cook smoke зелёные; двойной
unity gate.

---

## V5-S8 — Level-Instance cache (PARKED)

Не входит в обязательную последовательность и ничего не блокирует. Level
Instance никогда не выбирает random. Возможный будущий backend может кэшировать
только уже разрешённый plan-вариант после отдельного owner ADR.

## V5-S9 — Generative packing pattern (PARKED)

Инструмент авторинга: художник выделяет композит (например «вещи из гаража» —
полка, инструменты, коробки), задаёт объём/поверхность размещения, жмёт кнопку и
получает согласованную укладку предметов. Кандидаты алгоритма — Voronoi/poisson
disk/сеточная упаковка с учётом bounds; выбор — отдельный ADR.

**Ключевое проектное ограничение (чтобы срез остался дешёвым): результат —
ОБЫЧНЫЕ узлы композита.** Инструмент работает в редакторе как расширение
Edit-режима: считает раскладку, пишет её в трансформы top-level узлов, художник
правит руками, затем штатный Publish Composite. Ни резолвер, ни runtime, ни cook
об упаковке НЕ знают, протокол не меняется. Алгоритм внутри резолвера (укладка
как функция seed) отвергнут заранее: он потянул бы packing в cook и runtime.

## V5-S10 — Composite physics settle (PARKED)

Инструмент авторинга: внутри Edit-режима композита все размещения временно
получают `simulate physics`, художник может толкнуть/уронить/приложить импульс,
чтобы убрать «искусственность» раскладки, затем жмёт Freeze — симуляция
останавливается, финальные трансформы записываются в узлы, дальше штатный
Publish.

Те же ограничения, что у V5-S9: результат — обычные узлы, симуляция не входит ни
в резолвер, ни в runtime, ни в cook; протокол не меняется. Нужны: временные
collision-прокси из BodySetup импортированных мешей, изоляция симуляции от
уровня, отмена без следов (Cancel = ребилд из ассета) и явный shear-контроль на
Freeze (симуляция даёт чистые T/R, но масштаб родителя может сделать результат
непредставимым — 10 §13.5).

### Референс: сторонний UE-инструмент Composition Maker (A17)

Owner передал сторонний инструмент (Unreal Python, ставится в проект патчем
`init_unreal.py`), который решает обе задачи. Изучен СТАТИЧЕСКИ, не
запускался. Полезные находки, проверенные практикой:

Упаковка (V5-S9) — никакого Voronoi, greedy по AABB:
- сортировка по объёму по убыванию с джиттером ±14 % — намеренно, иначе
  одинаковые дубликаты обрабатываются как монолит и штабелируются только
  друг на друга;
- самый крупный объект — корень в центре, Z-поворот из {0, 90, 180, 270};
- каждый следующий: с вероятностью ~0.38 сначала пробуется пол (иначе всё
  срастается в одну башню), иначе штабелирование, иначе пол;
- штабель: родитель должен быть заметно крупнее ребёнка (объём ребёнка ≤ 0.72
  родительского), проверки «годная опора» и «влезает», лимиты глубины (2) и
  детей на родителя (3);
- позиция на родителе выбирается `edge_magnet_score` — тянет к КРАЮ/углу
  родителя и в сторону ближайшего соседнего кластера, плюс слабое притяжение
  к центру композиции;
- пол: полярный поиск вокруг корня — 120 углов по 3° в перемешанном порядке,
  радиус растёт кольцами по 5 см до 380 см, кандидат отвергается по
  пересечению AABB, счёт = расстояние до ближайшего соседа ×1.8 (прижаться к
  кластеру) + 0.12 × дистанция от центра + случайная добавка до 9.0 (вариации
  при повторном нажатии);
- роли задаются тегами объектов (`Place_Any`/`Place_Surface`/`Place_Floor`/
  `Copy_N`/`MaxStack_N`) — художник размечает меши, а не алгоритм угадывает.

Ограничения референса, которые мы НЕ наследуем: работа только по AABB и
Z-поворотам, кратным 90° (для ящиков годится, для нерегулярных форм даёт
заметные зазоры); генератор не сеян (`RANDOM_SEED = None`, каждый запуск
другой) — у нас укладка обязана логировать использованный seed, чтобы
результат можно было воспроизвести и предъявить.

Физика (V5-S10) — три реальные проблемы UE, решённые там и обязательные к
учёту у нас:
1. **Актор редактора недоступен на запись, пока идёт PIE.** Трансформы
   снимаются из PIE-мира ПОКА ОН ЖИВ, складываются во внешний файл, и
   применяются позже — по `register_slate_post_tick_callback`, дождавшись
   фактической остановки PIE, внутри `ScopedEditorTransaction`.
2. **Соответствие «актор редактора ↔ актор PIE».** Там оно решается тегом
   `physics_id` и стопкой эвристик сопоставления по имени/метке — их слабое
   место. У нас соответствие БЕСПЛАТНОЕ и точное: лист плана имеет
   канонический `NodePath` (10 §13.3) — им и метим прокси.
3. **Меш без simple collision не симулируется.** Нужен guard: проверка
   BodySetup, отказ либо временная простая коллизия. КРИТИЧНО: у нас
   временная коллизия допустима ТОЛЬКО на прокси; трогать managed
   `UStaticMesh` запрещено (source побеждает, 10 §9).

Ещё одно архитектурное отличие от референса: он работает с акторами уровня и
в уровень же пишет результат. У нас дерево компонентов derived/transient, а
истина — файл. Поэтому симуляция идёт на ВРЕМЕННЫХ прокси-акторах,
порождённых из resolved plan (по одному на лист), а результат ложится в
трансформы УЗЛОВ и уходит штатным Publish.

Оба среза — authoring-расширения Edit-режима §6.1, поэтому не блокируют
V5-S0…S7 и не требуют изменения формата.

Backlog `mh_asset_io`/`ufbx` остаётся отдельным по
`ADR_V4_mh_asset_io.md` и не смешивается с V5-S0…S7.

## Сводное end-to-end acceptance v5

1. Missing `"v"`: legacy-generation error, no dual-read.
2. Random option order/weights canonical и lossless; zero-weight не выбирается.
3. Cycle в любом невыбранном option блокирует source closure.
4. Parent 100 + local 25 = world 125; parent motion moves child.
5. Shear reject на Dagor import, Blender export и UE compile.
6. Source closure включает все options; resolved plan — только selected deps.
7. Draw-order и absent-parameter no-draw совпадают Python/C++.
8. Seed set имеет frozen choices/traces/signatures cross-host.
9. Seed lives only on UE placement; Blender seed surface отсутствует.
10. Two seed-100 placements equal; seed-200 differs; actor movement stable.
11. Editor preview = Break = PIE = packaged = cook plan; thumbnail исключён.
12. Include-All batch stages closure and publishes root last.
13. Runtime actor precedes cook flattening; Level Instance does not resolve.
14. Engine and `reference/` unchanged; each C++ slice passes both unity gates.
