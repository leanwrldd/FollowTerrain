#include "TerrainBeltsModule.h"

#include "TerrainBeltsConfig.h"
#include "EnhancedPlayerInput.h"
#include "FGCharacterPlayer.h"
#include "FGConstructDisqualifier.h"
#include "FGFactoryConnectionComponent.h"
#include "FGPlayerController.h"
#include "FGRecipe.h"
#include "FGRecipeManager.h"
#include "Algo/Reverse.h"
#include "Buildables/FGBuildable.h"
#include "Buildables/FGBuildableConveyorBelt.h"
#include "Buildables/FGBuildableConveyorLift.h"
#include "Buildables/FGBuildablePipeHyper.h"
#include "Buildables/FGBuildablePoleConveyor.h"
#include "Buildables/FGBuildablePoleStackable.h"
#include "Hologram/FGConveyorBeltHologram.h"
#include "Hologram/FGConveyorLiftHologram.h"
#include "Hologram/FGConveyorMultiPoleHologram.h"
#include "Hologram/FGConveyorPoleHologram.h"
#include "Hologram/FGFactoryBuildingHologram.h"
#include "Hologram/FGPipelineHologram.h"
#include "Hologram/FGPipelinePoleHologram.h"
#include "Hologram/FGRailroadTrackHologram.h"
#include "Blueprint/UserWidget.h"
#include "Engine/AssetManager.h"
#include "Equipment/FGBuildGun.h"
#include "Equipment/FGBuildGunBuild.h"
#include "InputAction.h"
#include "Input/FGChildInputMappingContext.h"
#include "InstancedFoliageActor.h"
#include "Patching/NativeHookManager.h"
#include "UObject/ObjectKey.h"
#include "UObject/UObjectIterator.h"

DEFINE_LOG_CATEGORY_STATIC(LogTerrainBelts, Log, All);

// Belts split into belt -> lift -> belt at cliffs too steep for them. Not yet tested in game;
// set to 0 to switch it off without touching the draping.
#define TERRAINBELTS_ENABLE_AUTO_LIFT 1

namespace TerrainBelts
{
	struct FDrapeSettings
	{
		// Longest stretch between two draped points (cm). Shorter follows the ground more
		// closely but makes a heavier spline (vehicle paths use 100).
		double StepLength;

		// Height of the spline above the ground (cm). Tuned against each mesh in game.
		double GroundClearance;

		// Steepest slope the draped spline may take (degrees).
		double MaxInclineDegrees;

		// Smoothing passes over the draped heights, so bumpy ground doesn't trip curvature checks.
		int32 SmoothingPasses;

		// Ground traces start at least this far above the higher end (cm), even where the
		// incline couldn't climb that high. Auto-lift uses it to see cliffs above the belt.
		double ExtraTraceHeadroom = 0.0;

		// Ground is also traced this far to either side of the centre line (cm), so a ledge under
		// the edge of a wide mesh counts. 0 traces the centre only.
		double HalfWidth = 0.0;

		// Extra ground checks along each segment, this far apart (cm), so ground rising between two
		// points (a rock edge) can't poke through. 0 checks the points only.
		double GroundSampleSpacing = 0.0;

		// Tightest vertical bend (cm). Kinks sharper than this are rounded off by raising the spline
		// around them. 0 leaves bends alone.
		double VerticalBendRadius = 0.0;

		// Re-shape the horizontal route through its points (Catmull-Rom), so corners left where a
		// vanilla route went vertical become curves. Pipes only: belts keep vanilla's exact bends.
		bool bSmoothRoute = false;

		// Tightest bend the hologram accepts, with some margin (cm). Smoothing keeps points far
		// enough apart for it, and pipe steps bend at least this wide.
		double MinBendRadius = 0.0;

		// Level belt/pipe supports over a flat surface keep their height instead of sagging down to
		// GroundClearance between the supports.
		bool bKeepLevelOnFlatGround = false;

		// A vanilla route that stays clear of the ground and never floats more than this above
		// ground + GroundClearance (cm) is kept as it is: dragging a short belt between two machine
		// ports down towards an uneven floor only bent it. 0 always drapes.
		double MaxKeptFloat = 0.0;

		// Never go below the vanilla spline, only over ground that pokes above it (rails: a
		// straight grade is what you want, not one that dips into every hollow).
		bool bOnlyRaise = false;

		// Belts must reject ground that cannot be reached from their fixed endpoints.
		bool bEnforceIncline = false;
	};

	// Belts: incline comes from the hologram's mMaxIncline, minus this margin so draped slopes
	// don't land exactly on the validation limit.
	constexpr double BeltStepLength = 400.0;
	constexpr double BeltClearance = 50.0;
	constexpr double BeltMaxKeptFloat = 150.0; // a belt up to ~2 m above the ground keeps vanilla's shape
	constexpr double BeltInclineMarginDegrees = 1.0;
	constexpr int32 BeltSmoothingPasses = 1;
	constexpr double BeltHalfWidth = 60.0;
	constexpr double BeltGroundSampleSpacing = 100.0;
	constexpr double BeltVerticalBendRadius = 2500.0;

	// Pipes have no incline limit, but a cap turns an elevated end into a ramp rather than a
	// near-vertical drop. The spline runs through the pipe's centre.
	constexpr double PipeStepLength = 400.0;
	constexpr double PipeClearance = 60.0; // fallback if vanilla reports no hover height
	constexpr double PipeMaxInclineDegrees = 60.0;
	constexpr int32 PipeSmoothingPasses = 2;
	constexpr double PipeHalfWidth = 50.0;
	constexpr double PipeGroundSampleSpacing = 100.0;
	constexpr double PipeVerticalBendRadius = 1500.0;

	// Hypertubes are pipeline holograms too, with a much thicker tube (the spline runs through
	// its centre) and wider bends; the rest of the pipe settings apply.
	constexpr double HypertubeClearance = 150.0;
	constexpr double HypertubeHalfWidth = 120.0;
	constexpr double HypertubeVerticalBendRadius = 3000.0;

	// Pipeline holograms reject bends tighter than 1.05 x mMinBendRadius (seen in the 1.2
	// disassembly of ValidateCurvatureAndReturnFaultyPosition); stay well clear of that.
	constexpr double PipelineBendMargin = 1.5;

	// Smoothed pipe routes keep points at least this far apart (cm).
	constexpr double MinRouteSpacing = 150.0;

	// Steep stretches tall enough for two elbows become steps. Trace headroom still covers
	// metre-scale cliffs; shorter stairs are resolved with denser points in StepPipeSpline.
	constexpr double PipeStepMinRise = 1000.0;
	constexpr double PipeStepMinSlopeDegrees = 45.0;
	// The step's two 90 degree bends have this radius (vanilla's 2D pipe bend radius is 199 cm).
	constexpr double PipeRiserBendRadius = 200.0;
	// Hermite tangent length, as a multiple of the radius, closest to a quarter circle.
	constexpr double PipeBendTangentScale = 1.657;
	// Shortest straight piece kept between a bend and anything else (cm).
	constexpr double PipeMinStraight = 50.0;
	// The top of a step clears the highest ground of the slope by this much (cm).
	constexpr double PipeRiseMargin = 25.0;
	// The ground has started to rise (or still hasn't reached the foot) once it is this much
	// above the foot (cm); the step's vertical part stands this far from that spot (cm), so
	// the pipe and its top bend clear the edge.
	constexpr double PipeStepFootTolerance = 50.0;
	constexpr double PipeStepWallClearance = 100.0;

	// Furthest a pipe hanging from a vertical end drops straight down to reach the ground (cm).
	constexpr double MaxRiserDrop = 5000.0;

	// Pipes leave a fixed end tangent along a bend at least this many times the hologram's
	// tightest accepted bend.
	constexpr double EndExitBendScale = 4.0;

	// Longest tangent relative to its segment's chord. Our 90 degree bends use 1.657 x radius
	// over a chord of 1.414 x radius (1.17), so this leaves them alone.
	constexpr double MaxTangentToChord = 1.25;

	// Rails: ValidateGrade rejects slopes over (mMaxGrade - 1) degrees but only samples three
	// points, so the drape enforces the grade itself. Rails bend far less tightly than belts,
	// hence the longer steps and extra smoothing. Vanilla tracks sit on the hit point, so no
	// clearance.
	constexpr double RailStepLength = 1000.0;
	constexpr double RailClearance = 0.0;
	constexpr double RailGradeMarginDegrees = 2.0;
	constexpr int32 RailSmoothingPasses = 3;

	// Ground traces start this far above the highest the spline could climb at that point (cm),
	// and search this far below the lowest end (cm). Matches the vehicle path hologram.
	constexpr double TraceElevation = 200.0;
	constexpr double TraceSearchDistance = 2000.0;

	// Foundations and walls count as ground only up to this far above the higher end (cm);
	// higher ones are platforms the spline passes under.
	constexpr double BuildingFloorTolerance = 150.0;

	// Give up looking for ground after skipping this many buildables, holograms and foliage.
	constexpr int32 MaxSkippedHits = 8;

	// Ground found more than OverhangMinRise above the route (cm) may be the top of an overhang
	// (a rock arch, a cave roof) with open floor under it. A floor below counts only with at
	// least OverhangMinHeadroom of open space above it, and up to OverhangMaxLayers surfaces are
	// looked through. The route's own height is also probed from OverhangProbeHeight above it.
	constexpr double OverhangMinRise = 300.0;
	constexpr double OverhangMinHeadroom = 400.0;
	constexpr int32 OverhangMaxLayers = 3;
	constexpr double OverhangProbeHeight = 200.0;
	// Passing over an overhang must climb at least this much less (cm) than passing under it, or
	// the line goes under. Rock tops are uneven and narrow, so a lift down at the start and up at
	// the end over open floor beats riding the rock.
	constexpr double OverhangClimbTolerance = 300.0;

	// Points closer than this horizontally to the previous one (cm) are dropped. Vertical runs
	// in a vanilla route (pipes climbing to an elevated end) would otherwise collapse onto one
	// spot once draped; the slope limit replaces them with a ramp.
	constexpr double MinHorizontalSpacing = 25.0;

	// No draped point may end up more than this far above the highest ground or end it rests on
	// (cm); anything higher is a bug, and vanilla's shape is kept instead.
	constexpr double MaxDrapeRise = 2000.0;

	// Auto-lift only for a steep stretch of ground rising or falling at least this much (cm),
	// with at least this much belt (horizontal cm) on either side of the lift.
	constexpr double AutoLiftMinRise = 300.0;
	constexpr double AutoLiftMinBeltLength = 300.0;

	// Lifts only when the belt without them would float more than this above the ground (cm)
	// somewhere, or exceed its incline by more than this factor (rounding aside).
	constexpr double AutoLiftMaxFloat = 300.0;
	constexpr double AutoLiftSteepnessTolerance = 1.02;

	// Fallbacks if the lift hologram hasn't read its limits from the buildable (cm).
	constexpr double FallbackLiftStepHeight = 100.0;
	constexpr double FallbackLiftMinHeight = 200.0;
	constexpr double FallbackLiftMaxHeight = 1700.0;

	// How far the lift's exit may sit from where the top transform says it should (cm) before
	// we stop trusting the hologram's connection component.
	constexpr double LiftExitTolerance = 100.0;

	// Up to this many lifts per belt. Each takes a hidden lift child and a hidden belt child: the
	// main belt runs to lift 0, belt child k runs from lift k to lift k + 1 (or the end).
	constexpr int32 MaxAutoLifts = 3;

	FName LiftChildName(int32 Index)
	{
		return FName(TEXT("TerrainBeltsLift"), Index + 1);
	}

	FName BeltChildName(int32 Index)
	{
		return FName(TEXT("TerrainBeltsBelt"), Index + 1);
	}

	// Which of our children Hologram is, if any.
	bool FindAutoLiftChild(const AFGHologram* Parent, const AFGHologram* Hologram, bool& bOutIsLift, int32& OutIndex)
	{
		for (int32 Index = 0; Index < MaxAutoLifts; ++Index)
		{
			if (Hologram == Parent->FindChildHologramByName(LiftChildName(Index)) || Hologram == Parent->FindChildHologramByName(BeltChildName(Index)))
			{
				bOutIsLift = Hologram == Parent->FindChildHologramByName(LiftChildName(Index));
				OutIndex = Index;
				return true;
			}
		}
		return false;
	}

	// Lifts in use on a main belt hologram; they are always the first ones.
	int32 NumActiveLifts(const AFGHologram* MainBelt)
	{
		int32 Count = 0;
		while (Count < MaxAutoLifts)
		{
			const AFGHologram* Lift = MainBelt->FindChildHologramByName(LiftChildName(Count));
			if (Lift == nullptr || Lift->IsDisabled())
			{
				break;
			}
			++Count;
		}
		return Count;
	}

	struct FChildBeltState
	{
		FTransform Transform;
		TArray<FSplinePointData> SplineData;
	};

	// Where each belt child of a split should be. Its own placement re-routes it from the hit
	// result every frame, so the shape is put back after that.
	TMap<FObjectKey, FChildBeltState> ChildBeltStates;

	// Pieces of a split built so far, keyed by the main belt hologram, so each piece can be
	// connected to its neighbours as soon as both exist (before the later one begins play).
	struct FBuiltSplit
	{
		// Belts[0] is the main belt; Belts[k + 1] leaves Lifts[k].
		TWeakObjectPtr<AFGBuildableConveyorBelt> Belts[MaxAutoLifts + 1];
		TWeakObjectPtr<AFGBuildableConveyorLift> Lifts[MaxAutoLifts];
	};
	TMap<FObjectKey, FBuiltSplit> BuiltSplits;

	// Construction Preferences, with its conveyor pole type set to none, destroys on the next tick
	// whatever a child of a belt hologram builds, assuming every child is a pole. While the main
	// belt constructs, our split children are detached from it so that check skips them; this maps
	// each detached child to its main belt for the code that still needs it.
	TMap<FObjectKey, TWeakObjectPtr<AFGConveyorBeltHologram>> DetachedSplitChildren;

	// Vanilla rail GenerateAndUpdateSpline only writes mSplineData[0] and [1] in place and never
	// resizes the array (seen in the 1.2 disassembly), unlike belts. Run on a draped spline, it
	// moves the end into slot 1 and keeps the stale draped tail, which the next drape subdivides
	// again: the spline loops and grows every frame. So vanilla's own spline is kept per hologram
	// and put back before vanilla runs, as long as nothing else changed the draped one meanwhile.
	struct FRailSplines
	{
		TArray<FSplinePointData> Vanilla;
		TArray<FSplinePointData> Draped;
	};
	TMap<FObjectKey, FRailSplines> RailSplines;

	// Curvature alone does not make a pipe safe: the final fallback can run through a floor.
	TSet<FObjectKey> PipelineGroundFailures;

	// Holograms dropped while in Follow Terrain leave an entry behind.
	constexpr int32 MaxRailSplinesBeforePrune = 32;

	bool SameSplineData(const TArray<FSplinePointData>& A, const TArray<FSplinePointData>& B)
	{
		if (A.Num() != B.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < A.Num(); ++Index)
		{
			if (!A[Index].Location.Equals(B[Index].Location, 0.0) || !A[Index].ArriveTangent.Equals(B[Index].ArriveTangent, 0.0) || !A[Index].LeaveTangent.Equals(B[Index].LeaveTangent, 0.0))
			{
				return false;
			}
		}
		return true;
	}

	bool FirstTime(const TCHAR* Key)
	{
		static TSet<FString> Seen;
		bool bAlreadySeen = false;
		Seen.Add(Key, &bAlreadySeen);
		return !bAlreadySeen;
	}

	bool ShouldSkipGroundHit(const FHitResult& Hit)
	{
		const AActor* Actor = Hit.GetActor();
		return Actor != nullptr && (Actor->IsA<AFGBuildable>() || Actor->IsA<AFGHologram>() || Actor->IsA<AInstancedFoliageActor>());
	}

	// Foundations, walls and ramps aren't buildable actors in 1.2: they are instances drawn by an
	// instance manager actor, so they need recognising by that actor's class.
	bool IsInstancedBuildingHit(const FHitResult& Hit)
	{
		const AActor* Actor = Hit.GetActor();
		if (Actor == nullptr)
		{
			return false;
		}
		const FString ClassName = Actor->GetClass()->GetName();
		return ClassName.Contains(TEXT("AbstractInstanceManager")) || ClassName.Contains(TEXT("LightweightBuildable"));
	}

	// First surface counting as ground on the way down from Top to Bottom. Params gathers the
	// skipped hits, so a caller can add more to ignore and trace again.
	bool TraceGround(const UWorld* World, const FVector2D& XY, double Top, double Bottom, double MaxBuildingZ, FCollisionQueryParams& Params, FHitResult& OutHit)
	{
		const FCollisionObjectQueryParams ObjectParams(ECC_WorldStatic);
		const FVector Start(XY, Top);
		const FVector End(XY, Bottom);

		for (int32 Attempt = 0; Attempt <= MaxSkippedHits; ++Attempt)
		{
			FHitResult Hit;
			if (!World->LineTraceSingleByObjectType(Hit, Start, End, ObjectParams, Params))
			{
				return false;
			}
			const bool bInstancedBuilding = IsInstancedBuildingHit(Hit);
			if (FirstTime(*FString::Printf(TEXT("ground class %s"), *GetNameSafe(Hit.GetActor() ? Hit.GetActor()->GetClass() : nullptr))))
			{
				UE_LOG(LogTerrainBelts, Display, TEXT("Ground trace hit %s (%s), instanced building: %s"),
					*GetNameSafe(Hit.GetActor()), *GetNameSafe(Hit.GetComponent()), bInstancedBuilding ? TEXT("yes") : TEXT("no"));
			}
			const bool bSkipActor = ShouldSkipGroundHit(Hit);
			if (!bSkipActor && !(bInstancedBuilding && Hit.ImpactPoint.Z > MaxBuildingZ))
			{
				OutHit = Hit;
				return true;
			}
			// A buildable can have several collision components (a pipe floor hole has a few);
			// skipping them one by one used up every skip. An instance manager draws every
			// foundation, though, so only that one instance's component is skipped.
			if (bSkipActor && Hit.GetActor() != nullptr)
			{
				Params.AddIgnoredActor(Hit.GetActor());
			}
			else
			{
				Params.AddIgnoredComponent(Hit.GetComponent());
			}
		}
		return false;
	}

	// Whether a floor at FloorZ has open space above it: straight up from it, the first thing met
	// is the underside of something (facing down) at least OverhangMinHeadroom higher. A trace
	// starting inside a rock finds nothing, or a face pointing up, so ground buried in a hill
	// doesn't count.
	bool HasOpenSpaceAbove(const UWorld* World, const AActor* Hologram, const FVector2D& XY, double FloorZ, double TopZ)
	{
		FCollisionQueryParams Params(SCENE_QUERY_STAT(TerrainBeltsOverhang), false, Hologram);
		const FCollisionObjectQueryParams ObjectParams(ECC_WorldStatic);
		const FVector Start(XY, FloorZ + 5.0);
		const FVector End(XY, TopZ);
		for (int32 Attempt = 0; Attempt <= MaxSkippedHits; ++Attempt)
		{
			FHitResult Hit;
			if (!World->LineTraceSingleByObjectType(Hit, Start, End, ObjectParams, Params))
			{
				return false;
			}
			if (ShouldSkipGroundHit(Hit) && Hit.GetActor() != nullptr)
			{
				Params.AddIgnoredActor(Hit.GetActor());
				continue;
			}
			return !Hit.bStartPenetrating && Hit.ImpactNormal.Z < -0.1 && Hit.ImpactPoint.Z - FloorZ >= OverhangMinHeadroom;
		}
		return false;
	}

	// Satisfactory's mountains are mostly static-mesh cliffs, not landscape, so anything static
	// counts as ground except buildable actors, holograms and foliage (which building clears anyway).
	// Foundations and the like count only up to MaxBuildingZ: a floor the spline is being built on
	// is ground, a platform well above both ends is something to pass under.
	// OutZ is the top surface. With OutUnderZ and RouteZ (the spline's own height there), ground
	// well above the route may be the top of an overhang (a rock arch, a cave roof): then OutUnderZ
	// gets the open floor under it nearest the route, if there is one. Whether to pass over or
	// under is decided for the whole overhang at once (ChooseOverOrUnder), not here.
	bool FindGroundZ(const UWorld* World, const AActor* Hologram, const FVector2D& XY, double Top, double Bottom, double MaxBuildingZ, double& OutZ,
		TOptional<double> RouteZ = {}, TOptional<double>* OutUnderZ = nullptr)
	{
		FCollisionQueryParams Params(SCENE_QUERY_STAT(TerrainBeltsGround), false, Hologram);
		FHitResult Hit;
		if (!TraceGround(World, XY, Top, Bottom, MaxBuildingZ, Params, Hit))
		{
			return false;
		}
		OutZ = Hit.ImpactPoint.Z;
		if (OutUnderZ == nullptr || !RouteZ.IsSet() || OutZ <= *RouteZ + OverhangMinRise)
		{
			return true;
		}

		const double TopZ = OutZ;
		TOptional<double> BestZ;
		auto Consider = [&](const FHitResult& Floor)
		{
			const double FloorZ = Floor.ImpactPoint.Z;
			if (!Floor.bStartPenetrating && Floor.ImpactNormal.Z > 0.1 && FloorZ < TopZ - OverhangMinHeadroom &&
				(!BestZ.IsSet() || FMath::Abs(FloorZ - *RouteZ) < FMath::Abs(*BestZ - *RouteZ)) && HasOpenSpaceAbove(World, Hologram, XY, FloorZ, TopZ))
			{
				BestZ = FloorZ;
			}
		};

		// Look through the surface found, and the next few, to whatever lies under them.
		FCollisionQueryParams LayerParams = Params;
		FHitResult Layer = Hit;
		for (int32 Index = 0; Index < OverhangMaxLayers && Layer.GetComponent() != nullptr; ++Index)
		{
			LayerParams.AddIgnoredComponent(Layer.GetComponent());
			if (!TraceGround(World, XY, Top, Bottom, MaxBuildingZ, LayerParams, Layer))
			{
				break;
			}
			Consider(Layer);
		}
		// The route may run through the open space under the overhang. A trace from just above it
		// finds that floor even when the overhang and the floor are one mesh.
		const double ProbeTop = *RouteZ + OverhangProbeHeight;
		FCollisionQueryParams ProbeParams(SCENE_QUERY_STAT(TerrainBeltsGround), false, Hologram);
		FHitResult Probe;
		if (ProbeTop < TopZ && TraceGround(World, XY, ProbeTop, Bottom, MaxBuildingZ, ProbeParams, Probe))
		{
			Consider(Probe);
		}

		*OutUnderZ = BestZ;
		return true;
	}

