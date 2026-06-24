using UnrealBuildTool;

public class MiniWorldSim : ModuleRules
{
    public MiniWorldSim(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "HTTP",
            "ImageCore",
            "ImageWrapper",
            "InputCore",
            "Json",
            "JsonUtilities",
            "ProceduralMeshComponent",
            "RenderCore"
        });
    }
}
