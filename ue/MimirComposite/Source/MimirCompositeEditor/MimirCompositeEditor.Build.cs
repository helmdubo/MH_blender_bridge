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
                "DeveloperSettings",
                "EditorSubsystem",
                "Engine",
                "MeshDescription",
                "MimirCompositeRuntime",
                "StaticMeshDescription"
            });

        PrivateDependencyModuleNames.AddRange(
            new[]
            {
                "ApplicationCore",
                "AssetTools",
                "AssetRegistry",
                "ContentBrowser",
                "DirectoryWatcher",
                "Json",
                "LevelEditor",
                "MessageLog",
                "Projects",
                "Slate",
                "SlateCore",
                "SQLiteCore",
                "ToolMenus",
                "TypedElementRuntime",
                "UnrealEd"
            });

        // Retained direct Autodesk FBX SDK -> IMHGeometryTranslator seam.
        AddEngineThirdPartyPrivateStaticDependencies(Target, "FBX");
    }
}