	// Highest ground under the centre line and HalfWidth to either side of it. Across is the
	// horizontal unit vector at right angles to the spline. SplineZ is the spline's own height
	// there: a building the side traces find above it is one the spline runs alongside (a
	// foundation wall), not under, so they look past it for the ground below. OutUnderZ is the
	// highest ground under any overhang there (see FindGroundZ); a trace with no overhang counts
	// with its own ground, and if no trace found an overhang it stays unset.
	bool FindGroundZAcross(const UWorld* World, const AActor* Hologram, const FVector2D& Center, const FVector2D& Across, double HalfWidth, double Top, double Bottom, double MaxBuildingZ, double SplineZ, double& OutZ, TOptional<double>& OutUnderZ)
	{
		OutUnderZ.Reset();
		bool bFound = false;
		bool bAnyUnder = false;
		double UnderZ = 0.0;
		auto Trace = [&](const FVector2D& XY, double TraceMaxBuildingZ)
		{
			double TraceZ = 0.0;
			TOptional<double> TraceUnderZ;
			if (!FindGroundZ(World, Hologram, XY, Top, Bottom, TraceMaxBuildingZ, TraceZ, SplineZ, &TraceUnderZ))
			{
				return;
			}
			const double TraceFloorZ = TraceUnderZ.Get(TraceZ);
			UnderZ = bFound ? FMath::Max(UnderZ, TraceFloorZ) : TraceFloorZ;
			OutZ = bFound ? FMath::Max(OutZ, TraceZ) : TraceZ;
			bAnyUnder |= TraceUnderZ.IsSet();
			bFound = true;
		};
		Trace(Center, MaxBuildingZ);
		if (HalfWidth > 0.0 && !Across.IsNearlyZero())
		{
			for (const double Side : {-HalfWidth, HalfWidth})
			{
				Trace(Center + Across * Side, FMath::Min(MaxBuildingZ, SplineZ));
			}
		}
		if (bFound && bAnyUnder && UnderZ < OutZ)
		{
			OutUnderZ = UnderZ;
		}
		return bFound;
	}

	// A spot along the route: Target is its MinZ (the top surface, plus clearance), UnderZ the
	// floor under an overhang there (plus clearance), if any.
	struct FGroundSite
	{
		double* Target;
		TOptional<double> UnderZ;
	};

	// Sites run along the route from end to end; the ends have no UnderZ. Each stretch of sites
	// under an overhang passes over it (keeps the tops) or under it (takes the floors) as a whole:
	// under, unless over climbs clearly less from the sites on either side. Choosing spot by spot
	// by distance to the route dropped a belt between two points on top of an arch to the sand
	// under it (the straight line joining them dips below the arch's top), and put a lift in the
	// middle of a belt running from an arch top down to the ground.
	void ChooseOverOrUnder(const TArray<FGroundSite>& Sites)
	{
		int32 Index = 0;
		while (Index < Sites.Num())
		{
			if (!Sites[Index].UnderZ.IsSet())
			{
				++Index;
				continue;
			}
			const int32 First = Index;
			while (Index < Sites.Num() && Sites[Index].UnderZ.IsSet())
			{
				++Index;
			}
			const int32 Last = Index - 1;
			if (First == 0 || Last + 1 >= Sites.Num())
			{
				continue; // can't happen: the ends have no UnderZ
			}

			double OverClimb = 0.0;
			double UnderClimb = 0.0;
			double PreviousOver = *Sites[First - 1].Target;
			double PreviousUnder = PreviousOver;
			for (int32 Site = First; Site <= Last; ++Site)
			{
				const double Over = *Sites[Site].Target;
				const double Under = *Sites[Site].UnderZ;
				OverClimb += FMath::Abs(Over - PreviousOver);
				UnderClimb += FMath::Abs(Under - PreviousUnder);
				PreviousOver = Over;
				PreviousUnder = Under;
			}
			const double After = *Sites[Last + 1].Target;
			OverClimb += FMath::Abs(After - PreviousOver);
			UnderClimb += FMath::Abs(After - PreviousUnder);

			const bool bUnder = UnderClimb <= OverClimb + OverhangClimbTolerance;
			if (FirstTime(bUnder ? TEXT("ground:overhang-under") : TEXT("ground:overhang-over")))
			{
				UE_LOG(LogTerrainBelts, Display, TEXT("Overhang over %d spots: passing %s (climb over %.0f cm, under %.0f cm)"),
					Last - First + 1, bUnder ? TEXT("under") : TEXT("over"), OverClimb, UnderClimb);
			}
			if (bUnder)
			{
				for (int32 Site = First; Site <= Last; ++Site)
				{
					*Sites[Site].Target = *Sites[Site].UnderZ;
				}
			}
		}
	}

	FVector2D AcrossDirection(const FVector& Tangent)
	{
		const FVector2D Forward = FVector2D(Tangent).GetSafeNormal();
		return FVector2D(-Forward.Y, Forward.X);
	}

	// Splits every segment longer than StepLength into equal pieces without changing the curve:
	// each new point sits on the Hermite segment, and every tangent is scaled to its piece.
	TArray<FSplinePointData> Subdivide(const TArray<FSplinePointData>& Points, double StepLength)
	{
		TArray<FSplinePointData> Dense;
		Dense.Add(Points[0]);
		for (int32 Index = 0; Index + 1 < Points.Num(); ++Index)
		{
			const FSplinePointData& A = Points[Index];
			const FSplinePointData& B = Points[Index + 1];
			const int32 Pieces = FMath::Max(1, FMath::CeilToInt(FVector::Dist(A.Location, B.Location) / StepLength));
			const double Dt = 1.0 / Pieces;

			Dense.Last().LeaveTangent = A.LeaveTangent * Dt;
			for (int32 Piece = 1; Piece < Pieces; ++Piece)
			{
				const double T = Piece * Dt;
				const FVector Location = FMath::CubicInterp(A.Location, A.LeaveTangent, B.Location, B.ArriveTangent, T);
				const FVector Tangent = FMath::CubicInterpDerivative(A.Location, A.LeaveTangent, B.Location, B.ArriveTangent, T) * Dt;
				Dense.Emplace(Location, Tangent);
			}
			Dense.Emplace(B.Location, B.ArriveTangent * Dt, B.LeaveTangent);
		}
		return Dense;
	}

	FVector RescaleTangent(const FVector& Tangent, const FVector& Chord)
	{
		const FVector Direction = Tangent.IsNearlyZero() ? Chord.GetSafeNormal() : Tangent.GetSafeNormal();
		return Direction * Chord.Size();
	}

	// Drops interior points that sit (nearly) above or below the previous point, or above the
	// end. Where points were dropped, the tangents on either side are resized to the new chord.
	TArray<FSplinePointData> DropVerticalRuns(const TArray<FSplinePointData>& Points)
	{
		const FVector EndLocation = Points.Last().Location;
		TArray<FSplinePointData> Kept;
		Kept.Add(Points[0]);
		bool bDropped = false;
		for (int32 Index = 1; Index < Points.Num(); ++Index)
		{
			const bool bIsEnd = Index == Points.Num() - 1;
			const FVector& Location = Points[Index].Location;
			if (!bIsEnd && (FVector::Dist2D(Location, Kept.Last().Location) < MinHorizontalSpacing || FVector::Dist2D(Location, EndLocation) < MinHorizontalSpacing))
			{
				bDropped = true;
				continue;
			}

			FSplinePointData Next = Points[Index];
			if (bDropped)
			{
				const FVector Chord = Next.Location - Kept.Last().Location;
				Kept.Last().LeaveTangent = RescaleTangent(Kept.Last().LeaveTangent, Chord);
				Next.ArriveTangent = RescaleTangent(Next.ArriveTangent, Chord);
				bDropped = false;
			}
			Kept.Add(Next);
		}
		return Kept;
	}

	// Catmull-Rom tangents through the points: each interior point heads from its previous
	// neighbour towards its next one, sized to the segment on each side. The ends keep their
	// direction. Heights are set later, so only the horizontal shape matters here.
	void SmoothRoute(TArray<FSplinePointData>& Points, double MinSpacing)
	{
		// Points bunched up around a corner would still make a tight curve; thin them out first.
		TArray<FSplinePointData> Kept;
		Kept.Add(Points[0]);
		for (int32 Index = 1; Index + 1 < Points.Num(); ++Index)
		{
			if (FVector::Dist2D(Points[Index].Location, Kept.Last().Location) >= MinSpacing && FVector::Dist2D(Points[Index].Location, Points.Last().Location) >= MinSpacing)
			{
				Kept.Add(Points[Index]);
			}
		}
		Kept.Add(Points.Last());
		Points = MoveTemp(Kept);

		const int32 Num = Points.Num();
		for (int32 Index = 0; Index < Num; ++Index)
		{
			FSplinePointData& Point = Points[Index];
			const FVector Direction = Index == 0 ? Point.LeaveTangent.GetSafeNormal()
				: Index == Num - 1 ? Point.ArriveTangent.GetSafeNormal()
				: (Points[Index + 1].Location - Points[Index - 1].Location).GetSafeNormal2D();
			if (Index > 0)
			{
				Point.ArriveTangent = Direction * FVector::Dist2D(Points[Index - 1].Location, Point.Location);
			}
			if (Index < Num - 1)
			{
				Point.LeaveTangent = Direction * FVector::Dist2D(Point.Location, Points[Index + 1].Location);
			}
		}
	}

	// Keeps the tangent's horizontal part (the vanilla route) and tilts it to the slope. A
	// (nearly) vertical tangent has no usable horizontal part, so it takes the neighbours' chord.
	void SetTangentSlope(FVector& Tangent, double Slope, const FVector& Fallback)
	{
		const FVector2D Horizontal = FVector2D(Tangent).Size() >= 0.25 * Tangent.Size() ? FVector2D(Tangent) : FVector2D(Fallback);
		Tangent = FVector(Horizontal, Slope * Horizontal.Size());
	}

	TArray<FSplinePointData> ToWorldPoints(const TArray<FSplinePointData>& LocalPoints, const FTransform& ToWorld)
	{
		TArray<FSplinePointData> WorldPoints;
		WorldPoints.Reserve(LocalPoints.Num());
		for (const FSplinePointData& Point : LocalPoints)
		{
			WorldPoints.Emplace(ToWorld.TransformPosition(Point.Location), ToWorld.TransformVector(Point.ArriveTangent), ToWorld.TransformVector(Point.LeaveTangent));
		}
		return WorldPoints;
	}

	TArray<FSplinePointData> ToLocalPoints(const TArray<FSplinePointData>& WorldPoints, const FTransform& ToWorld)
	{
		TArray<FSplinePointData> LocalPoints;
		LocalPoints.Reserve(WorldPoints.Num());
		for (const FSplinePointData& Point : WorldPoints)
		{
			LocalPoints.Emplace(ToWorld.InverseTransformPosition(Point.Location), ToWorld.InverseTransformVector(Point.ArriveTangent), ToWorld.InverseTransformVector(Point.LeaveTangent));
		}
		return LocalPoints;
	}

	// Ground + clearance at a spot between two points: segment Segment (points Segment and
	// Segment + 1) at curve parameter T.
	struct FGroundSample
	{
		int32 Segment;
		double T;
		double MinZ;
	};

	// The vanilla route, densified (world space), with the lowest height each point may take.
	struct FGroundProfile
	{
		TArray<FSplinePointData> Points;
		TArray<double> Step; // horizontal distance to the next point
		TArray<double> Along; // horizontal distance from the start
		TArray<double> MinZ; // ground + clearance; the ends keep their own height
		TArray<FGroundSample> Samples;
		double MaxSlope = 0.0;
		bool bFoundGround = false; // at any interior point or sample
	};

	// The samples of segments [FirstSegment, FirstSegment + NumSegments), renumbered from 0, for
	// draping one piece of a split profile.
	TArray<FGroundSample> SliceSamples(const TArray<FGroundSample>& Samples, int32 FirstSegment, int32 NumSegments)
	{
		TArray<FGroundSample> Slice;
		for (const FGroundSample& Sample : Samples)
		{
			if (Sample.Segment >= FirstSegment && Sample.Segment < FirstSegment + NumSegments)
			{
				Slice.Add({Sample.Segment - FirstSegment, Sample.T, Sample.MinZ});
			}
		}
		return Slice;
	}

	void ComputeSteps(const TArray<FSplinePointData>& Points, TArray<double>& OutStep, TArray<double>& OutAlong)
	{
		const int32 Num = Points.Num();
		OutStep.SetNum(Num - 1);
		OutAlong.SetNum(Num);
		OutAlong[0] = 0.0;
		for (int32 Index = 0; Index + 1 < Num; ++Index)
		{
			OutStep[Index] = FMath::Max(1.0, FVector::Dist2D(Points[Index].Location, Points[Index + 1].Location));
			OutAlong[Index + 1] = OutAlong[Index] + OutStep[Index];
		}
	}

	// The last profiles built per hologram. Vanilla regenerates the same route every frame while
	// the cursor rests, so this skips re-tracing the ground then.
	struct FProfileCacheEntry
	{
		TArray<FSplinePointData> Input;
		double StepLength;
		double ExtraTraceHeadroom;
		double GroundClearance;
		FGroundProfile Profile;
	};
	TMap<FObjectKey, TArray<FProfileCacheEntry>> ProfileCache;
	constexpr int32 MaxProfileCacheHolograms = 32;
	constexpr int32 MaxProfileCacheEntriesPerHologram = 2; // auto-lift and plain drape differ in headroom

	bool BuildProfileUncached(const TArray<FSplinePointData>& WorldPoints, const AActor* Hologram, const FDrapeSettings& Settings, FGroundProfile& Out);

	bool BuildProfile(const TArray<FSplinePointData>& WorldPoints, const AActor* Hologram, const FDrapeSettings& Settings, FGroundProfile& Out)
	{
		TArray<FProfileCacheEntry>& Entries = ProfileCache.FindOrAdd(FObjectKey(Hologram));
		for (const FProfileCacheEntry& Entry : Entries)
		{
			if (Entry.StepLength == Settings.StepLength && Entry.ExtraTraceHeadroom == Settings.ExtraTraceHeadroom &&
				Entry.GroundClearance == Settings.GroundClearance && SameSplineData(Entry.Input, WorldPoints))
			{
				Out = Entry.Profile;
				return true;
			}
		}

		if (!BuildProfileUncached(WorldPoints, Hologram, Settings, Out))
		{
			return false;
		}
		if (ProfileCache.Num() > MaxProfileCacheHolograms)
		{
			// Holograms that were put away leave entries behind; start over.
			ProfileCache.Reset();
		}
		TArray<FProfileCacheEntry>& NewEntries = ProfileCache.FindOrAdd(FObjectKey(Hologram));
		if (NewEntries.Num() >= MaxProfileCacheEntriesPerHologram)
		{
			NewEntries.RemoveAt(0);
		}
		NewEntries.Add({WorldPoints, Settings.StepLength, Settings.ExtraTraceHeadroom, Settings.GroundClearance, Out});
		return true;
	}

	bool BuildProfileUncached(const TArray<FSplinePointData>& WorldPoints, const AActor* Hologram, const FDrapeSettings& Settings, FGroundProfile& Out)
	{
		const UWorld* World = Hologram->GetWorld();
		if (WorldPoints.Num() < 2 || World == nullptr)
		{
			return false;
		}

		const TArray<FSplinePointData> Dense = Subdivide(WorldPoints, Settings.StepLength);
		Out.Points = DropVerticalRuns(Dense);
		// Only re-shape the route where vertical runs were dropped and left corners. Otherwise
		// the subdivided points carry vanilla's exact horizontal curve, and re-shaping it through
		// thinned points turned a tight vanilla bend at a support into a sharper one.
		if (Settings.bSmoothRoute && Out.Points.Num() < Dense.Num())
		{
			// Points this close make a curve about this tight, so keep them at least twice the
			// tightest bend apart. A short route thinned below three points can't be draped, so
			// it keeps a single midpoint instead.
			const TArray<FSplinePointData> Unsmoothed = Out.Points;
			SmoothRoute(Out.Points, FMath::Max(MinRouteSpacing, 2.0 * Settings.MinBendRadius));
			if (Out.Points.Num() < 3 && Unsmoothed.Num() >= 3)
			{
				Out.Points = {Unsmoothed[0], Unsmoothed[Unsmoothed.Num() / 2], Unsmoothed.Last()};
				SmoothRoute(Out.Points, 0.0);
			}
		}
		const int32 Num = Out.Points.Num();
		if (Num < 3)
		{
			return false;
		}
		ComputeSteps(Out.Points, Out.Step, Out.Along);
		Out.MaxSlope = FMath::Tan(FMath::DegreesToRadians(FMath::Clamp(Settings.MaxInclineDegrees, 0.0, 89.0)));

		const double StartZ = Out.Points[0].Location.Z;
		const double EndZ = Out.Points.Last().Location.Z;
		const double TotalLength = Out.Along[Num - 1];
		const double MaxBuildingZ = FMath::Max(StartZ, EndZ) + BuildingFloorTolerance;

		// The spline can't climb above the higher end by more than the max incline allows, so
		// starting the trace there avoids most ceilings while still clearing any reachable hill.
		auto TraceTop = [&](double Along)
		{
			const double DistanceToEnd = FMath::Min(Along, TotalLength - Along);
			return FMath::Max(StartZ, EndZ) + FMath::Max(Out.MaxSlope * DistanceToEnd, Settings.ExtraTraceHeadroom) + TraceElevation;
		};

		Out.MinZ.SetNum(Num);
		Out.MinZ[0] = StartZ;
		Out.MinZ[Num - 1] = EndZ;
		TArray<TOptional<double>> PointUnderZ;
		PointUnderZ.SetNum(Num);
		for (int32 Index = 1; Index + 1 < Num; ++Index)
		{
			const FSplinePointData& Point = Out.Points[Index];
			const double Bottom = FMath::Min3(StartZ, EndZ, Point.Location.Z) - TraceSearchDistance;
			double GroundZ;
			TOptional<double> UnderZ;
			const bool bFound = FindGroundZAcross(World, Hologram, FVector2D(Point.Location), AcrossDirection(Point.LeaveTangent), Settings.HalfWidth, TraceTop(Out.Along[Index]), Bottom, MaxBuildingZ, Point.Location.Z, GroundZ, UnderZ);
			Out.MinZ[Index] = bFound ? GroundZ + Settings.GroundClearance : Point.Location.Z;
			if (bFound && UnderZ.IsSet())
			{
				PointUnderZ[Index] = *UnderZ + Settings.GroundClearance;
			}
			Out.bFoundGround |= bFound;
		}

		Out.Samples.Reset();
		TArray<TOptional<double>> SampleUnderZ;
		if (Settings.GroundSampleSpacing > 0.0)
		{
			for (int32 Segment = 0; Segment + 1 < Num; ++Segment)
			{
				const FSplinePointData& A = Out.Points[Segment];
				const FSplinePointData& B = Out.Points[Segment + 1];
				const int32 Pieces = FMath::CeilToInt(Out.Step[Segment] / Settings.GroundSampleSpacing);
				for (int32 Piece = 1; Piece < Pieces; ++Piece)
				{
					const double T = static_cast<double>(Piece) / Pieces;
					const FVector Location = FMath::CubicInterp(A.Location, A.LeaveTangent, B.Location, B.ArriveTangent, T);
					const FVector Tangent = FMath::CubicInterpDerivative(A.Location, A.LeaveTangent, B.Location, B.ArriveTangent, T);
					const double Bottom = FMath::Min3(StartZ, EndZ, Location.Z) - TraceSearchDistance;
					double GroundZ;
					TOptional<double> UnderZ;
					if (FindGroundZAcross(World, Hologram, FVector2D(Location), AcrossDirection(Tangent), Settings.HalfWidth, TraceTop(Out.Along[Segment] + T * Out.Step[Segment]), Bottom, MaxBuildingZ, Location.Z, GroundZ, UnderZ))
					{
						Out.Samples.Add({Segment, T, GroundZ + Settings.GroundClearance});
						SampleUnderZ.Add(UnderZ.IsSet() ? TOptional<double>(*UnderZ + Settings.GroundClearance) : TOptional<double>());
						Out.bFoundGround = true;
					}
				}
			}
		}

		// Every point and sample in order along the route (samples come segment by segment), then
		// pass over or under each overhang as a whole.
		TArray<FGroundSite> Sites;
		int32 SampleIndex = 0;
		for (int32 Index = 0; Index < Num; ++Index)
		{
			Sites.Add({&Out.MinZ[Index], PointUnderZ[Index]});
			for (; SampleIndex < Out.Samples.Num() && Out.Samples[SampleIndex].Segment == Index; ++SampleIndex)
			{
				Sites.Add({&Out.Samples[SampleIndex].MinZ, SampleUnderZ[SampleIndex]});
			}
		}
		ChooseOverOrUnder(Sites);
		return true;
	}

