# Composite F / Ctrl+B — 2026-09-15

Implemented in the editor plugin; Engine source and project content were not edited.

- Level viewport F frames the selected placement leaf or the whole placement.
  Bounds use the current ISM address behind each refreshed materialization row,
  never the bounds of the shared pool component or the actor icon.
- Edit selection F unions selected node geometry, including nested references
  and child-actor geometry. Composite Outliner F uses its selected rows.
- Ctrl+B resolves selected meshes and composite definitions, supports multiple
  nodes, and preserves the root asset target for whole-placement selection.
  Outliner option rows browse their own resource, including inactive options;
  empty or missing resources do not fall through to the root asset.
- Viewport command bindings are refreshed after layout changes and removed on
  module shutdown. Ordinary non-composite selection retains native commands.

## Verification

UE 5.7, separate test project under `E:/temp/MH_navigation_20260915/`:

- `BuildPlugin -StrictIncludes -TargetPlatforms=Win64`: SUCCESS. Final log:
  `E:/temp/MH_navigation_20260915_build_final.log`.
- NullRHI: **13 passed, 0 failed, 0 not run** (6 success, 7 with warnings).
  `NullReport/index.json` covers Navigation, Selection, EditMode.Selection,
  OutlinerModel, and Lifecycle.BrowseToAssetFindsComposite.
- D3D12/SM6 offscreen: **5 passed, 0 failed, 0 not run** (1 success, 4 with
  warnings), `D3DReport/index.json`: Navigation and EditMode.Selection.
- The placement navigation test executes the actual viewport F command and
  checks the camera look-at point for the whole placement and a selected leaf.
- Packaged Source hashes match the working tree; `git diff --check` passes.

Warnings include the existing synthetic-world cleanup warnings. The initial
build found an incorrect UE header include path; this was fixed before the
successful final build and both automation runs.

Package: `E:/temp/MH_navigation_20260915/Package/`.
Manual owner acceptance was not run.

## Installation follow-up

Installed into `D:/PersonalProjects/UE5/MimirHead_portfolio 5.7/Plugins/MimirComposite`
on the user's explicit install/commit/push/merge request.

The previous installation contained WPO ray-tracing changes absent from main.
The navigation patch was applied to a separate copy of that installation and
built with `BuildPlugin -StrictIncludes`, preserving those local changes without
including them in the navigation commit.

- Combined package: `E:/temp/MH_navigation_20260915/InstallPackage/`.
- Combined strict build: SUCCESS, `install_build.log` in that task directory.
- D3D12/SM6: **23 passed, 0 failed, 0 not run** (15 success, 8 with warnings),
  `InstallReport/index.json`; navigation, edit selection/projection, pool, runtime.
- Installed files verified against the combined package by SHA-256. The previous
  installation was checked for concurrent changes before replacement.
- Backup: `D:/PersonalProjects/UE5/MH_plugin_backups/MimirComposite_before_navigation_20260915`.

No editor was running at replacement; no project content was changed.
