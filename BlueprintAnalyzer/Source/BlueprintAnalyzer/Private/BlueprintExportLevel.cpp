// BlueprintExportLevel.cpp
// Read-only JSON export of level packages: World Composition tile table,
// per-level actor dumps, landscape height/weight sampling.
//
// Loading model: LoadPackage + UWorld::FindWorldInPackage. No world
// initialization, no actor registration, no streaming. Everything emitted is
// serialized data; transforms come from USceneComponent::UpdateComponentToWorld
// (safe on unregistered components) and bounds from CalcBounds.
//
// World Composition: UWorldComposition::PostInitProperties runs Rescan() in
// the editor, which reads every tile's FWorldTileInfo from its package
// summary (FWorldTileInfo::Read) — tiles are never fully loaded. Tile actors
// are stored tile-local; world_* fields add the tile's absolute offset
// (own Position + parent-chain Positions, read the same way).
//
// Landscape: ULandscapeInfo is NOT usable here (it only indexes registered
// components), so proxies are collected from ULevel::Actors and each
// component's heightmap/weightmap texture SOURCE is read through
// FLandscapeComponentDataInterface (editor-only, on disk, per-component
// mip lock cached for the duration of one request).
//
// Unity-build note: unique Level_ prefix on file-local helpers.

#include "BlueprintExportCommandlet.h"

