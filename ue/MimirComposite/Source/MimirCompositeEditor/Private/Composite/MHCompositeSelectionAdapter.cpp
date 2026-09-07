#include "Composite/MHCompositeSelectionAdapter.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Composite/MHCompositeActor.h"
#include "Composite/MHCompositeLevelSubsystem.h"
#include "Composite/MHInstancePool.h"
#include "CoreGlobals.h"
#include "Elements/Actor/ActorElementData.h"
#include "Elements/Framework/EngineElementsLibrary.h"
#include "Elements/Framework/TypedElementSelectionSet.h"
#include "Elements/Interfaces/TypedElementHierarchyInterface.h"
#include "Elements/SMInstance/SMInstanceElementData.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "Editing/MHCompositeEditProjection.h"
#include "Editing/MHCompositeEditSession.h"
#include "Editing/MHCompositeEditorMode.h"
#include "Editor.h"
#include "LevelUtils.h"

namespace UE::MimirComposite
{
namespace
{

// Logical equivalent of UE's actor/component selection. Resolve before the
// native click, then commit after ClearSelection/SelectElement has completed.
struct FPendingCompositeContextHit
{
    TWeakObjectPtr<AMHCompositeActor> Owner;
    TWeakObjectPtr<const UTypedElementSelectionSet> SelectionSet;
    FString LeafPath;
    uint64 Frame = MAX_uint64;
};

FPendingCompositeContextHit PendingContextHit;

bool IsCompositeEditOpen()
{
    const UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr
        ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    return Subsystem != nullptr && Subsystem->IsEditingComposite();
}

bool ApplyPendingCompositeSelection(const UTypedElementSelectionSet& SelectionSet)
{
    if (PendingContextHit.SelectionSet.Get() != &SelectionSet) return false;
    const FPendingCompositeContextHit Hit = MoveTemp(PendingContextHit);
    PendingContextHit = FPendingCompositeContextHit();
    AMHCompositeActor* Owner = Hit.Owner.Get();
    if (Hit.Frame != GFrameCounter || Owner == nullptr || GEdSelectionLock || IsCompositeEditOpen()) return false;
    const FTypedElementHandle OwnerHandle = UEngineElementsLibrary::AcquireEditorActorElementHandle(Owner);
    if (!SelectionSet.GetElementList()->Contains(OwnerHandle) ||
        SelectionSet.GetElementList()->CountElementsOfType(NAME_Actor) != 1) return false;
    if (Hit.LeafPath.IsEmpty())
    {
        Owner->ClearPlacementLeafSelection();
        return true;
    }
    if (Owner->SelectPlacementLeafByNodePath(Hit.LeafPath)) return true;
    Owner->ClearPlacementLeafSelection();
    return false;
}

/**
 * NAME_SMInstance selection customization of the level editor's selection set.
 * A pooled instance resolves to the owner composite actor (16 §2.8); stock
 * instances keep the level editor's behaviour: instance -> owning component ->
 * owning actor on a single click, the instance itself on a second click.
 */
class FMHPoolInstanceSelectionCustomization final : public FTypedElementSelectionCustomization
{
public:
    explicit FMHPoolInstanceSelectionCustomization(UTypedElementSelectionSet& InSelectionSet)
        : SelectionSet(&InSelectionSet)
    {
        SelectionChangedHandle = InSelectionSet.OnChanged().AddLambda([](const UTypedElementSelectionSet* ChangedSet)
        {
            if (ChangedSet == nullptr) return;
            if (!GEdSelectionLock && !IsCompositeEditOpen() &&
                ChangedSet->GetElementList()->CountElementsOfType(NAME_Actor) != 1)
            {
                for (AMHCompositeActor* Actor : ChangedSet->GetSelectedObjects<AMHCompositeActor>())
                    Actor->ClearPlacementLeafSelection();
            }
            ApplyPendingCompositeSelection(*ChangedSet);
        });
    }

    virtual ~FMHPoolInstanceSelectionCustomization() override
    {
        if (UTypedElementSelectionSet* Set = SelectionSet.Get()) Set->OnChanged().Remove(SelectionChangedHandle);
    }

