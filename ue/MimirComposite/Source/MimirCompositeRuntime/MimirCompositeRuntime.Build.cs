using UnrealBuildTool;

public class MimirCompositeRuntime : ModuleRules
{
    public MimirCompositeRuntime(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        bWarningsAsErrors = true;

        PublicDependencyModuleNames.AddRange(
            new[]
            {
                "Core",
                "CoreUObject",
                "Engine",
                "Json"
            });

        if (Target.bCompileICU)
        {
            AddEngineThirdPartyPrivateStaticDependencies(Target, "ICU");
        }
    }
}
