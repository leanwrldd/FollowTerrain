#include "TerrainBeltsConfig.h"

#include "Configuration/ConfigManager.h"
#include "Configuration/Properties/ConfigPropertyBool.h"
#include "Configuration/Properties/WidgetExtension/CP_Float.h"
#include "Configuration/Properties/WidgetExtension/CP_Section.h"
#include "Engine/GameInstance.h"
#include "GameFramework/Actor.h"

namespace TerrainBelts
{
	static const TCHAR* const LengthMultiplierPropertyName = TEXT("LengthMultiplier");
	static const TCHAR* const AllowUnevenSurfacesPropertyName = TEXT("AllowUnevenSurfaces");

	bool AllowUnevenSurfaces(const AActor* Hologram)
	{
		UGameInstance* GameInstance = Hologram != nullptr ? Hologram->GetGameInstance() : nullptr;
		const UConfigManager* Manager = GameInstance != nullptr ? GameInstance->GetSubsystem<UConfigManager>() : nullptr;
		FConfigId Id;
		Id.ModReference = TEXT("TerrainBelts");
		const UConfigPropertySection* Root = Manager != nullptr ? Manager->GetConfigurationRootSection(Id) : nullptr;
		const UConfigPropertyBool* Property = Root != nullptr
			? Cast<UConfigPropertyBool>(Root->SectionProperties.FindRef(AllowUnevenSurfacesPropertyName)) : nullptr;
		return Property != nullptr && Property->Value;
	}

	float GetLengthMultiplier(const AActor* Hologram)
	{
		UGameInstance* GameInstance = Hologram != nullptr ? Hologram->GetGameInstance() : nullptr;
		const UConfigManager* Manager = GameInstance != nullptr ? GameInstance->GetSubsystem<UConfigManager>() : nullptr;
		FConfigId Id;
		Id.ModReference = TEXT("TerrainBelts");
		const UConfigPropertySection* Root = Manager != nullptr ? Manager->GetConfigurationRootSection(Id) : nullptr;
		const UConfigPropertyFloat* Property = Root != nullptr
			? Cast<UConfigPropertyFloat>(Root->SectionProperties.FindRef(LengthMultiplierPropertyName)) : nullptr;
		const float Value = Property != nullptr ? Property->Value : DefaultLengthMultiplier;
		return FMath::IsFinite(Value) ? FMath::Clamp(Value, MinLengthMultiplier, MaxLengthMultiplier) : DefaultLengthMultiplier;
	}
}

UTerrainBeltsConfiguration::UTerrainBeltsConfiguration(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	using namespace TerrainBelts;
	ConfigId.ModReference = TEXT("TerrainBelts");
	DisplayName = FText::FromString(TEXT("Follow Terrain"));
	Description = FText::FromString(TEXT("Placement settings for belts, pipelines, hypertubes, railway tracks and their supports."));

	// SML's Blueprint subclasses supply the settings widgets; the native classes do not.
	UClass* SectionClass = StaticLoadClass(UCP_Section::StaticClass(), nullptr,
		TEXT("/SML/Interface/UI/Menu/Mods/ConfigProperties/BP_ConfigPropertySection.BP_ConfigPropertySection_C"));
	UClass* FloatClass = StaticLoadClass(UCP_Float::StaticClass(), nullptr,
		TEXT("/SML/Interface/UI/Menu/Mods/ConfigProperties/BP_ConfigPropertyFloat.BP_ConfigPropertyFloat_C"));
	if (!ensureMsgf(SectionClass != nullptr && FloatClass != nullptr, TEXT("Follow Terrain could not load SML's configuration widgets")))
	{
		return;
	}

	UCP_Section* Root = NewObject<UCP_Section>(this, SectionClass, TEXT("RootSection"));
	Root->WidgetType = ECP_SectionWidgetType::CPS_Vertical;
	RootSection = Root;

	UCP_Float* Multiplier = NewObject<UCP_Float>(Root, FloatClass, LengthMultiplierPropertyName);
	Multiplier->DisplayName = FText::FromString(TEXT("Maximum length multiplier"));
	Multiplier->Tooltip = FText::FromString(TEXT("Multiplies each type's normal maximum length, including conveyor lift height, in all build modes. 1 uses the normal limit; 8 allows eight times the length. Applies when you start a new placement. Use the same value on every player and server in multiplayer."));
	Multiplier->WidgetType = ECP_FloatWidgetType::CPF_Spinbox;
	Multiplier->MinValue = MinLengthMultiplier;
	Multiplier->MaxValue = MaxLengthMultiplier;
	Multiplier->DefaultValue = DefaultLengthMultiplier;
	Multiplier->Value = DefaultLengthMultiplier;
	Multiplier->bRequiresWorldReload = false;
	Root->SectionProperties.Add(LengthMultiplierPropertyName, Multiplier);

	UClass* BoolClass = StaticLoadClass(UConfigPropertyBool::StaticClass(), nullptr,
		TEXT("/SML/Interface/UI/Menu/Mods/ConfigProperties/BP_ConfigPropertyBool.BP_ConfigPropertyBool_C"));
	if (!ensureMsgf(BoolClass != nullptr, TEXT("Follow Terrain could not load SML's checkbox widget")))
	{
		return;
	}
	UConfigPropertyBool* UnevenSurfaces = NewObject<UConfigPropertyBool>(Root, BoolClass, AllowUnevenSurfacesPropertyName);
	UnevenSurfaces->DisplayName = FText::FromString(TEXT("Allow uneven surfaces"));
	UnevenSurfaces->Tooltip = FText::FromString(TEXT("Removes the surface-too-uneven restriction for belts, pipelines, hypertubes, railway tracks and their supports in all build modes. Other placement checks still apply. Takes effect immediately. Use the same setting on every player and server in multiplayer."));
	UnevenSurfaces->DefaultValue = false;
	UnevenSurfaces->Value = false;
	UnevenSurfaces->bRequiresWorldReload = false;
	Root->SectionProperties.Add(AllowUnevenSurfacesPropertyName, UnevenSurfaces);
}

UTerrainBeltsGameInstanceModule::UTerrainBeltsGameInstanceModule()
{
	bRootModule = true;
	ModConfigurations.Add(UTerrainBeltsConfiguration::StaticClass());
}