	// Heights from MinZ: smoothing, then alternately the slope limit (only raising, so the spline
	// never dips below the ground and an elevated end ramps down at the max incline) and the bend
	// limit, until both hold. The ends keep their height.
	TArray<double> SolveHeights(const TArray<double>& MinZ, const TArray<double>& Step, double MaxSlope, const FDrapeSettings& Settings)
	{
		const int32 Num = MinZ.Num();
		TArray<double> Z = MinZ;

		for (int32 Pass = 0; Pass < Settings.SmoothingPasses; ++Pass)
		{
			TArray<double> Smoothed = Z;
			for (int32 Index = 1; Index + 1 < Num; ++Index)
			{
				Smoothed[Index] = FMath::Max(MinZ[Index], (Z[Index - 1] + 2.0 * Z[Index] + Z[Index + 1]) * 0.25);
			}
			Z = MoveTemp(Smoothed);
		}

		auto LimitSlope = [&]()
		{
			for (int32 Index = 1; Index + 1 < Num; ++Index)
			{
				Z[Index] = FMath::Max(Z[Index], Z[Index - 1] - MaxSlope * Step[Index - 1]);
			}
			for (int32 Index = Num - 2; Index >= 1; --Index)
			{
				Z[Index] = FMath::Max(Z[Index], Z[Index + 1] - MaxSlope * Step[Index]);
			}
		};

		const double Radius = Settings.VerticalBendRadius;
		if (Radius > 0.0)
		{
			// Round off crests: find the lowest shape above the points with no crest sharper than
			// Radius. A shape bends down no faster than 1 / Radius exactly when adding
			// Along^2 / (2 Radius) makes it convex. So take the lower convex hull of the points with
			// that added, then subtract it again. Between hull points the result is a parabola of
			// exactly that curvature, and a straight slope stays where it is.
			// Two earlier versions failed:
			// - Raising a crest's neighbours fed back on itself and froze the game with a
			//   kilometres-long belt.
			// - Lifting every point onto parabolas around the others floated the belt
			//   slope^2 * Radius / 2 above any slope (6 m on a 34 degree dune).
			TArray<double> Along;
			Along.SetNum(Num);
			Along[0] = 0.0;
			for (int32 Index = 0; Index + 1 < Num; ++Index)
			{
				Along[Index + 1] = Along[Index] + Step[Index];
			}
			const auto Bowed = [&](int32 Index)
			{
				return Z[Index] + FMath::Square(Along[Index]) / (2.0 * Radius);
			};
			TArray<int32> Hull;
			for (int32 Index = 0; Index < Num; ++Index)
			{
				// Drop the last hull point while it lies on or above the line from the one before it
				// to this point.
				while (Hull.Num() >= 2)
				{
					const int32 A = Hull[Hull.Num() - 2];
					const int32 B = Hull.Last();
					if ((Bowed(B) - Bowed(A)) * (Along[Index] - Along[A]) < (Bowed(Index) - Bowed(A)) * (Along[B] - Along[A]))
					{
						break;
					}
					Hull.Pop();
				}
				Hull.Add(Index);
			}
			TArray<double> Rounded = Z;
			for (int32 HullIndex = 0; HullIndex + 1 < Hull.Num(); ++HullIndex)
			{
				const int32 A = Hull[HullIndex];
				const int32 B = Hull[HullIndex + 1];
				for (int32 Index = A + 1; Index < B; ++Index)
				{
					const double T = (Along[Index] - Along[A]) / (Along[B] - Along[A]);
					Rounded[Index] = FMath::Max(Z[Index], FMath::Lerp(Bowed(A), Bowed(B), T) - FMath::Square(Along[Index]) / (2.0 * Radius));
				}
			}
			Z = MoveTemp(Rounded);

			// The rounding knows nothing about the slope limit, so near a lower end it could leave
			// the spline too high to get back down (a near-vertical drop at the end). Keep every
			// point within reach of both ends at the max incline, but never below the ground.
			for (int32 Index = 1; Index + 1 < Num; ++Index)
			{
				const double Reachable = FMath::Min(Z[0] + MaxSlope * Along[Index], Z[Num - 1] + MaxSlope * (Along[Num - 1] - Along[Index]));
				Z[Index] = FMath::Max(MinZ[Index], FMath::Min(Z[Index], Reachable));
			}
		}
		LimitSlope();

		constexpr int32 MaxDipIterations = 40;
		constexpr double DipTolerance = 0.5;
		for (int32 Iteration = 0; Iteration < MaxDipIterations && Radius > 0.0; ++Iteration)
		{
			// Fill sharp dips: a point may sit at most Limit below the line through its neighbours,
			// which bounds the curvature to about 1 / Radius. Raising a dip point only makes its
			// neighbours sit lower relative to their own lines, so this can't sharpen a crest, and
			// it never rises above the neighbours.
			bool bChanged = false;
			for (int32 Index = 1; Index + 1 < Num; ++Index)
			{
				const double Before = Step[Index - 1];
				const double After = Step[Index];
				const double Line = (Z[Index - 1] * After + Z[Index + 1] * Before) / (Before + After);
				const double Limit = Before * After / (2.0 * Radius);
				if (Z[Index] < Line - Limit - DipTolerance)
				{
					Z[Index] = Line - Limit;
					bChanged = true;
				}
			}
			LimitSlope();
			if (!bChanged)
			{
				break;
			}
		}
		return Z;
	}

	// A pipe entering a flat floor must flatten before its low point. A centred difference can
	// still point down there, pulling the Hermite curve below both points and into the floor.
	double PipeDrapeSlope(double BeforeSlope, double AfterSlope, double CenteredSlope)
	{
		if (BeforeSlope * AfterSlope <= 0.0)
		{
			return 0.0;
		}
		const double Limit = 2.0 * FMath::Min(FMath::Abs(BeforeSlope), FMath::Abs(AfterSlope));
		return FMath::Clamp(CenteredSlope, -Limit, Limit);
	}

	// Sets the heights and tangents of world-space points from MinZ (see SolveHeights). Then checks
	// the actual curve against the ground samples between points and, where it dips into the
	// ground, raises the points around it and solves again. The ends keep their location and tangents.
	// Returns false (leaving the points unusable) if the result is not sane, so the caller keeps vanilla.
	bool FinishDrape(TArray<FSplinePointData>& Points, TArray<double> MinZ, const FDrapeSettings& Settings, const TArray<FGroundSample>& Samples = TArray<FGroundSample>())
	{
		const int32 Num = Points.Num();
		if (Num < 2)
		{
			return false;
		}
		const double MaxSlope = FMath::Tan(FMath::DegreesToRadians(FMath::Clamp(Settings.MaxInclineDegrees, 0.0, 89.0)));
		// Fixed ends can make the ground constraints impossible to satisfy. The height solver
		// only raises interior points, so it cannot enforce the incline at a low fixed end.
		// Reject those constraints before rounding or ground-sample correction can grow an arch.
		TArray<double> Step;
		TArray<double> Along;
		ComputeSteps(Points, Step, Along);
		constexpr double HeightTolerance = 2.0; // same tolerance as ground-sample correction
		auto ReachableHeight = [&](double Distance)
		{
			return FMath::Min(Points[0].Location.Z + MaxSlope * Distance,
				Points.Last().Location.Z + MaxSlope * (Along.Last() - Distance)) + HeightTolerance;
		};
		if (Settings.bEnforceIncline)
		{
			if (FMath::Abs(Points.Last().Location.Z - Points[0].Location.Z) > MaxSlope * Along.Last() + HeightTolerance)
			{
				if (FirstTime(TEXT("belt:unreachable-ends")))
				{
					UE_LOG(LogTerrainBelts, Display, TEXT("Belt drape rejected: endpoints %s -> %s need %.0f cm of rise over %.0f cm; maximum %.0f cm"),
						*Points[0].Location.ToString(), *Points.Last().Location.ToString(),
						FMath::Abs(Points.Last().Location.Z - Points[0].Location.Z), Along.Last(), MaxSlope * Along.Last());
				}
				return false;
			}
			// Clearance is a preference, not a limit: ground the belt can reach only by giving up some
			// of it still counts, and the belt runs that much closer to it (MaxZ below). Rejecting
			// those dropped the belt from an auto-lift top across an arch's crown to its end.
			for (int32 Index = 1; Index + 1 < Num; ++Index)
			{
				if (MinZ[Index] - Settings.GroundClearance > ReachableHeight(Along[Index]))
				{
					if (FirstTime(TEXT("belt:unreachable-ground")))
					{
						UE_LOG(LogTerrainBelts, Display, TEXT("Belt drape rejected: ground at point %d needs Z %.0f, reachable Z %.0f; endpoints %s -> %s"),
							Index, MinZ[Index], ReachableHeight(Along[Index]), *Points[0].Location.ToString(), *Points.Last().Location.ToString());
					}
					return false;
				}
			}
			for (const FGroundSample& Sample : Samples)
			{
				if (Sample.MinZ - Settings.GroundClearance > ReachableHeight(Along[Sample.Segment] + Sample.T * Step[Sample.Segment]))
				{
					if (FirstTime(TEXT("belt:unreachable-sample")))
					{
						UE_LOG(LogTerrainBelts, Display, TEXT("Belt drape rejected: ground sample on segment %d at %.2f needs Z %.0f, reachable Z %.0f; endpoints %s -> %s"),
							Sample.Segment, Sample.T, Sample.MinZ, ReachableHeight(Along[Sample.Segment] + Sample.T * Step[Sample.Segment]),
							*Points[0].Location.ToString(), *Points.Last().Location.ToString());
					}
					return false;
				}
			}
		}
		if (Num < 3)
		{
			return true;
		}

		MinZ[0] = Points[0].Location.Z;
		MinZ[Num - 1] = Points.Last().Location.Z;
		const double HighestAllowedZ = FMath::Max(MinZ) + MaxDrapeRise;

		// Each end keeps vanilla's tangent (a pipe leaving a support level, say), and the curve
		// can't turn away from it faster than the exit radius, down or up. So near an end every
		// point stays between two parabolas around the end's direction. Without the lower one the
		// drape dropped straight to the ground beside a support; without the upper one it climbed
		// a dune right beside the support. Either way the pipe kinked at the end. Near an end,
		// the pipe may clip rising ground instead, as vanilla's does.
		TArray<double> MaxZ;
		MaxZ.Init(UE_BIG_NUMBER, Num);
		if (Settings.MinBendRadius > 0.0)
		{
			const double ExitRadius = EndExitBendScale * Settings.MinBendRadius;
			auto KeepExit = [&](const FVector& Tangent, double EndZ, auto DistanceFromEnd)
			{
				const double Horizontal = FVector2D(Tangent).Size();
				if (Horizontal < 0.25 * Tangent.Size())
				{
					return; // a vertical end; ShapeWithVerticalEnds deals with those
				}
				const double Slope = Tangent.Z / Horizontal;
				for (int32 Index = 1; Index + 1 < Num; ++Index)
				{
					const double Distance = DistanceFromEnd(Index);
					const double Line = EndZ + Slope * Distance;
					const double Bend = FMath::Square(Distance) / (2.0 * ExitRadius);
					MaxZ[Index] = FMath::Min(MaxZ[Index], Line + Bend);
					MinZ[Index] = FMath::Max(MinZ[Index], Line - Bend);
				}
			};
			KeepExit(Points[0].LeaveTangent, Points[0].Location.Z, [&](int32 Index) { return Along[Index]; });
			KeepExit(-Points.Last().ArriveTangent, Points.Last().Location.Z, [&](int32 Index) { return Along[Num - 1] - Along[Index]; });
			for (int32 Index = 1; Index + 1 < Num; ++Index)
			{
				MinZ[Index] = FMath::Min(MinZ[Index], MaxZ[Index]);
			}
		}

		// A belt can't go higher than it can climb back down from to either end. Without this cap,
		// sample correction over a short crest raised a point past it, and the result was rejected
		// as too steep.
		TArray<FGroundSample> ReachableSamples = Samples;
		if (Settings.bEnforceIncline)
		{
			for (int32 Index = 1; Index + 1 < Num; ++Index)
			{
				MaxZ[Index] = FMath::Min(MaxZ[Index], ReachableHeight(Along[Index]) - HeightTolerance);
				MinZ[Index] = FMath::Min(MinZ[Index], MaxZ[Index]);
			}
			for (FGroundSample& Sample : ReachableSamples)
			{
				Sample.MinZ = FMath::Min(Sample.MinZ, ReachableHeight(Along[Sample.Segment] + Sample.T * Step[Sample.Segment]) - HeightTolerance);
			}
		}

		constexpr int32 MaxSampleRounds = 4;
		constexpr double SampleTolerance = 2.0;
		for (int32 Round = 0; Round < MaxSampleRounds; ++Round)
		{
			TArray<double> Z = SolveHeights(MinZ, Step, MaxSlope, Settings);
			for (int32 Index = 1; Index + 1 < Num; ++Index)
			{
				Z[Index] = FMath::Min(Z[Index], MaxZ[Index]);
			}
			for (int32 Index = 1; Index + 1 < Num; ++Index)
			{
				FSplinePointData& Point = Points[Index];
				Point.Location.Z = Z[Index];
				double Slope = (Z[Index + 1] - Z[Index - 1]) / (Step[Index - 1] + Step[Index]);
				if (Settings.MinBendRadius > 0.0)
				{
					Slope = PipeDrapeSlope((Z[Index] - Z[Index - 1]) / Step[Index - 1],
						(Z[Index + 1] - Z[Index]) / Step[Index], Slope);
				}
				const FVector Fallback = (Points[Index + 1].Location - Points[Index - 1].Location) * 0.5;
				SetTangentSlope(Point.ArriveTangent, Slope, Fallback);
				SetTangentSlope(Point.LeaveTangent, Slope, Fallback);
			}

			bool bRaised = false;
			for (const FGroundSample& Sample : ReachableSamples)
			{
				const FSplinePointData& A = Points[Sample.Segment];
				const FSplinePointData& B = Points[Sample.Segment + 1];
				const double CurveZ = FMath::CubicInterp(A.Location.Z, A.LeaveTangent.Z, B.Location.Z, B.ArriveTangent.Z, Sample.T);
				const double Deficit = Sample.MinZ - CurveZ;
				if (Deficit <= SampleTolerance)
				{
					continue;
				}

				// Raising both ends by the deficit lifts the curve by it everywhere between them. With
				// one end fixed, the free end is raised by the deficit only. Raising it by more (to
				// make up for its small pull near the fixed end) ran away: next to an end that arrives
				// level, a higher neighbour makes the curve dip lower before the end, which asked for
				// a bigger raise again. That put a pipe 30 m up beside its end on the ground.
				const bool bMoveA = Sample.Segment > 0;
				const bool bMoveB = Sample.Segment + 1 < Num - 1;
				const double RaiseA = Deficit;
				const double RaiseB = Deficit;
				if (bMoveA)
				{
					MinZ[Sample.Segment] = FMath::Min(FMath::Max(MinZ[Sample.Segment], Z[Sample.Segment] + RaiseA), MaxZ[Sample.Segment]);
					bRaised = true;
				}
				if (bMoveB)
				{
					MinZ[Sample.Segment + 1] = FMath::Min(FMath::Max(MinZ[Sample.Segment + 1], Z[Sample.Segment + 1] + RaiseB), MaxZ[Sample.Segment + 1]);
					bRaised = true;
				}
			}
			if (!bRaised)
			{
				break;
			}
		}

		// A runaway shape would make the game build an enormous spline and hang.
		for (const FSplinePointData& Point : Points)
		{
			if (!Point.Location.ContainsNaN() && !Point.ArriveTangent.ContainsNaN() && !Point.LeaveTangent.ContainsNaN() && Point.Location.Z <= HighestAllowedZ)
			{
				continue;
			}
			if (FirstTime(TEXT("insane drape")))
			{
				UE_LOG(LogTerrainBelts, Warning, TEXT("Drape produced a bad point %s (highest allowed Z %.0f); keeping the vanilla shape"), *Point.Location.ToString(), HighestAllowedZ);
			}
			return false;
		}
		// Smoothing and sample correction can also leave an interior segment too steep.
		for (int32 Index = 0; Settings.bEnforceIncline && Index + 1 < Num; ++Index)
		{
			if (FMath::Abs(Points[Index + 1].Location.Z - Points[Index].Location.Z) > MaxSlope * Step[Index] + HeightTolerance)
			{
				if (FirstTime(TEXT("belt:solved-too-steep")))
				{
					UE_LOG(LogTerrainBelts, Display, TEXT("Belt drape rejected: solved segment %d rises %.0f cm over %.0f cm; endpoints %s -> %s"),
						Index, FMath::Abs(Points[Index + 1].Location.Z - Points[Index].Location.Z), Step[Index],
						*Points[0].Location.ToString(), *Points.Last().Location.ToString());
				}
				return false;
			}
		}
		return true;
	}

	bool ShouldKeepLevelOnFlatGround(const FGroundProfile& Profile)
	{
		constexpr double HeightTolerance = 2.0; // cm; allow foundation collision seams
		constexpr double LevelSlopeTolerance = 0.001;
		const double LevelZ = Profile.Points[0].Location.Z;
		auto IsLevel = [](const FVector& Tangent)
		{
			return FMath::Abs(Tangent.Z) <= LevelSlopeTolerance * FVector2D(Tangent).Size();
		};
		if (FMath::Abs(Profile.Points.Last().Location.Z - LevelZ) > HeightTolerance ||
			!IsLevel(Profile.Points[0].LeaveTangent) || !IsLevel(Profile.Points.Last().ArriveTangent) ||
			Profile.Samples.IsEmpty())
		{
			return false;
		}

		// MinZ includes clearance, so don't flatten a route whose ground needs it to rise.
		const double GroundLevel = Profile.MinZ[1];
		auto IsFlat = [&](double Z)
		{
			return Z <= FMath::Min(LevelZ, Profile.Points.Last().Location.Z) && FMath::Abs(Z - GroundLevel) <= HeightTolerance;
		};
		for (int32 Index = 1; Index + 1 < Profile.MinZ.Num(); ++Index)
		{
			if (!IsFlat(Profile.MinZ[Index]))
			{
				return false;
			}
		}
		for (const FGroundSample& Sample : Profile.Samples)
		{
			if (!IsFlat(Sample.MinZ))
			{
				return false;
			}
		}
		return true;
	}

	// Whether the vanilla route stays above the ground everywhere, by at most MaxFloat.
	bool RidesCloseAboveGround(const FGroundProfile& Profile, double MaxFloat)
	{
		constexpr double HeightTolerance = 2.0; // cm; allow collision seams
		auto IsClose = [&](double VanillaZ, double MinZ)
		{
			const double Float = VanillaZ - MinZ;
			return Float >= -HeightTolerance && Float <= MaxFloat;
		};
		for (int32 Index = 1; Index + 1 < Profile.MinZ.Num(); ++Index)
		{
			if (!IsClose(Profile.Points[Index].Location.Z, Profile.MinZ[Index]))
			{
				return false;
			}
		}
		for (const FGroundSample& Sample : Profile.Samples)
		{
			const FSplinePointData& A = Profile.Points[Sample.Segment];
			const FSplinePointData& B = Profile.Points[Sample.Segment + 1];
			if (!IsClose(FMath::CubicInterp(A.Location.Z, A.LeaveTangent.Z, B.Location.Z, B.ArriveTangent.Z, Sample.T), Sample.MinZ))
			{
				return false;
			}
		}
		return true;
	}

	// A lift beside a flat approach inherits its support height instead of pulling the belt
	// down to ground clearance. On uneven ground the foot still follows the terrain.
	double LiftFootHeight(const FGroundProfile& Profile, int32 Low, bool bUphill)
	{
		const double GroundZ = Profile.MinZ[Low];
		const int32 End = bUphill ? 0 : Profile.Points.Num() - 1;
		const double SupportZ = Profile.Points[End].Location.Z;
		if (SupportZ <= GroundZ)
		{
			return GroundZ;
		}
		const int32 First = FMath::Min(Low, End);
		const int32 Last = FMath::Max(Low, End);
		for (int32 Index = First + 1; Index < Last; ++Index)
		{
			if (FMath::Abs(Profile.MinZ[Index] - GroundZ) > 2.0)
			{
				return GroundZ;
			}
		}
		for (const FGroundSample& Sample : Profile.Samples)
		{
			if (Sample.Segment >= First && Sample.Segment < Last && FMath::Abs(Sample.MinZ - GroundZ) > 2.0)
			{
				return GroundZ;
			}
		}
		return SupportZ;
	}

