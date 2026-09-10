#include "Wind/MHPivotWindMaterial.h"

#include "Engine/Texture2D.h"
#include "Engine/VolumeTexture.h"
#include "Wind/MHDagorWindNoise.h"
#include "Interfaces/IPluginManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionCollectionParameter.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionLocalPosition.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureObject.h"
#include "Materials/MaterialExpressionTextureObjectParameter.h"
#include "Materials/MaterialExpressionTime.h"
#include "Materials/MaterialExpressionTransform.h"
#include "Materials/MaterialExpressionTransformPosition.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "Materials/MaterialExpressionVertexInterpolator.h"
#include "Materials/MaterialExpressionVertexNormalWS.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialParameterCollection.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace UE::MimirComposite
{
namespace
{
constexpr const TCHAR* AttachmentMarker = TEXT("MH Pivot Wind v1");
constexpr const TCHAR* AlgorithmMarker = TEXT("MH Pivot Wind algorithm v2");

struct FGraph
{
    UObject& Owner;
    FMaterialExpressionCollection& Expressions;
    int32 Row = 0;

    template<typename T> T* Add()
    {
        T* Node = NewObject<T>(&Owner, NAME_None, RF_Transactional);
        Node->Material = Cast<UMaterial>(&Owner);
        Node->Function = Cast<UMaterialFunction>(&Owner);
        Node->MaterialExpressionEditorX = -1000;
        Node->MaterialExpressionEditorY = Row++ * 100;
        Node->MaterialExpressionGuid = FGuid::NewGuid();
        Expressions.AddExpression(Node);
        return Node;
    }

    UMaterialExpressionScalarParameter* Scalar(const TCHAR* Name, float Value)
    {
        auto* Node = Add<UMaterialExpressionScalarParameter>();
        Node->ParameterName = Name;
        Node->DefaultValue = Value;
        Node->Group = TEXT("Plant wind response");
        Node->ExpressionGUID = FGuid::NewGuid();
        return Node;
    }

    UMaterialExpressionVectorParameter* Vector(const TCHAR* Name, FLinearColor Value)
    {
        auto* Node = Add<UMaterialExpressionVectorParameter>();
        Node->ParameterName = Name;
        Node->DefaultValue = Value;
        Node->Group = TEXT("Plant wind response");
        Node->ExpressionGUID = FGuid::NewGuid();
        return Node;
    }

    UMaterialExpressionConstant3Vector* Constant(FLinearColor Value)
    {
        auto* Node = Add<UMaterialExpressionConstant3Vector>();
        Node->Constant = Value;
        return Node;
    }

    UMaterialExpressionTransform* Transform(
        UMaterialExpression* Input, EMaterialVectorCoordTransformSource From,
        EMaterialVectorCoordTransform To)
    {
        auto* Node = Add<UMaterialExpressionTransform>();
        Node->Input.Connect(0, Input);
        Node->TransformSourceType = From;
        Node->TransformType = To;
        return Node;
    }
};

void Input(UMaterialExpressionCustom& Node, const TCHAR* Name, UMaterialExpression* Value, int32 Output = 0)
{
    FCustomInput& Item = Node.Inputs.AddDefaulted_GetRef();
    Item.InputName = Name;
    Item.Input.Connect(Output, Value);
}

bool LoadPivotWindCode(FString& OutCode, const bool bLegacy = false)
{
    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("MimirComposite"));
    return Plugin.IsValid() && FFileHelper::LoadFileToString(OutCode,
        *FPaths::Combine(Plugin->GetBaseDir(), bLegacy
            ? TEXT("Resources/MaterialFunctions/Legacy/MHPivotWind_v1.hlsl")
            : TEXT("Resources/MaterialFunctions/MHPivotWind.hlsl")));
}

const FExpressionInput* CustomInput(
    const UMaterialExpressionCustom& Custom, const FName Name)
{
    const FCustomInput* Match = nullptr;
    for (const FCustomInput& Input : Custom.Inputs)
    {
        if (Input.InputName != Name)
        {
            continue;
        }
        if (Match != nullptr)
        {
            return nullptr;
        }
        Match = &Input;
    }
    return Match != nullptr ? &Match->Input : nullptr;
}

bool ConnectedTo(
    const FExpressionInput* Input, const UMaterialExpression* Expression,
    const int32 OutputIndex = 0)
{
    if (Input == nullptr || Expression == nullptr || Input->Expression != Expression ||
        Input->OutputIndex != OutputIndex) return false;
    FExpressionInput Expected;
    Expected.Connect(OutputIndex, const_cast<UMaterialExpression*>(Expression));
    return Input->Mask == Expected.Mask && Input->MaskR == Expected.MaskR &&
        Input->MaskG == Expected.MaskG && Input->MaskB == Expected.MaskB && Input->MaskA == Expected.MaskA;
}

bool InvalidGraph(const UObject& Asset, const TCHAR* Detail, FString& OutError)
{
    OutError = FString::Printf(
        TEXT("%s: %s; existing asset preserved"), *Asset.GetPathName(), Detail);
    return false;
}
}

