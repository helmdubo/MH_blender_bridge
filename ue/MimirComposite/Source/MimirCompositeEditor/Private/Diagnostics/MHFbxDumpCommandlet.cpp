#include "Diagnostics/MHFbxDumpCommandlet.h"

#include "Diagnostics/MHReaderOutputPath.h"
#include "Dom/JsonObject.h"
#include "Geometry/MHFbxSceneTranslator.h"
#include "Geometry/MHSceneIR.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Settings/MHCompositeSettings.h"
#include "Source/MHSourceResolver.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MHFbxDumpCommandlet)

DEFINE_LOG_CATEGORY_STATIC(LogMHFbxDump, Display, All);

using namespace UE::MimirComposite;

namespace
{

const TCHAR* AttributeLabel(const EMHSceneNodeAttribute Attribute)
{
    switch (Attribute)
    {
    case EMHSceneNodeAttribute::Mesh: return TEXT("mesh");
    case EMHSceneNodeAttribute::Null: return TEXT("null");
    case EMHSceneNodeAttribute::Unsupported:
    default: return TEXT("unsupported");
    }
}

const TCHAR* FbxDumpNodeKindLabel(const EMHSceneNodeKind Kind)
{
    switch (Kind)
    {
    case EMHSceneNodeKind::Render: return TEXT("render");
    case EMHSceneNodeKind::Collision: return TEXT("collision");
    case EMHSceneNodeKind::Socket: return TEXT("socket");
    case EMHSceneNodeKind::Group: return TEXT("group");
    case EMHSceneNodeKind::Unclassified:
    default: return TEXT("unclassified");
    }
}

const TCHAR* CollisionLabel(const EMHSceneCollisionMode Mode)
{
    switch (Mode)
    {
    case EMHSceneCollisionMode::PhysicsOnly: return TEXT("physics_only");
    case EMHSceneCollisionMode::QueryOnly: return TEXT("query_only");
    case EMHSceneCollisionMode::QueryAndPhysics: return TEXT("query_and_physics");
    case EMHSceneCollisionMode::None:
    default: return TEXT("none");
    }
}

TArray<TSharedPtr<FJsonValue>> NumberArray(const FVector3f& Value)
{
    return {
        MakeShared<FJsonValueNumber>(Value.X),
        MakeShared<FJsonValueNumber>(Value.Y),
        MakeShared<FJsonValueNumber>(Value.Z)};
}

TArray<TSharedPtr<FJsonValue>> NumberArray(const FVector2f& Value)
{
    return {
        MakeShared<FJsonValueNumber>(Value.X),
        MakeShared<FJsonValueNumber>(Value.Y)};
}

TArray<TSharedPtr<FJsonValue>> StringArray(const TArray<FString>& Values)
{
    TArray<TSharedPtr<FJsonValue>> Result;
    Result.Reserve(Values.Num());
    for (const FString& Value : Values)
    {
        Result.Add(MakeShared<FJsonValueString>(Value));
    }
    return Result;
}

TSharedRef<FJsonObject> TransformObject(const FTransform& Transform)
{
    TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetArrayField(TEXT("translation_cm"), NumberArray(FVector3f(Transform.GetTranslation())));
    FQuat Rotation = Transform.GetRotation();
    Rotation.Normalize();
    Result->SetArrayField(TEXT("rotation_quat"), {
        MakeShared<FJsonValueNumber>(Rotation.X),
        MakeShared<FJsonValueNumber>(Rotation.Y),
        MakeShared<FJsonValueNumber>(Rotation.Z),
        MakeShared<FJsonValueNumber>(Rotation.W)});
    Result->SetArrayField(TEXT("scale"), NumberArray(FVector3f(Transform.GetScale3D())));
    return Result;
}

bool BuildDump(const FMHSceneIR& Scene, const bool bFull, FString& OutJson)
{
    TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("tag"), TEXT("mh.fbxdump:4"));
    Root->SetStringField(TEXT("resource"), Scene.ResourceName);
    Root->SetBoolField(TEXT("explicit_lods"), Scene.bUsesExplicitLODs);
    TArray<TSharedPtr<FJsonValue>> LODs;
    for (const int32 Level : Scene.LODLevels)
    {
        LODs.Add(MakeShared<FJsonValueNumber>(Level));
    }
    Root->SetArrayField(TEXT("lods"), LODs);
    Root->SetArrayField(TEXT("materials"), StringArray(Scene.MaterialNames));

