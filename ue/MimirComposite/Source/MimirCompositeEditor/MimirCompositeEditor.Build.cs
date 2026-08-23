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
                "DesktopPlatform",
                "AssetRegistry",
                "Json",
                "MessageLog",
                "Projects",
                "Slate",
                "SlateCore",
                "ToolMenus",
                "UnrealEd"
            });

        AddEngineThirdPartyPrivateStaticDependencies(Target, "FBX");
    }
}
