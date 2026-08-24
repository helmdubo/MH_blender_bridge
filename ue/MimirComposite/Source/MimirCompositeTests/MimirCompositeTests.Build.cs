using UnrealBuildTool;

public class MimirCompositeTests : ModuleRules
{
    public MimirCompositeTests(ReadOnlyTargetRules Target) : base(Target)
    {
        // C0 tests exercise editor-only import backends and headless commandlets.
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        bWarningsAsErrors = true;

        PrivateDependencyModuleNames.AddRange(
            new[]
            {
                "AssetRegistry",
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