#include "Engine/World.h"
#include "Engine/Level.h"
#include "Engine/LevelStreaming.h"
#include "Engine/WorldComposition.h"
#include "Engine/StaticMesh.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Misc/WorldCompositionUtility.h"
#include "Misc/PackageName.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Volume.h"
#include "GameFramework/WorldSettings.h"
#include "Components/SceneComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/ChildActorComponent.h"
#include "Components/SplineComponent.h"
#include "Components/BrushComponent.h"
#include "Materials/MaterialInterface.h"
#include "LandscapeProxy.h"
#include "Landscape.h"
#include "LandscapeComponent.h"
#include "LandscapeLayerInfoObject.h"
#include "LandscapeDataAccess.h"
#include "InstancedFoliageActor.h"
#include "InstancedFoliage.h"
#include "FoliageType.h"
#include "FoliageType_InstancedStaticMesh.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "PackageTools.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	// ------------------------------------------------------------------
	// Shared JSON / loading helpers
	// ------------------------------------------------------------------

	TSharedPtr<FJsonObject> Level_MakeError(const FString& Message)
	{
		TSharedPtr<FJsonObject> Out = MakeShareable(new FJsonObject);
		Out->SetBoolField(TEXT("success"), false);
		Out->SetStringField(TEXT("error"), Message);
		return Out;
	}

	TSharedPtr<FJsonObject> Level_Vec(const FVector& V)
	{
		TSharedPtr<FJsonObject> O = MakeShareable(new FJsonObject);
		O->SetNumberField(TEXT("x"), V.X);
		O->SetNumberField(TEXT("y"), V.Y);
		O->SetNumberField(TEXT("z"), V.Z);
		return O;
	}

	TSharedPtr<FJsonObject> Level_IntVec(const FIntVector& V)
	{
		TSharedPtr<FJsonObject> O = MakeShareable(new FJsonObject);
		O->SetNumberField(TEXT("x"), V.X);
		O->SetNumberField(TEXT("y"), V.Y);
		O->SetNumberField(TEXT("z"), V.Z);
		return O;
	}

	TSharedPtr<FJsonObject> Level_IntPoint(const FIntPoint& P)
	{
		TSharedPtr<FJsonObject> O = MakeShareable(new FJsonObject);
		O->SetNumberField(TEXT("x"), P.X);
		O->SetNumberField(TEXT("y"), P.Y);
		return O;
	}

	TSharedPtr<FJsonObject> Level_Rot(const FRotator& R)
	{
		TSharedPtr<FJsonObject> O = MakeShareable(new FJsonObject);
		O->SetNumberField(TEXT("pitch"), R.Pitch);
		O->SetNumberField(TEXT("yaw"), R.Yaw);
		O->SetNumberField(TEXT("roll"), R.Roll);
		return O;
	}

	// null when the box is invalid (no geometry).
	TSharedPtr<FJsonValue> Level_Box(const FBox& B)
	{
		if (!B.IsValid)
		{
			return MakeShareable(new FJsonValueNull());
		}
		TSharedPtr<FJsonObject> O = MakeShareable(new FJsonObject);
		O->SetObjectField(TEXT("min"), Level_Vec(B.Min));
		O->SetObjectField(TEXT("max"), Level_Vec(B.Max));
		O->SetObjectField(TEXT("size"), Level_Vec(B.GetSize()));
		return MakeShareable(new FJsonValueObject(O));
	}

	TSharedPtr<FJsonValue> Level_PathOrNull(const UObject* O)
	{
		if (O)
		{
			return MakeShareable(new FJsonValueString(O->GetPathName()));
		}
		return MakeShareable(new FJsonValueNull());
	}

	TSharedPtr<FJsonValue> Level_StringOrNull(const FString& S)
	{
		if (S.IsEmpty() || S == TEXT("None"))
		{
			return MakeShareable(new FJsonValueNull());
		}
		return MakeShareable(new FJsonValueString(S));
	}

	void Level_SetTransform(const TSharedPtr<FJsonObject>& O, const FTransform& T, const TCHAR* Prefix)
	{
		O->SetObjectField(FString(Prefix) + TEXT("location"), Level_Vec(T.GetLocation()));
		O->SetObjectField(FString(Prefix) + TEXT("rotation"), Level_Rot(T.Rotator()));
		O->SetObjectField(FString(Prefix) + TEXT("scale"), Level_Vec(T.GetScale3D()));
	}

	TSharedPtr<FJsonObject> Level_LayerJson(const FWorldTileLayer& L)
	{
		TSharedPtr<FJsonObject> O = MakeShareable(new FJsonObject);
		O->SetStringField(TEXT("name"), L.Name);
		O->SetNumberField(TEXT("streaming_distance"), L.StreamingDistance);
		O->SetBoolField(TEXT("distance_streaming_enabled"), L.DistanceStreamingEnabled);
		return O;
	}

	// Load a map package and return its UWorld (nullptr + OutError on failure).
	UWorld* Level_LoadWorld(const FString& Path, FString& OutError)
	{
		FString PackageName = Path;
		// Accept "/Game/Maps/Foo.Foo" object paths too.
		int32 DotIdx;
		if (PackageName.FindChar(TEXT('.'), DotIdx))
		{
			PackageName = PackageName.Left(DotIdx);
		}
		if (!FPackageName::IsValidLongPackageName(PackageName))
		{
			OutError = FString::Printf(TEXT("Not a valid long package name: %s"), *Path);
			return nullptr;
		}
		if (!FPackageName::DoesPackageExist(PackageName))
		{
			OutError = FString::Printf(TEXT("Package not found: %s"), *PackageName);
			return nullptr;
		}
		UPackage* Package = LoadPackage(nullptr, *PackageName, LOAD_None);
		if (!Package)
		{
			OutError = FString::Printf(TEXT("LoadPackage failed: %s"), *PackageName);
			return nullptr;
		}
		UWorld* World = UWorld::FindWorldInPackage(Package);
		if (!World)
		{
			OutError = FString::Printf(TEXT("No UWorld in package (not a map?): %s"), *PackageName);
			return nullptr;
		}
		return World;
	}

	// Unload a map package so batch walks don't accumulate worlds.
	void Level_UnloadWorld(UWorld* World)
	{
		if (!World)
		{
			return;
		}
		TArray<UPackage*> Packages;
		Packages.Add(World->GetOutermost());
		FText Err;
		UPackageTools::UnloadPackages(Packages, Err);
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	}

	// This package's own tile info + parent-chain walk for the absolute
	// offset. bFound=false for non-composition levels.
	struct FLevel_TileCtx
	{
		bool bFound = false;
		FWorldTileInfo Info;
		FIntVector AbsolutePosition = FIntVector::ZeroValue;
	};

	FLevel_TileCtx Level_ReadTileCtx(const FString& PackageName)
	{
		FLevel_TileCtx Ctx;
		FString Filename;
		if (!FPackageName::DoesPackageExist(PackageName, nullptr, &Filename))
		{
			return Ctx;
		}
		if (!FWorldTileInfo::Read(Filename, Ctx.Info))
		{
			return Ctx;
		}
		Ctx.bFound = true;
		Ctx.AbsolutePosition = Ctx.Info.Position;
		// Walk parents (bounded: a chain deeper than 16 is a data bug).
		FString Parent = Ctx.Info.ParentTilePackageName;
		for (int32 Depth = 0; Depth < 16 && !Parent.IsEmpty() && Parent != TEXT("None"); ++Depth)
		{
			FString ParentFile;
			FWorldTileInfo ParentInfo;
			if (!FPackageName::DoesPackageExist(Parent, nullptr, &ParentFile) || !FWorldTileInfo::Read(ParentFile, ParentInfo))
			{
				break;
			}
			Ctx.AbsolutePosition += ParentInfo.Position;
			Parent = ParentInfo.ParentTilePackageName;
		}
		return Ctx;
	}

	TSharedPtr<FJsonObject> Level_TileCtxJson(const FLevel_TileCtx& Ctx)
	{
		TSharedPtr<FJsonObject> O = MakeShareable(new FJsonObject);
		O->SetObjectField(TEXT("position"), Level_IntVec(Ctx.Info.Position));
		O->SetObjectField(TEXT("absolute_position"), Level_IntVec(Ctx.AbsolutePosition));
		O->SetField(TEXT("bounds"), Level_Box(Ctx.Info.Bounds));
		O->SetObjectField(TEXT("layer"), Level_LayerJson(Ctx.Info.Layer));
		O->SetField(TEXT("parent_package"), Level_StringOrNull(Ctx.Info.ParentTilePackageName));
		O->SetNumberField(TEXT("z_order"), Ctx.Info.ZOrder);
		return O;
	}

	// ------------------------------------------------------------------
	// Actor helpers
	// ------------------------------------------------------------------

	// Nearest native ancestor + BP generated-class chain (most-derived first).
	void Level_ClassChain(const UClass* Cls, FString& OutNative, TArray<FString>& OutBPChain)
	{
		for (const UClass* C = Cls; C; C = C->GetSuperClass())
		{
			if (C->HasAnyClassFlags(CLASS_Native))
			{
				OutNative = C->GetName();
				return;
			}
			OutBPChain.Add(C->GetPathName());
		}
	}

	bool Level_MatchesFilter(const AActor* A, const TArray<FString>& Filters)
	{
		if (Filters.Num() == 0)
		{
			return true;
		}
		for (const UClass* C = A->GetClass(); C; C = C->GetSuperClass())
		{
			const FString N = C->GetName();
			for (const FString& F : Filters)
			{
				// Match "Foo", "Foo_C", or "/Game/.../Foo.Foo_C".
				if (N == F || N == F + TEXT("_C") || C->GetPathName() == F)
				{
					return true;
				}
			}
		}
		return false;
	}

	// Make ComponentToWorld valid on an unregistered component tree.
	// UpdateComponentToWorld walks attach parents itself, so order only
	// affects cost, not correctness.
	void Level_RefreshTransforms(AActor* A)
	{
		TArray<USceneComponent*> Comps;
		A->GetComponents<USceneComponent>(Comps);
		for (USceneComponent* C : Comps)
		{
			C->UpdateComponentToWorld();
		}
	}

	// CalcBounds is virtual and needs no registration for the classes we
	// care about (StaticMesh/ISM/HISM/Brush/Landscape).
	FBox Level_ComponentBounds(USceneComponent* C)
	{
		return C->CalcBounds(C->GetComponentTransform()).GetBox();
	}

	FBox Level_ActorBounds(AActor* A)
	{
		FBox Box(ForceInit);
		TArray<UPrimitiveComponent*> Prims;
		A->GetComponents<UPrimitiveComponent>(Prims);
		for (UPrimitiveComponent* P : Prims)
		{
			if (P->IsEditorOnly())
			{
				continue;
			}
			const FBox B = Level_ComponentBounds(P);
			if (B.IsValid)
			{
				Box += B;
			}
		}
		return Box;
	}

	FBox Level_Offset(const FBox& B, const FVector& O)
	{
		return B.IsValid ? FBox(B.Min + O, B.Max + O) : B;
	}

	TSharedPtr<FJsonObject> Level_ComponentJson(USceneComponent* C, bool bInstances)
	{
		TSharedPtr<FJsonObject> O = MakeShareable(new FJsonObject);
		O->SetStringField(TEXT("name"), C->GetName());
		O->SetStringField(TEXT("class"), C->GetClass()->GetName());
		Level_SetTransform(O, C->GetRelativeTransform(), TEXT("relative_"));
		if (USceneComponent* Parent = C->GetAttachParent())
		{
			O->SetStringField(TEXT("attach_parent"), Parent->GetName());
		}

		if (UStaticMeshComponent* SMC = Cast<UStaticMeshComponent>(C))
		{
			O->SetField(TEXT("static_mesh"), Level_PathOrNull(SMC->GetStaticMesh()));
			TArray<TSharedPtr<FJsonValue>> Mats;
			for (int32 i = 0; i < SMC->GetNumMaterials(); ++i)
			{
				Mats.Add(Level_PathOrNull(SMC->GetMaterial(i)));
			}
			O->SetArrayField(TEXT("materials"), Mats);
			O->SetField(TEXT("bounds"), Level_Box(Level_ComponentBounds(SMC)));
			if (UInstancedStaticMeshComponent* ISM = Cast<UInstancedStaticMeshComponent>(C))
			{
				O->SetNumberField(TEXT("instance_count"), ISM->GetInstanceCount());
				if (bInstances)
				{
					TArray<TSharedPtr<FJsonValue>> Inst;
					for (int32 i = 0; i < ISM->GetInstanceCount(); ++i)
					{
						FTransform T;
						if (ISM->GetInstanceTransform(i, T, /*bWorldSpace*/true))
						{
							TSharedPtr<FJsonObject> IJ = MakeShareable(new FJsonObject);
							Level_SetTransform(IJ, T, TEXT(""));
							Inst.Add(MakeShareable(new FJsonValueObject(IJ)));
						}
					}
					O->SetArrayField(TEXT("instances"), Inst);
				}
			}
		}
		else if (UChildActorComponent* CAC = Cast<UChildActorComponent>(C))
		{
			O->SetField(TEXT("child_actor_class"), Level_PathOrNull(CAC->GetChildActorClass().Get()));
			if (AActor* Tmpl = CAC->GetChildActorTemplate())
			{
				O->SetStringField(TEXT("child_actor_template"), Tmpl->GetName());
			}
		}
		else if (USplineComponent* Spline = Cast<USplineComponent>(C))
		{
			O->SetBoolField(TEXT("closed_loop"), Spline->IsClosedLoop());
			TArray<TSharedPtr<FJsonValue>> Pts;
			const int32 N = Spline->GetNumberOfSplinePoints();
			for (int32 i = 0; i < N; ++i)
			{
				TSharedPtr<FJsonObject> PJ = MakeShareable(new FJsonObject);
				PJ->SetObjectField(TEXT("location"), Level_Vec(Spline->GetLocationAtSplinePoint(i, ESplineCoordinateSpace::World)));
				PJ->SetObjectField(TEXT("arrive_tangent"), Level_Vec(Spline->GetArriveTangentAtSplinePoint(i, ESplineCoordinateSpace::World)));
				PJ->SetObjectField(TEXT("leave_tangent"), Level_Vec(Spline->GetLeaveTangentAtSplinePoint(i, ESplineCoordinateSpace::World)));
				Pts.Add(MakeShareable(new FJsonValueObject(PJ)));
			}
			O->SetArrayField(TEXT("points"), Pts);
		}
		else if (ULandscapeComponent* LC = Cast<ULandscapeComponent>(C))
		{
			O->SetObjectField(TEXT("section_base"), Level_IntPoint(LC->GetSectionBase()));
			O->SetField(TEXT("bounds"), Level_Box(Level_ComponentBounds(LC)));
		}
		else if (UPrimitiveComponent* Prim = Cast<UPrimitiveComponent>(C))
		{
			// Brush, shape, skeletal-mesh, etc.
			O->SetField(TEXT("bounds"), Level_Box(Level_ComponentBounds(Prim)));
		}
		return O;
	}

	TSharedPtr<FJsonObject> Level_ActorJson(AActor* A, const FVector& WorldOffset, bool bBoundsOnly, bool bInstances, bool bListLandscapeComponents)
	{
		Level_RefreshTransforms(A);
		TSharedPtr<FJsonObject> O = MakeShareable(new FJsonObject);
		O->SetStringField(TEXT("name"), A->GetName());
		O->SetStringField(TEXT("label"), A->GetActorLabel());
		O->SetStringField(TEXT("class"), A->GetClass()->GetName());
		O->SetStringField(TEXT("class_path"), A->GetClass()->GetPathName());
		FString Native;
		TArray<FString> Chain;
		Level_ClassChain(A->GetClass(), Native, Chain);
		O->SetStringField(TEXT("native_class"), Native);
		TArray<TSharedPtr<FJsonValue>> ChainArr;
		for (const FString& S : Chain)
		{
			ChainArr.Add(MakeShareable(new FJsonValueString(S)));
		}
		O->SetArrayField(TEXT("blueprint_chain"), ChainArr);
		O->SetStringField(TEXT("folder"), A->GetFolderPath().IsNone() ? FString() : A->GetFolderPath().ToString());
		TArray<TSharedPtr<FJsonValue>> Tags;
		for (const FName& T : A->Tags)
		{
			Tags.Add(MakeShareable(new FJsonValueString(T.ToString())));
		}
		O->SetArrayField(TEXT("tags"), Tags);

		const FTransform T = A->GetActorTransform();
		Level_SetTransform(O, T, TEXT(""));
		O->SetObjectField(TEXT("world_location"), Level_Vec(T.GetLocation() + WorldOffset));
		const FBox Bounds = Level_ActorBounds(A);
		O->SetField(TEXT("bounds"), Level_Box(Bounds));
		O->SetField(TEXT("world_bounds"), Level_Box(Level_Offset(Bounds, WorldOffset)));

		// Type-specific summaries.
		if (ALandscapeProxy* LP = Cast<ALandscapeProxy>(A))
		{
			TSharedPtr<FJsonObject> LJ = MakeShareable(new FJsonObject);
			LJ->SetNumberField(TEXT("component_count"), LP->LandscapeComponents.Num());
			LJ->SetNumberField(TEXT("component_size_quads"), LP->ComponentSizeQuads);
			LJ->SetNumberField(TEXT("subsection_size_quads"), LP->SubsectionSizeQuads);
			LJ->SetNumberField(TEXT("num_subsections"), LP->NumSubsections);
			LJ->SetStringField(TEXT("landscape_guid"), LP->GetLandscapeGuid().ToString());
			FIntPoint MinSB(MAX_int32, MAX_int32);
			FIntPoint MaxSB(MIN_int32, MIN_int32);
			TSet<FName> LayerNames;
			for (ULandscapeComponent* LC : LP->LandscapeComponents)
			{
				if (!LC)
				{
					continue;
				}
				const FIntPoint SB = LC->GetSectionBase();
				MinSB.X = FMath::Min(MinSB.X, SB.X);
				MinSB.Y = FMath::Min(MinSB.Y, SB.Y);
				MaxSB.X = FMath::Max(MaxSB.X, SB.X);
				MaxSB.Y = FMath::Max(MaxSB.Y, SB.Y);
				for (const FWeightmapLayerAllocationInfo& Alloc : LC->GetWeightmapLayerAllocations())
				{
					if (Alloc.LayerInfo)
					{
						LayerNames.Add(Alloc.LayerInfo->LayerName);
					}
				}
			}
			TArray<TSharedPtr<FJsonValue>> LN;
			for (const FName& N : LayerNames)
			{
				LN.Add(MakeShareable(new FJsonValueString(N.ToString())));
			}
			LJ->SetArrayField(TEXT("layers"), LN);
			if (LP->LandscapeComponents.Num() > 0)
			{
				LJ->SetObjectField(TEXT("section_base_min"), Level_IntPoint(MinSB));
				LJ->SetObjectField(TEXT("section_base_max"), Level_IntPoint(MaxSB));
			}
			O->SetObjectField(TEXT("landscape"), LJ);
		}
		if (AInstancedFoliageActor* IFA = Cast<AInstancedFoliageActor>(A))
		{
			TArray<TSharedPtr<FJsonValue>> FArr;
			for (auto& Pair : IFA->FoliageInfos)
			{
				TSharedPtr<FJsonObject> FJ = MakeShareable(new FJsonObject);
				FJ->SetField(TEXT("foliage_type"), Level_PathOrNull(Pair.Key));
				UStaticMesh* Mesh = nullptr;
				if (UFoliageType_InstancedStaticMesh* FT = Cast<UFoliageType_InstancedStaticMesh>(Pair.Key))
				{
					Mesh = FT->GetStaticMesh();
				}
				FJ->SetField(TEXT("static_mesh"), Level_PathOrNull(Mesh));
				FJ->SetNumberField(TEXT("instance_count"), Pair.Value->Instances.Num());
				FArr.Add(MakeShareable(new FJsonValueObject(FJ)));
			}
			O->SetArrayField(TEXT("foliage"), FArr);
		}
		if (AVolume* Vol = Cast<AVolume>(A))
		{
			TSharedPtr<FJsonObject> VJ = MakeShareable(new FJsonObject);
			VJ->SetStringField(TEXT("class"), Vol->GetClass()->GetName());
			VJ->SetField(TEXT("bounds"), Level_Box(Bounds));
			O->SetObjectField(TEXT("volume"), VJ);
		}

		if (!bBoundsOnly)
		{
			TArray<TSharedPtr<FJsonValue>> Comps;
			TArray<USceneComponent*> SceneComps;
			A->GetComponents<USceneComponent>(SceneComps);
			for (USceneComponent* C : SceneComps)
			{
				if (C->IsEditorOnly())
				{
					continue;
				}
				if (!bListLandscapeComponents && C->IsA<ULandscapeComponent>())
				{
					continue;
				}
				Comps.Add(MakeShareable(new FJsonValueObject(Level_ComponentJson(C, bInstances))));
			}
			O->SetArrayField(TEXT("components"), Comps);
		}
		return O;
	}
}

