# Квитанция: inline p2 при direct-export сцены dag4blend

Дата: 2026-08-30  
Ветка: `fix/dagor-uppercase-projection`  
База изменения: `b232aca`  
Статус: **READY FOR OWNER FIELD TEST**

## 1. Ратификация

Owner 2026-08-30 подтвердил закрытие `OPEN-V5-15`: сохранённые в сцене
dag4blend inline-параметры `*:p2` разрешено автоматически преобразовывать во
внешний `.placement`, если это не меняет уже ратифицированные wire/lifecycle
контракты. Формат `.composite`, placement-v1, resolver, golden и reference не
изменялись.

Принята следующая точная семантика:

- полный `[base, deviation]` каждого `p2` записывается в существующий
  placement-v1;
- отсутствующая ось внутри частичной offset/rotation-группы дополняется
  нейтральным `[0, 0]`;
- имя равно `dagor_p2_<xxh3-64 canonical placement bytes>`;
- одинаковые канонические байты дают один ресурс;
- dag4blend `matrix_local` при наличии автоматически принятого inline p2 —
  только preview base. Узел получает identity authored transform, поэтому
  resolver применяет base ровно один раз;
- `.placement` входит в тот же dependency-first batch перед ссылающимся
  `.composite`;
- существующий файл с тем же content-addressed именем и иными байтами не
  перезаписывается: `MH_E_AMBIGUOUS_RESOURCE_NAME` до записи composite.

Typed `mh4blend.profile` по-прежнему имеет приоритет. Сценовый `include` без
typed profile и materialize-only команда без явной папки публикации остаются
fail-closed.

## 2. Red-first

Красный тест:
`test_saved_dagorprops_inline_p2_publishes_one_shared_profile_without_double_base`.

До реализации direct-export остановился до первой записи:

```text
MH_E_COMPOSITE_GRAMMAR: subjects
['collection:direct_root/object:frame_a/NodePath:direct_root:nodes[0]',
 'rot_y:p2', 'rot_z:p2']:
dag4blend scene adapter refuses p2/include placement data without the exact
typed mh4blend.profile authority
1 failed
```

После реализации тот же тест зелёный. Два узла с одинаковым частичным p2
получают один профиль, оба authored transform равны identity; порядок
публикации:

```text
placement_profile:dagor_p2_<hash>
composite:direct_root
```

Повторный экспорт выполняет точный reuse обоих ресурсов без replace.

## 3. Дополнительные доказательства

Автотестами покрыты:

- частичные offset/rotation-группы и нейтральное дополнение осей;
- `scale:p2` и `yScale:p2` с float32-канонизацией;
- дедупликация одинаковых профилей;
- отсутствие double-base даже при заведомо ненулевом preview matrix;
- отрицательная deviation, пересечение scale с нулём, неверная арность и
  NaN/Inf — отказ до любой публикации;
- коллизия content-addressed имени — сохранение и чужого profile, и прежнего
  composite без перезаписи;
- сценовый `include` без typed authority — прежний fail-closed;
- DTO/materialize-only маршрут — прежний fail-closed с NodePath и перечнем
  параметров;
- snapshot сцены до/после direct-export полностью одинаков.

## 4. Гейты

| Гейт | Результат |
|---|---:|
| Pure `python -m pytest tests/ -q` | **305 passed / 14 skipped / 0 failed** |
| Focus: direct/bridge/publication/import | **156 passed / 0 failed** |
| `test_dag4blend_direct_export_bpy.py` | **83 passed / 0 failed** |
| Blender 4.5.12, 12 отдельных factory-startup процессов | **344 passed / 0 failed / 0 skipped** |
| `git diff --check` | **PASS** |
| Extension build + validate | **PASS** |

Пакет:
`dist/mh4blend-0.8.0-windows-x64.zip`  
SHA256:
`f7fcbfbe1f6a0e4b2c674fd2b3f121c5eeeb9adfb38723a21ab7986123cb0f10`.

Открытое GUI-окно Blender owner не перехватывалось и установленное расширение
во время его работы не перезаписывалось. Для полевого теста нужен install/reload
собранного пакета после закрытия либо перезапуска Blender.

`golden/`, `reference/`, реестр кодов, форматы и UE/plugin-файлы не изменены.

## 5. Изменённые файлы

- `addon/mh4blend/scene/import_dagor_composite.py`;
- `addon/mh4blend/scene/dag4blend_publication.py`;
- `tests/test_dag4blend_direct_export_bpy.py`;
- `docs/QUESTIONS.md`;
- `docs/13_v5_s6_1_dag4blend_bridge.md`;
- `docs/14_v5_ue_editor_program.md`;
- эта квитанция.

## 6. Полевой протокол owner

1. Перезапустить Blender с обновлённым расширением.
2. В исходной dag4blend-сцене повторить `Export Composite Include All Stuff`
   для композита с `second_floor_random/node.011`.
3. Убедиться, что прежний отказ по `rot_y:p2, rot_z:p2` исчез.
4. В output проверить `dagor_p2_<hash>.placement` и ссылку `profile` из
   опубликованного `.composite`.
5. Повторить экспорт: профиль и composite должны перейти в reuse/no-op.
6. Импортировать Source Root в UE и визуально проверить разброс узлов на
   нескольких Layout Seed.

## 7. Вопросы

Новых вопросов нет. Сценовый `include` без typed `mh4blend.profile` намеренно
не резолвится: сохранённый произвольный путь не является authority. Это
fail-closed граница, а не потеря inline p2.
