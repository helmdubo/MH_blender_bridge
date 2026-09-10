#include "Wind/MHPivotWindMaterial.h"
#include "Wind/MHDagorWindNoise.h"

#include "AssetCompilingManager.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/VolumeTexture.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "Geometry/MHFbxSceneTranslator.h"
#include "Geometry/MHSceneIR.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionTransform.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "MaterialShared.h"
#include "Math/Float16Color.h"
#include "Math/Transform.h"
#include "Math/Vector2D.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "RenderingThread.h"
#include "RHI.h"
#include "StaticMesh/MHStaticMeshImporter.h"
#include "TextureResource.h"
#include "UObject/StrongObjectPtr.h"

namespace UE::MimirComposite::Tests
{
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMHPivotWindMaterialRenderTest,
    "Mimir.Wind.Material.RenderStaticAndInstanced",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMHPivotWindMaterialRenderTest::RunTest(const FString& Parameters)
{
    if (!FApp::CanEverRender())
    {
        AddWarning(TEXT("NOT RUN: wind material render test requires a rendering RHI"));
        return true;
    }
    const IConsoleVariable* JobCacheDDC = IConsoleManager::Get().FindConsoleVariable(TEXT("r.ShaderCompiler.JobCacheDDC"));
    if (JobCacheDDC && JobCacheDDC->GetInt() != 0)
    {
        AddError(TEXT("This render gate requires complete shader maps. Start with -ini:Engine:[SystemSettings]:r.ShaderCompiler.JobCacheDDC=0"));
        return false;
    }
    TStrongObjectPtr<UTexture2D> Position(NewObject<UTexture2D>());
    TStrongObjectPtr<UTexture2D> Direction(NewObject<UTexture2D>());
    TStrongObjectPtr<UVolumeTexture> NoiseTexture(NewObject<UVolumeTexture>());
    FString Error;
    if (!TestTrue(TEXT("build Dagor gust volume"), MHBuildDagorWindNoise(*NoiseTexture, Error)))
    {
        AddError(Error);
        return false;
    }
    TArray<FFloat16Color> P;
    P.Init(FFloat16Color(FLinearColor::Transparent), 2048);
    TArray<FColor> D;
    D.Init(FColor(128,255,128,1), 2048);
    // Optional read-only field fixture: use a real exported plant and its
    // original atlas pair in the same render assertion, without committing the
    // artist's source assets to the repository.
    FString ProbeMeshPath, ProbeAtlasRoot;
    const bool bSourceProbe = FParse::Value(FCommandLine::Get(),TEXT("MHWindProbeMesh="),ProbeMeshPath);
    FString ProbeName = FPaths::GetBaseFilename(ProbeMeshPath);
    ProbeName.RemoveFromEnd(TEXT(".mesh"));
    if (bSourceProbe)
    {
        TArray<uint8> PositionBytes, DirectionBytes;
        if (!FParse::Value(FCommandLine::Get(),TEXT("MHWindProbeAtlasRoot="),ProbeAtlasRoot) ||
            !FFileHelper::LoadFileToArray(PositionBytes,*FPaths::Combine(ProbeAtlasRoot,ProbeName+TEXT("_pivot_pos.dds"))) ||
            !FFileHelper::LoadFileToArray(DirectionBytes,*FPaths::Combine(ProbeAtlasRoot,ProbeName+TEXT("_pivot_dir.dds"))) ||
            PositionBytes.Num()!=128+2048*8 || DirectionBytes.Num()!=128+2048*4)
        {
            AddError(TEXT("Wind source probe requires the matching legacy 32x64 RGBA16F / RGBA8 DDS pair"));
            return false;
        }
        FMemory::Memcpy(P.GetData(),PositionBytes.GetData()+128,2048*8);
        for (int32 Index=0;Index<2048;++Index)
        {
            const uint8* Pixel=DirectionBytes.GetData()+128+Index*4;
            D[Index]=FColor(Pixel[0],Pixel[1],Pixel[2],Pixel[3]);
        }
        AddInfo(TEXT("Read-only wind render fixture: ")+ProbeMeshPath);
    }
    else
    {
        // Three authored hierarchy levels: sampled cell 2 -> parent 1 -> root
        // 0. Non-zero local pivots ensure the render probe exercises parent
        // traversal and rotation composition instead of a single identity root.
        P[0] = FFloat16Color(FLinearColor(0.0f,0.0f,0.0f,0.0f));
        P[1] = FFloat16Color(FLinearColor(0.0f,0.20f,0.0f,0.0f));
        P[2] = FFloat16Color(FLinearColor(0.0f,0.55f,0.0f,1.0f));
        D[0] = D[1] = D[2] = FColor(128,255,128,16);
    }
    Position->Source.Init(32,64,1,1,TSF_RGBA16F,reinterpret_cast<const uint8*>(P.GetData()));
    Direction->Source.Init(32,64,1,1,TSF_BGRA8,reinterpret_cast<const uint8*>(D.GetData()));
    Position->CompressionSettings = TC_HDR;
    Direction->CompressionSettings = TC_VectorDisplacementmap;
    for (UTexture2D* Texture : {Position.Get(),Direction.Get()})
    {
        Texture->SRGB = false;
        Texture->MipGenSettings = TMGS_NoMipmaps;
        Texture->Filter = TF_Nearest;
        Texture->AddressX = Texture->AddressY = TA_Clamp;
        Texture->NeverStream = true;
        Texture->PostEditChange();
    }
    // Function texture references are cached when the material graph is
    // compiled. Complete both transient texture resources first so a later
    // PostEditChange cannot invalidate their pending builds and cancel the
    // shader jobs that reference them.
    FAssetCompilingManager::Get().FinishAllCompilation();
    TStrongObjectPtr<UMaterialParameterCollection> Collection(NewObject<UMaterialParameterCollection>());
    Collection->PreEditChange(nullptr);
    FCollectionVectorParameter Flow;
    Flow.ParameterName = TEXT("MH_WindDirectionSpeed");
    Flow.DefaultValue = FLinearColor(1,0,1,0);
    Collection->VectorParameters.Add(Flow);
    FCollectionVectorParameter Noise;
    Noise.ParameterName = TEXT("MH_WindNoise");
    // Spatial noise is active, while its advection speed is zero. This makes
    // an incorrect primitive/instance origin observable without changing the
    // expected deformation between sequential captures.
    Noise.DefaultValue = FLinearColor(0,0.01f,0.5f,0.6f);
    Collection->VectorParameters.Add(Noise);
    FCollectionVectorParameter Tree;
    Tree.ParameterName = TEXT("MH_WindTree");
    // Isolate pivot motion in this raster/basis test. Numeric parity tests
    // exercise the separate leaf wave and all three vertex-colour channels.
    Tree.DefaultValue = FLinearColor::Transparent;
    Collection->VectorParameters.Add(Tree);
    Collection->PostEditChange();
    TStrongObjectPtr<UMaterialFunction> Function(NewObject<UMaterialFunction>());
    if (!TestTrue(TEXT("build wind function"), MHBuildPivotWindFunction(
        *Function, *Collection, *Position, *Direction, *NoiseTexture, Error)))
    {
        AddError(Error);
        return false;
    }
    // Freeze shader time through a test-only parameter; sequential SMC/ISM
    // captures must compare the same animation instant, including leaf detail.
    auto* TestTime = NewObject<UMaterialExpressionScalarParameter>(Function.Get());
    TestTime->Function = Function.Get();
    TestTime->ParameterName = TEXT("MH_TestWindTime");
    TestTime->DefaultValue = 0;
    Function->GetExpressionCollection().AddExpression(TestTime);
    for (UMaterialExpression* Expression : Function->GetExpressions())
    {
        if (auto* Custom = Cast<UMaterialExpressionCustom>(Expression))
            for (FCustomInput& CustomInput : Custom->Inputs)
                if (CustomInput.InputName == TEXT("TimeSeconds")) CustomInput.Input.Connect(0, TestTime);
    }
    Function->UpdateFromFunctionResource();
    TStrongObjectPtr<UMaterial> Material(NewObject<UMaterial>());
    TStrongObjectPtr<UMaterial> LitMaterial(NewObject<UMaterial>());
    TStrongObjectPtr<UMaterialInstanceConstant> Instance(NewObject<UMaterialInstanceConstant>());
    {
        FMaterialUpdateContext Context;
        Context.AddMaterial(Material.Get());
        Context.AddMaterial(LitMaterial.Get());
        Context.AddMaterialInstance(Instance.Get());
        Material->SetShadingModel(MSM_Unlit);
        Material->BlendMode = BLEND_Masked;
        Material->bTangentSpaceNormal = true;
        Material->TwoSided = true;
        auto* White = NewObject<UMaterialExpressionConstant3Vector>(Material.Get());
        White->Constant = FLinearColor::White;
        White->Material = Material.Get();
        Material->GetExpressionCollection().AddExpression(White);
        Material->GetEditorOnlyData()->EmissiveColor.Connect(0,White);
        auto* Mask = NewObject<UMaterialExpressionScalarParameter>(Material.Get());
        Mask->Material = Material.Get();
        Mask->ParameterName = TEXT("MH_TestMask");
        Mask->DefaultValue = 1;
        Material->GetExpressionCollection().AddExpression(Mask);
        Material->GetEditorOnlyData()->OpacityMask.Connect(0,Mask);
        auto* TiltedNormal = NewObject<UMaterialExpressionConstant3Vector>(Material.Get());
        TiltedNormal->Constant = FLinearColor(1.0f/3.0f,2.0f/3.0f,2.0f/3.0f);
        TiltedNormal->Material = Material.Get();
        Material->GetExpressionCollection().AddExpression(TiltedNormal);
        Material->GetEditorOnlyData()->Normal.Connect(0,TiltedNormal);
        if (!TestTrue(TEXT("attach unlit wind"), MHAttachPivotWind(*Material,*Function,Error)) ||
            !TestTrue(TEXT("attach lit wind including deformed normals"), MHAttachPivotWind(*LitMaterial,*Function,Error)))
        {
            AddError(Error);
            return false;
        }
        Context.AddMaterial(Material.Get());
        Context.AddMaterial(LitMaterial.Get());
        const int32 Nodes = Material->GetExpressions().Num();
        TestTrue(TEXT("attach is idempotent"), MHAttachPivotWind(*Material,*Function,Error));
        TestEqual(TEXT("no duplicate graph nodes"), Material->GetExpressions().Num(), Nodes);
        Instance->SetParentEditorOnly(Material.Get());
        Instance->SetScalarParameterValueEditorOnly(TEXT("is_pivoted"),1);
        Instance->SetScalarParameterValueEditorOnly(TEXT("wind_angle_rot_base"),30);
        Instance->PostEditChange();
        Context.AddMaterialInstance(Instance.Get());
    }
    FAssetCompilingManager::Get().FinishAllCompilation();
    for (UMaterial* Item : {Material.Get(),LitMaterial.Get()})
    {
        FMaterialResource* Resource = Item->GetMaterialResource(GMaxRHIShaderPlatform);
        if (!TestNotNull(TEXT("compiled material resource"),Resource)) return false;
        Resource->FinishCompilation();
        for (const FString& CompileError : Resource->GetCompileErrors()) AddError(CompileError);
        if (HasAnyErrors()) return false;
        if (!TestTrue(TEXT("material has a usable shader map"),Resource->HasValidGameThreadShaderMap())) return false;
    }

    // The same authored probe will be rendered through SMC and ISM at exactly
    // the same world transform. Rotation and non-uniform scale exercise the
    // complete instance basis; the ISM component has a deliberately different
    // origin so primitive-space sampling cannot accidentally agree.
    FMHSceneIR Scene;
    Scene.ResourceName = TEXT("wind_render_probe");
    Scene.MaterialNames = {TEXT("wind")};
    Scene.LODLevels = {0};
    FMHSceneIRNode& Node = Scene.Nodes.AddDefaulted_GetRef();
    Node.Name = Scene.ResourceName;
    Node.Attribute = EMHSceneNodeAttribute::Mesh;
    Node.Kind = EMHSceneNodeKind::Render;
    Node.LODLevel = 0;
    Node.MaterialSlots = Scene.MaterialNames;
    Node.Geometry.Emplace();
    Node.Geometry->Positions = {FVector3f(0,0,0),FVector3f(25,0,0),FVector3f(25,0,100),FVector3f(0,0,100)};
    const TStaticArray<FVector2f,4> SurfaceUVs = {
        FVector2f(0,0),FVector2f(1,0),FVector2f(1,1),FVector2f(0,1)};
    for (const TStaticArray<int32,3>& Corners : {TStaticArray<int32,3>{0,1,2},TStaticArray<int32,3>{0,2,3}})
    {
        FMHSceneTriangle& Triangle = Node.Geometry->Triangles.AddDefaulted_GetRef();
        Triangle.PositionIndices = Corners;
        Triangle.MaterialSlotIndex = 0;
        Triangle.AdditionalCornerUVs.Add({FVector2f(2.5f/32,0.5f/64),FVector2f(2.5f/32,0.5f/64),FVector2f(2.5f/32,0.5f/64)});
        for (int32 Corner=0;Corner<3;++Corner)
        {
            Triangle.CornerUV0[Corner] = SurfaceUVs[Corners[Corner]];
            Triangle.CornerNormals[Corner] = FVector3f(0,-1,0);
        }
    }
    TStrongObjectPtr<UStaticMesh> Mesh(NewObject<UStaticMesh>());
    FMHStaticMeshBuildPlan Plan;
    if (bSourceProbe)
    {
        TArray<uint8> FbxBytes;
        FMHFbxSceneTranslator Translator;
        if (!FFileHelper::LoadFileToArray(FbxBytes,*ProbeMeshPath) ||
            !Translator.Translate(ProbeName,FbxBytes,Scene,Error) || !MHClassifySceneIR(Scene,Error))
        {
            AddError(TEXT("Wind source probe mesh failed: ")+Error);
            return false;
        }
    }
    Plan.Scene = &Scene;
    for (const FString& Name : Scene.MaterialNames) Plan.Materials.Add(Name,Instance.Get());
    if (!TestTrue(TEXT("build render probe with authored pivot UV"),FMHStaticMeshBuilder::Rebuild(*Mesh,Plan,Error)))
    {
        AddError(Error);
        return false;
    }
    FAssetCompilingManager::Get().FinishAllCompilation();
    UWorld* World = UWorld::CreateWorld(EWorldType::EditorPreview,false);
    if (!TestNotNull(TEXT("render world"),World)) return false;
    ON_SCOPE_EXIT { World->DestroyWorld(false); FlushRenderingCommands(); };
    AActor* Actor = World->SpawnActor<AActor>();
    const FTransform DesiredWorld(
        FRotator(0.0f,20.0f,0.0f),
        FVector::ZeroVector,
        FVector(0.75f,1.25f,0.8f));
    UStaticMeshComponent* Single = NewObject<UStaticMeshComponent>(Actor);
    Actor->AddInstanceComponent(Single);
    Single->SetStaticMesh(Mesh.Get());
    Single->SetWorldTransform(DesiredWorld);
    Single->SetMobility(EComponentMobility::Movable);
    Single->RegisterComponent();
    UInstancedStaticMeshComponent* Instanced = NewObject<UInstancedStaticMeshComponent>(Actor);
    Actor->AddInstanceComponent(Instanced);
    Instanced->SetStaticMesh(Mesh.Get());
    Instanced->SetMobility(EComponentMobility::Movable);
    const FTransform InstanceComponentWorld(
        FQuat::Identity,
        FVector(137.0f,-89.0f,23.0f),
        FVector::OneVector);
    Instanced->SetWorldTransform(InstanceComponentWorld);
    Instanced->RegisterComponent();
    Instanced->AddInstance(DesiredWorld.GetRelativeTransform(InstanceComponentWorld));
    FTransform ActualInstanceWorld;
    if (!TestTrue(TEXT("read ISM instance world transform"),Instanced->GetInstanceTransform(0,ActualInstanceWorld,true)))
        return false;
    TestTrue(TEXT("ISM instance has the desired world location"),ActualInstanceWorld.GetLocation().Equals(DesiredWorld.GetLocation(),0.001));
    TestTrue(TEXT("ISM instance has the desired world rotation"),ActualInstanceWorld.GetRotation().Equals(DesiredWorld.GetRotation(),0.0001));
    TestTrue(TEXT("ISM instance has the desired world scale"),ActualInstanceWorld.GetScale3D().Equals(DesiredWorld.GetScale3D(),0.0001));

    TStrongObjectPtr<UTextureRenderTarget2D> Target(NewObject<UTextureRenderTarget2D>());
    Target->ClearColor = FLinearColor::Black;
    Target->InitCustomFormat(512,512,PF_B8G8R8A8,false);
    Target->UpdateResourceImmediate(true);
    USceneCaptureComponent2D* Capture = NewObject<USceneCaptureComponent2D>(Actor);
    Actor->AddInstanceComponent(Capture);
    Capture->TextureTarget = Target.Get();
    Capture->ProjectionType = ECameraProjectionMode::Orthographic;
    Capture->OrthoWidth = 600;
    Capture->CaptureSource = SCS_SceneColorHDR;
    Capture->bCaptureEveryFrame = false;
    Capture->bCaptureOnMovement = false;
    Capture->ShowFlags.SetAtmosphere(false);
    Capture->ShowFlags.SetFog(false);
    Capture->ShowFlags.SetMotionBlur(false);
    Capture->ShowFlags.SetAntiAliasing(false);
    Capture->SetWorldLocationAndRotation(FVector(0,-500,100),FRotator(0,90,0));
    Capture->RegisterComponent();
    UMaterialParameterCollectionInstance* Wind = World->GetParameterCollectionInstance(Collection.Get());
    auto Read = [&](bool bRenderSingle, bool bEnabled, TArray<FColor>& Pixels)
    {
        Single->SetVisibility(bRenderSingle,true);
        Instanced->SetVisibility(!bRenderSingle,true);
        Wind->SetVectorParameterValue(TEXT("MH_WindDirectionSpeed"),FLinearColor(1,0,4,bEnabled?1.0f:0.0f));
        World->SendAllEndOfFrameUpdates();
        Capture->CaptureScene();
        FlushRenderingCommands();
        return Target->GameThread_GetRenderTargetResource()->ReadPixels(Pixels);
    };
    TArray<FColor> SingleRest, SingleBent, SingleRestored;
    TArray<FColor> InstanceRest, InstanceBent, InstanceRestored;
    if (!TestTrue(TEXT("read SMC rest capture"),Read(true,false,SingleRest)) ||
        !TestTrue(TEXT("read SMC wind capture"),Read(true,true,SingleBent)) ||
        !TestTrue(TEXT("read SMC disabled capture"),Read(true,false,SingleRestored)) ||
        !TestTrue(TEXT("read ISM rest capture"),Read(false,false,InstanceRest)) ||
        !TestTrue(TEXT("read ISM wind capture"),Read(false,true,InstanceBent)) ||
        !TestTrue(TEXT("read ISM disabled capture"),Read(false,false,InstanceRestored))) return false;
    struct FSilhouette
    {
        FVector2D Centroid = FVector2D::ZeroVector;
        int32 Count = 0;
    };
    auto Silhouette = [&](const TArray<FColor>& Pixels)
    {
        FSilhouette Result;
        FVector2D Sum = FVector2D::ZeroVector;
        for(int32 Y=0;Y<512;++Y) for(int32 X=0;X<512;++X)
            if(Pixels[Y*512+X].R>100) {Sum+=FVector2D(X,Y); ++Result.Count;}
        TestTrue(TEXT("rendered mesh is visible in capture"),Result.Count>100);
        if(Result.Count>0) Result.Centroid=Sum/static_cast<double>(Result.Count);
        return Result;
    };
    const FSilhouette SingleRestShape=Silhouette(SingleRest);
    const FSilhouette SingleBentShape=Silhouette(SingleBent);
    const FSilhouette InstanceRestShape=Silhouette(InstanceRest);
    const FSilhouette InstanceBentShape=Silhouette(InstanceBent);
    const FVector2D SingleDelta=SingleBentShape.Centroid-SingleRestShape.Centroid;
    const FVector2D InstanceDelta=InstanceBentShape.Centroid-InstanceRestShape.Centroid;
    const int32 RestCoverageDelta=FMath::Abs(SingleRestShape.Count-InstanceRestShape.Count);
    const int32 BentCoverageDelta=FMath::Abs(SingleBentShape.Count-InstanceBentShape.Count);
    const int32 CoverageTolerance=FMath::Max(8,FMath::RoundToInt(
        0.03*FMath::Max(SingleRestShape.Count,InstanceRestShape.Count)));
    TestTrue(TEXT("wind visibly displaces SMC"),SingleDelta.Size()>3.0);
    TestTrue(TEXT("wind visibly displaces ISM"),InstanceDelta.Size()>3.0);
    TestTrue(TEXT("instance and SMC share the same world-frame deformation"),(SingleDelta-InstanceDelta).Size()<1.0);
    TestTrue(TEXT("rest silhouettes have nearly equal coverage"),RestCoverageDelta<=CoverageTolerance);
    TestTrue(TEXT("bent silhouettes have nearly equal coverage"),BentCoverageDelta<=CoverageTolerance);
    TestTrue(TEXT("disabled wind restores exact SMC geometry"),SingleRest==SingleRestored);
    TestTrue(TEXT("disabled wind restores exact ISM geometry"),InstanceRest==InstanceRestored);
    AddInfo(FString::Printf(
        TEXT("MH_WIND_RENDER smc_delta=(%.3f,%.3f) ism_delta=(%.3f,%.3f) rest_coverage_delta=%d bent_coverage_delta=%d"),
        SingleDelta.X,SingleDelta.Y,InstanceDelta.X,InstanceDelta.Y,RestCoverageDelta,BentCoverageDelta));

    // Make the attached material normal directly observable. The attachment
    // stores its result in tangent space, so use UE's native tangent-to-world
    // transform before encoding [-1,1] as emissive [0,1].
    {
        FMaterialUpdateContext Context;
        Context.AddMaterial(Material.Get());
        Context.AddMaterialInstance(Instance.Get());
        Material->Modify();
        auto* NormalToWorld = NewObject<UMaterialExpressionTransform>(Material.Get());
        NormalToWorld->Material = Material.Get();
        NormalToWorld->TransformSourceType = TRANSFORMSOURCE_Tangent;
        NormalToWorld->TransformType = TRANSFORM_World;
        NormalToWorld->Input = Material->GetEditorOnlyData()->Normal;
        Material->GetExpressionCollection().AddExpression(NormalToWorld);
        auto* Half = NewObject<UMaterialExpressionConstant>(Material.Get());
        Half->Material = Material.Get();
        Half->R = 0.5f;
        Material->GetExpressionCollection().AddExpression(Half);
        auto* Scale = NewObject<UMaterialExpressionMultiply>(Material.Get());
        Scale->Material = Material.Get();
        Scale->A.Connect(0,NormalToWorld);
        Scale->B.Connect(0,Half);
        Material->GetExpressionCollection().AddExpression(Scale);
        auto* Bias = NewObject<UMaterialExpressionAdd>(Material.Get());
        Bias->Material = Material.Get();
        Bias->A.Connect(0,Scale);
        Bias->B.Connect(0,Half);
        Material->GetExpressionCollection().AddExpression(Bias);
        Material->GetEditorOnlyData()->EmissiveColor.Connect(0,Bias);
        Material->UpdateCachedExpressionData();
        Material->PostEditChange();
        Material->ForceRecompileForRendering();
        Instance->PostEditChange();
    }
    FAssetCompilingManager::Get().FinishAllCompilation();
    FMaterialResource* NormalProbeResource = Material->GetMaterialResource(GMaxRHIShaderPlatform);
    if (!TestNotNull(TEXT("normal-probe material resource"),NormalProbeResource)) return false;
    NormalProbeResource->FinishCompilation();
    for (const FString& CompileError : NormalProbeResource->GetCompileErrors()) AddError(CompileError);
    if (HasAnyErrors() ||
        !TestTrue(TEXT("normal-probe material has a usable shader map"),NormalProbeResource->HasValidGameThreadShaderMap()))
        return false;
    Single->MarkRenderStateDirty();
    Instanced->MarkRenderStateDirty();

    TArray<FColor> SingleNormalRest, SingleNormalBent, SingleNormalRestored;
    TArray<FColor> InstanceNormalRest, InstanceNormalBent, InstanceNormalRestored;
    if (!TestTrue(TEXT("read SMC rest normal capture"),Read(true,false,SingleNormalRest)) ||
        !TestTrue(TEXT("read SMC wind normal capture"),Read(true,true,SingleNormalBent)) ||
        !TestTrue(TEXT("read SMC disabled normal capture"),Read(true,false,SingleNormalRestored)) ||
        !TestTrue(TEXT("read ISM rest normal capture"),Read(false,false,InstanceNormalRest)) ||
        !TestTrue(TEXT("read ISM wind normal capture"),Read(false,true,InstanceNormalBent)) ||
        !TestTrue(TEXT("read ISM disabled normal capture"),Read(false,false,InstanceNormalRestored))) return false;
    struct FMeanColor
    {
        FVector RGB = FVector::ZeroVector;
        int32 Count = 0;
    };
    auto MeanForegroundColor = [&](const TArray<FColor>& Pixels)
    {
        FMeanColor Result;
        for (const FColor& Pixel : Pixels)
        {
            if (FMath::Max3(Pixel.R,Pixel.G,Pixel.B)>8)
            {
                Result.RGB += FVector(Pixel.R,Pixel.G,Pixel.B);
                ++Result.Count;
            }
        }
        TestTrue(TEXT("normal probe mesh is visible in capture"),Result.Count>100);
        if (Result.Count>0) Result.RGB/=static_cast<double>(Result.Count);
        return Result;
    };
    const FMeanColor SingleNormalRestMean=MeanForegroundColor(SingleNormalRest);
    const FMeanColor SingleNormalBentMean=MeanForegroundColor(SingleNormalBent);
    const FMeanColor InstanceNormalRestMean=MeanForegroundColor(InstanceNormalRest);
    const FMeanColor InstanceNormalBentMean=MeanForegroundColor(InstanceNormalBent);
    const FVector SingleNormalDelta=SingleNormalBentMean.RGB-SingleNormalRestMean.RGB;
    const FVector InstanceNormalDelta=InstanceNormalBentMean.RGB-InstanceNormalRestMean.RGB;
    TestTrue(TEXT("wind visibly rotates SMC normals"),SingleNormalDelta.Size()>3.0);
    TestTrue(TEXT("wind visibly rotates ISM normals"),InstanceNormalDelta.Size()>3.0);
    TestTrue(TEXT("SMC and ISM rest normals share the same world frame"),
        (SingleNormalRestMean.RGB-InstanceNormalRestMean.RGB).Size()<1.0);
    TestTrue(TEXT("SMC and ISM bent normals share the same world frame"),
        (SingleNormalBentMean.RGB-InstanceNormalBentMean.RGB).Size()<1.0);
    TestTrue(TEXT("SMC and ISM normal rotation deltas agree"),
        (SingleNormalDelta-InstanceNormalDelta).Size()<1.0);
    TestTrue(TEXT("disabled wind exactly restores SMC normal output"),SingleNormalRest==SingleNormalRestored);
    TestTrue(TEXT("disabled wind exactly restores ISM normal output"),InstanceNormalRest==InstanceNormalRestored);
    AddInfo(FString::Printf(
        TEXT("MH_WIND_NORMAL smc_delta=(%.3f,%.3f,%.3f) ism_delta=(%.3f,%.3f,%.3f)"),
        SingleNormalDelta.X,SingleNormalDelta.Y,SingleNormalDelta.Z,
        InstanceNormalDelta.X,InstanceNormalDelta.Y,InstanceNormalDelta.Z));

    // Exercise the actual masked vertex-main leaf call with frozen gusts and
    // zero branch rotations. This catches a disconnected/gated-off detail path
    // even if the independent helper-equation tests still pass.
    const auto SetLeafTime = [&](float Time)
    {
        FMaterialUpdateContext Update;
        Update.AddMaterialInstance(Instance.Get());
        Instance->SetScalarParameterValueEditorOnly(TEXT("wind_angle_rot_base"),0);
        Instance->SetScalarParameterValueEditorOnly(TEXT("wind_angle_rot_level_mul"),0);
        Instance->SetScalarParameterValueEditorOnly(TEXT("MH_TestWindTime"),Time);
        Instance->PostEditChange();
    };
    Wind->SetVectorParameterValue(TEXT("MH_WindTree"),FLinearColor(0.5f,0.5f,0,0));
    SetLeafTime(0.25f);
    TArray<FColor> LeafSingleA, LeafInstanceA, LeafSingleB, LeafInstanceB;
    if (!Read(true,true,LeafSingleA) || !Read(false,true,LeafInstanceA)) return false;
    SetLeafTime(2.5f);
    if (!Read(true,true,LeafSingleB) || !Read(false,true,LeafInstanceB)) return false;
    TestTrue(TEXT("masked SMC leaf detail changes over time with pivot rotation disabled"),LeafSingleA!=LeafSingleB);
    TestTrue(TEXT("masked ISM leaf detail changes over time with pivot rotation disabled"),LeafInstanceA!=LeafInstanceB);
    const auto LeafDifference = [](const TArray<FColor>& A,const TArray<FColor>& B)
    {
        int32 Changed = 0;
        for(int32 I=0;I<A.Num();++I) if(A[I]!=B[I]) ++Changed;
        return Changed;
    };
    AddInfo(FString::Printf(TEXT("MH_LEAF_TIME smc_changed_pixels=%d ism_changed_pixels=%d frame_parity=(%d,%d)"),
        LeafDifference(LeafSingleA,LeafSingleB),LeafDifference(LeafInstanceA,LeafInstanceB),
        LeafDifference(LeafSingleA,LeafInstanceA),LeafDifference(LeafSingleB,LeafInstanceB)));
    TestTrue(TEXT("SMC and ISM leaf frame A agree"),LeafDifference(LeafSingleA,LeafInstanceA)<=4);
    TestTrue(TEXT("SMC and ISM leaf frame B agree"),LeafDifference(LeafSingleB,LeafInstanceB)<=4);
    return !HasAnyErrors();
}
}