bool MHBuildPivotWindFunction(
    UMaterialFunction& Function, UMaterialParameterCollection& Collection,
    UTexture2D& DefaultPosition, UTexture2D& DefaultDirection,
    UVolumeTexture& NoiseVolume, FString& OutError)
{
    OutError.Reset();
    FString Code;
    if (!LoadPivotWindCode(Code))
    {
        OutError = TEXT("MH pivot wind shader source is missing from the plugin Resources folder");
        return false;
    }
    if (!Collection.GetParameterId(TEXT("MH_WindDirectionSpeed")).IsValid() ||
        !Collection.GetParameterId(TEXT("MH_WindNoise")).IsValid() ||
        !Collection.GetParameterId(TEXT("MH_WindTree")).IsValid())
    {
        OutError = TEXT("MH wind parameter collection has no direction/noise/tree vectors");
        return false;
    }
    if (!Function.GetExpressions().IsEmpty())
    {
        OutError = TEXT("Build requires a new material function; existing functions are preserved");
        return false;
    }
    if (!MHValidateDagorWindNoise(NoiseVolume, OutError)) return false;
    FGraph Graph{Function, Function.GetExpressionCollection()};
    auto* Wind = Graph.Add<UMaterialExpressionCustom>();
    Wind->Description = AlgorithmMarker;
    Wind->Code = MoveTemp(Code);
    Wind->OutputType = CMOT_Float3;
    Wind->Inputs.Empty();
    FCustomOutput& Rotation = Wind->AdditionalOutputs.AddDefaulted_GetRef();
    Rotation.OutputName = TEXT("Rotation");
    Rotation.OutputType = CMOT_Float4;
    Wind->RebuildOutputs();

    for (const TPair<const TCHAR*, UTexture2D*>& Texture : {
        TPair<const TCHAR*, UTexture2D*>(TEXT("tex7"), &DefaultPosition),
        TPair<const TCHAR*, UTexture2D*>(TEXT("tex8"), &DefaultDirection)})
    {
        auto* Param = Graph.Add<UMaterialExpressionTextureObjectParameter>();
        Param->ParameterName = Texture.Key;
        Param->Texture = Texture.Value;
        Param->SamplerType = SAMPLERTYPE_LinearColor;
        Param->Group = TEXT("Pivot atlases");
        Param->ExpressionGUID = FGuid::NewGuid();
        Input(*Wind, Texture.Value == &DefaultPosition ? TEXT("PivotPos") : TEXT("PivotDir"), Param);
    }
    auto* UV = Graph.Add<UMaterialExpressionTextureCoordinate>();
    UV->CoordinateIndex = 1;
    Input(*Wind, TEXT("PivotUV"), UV);
    auto* Position = Graph.Add<UMaterialExpressionLocalPosition>();
    Position->LocalOrigin = ELocalPositionOrigin::InstancePreSkinning;
    Position->IncludedOffsets = EPositionIncludedOffsets::ExcludeOffsets;
    Input(*Wind, TEXT("LocalPosition"), Position);
    auto* Origin = Graph.Add<UMaterialExpressionTransformPosition>();
    Origin->TransformSourceType = TRANSFORMPOSSOURCE_Local;
    Origin->TransformType = TRANSFORMPOSSOURCE_World;
    Origin->Input.Connect(0, Graph.Constant(FLinearColor::Black));
    Input(*Wind, TEXT("OriginWS"), Origin);
    Input(*Wind, TEXT("BasisX"), Graph.Transform(Graph.Constant(FLinearColor(1,0,0)), TRANSFORMSOURCE_Local, TRANSFORM_World));
    Input(*Wind, TEXT("BasisY"), Graph.Transform(Graph.Constant(FLinearColor(0,1,0)), TRANSFORMSOURCE_Local, TRANSFORM_World));
    Input(*Wind, TEXT("BasisZ"), Graph.Transform(Graph.Constant(FLinearColor(0,0,1)), TRANSFORMSOURCE_Local, TRANSFORM_World));
    Input(*Wind, TEXT("VertexNormalWS"), Graph.Add<UMaterialExpressionVertexNormalWS>());
    Input(*Wind, TEXT("VertexColor"), Graph.Add<UMaterialExpressionVertexColor>());
    Input(*Wind, TEXT("TimeSeconds"), Graph.Add<UMaterialExpressionTime>());
    for (const auto& Names : {TPair<const TCHAR*,const TCHAR*>(TEXT("WindFlow"),TEXT("MH_WindDirectionSpeed")),
        TPair<const TCHAR*,const TCHAR*>(TEXT("WindNoise"),TEXT("MH_WindNoise")),
        TPair<const TCHAR*,const TCHAR*>(TEXT("TreeWind"),TEXT("MH_WindTree"))})
    {
        auto* Param = Graph.Add<UMaterialExpressionCollectionParameter>();
        Param->Collection = &Collection;
        Param->ParameterName = Names.Value;
        Param->ParameterId = Collection.GetParameterId(Param->ParameterName);
        Input(*Wind, Names.Key, Param);
    }
    Input(*Wind, TEXT("IsPivoted"), Graph.Scalar(TEXT("is_pivoted"), 0));
    Input(*Wind, TEXT("NoiseSpeedBase"), Graph.Scalar(TEXT("wind_noise_speed_base"), 0.1f));
    Input(*Wind, TEXT("NoiseSpeedLevel"), Graph.Scalar(TEXT("wind_noise_speed_level_mul"), 1.666f));
    Input(*Wind, TEXT("AngleBase"), Graph.Scalar(TEXT("wind_angle_rot_base"), 5));
    Input(*Wind, TEXT("AngleLevel"), Graph.Scalar(TEXT("wind_angle_rot_level_mul"), 5));
    Input(*Wind, TEXT("AngleLimits"), Graph.Vector(TEXT("wind_per_level_angle_rot_max"), FLinearColor(60,60,60,60)), 5);
    Input(*Wind, TEXT("ParentContribution"), Graph.Scalar(TEXT("wind_parent_contrib"), 0.25f));
    Input(*Wind, TEXT("DampBase"), Graph.Scalar(TEXT("wind_motion_damp_base"), 3));
    Input(*Wind, TEXT("DampLevel"), Graph.Scalar(TEXT("wind_motion_damp_level_mul"), 0.8f));
    Input(*Wind, TEXT("ChannelStrength"), Graph.Vector(TEXT("wind_channel_strength"), FLinearColor(1,1,0,1)), 5);
    auto* NoiseTexture = Graph.Add<UMaterialExpressionTextureObject>();
    NoiseTexture->Texture = &NoiseVolume;
    NoiseTexture->SamplerType = SAMPLERTYPE_LinearColor;
    Input(*Wind, TEXT("WindNoiseTexture"), NoiseTexture);
    for (int32 Index = 0; Index < 2; ++Index)
    {
        auto* Output = Graph.Add<UMaterialExpressionFunctionOutput>();
        Output->OutputName = Index == 0 ? TEXT("World Position Offset") : TEXT("World Rotation");
        Output->SortPriority = Index;
        // Stable connector IDs allow a later, explicit implementation upgrade.
        Output->Id = FGuid(0x4D485750, 0, 0, Index + 1);
        Output->A.Connect(Index, Wind);
        Output->MaterialExpressionEditorX = 300;
        Output->MaterialExpressionEditorY = Index * 150;
    }
    Function.Description = TEXT("Dagor pivot atlases to UE wind. UV1 indexes tex7/tex8; global wind comes from MH Wind Controller.");
    Function.bExposeToLibrary = true;
    Function.UpdateFromFunctionResource();
    Function.PostEditChange();
    return true;
}