    TArray<TSharedPtr<FJsonValue>> Nodes;
    Nodes.Reserve(Scene.Nodes.Num());
    for (int32 NodeIndex = 0; NodeIndex < Scene.Nodes.Num(); ++NodeIndex)
    {
        const FMHSceneIRNode& Node = Scene.Nodes[NodeIndex];
        TSharedRef<FJsonObject> JsonNode = MakeShared<FJsonObject>();
        JsonNode->SetNumberField(TEXT("index"), NodeIndex);
        JsonNode->SetStringField(TEXT("name"), Node.Name);
        JsonNode->SetStringField(TEXT("attribute"), AttributeLabel(Node.Attribute));
        JsonNode->SetStringField(TEXT("kind"), FbxDumpNodeKindLabel(Node.Kind));
        JsonNode->SetNumberField(TEXT("parent"), Node.ParentIndex);
        JsonNode->SetObjectField(TEXT("global_transform"), TransformObject(Node.GlobalTransform));
        if (Node.Attribute == EMHSceneNodeAttribute::Mesh)
        {
            JsonNode->SetBoolField(TEXT("reverse_winding"), Node.bReverseWinding);
        }
        if (Node.Kind == EMHSceneNodeKind::Render)
        {
            JsonNode->SetNumberField(TEXT("lod"), Node.LODLevel);
        }
        if (Node.Kind == EMHSceneNodeKind::Collision)
        {
            JsonNode->SetStringField(TEXT("collision_enabled"), CollisionLabel(Node.CollisionMode));
        }
        if (Node.Kind == EMHSceneNodeKind::Socket)
        {
            JsonNode->SetStringField(TEXT("socket_name"), Node.SocketName);
        }
        JsonNode->SetArrayField(TEXT("material_slots"), StringArray(Node.MaterialSlots));
        if (Node.Geometry.IsSet())
        {
            const FMHSceneGeometry& Geometry = Node.Geometry.GetValue();
            TSharedRef<FJsonObject> JsonGeometry = MakeShared<FJsonObject>();
            JsonGeometry->SetNumberField(TEXT("vertex_count"), Geometry.Positions.Num());
            JsonGeometry->SetNumberField(TEXT("triangle_count"), Geometry.Triangles.Num());
            if (bFull)
            {
                TArray<TSharedPtr<FJsonValue>> Positions;
                Positions.Reserve(Geometry.Positions.Num());
                for (const FVector3f& Position : Geometry.Positions)
                {
                    Positions.Add(MakeShared<FJsonValueArray>(NumberArray(Position)));
                }
                JsonGeometry->SetArrayField(TEXT("positions"), Positions);

                TArray<TSharedPtr<FJsonValue>> Triangles;
                Triangles.Reserve(Geometry.Triangles.Num());
                for (const FMHSceneTriangle& Triangle : Geometry.Triangles)
                {
                    TSharedRef<FJsonObject> JsonTriangle = MakeShared<FJsonObject>();
                    JsonTriangle->SetArrayField(TEXT("position_indices"), {
                        MakeShared<FJsonValueNumber>(Triangle.PositionIndices[0]),
                        MakeShared<FJsonValueNumber>(Triangle.PositionIndices[1]),
                        MakeShared<FJsonValueNumber>(Triangle.PositionIndices[2])});
                    JsonTriangle->SetNumberField(TEXT("material_slot"), Triangle.MaterialSlotIndex);
                    TArray<TSharedPtr<FJsonValue>> Normals;
                    TArray<TSharedPtr<FJsonValue>> UV0;
                    for (int32 Corner = 0; Corner < 3; ++Corner)
                    {
                        Normals.Add(MakeShared<FJsonValueArray>(NumberArray(Triangle.CornerNormals[Corner])));
                        UV0.Add(MakeShared<FJsonValueArray>(NumberArray(Triangle.CornerUV0[Corner])));
                    }
                    JsonTriangle->SetArrayField(TEXT("normals"), Normals);
                    JsonTriangle->SetArrayField(TEXT("uv0"), UV0);
                    Triangles.Add(MakeShared<FJsonValueObject>(JsonTriangle));
                }
                JsonGeometry->SetArrayField(TEXT("triangles"), Triangles);
            }
            JsonNode->SetObjectField(TEXT("geometry"), JsonGeometry);
        }
        Nodes.Add(MakeShared<FJsonValueObject>(JsonNode));
    }
    Root->SetArrayField(TEXT("nodes"), Nodes);

    OutJson.Reset();
    TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
        TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&OutJson);
    return FJsonSerializer::Serialize(Root, Writer);
}