	double AutoLiftClearanceOffset(const FGroundProfile& Profile)
	{
		if (Profile.Points.Num() < 3)
		{
			return 0.0;
		}
		const int32 LastInterior = Profile.Points.Num() - 2;
		// Measure on the flat approaches beside the original supports. Checking a later lift's
		// whole approach crosses earlier cliffs and incorrectly loses this support clearance.
		const double StartOffset = LiftFootHeight(Profile, 1, true) - Profile.MinZ[1];
		const double EndOffset = LiftFootHeight(Profile, LastInterior, false) - Profile.MinZ[LastInterior];
		return FMath::Max(StartOffset, EndOffset);
	}

	// Drapes local-space spline data over the ground below it. The ends never move, so snapping,
	// poles and supports stay valid. Returns false if the spline was left alone.
	bool DrapeSpline(TArray<FSplinePointData>& SplineData, const AActor* Hologram, const FDrapeSettings& Settings)
	{
		// Work in world space so traces and slopes don't depend on the hologram's rotation.
		const FTransform ToWorld = Hologram->GetActorTransform();
		FGroundProfile Profile;
		if (!BuildProfile(ToWorldPoints(SplineData, ToWorld), Hologram, Settings, Profile))
		{
			return false;
		}
		// No ground under any of it: nothing to follow. Draping would only smooth and round the
		// vanilla curve upwards (heights never drop below vanilla's where there's no ground),
		// turning a clean S-curve over a gap into a wavier, higher one.
		if (!Profile.bFoundGround)
		{
			if (FirstTime(TEXT("no ground")))
			{
				UE_LOG(LogTerrainBelts, Display, TEXT("No ground under the spline; keeping vanilla's shape"));
			}
			return true;
		}
		if (Settings.MaxKeptFloat > 0.0 && RidesCloseAboveGround(Profile, Settings.MaxKeptFloat))
		{
			if (FirstTime(TEXT("close above ground")))
			{
				UE_LOG(LogTerrainBelts, Display, TEXT("Spline rides close above the ground; keeping vanilla's shape"));
			}
			return true;
		}
		TArray<double> MinZ = Profile.MinZ;
		TArray<FGroundSample> Samples = Profile.Samples;
		if (Settings.bKeepLevelOnFlatGround && ShouldKeepLevelOnFlatGround(Profile))
		{
			for (int32 Index = 1; Index + 1 < MinZ.Num(); ++Index)
			{
				MinZ[Index] = FMath::Lerp(Profile.Points[0].Location.Z, Profile.Points.Last().Location.Z,
					Profile.Along[Index] / Profile.Along.Last());
			}
		}
		if (Settings.bOnlyRaise)
		{
			for (int32 Index = 0; Index < MinZ.Num(); ++Index)
			{
				MinZ[Index] = FMath::Max(MinZ[Index], Profile.Points[Index].Location.Z);
			}
			for (FGroundSample& Sample : Samples)
			{
				const FSplinePointData& A = Profile.Points[Sample.Segment];
				const FSplinePointData& B = Profile.Points[Sample.Segment + 1];
				Sample.MinZ = FMath::Max(Sample.MinZ, FMath::CubicInterp(A.Location.Z, A.LeaveTangent.Z, B.Location.Z, B.ArriveTangent.Z, Sample.T));
			}
		}
		if (!FinishDrape(Profile.Points, MinZ, Settings, Samples))
		{
			return false;
		}
		SplineData = ToLocalPoints(Profile.Points, ToWorld);
		return true;
	}

	// Sweep the pipe's volume along the final Hermite curve, including steps and the vanilla
	// fallback. A downward ground trace cannot distinguish a solid floor from open space under it.
	// bIgnoreBuildings lets foundations and other buildings through: vanilla's own route may clip
	// those (with only a clearance warning), so only terrain is held against it.
	bool HasPipelineGroundClearance(const TArray<FSplinePointData>& SplineData, const AActor* Hologram, const FDrapeSettings& Settings,
		bool bIgnoreBuildings = false)
	{
		const TArray<FSplinePointData> Points = ToWorldPoints(SplineData, Hologram->GetActorTransform());
		const UWorld* World = Hologram->GetWorld();
		if (Points.Num() < 2 || World == nullptr)
		{
			return false;
		}
		struct FCachedCheck
		{
			TArray<FSplinePointData> Points;
			double Time;
			bool bIgnoreBuildings;
			bool bClear;
		};
		static TMap<FObjectKey, TArray<FCachedCheck>> Checks;
		const FObjectKey Key(Hologram);
		const double Now = World->GetRealTimeSeconds();
		if (const TArray<FCachedCheck>* Cached = Checks.Find(Key))
		{
			for (const FCachedCheck& Check : *Cached)
			{
				// Refresh even an unchanged route so newly placed/dismantled foundations count.
				if (Now - Check.Time < 0.25 && Check.bIgnoreBuildings == bIgnoreBuildings && SameSplineData(Check.Points, Points))
				{
					return Check.bClear;
				}
			}
		}
		const FCollisionObjectQueryParams ObjectParams(ECC_WorldStatic);
		const FCollisionShape Shape = FCollisionShape::MakeSphere(Settings.HalfWidth);
		bool bClear = true;
		for (int32 Segment = 0; bClear && Segment + 1 < Points.Num(); ++Segment)
		{
			const FSplinePointData& A = Points[Segment];
			const FSplinePointData& B = Points[Segment + 1];
			const double Span = FMath::Max3(FVector::Dist(A.Location, B.Location), A.LeaveTangent.Size(), B.ArriveTangent.Size());
			const int32 Pieces = FMath::Max(2, FMath::CeilToInt(Span / 50.0));
			FVector Previous = A.Location;
			for (int32 Piece = 1; bClear && Piece <= Pieces; ++Piece)
			{
				const double T = static_cast<double>(Piece) / Pieces;
				const FVector Location = FMath::CubicInterp(A.Location, A.LeaveTangent, B.Location, B.ArriveTangent, T);
				FCollisionQueryParams Params(SCENE_QUERY_STAT(TerrainBeltsPipeClearance), false, Hologram);
				for (int32 Attempt = 0; Attempt <= MaxSkippedHits; ++Attempt)
				{
					FHitResult Hit;
					if (!World->SweepSingleByObjectType(Hit, Previous, Location, FQuat::Identity, ObjectParams, Shape, Params))
					{
						break;
					}
					const bool bSkip = ShouldSkipGroundHit(Hit) || (bIgnoreBuildings && IsInstancedBuildingHit(Hit));
					if (!bSkip || Attempt == MaxSkippedHits)
					{
						bClear = false;
						if (FirstTime(TEXT("pipe:ground-obstruction")))
						{
							UE_LOG(LogTerrainBelts, Display, TEXT("Rejecting pipe curve intersecting %s at %s"),
								*GetNameSafe(Hit.GetActor()), *Hit.ImpactPoint.ToString());
						}
						break;
					}
					Params.AddIgnoredActor(Hit.GetActor());
				}
				Previous = Location;
			}
		}
		if (Checks.Num() >= MaxProfileCacheHolograms)
		{
			Checks.Reset();
		}
		TArray<FCachedCheck>& Entries = Checks.FindOrAdd(Key);
		if (Entries.Num() >= MaxProfileCacheEntriesPerHologram)
		{
			Entries.RemoveAt(0);
		}
		Entries.Add({Points, Now, bIgnoreBuildings, bClear});
		return bClear;
	}

	// A stretch of ground steeper than the belt may climb.
	struct FCliff
	{
		int32 RunStart = INDEX_NONE; // first and last profile point of the steep stretch
		int32 RunEnd = INDEX_NONE;
		int32 Low = INDEX_NONE; // the lower of the two, where the lift stands
		bool bUphill = false; // ground rises along the belt
		double Rise = 0.0;
	};

	// Every stretch of ground steeper than MaxSlope that rises or falls at least MinRise. Unless
	// bIncludeEnds, stretches reaching an end are skipped: usually that's the spline leaving a
	// raised support, not ground.
	TArray<FCliff> FindSteepStretches(const FGroundProfile& Profile, double MaxSlope, double MinRise, bool bIncludeEnds = false)
	{
		TArray<FCliff> Stretches;
		const int32 Num = Profile.Points.Num();
		int32 Index = 0;
		while (Index + 1 < Num)
		{
			const double Grade = (Profile.MinZ[Index + 1] - Profile.MinZ[Index]) / Profile.Step[Index];
			if (FMath::Abs(Grade) <= MaxSlope)
			{
				++Index;
				continue;
			}
			const bool bUphill = Grade > 0.0;
			int32 RunEnd = Index + 1;
			while (RunEnd + 1 < Num)
			{
				const double NextGrade = (Profile.MinZ[RunEnd + 1] - Profile.MinZ[RunEnd]) / Profile.Step[RunEnd];
				if (FMath::Abs(NextGrade) <= MaxSlope || (NextGrade > 0.0) != bUphill)
				{
					break;
				}
				++RunEnd;
			}
			FCliff Stretch;
			Stretch.RunStart = Index;
			Stretch.RunEnd = RunEnd;
			Stretch.Low = bUphill ? Index : RunEnd;
			Stretch.bUphill = bUphill;
			Stretch.Rise = FMath::Abs(Profile.MinZ[RunEnd] - Profile.MinZ[Index]);
			if (Stretch.Rise >= MinRise && (bIncludeEnds || (Index > 0 && RunEnd < Num - 1)))
			{
				Stretches.Add(Stretch);
			}
			Index = RunEnd;
		}
		return Stretches;
	}

	// Caps every tangent at MaxTangentToChord times the chord of its segment, keeping its
	// direction. A tangent much longer than its segment (vanilla sizes end tangents to the whole
	// pipe) overshoots and loops back: the kinks the pipe's curvature check rejects.
	void CapTangentsToChords(TArray<FSplinePointData>& Points)
	{
		for (int32 Index = 0; Index + 1 < Points.Num(); ++Index)
		{
			const double MaxLength = MaxTangentToChord * FVector::Dist(Points[Index].Location, Points[Index + 1].Location);
			Points[Index].LeaveTangent = Points[Index].LeaveTangent.GetClampedToMaxSize(MaxLength);
			Points[Index + 1].ArriveTangent = Points[Index + 1].ArriveTangent.GetClampedToMaxSize(MaxLength);
		}
	}

	// Whether To lies at least MinAhead in front of From, measured horizontally along From's
	// leave tangent (or straight on when it has none).
	bool IsAhead(const FSplinePointData& From, const FVector& To, double MinAhead)
	{
		const FVector Offset = To - From.Location;
		FVector Direction = From.LeaveTangent.GetSafeNormal2D();
		if (Direction.IsNearlyZero())
		{
			return FVector::Dist2D(From.Location, To) >= MinAhead;
		}
		return (FVector2D(Offset) | FVector2D(Direction)) >= MinAhead;
	}

	// Position and horizontal direction on the profile's route, at horizontal distance Along from the start.
	void RouteAt(const FGroundProfile& Profile, double Along, FVector2D& OutLocation, FVector& OutDirection)
	{
		const int32 Num = Profile.Points.Num();
		int32 Segment = 0;
		while (Segment + 2 < Num && Profile.Along[Segment + 1] <= Along)
		{
			++Segment;
		}
		const FSplinePointData& A = Profile.Points[Segment];
		const FSplinePointData& B = Profile.Points[Segment + 1];
		const double T = FMath::Clamp((Along - Profile.Along[Segment]) / Profile.Step[Segment], 0.0, 1.0);
		OutLocation = FVector2D(FMath::CubicInterp(A.Location, A.LeaveTangent, B.Location, B.ArriveTangent, T));
		OutDirection = FMath::CubicInterpDerivative(A.Location, A.LeaveTangent, B.Location, B.ArriveTangent, T).GetSafeNormal2D();
		if (OutDirection.IsNearlyZero())
		{
			OutDirection = (B.Location - A.Location).GetSafeNormal2D();
		}
	}

	// When the high side clears the ground at support height, keep its upper elbow there.
	// Ground clearance alone would put the elbow below the connector and add an S-bend.
	double PipeStepTopHeight(const FGroundProfile& Profile, const FCliff& Cliff, double DefaultTop)
	{
		const int32 High = Cliff.bUphill ? Cliff.RunEnd : Cliff.RunStart;
		const int32 End = Cliff.bUphill ? Profile.Points.Num() - 1 : 0;
		const double SupportZ = Profile.Points[End].Location.Z;
		if (High == End)
		{
			return SupportZ;
		}
		const int32 First = FMath::Min(High, End);
		const int32 Last = FMath::Max(High, End);
		double LowestGround = UE_BIG_NUMBER;
		double HighestGround = -UE_BIG_NUMBER;
		for (int32 Index = First; Index <= Last; ++Index)
		{
			if (Index != End && Profile.MinZ[Index] > SupportZ)
			{
				return DefaultTop;
			}
			if (Index != End)
			{
				LowestGround = FMath::Min(LowestGround, Profile.MinZ[Index]);
				HighestGround = FMath::Max(HighestGround, Profile.MinZ[Index]);
			}
		}
		for (const FGroundSample& Sample : Profile.Samples)
		{
			if (Sample.Segment >= First && Sample.Segment < Last && Sample.MinZ > SupportZ)
			{
				return DefaultTop;
			}
			if (Sample.Segment >= First && Sample.Segment < Last)
			{
				LowestGround = FMath::Min(LowestGround, Sample.MinZ);
				HighestGround = FMath::Max(HighestGround, Sample.MinZ);
			}
		}
		// A separate terrace between this step and the support keeps its own height.
		if (HighestGround - LowestGround >= 2.0 * PipeRiserBendRadius)
		{
			return DefaultTop;
		}
		return SupportZ;
	}

	// Direct height transition between horizontal connectors, with no intermediate crest or
	// dip. The caller must check the actual curve's clearance before accepting it.
	bool StraightenPipeRun(TArray<FSplinePointData>& Points)
	{
		if (Points.Num() < 2)
		{
			return false;
		}
		auto IsHorizontal = [](const FVector& Tangent)
		{
			return FMath::Abs(Tangent.Z) <= 0.001 * FVector2D(Tangent).Size();
		};
		if (!IsHorizontal(Points[0].LeaveTangent) || !IsHorizontal(Points.Last().ArriveTangent))
		{
			return false;
		}
		TArray<double> Step;
		TArray<double> Along;
		ComputeSteps(Points, Step, Along);
		const double StartZ = Points[0].Location.Z;
		const double EndZ = Points.Last().Location.Z;
		const double Length = Along.Last();
		for (int32 Index = 1; Index + 1 < Points.Num(); ++Index)
		{
			const double T = Along[Index] / Length;
			Points[Index].Location.Z = FMath::CubicInterp(StartZ, 0.0, EndZ, 0.0, T);
			const double Slope = 6.0 * (EndZ - StartZ) * T * (1.0 - T) / Length;
			const FVector Fallback = (Points[Index + 1].Location - Points[Index - 1].Location) * 0.5;
			SetTangentSlope(Points[Index].ArriveTangent, Slope, Fallback);
			SetTangentSlope(Points[Index].LeaveTangent, Slope, Fallback);
		}
		return true;
	}

