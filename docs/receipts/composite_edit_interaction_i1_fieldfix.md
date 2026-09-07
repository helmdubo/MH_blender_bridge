# CE-I1 — исправления после полевой проверки

Статус: READY FOR OWNER RETEST, 2026-09-07. Заменяет отклонённый полем первый
кандидат из `composite_edit_interaction_i1.md`; не является полевой приёмкой.
Ветка `codex/ce-interaction-nodes`, draft PR #159.

## Поведение

- Вход в Edit с уже включённым Game View сохраняет видимость временных SMC,
  положение камеры и пользовательские show flags. `HiddenInGame` и
  `bIsEditorOnlyActor` больше не используются для исключения проекции из мира
  игры: actor/components имеют `RF_Transient | RF_DuplicateTransient`.
- Composite Outliner сохраняет корень открытой сессии при пустом выборе и
  при выборе компонентов временного актора. Без выбранного NodeId служебный
  актор не выбирается: лишнее гизмо в нуле не появляется.
- Save в CE-сессии публикует без повторного overwrite prompt. Ошибка
  публикации сохраняет сессию и существующую диагностику результата записи.
  Настройка подтверждения остальных source-overwrite операций не меняется.
- Cancel и один Esc отменяют всю сессию без dirty prompt. Активный жест
  сначала откатывается; его Undo-запись закрывается. При viewport key dispatch
  жест останавливается сразу, teardown режима выполняется на следующем tick.
- Edit Contents выбранного Composite-варианта Random принимает канонический
  путь `.../options[N]` и использует transform владельца Random. Невыбранный
  вариант не становится редактируемым occurrence.

## Проверки на собственном host

UE 5.7.4 CL 51494982, BuildId 47537391. Собственный проект:
`E:\temp\MH_CEI1_host_20260907\MimirCompositeV5S6.uproject`.
Данные пользовательской сцены не используются как изменяемая тестовая фикстура.

| Проверка | Результат / свидетельство |
|---|---|
| RED: Game View | `FIELD_RED_RHI.log`: actor/component hidden, `FPrimitiveSceneProxy::IsShown` и raster hit proxy обоих листьев не проходят |
| RED: пустой выбор | `FIELD_RED_RHI.log`: при пустом NodeId выбран служебный актор |
| RED: Cancel | `FIELD_RED_RHI.log`: ответ обработчика «Нет» удерживает dirty-сессию |
| RED: Save / Esc | `FIELD_RED2_RHI.log`: Save отклонён обработчиком, Esc во время жеста оставляет сессию и компоненты выбранными |
| RED: Outliner | `FIELD_RED2_RHI.log`: реальный Slate widget теряет дерево при пустом/native projection selection; первая попытка теста с dock tab была ошибкой harness и не учитывается |
| RED: выбранный Composite option | `FIELD_RED2_RHI.log`: вложенный полный путь через Random отклоняется с `MH_E_COMPOSITE_GRAMMAR` |
| GREEN: полный NullRHI | `FIELD_FULL.log`, `FieldFullReport/index.json`: 282 passed, 0 failed, включая обе RecipeShadowParity и выбранный вложенный Composite option |
| Guarded non-unity/no-PCH | `FIELD_GREEN3_BUILD.log`: Succeeded |
| StrictIncludes host | Повторная сборка сохранённого BuildPlugin host с `-DisableUnity -NoPCH -NoSharedPCH -NoEngineChanges`, `FIELD_STRICT_FINAL.log`: Succeeded |
| Force Unity, adaptive off | `FIELD_FORCE_UNITY.log`: Succeeded |
| Финальный D3D12 RHI | `FIELD_FINAL_RHI.log`, `FieldFinalRHIReport/index.json`: 50 passed, 0 failed, RTX 3070; в том числе viewport Escape next-tick, Game View до Enter, реальные hit proxies и выбранный Composite option |
| Документы / diff | `python tools/check_normative_docs.py`: OK; `git diff --check`: clean |

Финальный RHI выполнен на DLL сохранённого strict host, которые идут в пакет.
Для запуска использован дополнительный descriptor собственного host
`E:\temp\MH_CEI1_package_20260907\HostProject\MimirCompositeV5S6.uproject`
(тот же plugin, без модулей portfolio-проекта). Исходники пакета сверяются с
checkout, DLL и исходники записываются в SHA-256 manifest, ZIP проверяется CRC.
Пакет: `E:\temp\MH_CEI1_fieldfix_20260907\MimirComposite_CE-I1_FieldFix_UE5.7.4_Win64.zip`;
контрольная сумма — рядом в `SHA256.txt`.

Промежуточный `FIELD_GREEN_RHI.log`: 49/50; все UI/visibility/Cancel/Save/Esc
проверки прошли, остался выбранный Composite option. Причина исправлена:
компоненты compiled recipe имеют локальные пути определения, а plan — полные
пути occurrence. Поиск выполняется в рецепте определения после последнего `>`.
Проверяются и сам option, и более глубокая ссылка после него.

## Полевая проверка нового пакета

1. До Edit включить Game View (G); открыть тот же подкомпозит ворот.
   Геометрия остаётся на месте, камера не перемещается, дерево доступно.
2. Клик по мешу выбирает его авторский узел; вложенная ссылка выделяется и
   перемещается целиком. Гизмо находится у выбранного узла.
3. Изменить transform, Cancel: режим закрывается сразу и исходный вид возвращается.
4. Повторить с Esc, включая Esc во время перетаскивания: достаточно одного нажатия.
5. Изменить transform, Save: без подтверждения сохраняется определение,
   режим закрывается; повторный Edit показывает сохранённый transform.
6. Открыть выбранный Composite option внутри Random ворот; проверить его
   узлы и более глубокое вложение, если оно есть.

Качество мыши, подсветки и snapping требует повторной оценки owner в сцене.
Автоматика с реальным RHI проверяет видимость/кликабельность и сохранение
камеры; NullRHI не заменяет визуальную приёмку.