bool WriteAtomicReport(const FString& Filename, const FString& Contents, FString& OutError)
{
    const FString Directory = FPaths::GetPath(Filename);
    if (!IFileManager::Get().MakeDirectory(*Directory, true))
    {
        OutError = FString::Printf(TEXT("cannot create report directory: %s"), *Directory);
        return false;
    }
    const FString Temporary = Filename + FString::Printf(TEXT(".tmp.%u"), FPlatformProcess::GetCurrentProcessId());
    if (!FFileHelper::SaveStringToFile(Contents, *Temporary, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        OutError = FString::Printf(TEXT("cannot write temporary report: %s"), *Temporary);
        return false;
    }
    FString ReadBack;
    if (!FFileHelper::LoadFileToString(ReadBack, *Temporary) || ReadBack != Contents)
    {
        IFileManager::Get().Delete(*Temporary);
        OutError = TEXT("temporary report read-back mismatch");
        return false;
    }
    if (!IFileManager::Get().Move(*Filename, *Temporary, true, false, false, false))
    {
        IFileManager::Get().Delete(*Temporary);
        OutError = FString::Printf(TEXT("cannot atomically replace report: %s"), *Filename);
        return false;
    }
    return true;
}

} // namespace

UMHFbxDumpCommandlet::UMHFbxDumpCommandlet(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
    ShowErrorCount = true;
    UseCommandletResultAsExitCode = true;
}

int32 UMHFbxDumpCommandlet::Main(const FString& Params)
{
    const TCHAR* Cursor = *Params;
    FString Filename = FParse::Token(Cursor, false);
    FParse::Value(*Params, TEXT("file="), Filename);
    if (Filename.IsEmpty() || Filename.StartsWith(TEXT("-")))
    {
        const TCHAR* CommandCursor = FCommandLine::Get();
        while (*CommandCursor != TEXT('\0'))
        {
            FString Token = FParse::Token(CommandCursor, false);
            if (Token.EndsWith(TEXT(".mesh.fbx"), ESearchCase::IgnoreCase))
            {
                Filename = MoveTemp(Token);
                break;
            }
        }
    }
    if (Filename.IsEmpty() || Filename.StartsWith(TEXT("-")))
    {
        UE_LOG(LogMHFbxDump, Error, TEXT("Usage: -run=MHFbxDump <file.mesh.fbx> [-file=<file.mesh.fbx>] [-full] [-report=<path>]"));
        return 2;
    }
    Filename = FPaths::ConvertRelativePathToFull(Filename);
    FPaths::NormalizeFilename(Filename);

    FMHResourceKey Key;
    FString Error;
    if (!MHResourceKeyFromSourceFile(Filename, Key, Error) || Key.Kind != EMHResourceKind::StaticMesh)
    {
        UE_LOG(LogMHFbxDump, Error, TEXT("%s"), Error.IsEmpty() ? TEXT("MH_E_INVALID_RESOURCE_SOURCE: expected *.mesh.fbx") : *Error);
        return 1;
    }
    TArray<uint8> Bytes;
    if (!FFileHelper::LoadFileToArray(Bytes, *Filename))
    {
        UE_LOG(LogMHFbxDump, Error, TEXT("MH_E_FBX_TRANSPORT_FAILED: cannot read '%s'"), *Filename);
        return 1;
    }

    FMHSceneIR Scene;
    FMHFbxSceneTranslator Translator;
    if (!Translator.Translate(Key.LogicalName, Bytes, Scene, Error))
    {
        UE_LOG(LogMHFbxDump, Error, TEXT("%s"), *Error);
        return 1;
    }
    FString Json;
    if (!BuildDump(Scene, FParse::Param(*Params, TEXT("full")), Json))
    {
        UE_LOG(LogMHFbxDump, Error, TEXT("MH_E_FBX_TRANSPORT_FAILED: cannot serialize SceneIR dump"));
        return 1;
    }

    FString RequestedReport;
    FParse::Value(*Params, TEXT("report="), RequestedReport);
    if (!RequestedReport.IsEmpty())
    {
        const UMHCompositeSettings* Settings = GetDefault<UMHCompositeSettings>();
        const FString SourceRoot = Settings != nullptr && !Settings->GetSourceRootPath().IsEmpty()
            ? Settings->GetSourceRootPath()
            : FPaths::GetPath(Filename);
        FString ReportPath;
        if (!MHResolveReaderOutputPath(SourceRoot, RequestedReport, ReportPath, Error) ||
            !WriteAtomicReport(ReportPath, Json + TEXT("\n"), Error))
        {
            UE_LOG(LogMHFbxDump, Error, TEXT("MH_E_SOURCE_INDEX_INVALID: %s"), *Error);
            return 1;
        }
        UE_LOG(LogMHFbxDump, Display, TEXT("mh.fbxdump:4 report: %s"), *ReportPath);
    }
    else
    {
        UE_LOG(LogMHFbxDump, Display, TEXT("%s"), *Json);
    }
    return 0;
}
