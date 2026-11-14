// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class NDGIRuntime : ModuleRules
{
	public NDGIRuntime(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"RenderCore",
				"RHI",
				"Projects",
			}
			);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
			}
			);
	}
}
