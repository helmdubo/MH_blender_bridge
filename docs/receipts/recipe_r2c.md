# R2c — точки выхода proof-плоскости

Статус: **STOP — OPEN-R2C-2**. `OPEN-R2C-1` закрыт близнецом: два обязательных
red-теста и наблюдаемый API счётчика save-аудита добавлены в ветку. Собственный
red-first воспроизведён. До production-правок найден существующий тест, который
запрещает требуемое контрактом расширение диагностического реестра.

## 1. База и host

- ветка: `recipe/r2c-exit-points`; база `origin/main` `ec1d5ff`; исходный
  red-коммит `94938d7`; red-тесты ответа близнеца `4b38f7a`; контрактный HEAD
  `7648e72`;
- отдельный worktree:
  `E:\GITHUB\Mimirhead_UE5Exporter\MH_blender_bridge_r2c_executor`;
- собственный module-free host:
  `E:\MimirComposite_R2C_External_20260903\HostProject.uproject`; stock UE
  5.7.4: `D:\PersonalProjects\UE5\UE_5.7`; plugin подключён junction к
  R2c-worktree;
- трекер подтверждает `R2c | NEXT`; основной checkout `main`, Engine,
  audit-host и portfolio-проект не изменялись; `main` не пушился.

## 2. Red-first

Нетронутый HEAD `7648e72` сначала собран обязательной командой
non-unity/no-PCH (`-DisableUnity -NoPCH -NoSharedPCH -WarningsAsErrors`):

```text
E:\MimirComposite_R2C_External_20260903\Saved\Logs\R2C_RED_BUILD_NONUNITY.log
2098: Result: Succeeded
```

Первым тестовым действием после сборки был фильтр
`Automation RunTests Mimir.V5.Composite.Proof`; лог
`E:\MimirComposite_R2C_External_20260903\Saved\Logs\R2C_RED_TEST.log`:

```text
1087: Test Completed. Result={Fail} Name={BuildPreflightFullClosure}
1089: Expected 'request schedules a deferred proof' to be true.
1092: Expected 'stale proof is Stale: MH_E_NOT_IMPLEMENTED: ...' to be true.
1105: Test Completed. Result={Fail} Name={SaveWarnsWithoutProof}
1107: Expected 'save schedules the missing proof' to be true.
1108: Expected 'save warned about the unproven placement' to be true.
1109: Expected 'deferred proof became Fresh' to be true.
1116: Test Completed. Result={Fail} Name={StaleSourceBlocksCookAndSnapshot}
1118: Expected 'stale source refuses cook preflight' to be false.
1120: Expected 'stale source refuses snapshot admission' to be false.
1122: Expected 'cache reports Stale' to be true.
```

Итог red-фильтра: 3 completed, 0 Success, 3 Fail; падения соответствуют
заглушке и ожидаемому red из ответа контракта.

## 3. Закрытые и открытые вопросы

### OPEN-R2C-1 — закрыт близнецом

Закрыт 2026-09-03 коммитами `4b38f7a` и `7648e72`: тесты
`Proof.SaveWarnsWithoutProof` и `Proof.StaleSourceBlocksCookAndSnapshot` пишет
близнец, исполнитель их не меняет. В API добавлен только наблюдаемый
`GetLastSaveAuditWarningCount()`.

### OPEN-R2C-2 — диагностический реестр против неизменяемого теста

- **Контекст:** задача R2c требует зарегистрировать `MH_E_STALE_SOURCE` и
  семейство `MH_W_PROOF_*` в `MHDiagnosticRegistry`. Существующий тест
  `Mimir.V5.Random.CrossHostGoldenVectors` в неизменяемом
  `MimirCompositeTests/Private/MHRandomStreamV5Test.cpp:241,251` требует точные
  размеры реестров: `MHRegisteredErrorCodes().Num() == 53` и
  `MHRegisteredWarningCodes().Num() == 16`. Сейчас это фактические размеры.
  Добавление хотя бы одного обязательного кода нарушает Acceptance 4–5. Этот
  тестовый файл отсутствует в закрытом списке, а контракт прямо говорит
  «Тесты не менять».
- **Вопрос:** близнец добавляет red-коммит, который обновляет точные количества
  и проверяет новые коды в `MHRandomStreamV5Test.cpp`, после чего исполнитель
  продолжает без правок тестов, либо нормативно снимает требование регистрации
  новых кодов?
- **Временное fail-closed правило:** новые диагностики не маскировать старыми
  кодами и не оставлять незарегистрированными; тесты, API и production-код не
  менять; green/full/build-гейты не начинать; трекер оставить `NEXT`; PR не
  открывать до непротиворечивого ответа.
- **Статус:** **OPEN — нужен red-коммит/ответ близнеца.**

## 4. Гейты

| Gate | Результат |
|---|---|
| non-unity/no-PCH, RED HEAD | PASS — `R2C_RED_BUILD_NONUNITY.log:2098`, `Result: Succeeded` |
| три `Mimir.V5.Composite.Proof.*`, RED | ожидаемый FAIL — `R2C_RED_TEST.log:1087,1105,1116` |
| Acceptance 1–5 | NOT RUN — STOP до регистрации диагностик без поломки существующего теста |
| `RecipeShadowParityTest` | NOT RUN — STOP |
| полный NullRHI suite | NOT RUN — STOP |
| force-unity | NOT RUN — STOP |
| `BuildPlugin -StrictIncludes` | NOT RUN — STOP |
| `git diff --check` | PASS для STOP-квитанции |
| `python tools/check_normative_docs.py` | PASS для STOP-квитанции |

## 5. Изменённые файлы

- `docs/receipts/recipe_r2c.md` — эта STOP-квитанция, разрешённая контрактом.

Production-код, публичный API, red-тесты и существующие тесты не менялись.
Удалённых тестов нет.

## 6. Строка трекера и PR

Трекер не изменён: `R2c | NEXT`. PR не открыт по временному fail-closed правилу
OPEN-R2C-2.
