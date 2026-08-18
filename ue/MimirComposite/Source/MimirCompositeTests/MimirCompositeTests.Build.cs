using UnrealBuildTool;

public class MimirCompositeTests : ModuleRules
{
    public MimirCompositeTests(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        bWarningsAsErrors = true;

        PrivateDependencyModuleNames.AddRange(
            new[]
            {
                "Core",
                "CoreUObject",
                "Engine",
                "Json",
                "MeshDescription",
                "MimirCompositeEditor",
                "MimirCompositeRuntime",
                "Projects",
                "StaticMeshDescription",
                "UnrealEd"
            });
    }
}