static bool ValidatePivotWindFunction(
    const UMaterialFunction& Function, const UMaterialParameterCollection& Collection,
    const UTexture2D& DefaultPosition, const UTexture2D& DefaultDirection,
    const UVolumeTexture* NoiseVolume, FString& OutError)
{
    OutError.Reset();
    const bool bLegacy = NoiseVolume == nullptr;
    FString Code;
    if (!LoadPivotWindCode(Code, bLegacy))
    {
        OutError = TEXT("MH pivot wind shader source is missing from the plugin Resources folder");
        return false;
    }
    const auto Expressions = Function.GetExpressions();
    if (Expressions.Num() != (bLegacy ? 31 : 32))
    {
        return InvalidGraph(Function, TEXT("generated function node count differs"), OutError);
    }

    const UMaterialExpressionCustom* Wind = nullptr;
    TMap<FName, const UMaterialExpressionScalarParameter*> Scalars;
    TMap<FName, const UMaterialExpressionVectorParameter*> Vectors;
    TMap<FName, const UMaterialExpressionTextureObjectParameter*> Textures;
    TMap<FName, const UMaterialExpressionCollectionParameter*> Collections;
    TArray<const UMaterialExpressionFunctionOutput*> Outputs;
    for (const UMaterialExpression* Expression : Expressions)
    {
        if (const auto* Candidate = Cast<UMaterialExpressionCustom>(Expression))
        {
            if (Wind != nullptr)
                return InvalidGraph(Function, TEXT("generated v1 function has an unexpected custom node"), OutError);
            Wind = Candidate;
        }
        else if (const auto* ScalarParameter = Cast<UMaterialExpressionScalarParameter>(Expression))
        {
            if (Scalars.Contains(ScalarParameter->ParameterName))
                return InvalidGraph(Function, TEXT("generated v1 function has duplicate scalar parameters"), OutError);
            Scalars.Add(ScalarParameter->ParameterName, ScalarParameter);
        }
        else if (const auto* VectorParameter = Cast<UMaterialExpressionVectorParameter>(Expression))
        {
            if (Vectors.Contains(VectorParameter->ParameterName))
                return InvalidGraph(Function, TEXT("generated v1 function has duplicate vector parameters"), OutError);
            Vectors.Add(VectorParameter->ParameterName, VectorParameter);
        }
        else if (const auto* TextureParameter = Cast<UMaterialExpressionTextureObjectParameter>(Expression))
        {
            if (Textures.Contains(TextureParameter->ParameterName))
                return InvalidGraph(Function, TEXT("generated v1 function has duplicate texture parameters"), OutError);
            Textures.Add(TextureParameter->ParameterName, TextureParameter);
        }
        else if (const auto* MPCParameter = Cast<UMaterialExpressionCollectionParameter>(Expression))
        {
            if (Collections.Contains(MPCParameter->ParameterName))
                return InvalidGraph(Function, TEXT("generated v1 function has duplicate collection parameters"), OutError);
            Collections.Add(MPCParameter->ParameterName, MPCParameter);
        }
        else if (const auto* Output = Cast<UMaterialExpressionFunctionOutput>(Expression))
        {
            Outputs.Add(Output);
        }
    }
    if (Wind == nullptr || Wind->Description != (bLegacy ? AttachmentMarker : AlgorithmMarker) || Wind->Code != Code ||
        Wind->OutputType != CMOT_Float3 || Wind->Inputs.Num() != (bLegacy ? 24 : 25) ||
        !Wind->AdditionalDefines.IsEmpty() || !Wind->IncludeFilePaths.IsEmpty() ||
        Wind->AdditionalOutputs.Num() != 1 ||
        Wind->AdditionalOutputs[0].OutputName != TEXT("Rotation") ||
        Wind->AdditionalOutputs[0].OutputType != CMOT_Float4 ||
        Function.Description != TEXT("Dagor pivot atlases to UE wind. UV1 indexes tex7/tex8; global wind comes from MH Wind Controller.") ||
        !Function.bExposeToLibrary)
    {
        return InvalidGraph(Function, TEXT("generated v1 custom shader contract differs"), OutError);
    }

    const auto Scalar = [&](const TCHAR* InputName, const TCHAR* ParameterName, const float Value)
    {
        const UMaterialExpressionScalarParameter* const* Found = Scalars.Find(ParameterName);
        const UMaterialExpressionScalarParameter* Parameter = Found != nullptr ? *Found : nullptr;
        return Parameter != nullptr && Parameter->ExpressionGUID.IsValid() && Parameter->DefaultValue == Value &&
            Parameter->Group == TEXT("Plant wind response") &&
            ConnectedTo(CustomInput(*Wind, InputName), Parameter);
    };
    const auto Vector = [&](const TCHAR* InputName, const TCHAR* ParameterName, const FLinearColor& Value)
    {
        const UMaterialExpressionVectorParameter* const* Found = Vectors.Find(ParameterName);
        const UMaterialExpressionVectorParameter* Parameter = Found != nullptr ? *Found : nullptr;
        return Parameter != nullptr && Parameter->ExpressionGUID.IsValid() && Parameter->DefaultValue == Value &&
            Parameter->Group == TEXT("Plant wind response") &&
            ConnectedTo(CustomInput(*Wind, InputName), Parameter, 5);
    };
    if (Scalars.Num() != (bLegacy ? 9 : 8) ||
        !Scalar(TEXT("IsPivoted"), TEXT("is_pivoted"), 0) ||
        !Scalar(TEXT("NoiseSpeedBase"), TEXT("wind_noise_speed_base"), 0.1f) ||
        !Scalar(TEXT("NoiseSpeedLevel"), TEXT("wind_noise_speed_level_mul"), 1.666f) ||
        !Scalar(TEXT("AngleBase"), TEXT("wind_angle_rot_base"), 5) ||
        !Scalar(TEXT("AngleLevel"), TEXT("wind_angle_rot_level_mul"), 5) ||
        !Scalar(TEXT("ParentContribution"), TEXT("wind_parent_contrib"), 0.25f) ||
        !Scalar(TEXT("DampBase"), TEXT("wind_motion_damp_base"), 3) ||
        !Scalar(TEXT("DampLevel"), TEXT("wind_motion_damp_level_mul"), 0.8f) ||
        (bLegacy && !Scalar(TEXT("FlutterCm"), TEXT("mh_leaf_flutter_cm"), 1)) ||
        Vectors.Num() != 2 ||
        !Vector(TEXT("AngleLimits"), TEXT("wind_per_level_angle_rot_max"), FLinearColor(60,60,60,60)) ||
        !Vector(TEXT("ChannelStrength"), TEXT("wind_channel_strength"), FLinearColor(1,1,0,1)))
    {
        return InvalidGraph(Function, TEXT("generated v1 response parameter wiring differs"), OutError);
    }
    if (!bLegacy)
    {
        const FExpressionInput* NoiseInput = CustomInput(*Wind, TEXT("WindNoiseTexture"));
        const auto* NoiseTexture = NoiseInput != nullptr
            ? Cast<UMaterialExpressionTextureObject>(NoiseInput->Expression) : nullptr;
        if (NoiseTexture == nullptr || NoiseTexture->Texture != NoiseVolume ||
            NoiseTexture->SamplerType != SAMPLERTYPE_LinearColor || NoiseInput->OutputIndex != 0 ||
            !Expressions.Contains(NoiseTexture))
            return InvalidGraph(Function, TEXT("generated v2 wind noise texture wiring differs"), OutError);
        if (!MHValidateDagorWindNoise(*NoiseVolume, OutError)) return false;
    }

    const auto Texture = [&](const TCHAR* InputName, const TCHAR* ParameterName,
        const UTexture2D& DefaultTexture)
    {
        const UMaterialExpressionTextureObjectParameter* const* Found = Textures.Find(ParameterName);
        const UMaterialExpressionTextureObjectParameter* Parameter = Found != nullptr ? *Found : nullptr;
        return Parameter != nullptr && Parameter->ExpressionGUID.IsValid() && Parameter->Texture == &DefaultTexture &&
            Parameter->SamplerType == SAMPLERTYPE_LinearColor &&
            Parameter->Group == TEXT("Pivot atlases") &&
            ConnectedTo(CustomInput(*Wind, InputName), Parameter);
    };
    if (Textures.Num() != 2 ||
        !Texture(TEXT("PivotPos"), TEXT("tex7"), DefaultPosition) ||
        !Texture(TEXT("PivotDir"), TEXT("tex8"), DefaultDirection))
    {
        return InvalidGraph(Function, TEXT("generated v1 pivot texture wiring differs"), OutError);
    }

    const auto CollectionParameter = [&](const TCHAR* InputName, const TCHAR* ParameterName)
    {
        const UMaterialExpressionCollectionParameter* const* Found = Collections.Find(ParameterName);
        const UMaterialExpressionCollectionParameter* Parameter = Found != nullptr ? *Found : nullptr;
        return Parameter != nullptr && Parameter->Collection == &Collection &&
            Parameter->ParameterId == Collection.GetParameterId(ParameterName) &&
            ConnectedTo(CustomInput(*Wind, InputName), Parameter);
    };
    if (Collections.Num() != (bLegacy ? 2 : 3) ||
        !CollectionParameter(TEXT("WindFlow"), TEXT("MH_WindDirectionSpeed")) ||
        !CollectionParameter(TEXT("WindNoise"), TEXT("MH_WindNoise")) ||
        (!bLegacy && !CollectionParameter(TEXT("TreeWind"), TEXT("MH_WindTree"))))
    {
        return InvalidGraph(Function, TEXT("generated v1 collection wiring differs"), OutError);
    }

    const FExpressionInput* UVInput = CustomInput(*Wind, TEXT("PivotUV"));
    const FExpressionInput* PositionInput = CustomInput(*Wind, TEXT("LocalPosition"));
    const FExpressionInput* OriginInput = CustomInput(*Wind, TEXT("OriginWS"));
    const FExpressionInput* VertexNormalInput = CustomInput(*Wind, TEXT("VertexNormalWS"));
    const FExpressionInput* VertexColorInput = CustomInput(*Wind, TEXT("VertexColor"));
    const FExpressionInput* TimeInput = CustomInput(*Wind, TEXT("TimeSeconds"));
    const auto* UV = UVInput != nullptr ? Cast<UMaterialExpressionTextureCoordinate>(UVInput->Expression) : nullptr;
    const auto* Position = PositionInput != nullptr ? Cast<UMaterialExpressionLocalPosition>(PositionInput->Expression) : nullptr;
    const auto* Origin = OriginInput != nullptr ? Cast<UMaterialExpressionTransformPosition>(OriginInput->Expression) : nullptr;
    const auto* VertexNormal = VertexNormalInput != nullptr ? Cast<UMaterialExpressionVertexNormalWS>(VertexNormalInput->Expression) : nullptr;
    const auto* VertexColor = VertexColorInput != nullptr ? Cast<UMaterialExpressionVertexColor>(VertexColorInput->Expression) : nullptr;
    const auto* Time = TimeInput != nullptr ? Cast<UMaterialExpressionTime>(TimeInput->Expression) : nullptr;
    if (UV == nullptr || UV->CoordinateIndex != 1 || UV->UTiling != 1.0f || UV->VTiling != 1.0f ||
        UV->UnMirrorU || UV->UnMirrorV || Position == nullptr ||
        Position->LocalOrigin != ELocalPositionOrigin::InstancePreSkinning ||
        Position->IncludedOffsets != EPositionIncludedOffsets::ExcludeOffsets ||
        Origin == nullptr || Origin->TransformSourceType != TRANSFORMPOSSOURCE_Local ||
        Origin->TransformType != TRANSFORMPOSSOURCE_World ||
        VertexNormal == nullptr || VertexColor == nullptr || Time == nullptr ||
        Time->bIgnorePause || Time->bOverride_Period || Time->Period != 0.0f ||
        UVInput->OutputIndex != 0 || PositionInput->OutputIndex != 0 ||
        OriginInput->OutputIndex != 0 || VertexNormalInput->OutputIndex != 0 ||
        VertexColorInput->OutputIndex != 0 || TimeInput->OutputIndex != 0)
    {
        return InvalidGraph(Function, TEXT("generated v1 vertex source wiring differs"), OutError);
    }
    const auto* OriginConstant = Cast<UMaterialExpressionConstant3Vector>(Origin->Input.Expression);
    if (OriginConstant == nullptr || OriginConstant->Constant != FLinearColor::Black ||
        !ConnectedTo(&Origin->Input, OriginConstant))
    {
        return InvalidGraph(Function, TEXT("generated v1 instance origin wiring differs"), OutError);
    }
    for (const auto& Basis : {
        TPair<const TCHAR*, FLinearColor>(TEXT("BasisX"), FLinearColor(1,0,0)),
        TPair<const TCHAR*, FLinearColor>(TEXT("BasisY"), FLinearColor(0,1,0)),
        TPair<const TCHAR*, FLinearColor>(TEXT("BasisZ"), FLinearColor(0,0,1))})
    {
        const FExpressionInput* BasisInput = CustomInput(*Wind, Basis.Key);
        const auto* Transform = BasisInput != nullptr
            ? Cast<UMaterialExpressionTransform>(BasisInput->Expression) : nullptr;
        const auto* Constant = Transform != nullptr
            ? Cast<UMaterialExpressionConstant3Vector>(Transform->Input.Expression) : nullptr;
        if (Transform == nullptr || Transform->TransformSourceType != TRANSFORMSOURCE_Local ||
            Transform->TransformType != TRANSFORM_World || Constant == nullptr ||
            Constant->Constant != Basis.Value || !ConnectedTo(BasisInput, Transform) ||
            !ConnectedTo(&Transform->Input, Constant))
        {
            return InvalidGraph(Function, TEXT("generated v1 instance basis wiring differs"), OutError);
        }
    }
    if (Outputs.Num() != 2)
    {
        return InvalidGraph(Function, TEXT("generated v1 function outputs differ"), OutError);
    }
    for (int32 Index = 0; Index < 2; ++Index)
    {
        const FName ExpectedName = Index == 0 ? TEXT("World Position Offset") : TEXT("World Rotation");
        const FGuid ExpectedId(0x4D485750, 0, 0, Index + 1);
        const UMaterialExpressionFunctionOutput* Match = nullptr;
        for (const UMaterialExpressionFunctionOutput* Output : Outputs)
            if (Output->OutputName == ExpectedName) Match = Output;
        if (Match == nullptr || Match->Id != ExpectedId || Match->SortPriority != Index ||
            !ConnectedTo(&Match->A, Wind, Index))
        {
            return InvalidGraph(Function, TEXT("generated v1 function output wiring differs"), OutError);
        }
    }
    // All generated nodes belong to this function and are reachable from its
    // two outputs; reject orphan replacement nodes and external graph links.
    TSet<const UMaterialExpression*> Visited;
    TArray<const UMaterialExpression*> Pending;
    for (const auto* Output : Outputs) Pending.Add(Output);
    while (!Pending.IsEmpty())
    {
        const UMaterialExpression* Expression = Pending.Pop(EAllowShrinking::No);
        if (Visited.Contains(Expression)) continue;
        if (!Expressions.Contains(Expression) || Expression->GetOuter() != &Function ||
            !Expression->MaterialExpressionGuid.IsValid())
            return InvalidGraph(Function, TEXT("generated function node ownership differs"), OutError);
        Visited.Add(Expression);
        for (FExpressionInputIterator It{const_cast<UMaterialExpression*>(Expression)}; It; ++It)
        {
            const FExpressionInput* Link = It.Input;
            if (Link && Link->Expression)
            {
                if (!ConnectedTo(Link, Link->Expression, Link->OutputIndex))
                    return InvalidGraph(Function, TEXT("generated function connection mask differs"), OutError);
                Pending.Add(Link->Expression);
            }
        }
    }
    if (Visited.Num() != Expressions.Num())
        return InvalidGraph(Function, TEXT("generated function contains disconnected nodes"), OutError);
    return true;
}

