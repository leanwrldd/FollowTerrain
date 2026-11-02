#pragma once

#include "CoreMinimal.h"
#include "Hologram/FGHologramBuildModeDescriptor.h"
#include "TerrainBeltsBuildMode.generated.h"

/// Spline build mode (belts, pipes, rails) that drapes the auto-routed spline over the ground.
UCLASS()
class TERRAINBELTS_API UTerrainBeltsBuildMode : public UFGHologramBuildModeDescriptor
{
	GENERATED_BODY()
public:
	UTerrainBeltsBuildMode()
	{
		mDisplayName = INVTEXT("Follow Terrain");
	}
};

/// Plain vanilla routing, for holograms that have no build modes of their own (rails), so
/// cycling can leave Follow Terrain again.
UCLASS()
class TERRAINBELTS_API UTerrainBeltsDefaultBuildMode : public UFGHologramBuildModeDescriptor
{
	GENERATED_BODY()
public:
	UTerrainBeltsDefaultBuildMode()
	{
		mDisplayName = INVTEXT("Default");
	}
};
