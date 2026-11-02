#pragma once

#include "CoreMinimal.h"
#include "Configuration/ModConfiguration.h"
#include "Module/GameInstanceModule.h"
#include "TerrainBeltsConfig.generated.h"

namespace TerrainBelts
{
	constexpr float DefaultLengthMultiplier = 8.0f;
	constexpr float MinLengthMultiplier = 1.0f;
	constexpr float MaxLengthMultiplier = 32.0f;

	float GetLengthMultiplier(const AActor* Hologram);
	bool AllowUnevenSurfaces(const AActor* Hologram);
}

UCLASS()
class TERRAINBELTS_API UTerrainBeltsConfiguration : public UModConfiguration
{
	GENERATED_BODY()
public:
	UTerrainBeltsConfiguration(const FObjectInitializer& ObjectInitializer);
};

UCLASS()
class TERRAINBELTS_API UTerrainBeltsGameInstanceModule : public UGameInstanceModule
{
	GENERATED_BODY()
public:
	UTerrainBeltsGameInstanceModule();
};
