using UnrealBuildTool;

public class MimirCompositeV5S6Target : TargetRules
{
    public MimirCompositeV5S6Target(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Game;
        DefaultBuildSettings = BuildSettingsVersion.V6;
        IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_7;
        ExtraModuleNames.Add("MimirCompositeV5S6");
    }
}