bool MHValidatePivotWindFunction(
    const UMaterialFunction& Function, const UMaterialParameterCollection& Collection,
    const UTexture2D& DefaultPosition, const UTexture2D& DefaultDirection,
    const UVolumeTexture& NoiseVolume, FString& OutError)
{
    return ValidatePivotWindFunction(
        Function, Collection, DefaultPosition, DefaultDirection, &NoiseVolume, OutError);
}

bool MHValidateLegacyPivotWindFunction(
    const UMaterialFunction& Function, const UMaterialParameterCollection& Collection,
    const UTexture2D& DefaultPosition, const UTexture2D& DefaultDirection,
    FString& OutError)
{
    return ValidatePivotWindFunction(
        Function, Collection, DefaultPosition, DefaultDirection, nullptr, OutError);
}

bool MHUpgradePivotWindFunction(
    UMaterialFunction& Function, UMaterialParameterCollection& Collection,
    UTexture2D& DefaultPosition, UTexture2D& DefaultDirection,
    UVolumeTexture& NoiseVolume, FString& OutError)
{
    // Admission must happen before Modify/PostEditChange so a user-edited v1
    // graph is never silently replaced with the generated implementation.
    if (!MHValidateLegacyPivotWindFunction(
        Function, Collection, DefaultPosition, DefaultDirection, OutError) ||
        !MHValidateDagorWindNoise(NoiseVolume, OutError)) return false;
    if (!Collection.GetParameterId(TEXT("MH_WindTree")).IsValid())
    {
        OutError = TEXT("MH wind upgrade requires the MH_WindTree collection vector");
        return false;
    }
    FString Code;
    if (!LoadPivotWindCode(Code))
    {
        OutError = TEXT("MH pivot wind v2 shader source is missing; existing function preserved");
        return false;
    }
    UMaterialExpressionCustom* Wind = nullptr;
    UMaterialExpressionScalarParameter* Flutter = nullptr;
    for (UMaterialExpression* Expression : Function.GetExpressions())
    {
        if (auto* Custom = Cast<UMaterialExpressionCustom>(Expression)) Wind = Custom;
        if (auto* Scalar = Cast<UMaterialExpressionScalarParameter>(Expression))
            if (Scalar->ParameterName == TEXT("mh_leaf_flutter_cm")) Flutter = Scalar;
    }
    check(Wind && Flutter); // The complete v1 admission above proved both nodes.
    Function.Modify();
    Wind->Modify();
    FGraph Graph{Function, Function.GetExpressionCollection()};
    auto* NoiseTexture = Graph.Add<UMaterialExpressionTextureObject>();
    NoiseTexture->Texture = &NoiseVolume;
    NoiseTexture->SamplerType = SAMPLERTYPE_LinearColor;
    for (FCustomInput& InputSlot : Wind->Inputs)
    {
        if (InputSlot.InputName == TEXT("FlutterCm"))
        {
            InputSlot.InputName = TEXT("WindNoiseTexture");
            InputSlot.Input.Connect(0, NoiseTexture);
        }
    }
    Function.GetExpressionCollection().RemoveExpression(Flutter);
    auto* Tree = Graph.Add<UMaterialExpressionCollectionParameter>();
    Tree->Collection = &Collection;
    Tree->ParameterName = TEXT("MH_WindTree");
    Tree->ParameterId = Collection.GetParameterId(Tree->ParameterName);
    Input(*Wind, TEXT("TreeWind"), Tree);
    Wind->Code = MoveTemp(Code);
    Wind->Description = AlgorithmMarker;
    // Existing output nodes, GUIDs, surface wrappers and retained parameter
    // expression IDs remain intact so instance overrides keep their identity.
    Function.UpdateFromFunctionResource();
    Function.PostEditChange();
    Function.MarkPackageDirty();
    return MHValidatePivotWindFunction(
        Function, Collection, DefaultPosition, DefaultDirection, NoiseVolume, OutError);
}