    virtual bool CanSelectElement(const TTypedElement<ITypedElementSelectionInterface>& InElementSelectionHandle, const FTypedElementSelectionOptions& InSelectionOptions) override
    {
        static_cast<void>(InSelectionOptions);
        const FSMInstanceManager SMInstance = SMInstanceElementDataUtil::GetSMInstanceFromHandle(InElementSelectionHandle, true);
        if (!SMInstance) return false;
        AActor* Owner = SMInstance.GetISMComponent()->GetOwner();
        if (Owner == nullptr) return false;
        AActor* SelectionRoot = Owner->GetRootSelectionParent();
        ULevel* SelectionLevel = SelectionRoot != nullptr ? SelectionRoot->GetLevel() : Owner->GetLevel();
        if (!Owner->IsTemplate() && FLevelUtils::IsLevelLocked(SelectionLevel)) return false;
        return !GEdSelectionLock;
    }

    virtual bool CanDeselectElement(const TTypedElement<ITypedElementSelectionInterface>& InElementSelectionHandle, const FTypedElementSelectionOptions& InSelectionOptions) override
    {
        static_cast<void>(InSelectionOptions);
        const FSMInstanceManager SMInstance = SMInstanceElementDataUtil::GetSMInstanceFromHandle(InElementSelectionHandle, true);
        return SMInstance && !GEdSelectionLock;
    }

