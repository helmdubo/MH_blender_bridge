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
                "AssetTools",
                "AssetRegistry",
                "Json",
                "MessageLog",
                "Projects",
                "Slate",
                "SlateCore",
                "SQLiteCore",
                "UnrealEd"
            });

        // Retained direct Autodesk FBX SDK -> IMHGeometryTranslator seam.
        AddEngineThirdPartyPrivateStaticDependencies(Target, "FBX");
    }
}
