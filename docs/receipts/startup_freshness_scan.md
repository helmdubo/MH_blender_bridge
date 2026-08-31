# Квитанция — freshness-only старт редактора (U0c)

Дата: 2026-09-01
Ветка: `perf/startup-freshness-scan`
Исполнитель среза: Lead (по слову owner «go U0c»)
Статус: **READY FOR OWNER FIELD TEST (старт редактора/карты)**

## 1. Полевой дефект

Старт редактора с полным коттеджем — до 30 минут с «замираниями».
`UMHSourceImporter::RunStartupPlan()` выполнял на каждом запуске
полный `ImportSources(All)` (скан и обработка всех ~2494 ресурсов
Source Root) и затем `MHRebuildAllLoadedCompositeActors()`.

## 2. Реализация

`RunStartupPlan` теперь выполняет только write-free
`MHScanSourcesOperation` и печатает в Message Log «Mimir»:

```
Source freshness: N resource(s) differ from their managed receipts
(M blocked). Run MH Source -> Import Changed to sync.
```

Никакого импорта и никаких ребилдов размещённых инстансов на старте;
вся тяжёлая работа — только по явным командам (Import Changed,
точечный Reimport, watcher-батчи в живой сессии — без изменений).
Стартовый рефреш инстансов удалён вместе со своим тестовым хуком
(после отказа от стартового импорта ему нечего догонять). Ретраи
тикера/quiet-периоды/PIE-задержки стартового плана не менялись.
Все подписи — только на английском (правило owner 2026-09-01).

## 3. Red → green

Новый `Mimir.V4.SourceLifecycle.StartupFreshnessOnly`: временный
Source Root с одним несинхронизированным composite → прод-ветка
стартового плана обязана завершиться, отрапортовать pending и **не
создать ни одного managed-ассета**. RED на старом коде — ассет
создавался стартовым импортом; GREEN после фикса.
`StartupRetryAndImportOrder` обновлён: ожидания стартовых рефрешей
удалены вместе с механизмом.

## 4. Гейты

| Гейт | Результат |
|---|---|
| Guarded UE build | **Succeeded** |
| Полный NullRHI + `-MHGoldenRoot` | **164/164, 0 failed** |

## 5. Полевой протокол owner

1. UE-плагин переустановлен; перезапустить редактор.
2. Старт редактора больше не зависит от объёма Source Root: вместо
   автоимпорта — одна строка freshness в Message Log «Mimir».
3. Синхронизация — явная: MH Source → Import Changed.
4. Загрузка карты всё ещё строит definitions размещённых композитов
   (holодный пул) — это остаточная стоимость до U5 (ISM, контракт у
   внешнего исполнителя) и U7.

## 6. Инцидент песочницы

Во время гейтов в audit-хост Lead
(`E:\MimirComposite_GameObj_Audit_20260828`) внешним процессом был
скопирован посторонний файл `MHCompositeISMMaterializationTest.cpp`
(U5-заготовка исполнителя), что временно уронило полный прогон.
Файл сохранён в
`C:\Users\helmd\AppData\Local\Temp\mh_lead\foreign_MHCompositeISMMaterializationTest.cpp`,
песочница восстановлена `robocopy /MIR`. Исполнителю нужен
собственный UE-хост — как в его прежних срезах.
