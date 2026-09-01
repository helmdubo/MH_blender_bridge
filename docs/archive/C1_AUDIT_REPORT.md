> HISTORY. Не норматив. Модель заменена docs/16_recipe_model.md (2026-09-02).

# C1 — отчёт-квитанция внешнему аудитору

Статус: **IMPLEMENTATION CANDIDATE; ВНЕШНЯЯ ПРИЁМКА НЕ ПОЛУЧЕНА**.

Дата локальной проверки: 2026-08-23. Host: stock UE 5.7.4,
CL 51494982. Engine не изменялся и не форкался; сборка выполнялась только для
плагина в test-host project с `-NoEngineChanges`.

## Что подтвердилось

- `IMHSourceResolver` и `IMHChangeDetector` являются подменяемыми seam'ами;
  coordinator не зависит от concrete resolver/detector.
- Source scan строит стабильный snapshot: payload подтверждается до/после
  чтения и повторным byte-read; divergent UID глобален для всех primary kinds.
- Quarantine возвращает `MH_E_*`; прежний Ledger path в quarantine даёт
  `BLOCKED`, а не `REMOVE`/`MOVE`, даже если тот же UID найден в другом
  валидном payload.
- Passport descriptor/fingerprint совпадают с Python-правилами (SHA-256,
  lossless case-distinct JSON, NFC, Python-compatible canonical spelling).
- Material/composite semantic hashes используют lossless JSON и сохраняют
  case-distinct ключи `A`/`a`; NFC collision отклоняется fail-closed.
- Diff-report сохраняет независимые флаги и composite node ops, использует
  case-sensitive сравнение и deterministic JSON с двумя пробелами и LF.
- Startup C1 выполняет только `Scan -> Resolve -> Analyze -> Plan`.
  `bExecuted=false`; assets, packages и Ledger не создаются и не продвигаются.
- Writable reader output допускается только под `Saved/Mimir`; source tree,
  symlink и junction отклоняются до записи.

## Архитектурный pivot, принятый owner

После внешней приёмки C1 следующий доказательный slice начинается со stock
`UInterchangeFbxTranslator -> UInterchangeBaseNodeContainer -> UStaticMesh` и
проверки save/reload `UInterchangeAssetImportData`. Над транспортом вводится
собственный immutable MH semantic IR (`FMHSceneIR`, material/composite IR),
единый для import и будущего export. Interchange владеет UE-native lifecycle и
per-asset applied state; direct Autodesk FBX SDK остаётся transport/parity
backend и будущим deterministic writer; центральный Ledger — derived
dashboard/index. Самостоятельный `UMHStaticMeshImportData` из первого
архитектурного сообщения не является равноправной альтернативой. До решения
`UE-QUESTION-15/16` Interchange-код и новый gate не начинались.

## Проверки

- Guarded host build:
  `MimirHead_portfolioEditor Win64 Development -EnablePlugin=MimirComposite
  -NoEngineChanges -NoUBA` — **PASS**.
- Isolated `BuildPlugin -StrictIncludes` без unity/PCH — **PASS** для Editor,
  UnrealGame Development и UnrealGame Shipping. Пакет:
  `E:/MimirComposite_C1_FinalBuild_20260823`.
- `Automation RunTests Mimir` — **18/18 PASS**, process exit 0. Набор включает
  настоящий Windows junction-vector и diagnostics snapshot без concrete
  resolver.
- `MHAnalyzeSources` на `golden/fixtures/composite_cycle` — exit 0, два
  `CREATE`, отчёт записан под
  `Saved/Mimir/C1Final/analyze_sources_final18.json`;
  SHA-256 всех source-файлов до/после совпал.
- `MHAnalyzeSources -writeledger=...` — ожидаемый exit 2; файл не создан.
- `MHFbxDump golden/fixtures/axis/axis_probe.fbx --full` — exit 0; SHA-256 FBX
  до/после совпал.
- Python reference: `test_fbx_passport.py`, `test_diff_sources_v2.py`,
  `test_diff.py` — **46/46 PASS**.

