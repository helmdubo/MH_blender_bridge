#include "Material/MHMaterialAdoptDialog.h"

#include "Framework/Application/SlateApplication.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Settings/MHCompositeSettings.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

namespace UE::MimirComposite
{
namespace
{

class SMHMaterialAdoptDialog final : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SMHMaterialAdoptDialog) {}
        SLATE_ARGUMENT(UMaterialInstanceConstant*, Material)
        SLATE_ARGUMENT(FString, SourceRoot)
        SLATE_ARGUMENT(const UMHCompositeSettings*, Settings)
    SLATE_END_ARGS()

    void Construct(const FArguments& Args)
    {
        Material = Args._Material;
        SourceRoot = Args._SourceRoot;
        Settings = Args._Settings;
        const FString SuggestedName = Material != nullptr && MHIsCanonicalMaterialToken(Material->GetName())
            ? Material->GetName()
            : FString();

        ChildSlot
        [
            SNew(SBorder)
            .Padding(12.0f)
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
                [
                    SNew(STextBlock).Text(INVTEXT("Source folder under source_root"))
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
                [
                    SAssignNew(FolderBox, SEditableTextBox).Text(FText::FromString(SourceRoot))
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
                [
                    SNew(STextBlock).Text(INVTEXT("Material logical name ([a-z0-9_]+)"))
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
                [
                    SAssignNew(NameBox, SEditableTextBox).Text(FText::FromString(SuggestedName))
                ]
                + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
                [
                    SAssignNew(ErrorText, STextBlock).ColorAndOpacity(FLinearColor::Red)
                ]
                + SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right)
                [
                    SNew(SHorizontalBox)
                    + SHorizontalBox::Slot().AutoWidth().Padding(0.0f, 0.0f, 8.0f, 0.0f)
                    [
                        SNew(SButton).Text(INVTEXT("Cancel")).OnClicked(this, &SMHMaterialAdoptDialog::OnCancel)
                    ]
                    + SHorizontalBox::Slot().AutoWidth()
                    [
                        SNew(SButton).Text(INVTEXT("Adopt and Publish")).OnClicked(this, &SMHMaterialAdoptDialog::OnPublish)
                    ]
                ]
            ]
        ];
    }

    void SetWindow(const TSharedRef<SWindow>& InWindow) { Window = InWindow; }
    const FMHMaterialOperationResult& GetResult() const { return Result; }

private:
    FReply OnCancel()
    {
        if (const TSharedPtr<SWindow> Pinned = Window.Pin())
        {
            Pinned->RequestDestroyWindow();
        }
        return FReply::Handled();
    }

    FReply OnPublish()
    {
        FMHMaterialAdoptTarget Target;
        Target.Folder = FolderBox->GetText().ToString();
        Target.LogicalName = NameBox->GetText().ToString();
        Result = MHPublishMaterialV4(*Material, SourceRoot, *Settings, &Target);
        if (!Result.Succeeded())
        {
            ErrorText->SetText(FText::FromString(Result.Error));
            return FReply::Handled();
        }
        if (const TSharedPtr<SWindow> Pinned = Window.Pin())
        {
            Pinned->RequestDestroyWindow();
        }
        return FReply::Handled();
    }

    UMaterialInstanceConstant* Material = nullptr;
    FString SourceRoot;
    const UMHCompositeSettings* Settings = nullptr;
    TSharedPtr<SEditableTextBox> FolderBox;
    TSharedPtr<SEditableTextBox> NameBox;
    TSharedPtr<STextBlock> ErrorText;
    TWeakPtr<SWindow> Window;
    FMHMaterialOperationResult Result;
};

} // namespace

FMHMaterialOperationResult MHShowMaterialAdoptDialog(
    UMaterialInstanceConstant& Material,
    const FString& SourceRoot,
    const UMHCompositeSettings& Settings)
{
    FMHMaterialOperationResult Result;
    if (!FSlateApplication::IsInitialized() || IsRunningCommandlet())
    {
        Result.Error = TEXT("MH_E_MATERIAL_NOT_ROUNDTRIPPABLE: Adopt dialog is unavailable in this editor context");
        return Result;
    }
    const TSharedRef<SMHMaterialAdoptDialog> Dialog = SNew(SMHMaterialAdoptDialog)
        .Material(&Material)
        .SourceRoot(SourceRoot)
        .Settings(&Settings);
    const TSharedRef<SWindow> Window = SNew(SWindow)
        .Title(INVTEXT("Adopt Mimir Material"))
        .ClientSize(FVector2D(620.0f, 220.0f))
        .SupportsMaximize(false)
        .SupportsMinimize(false)
        [
            Dialog
        ];
    Dialog->SetWindow(Window);
    FSlateApplication::Get().AddModalWindow(Window, nullptr);
    return Dialog->GetResult();
}

} // namespace UE::MimirComposite