// ----------------------------------------------------------------------
// level.list
// ----------------------------------------------------------------------

TSharedPtr<FJsonObject> UBlueprintExportCommandlet::LevelListToJson(const FString& Dir)
{
	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AR = ARM.Get();
	FString Folder = Dir;
	while (Folder.EndsWith(TEXT("/")))
	{
		Folder.LeftChopInline(1);
	}
	if (!FPackageName::IsValidLongPackageName(Folder + TEXT("/X")))
	{
		return Level_MakeError(FString::Printf(TEXT("Not a valid content folder: %s"), *Dir));
	}
	FARFilter Filter;
	Filter.PackagePaths.Add(FName(*Folder));
	Filter.bRecursivePaths = true;
	Filter.ClassNames.Add(UWorld::StaticClass()->GetFName());
	TArray<FAssetData> Assets;
	AR.GetAssets(Filter, Assets);
	TArray<FString> Names;
	for (const FAssetData& A : Assets)
	{
		Names.AddUnique(A.PackageName.ToString());
	}
	Names.Sort();
	TArray<TSharedPtr<FJsonValue>> Arr;
	for (const FString& N : Names)
	{
		Arr.Add(MakeShareable(new FJsonValueString(N)));
	}
	TSharedPtr<FJsonObject> Out = MakeShareable(new FJsonObject);
	Out->SetBoolField(TEXT("success"), true);
	Out->SetStringField(TEXT("dir"), Folder + TEXT("/"));
	Out->SetNumberField(TEXT("count"), Arr.Num());
	Out->SetArrayField(TEXT("maps"), Arr);
	return Out;
}

