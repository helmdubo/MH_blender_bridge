# Asset Viewer: референс вложенного Composite Edit

Справочная заметка, не норматив. Проверено 2026-09-07 по DagorEngine,
commit `75723669297e48e200a0dc67b18c1629e0975daf`.

- Вход сохраняет родительский ассет, accumulated transform ссылки и ID
  DataBlock в стеке; активным становится ассет подкомпозита.
  [enterSubCompositeEditing](https://github.com/GaijinEntertainment/DagorEngine/blob/75723669297e48e200a0dc67b18c1629e0975daf/prog/tools/AssetViewer/Entity/compositeEditor.cpp#L164-L203).
- Родительское окружение представлено ghost entity. При смене пространства
  ассета камера пересчитывается между parent/subcomposite coordinates.
  [begin](https://github.com/GaijinEntertainment/DagorEngine/blob/75723669297e48e200a0dc67b18c1629e0975daf/prog/tools/AssetViewer/Entity/compositeEditor.cpp#L79-L105),
  [exit](https://github.com/GaijinEntertainment/DagorEngine/blob/75723669297e48e200a0dc67b18c1629e0975daf/prog/tools/AssetViewer/Entity/compositeEditor.cpp#L304-L349).
- Shared Save сохраняет текущее определение и выходит. Unique создаёт новый
  файл, возвращает оригинальные props старого ассета, а после возврата заменяет
  ссылку в родительском узле отдельной Undo-операцией. Revert возвращает
  исходные props и выходит.
  [save / unique / revert](https://github.com/GaijinEntertainment/DagorEngine/blob/75723669297e48e200a0dc67b18c1629e0975daf/prog/tools/AssetViewer/Entity/compositeEditor.cpp#L225-L303).
- Undo хранит снимок дерева и при необходимости ID основного и множественного
  выбора. [CompositeEditorUndo](https://github.com/GaijinEntertainment/DagorEngine/blob/75723669297e48e200a0dc67b18c1629e0975daf/prog/tools/AssetViewer/Entity/compositeEditorUndo.cpp#L5-L75).

Вывод для MH: полезны явный контекст вложенности, identity узла и разделение
shared/unique publication. UE-проекция остаётся в одном world, поэтому
пересчёт камеры Asset Viewer сюда не переносится. Требование owner — сохранять
текущий пользовательский вид при Enter/switch/Save/Cancel — остаётся в силе.
Референс принят для следующих исправлений вложенной навигации и Unique;
в текущем CE-I1 источник и runtime-конвейер не заменяются реализацией Dagor.
