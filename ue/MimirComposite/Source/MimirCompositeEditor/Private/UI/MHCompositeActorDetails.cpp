#include "UI/MHCompositeActorDetails.h"

#include "Composite/MHCompositeActor.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "Editor.h"
#include "IDetailCustomization.h"
#include "IDetailPropertyRow.h"
#include "Input/Reply.h"
#include "IPropertyUtilities.h"
#include "Misc/CoreDelegates.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "PropertyHandle.h"
#include "ScopedTransaction.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SBoxPanel.h"

#define LOCTEXT_NAMESPACE "MHCompositeActorDetails"

namespace UE::MimirComposite
{
namespace
{

bool GCompositeActorDetailsRegistered = false;

FText SeedTransactionTitle(const EMHCompositeSeedTarget Target)
{
    return Target == EMHCompositeSeedTarget::Layout
        ? LOCTEXT("GenerateLayoutSeedTransaction", "Generate MH Composite Layout Seed")
        : LOCTEXT("GenerateAppearanceSeedTransaction", "Generate MH Composite Appearance Seed");
}

class FMHCompositeActorDetails final : public IDetailCustomization
{
public:
    static TSharedRef<IDetailCustomization> MakeInstance()
    {
        return MakeShared<FMHCompositeActorDetails>();
    }

    virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override
    {
        Actors.Reset();
        for (const TWeakObjectPtr<AMHCompositeActor>& Actor :
             DetailBuilder.GetObjectsOfTypeBeingCustomized<AMHCompositeActor>())
        {
            if (Actor.IsValid())
            {
                Actors.Add(Actor);
            }
        }
        PropertyUtilities = DetailBuilder.GetPropertyUtilities();

        CustomizeSeedRow(DetailBuilder, TEXT("Seed"), EMHCompositeSeedTarget::Layout);
        CustomizeSeedRow(DetailBuilder, TEXT("AppearanceSeed"), EMHCompositeSeedTarget::Appearance);
    }

private:
    void CustomizeSeedRow(
        IDetailLayoutBuilder& DetailBuilder,
        const FName PropertyName,
        const EMHCompositeSeedTarget Target)
    {
        const TSharedRef<IPropertyHandle> Property =
            DetailBuilder.GetProperty(PropertyName, AMHCompositeActor::StaticClass());
        IDetailPropertyRow* Row = DetailBuilder.EditDefaultProperty(Property);
        if (!Property->IsValidHandle() || Row == nullptr)
        {
            return;
        }

        const FText ToolTip = Target == EMHCompositeSeedTarget::Layout
            ? LOCTEXT("GenerateLayoutSeedToolTip", "Generate a new layout seed for every selected MH Composite actor.")
            : LOCTEXT("GenerateAppearanceSeedToolTip", "Generate a new appearance seed for every selected MH Composite actor.");
        Row->CustomWidget()
        .NameContent()
        [
            Property->CreatePropertyNameWidget()
        ]
        .ValueContent()
        .MinDesiredWidth(260.0f)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot()
            .FillWidth(1.0f)
            [
                Property->CreatePropertyValueWidget()
            ]
            + SHorizontalBox::Slot()
            .AutoWidth()
            .Padding(6.0f, 0.0f, 0.0f, 0.0f)
            [
                SNew(SButton)
                .Text(LOCTEXT("GenerateSeedButton", "Generate"))
                .ToolTipText(ToolTip)
                .IsEnabled(this, &FMHCompositeActorDetails::CanGenerate)
                .OnClicked(this, &FMHCompositeActorDetails::Generate, Target)
            ]
        ];
    }

    bool CanGenerate() const
    {
        return !Actors.IsEmpty() &&
            !Actors.ContainsByPredicate([](const TWeakObjectPtr<AMHCompositeActor>& Actor)
            {
                return !Actor.IsValid() || Actor->IsPlacementEditMode();
            });
    }

    FReply Generate(const EMHCompositeSeedTarget Target)
    {
        FString Error;
        if (MHGenerateCompositeSeedsForDetails(Actors, Target, Error))
        {
            if (const TSharedPtr<IPropertyUtilities> Utilities = PropertyUtilities.Pin())
            {
                Utilities->RequestRefresh();
            }
        }
        return FReply::Handled();
    }

    TArray<TWeakObjectPtr<AMHCompositeActor>> Actors;
    TWeakPtr<IPropertyUtilities> PropertyUtilities;
};

} // namespace

bool MHGenerateCompositeSeedsForDetails(
    const TConstArrayView<TWeakObjectPtr<AMHCompositeActor>> Actors,
    const EMHCompositeSeedTarget Target,
    FString& OutError)
{
    OutError.Reset();
    if (Actors.IsEmpty())
    {
        OutError = TEXT("No MH Composite actors are available in Details");
        return false;
    }

    TArray<AMHCompositeActor*> ResolvedActors;
    ResolvedActors.Reserve(Actors.Num());
    for (const TWeakObjectPtr<AMHCompositeActor>& WeakActor : Actors)
    {
        AMHCompositeActor* Actor = WeakActor.Get();
        if (Actor == nullptr)
        {
            OutError = TEXT("The MH Composite Details selection is no longer valid");
            return false;
        }
        if (Actor->IsPlacementEditMode())
        {
            OutError = FString::Printf(
                TEXT("%s is in Placement Edit Mode; finish or cancel Edit before generating seeds"),
                *Actor->GetPathName());
            return false;
        }
        ResolvedActors.AddUnique(Actor);
    }

    const FScopedTransaction Transaction(SeedTransactionTitle(Target));
    for (AMHCompositeActor* Actor : ResolvedActors)
    {
        Actor->Modify();
        if (Target == EMHCompositeSeedTarget::Layout)
        {
            Actor->Reseed();
        }
        else
        {
            Actor->ReseedAppearance();
        }

        if (!Actor->GetLastPlacementError().IsEmpty())
        {
            OutError += FString::Printf(
                TEXT("%s: %s\n"),
                *Actor->GetPathName(),
                *Actor->GetLastPlacementError());
        }
    }
    if (GEditor != nullptr)
    {
        GEditor->RedrawLevelEditingViewports();
    }
    return true;
}

void MHRegisterCompositeActorDetails()
{
    if (GCompositeActorDetailsRegistered || IsRunningCommandlet())
    {
        return;
    }
    FPropertyEditorModule& PropertyEditor =
        FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
    PropertyEditor.RegisterCustomClassLayout(
        AMHCompositeActor::StaticClass()->GetFName(),
        FOnGetDetailCustomizationInstance::CreateStatic(&FMHCompositeActorDetails::MakeInstance));
    PropertyEditor.NotifyCustomizationModuleChanged();
    GCompositeActorDetailsRegistered = true;
}

void MHUnregisterCompositeActorDetails()
{
    if (!GCompositeActorDetailsRegistered)
    {
        return;
    }
    if (FPropertyEditorModule* PropertyEditor =
            FModuleManager::GetModulePtr<FPropertyEditorModule>(TEXT("PropertyEditor")))
    {
        PropertyEditor->UnregisterCustomClassLayout(AMHCompositeActor::StaticClass()->GetFName());
        if (!IsEngineExitRequested())
        {
            PropertyEditor->NotifyCustomizationModuleChanged();
        }
    }
    GCompositeActorDetailsRegistered = false;
}

} // namespace UE::MimirComposite

#undef LOCTEXT_NAMESPACE