// ----------------------------------------------------------------------
// level.tiles
// ----------------------------------------------------------------------

TSharedPtr<FJsonObject> UBlueprintExportCommandlet::LevelTilesToJson(const FString& Path)
{
	FString Err;
	UWorld* World = Level_LoadWorld(Path, Err);
	if (!World)
	{
		return Level_MakeError(Err);
	}
	const FString PackageName = World->GetOutermost()->GetName();

	TSharedPtr<FJsonObject> Out = MakeShareable(new FJsonObject);
	Out->SetBoolField(TEXT("success"), true);
	Out->SetStringField(TEXT("path"), PackageName);

	// Persistent level actors, bounds-only shape.
	{
		TArray<FString> NoFilter;
		TSharedPtr<FJsonObject> Persistent = LevelActorsToJson(PackageName, NoFilter, /*bBoundsOnly*/true, /*bInstances*/false, /*bUnload*/false);
		TSharedPtr<FJsonObject> P = MakeShareable(new FJsonObject);
		double Count = 0;
		Persistent->TryGetNumberField(TEXT("actor_count"), Count);
		P->SetNumberField(TEXT("actor_count"), Count);
		const TArray<TSharedPtr<FJsonValue>>* Actors = nullptr;
		if (Persistent->TryGetArrayField(TEXT("actors"), Actors))
		{
			P->SetArrayField(TEXT("actors"), *Actors);
		}
		else
		{
			P->SetArrayField(TEXT("actors"), TArray<TSharedPtr<FJsonValue>>());
		}
		Out->SetObjectField(TEXT("persistent_level"), P);
	}

	UWorldComposition* WC = World->WorldComposition;
	Out->SetBoolField(TEXT("world_composition"), WC != nullptr);
	TArray<TSharedPtr<FJsonValue>> TilesArr;
	TArray<TSharedPtr<FJsonValue>> LayersArr;
	if (WC)
	{
		Out->SetStringField(TEXT("world_root"), WC->GetWorldRoot());
		UWorldComposition::FTilesList& Tiles = WC->GetTilesList();
		TMap<FString, int32> LayerCounts;
		TMap<FString, FWorldTileLayer> LayerDefs;
		for (int32 i = 0; i < Tiles.Num(); ++i)
		{
			const FWorldCompositionTile& T = Tiles[i];
			TSharedPtr<FJsonObject> TJ = MakeShareable(new FJsonObject);
			const FString Pkg = T.PackageName.ToString();
			TJ->SetStringField(TEXT("package"), Pkg);
			TJ->SetStringField(TEXT("short_name"), FPackageName::GetShortName(Pkg));
			TJ->SetField(TEXT("parent_package"), Level_StringOrNull(T.Info.ParentTilePackageName));
			TJ->SetObjectField(TEXT("layer"), Level_LayerJson(T.Info.Layer));
			TJ->SetObjectField(TEXT("position"), Level_IntVec(T.Info.Position));
			TJ->SetObjectField(TEXT("absolute_position"), Level_IntVec(T.Info.AbsolutePosition));
			TJ->SetField(TEXT("bounds"), Level_Box(T.Info.Bounds));
			TJ->SetNumberField(TEXT("z_order"), T.Info.ZOrder);
			TJ->SetBoolField(TEXT("hide_in_tile_view"), T.Info.bHideInTileView);
			TArray<TSharedPtr<FJsonValue>> Lods;
			for (int32 L = 0; L < T.Info.LODList.Num(); ++L)
			{
				TSharedPtr<FJsonObject> LJ = MakeShareable(new FJsonObject);
				LJ->SetNumberField(TEXT("index"), L + 1);
				if (T.LODPackageNames.IsValidIndex(L) && !T.LODPackageNames[L].IsNone())
				{
					LJ->SetStringField(TEXT("package"), T.LODPackageNames[L].ToString());
				}
				else
				{
					LJ->SetField(TEXT("package"), MakeShareable(new FJsonValueNull()));
				}
				LJ->SetNumberField(TEXT("relative_streaming_distance"), T.Info.LODList[L].RelativeStreamingDistance);
				LJ->SetNumberField(TEXT("streaming_distance"), T.Info.GetStreamingDistance(L));
				Lods.Add(MakeShareable(new FJsonValueObject(LJ)));
			}
			TJ->SetArrayField(TEXT("lods"), Lods);
			if (WC->TilesStreaming.IsValidIndex(i) && WC->TilesStreaming[i])
			{
				TJ->SetStringField(TEXT("streaming_class"), WC->TilesStreaming[i]->GetClass()->GetName());
			}
			else
			{
				TJ->SetField(TEXT("streaming_class"), MakeShareable(new FJsonValueNull()));
			}
			TJ->SetBoolField(TEXT("distance_dependent"), WC->IsDistanceDependentLevel(T.PackageName));
			TilesArr.Add(MakeShareable(new FJsonValueObject(TJ)));
			LayerCounts.FindOrAdd(T.Info.Layer.Name)++;
			LayerDefs.FindOrAdd(T.Info.Layer.Name) = T.Info.Layer;
		}
		TArray<FString> LayerNames;
		LayerDefs.GetKeys(LayerNames);
		LayerNames.Sort();
		for (const FString& LN : LayerNames)
		{
			TSharedPtr<FJsonObject> LJ = Level_LayerJson(LayerDefs[LN]);
			LJ->SetNumberField(TEXT("tile_count"), LayerCounts[LN]);
			LayersArr.Add(MakeShareable(new FJsonValueObject(LJ)));
		}
	}
	Out->SetNumberField(TEXT("tile_count"), TilesArr.Num());
	Out->SetArrayField(TEXT("layers"), LayersArr);
	Out->SetArrayField(TEXT("tiles"), TilesArr);
	return Out;
}

