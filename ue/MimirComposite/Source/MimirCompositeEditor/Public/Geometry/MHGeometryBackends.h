#pragma once

#include "CoreMinimal.h"

class UStaticMesh;

struct FMHGeometryProbeResult
{
    UStaticMesh* StaticMesh = nullptr;
    int32 SourceVertexCount = 0;
    int32 SourcePolygonCount = 0;
    int32 MeshDescriptionVertexCount = 0;
    int32 MeshDescriptionPolygonCount = 0;
    int32 RenderVertexCount = 0;
    int32 RenderTriangleCount = 0;
    TArray<FVector3f> MeshDescriptionPositions;
    TArray<FVector3f> RenderPositions;
};

class MIMIRCOMPOSITEEDITOR_API FMHFbxBackend final
{
public:
    static bool ImportAxisProbe(
        const FString& Filename,
        FMHGeometryProbeResult& OutResult,
        FString& OutError);
};

class MIMIRCOMPOSITEEDITOR_API FMHLegacyFbxBackend final
{
public:
    static bool ImportAxisProbe(
        const FString& Filename,
        FMHGeometryProbeResult& OutResult,
        FString& OutError);
};
