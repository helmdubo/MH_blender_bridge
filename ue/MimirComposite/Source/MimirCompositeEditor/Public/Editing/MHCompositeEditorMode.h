#pragma once

#include "CoreMinimal.h"
#include "Framework/Commands/Commands.h"
#include "Tools/LegacyEdModeInterfaces.h"
#include "Tools/UEdMode.h"
#include "MHCompositeEditorMode.generated.h"

class UMHCompositeEditSession;
class USceneComponent;
class HHitProxy;
struct FMHCompositeEditContext;

/** CE-3a: the mode's commands (Escape = Cancel, like Level Instance Edit). */
class MIMIRCOMPOSITEEDITOR_API FMHCompositeEditCommands final : public TCommands<FMHCompositeEditCommands>
{
public:
    FMHCompositeEditCommands();
    virtual void RegisterCommands() override;

    TSharedPtr<FUICommandInfo> CancelEdit;
    TSharedPtr<FUICommandInfo> SaveEdit;
};

/**
 * CE-3a (docs/contracts/composite_edit_ce0.md §2, spec CE-ADR-2): the
 * Composite Edit Mode — a public `UEdMode` of this plugin, invisible in the
 * modes toolbar, activated by the level subsystem for a CE-backend session
 * and deactivated when the session ends. It restricts selection and editing
 * to the session's projection actor, shows the `<breadcrumb> | Save | Cancel`
 * overlay (Level Instance Edit shape, owner 2026-09-07), routes Escape to
 * Cancel (after SelectNone, like the engine's mode), dims everything but the
 * projection through the `EditingLevelInstance` show flag, and leaves the
 * session before PIE.
 *
 * CE-3b: a Composite Outliner row or a viewport click grabs a node of the
 * projection (`SelectComponent`, `HandleHitProxy`): the projection actor is
 * selected exclusively, then the node's component — the gizmo sits on the
 * node. Clicks on anything else are swallowed (locked context); gizmo axes
 * and empty space keep their meaning.
 */
UCLASS(Transient)
class MIMIRCOMPOSITEEDITOR_API UMHCompositeEditorMode : public UEdMode, public ILegacyEdModeViewportInterface
{
    GENERATED_BODY()

public:
    static const FEditorModeID EM_MHCompositeEditModeId;

    UMHCompositeEditorMode();

    /** Subsystem seam: the mode follows the CE session on the level editor's mode manager. */
    static void ActivateForSession();
    static void DeactivateForSession();
    static bool IsActive();
    static UMHCompositeEditorMode* GetActive();
    /** Module lifecycle for the command set. */
    static void RegisterCommands();
    static void UnregisterCommands();

    /** Save: Apply Shared Definition (with the usual overwrite confirmation) and leave. */
    void RequestSave();
    /** Cancel: discard the draft and leave; asks first when the draft is dirty. Returns false when the user stays. */
    bool RequestCancel();
    /** CE-3b: grab a node — the projection actor exclusively, then its component. False for anything outside the projection. */
    bool SelectComponent(USceneComponent* Component);
    /** CE-3b: a viewport click. True = handled (a projection node grabbed, or a locked target swallowed). */
    bool HandleHitProxy(HHitProxy* HitProxy);
    virtual bool HandleClick(FEditorViewportClient* InViewportClient, HHitProxy* HitProxy, const FViewportClick& Click) override;
    /**
     * CE-3d: one writable session — leave the current definition (clean: at
     * once; dirty: Save / Discard / stay) and open another one of the same
     * placement; an empty path is the root definition. False when the user
     * stays or the target cannot be opened; true when nothing had to change.
     */
    bool RequestSwitch(const FString& InvocationPath);
    /** CE-3d: the breadcrumb — label and the invocation path each crumb opens (root first, the current scope last); empty without a session. */
    static TArray<TPair<FString, FString>> BreadcrumbTargets(const FMHCompositeEditContext& Context);

    virtual void Enter() override;
    virtual void Exit() override;
    virtual void CreateToolkit() override;
    virtual bool UsesToolkits() const override;
    virtual bool IsCompatibleWith(FEditorModeID OtherModeID) const override;
    virtual bool IsSelectionDisallowed(AActor* InActor, bool bInSelection) const override;
    virtual bool IsEditingDisallowed(AActor* InActor) const override;
    virtual bool OnRequestClose() override;
    virtual void ModeTick(float DeltaTime) override;

#if WITH_DEV_AUTOMATION_TESTS
    /** Stands in for the "Discard unsaved composite changes?" question: true = discard. */
    static void SetDiscardConfirmForTests(TFunction<bool()> Confirm);
    /** Stands in for "Save changes before editing …?": Yes = save, No = discard, Cancel = stay. */
    static void SetSwitchConfirmForTests(TFunction<EAppReturnType::Type()> Confirm);
#endif

private:
    virtual void BindCommands() override;
    void OnPreBeginPIE(bool bSimulate);
    void UpdateEngineShowFlags(bool bEditing);
    UMHCompositeEditSession* GetSession() const;
    bool ConfirmDiscard() const;
};