// ----------------------------------------------------------------------
// level.actors
// ----------------------------------------------------------------------

TSharedPtr<FJsonObject> UBlueprintExportCommandlet::LevelActorsToJson(const FString& Path, const TArray<FString>& ClassFilters, bool bBoundsOnly, bool bInstances, bool bUnload)
{
	FString Err;
	UWorld* World = Level_LoadWorld(Path, Err);
	if (!World)
	{
		return Level_MakeError(Err);
	}

	const FString PackageName = World->GetOutermost()->GetName();
	const FLevel_TileCtx Tile = Level_ReadTileCtx(PackageName);
	const FVector WorldOffset = Tile.bFound ? FVector(Tile.AbsolutePosition) : FVector::ZeroVector;

	bool bListLandscapeComps = false;
	for (const FString& F : ClassFilters)
	{
		if (F.StartsWith(TEXT("Landscape")))
		{
			bListLandscapeComps = true;
		}
	}

	TArray<TSharedPtr<FJsonValue>> Actors;
	ULevel* Level = World->PersistentLevel;
	if (Level)
	{
		for (AActor* A : Level->Actors)
		{
			if (!A || A->IsPendingKill())
			{
				continue;
			}
			if (A->IsA<AWorldSettings>())
			{
				continue;
			}
			if (!Level_MatchesFilter(A, ClassFilters))
			{
				continue;
			}
			Actors.Add(MakeShareable(new FJsonValueObject(Level_ActorJson(A, WorldOffset, bBoundsOnly, bInstances, bListLandscapeComps))));
		}
	}

	TSharedPtr<FJsonObject> Out = MakeShareable(new FJsonObject);
	Out->SetBoolField(TEXT("success"), true);
	Out->SetStringField(TEXT("path"), PackageName);
	if (Tile.bFound)
	{
		Out->SetObjectField(TEXT("tile"), Level_TileCtxJson(Tile));
	}
	else
	{
		Out->SetField(TEXT("tile"), MakeShareable(new FJsonValueNull()));
	}
	Out->SetNumberField(TEXT("actor_count"), Actors.Num());
	Out->SetArrayField(TEXT("actors"), Actors);

	if (bUnload)
	{
		Level_UnloadWorld(World);
	}
	return Out;
}