	// Pipes: drapes like DrapeSpline, but where a steep stretch fits two elbows, the pipe takes
	// a vertical step at the foot of it (flat - 90 degree bend -
	// straight up or down - 90 degree bend - flat), like vanilla's horizontal-to-vertical mode.
	// Returns false when there is no such stretch (or no room for a step), so the caller drapes.
	bool StepPipeSpline(TArray<FSplinePointData>& SplineData, const AActor* Hologram, const FDrapeSettings& Settings)
	{
		const FTransform ToWorld = Hologram->GetActorTransform();
		FDrapeSettings StepSettings = Settings;
		StepSettings.ExtraTraceHeadroom = FMath::Max(Settings.ExtraTraceHeadroom, PipeStepMinRise * 2.0);
		// Resolve individual stair faces instead of averaging several terraces into a ramp.
		StepSettings.StepLength = FMath::Min(Settings.StepLength, 100.0);
		FGroundProfile Profile;
		if (!BuildProfile(ToWorldPoints(SplineData, ToWorld), Hologram, StepSettings, Profile))
		{
			return false;
		}
		const TArray<FSplinePointData>& Points = Profile.Points;
		const int32 Num = Points.Num();
		const double Length = Profile.Along[Num - 1];
		const double Radius = FMath::Max(PipeRiserBendRadius, Settings.MinBendRadius);
		const double Zone = Radius + PipeMinStraight; // from a step's centre to anything else
		// A gap between platforms may expose terrain far below either support. A step should
		// meet the lower support's level, not dive into that gap and immediately climb back out.
		const double LowestEndZ = FMath::Min(Points[0].Location.Z, Points.Last().Location.Z);

		// Every ground height known along the route, points and the checks between them, in order.
		TArray<TPair<double, double>> Ground;
		for (int32 Index = 0; Index < Num; ++Index)
		{
			Ground.Emplace(Profile.Along[Index], Profile.MinZ[Index]);
		}
		for (const FGroundSample& Sample : Profile.Samples)
		{
			Ground.Emplace(Profile.Along[Sample.Segment] + Sample.T * Profile.Step[Sample.Segment], Sample.MinZ);
		}
		Ground.Sort([](const TPair<double, double>& A, const TPair<double, double>& B) { return A.Key < B.Key; });

		// Put each step just in front of where the ground leaves its foot (going up), or just past
		// where it reaches its foot (going down), far enough that the bend at the top clears the
		// edge. Keep steps that leave room for their bends, apart from the ends and from each other.
		TArray<FCliff> Steps;
		TArray<double> StepAt;
		TArray<double> FootZ;
		TArray<double> TopZ;
		// Unlike belt auto-lifts, pipes also step down from elevated supports/platforms. A cliff
		// beside an end can occupy the very first/last profile segment, even for a 16 m drop.
		for (const FCliff& Stretch : FindSteepStretches(Profile, FMath::Tan(FMath::DegreesToRadians(PipeStepMinSlopeDegrees)), 2.0 * Radius, true))
		{
			const double RunStartAlong = Profile.Along[Stretch.RunStart];
			const double RunEndAlong = Profile.Along[Stretch.RunEnd];
			const double Foot = FMath::Max(Profile.MinZ[Stretch.Low], LowestEndZ);
			double Top = -UE_BIG_NUMBER;
			double Edge = Stretch.bUphill ? RunEndAlong : RunStartAlong;
			bool bEdgeFound = false;
			for (int32 GroundIndex = 0; GroundIndex < Ground.Num(); ++GroundIndex)
			{
				// Uphill scans forward from the foot, downhill backward from it. The top also looks a
				// little past the steep stretch on the high side: ground still rising there (onto a
				// platform, say) would otherwise force a second, too-tight bend right after the step.
				const TPair<double, double>& Spot = Ground[Stretch.bUphill ? GroundIndex : Ground.Num() - 1 - GroundIndex];
				const double ScanFrom = Stretch.bUphill ? RunStartAlong : RunStartAlong - 2.0 * Zone;
				const double ScanTo = Stretch.bUphill ? RunEndAlong + 2.0 * Zone : RunEndAlong;
				if (Spot.Key < ScanFrom || Spot.Key > ScanTo)
				{
					continue;
				}
				Top = FMath::Max(Top, Spot.Value);
				if (!bEdgeFound && Spot.Value > Foot + PipeStepFootTolerance)
				{
					Edge = Spot.Key;
					bEdgeFound = true;
				}
			}
			// The first high ground sample can be one sample spacing inside the platform. Keep
			// the entire top bend, including the pipe's width, outside that uncertain edge.
			const double EdgeStandOff = FMath::Max(PipeStepWallClearance,
				Radius + Settings.HalfWidth + Settings.GroundSampleSpacing);
			// An edge beside the high end may leave less than one bend of horizontal room.
			// Move the riser farther out over the low side so the high bend still fits.
			const double At = Stretch.bUphill
				? FMath::Min(Edge - EdgeStandOff, Length - Zone)
				: FMath::Max(Edge + EdgeStandOff, Zone);
			// The high endpoint already includes its support clearance. Adding the rise margin
			// there creates a small hump immediately after the support and fails curvature.
			const double TopLevel = PipeStepTopHeight(Profile, Stretch, Top + PipeRiseMargin);

			const bool bClearOfEnds = At >= Zone && At <= Length - Zone;
			const bool bClearOfLast = StepAt.IsEmpty() || At - StepAt.Last() >= 2.0 * Zone;
			// A short stair can join its two elbows directly, with no straight riser between them.
			const bool bTallEnough = TopLevel - Foot >= 2.0 * Radius;
			if (bClearOfEnds && bClearOfLast && bTallEnough)
			{
				Steps.Add(Stretch);
				StepAt.Add(At);
				FootZ.Add(Foot);
				TopZ.Add(TopLevel);
			}
		}
		if (Steps.IsEmpty())
		{
			return false;
		}

		// Over each steep stretch, on the high side of its step, the pipe stays level with the top.
		TArray<double> MinZ = Profile.MinZ;
		for (double& Z : MinZ)
		{
			Z = FMath::Max(Z, LowestEndZ);
		}
		for (int32 StepIndex = 0; StepIndex < Steps.Num(); ++StepIndex)
		{
			const FCliff& Step = Steps[StepIndex];
			for (int32 Index = Step.RunStart; Index <= Step.RunEnd; ++Index)
			{
				const bool bHighSide = Step.bUphill ? Profile.Along[Index] > StepAt[StepIndex] : Profile.Along[Index] < StepAt[StepIndex];
				if (bHighSide)
				{
					MinZ[Index] = FMath::Max(MinZ[Index], TopZ[StepIndex]);
				}
			}
		}

		const double Bend = PipeBendTangentScale * Radius;
		TArray<FSplinePointData> Out;
		FSplinePointData PieceStart = Points[0];
		double PieceStartAlong = 0.0;
		int32 NextIndex = 1;

		// Drapes the flat-ish piece from PieceStart to PieceEnd over the profile points between them.
		auto AddPiece = [&](FSplinePointData PieceEnd, double PieceEndAlong) -> bool
		{
			TArray<FSplinePointData> Piece;
			TArray<double> PieceMinZ;
			TArray<int32> PieceSources;
			Piece.Add(PieceStart);
			PieceMinZ.Add(PieceStart.Location.Z);
			PieceSources.Add(Out.IsEmpty() ? 0 : INDEX_NONE);
			for (; NextIndex + 1 < Num && Profile.Along[NextIndex] < PieceEndAlong; ++NextIndex)
			{
				// The bends sit a fixed Radius from the riser, which can be off a curving route, so
				// distances along the route aren't enough: a point could land behind a bend end and
				// the pipe would double back on itself.
				const FVector& Location = Points[NextIndex].Location;
				const bool bMovesOn = Out.IsEmpty() || Piece.Num() > 1 || IsAhead(PieceStart, Location, PipeMinStraight);
				const bool bShortOfEnd = PieceEndAlong == Length || IsAhead(FSplinePointData(Location, PieceEnd.ArriveTangent), PieceEnd.Location, PipeMinStraight);
				if (bMovesOn && bShortOfEnd)
				{
					Piece.Add(Points[NextIndex]);
					PieceMinZ.Add(MinZ[NextIndex]);
					PieceSources.Add(NextIndex);
				}
			}
			Piece.Add(PieceEnd);
			PieceMinZ.Add(PieceEnd.Location.Z);
			PieceSources.Add(PieceEndAlong == Length ? Num - 1 : INDEX_NONE);

			const int32 Last = Piece.Num() - 1;
			// Keep vanilla's exact curve on untouched segments. Resizing all tangents to their
			// chords sharpens its horizontal endpoint elbows, especially with dense stair points.
			// Only a new bend endpoint or a skipped profile point changes the segment's length.
			for (int32 Index = 0; Index <= Last; ++Index)
			{
				FSplinePointData& Point = Piece[Index];
				if (Index > 0 && !(PieceSources[Index] > 0 && PieceSources[Index - 1] == PieceSources[Index] - 1))
				{
					Point.ArriveTangent = RescaleTangent(Point.ArriveTangent, Point.Location - Piece[Index - 1].Location);
				}
				if (Index < Last && !(PieceSources[Index] >= 0 && PieceSources[Index + 1] == PieceSources[Index] + 1))
				{
					Point.LeaveTangent = RescaleTangent(Point.LeaveTangent, Piece[Index + 1].Location - Point.Location);
				}
			}

			// Keep each run above its lower connector instead of sagging toward the platform.
			const double ConnectorFloor = FMath::Min(Piece[0].Location.Z, Piece.Last().Location.Z);
			for (int32 Index = 1; Index < Last; ++Index)
			{
				PieceMinZ[Index] = FMath::Max(PieceMinZ[Index], ConnectorFloor);
			}
			TArray<FSplinePointData> Direct = Piece;
			if (StraightenPipeRun(Direct) && HasPipelineGroundClearance(ToLocalPoints(Direct, ToWorld), Hologram, Settings))
			{
				Piece = MoveTemp(Direct);
			}
			else if (!FinishDrape(Piece, PieceMinZ, Settings))
			{
				return false;
			}
			if (!Out.IsEmpty())
			{
				// The piece's start is the previous step's last bend end, already in Out.
				Out.Last().LeaveTangent = Piece[0].LeaveTangent;
				Piece.RemoveAt(0);
			}
			Out.Append(Piece);
			return true;
		};

		for (int32 StepIndex = 0; StepIndex < Steps.Num(); ++StepIndex)
		{
			const FCliff& Step = Steps[StepIndex];
			const double At = StepAt[StepIndex];
			const double FromZ = Step.bUphill ? FootZ[StepIndex] : TopZ[StepIndex];
			const double ToZ = Step.bUphill ? TopZ[StepIndex] : FootZ[StepIndex];
			const double Sign = ToZ > FromZ ? 1.0 : -1.0;
			const FVector Up(0.0, 0.0, Sign);

			FVector2D BeforeXY;
			FVector BeforeDirection;
			RouteAt(Profile, At - Radius, BeforeXY, BeforeDirection);
			FVector2D UnusedXY;
			FVector AfterDirection;
			RouteAt(Profile, At + Radius, UnusedXY, AfterDirection);
			// Put the riser and the far bend end exactly one Radius along each bend's own direction.
			// Taking them from the route instead, where it curves, leaves a bend whose ends are
			// closer or further apart than its tangents assume, and the curve overshoots the corner
			// in a fin.
			const FVector2D CentreXY = BeforeXY + FVector2D(BeforeDirection) * Radius;
			const FVector2D AfterXY = CentreXY + FVector2D(AfterDirection) * Radius;

			// Flat run into the first bend.
			if (!AddPiece(FSplinePointData(FVector(BeforeXY, FromZ), BeforeDirection, BeforeDirection), At - Radius))
			{
				return false;
			}
			Out.Last().LeaveTangent = BeforeDirection * Bend;

			// Bend, straight vertical part, bend.
			const double Straight = FMath::Abs(ToZ - FromZ) - 2.0 * Radius;
			if (Straight > 0.0)
			{
				Out.Emplace(FVector(CentreXY, FromZ + Sign * Radius), Up * Bend, Up * Straight);
				Out.Emplace(FVector(CentreXY, ToZ - Sign * Radius), Up * Straight, Up * Bend);
			}
			else
			{
				Out.Emplace(FVector(CentreXY, FromZ + Sign * Radius), Up * Bend, Up * Bend);
			}
			Out.Emplace(FVector(AfterXY, ToZ), AfterDirection * Bend, AfterDirection);

			PieceStart = Out.Last();
			PieceStartAlong = At + Radius;
			while (NextIndex + 1 < Num && Profile.Along[NextIndex] <= PieceStartAlong)
			{
				++NextIndex;
			}
		}
		if (!AddPiece(Points.Last(), Length))
		{
			return false;
		}

		SplineData = ToLocalPoints(Out, ToWorld);
		return true;
	}

	// Pipe ends that leave or arrive vertically (a free end placed on the floor or under a
	// ceiling, or a vertical connection) keep doing so: a 90 degree bend of Radius joins each to
	// the middle, which ShapeMiddle then drapes or steps between level ends. Without this the
	// middle (which drops vanilla's vertical runs) turns a vertical end sideways within a few cm:
	// the pipe loops or its shape is invalid. Returns ShapeMiddle's result.
	template <typename FShapeMiddle>
	bool ShapeWithVerticalEnds(TArray<FSplinePointData>& SplineData, const AActor* Hologram, double Radius, const FDrapeSettings& Settings, FShapeMiddle ShapeMiddle)
	{
		const FTransform ToWorld = Hologram->GetActorTransform();
		const TArray<FSplinePointData> World = ToWorldPoints(SplineData, ToWorld);
		auto IsVertical = [](const FVector& Tangent) { return FMath::Abs(Tangent.GetSafeNormal().Z) > 0.7; };
		if (World.Num() < 2 || (!IsVertical(World[0].LeaveTangent) && !IsVertical(World.Last().ArriveTangent)))
		{
			return ShapeMiddle(SplineData);
		}
		const FSplinePointData Start = World[0];
		const FSplinePointData End = World.Last();
		const bool bStartVertical = IsVertical(Start.LeaveTangent);
		const bool bEndVertical = IsVertical(End.ArriveTangent);

		// Which way the route heads off level from each end. A pipe that is vertical all the way
		// has none, and is left to the middle shaping as a whole.
		constexpr double MinOffset = 10.0;
		FVector StartDirection = FVector::ZeroVector;
		FVector EndDirection = FVector::ZeroVector;
		for (const FSplinePointData& Point : World)
		{
			if (FVector::Dist2D(Point.Location, Start.Location) >= MinOffset)
			{
				StartDirection = (Point.Location - Start.Location).GetSafeNormal2D();
				break;
			}
		}
		for (int32 Index = World.Num() - 1; Index >= 0; --Index)
		{
			if (FVector::Dist2D(End.Location, World[Index].Location) >= MinOffset)
			{
				EndDirection = (End.Location - World[Index].Location).GetSafeNormal2D();
				break;
			}
		}
		if (StartDirection.IsNearlyZero() || EndDirection.IsNearlyZero())
		{
			return ShapeMiddle(SplineData);
		}

		// Where each bend turns level. An end the pipe hangs down from (under a ceiling, or a
		// connection facing down) drops straight to the ground first, so the pipe runs along the
		// ground and rises at the end, like vanilla's horizontal-to-vertical mode, instead of
		// ramping all the way up.
		const double StartSign = Start.LeaveTangent.Z > 0.0 ? 1.0 : -1.0;
		const double EndSign = End.ArriveTangent.Z > 0.0 ? 1.0 : -1.0;
		auto LevelBelow = [&](const FVector& Tip, const FVector& BendEnd)
		{
			const double Default = Tip.Z - Radius;
			const UWorld* World = Hologram->GetWorld();
			double GroundAtTip = 0.0;
			double GroundAtBendEnd = 0.0;
			// Start below the bend: the end sits inside the floor it hangs from (a pipe floor hole),
			// and a trace starting there found that floor itself and left no room to drop.
			const double Top = Tip.Z - Radius;
			const double Bottom = Tip.Z - MaxRiserDrop;
			const bool bFound = World != nullptr &&
				FindGroundZ(World, Hologram, FVector2D(Tip), Top, Bottom, Top, GroundAtTip) &&
				FindGroundZ(World, Hologram, FVector2D(BendEnd), Top, Bottom, Top, GroundAtBendEnd);
			const double Level = bFound ? FMath::Max(GroundAtTip, GroundAtBendEnd) + Settings.GroundClearance : Default;
			// Keep the bend and some straight between the level and the tip.
			const double Result = Level <= Default - PipeMinStraight ? Level : Default;
			if (FirstTime(*FString::Printf(TEXT("riser %d"), static_cast<int32>(Result < Default))))
			{
				UE_LOG(LogTerrainBelts, Display, TEXT("Vertical pipe end at Z %.0f: ground %s (%.0f / %.0f), level %.0f"),
					Tip.Z, bFound ? TEXT("found") : TEXT("not found"), bFound ? GroundAtTip : 0.0, bFound ? GroundAtBendEnd : 0.0, Result);
			}
			return Result;
		};
		double StartLevel = Start.Location.Z + StartSign * Radius;
		if (bStartVertical && StartSign < 0.0)
		{
			StartLevel = LevelBelow(Start.Location, Start.Location + StartDirection * Radius);
		}
		double EndLevel = End.Location.Z - EndSign * Radius;
		if (bEndVertical && EndSign > 0.0)
		{
			EndLevel = LevelBelow(End.Location, End.Location - EndDirection * Radius);
		}
		const FVector First = bStartVertical ? FVector(FVector2D(Start.Location + StartDirection * Radius), StartLevel) : Start.Location;
		const FVector Last = bEndVertical ? FVector(FVector2D(End.Location - EndDirection * Radius), EndLevel) : End.Location;
		// Ends so close that the bends meet or pass each other: no room for this shape.
		const FVector2D Between = FVector2D(Last - First);
		const bool bBendsApart = (!bStartVertical || (Between | FVector2D(StartDirection)) >= 2.0 * PipeMinStraight) &&
			(!bEndVertical || (Between | FVector2D(EndDirection)) >= 2.0 * PipeMinStraight);
		if (FVector::Dist2D(First, Last) < 2.0 * PipeMinStraight || !bBendsApart)
		{
			return ShapeMiddle(SplineData);
		}

		// The vanilla route between them, without what lies inside the bends.
		const double Clear = Radius + MinHorizontalSpacing;
		TArray<FSplinePointData> Middle;
		Middle.Emplace(First, bStartVertical ? StartDirection : Start.LeaveTangent);
		for (int32 Index = 1; Index + 1 < World.Num(); ++Index)
		{
			const FVector& Location = World[Index].Location;
			// Also keep route points a bend's length clear of where the bends turn level: one just
			// beside it, off the bend's line, makes a hairpin that overshoots the corner.
			const bool bClearOfStart = !bStartVertical || (FVector::Dist2D(Location, Start.Location) >= Clear && FVector::Dist2D(Location, First) >= Radius &&
				IsAhead(FSplinePointData(First, StartDirection), Location, PipeMinStraight));
			const bool bClearOfEnd = !bEndVertical || (FVector::Dist2D(Location, End.Location) >= Clear && FVector::Dist2D(Location, Last) >= Radius &&
				IsAhead(FSplinePointData(Location, EndDirection), Last, PipeMinStraight));
			if (bClearOfStart && bClearOfEnd)
			{
				Middle.Add(World[Index]);
			}
		}
		Middle.Emplace(Last, bEndVertical ? EndDirection : End.ArriveTangent);
		// Size the tangents to the remaining segments (Subdivide scales them per piece).
		for (int32 Index = 0; Index < Middle.Num(); ++Index)
		{
			FSplinePointData& Point = Middle[Index];
			if (Index > 0)
			{
				Point.ArriveTangent = RescaleTangent(Point.ArriveTangent, Point.Location - Middle[Index - 1].Location);
			}
			if (Index + 1 < Middle.Num())
			{
				Point.LeaveTangent = RescaleTangent(Point.LeaveTangent, Middle[Index + 1].Location - Point.Location);
			}
		}

		TArray<FSplinePointData> MiddleData = ToLocalPoints(Middle, ToWorld);
		const bool bShaped = ShapeMiddle(MiddleData);
		TArray<FSplinePointData> Out = ToWorldPoints(MiddleData, ToWorld);

		// Each vertical end: bend, then (if the riser drops further than the bend) a straight
		// vertical run to the end.
		const double Bend = PipeBendTangentScale * Radius;
		if (bStartVertical)
		{
			const FVector Up(0.0, 0.0, StartSign);
			const double Straight = FMath::Abs(Start.Location.Z - First.Z) - Radius;
			Out[0].ArriveTangent = StartDirection * Bend;
			if (Straight > 1.0)
			{
				Out.Insert(FSplinePointData(FVector(FVector2D(Start.Location), First.Z - StartSign * Radius), Up * Straight, Up * Bend), 0);
				Out.Insert(FSplinePointData(Start.Location, Start.ArriveTangent, Up * Straight), 0);
			}
			else
			{
				Out.Insert(FSplinePointData(Start.Location, Start.ArriveTangent, Up * Bend), 0);
			}
		}
		if (bEndVertical)
		{
			const FVector Up(0.0, 0.0, EndSign);
			const double Straight = FMath::Abs(End.Location.Z - Last.Z) - Radius;
			Out.Last().LeaveTangent = EndDirection * Bend;
			if (Straight > 1.0)
			{
				Out.Emplace(FVector(FVector2D(End.Location), Last.Z + EndSign * Radius), Up * Bend, Up * Straight);
				Out.Emplace(End.Location, Up * Straight, End.LeaveTangent);
			}
			else
			{
				Out.Emplace(End.Location, Up * Bend, End.LeaveTangent);
			}
		}
		SplineData = ToLocalPoints(Out, ToWorld);

		if (FirstTime(TEXT("vertical pipe ends")))
		{
			UE_LOG(LogTerrainBelts, Display, TEXT("Pipe with vertical end(s) (start %s, end %s): bends of %.0f cm, middle %s"),
				bStartVertical ? TEXT("yes") : TEXT("no"), bEndVertical ? TEXT("yes") : TEXT("no"), Radius, bShaped ? TEXT("shaped") : TEXT("vanilla"));
		}
		return true;
	}

	// Recipe_ConveyorBeltMkN -> Recipe_ConveyorLiftMkN. Belts from other mods get no auto-lift.
	TSubclassOf<UFGRecipe> FindLiftRecipe(TSubclassOf<UFGRecipe> BeltRecipe)
	{
		if (BeltRecipe == nullptr)
		{
			return nullptr;
		}
		FString Path = BeltRecipe->GetPathName();
		if (!Path.Contains(TEXT("Recipe_ConveyorBelt")))
		{
			return nullptr;
		}
		Path.ReplaceInline(TEXT("Recipe_ConveyorBelt"), TEXT("Recipe_ConveyorLift"));
		return LoadClass<UFGRecipe>(nullptr, *Path);
	}

	void ConnectIfFree(UFGFactoryConnectionComponent* From, UFGFactoryConnectionComponent* To)
	{
		if (From == nullptr || To == nullptr || From->IsConnected() || To->IsConnected())
		{
			return;
		}
		From->SetConnection(To);
		if (FirstTime(TEXT("connect")))
		{
			UE_LOG(LogTerrainBelts, Display, TEXT("Auto-lift: connected %s to %s"), *GetFullNameSafe(From), *GetFullNameSafe(To));
		}
	}

	// Connects a belt piece to a lift. The lift's input (Connection0) is at the actor, where
	// PlaceLift put it, and its output (Connection1) at the top transform. The belt meets the lift
	// with whichever end of its spline lies nearer the lift, so it doesn't matter which way round
	// vanilla built it. (While the newest piece is still being constructed its connection
	// components may not be positioned yet, but its spline and actor transform are.)
	void ConnectBeltToLift(AFGBuildableConveyorBelt* Belt, AFGBuildableConveyorLift* Lift, bool bBeltIsUpstream)
	{
		const TArray<FSplinePointData>& Spline = Belt->GetSplinePointData();
		if (Spline.Num() < 2)
		{
			return;
		}
		const FTransform& BeltTransform = Belt->GetActorTransform();
		const FVector LiftLocation = Lift->GetActorLocation();
		const double ToStart = FVector::Dist2D(BeltTransform.TransformPosition(Spline[0].Location), LiftLocation);
		const double ToEnd = FVector::Dist2D(BeltTransform.TransformPosition(Spline.Last().Location), LiftLocation);
		UFGFactoryConnectionComponent* BeltEnd = ToStart < ToEnd ? Belt->GetConnection0() : Belt->GetConnection1();
		ConnectIfFree(BeltEnd, bBeltIsUpstream ? Lift->GetConnection0() : Lift->GetConnection1());
	}

	// A piece of an active belt -> lift -> belt split: the main belt, or one of our enabled children.
	bool IsAutoLiftPiece(const AFGHologram* Hologram)
	{
		if (const AFGHologram* Parent = Hologram->GetParentHologram())
		{
			bool bIsLift;
			int32 Index;
			return !Hologram->IsDisabled() && FindAutoLiftChild(Parent, Hologram, bIsLift, Index);
		}
		return NumActiveLifts(Hologram) > 0;
	}

	// Follow Terrain is a toggle rather than a build mode: while it's on, the Default, Straight and
	// Curve modes drape over the ground. Local to this game process; the draped spline itself is
	// what the construct message carries.
	bool bFollowTerrainEnabled = false;

	const TCHAR* const InputContentPath = TEXT("/TerrainBelts/Input");
	const TCHAR* const ToggleActionPath = TEXT("/TerrainBelts/Input/IA_TB_ToggleFollowTerrain.IA_TB_ToggleFollowTerrain");

	UInputAction* GetToggleAction()
	{
		static UInputAction* Action = []
		{
			UInputAction* Loaded = LoadObject<UInputAction>(nullptr, ToggleActionPath);
			if (Loaded == nullptr)
			{
				UE_LOG(LogTerrainBelts, Error, TEXT("Failed to load input action %s"), ToggleActionPath);
				return Loaded;
			}
			// Only native code references it, so keep it from being garbage collected.
			Loaded->AddToRoot();
			return Loaded;
		}();
		return Action;
	}

	// The rebindable key lives in a child context of the build gun's build context, so it only
	// works while building. The game finds child contexts through the Asset Manager, which only
	// scans game feature plugins for them, so register this mod's input folder explicitly.
	void RegisterChildInputContexts()
	{
		UAssetManager::CallOrRegister_OnAssetManagerCreated(FSimpleMulticastDelegate::FDelegate::CreateLambda([]
		{
			const int32 NumFound = UAssetManager::Get().ScanPathForPrimaryAssets(
				FPrimaryAssetType(UFGChildInputMappingContext::StaticClass()->GetFName()),
				InputContentPath, UFGChildInputMappingContext::StaticClass(), false);
			UE_LOG(LogTerrainBelts, Display, TEXT("Registered %d child input context(s) from %s"), NumFound, InputContentPath);
		}));
	}

	void HandleInput(AFGCharacterPlayer* Player)
	{
		const AFGPlayerController* PlayerController = Cast<AFGPlayerController>(Player->GetController());
		const UEnhancedPlayerInput* PlayerInput = PlayerController != nullptr ? Cast<UEnhancedPlayerInput>(PlayerController->PlayerInput) : nullptr;
		const UInputAction* ToggleAction = GetToggleAction();
		const FInputActionInstance* Instance = PlayerInput != nullptr && ToggleAction != nullptr ? PlayerInput->FindActionInstanceData(ToggleAction) : nullptr;
		if (Instance == nullptr || Instance->GetTriggerEvent() != ETriggerEvent::Triggered)
		{
			return;
		}

		bFollowTerrainEnabled = !bFollowTerrainEnabled;
		UE_LOG(LogTerrainBelts, Display, TEXT("Follow Terrain %s"), bFollowTerrainEnabled ? TEXT("enabled") : TEXT("disabled"));
	}

