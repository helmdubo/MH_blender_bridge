#pragma once

#include "CoreMinimal.h"

class UMaterialInstanceConstant;
class UMHCompositeSettings;

namespace UE::MimirComposite
{

/**
 * Editor-session clipboard for Material Instance data (owner decision
 * 2026-09-03). It moves live MI state between assets without touching source
 * documents or receipts, so a donor whose parent is not registered under
 * MasterRoot -- an instance that predates the MH source protocol -- can still
 * hand its parent, parameters and static state to another instance.
 *
 * The snapshot holds only the donor's own local overrides, never the values it
 * inherits: pasting parent + own overrides reproduces the donor exactly and
 * keeps a managed target's future .material document minimal. Assets are kept
 * as soft paths, so the buffer roots nothing and survives garbage collection.
 */

/** Copies one Material Instance into the clipboard, replacing its contents. */
MIMIRCOMPOSITEEDITOR_API bool MHCopyMaterialDataToClipboard(
    const UMaterialInstanceConstant& Material,
    TArray<FString>& OutWarnings,
    FString& OutError);

/**
 * Applies the clipboard to one Material Instance: parent, static parameters,
 * base property overrides and every copied parameter override, in one
 * transaction. Nothing else on the asset is touched; the target's MH receipt,
 * name and package stay as they are.
 */
MIMIRCOMPOSITEEDITOR_API bool MHPasteMaterialDataFromClipboard(
    UMaterialInstanceConstant& Material,
    const UMHCompositeSettings& Settings,
    TArray<FString>& OutWarnings,
    FString& OutError);

/** True when a copy is available for pasting in this editor session. */
MIMIRCOMPOSITEEDITOR_API bool MHHasMaterialClipboardData();

/** Object path of the copied Material Instance, for menus and logs. */
MIMIRCOMPOSITEEDITOR_API FString MHGetMaterialClipboardSourceLabel();

/** Drops the copy. */
MIMIRCOMPOSITEEDITOR_API void MHClearMaterialClipboard();

} // namespace UE::MimirComposite
