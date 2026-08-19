#include "UI/MHEditorMenus.h"

#include "DesktopPlatformModule.h"
#include "Diagnostics/MHCompositeDumpUtil.h"
#include "Diagnostics/MHFbxDump.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Framework/Application/SlateApplication.h"
#include "Logging/MessageLog.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Source/MHFbxPassport.h"
#include "ToolMenus.h"

namespace MH::EditorMenus
{
namespace
{

const FName MimirLogName(TEXT("Mimir"));

bool PickFile(const FString& Title, const FString& FileTypes, FString& OutFile)
{
    IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
    if (DesktopPlatform == nullptr || !FSlateApplication::IsInitialized())
    {
        return false;
    }
    const void* ParentWindowHandle =
        FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr);
    TArray<FString> Files;
    if (!DesktopPlatform->OpenFileDialog(
            ParentWindowHandle, Title, FPaths::ProjectDir(), FString(), FileTypes,
            EFileDialogFlags::None, Files) ||
        Files.Num() != 1)
    {
        return false;
    }
    OutFile = FPaths::ConvertRelativePathToFull(Files[0]);
    return true;
}

bool PickDirectory(const FString& Title, FString& OutDirectory)
{
    IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
    if (DesktopPlatform == nullptr || !FSlateApplication::IsInitialized())
    {
        return false;
    }
    const void* ParentWindowHandle =
        FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr);
    FString Directory;
    if (!DesktopPlatform->OpenDirectoryDialog(ParentWindowHandle, Title, FPaths::ProjectDir(), Directory))
    {
        return false;
    }
    OutDirectory = FPaths::ConvertRelativePathToFull(Directory);
    return true;
}

FString DumpOutputDir()
{
    return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MimirDumps"));
}