	// The build gun's key hint bar. Its rows live in the Widget_BuildMode Blueprint, so they're
	// reached through reflection: mCachedKeybinds holds the rows on show and SetKeybindingHints
	// replaces them. A row is a Blueprint struct with an Action and a KeyBinding text; the widget
	// turns a "{Name}" key text into whatever key the player has bound to that key setting.
	const TCHAR* const BuildModeWidgetPath = TEXT("/Game/FactoryGame/Interface/UI/InGame/BuildMenu/Widget_BuildMode.Widget_BuildMode_C");
	const TCHAR* const ToggleKeyHint = TEXT("{TB.ToggleFollowTerrain}");
	// Finding the widget walks every object, so a missing one is only looked for this often (s).
	constexpr double BuildModeWidgetSearchInterval = 1.0;

	struct FHintRow
	{
		FText Action;
		FText KeyBinding;
	};

	TWeakObjectPtr<UUserWidget> BuildModeWidget;
	double NextBuildModeWidgetSearch = 0.0;

	// Blueprint struct members carry a generated suffix, so match on the start of their names.
	bool FindHintRowFields(const FProperty* ArrayProperty, FTextProperty*& OutAction, FTextProperty*& OutKeyBinding)
	{
		const FArrayProperty* Array = CastField<FArrayProperty>(ArrayProperty);
		const FStructProperty* Row = Array != nullptr ? CastField<FStructProperty>(Array->Inner) : nullptr;
		OutAction = nullptr;
		OutKeyBinding = nullptr;
		if (Row == nullptr)
		{
			return false;
		}
		for (TFieldIterator<FTextProperty> It(Row->Struct); It; ++It)
		{
			if (It->GetName().StartsWith(TEXT("Action")))
			{
				OutAction = *It;
			}
			else if (It->GetName().StartsWith(TEXT("KeyBinding")))
			{
				OutKeyBinding = *It;
			}
		}
		return OutAction != nullptr && OutKeyBinding != nullptr;
	}

	bool ReadHintRows(const UUserWidget* Widget, TArray<FHintRow>& OutRows)
	{
		const FArrayProperty* Array = FindFProperty<FArrayProperty>(Widget->GetClass(), TEXT("mCachedKeybinds"));
		FTextProperty* Action;
		FTextProperty* KeyBinding;
		if (!FindHintRowFields(Array, Action, KeyBinding))
		{
			return false;
		}
		FScriptArrayHelper Rows(Array, Array->ContainerPtrToValuePtr<void>(Widget));
		for (int32 Index = 0; Index < Rows.Num(); ++Index)
		{
			const uint8* Row = Rows.GetRawPtr(Index);
			OutRows.Add({Action->GetPropertyValue_InContainer(Row), KeyBinding->GetPropertyValue_InContainer(Row)});
		}
		return true;
	}

	bool WriteHintRows(UUserWidget* Widget, const TArray<FHintRow>& Rows)
	{
		UFunction* Function = Widget->FindFunction(TEXT("SetKeybindingHints"));
		FProperty* Param = nullptr;
		if (Function != nullptr && Function->NumParms == 1)
		{
			Param = *TFieldIterator<FProperty>(Function);
			if (Param != nullptr && (!Param->HasAnyPropertyFlags(CPF_Parm) || !Param->IsInContainer(Function->ParmsSize)))
			{
				Param = nullptr;
			}
		}
		FTextProperty* Action;
		FTextProperty* KeyBinding;
		if (Param == nullptr || !FindHintRowFields(Param, Action, KeyBinding))
		{
			if (FirstTime(TEXT("hint bar")))
			{
				UE_LOG(LogTerrainBelts, Warning, TEXT("%s has no usable SetKeybindingHints; the Follow Terrain key hint can't be shown"), *GetNameSafe(Widget->GetClass()));
			}
			return false;
		}

		// Only the parameter block: UFunction::InitializeStruct would also construct the
		// Blueprint's local variables, which lie past ParmsSize, and overrun the buffer.
		uint8* Params = static_cast<uint8*>(FMemory_Alloca_Aligned(Function->ParmsSize, Function->GetMinAlignment()));
		FMemory::Memzero(Params, Function->ParmsSize);
		Param->InitializeValue_InContainer(Params);
		FScriptArrayHelper Out(CastField<FArrayProperty>(Param), Param->ContainerPtrToValuePtr<void>(Params));
		for (const FHintRow& Row : Rows)
		{
			uint8* Entry = Out.GetRawPtr(Out.AddValue());
			Action->SetPropertyValue_InContainer(Entry, Row.Action);
			KeyBinding->SetPropertyValue_InContainer(Entry, Row.KeyBinding);
		}
		Widget->ProcessEvent(Function, Params);
		Param->DestroyValue_InContainer(Params);
		return true;
	}

	bool IsShown(const UUserWidget* Widget)
	{
		const ESlateVisibility Visibility = Widget->GetVisibility();
		return Widget->GetCachedWidget().IsValid() && Visibility != ESlateVisibility::Collapsed && Visibility != ESlateVisibility::Hidden;
	}

	UUserWidget* FindBuildModeWidget(const UWorld* World)
	{
		UUserWidget* Cached = BuildModeWidget.Get();
		if (Cached != nullptr && Cached->GetWorld() == World && IsShown(Cached))
		{
			return Cached;
		}
		const double Now = FPlatformTime::Seconds();
		if (Now < NextBuildModeWidgetSearch)
		{
			return nullptr;
		}
		NextBuildModeWidgetSearch = Now + BuildModeWidgetSearchInterval;

		static UClass* WidgetClass = []
		{
			UClass* Loaded = LoadClass<UUserWidget>(nullptr, BuildModeWidgetPath);
			if (Loaded != nullptr)
			{
				Loaded->AddToRoot();
			}
			return Loaded;
		}();
		if (WidgetClass == nullptr)
		{
			return nullptr;
		}
		for (TObjectIterator<UUserWidget> It(RF_ClassDefaultObject | RF_ArchetypeObject); It; ++It)
		{
			// Dismantle mode uses the same widget class.
			if (It->GetClass() == WidgetClass && It->GetWorld() == World && IsShown(*It) && !It->GetName().Contains(TEXT("Dismantle")))
			{
				BuildModeWidget = *It;
				return *It;
			}
		}
		return nullptr;
	}

	// Shows "Follow Terrain: On/Off" with its key while placing a belt, pipe, hypertube or track,
	// so the toggle's state is visible even on flat ground.
	void UpdateKeyHint(AFGCharacterPlayer* Player)
	{
		const AFGBuildGun* BuildGun = Player->GetBuildGun();
		const UFGBuildGunStateBuild* BuildState = BuildGun != nullptr && BuildGun->IsEquipped() ? Cast<UFGBuildGunStateBuild>(BuildGun->GetCurrentState()) : nullptr;
		const AFGHologram* Hologram = BuildState != nullptr ? BuildState->GetHologram() : nullptr;
		const bool bShow = Hologram != nullptr && Hologram->GetParentHologram() == nullptr &&
			(Hologram->IsA<AFGConveyorBeltHologram>() || Hologram->IsA<AFGPipelineHologram>() || Hologram->IsA<AFGRailroadTrackHologram>());

		// Only look for the widget while there's a hint to show; otherwise just clean up after it.
		UUserWidget* Widget = bShow ? FindBuildModeWidget(Player->GetWorld()) : BuildModeWidget.Get();
		TArray<FHintRow> Rows;
		if (Widget == nullptr || !ReadHintRows(Widget, Rows))
		{
			return;
		}
		const int32 Ours = Rows.IndexOfByPredicate([](const FHintRow& Row) { return Row.KeyBinding.ToString() == ToggleKeyHint; });
		if (bShow)
		{
			const FText Label = bFollowTerrainEnabled ? INVTEXT("Follow Terrain: On") : INVTEXT("Follow Terrain: Off");
			if (Ours == INDEX_NONE)
			{
				Rows.Add({Label, FText::FromString(ToggleKeyHint)});
			}
			else if (!Rows[Ours].Action.EqualTo(Label))
			{
				Rows[Ours].Action = Label;
			}
			else
			{
				return;
			}
		}
		else if (Ours != INDEX_NONE)
		{
			Rows.RemoveAt(Ours);
		}
		else
		{
			return;
		}
		WriteHintRows(Widget, Rows);
	}
}

bool FTerrainBeltsModule::ShouldFollowTerrain(const AFGHologram* Hologram)
{
	using namespace TerrainBelts;

	if (!bFollowTerrainEnabled || Hologram == nullptr)
	{
		return false;
	}
	// Rails have no build modes of their own.
	if (Hologram->IsA<AFGRailroadTrackHologram>())
	{
		return true;
	}

	// Default, Straight and Curve keep their shape from above and only change height. Shape
	// modes (pipe Noodle, 2D, Horizontal to Vertical) stay vanilla.
	TArray<TSubclassOf<UFGHologramBuildModeDescriptor>, TInlineAllocator<4>> Modes{Hologram->GetDefaultBuildGunMode()};
	if (const AFGConveyorBeltHologram* Belt = Cast<AFGConveyorBeltHologram>(Hologram))
	{
		Modes.Append({Belt->mBuildModeStraight, Belt->mBuildModeCurve});
	}
	else if (const AFGPipelineHologram* Pipe = Cast<AFGPipelineHologram>(Hologram))
	{
		Modes.Append({Pipe->mBuildModeAuto, Pipe->mBuildModeStraight, Pipe->mBuildModeCurve});
	}
	else
	{
		return false;
	}
	return Modes.ContainsByPredicate([Hologram](const TSubclassOf<UFGHologramBuildModeDescriptor>& Mode)
	{
		return Mode != nullptr && Hologram->IsCurrentBuildMode(Mode);
	});
}

void FTerrainBeltsModule::SpawnAutoLiftChildren(AFGConveyorBeltHologram* Belt, AActor* HologramOwner, const FVector& SpawnLocation)
{
#if TERRAINBELTS_ENABLE_AUTO_LIFT
	using namespace TerrainBelts;

	// Only the belt the player is placing, not our own second belt (or a belt inside another hologram).
	if (Belt->GetParentHologram() != nullptr)
	{
		return;
	}

	const TSubclassOf<UFGRecipe> LiftRecipe = FindLiftRecipe(Belt->GetRecipe());
	AFGRecipeManager* RecipeManager = AFGRecipeManager::Get(Belt->GetWorld());
	if (LiftRecipe == nullptr || RecipeManager == nullptr || !RecipeManager->IsRecipeAvailable(LiftRecipe))
	{
		if (FirstTime(TEXT("no lift recipe")))
		{
			UE_LOG(LogTerrainBelts, Display, TEXT("Auto-lift: no available lift recipe for %s"), *GetNameSafe(Belt->GetRecipe()));
		}
		return;
	}

	for (int32 Index = 0; Index < MaxAutoLifts; ++Index)
	{
		AFGHologram* Lift = AFGHologram::SpawnChildHologramFromRecipe(Belt, LiftChildName(Index), LiftRecipe, HologramOwner, SpawnLocation);
		AFGHologram* NextBelt = AFGHologram::SpawnChildHologramFromRecipe(Belt, BeltChildName(Index), Belt->GetRecipe(), HologramOwner, SpawnLocation,
			[](AFGHologram* Child)
			{
				// Its ends are lifts and the main belt's end, so it needs no poles of its own.
				Child->SetShouldSpawnChildHolograms(false);
			});
		if (Lift != nullptr)
		{
			Lift->SetDisabled(true);
		}
		if (NextBelt != nullptr)
		{
			NextBelt->SetDisabled(true);
		}
	}

	if (FirstTime(TEXT("spawn children")))
	{
		UE_LOG(LogTerrainBelts, Display, TEXT("Auto-lift: spawned %d lift and belt children for %s"), MaxAutoLifts, *GetNameSafe(Belt));
	}
#endif
}

bool FTerrainBeltsModule::IsDraggedAgainstFlow(const AFGConveyorBeltHologram* Belt)
{
	using namespace TerrainBelts;

	if (Belt == nullptr)
	{
		return false;
	}
	const UFGFactoryConnectionComponent* StartSnap = Belt->mSnappedConnectionComponents[0];
	if (StartSnap != nullptr && StartSnap->GetDirection() == EFactoryConnectionDirection::FCD_INPUT)
	{
		return true;
	}
	// ApplyAutoLift hands the end snap to the last belt piece, so look there too.
	TArray<const AFGConveyorBeltHologram*> Holders{Belt};
	for (int32 Index = 0; Index < MaxAutoLifts; ++Index)
	{
		if (const AFGConveyorBeltHologram* NextBelt = Cast<AFGConveyorBeltHologram>(Belt->FindChildHologramByName(BeltChildName(Index))))
		{
			Holders.Add(NextBelt);
		}
	}
	for (const AFGConveyorBeltHologram* Holder : Holders)
	{
		for (int32 Side = 0; Side < 2; ++Side)
		{
			if (Holder == Belt && Side == 0)
			{
				continue; // the start snap, checked above
			}
			const UFGFactoryConnectionComponent* Snap = Holder->mSnappedConnectionComponents[Side];
			if (Snap != nullptr && Snap->GetDirection() == EFactoryConnectionDirection::FCD_OUTPUT)
			{
				return true;
			}
		}
	}
	return false;
}

bool FTerrainBeltsModule::PlaceLift(AFGConveyorLiftHologram* Lift, const FVector& Entry, const FVector& Direction, double SignedHeight, FVector& OutExit)
{
	using namespace TerrainBelts;

	UFGFactoryConnectionComponent* Input = Lift->mConnectionComponents[0];
	UFGFactoryConnectionComponent* Output = Lift->mConnectionComponents[1];
	if (Input == nullptr || Output == nullptr)
	{
		return false;
	}

	// The input sits at the actor and the output at the top transform, which may point down.
	Lift->mTopTransform = FTransform(FVector(0.0, 0.0, SignedHeight));
	Lift->SetActorLocationAndRotation(Entry, Direction.Rotation());
	Lift->OnRep_TopTransform();

	// Turn the lift so its input faces back along the incoming belt, and its output faces on.
	const double InputYaw = Input->GetConnectorNormal().Rotation().Yaw;
	Lift->AddActorWorldRotation(FRotator(0.0, (-Direction).Rotation().Yaw - InputYaw, 0.0));
	if ((Output->GetConnectorNormal() | Direction) < 0.0)
	{
		Lift->mTopTransform.SetRotation(FRotator(0.0, 180.0, 0.0).Quaternion());
		Lift->OnRep_TopTransform();
	}

	// Line the input up exactly with the end of the first belt.
	Lift->AddActorWorldOffset(Entry - Input->GetComponentLocation());
	OutExit = Output->GetComponentLocation();

	// Not snapped and not in Reverse mode, this makes the input the actor end and refreshes the
	// flow arrows, which vanilla only does during its own placement.
	Lift->UpdateConnectionDirections();

	const FVector ExpectedExit = Entry + FVector(0.0, 0.0, SignedHeight);
	if (FMath::Abs(OutExit.Z - ExpectedExit.Z) > LiftExitTolerance)
	{
		// The hologram doesn't move its output up with the top transform (seen in game: it stays
		// at the bottom, about 1 m from the input). Place it as the top transform carries it: its
		// layout relative to the lift, raised (and turned) by the top transform.
		const FVector Carried = (Output->GetRelativeTransform() * Lift->mTopTransform * Lift->GetActorTransform()).GetLocation();
		const bool bPlausible = FMath::Abs(Carried.Z - ExpectedExit.Z) <= LiftExitTolerance && FVector::Dist2D(Carried, Entry) <= 4.0 * LiftExitTolerance;
		if (FirstTime(TEXT("lift exit")))
		{
			UE_LOG(LogTerrainBelts, Display, TEXT("Auto-lift: hologram output at %s, carried by the top transform to %s (%s), straight above the input %s"),
				*OutExit.ToString(), *Carried.ToString(), bPlausible ? TEXT("used") : TEXT("implausible"), *ExpectedExit.ToString());
		}
		OutExit = bPlausible ? Carried : ExpectedExit;
	}
	return true;
}

