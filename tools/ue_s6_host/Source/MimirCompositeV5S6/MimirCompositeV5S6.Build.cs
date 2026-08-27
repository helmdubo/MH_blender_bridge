using UnrealBuildTool;

public class MimirCompositeV5S6 : ModuleRules
{
    public MimirCompositeV5S6(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        bWarningsAsErrors = true;
        PrivateDependencyModuleNames.AddRange(new[]
        {
            "Core", "CoreUObject", "Engine", "Json", "MimirCompositeRuntime"
        });
    }
}
