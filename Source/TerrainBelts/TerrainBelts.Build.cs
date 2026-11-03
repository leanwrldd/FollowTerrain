using UnrealBuildTool;

public class TerrainBelts : ModuleRules
{
	public TerrainBelts(ReadOnlyTargetRules Target) : base(Target)
	{
		CppStandard = CppStandardVersion.Cpp20;
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		bLegacyPublicIncludePaths = false;

		PublicDependencyModuleNames.AddRange(
			new[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"InputCore",
				"EnhancedInput",
				"UMG",
				"Slate",
				"SlateCore",
				"Foliage",
				"NetCore",
				"FactoryGame",
				"SML"
			}
		);
	}
}