// ----------------------------------------------------------------------
// level.landscape
//
// Headless-loaded worlds never register components, and
// ULandscapeInfo::RegisterActorComponent skips unregistered components, so
// the ULandscapeInfo / FLandscapeEditDataInterface route yields an empty
// XYtoComponentMap (gamedev hit "No landscape found" on Sunrise). Instead we
// walk ULevel::Actors for ALandscapeProxy, key components by
// SectionBase / ComponentSizeQuads ourselves, and read each component's
// heightmap/weightmap texture SOURCE via FLandscapeComponentDataInterface,
// which only needs the component object (locks the mip's bulk data).
// ----------------------------------------------------------------------

namespace
{
	// One landscape (shared GUID) assembled from the level's proxies.
	struct FLevel_LandscapeSet
	{
		FGuid Guid;
		ALandscapeProxy* Primary = nullptr;               // ALandscape if present, else first proxy
		TArray<ALandscapeProxy*> Proxies;
		TMap<FIntPoint, ULandscapeComponent*> Components; // key = SectionBase / ComponentSizeQuads
		int32 ComponentSizeQuads = 0;
		int32 SubsectionSizeQuads = 0;
		int32 NumSubsections = 0;
		FIntPoint MinKey = FIntPoint(MAX_int32, MAX_int32);
		FIntPoint MaxKey = FIntPoint(MIN_int32, MIN_int32);
		TArray<ULandscapeLayerInfoObject*> Layers;

		int32 MinX() const { return MinKey.X * ComponentSizeQuads; }
		int32 MinY() const { return MinKey.Y * ComponentSizeQuads; }
		int32 MaxX() const { return (MaxKey.X + 1) * ComponentSizeQuads; }
		int32 MaxY() const { return (MaxKey.Y + 1) * ComponentSizeQuads; }
	};

	// Per-component cached reader: heightmap mip lock + per-layer weight arrays.
	struct FLevel_CompReader
	{
		TUniquePtr<FLandscapeComponentDataInterface> Data;
		TMap<ULandscapeLayerInfoObject*, TArray<uint8>> Weights; // empty array = layer not painted here
	};

	struct FLevel_LandscapeSampler
	{
		const FLevel_LandscapeSet& Set;
		TMap<ULandscapeComponent*, TUniquePtr<FLevel_CompReader>> Readers;

		explicit FLevel_LandscapeSampler(const FLevel_LandscapeSet& InSet) : Set(InSet) {}

		// Resolve quad coords to (component, local vertex). Edge vertices are
		// shared between neighbours; clamping the key keeps the landscape's
		// max edge resolvable.
		ULandscapeComponent* Resolve(int32 QX, int32 QY, int32& LX, int32& LY)
		{
			const int32 CSQ = Set.ComponentSizeQuads;
			int32 KX = FMath::FloorToInt((float)QX / CSQ);
			int32 KY = FMath::FloorToInt((float)QY / CSQ);
			KX = FMath::Clamp(KX, Set.MinKey.X, Set.MaxKey.X);
			KY = FMath::Clamp(KY, Set.MinKey.Y, Set.MaxKey.Y);
			ULandscapeComponent* const* Found = Set.Components.Find(FIntPoint(KX, KY));
			if (!Found)
			{
				return nullptr;
			}
			LX = FMath::Clamp(QX - KX * CSQ, 0, CSQ);
			LY = FMath::Clamp(QY - KY * CSQ, 0, CSQ);
			return *Found;
		}

		FLevel_CompReader& ReaderFor(ULandscapeComponent* C)
		{
			TUniquePtr<FLevel_CompReader>& R = Readers.FindOrAdd(C);
			if (!R)
			{
				R = MakeUnique<FLevel_CompReader>();
				// InWorkOnEditingLayer=false → final (baked) heightmap, not the edit-layer scratch.
				R->Data = MakeUnique<FLandscapeComponentDataInterface>(C, 0, /*InWorkOnEditingLayer*/false);
			}
			return *R;
		}

		// Raw uint16 height at quad coords; false if no component covers it.
		bool RawHeight(int32 QX, int32 QY, uint16& Out)
		{
			int32 LX, LY;
			ULandscapeComponent* C = Resolve(QX, QY, LX, LY);
			if (!C)
			{
				return false;
			}
			FLevel_CompReader& R = ReaderFor(C);
			if (!R.Data->GetRawHeightData())
			{
				return false;
			}
			Out = R.Data->GetHeight(LX, LY);
			return true;
		}

		// Weight 0..1 for a layer at quad coords (0 if unpainted / uncovered).
		float Weight(int32 QX, int32 QY, ULandscapeLayerInfoObject* Layer)
		{
			int32 LX, LY;
			ULandscapeComponent* C = Resolve(QX, QY, LX, LY);
			if (!C)
			{
				return 0.0f;
			}
			FLevel_CompReader& R = ReaderFor(C);
			TArray<uint8>* W = R.Weights.Find(Layer);
			if (!W)
			{
				W = &R.Weights.Add(Layer);
				R.Data->GetWeightmapTextureData(Layer, *W, /*InUseEditingWeightmap*/false);
			}
			const int32 Verts = Set.ComponentSizeQuads + 1;
			const int32 Idx = LY * Verts + LX;
			if (!W->IsValidIndex(Idx))
			{
				return 0.0f;
			}
			return (*W)[Idx] / 255.0f;
		}
	};

