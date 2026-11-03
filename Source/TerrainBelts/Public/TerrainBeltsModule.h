#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class AActor;
class AFGBuildable;
class AFGBuildableHologram;
class AFGConveyorBeltHologram;
class AFGConveyorLiftHologram;
class AFGHologram;

namespace TerrainBelts
{
	struct FDrapeSettings;
}

class FTerrainBeltsModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	// Members rather than free functions: the AccessTransformers friends grant this class
	// access to the holograms' protected/private state, and the hook lambdas inside
	// RegisterHooks share that access.
	static void RegisterHooks();
	static void RemoveUnevenSurfaceDisqualifier(AFGBuildableHologram* Hologram);
	static bool IsExemptFromUnevenFloor(const AFGHologram* Hologram);
	static bool ShouldFollowTerrain(const AFGHologram* Hologram);

	// Auto-lift: in Follow Terrain, a belt that meets a cliff too steep for it is split into
	// belt -> lift -> belt, using a lift and a second belt spawned as disabled children.
	static void SpawnAutoLiftChildren(AFGConveyorBeltHologram* Belt, AActor* HologramOwner, const FVector& SpawnLocation);
	static bool ApplyAutoLift(AFGConveyorBeltHologram* Belt, const TerrainBelts::FDrapeSettings& Settings);
	static bool IsDraggedAgainstFlow(const AFGConveyorBeltHologram* Belt);
	static void DisableAutoLift(AFGConveyorBeltHologram* Belt);
	static bool PlaceLift(AFGConveyorLiftHologram* Lift, const FVector& Entry, const FVector& Direction, double SignedHeight, FVector& OutExit);
	static void RestoreChildBelt(AFGConveyorBeltHologram* ChildBelt);
	static void ConnectBuiltPieces(const AFGHologram* Hologram, AFGBuildable* Buildable);
};
