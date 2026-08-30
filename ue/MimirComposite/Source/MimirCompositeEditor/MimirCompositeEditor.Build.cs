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
                "PhysicsCore",
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
                "InputCore",
                "Json",
                "LevelEditor",
                "MessageLog",
                "Projects",
                "PropertyEditor",
                "Slate",
                "SlateCore",
                "SQLiteCore",
                "ToolMenus",
                "TypedElementRuntime",
                "UnrealEd",
                "WorkspaceMenuStructure"
            });

        // Retained direct Autodesk FBX SDK -> IMHGeometryTranslator seam.
        AddEngineThirdPartyPrivateStaticDependencies(Target, "FBX");
    }
}
