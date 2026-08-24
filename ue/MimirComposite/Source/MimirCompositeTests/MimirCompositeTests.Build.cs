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

        // S5 index acceptance generates one real material-bound FBX fixture
        // through the same pinned Autodesk SDK used by the editor translator.
        AddEngineThirdPartyPrivateStaticDependencies(Target, "FBX");
    }
}
