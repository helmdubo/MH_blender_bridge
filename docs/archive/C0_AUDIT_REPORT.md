> HISTORY. Не норматив. Модель заменена docs/16_recipe_model.md (2026-09-02).

# C0 — отчёт-квитанция внешнему аудитору

Статус: **готово к внешнему аудиту, C1 не начат**. Ветка `codex/ue-c0`
перебазирована на `64314d5` и содержит только plugin/repository changes. Fork или
правки исходников/бинарников Engine не выполнялись. Test-host
`MimirHead_portfolio 5.7` использован для загрузки собранного плагина и
headless-прогонов; его `.uproject` не изменялся, поэтому команды явно передают
`-EnablePlugins=MimirComposite`.

## Что подтверждено

- Каркас `MimirCompositeRuntime` / `MimirCompositeEditor` /
  `MimirCompositeTests` собирается stock UE 5.7.4 без engine patch и без
  deprecated-in-5.7 API в собственных модулях.
- Строгий `RunUAT BuildPlugin -StrictIncludes` зелёный для Win64:
  UnrealEditor Development, UnrealGame Development, UnrealGame Shipping.
- `Mimir.C0.CanonicalVectors` зелёный: все 39 frozen vectors без изменения
  `golden/canonical_vectors.json`; отдельно сохраняется C++ contract-таблица
  path semantics §5.3. Добавлена fail-closed regression-проверка границы
  `int64`: `2^63` запрещён, `-2^63` допустим.
- `mh.fbxdump` читает raw FBX SDK scene без `ConvertScene`, transform evaluation
  и geometry mutation. Тег `mh.fbxdump:1` содержит per-node
  `mh_lod_level` и сводку Combined-LOD. Summary и `--full` дампы
  `axis_probe.fbx` закоммичены с обязательным LF и совпадают byte-for-byte.
- Настоящий commandlet найден через UCLASS и завершает оба smoke-вызова кодом
  `0`: summary и `--full`.
- R1 через прямой prototype `FMHFbxBackend` и legacy `UFbxFactory` зелёный.
  Для обоих путей зафиксированы: raw FBX `9` control points / `7` polygons;
  pre-build MeshDescription `9` vertices / `13` polygons; post-build render
  `9` unique vertices / `13` triangles. Контрольная локальная позиция
  `(37, -11, 193)` см и мировая `(223.3146, 219.4280, 242.7456)` см проходят
  допуск `0.1` см.

Финальный объединённый headless receipt после rebase:

```text
Mimir.C0.CanonicalVectors          Success
Mimir.C0.FbxDump.AxisProbe        Success
Mimir.C0.R1.AxisProbeBackends     Success
succeeded=3, failed=0, notRun=0
```

## Что разошлось и как разобрано

- Расхождений с frozen canonical vectors, `RISK_RESULTS` или ожидаемыми
  fbxdump-байтами нет.
- Первый strict-includes прогон нашёл транзитивный include `FPaths` в canonical
  automation; добавлен явный `Misc/Paths.h`, повторная строгая матрица зелёная.
- Первый canonical headless прогон выявил, что `FJsonObject` схлопывает
  case-distinct keys `A`/`a`; добавлен pair-preserving canonical object API,
  golden не менялся.
- Независимый static audit выявил недопустимую верхнюю границу q6 из-за
  округления `double(MAX_int64)` до `2^63`. Граница исправлена в единственной
  canonical-функции, `fbxdump` делегирует ей; regression и повторный headless
  прогон зелёные.

## Открытые вопросы и handoff

- `UE-QUESTION-13`: точное имя FBX carrier property паспорта ещё не ратифицировано.
  C0 перечисляет все `mh_*`, диагностически распознаёт provisional
  `mh_fbx_passport` и JSON schema `mh.fbx_passport`, но не даёт имени mapper
  authority.
- `UE-QUESTION-14`: требуется решение, означает ли «полные массивы» §7.1 уже в
  теге 1 topology-only C0 форму или также mapper-facing normals/smoothing/UV/
  colors. C2 до ответа не начинается.
- Переданный отдельно Clean Sources v2 contract на момент отчёта отсутствует в
  `docs/` ветки `64314d5`. По полученной директиве C0/C1 продолжаются на текущем
  v1 потоке; позднее переключение ограничено реализациями за
  `IMHSourceResolver` и `IMHChangeDetector`. Repository authority ожидается
  отдельным docs-коммитом владельца.

## Checklist gate C0 (§13)

- [x] Каркас UE 5.7.4 Runtime / Editor / Tests.
- [x] Canonical library проходит frozen vectors.
- [x] `mh.fbxdump` и два expected dump golden-фикстуры.
- [x] R1 automation через оба backend'а с topology counts до/после build.
- [x] Headless automation и настоящий commandlet smoke-run.
- [ ] Внешний аудит и явная приёмка C0.

До последнего пункта следующий gate не начинается.