	float Level_RawToLocalZ(uint16 Raw)
	{
		return ((float)Raw - 32768.0f) / 128.0f;
	}

	// Group the level's landscape proxies by GUID and index their components.
	void Level_CollectLandscapes(ULevel* Level, TArray<FLevel_LandscapeSet>& Out)
	{
		TMap<FGuid, int32> GuidToIdx;
		for (AActor* A : Level->Actors)
		{
			ALandscapeProxy* Proxy = Cast<ALandscapeProxy>(A);
			if (!Proxy || Proxy->IsPendingKill())
			{
				continue;
			}
			int32* IdxPtr = GuidToIdx.Find(Proxy->GetLandscapeGuid());
			if (!IdxPtr)
			{
				FLevel_LandscapeSet NewSet;
				NewSet.Guid = Proxy->GetLandscapeGuid();
				NewSet.ComponentSizeQuads = Proxy->ComponentSizeQuads;
				NewSet.SubsectionSizeQuads = Proxy->SubsectionSizeQuads;
				NewSet.NumSubsections = Proxy->NumSubsections;
				const int32 NewIdx = Out.Add(MoveTemp(NewSet));
				IdxPtr = &GuidToIdx.Add(Proxy->GetLandscapeGuid(), NewIdx);
			}
			FLevel_LandscapeSet& Set = Out[*IdxPtr];
			Set.Proxies.Add(Proxy);
			if (!Set.Primary || Proxy->IsA<ALandscape>())
			{
				Set.Primary = Proxy;
			}
			if (Set.ComponentSizeQuads <= 0)
			{
				continue;
			}
			for (ULandscapeComponent* C : Proxy->LandscapeComponents)
			{
				if (!C)
				{
					continue;
				}
				const FIntPoint Key = C->GetSectionBase() / Set.ComponentSizeQuads;
				Set.Components.Add(Key, C);
				Set.MinKey.X = FMath::Min(Set.MinKey.X, Key.X);
				Set.MinKey.Y = FMath::Min(Set.MinKey.Y, Key.Y);
				Set.MaxKey.X = FMath::Max(Set.MaxKey.X, Key.X);
				Set.MaxKey.Y = FMath::Max(Set.MaxKey.Y, Key.Y);
				for (const FWeightmapLayerAllocationInfo& Alloc : C->GetWeightmapLayerAllocations())
				{
					if (Alloc.LayerInfo)
					{
						Set.Layers.AddUnique(Alloc.LayerInfo);
					}
				}
			}
		}
		for (FLevel_LandscapeSet& Set : Out)
		{
			Set.Layers.Sort([](const ULandscapeLayerInfoObject& A, const ULandscapeLayerInfoObject& B)
			{
				return A.LayerName.LexicalLess(B.LayerName);
			});
		}
	}
}

