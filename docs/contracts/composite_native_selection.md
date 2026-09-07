# Composite native selection — 2026-09-07

Статус: READY FOR FIELD TEST. Прямое решение owner после исследования
UE5.7.4 заменяет разделение кнопок из `composite_click_buttons.md`.
Ветка `codex/composite-native-selection`, база `859dd0a`.

Вне Edit выбранный корень остаётся корнем при одиночном LMB/RMB. Двойной
LMB по единственному выбранному placement переключает root ↔ logical
subcomposite selection. В состоянии вложений одиночный клик выбирает
ближайшее вхождение композита, которому принадлежит leaf под курсором.
RMB открывает меню текущего уровня; Edit корня открывает корень, Edit
вложения сохраняет preselection authored node выбранного leaf.

Это адаптация ComponentElementLevelEditorSelectionCustomization к логическим
вхождениям MH. Служебные ISM component/instance не становятся пользовательским
выделением. Native selection set сохраняет owner actor; логический scope
подсветки переживает штатный LMB ClearSelection → SelectElement. Реальное
снятие выделения/переход к другому actor очищает scope. Обычный actor hit
возвращает root. Edit session, быстрый drag, source и geometry storage прежние.

Acceptance:
1. Root LMB → RMB → Edit открывает root, без углубления от RMB.
2. Double LMB переключает root ↔ occurrence, single выбирает sibling occurrence.
3. Выделяется всё occurrence и только оно; shared pool не выделяет соседний actor.
4. Реальный native цикл deselect/reselect сохраняет выбранную logical цель;
   другой actor, stale hit и отказ selection не оставляют ложную цель.
5. Nested Edit открывает выбранное occurrence с выбранным authored node.
6. Edit gestures не меняются; non-MH selection делегируется UE. По следующему
   уточнению owner Outliner перенесён в левую панель Edit toolkit, отдельная
   Nomad-вкладка удалена. Существующие node commands/details сохраняются.
7. Red-first, полные гейты KICKOFF §9, RHI smoke и проверенный install с backup.

Файлы: SelectionAdapter h/cpp, SourceToolMenus cpp, EditorMode cpp,
Outliner h/cpp, EditorModule cpp. Последние четыре файла добавлены прямым
уточнением owner о переносе панели. Тесты: MHCompositeNativeSelectionTest.cpp,
MHCompositeSelectionAdapterTest.cpp. Документы: этот контракт, receipt,
docs/16_recipe_model.md §2.8, docs/RECIPE_EXECUTION_STATUS.md.

Реализация: pure selection resolution сохраняет выбранный уровень в
одноразовом set/owner/frame pending result. OnChanged после штатного
ClearSelection → SelectElement применяет его; RMB menu завершает pending
result, только если redundant selection не дал OnChanged. Меню не вычисляет
новую область по raw hit. Native Actor customization остаётся установленным;
MH не копирует private Actor selection rules. Модификаторы/группы остаются
Actor-командами UE; при multi-actor selection logical scope очищается.
