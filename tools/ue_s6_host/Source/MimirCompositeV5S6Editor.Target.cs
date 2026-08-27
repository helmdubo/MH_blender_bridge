using UnrealBuildTool;

public class MimirCompositeV5S6EditorTarget : TargetRules
{
    public MimirCompositeV5S6EditorTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Editor;
        DefaultBuildSettings = BuildSettingsVersion.V6;
        IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_7;
        ExtraModuleNames.Add("MimirCompositeV5S6");
    }
}
