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
                "DesktopPlatform",
                "EditorFramework",
                "EditorInteractiveToolsFramework",
                "InteractiveToolsFramework",
                "DirectoryWatcher",
                "InputCore",
                "ImageCore",
                "Json",
                "LevelEditor",
                "MessageLog",
                "Projects",
                "PropertyEditor",
                // FMaterialUpdateContext defaults to GMaxRHIShaderPlatform.
                "RHI",
                "Slate",
                "SlateCore",
                "SQLiteCore",
                "ToolMenus",
                "ToolWidgets",
                "TypedElementFramework",
                "TypedElementRuntime",
                "UnrealEd",
                "WorkspaceMenuStructure"
            });

        // Retained direct Autodesk FBX SDK -> IMHGeometryTranslator seam.
        AddEngineThirdPartyPrivateStaticDependencies(Target, "FBX");
    }
}
