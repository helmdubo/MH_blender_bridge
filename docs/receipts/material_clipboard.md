# Квитанция: буфер данных материала (Copy/Paste MH Material Data)

Статус: **MERGED**. Основание — запрос owner 2026-09-03/04: перенести данные
(мастер-материал/тип шейдера и все параметры) из донорского Material Instance в
целевой MI **без** участия `.material`-исходников.

## 1. Почему не через экспорт/импорт

Донор owner (`m_sovmod_fence_wood_rotten_a` и подобные) жил в проекте до
появления source-протокола: его parent не лежит под `MasterRoot`/`LibraryRoot`,
поэтому `MatchParent` его не признаёт и `MHExtractMaterialV4` отказывает —
`MH_E_MATERIAL_NOT_ROUNDTRIPPABLE: parent is not a direct registered class or
library asset`. Смягчение extract от 2026-09-03 (PR #95) сняло отказ по
несериализуемым параметрам, но не по нерегистрируемому parent: такой материал
принципиально не представим в грамматике v4. Значит перенос обязан идти мимо
исходников — как обычная правка UObject в Content Browser.

## 2. Что сделано

`Public|Private/Material/MHMaterialClipboard.{h,cpp}` — сессионный буфер:

- `MHCopyMaterialDataToClipboard` снимает с донора **только его собственные**
  оверрайды: parent, scalar/vector/texture-параметры, static switches и
  component masks (`GetStaticParameters()`, а не `GetStaticParameterValues()` —
  второй перечисляет то, что объявил родитель), base property overrides
  (TwoSided, BlendMode, ShadingModel и т.д.). Унаследованные значения не
  копируются: parent + собственные оверрайды воспроизводят донора точно и
  оставляют будущий `.material` целевого материала минимальным. Движковый
  `CopyMaterialUniformParametersEditorOnly` не используется: он разворачивает
  всю иерархию и записал бы в целевой MI все параметры базового материала.
- Ассеты хранятся как `FSoftObjectPath`, буфер ничего не рутит и переживает GC.
- `MHPasteMaterialDataFromClipboard` в одной транзакции (`Ctrl+Z` работает),
  в порядке редактора инстансов (см. §5): `Modify` → `SetParentEditorOnly`
  с recache → `FMaterialInstanceParameterUpdateContext(All)` (очистка, значения
  параметров, base overrides, static-набор; `UpdateStaticPermutation` один раз
  при закрытии контекста) → `PostEditChange` + `MarkPackageDirty`. Пакет не
  сохраняется: сохранение остаётся за пользователем.
- Категории, которые буфер не несёт (double vector, font, texture collection,
  RVT, sparse volume, parameter collection, atlas-скаляры, material layers),
  перечисляются предупреждениями — молчаливой потери нет.
- Если цель — managed-материал, paste дополнительно сообщает
  `MH_W_MANAGED_ASSET_LOCALLY_MODIFIED` и, при нерегистрируемом parent, строку
  «can no longer be published to MH Source: …». Это следствие, а не запрет:
  операция выполняется.

UI (`UI/MHSourceToolMenus.cpp`, контекстное меню Material Instance):
«Copy MH Material Data» (ровно один выделенный MI) и «Paste MH Material Data»
(любое число целей; пункт неактивен при пустом буфере). Результат и все
предупреждения — на страницу «Mimir» Message Log.

Новых кодов диагностики нет: переиспользованы `MH_E_INVALID_RESOURCE_SOURCE`,
`MH_E_UNRESOLVED_COMPOSITE_REFERENCE`, `MH_W_MANAGED_ASSET_LOCALLY_MODIFIED`
(реестр остаётся 54/20).

## 3. Гейты

| Gate | Результат |
|---|---|
| non-unity/no-PCH build | `Result: Succeeded` (`MATCLIP2_BUILD.log`) |
| `Mimir.V4.Material.*` + `Mimir.V5.Material.*` | `Success=18 Fail=0` (`MATCLIP2_TEST.log`) |
| полный NullRHI suite (`Mimir`) | см. §4 |
| `git diff --check`, `tools/check_normative_docs.py` | чисто / OK |

Тест `Mimir.V5.Material.ClipboardCopyPaste`: донор с parent **вне** MasterRoot
и static switch → paste в managed-цель; проверяются parent, ровно один scalar,
один vector, одна текстура, base overrides, static switch, сохранность
receipt'а, оба предупреждения (локальная правка и невозможность publish), отказ
paste при пустом буфере без мутации цели, и повторный paste от донора с
зарегистрированным parent — после него материал снова извлекается в документ.

Промежуточный красный прогон: `MATCLIP_TEST.log` — `Fail=1`, «target adopts the
donor static switch», причина в `GetStaticParameterValues` (фильтрует по
параметрам родителя); исправлено переходом на `GetStaticParameters()`.

## 4. Порядок в редакторе

1. ПКМ по донорскому MI → **Copy MH Material Data**.
2. ПКМ по целевому MI → **Paste MH Material Data**, затем сохранить ассет.
3. Если нужен и исходник: цель должна иметь parent под `MasterRoot`; тогда
   **Publish Material to MH Source** запишет `.material`.

## 5. Инцидент 2026-09-04: краш при paste в портфолио

`EXCEPTION_ACCESS_VIOLATION reading 0xffffffffffffffff`, стек целиком в
`D3D12RHI`/`Renderer` — рендер-тред читал ресурсы материала, разрушенные во
время вставки. Причина в последовательности первой версии paste:
`SetParentEditorOnly(Parent, /*RecacheShader*/ false)` → `Clear…` →
`UpdateStaticPermutation` → значения: инстанс менял parent без пересборки
шейдеров, а пермутация собиралась до значений. На хосте в NullRHI и даже под
D3D12 автотест не воспроизводит краш: у синтетических материалов нет живых
render-прокси, только у реальных ассетов открытого редактора.

Первая попытка (PR #98) — движковая последовательность редактора инстансов:
`SetParentEditorOnly(Parent)` с recache, затем один
`FMaterialInstanceParameterUpdateContext(All)` (очистка в конструкторе,
значения, base overrides, static-набор через `GetStaticParameters()`,
`UpdateStaticPermutation` один раз в деструкторе). Null-текстуры донора больше
не записываются. **Краш повторился**, только позже.

Недостающая часть — `FMaterialUpdateContext` (PR #100). `MaterialShared.h`:
«This class should *always* be used when doing so». Его конструктор
синхронизируется с рендер-тредом и снимает render state со **всех**
компонентов, деструктор обновляет ресурсы материала, зависимые инстансы и
регистрирует компоненты обратно. Смена parent плюс смена static-пермутации без
него оставляла в сцене прокси, ссылающиеся на ресурсы прежнего родителя —
отсюда падение «чуть позже», уже на кадре рендера. Так же обёрнута смена
parent в самом редакторе инстансов (`PreviewMaterial.cpp`
`UpdateSourceInstanceParent` внутри `FMaterialUpdateContext`). Модуль
`MimirCompositeEditor` получил зависимость `RHI`: конструктор контекста имеет
дефолт `GMaxRHIShaderPlatform`.

Итоговый порядок paste: транзакция → `Modify` → `FMaterialUpdateContext` →
`SetParentEditorOnly` → `FMaterialInstanceParameterUpdateContext` (значения) →
`PostEditChange` → `AddMaterialInstance` → закрытие контекста →
`MarkPackageDirty`.

Проверка: `Mimir.V4/V5.Material` 18/18 под **D3D12** (`MATCLIP6_RHI.log`),
полный NullRHI suite, `BuildPlugin -StrictIncludes`. Автотесты краш не
воспроизводят (у синтетических материалов нет живых render-прокси и компонентов
в сцене), поэтому подтверждение на реальной сцене — за owner.

## 6. Вопросы

Открытых нет. Перенос material layers и редких типов параметров не
реализован осознанно (предупреждение вместо тихой потери); при необходимости —
отдельный срез.
