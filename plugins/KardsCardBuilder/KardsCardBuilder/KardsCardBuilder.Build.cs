using UnrealBuildTool;

public class KardsCardBuilder : ModuleRules
{
    public KardsCardBuilder(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        bLegacyPublicIncludePaths = false;

        PublicDependencyModuleNames.AddRange(new[]
        {
            "Core", "CoreUObject", "Engine"
        });

        PrivateDependencyModuleNames.AddRange(new[]
        {
            "UnrealEd",
            "BlueprintGraph",
            "Kismet",
            "KismetCompiler",
            "AssetRegistry",
            "Json",
            "JsonUtilities",
            "Projects",
            "kards",
            "KardsCore"
        });
    }
}
