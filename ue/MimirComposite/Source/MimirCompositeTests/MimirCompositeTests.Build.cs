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
                "AssetTools",
                "Core",
                "CoreUObject",
                "ContentBrowser",
                "Engine",
                // V5-S6.2 routes a real viewport click through EKeys.
                "InputCore",
                "ImageCore",
                "Json",
                "LevelEditor",
                "MeshDescription",
                "MimirCompositeEditor",
                "MimirCompositeRuntime",
                "PhysicsCore",
                "Projects",
                "RenderCore",
                "RHI",
                "Slate",
                "SlateCore",
                "StaticMeshDescription",
                "ToolMenus",
                "UnrealEd",
                "TypedElementFramework",
                "TypedElementRuntime"
            });

        // S5 index acceptance generates one real material-bound FBX fixture
        // through the same pinned Autodesk SDK used by the editor translator.
        AddEngineThirdPartyPrivateStaticDependencies(Target, "FBX");
    }
}
