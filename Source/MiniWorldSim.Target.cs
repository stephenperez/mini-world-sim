using UnrealBuildTool;
using System.Collections.Generic;

public class MiniWorldSimTarget : TargetRules
{
    public MiniWorldSimTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Game;
        DefaultBuildSettings = BuildSettingsVersion.V5;
        IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_6;
        CppStandard = CppStandardVersion.Cpp20;
        ExtraModuleNames.Add("MiniWorldSim");
    }
}