    virtual FTypedElementHandle GetSelectionElement(const TTypedElement<ITypedElementSelectionInterface>& InElementSelectionHandle, FTypedElementListConstRef InCurrentSelection, const ETypedElementSelectionMethod InSelectionMethod) override
    {
        PendingContextHit = FPendingCompositeContextHit();
        const FSMInstanceManager SMInstance = SMInstanceElementDataUtil::GetSMInstanceFromHandle(InElementSelectionHandle, true);
        if (!SMInstance) return InElementSelectionHandle;
        UInstancedStaticMeshComponent* Component = SMInstance.GetISMComponent();
        if (Component == nullptr) return InElementSelectionHandle;
        UTypedElementSelectionSet* Set = SelectionSet.Get();

        // Pooled instance: the logical owner is the pool's answer, never the
        // service actor. Primary keeps the current logical level; Secondary
        // toggles it just like UE's ordinary actor/component customization.
        if (Cast<AMHInstancePoolActor>(Component->GetOwner()) != nullptr)
        {
            if (const UMHInstancePoolSubsystem* Pool = UMHInstancePoolSubsystem::Get(Component->GetWorld()))
            {
                AActor* Owner = nullptr;
                FString NodePath;
                if (Pool->ReverseLookup(Component, SMInstance.GetISMInstanceIndex(), Owner, NodePath) && IsValid(Owner))
                {
                    const FTypedElementHandle OwnerHandle = UEngineElementsLibrary::AcquireEditorActorElementHandle(Owner);
                    const FTypedElementHandle Resolved = Set != nullptr ? Set->GetSelectionElement(OwnerHandle, InSelectionMethod) : OwnerHandle;
                    if (AMHCompositeActor* Composite = Cast<AMHCompositeActor>(Owner);
                        Composite != nullptr && Set != nullptr && Resolved == OwnerHandle &&
                        !GEdSelectionLock && !IsCompositeEditOpen() &&
                        Set->CanSelectElement(Resolved, FTypedElementSelectionOptions().SetAllowHidden(true).SetWarnIfLocked(true)))
                    {
                        const bool bOwnerSelectedAlone = InCurrentSelection->Contains(OwnerHandle) &&
                            InCurrentSelection->CountElementsOfType(NAME_Actor) == 1;
                        const bool bNestedSelected = !Composite->GetSelectedPlacementLeafPath().IsEmpty();
                        const bool bSecondary = InSelectionMethod == ETypedElementSelectionMethod::Secondary;
                        const bool bSelectNested = InSelectionMethod == ETypedElementSelectionMethod::FromSecondary ||
                            (bOwnerSelectedAlone && (bNestedSelected != bSecondary));
                        PendingContextHit.Owner = Composite;
                        PendingContextHit.SelectionSet = Set;
                        PendingContextHit.LeafPath = bSelectNested ? NodePath : FString();
                        PendingContextHit.Frame = GFrameCounter;
                    }
                    return Resolved;
                }
            }
            // An instance the pool does not know (stale index): nothing to select.
            return FTypedElementHandle();
        }

        // Stock instance: mirror the level editor's SMInstance customization.
        const FTypedElementHandle OwningComponentHandle = UEngineElementsLibrary::AcquireEditorComponentElementHandle(Component);
        const bool bWasDoubleClick = InSelectionMethod == ETypedElementSelectionMethod::Secondary;
        const bool bComponentAlreadySelected = InCurrentSelection->Contains(OwningComponentHandle);
        const bool bIsISMAlreadySelected = InCurrentSelection->Contains(InElementSelectionHandle);
        bool bIsSiblingSelected = false;
        if (InCurrentSelection->HasElementsOfType(InElementSelectionHandle.GetId().GetTypeId()))
        {
            const FSMInstanceManager Selected = SMInstanceElementDataUtil::GetSMInstanceFromHandle(InCurrentSelection->GetTopElement<ITypedElementHierarchyInterface>(), true);
            bIsSiblingSelected = Selected && Selected.GetISMComponent() == Component;
        }
        if (!bWasDoubleClick && (bIsSiblingSelected || bIsISMAlreadySelected))
        {
            return Set != nullptr ? Set->GetSelectionElement(OwningComponentHandle, ETypedElementSelectionMethod::FromSecondary) : OwningComponentHandle;
        }
        if (bWasDoubleClick && (bIsSiblingSelected || bComponentAlreadySelected)) return InElementSelectionHandle;
        return Set != nullptr ? Set->GetSelectionElement(OwningComponentHandle, InSelectionMethod) : OwningComponentHandle;
    }

private:
    TWeakObjectPtr<UTypedElementSelectionSet> SelectionSet;
    FDelegateHandle SelectionChangedHandle;
};

TSet<TWeakObjectPtr<const UTypedElementSelectionSet>>& RegisteredSets()
{
    static TSet<TWeakObjectPtr<const UTypedElementSelectionSet>> Sets;
    return Sets;
}

} // namespace

bool MHRegisterPoolInstanceSelection(UTypedElementSelectionSet& SelectionSet)
{
    if (MHIsPoolInstanceSelectionRegistered(SelectionSet)) return true;
    SelectionSet.RegisterInterfaceCustomizationByTypeName(NAME_SMInstance, MakeUnique<FMHPoolInstanceSelectionCustomization>(SelectionSet));
    RegisteredSets().Add(&SelectionSet);
    return true;
}

bool MHIsPoolInstanceSelectionRegistered(const UTypedElementSelectionSet& SelectionSet)
{
    return RegisteredSets().Contains(&SelectionSet);
}

bool MHSelectCompositeContextHit(const FTypedElementHandle& ContextHit, AMHCompositeActor& ExpectedOwner)
{
    // An already selected RMB target may produce no OnChanged notification.
    // Finish the choice made by GetSelectionElement; never infer a new scope here.
    const UTypedElementSelectionSet* Set = PendingContextHit.SelectionSet.Get();
    if (Set == nullptr || PendingContextHit.Owner.Get() != &ExpectedOwner ||
        ActorElementDataUtil::GetActorFromHandle(ContextHit, true) != &ExpectedOwner) return false;
    return ApplyPendingCompositeSelection(*Set);
}

bool MHBeginEditPickedComposite(
    AMHCompositeActor& Actor, const FString& LeafPath, FString& OutError)
{
    OutError.Reset();
    FString OccurrencePath;
    if (LeafPath.IsEmpty() || !Actor.FindPlacementOccurrenceForLeafPath(LeafPath, OccurrencePath))
    {
        OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: the clicked composite leaf is no longer present");
        return false;
    }
    UMHCompositeLevelSubsystem* Subsystem = GEditor != nullptr
        ? GEditor->GetEditorSubsystem<UMHCompositeLevelSubsystem>() : nullptr;
    if (Subsystem == nullptr)
    {
        OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: Edit Contents requires an editor composite subsystem");
        return false;
    }
    const bool bOpened = OccurrencePath.IsEmpty()
        ? Subsystem->BeginEditComposite(&Actor, OutError)
        : Subsystem->BeginEditNestedComposite(&Actor, OccurrencePath, OutError);
    if (!bOpened) return false;

    UMHCompositeEditSession* Session = Subsystem->GetEditSession();
    UMHCompositeEditProjection* Projection = Session != nullptr ? Session->GetProjection() : nullptr;
    UMHCompositeEditorMode* Mode = UMHCompositeEditorMode::GetActive();
    USceneComponent* Component = Projection != nullptr ? Projection->FindComponentForOrigin(LeafPath) : nullptr;
    if (Mode != nullptr && Component != nullptr && Mode->SelectComponent(Component)) return true;

    FString CancelError;
    Subsystem->CancelEditComposite(CancelError);
    OutError = TEXT("MH_E_INVALID_RESOURCE_SOURCE: the clicked leaf has no editable authored owner in this occurrence");
    if (!CancelError.IsEmpty()) OutError += TEXT(": ") + CancelError;
    return false;
}

} // namespace UE::MimirComposite
