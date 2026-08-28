# UE composite preview / placement regression fix

Дата: 2026-08-28. База: `7b46df5`, ветка `codex/fix-ue-composite-preview`.
Scope: UE plugin и его тесты. Blender, source payloads, `reference/`, `golden/`
и Engine source не изменяются. Merge выполняет owner.

## Publication / post-review status (2026-08-28)

Коммиты `21624a7` и `6aeac4b` опубликованы в
`origin/codex/fix-ue-composite-preview`; проверенный production tip — `6aeac4b`.
Последующая публикация отчёта меняет только документацию, не этот code snapshot.
Оба остаются вне `main` (`7b46df5`). **MERGE BLOCKED** до owner-решения об
объёме: shared definition cache нельзя считать автоматически принятым вместе
с исправлениями превью. Исторические зелёные gates ниже не закрывают это решение
и не являются red/green проверкой исходного main.

Первоначальная квитанция была написана до commit/push; прежняя формулировка
о локальной ветке больше не описывает её состояние. Детальная повторная
проверка, baseline-эксперимент и полный `git log --stat` —
`ue_composite_preview_review_response.md` в этом каталоге.

## Причины и исправления

1. `AMHCompositeActor::IsEditorOnly()` подавлял primitive scene proxies в
   Game View, но не их тени. Это реальный дефект, однако он **не объясняет
   сам по себе** сообщение owner о невидимости при G off. Теперь source actor
   исключается из cooked client/server load через `NeedsLoadForClient/Server`,
   а PIE/cook по-прежнему проходят существующий runtime handoff. Стандартные
   StaticMeshComponent принадлежат composite actor, получают visibility и
   native selection flags до регистрации. Кастомного raycast нет.
2. Семантическая иерархия восстанавливается из derived `ParentNodeIndex` /
   `NodeIndex` плана. Полные матрицы и абсолютные transforms сохраняют admission
   parent-local продукта; наивное перемножение декомпозированных FTransform не
   вводится. RNG, resolver tag, signature preimage и goldens не меняются.
3. UE factory вызывает assignment при PlaceAsset и PostPlaceAsset. Повторный
   assignment того же ассета с актуальным планом либо pending compile теперь
   idempotent. Компонент полностью настраивается до первого RegisterComponent.
   Поиск предыдущих компонентов и удаление retired компонентов используют
   map/set, а не вложенные линейные обходы.
4. Успешный seed-free applied graph разделяется между placements в памяти.
   Asset Registry claim events, applied-object edits, settings и notifications
   инвалидируют cache; неправильный class/path claim не отфильтровывается.
   Lease = max(global epoch, epochs всех placement dependencies). Canonical
   carrier edits инвалидируют свой key; изменения самих MH receipts, divergent
   identity, неизвестные/неканоничные carriers и AR claims — global. Проверка
   compiling mesh не читает его заблокированный AssetImportData. Read-only
   getter не выдаёт stale plan; Break/Edit требуют явного Rebuild после такой
   инвалидизации и сами не мутируют компоненты при отказе preflight.
   Одна операция использует один lazy snapshot generated claims, а не запрос
   всего registry для каждого leaf. Cold admission по-прежнему проверяет весь
   source closure, включая невыбранные варианты; cook admission не ослаблен.

   Этот пункт описывает исходный `21624a7`. Его отказ обновлять actor после
   инвалидации исправлен в `6aeac4b` (отдельная квитанция
   `ue_composite_break_freshness_fix.md`): refresh выполняется автоматически
   вне Break. Ограничение scheduling при feedback проверяется отдельно в
   post-review response; обязательный ручной Rebuild больше не является
   заявленным поведением исправления.
5. Interactive placement не вызывает FinishCompilation: pending mesh оставляет
   старые компоненты нетронутыми, либо показывает диагностический placeholder.
   Asset post-compile callback ставит retry на следующий editor tick. Проверка
   IsCompiling есть также на compiler endpoint после cache hit, до мутаций.
   Cook/runtime input admission сохраняет отдельный blocking режим. Нет второго
   резолвера и нет draw в materializer.
6. Watcher import scope — old/new changed keys, reverse dependents и их forward
   closure. Явный пустой scope не означает All. Несвязанные NO_CHANGE assets не
   загружаются и не заполняют отчёт; неизвестные/global integrity diagnostics
   сохраняются. Startup/manual scan остаются full-scope. SQLite/schema прежние.
7. Build Composite проверяет source dependencies и existing applied graph до
   CreatePackage/publish. Missing/unmanaged/ambiguous dependencies больше не
   создают новый source/UAsset до отказа. **Остаточная стоимость:** успешный Build
   сохраняет два fresh whole-root сканирования (preflight и import после publish).
   Этот fix не заявляет устранения всего filesystem I/O или замера ускорения на
   owner-сцене.
8. UE FBX translator теперь нормализует подтверждённый X-forward transport в
   рабочую MayaZUp-систему FBX SDK перед обходом сцены; сохранён существующий
   переход handedness. Geometry, normals, sockets и collision проходят один
   scene conversion. Blender exporter не менялся. StaticMesh ImporterVersion
   `2 -> 3`: существующим managed meshes нужен однократный reimport из их source.