Артефакты host-прогона:

- automation log:
  `D:/PersonalProjects/UE5/MimirHead_portfolio 5.7/Saved/Logs/MimirComposite-C1-Final18-20260823.log`;
- automation report:
  `D:/PersonalProjects/UE5/MimirHead_portfolio 5.7/Saved/AutomationReports/Mimir-C1-Final18-20260823/index.json`;
- strict BuildPlugin/UAT log:
  `C:/Users/helmd/AppData/Roaming/Unreal Engine/AutomationTool/Logs/D+PersonalProjects+UE5+UE_5.7/Log.txt`;
- analyzer log:
  `D:/PersonalProjects/UE5/MimirHead_portfolio 5.7/Saved/Logs/MimirComposite-C1-AnalyzeFinal18-20260823.log`;
- rejected writeledger log:
  `D:/PersonalProjects/UE5/MimirHead_portfolio 5.7/Saved/Logs/MimirComposite-C1-WriteLedgerRejectedFinal18-20260823.log`;
- fbxdump log:
  `D:/PersonalProjects/UE5/MimirHead_portfolio 5.7/Saved/Logs/MimirComposite-C1-FbxDumpFinal18-20260823.log`;
- Python JUnit:
  `D:/PersonalProjects/UE5/MimirHead_portfolio 5.7/Saved/Mimir/C1Final/python_parity.xml`.

Зафиксированные SHA-256 source-файлов до/после commandlet-прогонов совпали:

- `cycle_a.composite`:
  `25c37c6f5f623074f3ffef1a4a39b84a7927a4acc0384b695aef6c67a2f28203`;
- `cycle_b.composite`:
  `c5e719bab99a88f60a2961f03a7a31db847132015f93ab1047701a5531503d98`;
- `axis_probe.fbx`:
  `8424ad01b9e84b1063cd5842ca657c82c62d7455212a6debc74c2cc22358043e`.

Внутренний независимый pre-commit re-audit закрыл прежние P1 по diagnostics
resolver seam и quarantine precedence, подтвердил настоящий junction-vector и
существование перечисленных receipts. Это не заменяет внешнюю gate-приёмку.

## Что разошлось и было исправлено

- UE JSON object схлопывал case-distinct keys; passport и semantic hashing
  переведены на lossless DOM.
- UE `FString` сравнение по умолчанию не ловило case-only изменения; diff
  использует явное case-sensitive сравнение.
- Analyzer мог продвигать Ledger до Execute; C1 теперь полностью отклоняет
  `-writeledger`.
- Malformed/quarantined прежний payload мог выглядеть как `REMOVE`; теперь это
  `BLOCKED` с исходной диагностикой; quarantine старого path имеет приоритет и
  над валидным replacement candidate того же UID.
- `MHCompositeDumpUtil` обходил `IMHSourceResolver` через concrete scan API и
  понижал quarantine до warning. Диагностика переведена на composition root и
  нейтральный snapshot; quarantine остаётся blocking `MH_E_*`.
- Первый полный automation-прогон выявил дефект только в test vector: UUID из
  одних цифр нельзя было превратить в mis-cased UID. Вектор заменён UUID с
  `a-f`; повторный полный прогон зелёный.
- Первый strict non-unity/no-PCH build выявил два скрытых include-order дефекта:
  отсутствовал прямой `HAL/PlatformFile.h`, а тест полагался на namespace из
  соседней unity translation unit. Оба исправлены; повторный strict package и
  повторный host build зелёные.

## Внешние блокеры приёмки

- M8/M9/M10 parity-vectors и общая passport-bearing FBX golden-фикстура ещё не
  предоставлены Blender-стороной.
- `UE-QUESTION-17`: требуется принять границу C1 Analyze/Plan-only.
- `UE-QUESTION-18`: требуется нормативная политика filesystem aliases; C1
  временно fail-closed.
- Gate-положение post-C1 Interchange spike ожидает решение по
  `UE-QUESTION-15/16`.

Следующий gate до ответа внешнего аудитора не начинается.