bool WriteDumpFile(const FString& Json, const FString& OutPath, FMessageLog& Log)
{
    if (!FFileHelper::SaveStringToFile(
            Json + TEXT("\n"),
            *OutPath,
            FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        Log.Error(FText::FromString(FString::Printf(TEXT("cannot write %s"), *OutPath)));
        return false;
    }
    Log.Info(FText::FromString(FString::Printf(TEXT("dump written: %s"), *OutPath)));
    return true;
}

// mh.fbxdump emits every number as a JSON number string, so integers are read
// back through TryGetNumber instead of being cast from a double literal.
int64 FbxDumpTreeIntegerField(
    const TSharedPtr<FJsonObject>& Object,
    const TCHAR* FieldName,
    const int64 Fallback)
{
    int64 Value = Fallback;
    return Object.IsValid() && Object->TryGetNumberField(FieldName, Value) ? Value : Fallback;
}

bool FbxDumpTreeBoolField(
    const TSharedPtr<FJsonObject>& Object,
    const TCHAR* FieldName,
    const bool bFallback)
{
    bool bValue = bFallback;
    return Object.IsValid() && Object->TryGetBoolField(FieldName, bValue) ? bValue : bFallback;
}

FString FbxDumpTreeAttributeLabel(const TSharedPtr<FJsonObject>& NodeObject)
{
    const TArray<TSharedPtr<FJsonValue>>* AttributeTypes = nullptr;
    if (!NodeObject->TryGetArrayField(TEXT("attribute_types"), AttributeTypes) ||
        AttributeTypes == nullptr)
    {
        return TEXT("none");
    }
    TArray<FString> TypeNames;
    for (const TSharedPtr<FJsonValue>& AttributeType : *AttributeTypes)
    {
        FString TypeName;
        if (AttributeType.IsValid() && AttributeType->TryGetString(TypeName))
        {
            TypeNames.Add(MoveTemp(TypeName));
        }
    }
    return TypeNames.Num() > 0 ? FString::Join(TypeNames, TEXT("+")) : FString(TEXT("none"));
}

FString FbxDumpTreeLodLabel(const TSharedPtr<FJsonObject>& NodeObject)
{
    const TSharedPtr<FJsonObject>* LodObject = nullptr;
    if (!NodeObject->TryGetObjectField(TEXT("mh_lod_level"), LodObject) ||
        LodObject == nullptr || !LodObject->IsValid())
    {
        return FString();
    }
    // Nodes without an authored mh_lod_level property report explicit=false and
    // carry no meaningful level, so they are left out of the line entirely.
    if (!FbxDumpTreeBoolField(*LodObject, TEXT("explicit"), false))
    {
        return FString();
    }
    if (!FbxDumpTreeBoolField(*LodObject, TEXT("valid"), false))
    {
        return TEXT(" lod=invalid");
    }
    return FString::Printf(
        TEXT(" lod=%s"),
        *LexToString(FbxDumpTreeIntegerField(*LodObject, TEXT("effective"), 0)));
}

FString FbxDumpTreeMeshLabel(const TSharedPtr<FJsonObject>& NodeObject)
{
    const TSharedPtr<FJsonObject>* MeshObject = nullptr;
    if (!NodeObject->TryGetObjectField(TEXT("mesh"), MeshObject) ||
        MeshObject == nullptr || !MeshObject->IsValid())
    {
        return FString();
    }
    FString Label = FString::Printf(
        TEXT(" cp=%s poly=%s"),
        *LexToString(FbxDumpTreeIntegerField(*MeshObject, TEXT("control_point_count"), 0)),
        *LexToString(FbxDumpTreeIntegerField(*MeshObject, TEXT("polygon_count"), 0)));

    const TArray<TSharedPtr<FJsonValue>>* MaterialSlots = nullptr;
    if ((*MeshObject)->TryGetArrayField(TEXT("material_slots"), MaterialSlots) &&
        MaterialSlots != nullptr)
    {
        TArray<FString> SlotNames;
        for (const TSharedPtr<FJsonValue>& SlotValue : *MaterialSlots)
        {
            if (!SlotValue.IsValid() || SlotValue->Type != EJson::Object)
            {
                continue;
            }
            FString SlotName;
            if (!SlotValue->AsObject()->TryGetStringField(TEXT("name"), SlotName))
            {
                continue;
            }
            if (SlotName.IsEmpty())
            {
                SlotName = TEXT("<unnamed>");
            }
            SlotNames.Add(MoveTemp(SlotName));
        }
        if (SlotNames.Num() > 0)
        {
            Label += FString::Printf(TEXT(" mats=[%s]"), *FString::Join(SlotNames, TEXT(", ")));
        }
    }
    return Label;
}

FString FbxDumpTreeNodeLine(const TSharedPtr<FJsonObject>& NodeObject, const int32 Depth)
{
    const FString Indent = FString::ChrN(Depth * 2, TEXT(' '));
    if (!NodeObject.IsValid())
    {
        return FString::Printf(TEXT("%s- <malformed node>"), *Indent);
    }
    FString NodeName;
    if (!NodeObject->TryGetStringField(TEXT("name"), NodeName) || NodeName.IsEmpty())
    {
        NodeName = TEXT("<unnamed>");
    }
    FString Line = FString::Printf(
        TEXT("%s- [%s] %s"),
        *Indent,
        *FbxDumpTreeAttributeLabel(NodeObject),
        *NodeName);
    Line += FbxDumpTreeLodLabel(NodeObject);
    Line += FbxDumpTreeMeshLabel(NodeObject);
    if (FbxDumpTreeBoolField(NodeObject, TEXT("mh_fbx_passport_present"), false))
    {
        Line += TEXT(" [passport]");
    }
    return Line;
}

void LogFbxDumpTreeBranch(
    const TArray<TSharedPtr<FJsonObject>>& NodeObjects,
    const TArray<TArray<int32>>& ChildIndices,
    const int32 NodeIndex,
    const int32 Depth,
    FMessageLog& Log)
{
    Log.Info(FText::FromString(FbxDumpTreeNodeLine(NodeObjects[NodeIndex], Depth)));
    for (const int32 ChildIndex : ChildIndices[NodeIndex])
    {
        LogFbxDumpTreeBranch(NodeObjects, ChildIndices, ChildIndex, Depth + 1, Log);
    }
}

// The canonical JSON files stay the machine artifact; the same summary DOM is
// replayed here as an indented hierarchy so the message log is readable.
void LogFbxDumpNodeTree(const TSharedPtr<FJsonObject>& Root, FMessageLog& Log)
{
    const TArray<TSharedPtr<FJsonValue>>* NodeValues = nullptr;
    if (!Root.IsValid() || !Root->TryGetArrayField(TEXT("nodes"), NodeValues) ||
        NodeValues == nullptr)
    {
        return;
    }

    TArray<TSharedPtr<FJsonObject>> NodeObjects;
    NodeObjects.Reserve(NodeValues->Num());
    for (const TSharedPtr<FJsonValue>& NodeValue : *NodeValues)
    {
        NodeObjects.Add(
            NodeValue.IsValid() && NodeValue->Type == EJson::Object
                ? NodeValue->AsObject()
                : TSharedPtr<FJsonObject>());
    }

    // BuildNode() appends a node before its children, so a parent always has a
    // lower index than its children and scene roots report parent_index -1.
    TArray<TArray<int32>> ChildIndices;
    ChildIndices.SetNum(NodeObjects.Num());
    TArray<int32> RootIndices;
    for (int32 NodeIndex = 0; NodeIndex < NodeObjects.Num(); ++NodeIndex)
    {
        const int32 ParentIndex = static_cast<int32>(
            FbxDumpTreeIntegerField(NodeObjects[NodeIndex], TEXT("parent_index"), -1));
        if (ParentIndex >= 0 && ParentIndex < NodeIndex)
        {
            ChildIndices[ParentIndex].Add(NodeIndex);
        }
        else
        {
            RootIndices.Add(NodeIndex);
        }
    }

    Log.Info(FText::FromString(FString::Printf(
        TEXT("model hierarchy (%d nodes):"),
        NodeObjects.Num())));
    for (const int32 RootIndex : RootIndices)
    {
        LogFbxDumpTreeBranch(NodeObjects, ChildIndices, RootIndex, 1, Log);
    }
}

void OnDumpFbx()
{
    FString FilePath;
    if (!PickFile(
            TEXT("Mimir FBX Dump"),
            TEXT("FBX payload (*.fbx)|*.fbx|All files (*.*)|*.*"),
            FilePath))
    {
        return;
    }

    FMessageLog Log(MimirLogName);
    Log.NewPage(FText::FromString(FString::Printf(TEXT("FBX Dump: %s"), *FPaths::GetCleanFilename(FilePath))));
    Log.Info(FText::FromString(FString::Printf(TEXT("mh.fbxdump for %s"), *FilePath)));

    const FString BaseName = FPaths::GetBaseFilename(FilePath);
    TSharedPtr<FJsonObject> SummaryRoot;
    bool bAnyError = false;
    for (const bool bFull : {false, true})
    {
        TSharedPtr<FJsonObject> Root;
        FString Error;
        if (!MH::FbxDump::BuildDom(FilePath, bFull, Root, Error))
        {
            Log.Error(FText::FromString(Error));
            bAnyError = true;
            break;
        }
        if (!bFull)
        {
            SummaryRoot = Root;
        }
        FString Json;
        if (!MH::FbxDump::SerializeCanonical(Root, Json, Error))
        {
            Log.Error(FText::FromString(Error));
            bAnyError = true;
            break;
        }
        const FString OutPath = FPaths::Combine(
            DumpOutputDir(),
            FString::Printf(TEXT("%s.%s.json"), *BaseName, bFull ? TEXT("full") : TEXT("summary")));
        bAnyError |= !WriteDumpFile(Json, OutPath, Log);
    }

    LogFbxDumpNodeTree(SummaryRoot, Log);

    // Field visibility of the mandatory v2 identity: a legacy FBX without the
    // Carrier B passport is expected to be quarantined by the production
    // reader, so the outcome is reported either way.
    UE::MimirComposite::FMHFbxPassport Passport;
    FString PassportError;
    if (UE::MimirComposite::MHReadFbxPassport(FilePath, Passport, PassportError))
    {
        Log.Info(FText::FromString(FString::Printf(
            TEXT("passport: uid=%s name=%s lod_levels=%d slots=%d copies=%d"),
            *Passport.ResourceUid,
            *Passport.Name,
            Passport.LodLevels.Num(),
            Passport.MaterialSlots.Num(),
            Passport.CopyCount)));
    }
    else
    {
        Log.Warning(FText::FromString(FString::Printf(
            TEXT("no valid Carrier B passport (production reader would quarantine): %s"),
            *PassportError)));
    }

    Log.Open(bAnyError ? EMessageSeverity::Error : EMessageSeverity::Info, true);
}

void OnDumpComposite()
{
    FString FilePath;
    if (!PickFile(
            TEXT("Mimir Composite Dump"),
            TEXT("Mimir composite (*.composite)|*.composite"),
            FilePath))
    {
        return;
    }
    FString SourceRoot;
    PickDirectory(
        TEXT("Select source_root for recursive resolve (Cancel = single file dump)"),
        SourceRoot);

    MH::CompositeDump::FDumpOutput Output;
    const bool bClean = MH::CompositeDump::BuildDump(FilePath, SourceRoot, Output);

    FMessageLog Log(MimirLogName);
    Log.NewPage(FText::FromString(FString::Printf(
        TEXT("Composite Dump: %s"),
        *FPaths::GetCleanFilename(FilePath))));
    for (const FString& Line : Output.Lines)
    {
        if (!Line.IsEmpty())
        {
            Log.Info(FText::FromString(Line));
        }
    }
    for (const FString& Warning : Output.Warnings)
    {
        Log.Warning(FText::FromString(Warning));
    }
    for (const FString& Error : Output.Errors)
    {
        Log.Error(FText::FromString(Error));
    }
    Log.Open(bClean ? EMessageSeverity::Info : EMessageSeverity::Error, true);
}

} // namespace

void Populate()
{
    UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Tools");
    if (Menu == nullptr)
    {
        return;
    }
    FToolMenuSection& Section = Menu->AddSection("MimirComposite", INVTEXT("Mimir"));
    Section.AddMenuEntry(
        "MHFbxDump",
        INVTEXT("Mimir FBX Dump..."),
        INVTEXT("Read one FBX with the raw mh.fbxdump reader, report the Carrier B passport and write summary/full JSON dumps to Saved/MimirDumps."),
        FSlateIcon(),
        FUIAction(FExecuteAction::CreateStatic(&OnDumpFbx)));
    Section.AddMenuEntry(
        "MHCompositeDump",
        INVTEXT("Mimir Composite Dump..."),
        INVTEXT("Print the node hierarchy of one mh.composite v2 payload to the Mimir message log, optionally resolving dependencies under a source root."),
        FSlateIcon(),
        FUIAction(FExecuteAction::CreateStatic(&OnDumpComposite)));
}

} // namespace MH::EditorMenus
