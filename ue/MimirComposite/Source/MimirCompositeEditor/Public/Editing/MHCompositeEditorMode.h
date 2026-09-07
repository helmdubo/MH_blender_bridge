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

/** The mode's explicit Save/Cancel commands; Esc also respects gesture and node selection. */
class MIMIRCOMPOSITEEDITOR_API FMHCompositeEditCommands final : public TCommands<FMHCompositeEditCommands>
{
public:
    FMHCompositeEditCommands();
    virtual void RegisterCommands() override;

    TSharedPtr<FUICommandInfo> CancelEdit;
    TSharedPtr<FUICommandInfo> SaveEdit;
};

/**
 * Composite authoring mode: logical session nodes own selection, outlines and
 * widget frames. The viewport supplies native navigation, snapping and gizmo
 * deltas; the mode writes validated draft commands in one transaction per
 * gesture. Projection objects remain disposable rendering infrastructure.
 */
UCLASS(Transient)
class MIMIRCOMPOSITEEDITOR_API UMHCompositeEditorMode : public UEdMode, public ILegacyEdModeViewportInterface, public ILegacyEdModeWidgetInterface, public ILegacyEdModeSelectInterface
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
    /** Select the authored owner of any projection visual; all of its visuals follow. */
    bool SelectComponent(USceneComponent* Component);
    void SelectNodeIds(const TArray<FGuid>& NodeIds, const FGuid& ActiveNodeId = FGuid());
    /** Esc cancels a gesture, then clears nodes, then offers to leave the session. */
    bool HandleEscape();
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
    /**
     * CE-4a: a gizmo gesture on a projection node is one transaction — Start
     * opens it, every delta writes the node's local transform into the draft
     * (the projection follows), End closes it (an empty gesture is cancelled).
     * Empty logical selection hides the widget and starts no authoring gesture.
     */
    virtual bool StartTracking(FEditorViewportClient* InViewportClient, FViewport* InViewport) override;
    virtual bool InputDelta(FEditorViewportClient* InViewportClient, FViewport* InViewport, FVector& InDrag, FRotator& InRot, FVector& InScale) override;
    virtual bool EndTracking(FEditorViewportClient* InViewportClient, FViewport* InViewport) override;
    virtual bool InputKey(FEditorViewportClient* ViewportClient, FViewport* Viewport, FKey Key, EInputEvent Event) override;
    virtual bool AllowsViewportDragTool() const override { return true; }
    virtual bool BoxSelect(FBox& InBox, bool InSelect = true) override;
    virtual bool FrustumSelect(const FConvexVolume& InFrustum, FEditorViewportClient* InViewportClient, bool InSelect = true) override;
    virtual void SelectNone() override;
    virtual void ActorSelectionChangeNotify() override;
    virtual void PostUndo() override;

    virtual bool AllowWidgetMove() override { return false; }
    virtual bool CanCycleWidgetMode() const override { return true; }
    virtual bool ShowModeWidgets() const override { return true; }
    virtual EAxisList::Type GetWidgetAxisToDraw(UE::Widget::EWidgetMode InWidgetMode) const override { return EAxisList::XYZ; }
    virtual FVector GetWidgetLocation() const override;
    virtual bool ShouldDrawWidget() const override;
    virtual bool UsesTransformWidget() const override { return true; }
    virtual bool UsesTransformWidget(UE::Widget::EWidgetMode CheckMode) const override;
    virtual FVector GetWidgetNormalFromCurrentAxis(void* InData) override;
    virtual void SetCurrentWidgetAxis(EAxisList::Type InAxis) override { CurrentWidgetAxis = InAxis; }
    virtual EAxisList::Type GetCurrentWidgetAxis() const override { return CurrentWidgetAxis; }
    virtual bool UsesPropertyWidgets() const override { return false; }
    virtual bool GetCustomDrawingCoordinateSystem(FMatrix& InMatrix, void* InData) override;
    virtual bool GetCustomInputCoordinateSystem(FMatrix& InMatrix, void* InData) override;
    virtual bool HasCustomViewportFocus() const override { return true; }
    virtual FBox ComputeCustomViewportFocus() const override;

    // Projection objects are derived. Structural commands must go through the draft.
    virtual EEditAction::Type GetActionEditDuplicate() override { return EEditAction::Halt; }
    virtual EEditAction::Type GetActionEditDelete() override { return EEditAction::Halt; }
    virtual EEditAction::Type GetActionEditCut() override { return EEditAction::Halt; }
    virtual EEditAction::Type GetActionEditCopy() override { return EEditAction::Halt; }
    virtual EEditAction::Type GetActionEditPaste() override { return EEditAction::Halt; }

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
    EAppReturnType::Type ConfirmSwitch(const FString& TargetInvocationPath) const;
    void MirrorSelection();
    void CancelGesture();
    bool SelectBounds(TFunctionRef<bool(const FBox&)> Intersects, bool bSelect);
    bool bMirroringSelection = false;
    EAxisList::Type CurrentWidgetAxis = EAxisList::None;
    TWeakObjectPtr<UMHCompositeEditSession> BoundSession;
    struct FGestureNode
    {
        FGuid NodeId;
        FTransform AuthoredLocal;
        FMatrix ParentWorld = FMatrix::Identity;
        FTransform World;
    };
    TArray<FGestureNode> GestureNodes;
    FVector GesturePivot = FVector::ZeroVector;

    /** CE-4a gesture state. */
    bool bTracking = false;
    bool bCancelledTracking = false;
    bool bNavigationTracking = false;
    int32 GestureTransaction = INDEX_NONE;
    bool bGestureChanged = false;
};
