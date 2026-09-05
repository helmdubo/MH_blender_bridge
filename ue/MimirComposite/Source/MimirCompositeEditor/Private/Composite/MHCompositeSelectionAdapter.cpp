#include "Composite/MHCompositeSelectionAdapter.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Composite/MHCompositeActor.h"
#include "Composite/MHInstancePool.h"
#include "CoreGlobals.h"
#include "Elements/Framework/EngineElementsLibrary.h"
#include "Elements/Framework/TypedElementSelectionSet.h"
#include "Elements/Interfaces/TypedElementHierarchyInterface.h"
#include "Elements/SMInstance/SMInstanceElementData.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "LevelUtils.h"

namespace UE::MimirComposite
{
namespace
{

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
        const FSMInstanceManager SMInstance = SMInstanceElementDataUtil::GetSMInstanceFromHandle(InElementSelectionHandle, true);
        if (!SMInstance) return InElementSelectionHandle;
        UInstancedStaticMeshComponent* Component = SMInstance.GetISMComponent();
        if (Component == nullptr) return InElementSelectionHandle;
        UTypedElementSelectionSet* Set = SelectionSet.Get();

        // Pooled instance: the logical owner is the pool's answer, never the
        // service actor. The owner also learns which leaf was hit.
        if (Cast<AMHInstancePoolActor>(Component->GetOwner()) != nullptr)
        {
            if (const UMHInstancePoolSubsystem* Pool = UMHInstancePoolSubsystem::Get(Component->GetWorld()))
            {
                AActor* Owner = nullptr;
                FString NodePath;
                if (Pool->ReverseLookup(Component, SMInstance.GetISMInstanceIndex(), Owner, NodePath) && IsValid(Owner))
                {
                    if (AMHCompositeActor* Composite = Cast<AMHCompositeActor>(Owner)) Composite->SelectPlacementLeafByNodePath(NodePath);
                    const FTypedElementHandle OwnerHandle = UEngineElementsLibrary::AcquireEditorActorElementHandle(Owner);
                    return Set != nullptr ? Set->GetSelectionElement(OwnerHandle, InSelectionMethod) : OwnerHandle;
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

} // namespace UE::MimirComposite