TSharedPtr<FJsonObject> UBlueprintExportCommandlet::LevelLandscapeToJson(const FString& Path, int32 GridN, const TArray<FVector2D>& Points, bool bLayers, bool bUnload)
{
	if (GridN > 1024)
	{
		return Level_MakeError(TEXT("grid too large (max 1024); use two passes or --points"));
	}
	if (GridN <= 0 && Points.Num() == 0)
	{
		return Level_MakeError(TEXT("Need grid (>0) or points"));
	}
	FString Err;
	UWorld* World = Level_LoadWorld(Path, Err);
	if (!World)
	{
		return Level_MakeError(Err);
	}

	TArray<FLevel_LandscapeSet> Sets;
	if (World->PersistentLevel)
	{
		Level_CollectLandscapes(World->PersistentLevel, Sets);
	}

	TArray<TSharedPtr<FJsonValue>> Landscapes;
	for (FLevel_LandscapeSet& Set : Sets)
	{
		if (Set.Components.Num() == 0 || Set.ComponentSizeQuads <= 0 || !Set.Primary)
		{
			continue;
		}
		// LandscapeActorToWorld reads the root component transform — refresh
		// it first (unregistered components have a stale ComponentToWorld).
		Level_RefreshTransforms(Set.Primary);
		const FTransform L2W = Set.Primary->LandscapeActorToWorld();
		const int32 MinX = Set.MinX();
		const int32 MinY = Set.MinY();
		const int32 MaxX = Set.MaxX();
		const int32 MaxY = Set.MaxY();

		TSharedPtr<FJsonObject> LJ = MakeShareable(new FJsonObject);
		LJ->SetStringField(TEXT("guid"), Set.Guid.ToString());
		LJ->SetStringField(TEXT("actor"), Set.Primary->GetName());
		TArray<TSharedPtr<FJsonValue>> ProxyNames;
		for (ALandscapeProxy* P : Set.Proxies)
		{
			ProxyNames.Add(MakeShareable(new FJsonValueString(P->GetName())));
		}
		LJ->SetArrayField(TEXT("proxies"), ProxyNames);
		TSharedPtr<FJsonObject> TJ = MakeShareable(new FJsonObject);
		Level_SetTransform(TJ, L2W, TEXT(""));
		LJ->SetObjectField(TEXT("transform"), TJ);
		LJ->SetNumberField(TEXT("component_size_quads"), Set.ComponentSizeQuads);
		LJ->SetNumberField(TEXT("subsection_size_quads"), Set.SubsectionSizeQuads);
		LJ->SetNumberField(TEXT("num_subsections"), Set.NumSubsections);
		TSharedPtr<FJsonObject> EJ = MakeShareable(new FJsonObject);
		EJ->SetNumberField(TEXT("min_x"), MinX);
		EJ->SetNumberField(TEXT("min_y"), MinY);
		EJ->SetNumberField(TEXT("max_x"), MaxX);
		EJ->SetNumberField(TEXT("max_y"), MaxY);
		LJ->SetObjectField(TEXT("extent_quads"), EJ);
		FBox WB(ForceInit);
		WB += L2W.TransformPosition(FVector(MinX, MinY, Level_RawToLocalZ(0)));
		WB += L2W.TransformPosition(FVector(MaxX, MaxY, Level_RawToLocalZ(65535)));
		LJ->SetField(TEXT("world_bounds"), Level_Box(WB));
		LJ->SetNumberField(TEXT("component_count"), Set.Components.Num());
		TSharedPtr<FJsonObject> CG = MakeShareable(new FJsonObject);
		CG->SetObjectField(TEXT("min"), Level_IntPoint(Set.MinKey));
		CG->SetObjectField(TEXT("max"), Level_IntPoint(Set.MaxKey));
		LJ->SetObjectField(TEXT("component_grid"), CG);

		TArray<TSharedPtr<FJsonValue>> LayersArr;
		for (ULandscapeLayerInfoObject* LO : Set.Layers)
		{
			TSharedPtr<FJsonObject> SJ = MakeShareable(new FJsonObject);
			SJ->SetStringField(TEXT("name"), LO->LayerName.ToString());
			SJ->SetStringField(TEXT("layer_info"), LO->GetPathName());
			SJ->SetNumberField(TEXT("hardness"), LO->Hardness);
			SJ->SetBoolField(TEXT("no_weight_blend"), LO->bNoWeightBlend != 0);
			LayersArr.Add(MakeShareable(new FJsonValueObject(SJ)));
		}
		LJ->SetArrayField(TEXT("layers"), LayersArr);

		// --- Sampling ---
		const bool bPointsMode = Points.Num() > 0;
		const bool bWantWeights = bLayers || bPointsMode;
		const int32 N = FMath::Max(GridN, 2);
		TArray<FIntPoint> Targets;
		if (bPointsMode)
		{
			for (const FVector2D& P : Points)
			{
				const FVector Local = L2W.InverseTransformPosition(FVector(P.X, P.Y, 0.0f));
				Targets.Add(FIntPoint(
					FMath::Clamp(FMath::RoundToInt(Local.X), MinX, MaxX),
					FMath::Clamp(FMath::RoundToInt(Local.Y), MinY, MaxY)));
			}
		}
		else
		{
			Targets.Reserve(N * N);
			for (int32 j = 0; j < N; ++j)
			{
				const int32 QY = MinY + FMath::RoundToInt((double)(MaxY - MinY) * j / (N - 1));
				for (int32 i = 0; i < N; ++i)
				{
					Targets.Add(FIntPoint(MinX + FMath::RoundToInt((double)(MaxX - MinX) * i / (N - 1)), QY));
				}
			}
		}

		FLevel_LandscapeSampler Sampler(Set);
		const FVector Scale = L2W.GetScale3D();
		int32 Uncovered = 0;
		TArray<TSharedPtr<FJsonValue>> PtsArr;
		for (const FIntPoint& Q : Targets)
		{
			uint16 Raw = 0;
			if (!Sampler.RawHeight(Q.X, Q.Y, Raw))
			{
				++Uncovered; // hole in the component grid (non-rectangular landscape)
				continue;
			}
			const float Z = Level_RawToLocalZ(Raw);
			auto H = [&Sampler, Z](int32 X, int32 Y) -> float
			{
				uint16 R;
				return Sampler.RawHeight(X, Y, R) ? Level_RawToLocalZ(R) : Z;
			};
			const float DzDx = (H(Q.X + 1, Q.Y) - H(Q.X - 1, Q.Y)) * Scale.Z / (2.0f * Scale.X);
			const float DzDy = (H(Q.X, Q.Y + 1) - H(Q.X, Q.Y - 1)) * Scale.Z / (2.0f * Scale.Y);
			const FVector Normal = FVector(-DzDx, -DzDy, 1.0f).GetSafeNormal();

			TSharedPtr<FJsonObject> PJ = MakeShareable(new FJsonObject);
			PJ->SetNumberField(TEXT("qx"), Q.X);
			PJ->SetNumberField(TEXT("qy"), Q.Y);
			PJ->SetObjectField(TEXT("world"), Level_Vec(L2W.TransformPosition(FVector(Q.X, Q.Y, Z))));
			PJ->SetNumberField(TEXT("height_raw"), Raw);
			PJ->SetObjectField(TEXT("normal"), Level_Vec(Normal));
			PJ->SetNumberField(TEXT("slope_deg"), FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Normal.Z, -1.0f, 1.0f))));
			if (bWantWeights && Set.Layers.Num() > 0)
			{
				FName Dominant = NAME_None;
				float Best = -1.0f;
				TSharedPtr<FJsonObject> WJ = MakeShareable(new FJsonObject);
				for (ULandscapeLayerInfoObject* LO : Set.Layers)
				{
					const float Wt = Sampler.Weight(Q.X, Q.Y, LO);
					WJ->SetNumberField(LO->LayerName.ToString(), Wt);
					if (Wt > Best)
					{
						Best = Wt;
						Dominant = LO->LayerName;
					}
				}
				PJ->SetStringField(TEXT("dominant_layer"), Dominant.ToString());
				PJ->SetObjectField(TEXT("weights"), WJ);
			}
			PtsArr.Add(MakeShareable(new FJsonValueObject(PJ)));
		}
		TSharedPtr<FJsonObject> SJ = MakeShareable(new FJsonObject);
		SJ->SetStringField(TEXT("mode"), bPointsMode ? TEXT("points") : TEXT("grid"));
		if (!bPointsMode)
		{
			SJ->SetNumberField(TEXT("grid"), N);
		}
		SJ->SetNumberField(TEXT("count"), PtsArr.Num());
		SJ->SetNumberField(TEXT("uncovered"), Uncovered);
		SJ->SetArrayField(TEXT("points"), PtsArr);
		LJ->SetObjectField(TEXT("samples"), SJ);
		Landscapes.Add(MakeShareable(new FJsonValueObject(LJ)));
	}

	if (Landscapes.Num() == 0)
	{
		if (bUnload)
		{
			Level_UnloadWorld(World);
		}
		return Level_MakeError(FString::Printf(TEXT("No landscape proxies with components in %s"), *Path));
	}
	TSharedPtr<FJsonObject> Out = MakeShareable(new FJsonObject);
	Out->SetBoolField(TEXT("success"), true);
	Out->SetStringField(TEXT("path"), World->GetOutermost()->GetName());
	Out->SetArrayField(TEXT("landscapes"), Landscapes);
	if (bUnload)
	{
		Level_UnloadWorld(World);
	}
	return Out;
}
