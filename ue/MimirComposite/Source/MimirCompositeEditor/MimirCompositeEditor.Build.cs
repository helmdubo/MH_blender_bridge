using UnrealBuildTool;

public class MimirCompositeEditor : ModuleRules
{
    public MimirCompositeEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        bWarningsAsErrors = true;

        PublicDependencyModuleNames.AddRange(
            new[]
            {
                "Core",
                "CoreUObject",
                "Engine",
                "MeshDescription",
                "MimirCompositeRuntime",
                "StaticMeshDescription"
            });

        PrivateDependencyModuleNames.AddRange(
            new[]
            {
                "Json",
                "Projects",
                "UnrealEd"
            });

        AddEngineThirdPartyPrivateStaticDependencies(Target, "FBX");
    }
}
