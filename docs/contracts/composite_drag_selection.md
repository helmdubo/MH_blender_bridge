# Composite Edit: drag и viewport selection — 2026-09-07

Статус: ACCEPTED / READY FOR FIELD TEST. Owner после полевого теста разрешил исправить
низкую производительность драга и вернуть выбор вложенного композита через
его меш. Уточнение owner: Edit открывает содержимое найденного подкомпозита,
как Composite Outliner, и предварительно выделяет узел задетого меша.
Ветка `codex/composite-drag-selection`, база `b99ff394`.

## Поведение

1. Авторские трансформы и геометрия обновляются на каждом шаге жеста. Дерево,
   Details и native selection не пересобираются для каждого mouse delta.
   Один жест остаётся одной Undo-транзакцией; Cancel/Esc, отказ при shear и
   генеративные потомки сохраняют прежнюю семантику.
2. Проекция может повторно использовать последний успешно построенный граф,
   обновляя authored TRS без сериализации и перекомпиляции рецепта. Resolver
   остаётся авторитетом математики и случайных выборок. Структурные изменения
   и несовпадение topology используют полное обновление.
3. Клик по листу normal placement определяет ближайшее содержащее его
   вхождение композита, включая выбранный composite option random-узла.
   Подсвечиваются листья этого вхождения; соседние вызовы того же определения
   и другие размещения не подсвечиваются. Native actor selection сохраняется.
4. Edit Contents открывает это определение в контексте конкретного вхождения
   и выделяет авторский узел задетого листа. Выбор самого актора открывает
   корневое определение. Камера не перемещается.
5. Состояние выбора временное, не записывается в source или map; исчезнувший
   путь после rebuild не используется. Чужие ISM сохраняют штатное поведение.

## Связанный scope и проверка

Performance: EditSession, EditProjection, EditorMode (h/cpp), EditPerf tests.
Selection: CompositeActor, InstancePool (h/cpp), SelectionAdapter cpp и tests;
UI: SourceToolMenus, CompositeOutliner. Связанные API, тесты и документация
входят в принятый owner срез; wire-формат, resolver и source assets неизменны.

DECIDED: повторный клик по уже единственной выделенной строке World Outliner
не даёт selection event в UE 5.7. Поэтому команда из контекстного меню World
Outliner явно открывает root, независимо от прошлого viewport hit; глобальный
перехватчик мыши не добавляется. Новый реальный actor selection после deselect
сбрасывает логический leaf и возвращает подсветку всего owner.

До production изменений фиксируются RED лог уведомлений жеста и RED выбора
вложения. После — component identity и full-refresh parity, selected random
option, повторные вызовы, Undo/Cancel, guarded/StrictIncludes/force-unity,
полный NullRHI Mimir с golden и обоими RecipeShadowParity, RHI smoke,
до/после mh.PerfTrace и baseline drag. Установка проверенной сборки при
закрытом пользовательском Unreal Editor; результаты в отдельной квитанции.