bool MHValidatePivotWindAttachment(
    const UMaterial& Material, const UMaterialFunction& Function, FString& OutError)
{
    OutError.Reset();
    const UMaterialEditorOnlyData* Data = Material.GetEditorOnlyData();
    if (Data == nullptr || Material.bUseMaterialAttributes || Material.MaterialDomain != MD_Surface)
        return InvalidGraph(Material, TEXT("pivot wind requires an individual-input surface material"), OutError);
    const auto* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Data->WorldPositionOffset.Expression);
    if (Call == nullptr || Call->MaterialFunction != &Function || Call->Desc != AttachmentMarker ||
        Data->WorldPositionOffset.OutputIndex != 0)
        return InvalidGraph(Material, TEXT("pivot wind WPO function or output wiring differs"), OutError);

    const UMaterialExpression* NormalRoot = Data->Normal.Expression;
    if (Data->Normal.OutputIndex != 0)
        return InvalidGraph(Material, TEXT("pivot wind normal material output differs"), OutError);
    if (Material.bTangentSpaceNormal)
    {
        const auto* ToTangent = Cast<UMaterialExpressionTransform>(NormalRoot);
        if (ToTangent == nullptr || ToTangent->TransformSourceType != TRANSFORMSOURCE_World ||
            ToTangent->TransformType != TRANSFORM_Tangent || ToTangent->Input.OutputIndex != 0)
            return InvalidGraph(Material, TEXT("pivot wind tangent-space normal output wiring differs"), OutError);
        NormalRoot = ToTangent->Input.Expression;
    }
    const auto* Normal = Cast<UMaterialExpressionCustom>(NormalRoot);
    if (Normal == nullptr || Normal->Desc != AttachmentMarker ||
        Normal->Description != TEXT("Rotate the existing surface normal with the branches") ||
        Normal->Code != TEXT("float4 q = Q / max(length(Q),0.0001); return normalize(N + 2.0*cross(q.xyz,cross(q.xyz,N)+q.w*N));") ||
        Normal->OutputType != CMOT_Float3 || Normal->Inputs.Num() != 2)
        return InvalidGraph(Material, TEXT("pivot wind normal rotation node differs"), OutError);
    const FExpressionInput* Q = CustomInput(*Normal, TEXT("Q"));
    const FExpressionInput* N = CustomInput(*Normal, TEXT("N"));
    const auto* Interpolate = Q != nullptr ? Cast<UMaterialExpressionVertexInterpolator>(Q->Expression) : nullptr;
    const auto* ToWorld = N != nullptr ? Cast<UMaterialExpressionTransform>(N->Expression) : nullptr;
    if (Q == nullptr || Q->OutputIndex != 0 || N == nullptr || N->OutputIndex != 0 ||
        Interpolate == nullptr || !ConnectedTo(&Interpolate->Input, Call, 1) ||
        ToWorld == nullptr || ToWorld->TransformType != TRANSFORM_World ||
        ToWorld->TransformSourceType != (Material.bTangentSpaceNormal ? TRANSFORMSOURCE_Tangent : TRANSFORMSOURCE_World) ||
        ToWorld->Input.Expression == nullptr)
        return InvalidGraph(Material, TEXT("pivot wind normal input or quaternion wiring differs"), OutError);
    if (!Material.GetUsageByFlag(MATUSAGE_InstancedStaticMeshes) ||
        !FMath::IsFinite(Material.MaxWorldPositionOffsetDisplacement) ||
        Material.MaxWorldPositionOffsetDisplacement < 200.0f)
        return InvalidGraph(Material, TEXT("pivot wind ISM usage or displacement bound differs"), OutError);
    return true;
}