Thumbnail renderer не возвращается. Реальные ошибки отсутствующих/невалидных
source ресурсов не скрываются и не исправляются неявным переименованием.

## Независимая проверка реализации

- Делегированы отдельные bounded задачи: Build/watcher/registry latency;
  UE FBX transport и его parity tests; read-only component/cache lifecycle audit.
- Аудит нашёл cache-hit pending mesh и stale actor-local graph на seed/basis
  fast path, затем stale getter и foreign receipt claim invalidation. Для
  каждого добавлен guard и regression coverage.
- В начальном full run три regression tests потребовали исправления: старое
  ожидание плоского числа узлов, label child actor до RegisterComponent и тест,
  который не завершал editor edit через PostEditChange. Эти промежуточные
  результаты не выдаются за acceptance.
- Промежуточный вариант global-only lease + auto-refresh в Break провалил
  шесть существующих tests: лишняя инвалидизация соседних placements и мутация
  при отказавшем Break. Auto-refresh удалён, lease стал dependency-keyed;
  существующие no-mutation assertions не ослаблялись. После этого full suite
  снова 111/111; финальный release run дополнительно покрывает receipt claims.

## Gates

Host: stock UE 5.7.4 CL 51494982; VS 2022 14.44.35222, SDK 10.0.22621.0.
Изолированный regression host: `E:/MimirComposite_UEFix_GateHost_20260828`.
Изолированный cook host: `E:/MimirComposite_UEFix_CookHost_20260828`.
Owner project не открывался для тестов и не менялся.

- Force-unity + DisableAdaptiveUnity + NoPCH/NoSharedPCH: PASS.
  `force-unity-final.log`: 16 actions / 52.12 s; последующие source deltas
  также собраны в этом режиме, финальный `force-unity-accepted.log`: 4 actions /
  37.31 s. PCH-вариант был прерван исполнителем при переходе к audit fix;
  он не считается завершённым gate. Engine source не менялся.
- Game build: PASS, `game-build.log`, 13 actions / 30.41 s, NoPCH/NoSharedPCH,
  NoEngineChanges. Начальный PCH game build прерван исполнителем и заменён этим
  запуском. Cook/stage/pak/archive выполнены после успешного отдельного build,
  UAT exit 0, 18.03 s (`build-cook-stage.log`), без IgnoreCookErrors.
- Packaged smoke: PASS, exit 0, `packaged-smoke.log`. Семь реальных cooked
  runtime placements, Game world, editor/test/SQLite modules не загружены.
- Negative cook: PASS как ожидаемый отказ, прямой Cook commandlet exit 1,
  `negative-cook.log`: `static_mesh:variant_b1_mesh has no matching managed mesh
  receipt`. Убран только изолированный fixture-файл, не выбранный seeds
  0/1/2/42; остальные три placements его выбирают. Failed output отдельно в
  `E:/MimirComposite_UEFix_NegativeCook_20260828`, не упаковывался. Файл возвращён
  в finally, SHA256 до/после совпал:
  `94a2163bdbc6f64e4b5ea151e85fad115f99d0ce15166f9f2fcd5a034e2089b8`.

### Пять отдельных parity lanes

Во всех пяти — seed set `{0, 1, 2, 42, 123, 1024, 2147483647}`. Сверены choices,
samples, frozen WorldTrs, полные world matrices, materialized matrices по 8-ULP,
SelectedDependencies, closure и ResolvedSignature. `tools/s6_runtime_parity.py
verify`: 7/7 в каждом lane. Речь о frozen S1.1 fixture и stock-cube geometry
тестового host, не о визуальном acceptance owner-сцены.

| Lane | Процесс / отчёт | Результат |
|---|---|---|
| Python reference | GateHost `Saved/Mimir/S6/python.json` | 7/7 |
| UE Automation | GateHost `Saved/Mimir/S6/automation.json` | 7/7 |
| Editor preview | GateHost `Saved/Mimir/S6/editor_preview.json` | 7/7 |
| Real PIE | CookHost `Saved/Mimir/S6/pie.json`, `pie-release.log` | 7/7 |
| Packaged Game | Packaged/Windows/MimirCompositeV5S6 `Saved/Mimir/S6/packaged.json` | 7/7 |

Python и UE здесь прогонял исполнитель; это не независимое воспроизведение UE
внешним ревьювером. Финальный binary-host replay перечисляется ниже отдельно.

### Финальный бинарный пакет

- `BuildPlugin -StrictIncludes -NoPCH -NoSharedPCH -DisableUnity`: PASS,
  Editor 90 actions, Game Development 10, Game Shipping 10, UAT exit 0 / 4m52s.
  `strict-verified.log`, `strict-editor-verified.log` в GateHost.
- Output: `E:/MimirComposite_UEFix_Verified_20260828`. Все **130 source files**
  сверяются с рабочей веткой SHA256. UAT штатно меняет descriptor EngineVersion /
  Installed; module types и SQLite Editor-only allowlist сохраняются.