bool FTerrainBeltsModule::ApplyAutoLift(AFGConveyorBeltHologram* Belt, const TerrainBelts::FDrapeSettings& Settings)
{
#if TERRAINBELTS_ENABLE_AUTO_LIFT
	using namespace TerrainBelts;

	TArray<AFGConveyorLiftHologram*> Lifts;
	TArray<AFGConveyorBeltHologram*> NextBelts;
	for (int32 Index = 0; Index < MaxAutoLifts; ++Index)
	{
		AFGConveyorLiftHologram* Lift = Cast<AFGConveyorLiftHologram>(Belt->FindChildHologramByName(LiftChildName(Index)));
		AFGConveyorBeltHologram* NextBelt = Cast<AFGConveyorBeltHologram>(Belt->FindChildHologramByName(BeltChildName(Index)));
		if (Lift == nullptr || NextBelt == nullptr)
		{
			break;
		}
		Lifts.Add(Lift);
		NextBelts.Add(NextBelt);
	}
	if (Lifts.IsEmpty())
	{
		return false;
	}

	// Items flow from the start of the drag to the end, unless the start is snapped to an input
	// (or the end to an output): then the belt was dragged against the flow. Vanilla builds the
	// main belt to match its snap; the lifts and our own belt pieces are turned around here.
	const bool bReversed = IsDraggedAgainstFlow(Belt);

	const AFGConveyorLiftHologram* AnyLift = Lifts[0];
	const double StepHeight = AnyLift->mStepHeight > 0.0f ? AnyLift->mStepHeight : FallbackLiftStepHeight;
	const double MinHeight = AnyLift->mMinimumHeight > 0.0f ? AnyLift->mMinimumHeight : FallbackLiftMinHeight;
	const double MaxHeight = AnyLift->mMaximumHeight > 0.0f ? AnyLift->mMaximumHeight : FallbackLiftMaxHeight;

	FDrapeSettings LiftSettings = Settings;
	LiftSettings.ExtraTraceHeadroom = MaxHeight;
	const FTransform BeltTransform = Belt->GetActorTransform();
	FGroundProfile Profile;
	if (!BuildProfile(ToWorldPoints(Belt->mSplineData, BeltTransform), Belt, LiftSettings, Profile))
	{
		return false;
	}
	const double ClearanceOffset = AutoLiftClearanceOffset(Profile);
	if (ClearanceOffset > 0.0)
	{
		// All lift feet and connecting belt pieces use the same support clearance, including
		// plateaus between the second/third lifts. The ordinary terrain drape stays unchanged.
		LiftSettings.GroundClearance += ClearanceOffset;
		if (!BuildProfile(ToWorldPoints(Belt->mSplineData, BeltTransform), Belt, LiftSettings, Profile))
		{
			return false;
		}
	}

	const TArray<FSplinePointData>& Points = Profile.Points;
	const int32 Num = Points.Num();
	const double TotalLength = Profile.Along[Num - 1];

	// A lift is only worth it where a plain belt can't cope: somewhere steeper than the incline
	// allows (it can't get back down to an end), or riding high above the ground to clear a
	// cliff. A short steep patch that a slightly raised ramp handles gets no lift.
	{
		TArray<FSplinePointData> Plain = Points;
		if (FinishDrape(Plain, Profile.MinZ, Settings, Profile.Samples))
		{
			double HighestFloat = 0.0;
			bool bTooSteep = false;
			for (int32 Index = 0; Index + 1 < Num; ++Index)
			{
				HighestFloat = FMath::Max(HighestFloat, Plain[Index].Location.Z - Profile.MinZ[Index]);
				bTooSteep |= FMath::Abs(Plain[Index + 1].Location.Z - Plain[Index].Location.Z) > AutoLiftSteepnessTolerance * Profile.MaxSlope * Profile.Step[Index];
			}
			if (!bTooSteep && HighestFloat <= AutoLiftMaxFloat)
			{
				return false;
			}
		}
	}

	// Lifts come in whole steps; round up so the top clears the cliff edge. A drop taller than
	// a lift gets the tallest lift, and the belt ramps the rest.
	const double TallestLift = FMath::FloorToDouble(MaxHeight / StepHeight) * StepHeight;
	auto LiftHeightFor = [&](const FCliff& Cliff)
	{
		return FMath::Clamp(FMath::CeilToDouble(Cliff.Rise / StepHeight) * StepHeight, MinHeight, FMath::Max(TallestLift, MinHeight));
	};

	// Take the biggest cliffs first, as many as there are lifts, keeping enough belt between
	// lifts and at both ends; then put them back in order along the belt. Drops right at an end
	// count too (a belt starting high up on a building): the plain-belt check above already
	// rules out an end that is only a little raised.
	TArray<FCliff> Cliffs = FindSteepStretches(Profile, Profile.MaxSlope, FMath::Max(AutoLiftMinRise, MinHeight), true);
	Cliffs.Sort([](const FCliff& A, const FCliff& B) { return A.Rise > B.Rise; });
	TArray<FCliff> Chosen;
	for (FCliff Cliff : Cliffs)
	{
		// A drop right next to an end (a belt leaving the top of a building) leaves no room for
		// the belt before the lift. Stand the lift further out on the low ground instead; the
		// belt runs level from the high end to its top.
		if (!Cliff.bUphill && Profile.Along[Cliff.Low] < AutoLiftMinBeltLength)
		{
			int32 Index = Cliff.Low;
			while (Index + 2 < Num && Profile.Along[Index] < AutoLiftMinBeltLength)
			{
				++Index;
			}
			Cliff.Low = Cliff.RunEnd = Index;
			Cliff.Rise = FMath::Abs(Profile.MinZ[Cliff.RunStart] - Profile.MinZ[Index]);
		}
		else if (Cliff.bUphill && TotalLength - Profile.Along[Cliff.Low] < AutoLiftMinBeltLength)
		{
			int32 Index = Cliff.Low;
			while (Index > 1 && TotalLength - Profile.Along[Index] < AutoLiftMinBeltLength)
			{
				--Index;
			}
			Cliff.Low = Cliff.RunStart = Index;
			Cliff.Rise = FMath::Abs(Profile.MinZ[Cliff.RunEnd] - Profile.MinZ[Index]);
		}

		const double At = Profile.Along[Cliff.Low];
		if (Cliff.Rise > MaxHeight && FirstTime(TEXT("cliff too high")))
		{
			UE_LOG(LogTerrainBelts, Display, TEXT("Auto-lift: cliff of %.0f cm is taller than a lift (%.0f cm); the belt ramps the rest"), Cliff.Rise, MaxHeight);
		}
		bool bRoom = At >= AutoLiftMinBeltLength && TotalLength - At >= AutoLiftMinBeltLength;
		for (const FCliff& Other : Chosen)
		{
			bRoom &= FMath::Abs(At - Profile.Along[Other.Low]) >= 2.0 * AutoLiftMinBeltLength;
		}
		if (bRoom)
		{
			Chosen.Add(Cliff);
			if (Chosen.Num() == Lifts.Num())
			{
				break;
			}
		}
	}
	if (Chosen.IsEmpty())
	{
		if (FirstTime(*FString::Printf(TEXT("no room for lift:%d"), Cliffs.Num())))
		{
			UE_LOG(LogTerrainBelts, Display, TEXT("Auto-lift: no cliff with room for a lift (%d steep stretches, belt %.0f cm, minimum %.0f cm on each side), endpoints %s -> %s"),
				Cliffs.Num(), TotalLength, AutoLiftMinBeltLength, *Points[0].Location.ToString(), *Points.Last().Location.ToString());
			for (const FCliff& Cliff : Cliffs)
			{
				UE_LOG(LogTerrainBelts, Display, TEXT("    candidate: %s, rise %.0f cm, low point %d, room before %.0f cm / after %.0f cm"),
					Cliff.bUphill ? TEXT("uphill") : TEXT("downhill"), Cliff.Rise, Cliff.Low, Profile.Along[Cliff.Low], TotalLength - Profile.Along[Cliff.Low]);
			}
		}
		return false;
	}
	Chosen.Sort([](const FCliff& A, const FCliff& B) { return A.Low < B.Low; });

	// Place each lift at the foot of its cliff. Its input takes the belt the items arrive on: at
	// the foot where they go up, at the top where they go down. Entries and Exits are where the
	// belt meets the lift in drag order (arriving, then leaving). Against the flow, that is the
	// lift's output first.
	TArray<double> MinZ = Profile.MinZ;
	TArray<FVector> Entries;
	TArray<FVector> Exits;
	TArray<FVector> Directions;
	for (int32 LiftIndex = 0; LiftIndex < Chosen.Num(); ++LiftIndex)
	{
		const FCliff& Cliff = Chosen[LiftIndex];
		const int32 Low = Cliff.Low;
		const double LowZ = LiftFootHeight(Profile, Low, Cliff.bUphill);
		FCliff AdjustedCliff = Cliff;
		AdjustedCliff.Rise = FMath::Max(0.0, Cliff.Rise - (LowZ - Profile.MinZ[Low]));
		const double LiftHeight = LiftHeightFor(AdjustedCliff);

		// Belt direction at the foot of the cliff, kept horizontal where it meets the lift.
		FVector Direction = Points[Low].LeaveTangent.GetSafeNormal2D();
		if (Direction.IsNearlyZero())
		{
			Direction = (Points[Low + 1].Location - Points[Low - 1].Location).GetSafeNormal2D();
		}

		const bool bFlowsUp = Cliff.bUphill != bReversed;
		const FVector Input(FVector2D(Points[Low].Location), bFlowsUp ? LowZ : LowZ + LiftHeight);
		FVector Output;
		if (!PlaceLift(Lifts[LiftIndex], Input, bReversed ? -Direction : Direction, bFlowsUp ? LiftHeight : -LiftHeight, Output))
		{
			return false;
		}
		const FVector Entry = bReversed ? Output : Input;
		const FVector Exit = bReversed ? Input : Output;
		Entries.Add(Entry);
		Exits.Add(Exit);
		Directions.Add(Direction);

		// Over the cliff face, keep the belt level with the top of the lift instead of following
		// the face down and back up.
		const double TopZ = FMath::Max(Entry.Z, Exit.Z);
		for (int32 Index = Cliff.RunStart + 1; Index < Cliff.RunEnd; ++Index)
		{
			MinZ[Index] = FMath::Max(MinZ[Index], TopZ);
		}
	}

	// The belt pieces between them: start -> lift 0 -> lift 1 -> ... -> end.
	TArray<TArray<FSplinePointData>> Pieces;
	for (int32 PieceIndex = 0; PieceIndex <= Chosen.Num(); ++PieceIndex)
	{
		const bool bAfterLift = PieceIndex > 0;
		const bool bBeforeLift = PieceIndex < Chosen.Num();
		const int32 From = bAfterLift ? Chosen[PieceIndex - 1].Low : 0;
		const int32 To = bBeforeLift ? Chosen[PieceIndex].Low : Num - 1;
		if (To <= From)
		{
			return false;
		}

		TArray<FSplinePointData> Piece(Points.GetData() + From, To - From + 1);
		TArray<double> PieceMinZ(MinZ.GetData() + From, To - From + 1);
		if (bAfterLift)
		{
			const FVector& Direction = Directions[PieceIndex - 1];
			Piece[0] = FSplinePointData(Exits[PieceIndex - 1], Direction * FMath::Max(1.0, FVector2D(Piece[0].LeaveTangent).Size()));
		}
		if (bBeforeLift)
		{
			const FVector& Direction = Directions[PieceIndex];
			Piece.Last() = FSplinePointData(Entries[PieceIndex], Direction * FMath::Max(1.0, FVector2D(Piece.Last().ArriveTangent).Size()));
		}
		// A split already uses lifts for the height changes; do not add a dip below both
		// connectors between them. Ground above this floor still raises the belt normally.
		const double ConnectorFloor = FMath::Min(Piece[0].Location.Z, Piece.Last().Location.Z);
		for (int32 Index = 1; Index + 1 < PieceMinZ.Num(); ++Index)
		{
			PieceMinZ[Index] = FMath::Max(PieceMinZ[Index], ConnectorFloor);
		}
		if (!FinishDrape(Piece, PieceMinZ, Settings, SliceSamples(Profile.Samples, From, To - From)))
		{
			if (FirstTime(*FString::Printf(TEXT("auto-lift:rejected-piece:%d"), PieceIndex)))
			{
				UE_LOG(LogTerrainBelts, Display, TEXT("Auto-lift: rejected belt piece %d of %d (%d points), endpoints %s -> %s, reversed %s"),
					PieceIndex, Chosen.Num() + 1, Piece.Num(), *Piece[0].Location.ToString(), *Piece.Last().Location.ToString(), bReversed ? TEXT("yes") : TEXT("no"));
				for (int32 Index = 0; Index < Piece.Num(); ++Index)
				{
					UE_LOG(LogTerrainBelts, Display, TEXT("    point %d: %s, required Z %.0f"), Index, *Piece[Index].Location.ToString(), PieceMinZ[Index]);
				}
			}
			return false;
		}
		Pieces.Add(MoveTemp(Piece));
	}

	Belt->mSplineData = ToLocalPoints(Pieces[0], BeltTransform);
	Belt->UpdateSplineComponent();

	// Whatever the drag ends on belongs to the last belt piece. Vanilla may or may not re-snap
	// the main belt every frame, so collect the snap from wherever it is now.
	UFGFactoryConnectionComponent* HeldEndSnap = Belt->mSnappedConnectionComponents[1];
	Belt->mSnappedConnectionComponents[1] = nullptr;
	for (AFGConveyorBeltHologram* NextBelt : NextBelts)
	{
		for (TObjectPtr<UFGFactoryConnectionComponent>& Snap : NextBelt->mSnappedConnectionComponents)
		{
			if (HeldEndSnap == nullptr)
			{
				HeldEndSnap = Snap;
			}
			Snap = nullptr;
		}
	}

	for (int32 Index = 0; Index < Lifts.Num(); ++Index)
	{
		AFGConveyorBeltHologram* NextBelt = NextBelts[Index];
		if (Index >= Chosen.Num())
		{
			Lifts[Index]->SetDisabled(true);
			NextBelt->SetDisabled(true);
			ChildBeltStates.Remove(FObjectKey(NextBelt));
			continue;
		}

		// Our belts run from start to end in the flow direction. Against the flow they're built
		// from the far end back, and the drag's end snap goes on their start.
		TArray<FSplinePointData> Piece = Pieces[Index + 1];
		FVector PieceDirection = Directions[Index];
		if (bReversed)
		{
			Algo::Reverse(Piece);
			for (FSplinePointData& Point : Piece)
			{
				const FVector Arrive = Point.ArriveTangent;
				Point.ArriveTangent = -Point.LeaveTangent;
				Point.LeaveTangent = -Arrive;
			}
			PieceDirection = (Piece[1].Location - Piece[0].Location).GetSafeNormal2D();
			if (PieceDirection.IsNearlyZero())
			{
				PieceDirection = -Directions[Index];
			}
		}
		FChildBeltState& State = ChildBeltStates.FindOrAdd(FObjectKey(NextBelt));
		State.Transform = FTransform(PieceDirection.Rotation(), Piece[0].Location);
		State.SplineData = ToLocalPoints(Piece, State.Transform);
		NextBelt->SetActorTransform(State.Transform);
		NextBelt->mSplineData = State.SplineData;
		NextBelt->UpdateSplineComponent();
		if (Index == Chosen.Num() - 1)
		{
			NextBelt->mSnappedConnectionComponents[bReversed ? 0 : 1] = HeldEndSnap;
		}
		Lifts[Index]->SetDisabled(false);
		NextBelt->SetDisabled(false);
	}

	if (FirstTime(*FString::Printf(TEXT("lifts %d"), Chosen.Num())))
	{
		UE_LOG(LogTerrainBelts, Display, TEXT("Auto-lift: first split with %d lift(s); first %s cliff of %.0f cm, entry %s, exit %s"),
			Chosen.Num(), Chosen[0].bUphill ? TEXT("uphill") : TEXT("downhill"), Chosen[0].Rise, *Entries[0].ToString(), *Exits[0].ToString());
	}
	if (bReversed && FirstTime(TEXT("reversed split")))
	{
		UE_LOG(LogTerrainBelts, Display, TEXT("Auto-lift: belt dragged against the flow; lifts and belt pieces turned around"));
	}
	return true;
#else
	return false;
#endif
}

void FTerrainBeltsModule::DisableAutoLift(AFGConveyorBeltHologram* Belt)
{
	using namespace TerrainBelts;

	for (int32 Index = 0; Index < MaxAutoLifts; ++Index)
	{
		if (AFGConveyorBeltHologram* NextBelt = Cast<AFGConveyorBeltHologram>(Belt->FindChildHologramByName(BeltChildName(Index))))
		{
			// Hand the end snap back to the main belt (a reversed piece holds it on its start).
			for (TObjectPtr<UFGFactoryConnectionComponent>& Snap : NextBelt->mSnappedConnectionComponents)
			{
				if (Snap != nullptr)
				{
					if (Belt->mSnappedConnectionComponents[1] == nullptr)
					{
						Belt->mSnappedConnectionComponents[1] = Snap;
					}
					Snap = nullptr;
				}
			}
			if (!NextBelt->IsDisabled())
			{
				NextBelt->SetDisabled(true);
			}
			ChildBeltStates.Remove(FObjectKey(NextBelt));
		}
		if (AFGHologram* Lift = Belt->FindChildHologramByName(LiftChildName(Index)))
		{
			if (!Lift->IsDisabled())
			{
				Lift->SetDisabled(true);
			}
		}
	}
}

void FTerrainBeltsModule::RestoreChildBelt(AFGConveyorBeltHologram* ChildBelt)
{
	using namespace TerrainBelts;

	const FChildBeltState* State = ChildBeltStates.Find(FObjectKey(ChildBelt));
	if (State == nullptr || ChildBelt->IsDisabled())
	{
		return;
	}
	ChildBelt->SetActorTransform(State->Transform);
	ChildBelt->mSplineData = State->SplineData;
	ChildBelt->UpdateSplineComponent();
}

void FTerrainBeltsModule::ConnectBuiltPieces(const AFGHologram* Hologram, AFGBuildable* Buildable)
{
	using namespace TerrainBelts;

	const AFGHologram* MainBelt = nullptr;
	bool bIsLift = false;
	int32 Index = 0;
	const AFGHologram* ParentHologram = Hologram->GetParentHologram();
	if (ParentHologram == nullptr)
	{
		if (const TWeakObjectPtr<AFGConveyorBeltHologram>* Detached = DetachedSplitChildren.Find(FObjectKey(Hologram)))
		{
			ParentHologram = Detached->Get();
		}
	}
	if (const AFGConveyorBeltHologram* Parent = Cast<AFGConveyorBeltHologram>(ParentHologram))
	{
		if (!FindAutoLiftChild(Parent, Hologram, bIsLift, Index))
		{
			return;
		}
		MainBelt = Parent;
	}
	else if (const AFGConveyorBeltHologram* Belt = Cast<AFGConveyorBeltHologram>(Hologram))
	{
		if (NumActiveLifts(Belt) == 0)
		{
			return;
		}
		MainBelt = Belt;
	}
	else
	{
		return;
	}

	FBuiltSplit& Split = BuiltSplits.FindOrAdd(FObjectKey(MainBelt));
	if (Hologram == MainBelt)
	{
		Split.Belts[0] = Cast<AFGBuildableConveyorBelt>(Buildable);
	}
	else if (bIsLift)
	{
		Split.Lifts[Index] = Cast<AFGBuildableConveyorLift>(Buildable);
	}
	else
	{
		Split.Belts[Index + 1] = Cast<AFGBuildableConveyorBelt>(Buildable);
	}

	if (FirstTime(TEXT("configure")))
	{
		UE_LOG(LogTerrainBelts, Display, TEXT("Auto-lift: building split pieces (first: %s)"), *GetNameSafe(Buildable));
	}

	// The belt before each lift in drag order feeds its input, unless the belt was dragged against
	// the flow; then it's fed by the lift's output.
	const bool bReversed = IsDraggedAgainstFlow(Cast<AFGConveyorBeltHologram>(MainBelt));
	for (int32 LiftIndex = 0; LiftIndex < MaxAutoLifts; ++LiftIndex)
	{
		const TWeakObjectPtr<AFGBuildableConveyorLift>& Lift = Split.Lifts[LiftIndex];
		if (!Lift.IsValid())
		{
			continue;
		}
		if (Split.Belts[LiftIndex].IsValid())
		{
			ConnectBeltToLift(Split.Belts[LiftIndex].Get(), Lift.Get(), !bReversed);
		}
		if (Split.Belts[LiftIndex + 1].IsValid())
		{
			ConnectBeltToLift(Split.Belts[LiftIndex + 1].Get(), Lift.Get(), bReversed);
		}
	}

	// Done once every piece in use is built.
	const int32 ActiveLifts = NumActiveLifts(MainBelt);
	bool bAllBuilt = Split.Belts[0].IsValid();
	for (int32 LiftIndex = 0; LiftIndex < ActiveLifts; ++LiftIndex)
	{
		bAllBuilt &= Split.Lifts[LiftIndex].IsValid() && Split.Belts[LiftIndex + 1].IsValid();
	}
	if (bAllBuilt)
	{
		BuiltSplits.Remove(FObjectKey(MainBelt));
	}
}

void FTerrainBeltsModule::RemoveUnevenSurfaceDisqualifier(AFGBuildableHologram* Hologram)
{
	if (IsExemptFromUnevenFloor(Hologram))
	{
		Hologram->mConstructDisqualifiers.Remove(UFGCDInvalidFloor::StaticClass());
	}
}

bool FTerrainBeltsModule::IsExemptFromUnevenFloor(const AFGHologram* Hologram)
{
	if (!TerrainBelts::AllowUnevenSurfaces(Hologram))
	{
		return false;
	}
	// Restrict standalone supports as well as child supports; power poles and unrelated
	// buildings keep their normal floor checks.
	const UClass* BuildClass = Hologram->GetBuildClass();
	// Wall and ceiling supports use generic buildable classes and attachment holograms.
	// Match their actual asset classes (including derived mods), rather than exempting
	// every wall attachment or every object with a pipe/conveyor connection.
	bool bAttachmentSupport = false;
	for (const UClass* Class = BuildClass; Class != nullptr; Class = Class->GetSuperClass())
	{
		static const TSet<FName> AttachmentSupportClasses = {
			FName(TEXT("/Game/FactoryGame/Buildable/Factory/ConveyorPoleWall/Build_ConveyorPoleWall.Build_ConveyorPoleWall_C")),
			FName(TEXT("/Game/FactoryGame/Buildable/Factory/ConveyorPoleWall/Build_ConveyorCeilingAttachment.Build_ConveyorCeilingAttachment_C")),
			FName(TEXT("/Game/FactoryGame/Buildable/Factory/PipelineSupportWall/Build_PipelineSupportWall.Build_PipelineSupportWall_C")),
			FName(TEXT("/Game/FactoryGame/Buildable/Factory/HyperTubeWallSupport/Build_HyperTubeWallSupport.Build_HyperTubeWallSupport_C"))
		};
		if (AttachmentSupportClasses.Contains(FName(*Class->GetPathName())))
		{
			bAttachmentSupport = true;
			break;
		}
	}
	const bool bSplineOrSupport = Hologram->IsA<AFGConveyorBeltHologram>() ||
		Hologram->IsA<AFGConveyorLiftHologram>() || Hologram->IsA<AFGPipelineHologram>() ||
		Hologram->IsA<AFGRailroadTrackHologram>() || Hologram->IsA<AFGConveyorPoleHologram>() ||
		Hologram->IsA<AFGConveyorMultiPoleHologram>() || Hologram->IsA<AFGPipelinePoleHologram>() || bAttachmentSupport ||
		(BuildClass != nullptr && (BuildClass->IsChildOf(AFGBuildablePoleConveyor::StaticClass()) ||
			BuildClass->IsChildOf(AFGBuildablePoleStackable::StaticClass())));
	return bSplineOrSupport;
}