bool MHAttachPivotWind(UMaterial& Material, UMaterialFunction& Function, FString& OutError)
{
    OutError.Reset();
    UMaterialEditorOnlyData* Data = Material.GetEditorOnlyData();
    if (Data == nullptr || Material.bUseMaterialAttributes || Material.MaterialDomain != MD_Surface)
    {
        OutError = TEXT("Pivot wind requires a surface material with individual material-property inputs");
        return false;
    }
    if (Data->WorldPositionOffset.Expression != nullptr)
    {
        const auto* Call = Cast<UMaterialExpressionMaterialFunctionCall>(Data->WorldPositionOffset.Expression);
        if (Call != nullptr && Call->MaterialFunction == &Function && Call->Desc == AttachmentMarker)
            return MHValidatePivotWindAttachment(Material, Function, OutError);
        OutError = TEXT("Material already has a World Position Offset graph; it needs explicit composition with MH wind");
        return false;
    }
    Material.Modify();
    FGraph Graph{Material, Material.GetExpressionCollection()};
    auto* Call = Graph.Add<UMaterialExpressionMaterialFunctionCall>();
    Call->Desc = AttachmentMarker;
    if (!Call->SetMaterialFunction(&Function))
    {
        OutError = TEXT("Could not bind the MH pivot wind material function");
        Material.GetExpressionCollection().RemoveExpression(Call);
        return false;
    }
    Data->WorldPositionOffset.Connect(0, Call);
    auto* Interpolate = Graph.Add<UMaterialExpressionVertexInterpolator>();
    Interpolate->Input.Connect(1, Call);
    auto* Normal = Graph.Add<UMaterialExpressionCustom>();
    Normal->Desc = AttachmentMarker;
    Normal->Description = TEXT("Rotate the existing surface normal with the branches");
    Normal->OutputType = CMOT_Float3;
    Normal->Inputs.Empty();
    Normal->Code = TEXT("float4 q = Q / max(length(Q),0.0001); return normalize(N + 2.0*cross(q.xyz,cross(q.xyz,N)+q.w*N));");
    Input(*Normal, TEXT("Q"), Interpolate);
    // Preserve the original normal node and its selected output, including its mask.
    auto* ToWorld = Graph.Add<UMaterialExpressionTransform>();
    ToWorld->TransformSourceType = Material.bTangentSpaceNormal ? TRANSFORMSOURCE_Tangent : TRANSFORMSOURCE_World;
    ToWorld->TransformType = TRANSFORM_World;
    if (Data->Normal.Expression != nullptr)
    {
        ToWorld->Input = Data->Normal;
    }
    else
    {
        ToWorld->Input.Connect(0, Material.bTangentSpaceNormal
            ? static_cast<UMaterialExpression*>(Graph.Constant(FLinearColor(0,0,1)))
            : static_cast<UMaterialExpression*>(Graph.Add<UMaterialExpressionVertexNormalWS>()));
    }
    Input(*Normal, TEXT("N"), ToWorld);
    Data->Normal.Connect(0, Material.bTangentSpaceNormal
        ? static_cast<UMaterialExpression*>(Graph.Transform(Normal, TRANSFORMSOURCE_World, TRANSFORM_Tangent))
        : static_cast<UMaterialExpression*>(Normal));
    // A positive native WPO bound both expands culling bounds and clamps the
    // displacement. Keep a larger artist-authored bound; 2m is the bush pilot
    // default and can be adjusted in the material for larger vegetation.
    Material.MaxWorldPositionOffsetDisplacement = FMath::Max(Material.MaxWorldPositionOffsetDisplacement, 200.0f);
    // SetMaterialUsage can compile immediately. Populate the new function's
    // texture references before it sees the edited graph.
    Material.UpdateCachedExpressionData();
    bool bNeedsRecompile = false;
    Material.SetMaterialUsage(bNeedsRecompile, MATUSAGE_InstancedStaticMeshes);
    Material.PostEditChange();
    // UE 5.7 PostEditChange invalidates with PrecompileMode::None. Request a
    // usable shader map explicitly for headless setup and its compile gate.
    Material.ForceRecompileForRendering(EMaterialShaderPrecompileMode::Synchronous);
    Material.MarkPackageDirty();
    return MHValidatePivotWindAttachment(Material, Function, OutError);
}
}