- Отдельный **VerifiedHost** (`E:/MimirComposite_UEFix_VerifiedHost_20260828`)
  ссылается на этот package, не на repository plugin. `Reports/full/index.json`:
  **111/111** (84 Success, 27 SuccessWithWarnings, 0 Fail), `full.log`.
  Headless RenderedNativeHitProxy guard сам по себе не считается RHI-проверкой.
- `verify` повторён на Automation / Editor preview / real PIE отчётах именно
  VerifiedHost, с теми же независимыми Python и packaged отчётами: снова 7/7
  в каждом из пяти lanes.
- RHI на repository binary: **3/3**, `Reports/rhi-accepted`. Итоговый replay
  на **Verified package DLL: 3/3**, `VerifiedHost/Reports/rhi/index.json`,
  `rhi.log`: четыре native HActor passes (G off/on до и после rebuild/move/seed),
  viewport 615x339, зарегистрированный render proxy visible/selectable.
- Обычный editor shutdown: **PASS**, exit 0, `normal-editor-exit.log`, команда
  `QUIT_EDITOR` (не принудительный TestExit), завершение D3D12/модулей и
  `LogExit: Exiting`. Неподходящая первая команда `Quit` не закрыла editor;
  этот отдельный тестовый процесс остановлен исполнителем и не считается gate.

Последняя test-only правка отделила carrier PropertyChanged от Modify, который
дополнительно штампует receipt через существующий dirty hook. Промежуточный
package с тестовой DLL до этой правки не принят. Итоговый Verified package
собран заново с неизменяемым source snapshot; его тесты запускались отдельно.

ZIP: `E:/MimirComposite_UEFix_Verified_20260828.zip`, 44,857,928 bytes.
SHA256: `405f98eca761fed13478268f2b69d2829b3325ae5dcaf1cf33be0ca55aa7527b`.
Все 212 файлов ZIP прочитаны обратно и SHA256-сверены с Verified package;
130 source files package совпадают с рабочей веткой. Промежуточные сборки
с отклонёнными/устаревшими тестами не являются артефактами передачи.

### Pure Python (отдельно от Blender)

Существующий dev venv `E:/MimirComposite_V5S6_Python_20260827`,
`pure-python.xml`: **229 passed / 2 skipped**, 7.32 s JUnit time.
Это не новая установка зависимостей на чистом хосте.

| Модуль | Passed | Skipped |
|---|---:|---:|
| test_batch_publish | 13 | 0 |
| test_canonical | 23 | 0 |
| test_composites | 15 | 0 |
| test_dagor_composites | 20 | 0 |
| test_dagor_random_parity_probe | 7 | 0 |
| test_materials | 31 | 0 |
| test_mesh_nodes | 23 | 0 |
| test_no_blender_seed_surface | 1 | 0 |
| test_payload_publish_v2 | 10 | 0 |
| test_pending_v4_surfaces | 1 | 0 |
| test_placement_publication | 6 | 0 |
| test_placements | 4 | 0 |
| test_project_textures | 17 | 0 |
| test_random_reference | 22 | 0 |
| test_runtime_parity | 9 | 0 |
| test_s4_ledger_purge | 2 | 0 |
| test_source_closure | 10 | 0 |
| test_source_inventory | 7 | 2 |
| test_transforms | 8 | 0 |

Оба skip — ограничения создания symlink на Windows; на Linux ожидаются два
дополнительных passed. Blender-hosted suites **не запускались** в этом UE-only
исправлении; исторические числа предыдущих срезов здесь не переиспользуются.

## Граница подтверждения

Изолированный RHI test проверяет настоящий viewport hit proxy `HActor`, а не
только значение bSelectable: G off/on, затем rebuild/move/seed и снова G off/on.
Отдельно проверяются IsShown/IsSelectable у зарегистрированного render proxy.
Это не полевая проверка конкретной owner-сцены и не обещание её точного FPS.

## Передача и установка

Закрыть UE, сохранить прежнюю папку `Plugins/MimirComposite` как backup вне
сканируемой папки Plugins, затем распаковать ZIP в новую `Plugins/MimirComposite`.
Не переносить старый Intermediate поверх пакета. Сборка предназначена для
того же stock UE 5.7.4 CL 51494982; из Source можно выполнить собственный rebuild.
Для FBX axis correction нужен однократный reimport managed meshes (ImporterVersion 3).

Установленный plugin и owner UAssets исполнитель **не перезаписывал**, проект
автоматически не открывал. Commit/push выполнены позже по явному запросу owner:
`21624a7` и `6aeac4b` находятся на удалённой ветке. Merge не выполнялся.
Для code review использовать эту ветку и receipt,
а для полевой проверки — только Verified ZIP, не промежуточные build directories.

Повторный fetch: `origin/main = 7b46df5`. `git diff --check` проходит;
`addon/`, `reference/`, `golden/` вне diff; 16 files в `golden/v5` и
`reference/dagor_fixtures` LF-clean. UAT-generated пустой FilterPlugin.ini
удалён из checkout и не является изменением plugin configuration.
