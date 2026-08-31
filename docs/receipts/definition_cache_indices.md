# Квитанция — индексы definition-кэша (U2)

Дата: 2026-09-01
Ветка: `perf/definition-cache-indices`
Исполнители: внешний Opus-агент (реализация, по слову owner
«координируй opus5 агентов») + Lead (бриф, ревью, RED/GREEN, гейты)
Статус: **ГОТОВО (полевой шаг не требуется)**

## 1. Срез

U2 перф-программы (док 14 §6): `UMHCompositeDefinitionSubsystem` вёл
два линейных обхода всего пула — поиск root-кандидатов в
`GetOrBuildDefinition` (полный пятикомпонентный ключ не известен до
построения: ClosureHash — результат) и `InvalidateDefinition`
(проверка `Dependencies.Contains` у каждой записи).

## 2. Реализация

Два вторичных индекса (пятикомпонентный ключ остаётся identity):
`DefinitionKeysByRoot` (root → ключи) и `DefinitionKeysByDependency`
(ресурс → ключи зависимых). Обслуживание — три приватных хелпера
(`IndexDefinition` / `UnindexDefinition` / `RemoveDefinitionByKey`),
покрыты все пути мутации: добавление, stale-удаления внутри lookup
(итерация по snapshot-копии бакета), `RemoveDeadDefinitions`,
`InvalidateDefinition` (бакет зависимости отсоединяется целиком —
после инвалидации на ключе никто зависеть не может),
`InvalidateAllDefinitions`, `Deinitialize`. Пустые бакеты удаляются.
Семантика не изменена, включая запись
`ResourceInvalidationRevisions`/serial при инвалидации без зависимых.

Наблюдаемость: счётчики `LookupProbes`/`InvalidationProbes` в
существующей `FMHDefinitionCacheMetrics` (строка `S65_METRICS` не
менялась; тест печатает собственную `S65_INDEX`).

## 3. Red → green

`Mimir.V5.Composite.DefinitionPool.IndexedLookupAndInvalidation`:
4 разных root; повторный lookup последнего root обязан осмотреть ровно
1 запись; точечная инвалидация уникальной зависимости — ровно 1;
поведение: инвалидированный root пересобирается ровно один раз,
остальные остаются в кэше.

- RED против линейной реализации (HEAD subsystem + новый тест):
  `to be 1, but it was 0` по обоим probe-ассертам;
- GREEN: полный NullRHI **165/165, 0 failed**.

Инцидент верификации: после RED-отката robocopy вернул файлам старый
mtime и UBT не перекомпилировал subsystem («зелёная» сборка со старой
DLL, probes=0). Диагностировано по хэшам файлов, вылечено touch'ем.
Урок для будущих RED-прогонов через подмену файлов в audit-хосте.

## 4. Гейты

| Гейт | Результат |
|---|---|
| Guarded UE build | **Succeeded** |
| Полный NullRHI + `-MHGoldenRoot` | **165/165, 0 failed** |