void FTerrainBeltsModule::RegisterHooks()
{
	using namespace TerrainBelts;

	// Filter after placement validation, before ValidatePlacementAndCost updates materials.
	// Some CheckValidFloor overrides (notably FactoryBuilding) are empty in Shipping and
	// too short for funchook to patch. Use the placement checks instead. Register this before
	// our pipe ground-clearance hook below so its terrain-intersection rejection is retained.
	const auto FilterUnevenFloor = [](AFGBuildableHologram* Hologram)
	{
		RemoveUnevenSurfaceDisqualifier(Hologram);
	};
	// Also refuse the disqualifier wherever it's added: some holograms (or their children) add it
	// after CheckValidPlacement, which the filter above never sees.
	SUBSCRIBE_METHOD(AFGHologram::AddConstructDisqualifier,
		[](auto& Scope, AFGHologram* Hologram, TSubclassOf<UFGConstructDisqualifier> Disqualifier)
		{
			if (Disqualifier == UFGCDInvalidFloor::StaticClass() && IsExemptFromUnevenFloor(Hologram))
			{
				if (FirstTime(*FString::Printf(TEXT("uneven:%s"), *GetNameSafe(Hologram->GetClass()))))
				{
					UE_LOG(LogTerrainBelts, Display, TEXT("Blocked surface-too-uneven on %s"), *GetNameSafe(Hologram->GetClass()));
				}
				Scope.Cancel();
			}
		});
	SUBSCRIBE_UOBJECT_METHOD_AFTER(AFGBuildableHologram, CheckValidPlacement, FilterUnevenFloor);
	SUBSCRIBE_UOBJECT_METHOD_AFTER(AFGFactoryBuildingHologram, CheckValidPlacement, FilterUnevenFloor);
	SUBSCRIBE_UOBJECT_METHOD_AFTER(AFGConveyorBeltHologram, CheckValidPlacement, FilterUnevenFloor);
	SUBSCRIBE_UOBJECT_METHOD_AFTER(AFGConveyorLiftHologram, CheckValidPlacement, FilterUnevenFloor);
	SUBSCRIBE_UOBJECT_METHOD_AFTER(AFGPipelineHologram, CheckValidPlacement, FilterUnevenFloor);
	SUBSCRIBE_UOBJECT_METHOD_AFTER(AFGRailroadTrackHologram, CheckValidPlacement, FilterUnevenFloor);

	// Like Construction Preferences, change the placement limit after vanilla initializes it.
	// Use the class default as the baseline so child holograms don't compound the multiplier.
	SUBSCRIBE_UOBJECT_METHOD_AFTER(AFGConveyorBeltHologram, BeginPlay,
		[](AFGConveyorBeltHologram* Hologram)
		{
			const float BaseLength = Hologram->GetClass()->GetDefaultObject<AFGConveyorBeltHologram>()->mMaxSplineLength;
			Hologram->mMaxSplineLength = BaseLength * GetLengthMultiplier(Hologram);
		});

	// Lift height is initialized from the buildable in BeginPlay, rather than on the CDO.
	// This includes the child lifts spawned for automatic cliff placement.
	SUBSCRIBE_UOBJECT_METHOD_AFTER(AFGConveyorLiftHologram, BeginPlay,
		[](AFGConveyorLiftHologram* Hologram)
		{
			Hologram->mMaximumHeight *= GetLengthMultiplier(Hologram);
		});

	// Hypertubes use the same hologram class as pipelines, with their own class defaults.
	SUBSCRIBE_UOBJECT_METHOD_AFTER(AFGPipelineHologram, BeginPlay,
		[](AFGPipelineHologram* Hologram)
		{
			const float BaseLength = Hologram->GetClass()->GetDefaultObject<AFGPipelineHologram>()->mMaxSplineLength;
			Hologram->mMaxSplineLength = BaseLength * GetLengthMultiplier(Hologram);
		});

	SUBSCRIBE_UOBJECT_METHOD_AFTER(AFGRailroadTrackHologram, BeginPlay,
		[](AFGRailroadTrackHologram* Hologram)
		{
			const float BaseLength = Hologram->GetClass()->GetDefaultObject<AFGRailroadTrackHologram>()->mMaxLength;
			Hologram->mMaxLength = BaseLength * GetLengthMultiplier(Hologram);
		});

	// Runs after vanilla GenerateAndUpdateSpline, and drapes whatever route the current build
	// mode produced.
	auto DrapeHologram = [](auto* Hologram, const TCHAR* Kind, const FDrapeSettings& Settings)
	{
		const bool bFollowTerrain = ShouldFollowTerrain(Hologram);
		if (FirstTime(*FString::Printf(TEXT("%s:%d"), Kind, bFollowTerrain)))
		{
			UE_LOG(LogTerrainBelts, Display, TEXT("%s GenerateAndUpdateSpline hook live (follow terrain: %s, %d points)"),
				Kind, bFollowTerrain ? TEXT("yes") : TEXT("no"), Hologram->mSplineData.Num());
		}
		if (!bFollowTerrain)
		{
			return;
		}

		const int32 VanillaPoints = Hologram->mSplineData.Num();
		if (!DrapeSpline(Hologram->mSplineData, Hologram, Settings))
		{
			return;
		}
		Hologram->UpdateSplineComponent();

		if (FirstTime(*FString::Printf(TEXT("%s:drape"), Kind)))
		{
			UE_LOG(LogTerrainBelts, Display, TEXT("First %s drape: %d vanilla points -> %d draped points, max incline %.1f deg"),
				Kind, VanillaPoints, Hologram->mSplineData.Num(), Settings.MaxInclineDegrees);
		}
	};

	// The Follow Terrain key and its hint, updated each tick for the local player.
	SUBSCRIBE_UOBJECT_METHOD_AFTER(AFGCharacterPlayer, Tick,
		[](AFGCharacterPlayer* Player, float DeltaSeconds)
		{
			if (Player->IsLocallyControlled())
			{
				HandleInput(Player);
				UpdateKeyHint(Player);
			}
		});

	SUBSCRIBE_METHOD_AFTER(AFGConveyorBeltHologram::GenerateAndUpdateSpline,
		[DrapeHologram](AFGConveyorBeltHologram* Hologram, const FHitResult& HitResult)
		{
			// Our second belt: put its shape back after its own placement re-routed it.
			if (Hologram->GetParentHologram() != nullptr)
			{
				RestoreChildBelt(Hologram);
				return;
			}

			// Parenthesised so the macro doesn't split its arguments at the braced commas.
			const FDrapeSettings Settings = (FDrapeSettings{
				.StepLength = BeltStepLength,
				.GroundClearance = BeltClearance,
				.MaxInclineDegrees = Hologram->mMaxIncline - BeltInclineMarginDegrees,
				.SmoothingPasses = BeltSmoothingPasses,
				.HalfWidth = BeltHalfWidth,
				.GroundSampleSpacing = BeltGroundSampleSpacing,
				.VerticalBendRadius = BeltVerticalBendRadius,
				.bKeepLevelOnFlatGround = true,
				.MaxKeptFloat = BeltMaxKeptFloat,
				.bEnforceIncline = true});
			if (ShouldFollowTerrain(Hologram) && ApplyAutoLift(Hologram, Settings))
			{
				return;
			}
			DisableAutoLift(Hologram);
			DrapeHologram(Hologram, TEXT("belt"), Settings);
		});

	SUBSCRIBE_METHOD_AFTER(AFGPipelineHologram::GenerateAndUpdateSpline,
		[DrapeHologram](AFGPipelineHologram* Hologram, const FHitResult& HitResult)
		{
			PipelineGroundFailures.Remove(FObjectKey(Hologram));
			const TSubclassOf<AActor> BuildClass = Hologram->GetBuildClass();
			const bool bHypertube = BuildClass != nullptr && BuildClass->IsChildOf(AFGBuildablePipeHyper::StaticClass());
			const TCHAR* Kind = bHypertube ? TEXT("hypertube") : TEXT("pipe");
			const double VanillaClearance = Hologram->GetHologramHoverHeight();
			const double GroundClearance = bHypertube ? HypertubeClearance : FMath::Max(PipeClearance, VanillaClearance);
			if (!bHypertube && FirstTime(TEXT("pipe:vanilla-clearance")))
			{
				UE_LOG(LogTerrainBelts, Display, TEXT("Pipe ground clearance %.0f cm (vanilla hover height %.0f cm)"), GroundClearance, VanillaClearance);
			}

			// Parenthesised so the macro doesn't split its arguments at the braced commas.
			const FDrapeSettings Settings = (FDrapeSettings{
				.StepLength = PipeStepLength,
				.GroundClearance = GroundClearance,
				.MaxInclineDegrees = PipeMaxInclineDegrees,
				.SmoothingPasses = PipeSmoothingPasses,
				.HalfWidth = bHypertube ? HypertubeHalfWidth : PipeHalfWidth,
				.GroundSampleSpacing = PipeGroundSampleSpacing,
				.VerticalBendRadius = bHypertube ? HypertubeVerticalBendRadius : PipeVerticalBendRadius,
				.bSmoothRoute = true,
				.MinBendRadius = PipelineBendMargin * Hologram->mMinBendRadius,
				.bKeepLevelOnFlatGround = true});

			if (!ShouldFollowTerrain(Hologram))
			{
				DrapeHologram(Hologram, Kind, Settings); // only logs that the hook is live
				return;
			}

			// Vertical ends keep a bend; in between, big steep slopes get a vertical step and
			// everything else is draped. Each shape is checked with the pipe's own curvature test;
			// one that fails falls back to a simpler shape, and finally to vanilla's, so Follow
			// Terrain never leaves a pipe invalid that vanilla could have placed.
			const double BendRadius = FMath::Max(PipeRiserBendRadius, Settings.MinBendRadius);
			const TArray<FSplinePointData> Vanilla = Hologram->mSplineData;

			// A free end (snapped to nothing) that isn't vertical leaves level, like vanilla's own
			// horizontal-to-vertical mode. Vanilla's default route tilts it towards the other end
			// (steeply down from a ceiling), which no longer fits once the pipe runs along the
			// ground: the last metre had to swing into the tilt and failed the curvature check.
			TArray<FSplinePointData> Leveled = Vanilla;
			if (Leveled.Num() >= 2)
			{
				auto Level = [](FVector& Tangent)
				{
					const double Horizontal = FVector2D(Tangent).Size();
					if (Horizontal > 0.3 * Tangent.Size())
					{
						Tangent = FVector(FVector2D(Tangent).GetSafeNormal() * Tangent.Size(), 0.0);
					}
				};
				if (Hologram->mSnappedConnectionComponents[0] == nullptr)
				{
					Level(Leveled[0].LeaveTangent);
				}
				if (Hologram->mSnappedConnectionComponents[1] == nullptr)
				{
					Level(Leveled.Last().ArriveTangent);
				}
			}
			// The plain drape resamples the whole spline into steps and smooths it, which smears a
			// vertical end's tight elbow into a wide swoop that bulges out under the riser. A pipe
			// with a vertical end that neither vertical-end shape fits keeps vanilla's spline.
			bool bHasVerticalEnd = false;
			if (Leveled.Num() >= 2)
			{
				const FTransform& ToWorld = Hologram->GetActorTransform();
				auto IsVertical = [&ToWorld](const FVector& Tangent) { return FMath::Abs(ToWorld.TransformVector(Tangent).GetSafeNormal().Z) > 0.7; };
				bHasVerticalEnd = IsVertical(Leveled[0].LeaveTangent) || IsVertical(Leveled.Last().ArriveTangent);
			}
			constexpr int32 AttemptStep = 0; // then 1: drape between vertical ends
			constexpr int32 AttemptPlainDrape = 2;
			auto Shape = [&](int32 Attempt) -> bool
			{
				Hologram->mSplineData = Leveled;
				bool bShaped;
				if (Attempt == AttemptPlainDrape && bHasVerticalEnd)
				{
					if (FirstTime(*FString::Printf(TEXT("%s:no plain drape"), Kind)))
					{
						UE_LOG(LogTerrainBelts, Display, TEXT("%s with a vertical end fits no vertical-end shape; keeping vanilla's spline"), Kind);
					}
					return false;
				}
				if (Attempt == AttemptPlainDrape)
				{
					bShaped = DrapeSpline(Hologram->mSplineData, Hologram, Settings);
				}
				else
				{
					bShaped = ShapeWithVerticalEnds(Hologram->mSplineData, Hologram, BendRadius, Settings,
						[Hologram, &Settings, Attempt](TArray<FSplinePointData>& Data)
						{
							return (Attempt == AttemptStep && StepPipeSpline(Data, Hologram, Settings)) || DrapeSpline(Data, Hologram, Settings);
						});
				}
				if (!bShaped)
				{
					return false;
				}
				CapTangentsToChords(Hologram->mSplineData);
				Hologram->UpdateSplineComponent();
				const float Fault = Hologram->ValidateCurvatureAndReturnFaultyPosition();
				static int32 FaultsLogged = 0;
				static int32 LastDumpedPoints = 0;
				if (Fault > 0.0f && FaultsLogged++ < 60)
				{
					UE_LOG(LogTerrainBelts, Display, TEXT("%s shape %d bends too tightly %.0f cm along (%d points, at %s); trying a simpler shape"),
						Kind, Attempt, Fault, Hologram->mSplineData.Num(),
						*Hologram->mSplineComponent->GetLocationAtDistanceAlongSpline(Fault, ESplineCoordinateSpace::World).ToString());
					// Dump each differently shaped failure (not just the first few, which may all be a
					// short pipe while aiming).
					if (Hologram->mSplineData.Num() != LastDumpedPoints)
					{
						LastDumpedPoints = Hologram->mSplineData.Num();
						const FTransform& ToWorld = Hologram->GetActorTransform();
						for (const FSplinePointData& Point : Hologram->mSplineData)
						{
							UE_LOG(LogTerrainBelts, Display, TEXT("    point %s arrive %s leave %s"), *ToWorld.TransformPosition(Point.Location).ToString(),
								*ToWorld.TransformVector(Point.ArriveTangent).ToString(), *ToWorld.TransformVector(Point.LeaveTangent).ToString());
						}
					}
				}
				if (Fault > 0.0f)
				{
					return false;
				}
				if (!HasPipelineGroundClearance(Hologram->mSplineData, Hologram, Settings))
				{
					if (FirstTime(*FString::Printf(TEXT("%s:clearance:%d"), Kind, Attempt)))
					{
						UE_LOG(LogTerrainBelts, Display, TEXT("%s shape %d hits the ground or a building; trying a simpler shape"), Kind, Attempt);
					}
					return false;
				}
				return true;
			};
			for (int32 Attempt = AttemptStep; Attempt <= AttemptPlainDrape; ++Attempt)
			{
				if (Shape(Attempt))
				{
					if (FirstTime(*FString::Printf(TEXT("%s:shape:%d"), Kind, Attempt)))
					{
						UE_LOG(LogTerrainBelts, Display, TEXT("First %s with shape %d (0 step, 1 drape, 2 plain drape): %d points, bend radius %.0f"),
							Kind, Attempt, Hologram->mSplineData.Num(), BendRadius);
					}
					return;
				}
			}
			Hologram->mSplineData = Vanilla;
			Hologram->UpdateSplineComponent();
			if (!HasPipelineGroundClearance(Vanilla, Hologram, Settings, true))
			{
				if (PipelineGroundFailures.Num() >= MaxProfileCacheHolograms)
				{
					PipelineGroundFailures.Reset();
				}
				PipelineGroundFailures.Add(FObjectKey(Hologram));
			}
		});

	SUBSCRIBE_UOBJECT_METHOD_AFTER(AFGPipelineHologram, CheckValidPlacement,
		[](AFGPipelineHologram* Hologram)
		{
			if (ShouldFollowTerrain(Hologram) && PipelineGroundFailures.Contains(FObjectKey(Hologram)))
			{
				Hologram->mConstructDisqualifiers.AddUnique(UFGCDInvalidFloor::StaticClass());
			}
		});

	// Rails: put vanilla's own spline back first, so vanilla never runs on a draped one.
	SUBSCRIBE_METHOD(AFGRailroadTrackHologram::GenerateAndUpdateSpline,
		[](auto& Scope, AFGRailroadTrackHologram* Hologram, const FHitResult& HitResult)
		{
			const FObjectKey Key(Hologram);
			const FRailSplines* Splines = RailSplines.Find(Key);
			if (Splines == nullptr)
			{
				return;
			}
			if (SameSplineData(Hologram->mSplineData, Splines->Draped))
			{
				Hologram->mSplineData = Splines->Vanilla;
			}
			else
			{
				// Something else (a build, a reset) replaced the spline; trust it.
				if (FirstTime(TEXT("rail:replaced")))
				{
					UE_LOG(LogTerrainBelts, Display, TEXT("Rail spline changed outside the drape (%d points); not restoring"), Hologram->mSplineData.Num());
				}
				RailSplines.Remove(Key);
			}
		});

	SUBSCRIBE_METHOD_AFTER(AFGRailroadTrackHologram::GenerateAndUpdateSpline,
		[DrapeHologram](AFGRailroadTrackHologram* Hologram, const FHitResult& HitResult)
		{
			const FObjectKey Key(Hologram);
			if (!ShouldFollowTerrain(Hologram))
			{
				RailSplines.Remove(Key);
				DrapeHologram(Hologram, TEXT("rail"), (FDrapeSettings{.StepLength = RailStepLength, .GroundClearance = RailClearance, .MaxInclineDegrees = Hologram->mMaxGrade - RailGradeMarginDegrees, .SmoothingPasses = RailSmoothingPasses, .bOnlyRaise = true}));
				return;
			}

			if (RailSplines.Num() >= MaxRailSplinesBeforePrune)
			{
				for (auto It = RailSplines.CreateIterator(); It; ++It)
				{
					if (It.Key().ResolveObjectPtr() == nullptr)
					{
						It.RemoveCurrent();
					}
				}
			}

			FRailSplines& Splines = RailSplines.FindOrAdd(Key);
			Splines.Vanilla = Hologram->mSplineData;
			DrapeHologram(Hologram, TEXT("rail"), (FDrapeSettings{.StepLength = RailStepLength, .GroundClearance = RailClearance, .MaxInclineDegrees = Hologram->mMaxGrade - RailGradeMarginDegrees, .SmoothingPasses = RailSmoothingPasses, .bOnlyRaise = true}));
			Splines.Draped = Hologram->mSplineData;

			// One-off check that the point count stays put from frame to frame.
			static int32 LoggedFrames = 0;
			if (LoggedFrames < 5)
			{
				++LoggedFrames;
				UE_LOG(LogTerrainBelts, Display, TEXT("Rail drape: %d vanilla points -> %d draped points"), Splines.Vanilla.Num(), Splines.Draped.Num());
			}
		});

#if TERRAINBELTS_ENABLE_AUTO_LIFT
	SUBSCRIBE_UOBJECT_METHOD_AFTER(AFGConveyorBeltHologram, SpawnChildren,
		[](AFGConveyorBeltHologram* Hologram, AActor* HologramOwner, FVector SpawnLocation, APawn* HologramInstigator)
		{
			SpawnAutoLiftChildren(Hologram, HologramOwner, SpawnLocation);
		});

	SUBSCRIBE_UOBJECT_METHOD_AFTER(AFGConveyorBeltHologram, ConfigureComponents,
		[](const AFGConveyorBeltHologram* Hologram, AFGBuildable* Buildable)
		{
			ConnectBuiltPieces(Hologram, Buildable);
		});

	SUBSCRIBE_UOBJECT_METHOD_AFTER(AFGConveyorLiftHologram, ConfigureComponents,
		[](const AFGConveyorLiftHologram* Hologram, AFGBuildable* Buildable)
		{
			ConnectBuiltPieces(Hologram, Buildable);
		});

	// The main belt constructs its children inside its own Construct. Detach our split pieces for
	// that call so Construction Preferences doesn't take them for poles (see DetachedSplitChildren).
	// It reads the parent before the child builds, so this works whichever hook runs first.
	// Parenthesized: the braced list below has a comma the macro would split on.
	SUBSCRIBE_UOBJECT_METHOD(AFGBuildableHologram, Construct,
		([](auto& Scope, AFGBuildableHologram* Hologram, TArray<AActor*>& OutChildren, FNetConstructionID ConstructionID)
		{
			AFGConveyorBeltHologram* Belt = Cast<AFGConveyorBeltHologram>(Hologram);
			if (Belt == nullptr || Belt->GetParentHologram() != nullptr || NumActiveLifts(Belt) == 0)
			{
				return;
			}

			TArray<AFGHologram*, TInlineAllocator<2 * MaxAutoLifts>> Detached;
			for (int32 Index = 0; Index < MaxAutoLifts; ++Index)
			{
				for (const FName Name : {LiftChildName(Index), BeltChildName(Index)})
				{
					AFGHologram* Child = Belt->FindChildHologramByName(Name);
					if (Child != nullptr && !Child->IsDisabled() && Child->mParent == Belt)
					{
						Child->mParent = nullptr;
						DetachedSplitChildren.Add(FObjectKey(Child), Belt);
						Detached.Add(Child);
					}
				}
			}

			Scope(Hologram, OutChildren, ConstructionID);

			for (AFGHologram* Child : Detached)
			{
				DetachedSplitChildren.Remove(FObjectKey(Child));
				if (IsValid(Child))
				{
					Child->mParent = Belt;
				}
			}
			if (FirstTime(TEXT("detach split")))
			{
				UE_LOG(LogTerrainBelts, Display, TEXT("Auto-lift: built %d split piece(s) detached from %s"), Detached.Num(), *GetNameSafe(Belt));
			}
		}));

	// Our lift stands at the foot of a cliff wherever the ground is; vanilla's floor check would
	// call it "too uneven".
	SUBSCRIBE_UOBJECT_METHOD(AFGConveyorLiftHologram, CheckValidFloor,
		[](auto& Scope, AFGConveyorLiftHologram* Hologram)
		{
			if (IsAutoLiftPiece(Hologram))
			{
				Scope.Cancel();
				if (FirstTime(TEXT("skip lift floor")))
				{
					UE_LOG(LogTerrainBelts, Display, TEXT("Auto-lift: skipping the floor check for %s"), *GetNameSafe(Hologram));
				}
			}
		});

	SUBSCRIBE_UOBJECT_METHOD_AFTER(AFGConveyorLiftHologram, CheckValidPlacement,
		[](AFGConveyorLiftHologram* Hologram)
		{
			if (IsAutoLiftPiece(Hologram))
			{
				Hologram->mConstructDisqualifiers.Remove(UFGCDInvalidFloor::StaticClass());
			}
		});

	// The split pieces end at each other rather than at poles, which vanilla belts insist on.
	SUBSCRIBE_UOBJECT_METHOD_AFTER(AFGConveyorBeltHologram, CheckValidPlacement,
		[](AFGConveyorBeltHologram* Hologram)
		{
			if (IsAutoLiftPiece(Hologram) && Hologram->mConstructDisqualifiers.Remove(UFGCDBeltMustSnap::StaticClass()) > 0 && FirstTime(TEXT("skip must snap")))
			{
				UE_LOG(LogTerrainBelts, Display, TEXT("Auto-lift: dropped the must-snap rule for %s"), *GetNameSafe(Hologram));
			}
		});
#endif

	UE_LOG(LogTerrainBelts, Display, TEXT("Hooks registered"));
}

void FTerrainBeltsModule::StartupModule()
{
#if !WITH_EDITOR
	// FactoryGame's editor build only has link stubs, too short for funchook to patch,
	// so hooking there crashes the editor (and Alpakit's cook). Hooks only run in game.
	TerrainBelts::RegisterChildInputContexts();
	RegisterHooks();
#endif
}

void FTerrainBeltsModule::ShutdownModule()
{
}

IMPLEMENT_MODULE(FTerrainBeltsModule, TerrainBelts)
