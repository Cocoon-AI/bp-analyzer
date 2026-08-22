# `digbp level` Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Read-only JSON export of World-Composition tiles, per-level actors, and sampled landscape height/weights, as `digbp level {list,tiles,actors,landscape}`.

**Architecture:** New `BlueprintExportLevel.cpp` in the UE4.27 editor plugin exposes four `UBlueprintExportCommandlet::Level*ToJson` methods; `BlueprintExportServer.cpp` routes `level.*` JSON-RPC methods to them; `cmd/digbp/level.go` is the cobra CLI (incl. the `--dir` batch walker and CSV writer, which are Go-side).

**Tech Stack:** UE4.27 C++ (Engine, Landscape, Foliage, UnrealEd modules), Go 1.x + cobra.

**Spec:** `docs/superpowers/specs/2026-08-22-level-export-design.md`

## Global Constraints

- UE4.27 API only (no UE5: no `TArray::Slice`, no `PropertyGuids`). Verify any new engine call against `D:/sd/steamdev/Engine/Source` headers (read-only; never build there with UBT directly — ship with `just sync`).
- Unity build: every file-local helper gets a `Level_` prefix.
- Error envelope: `{"success":false,"error":"..."}`; never `check()` on user input.
- No asset mutation; never call `SavePackage`.
- Ship: `just sync` (plugin → steamdev, auto-submits CL) then `just build` (CLI → `C:/tools/digbp.exe`). Editor rebuild is gamedev's job — say so.

---

## File map

| File | Responsibility |
|---|---|
| `BlueprintAnalyzer/Source/BlueprintAnalyzer/BlueprintAnalyzer.Build.cs` | add `Landscape`, `Foliage` deps |
| `BlueprintAnalyzer/Source/BlueprintAnalyzer/Private/BlueprintExportCommandlet.h` | declare `LevelListToJson`, `LevelTilesToJson`, `LevelActorsToJson`, `LevelLandscapeToJson` |
| `BlueprintAnalyzer/Source/BlueprintAnalyzer/Private/BlueprintExportLevel.cpp` | all level export logic |
| `BlueprintAnalyzer/Source/BlueprintAnalyzer/Private/BlueprintExportServer.cpp` | `level.*` dispatch |
| `cmd/digbp/level.go` | CLI: list/tiles/actors/landscape, batch walker, points parser, CSV |
| `cmd/digbp/level_test.go` | unit tests for points parser + CSV |
| `cmd/digbp/main.go` | register `levelCmd()` |
| `CLAUDE.md`, `claude-code-skills/blueprint-export/SKILL.md` | docs |

---

### Task 1: Scaffold + `level.list` + `level.tiles`

**Files:**
- Modify: `BlueprintAnalyzer/Source/BlueprintAnalyzer/BlueprintAnalyzer.Build.cs`
- Modify: `BlueprintAnalyzer/Source/BlueprintAnalyzer/Private/BlueprintExportCommandlet.h` (after the anim block, ~line 230)
- Create: `BlueprintAnalyzer/Source/BlueprintAnalyzer/Private/BlueprintExportLevel.cpp`
- Modify: `BlueprintAnalyzer/Source/BlueprintAnalyzer/Private/BlueprintExportServer.cpp` (after `anim.save` block)

**Interfaces:**
- Produces: `TSharedPtr<FJsonObject> LevelListToJson(const FString& Dir)`, `LevelTilesToJson(const FString& Path)`; file-local helpers `Level_LoadWorld`, `Level_Vec`, `Level_Box`, `Level_MakeError`, `Level_TileInfoJson` reused by Tasks 2/4.

- [ ] **Step 1: Build.cs** — in the `Target.Type == TargetType.Editor` block add:

```csharp
				// ULandscapeInfo / FLandscapeEditDataInterface for `level landscape`.
				"Landscape",
				// AInstancedFoliageActor for `level actors` foliage summaries.
				"Foliage"
```

- [ ] **Step 2: Header declarations** — append to `BlueprintExportCommandlet.h` inside the class after the anim mutation block:

```cpp
	// --- Level / World Composition export (BlueprintExportLevel.cpp) ---
	// Read-only. Loads the map package (no world init, no streaming).
	// LevelListToJson: asset-registry World assets under Dir (recursive).
	// LevelTilesToJson: UWorldComposition tile table (tiles read from
	// package summaries, never fully loaded) + persistent-level actors.
	// LevelActorsToJson: ULevel::Actors dump with components/bounds.
	// LevelLandscapeToJson: height/weight sampling via FLandscapeEditDataInterface.
	TSharedPtr<FJsonObject> LevelListToJson(const FString& Dir);
	TSharedPtr<FJsonObject> LevelTilesToJson(const FString& Path);
	TSharedPtr<FJsonObject> LevelActorsToJson(const FString& Path, const TArray<FString>& ClassFilters, bool bBoundsOnly, bool bInstances, bool bUnload);
	TSharedPtr<FJsonObject> LevelLandscapeToJson(const FString& Path, int32 GridN, const TArray<FVector2D>& Points, bool bLayers, bool bUnload);
```

- [ ] **Step 3: Create `BlueprintExportLevel.cpp`** with shared helpers + list + tiles:

```cpp
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
// summary (FWorldTileInfo::Read) — tiles are never fully loaded.
//
// Unity-build note: unique Level_ prefix on file-local helpers.

#include "BlueprintExportCommandlet.h"

#include "Engine/World.h"
#include "Engine/Level.h"
#include "Engine/LevelStreaming.h"
#include "Engine/WorldComposition.h"
#include "Misc/WorldCompositionUtility.h"
#include "Misc/PackageName.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Volume.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/ChildActorComponent.h"
#include "Components/SplineComponent.h"
#include "Components/BrushComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Materials/MaterialInterface.h"
#include "LandscapeProxy.h"
#include "Landscape.h"
#include "LandscapeComponent.h"
#include "LandscapeInfo.h"
#include "LandscapeLayerInfoObject.h"
#include "LandscapeEdit.h"
#include "InstancedFoliageActor.h"
#include "InstancedFoliage.h"
#include "FoliageType.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "PackageTools.h"
#include "UObject/Package.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
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
		if (!World) return;
		UPackage* Package = World->GetOutermost();
		TArray<UPackage*> Packages;
		Packages.Add(Package);
		FText Err;
		UPackageTools::UnloadPackages(Packages, Err);
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	}

	// Read this package's own tile info + walk parents to get the absolute
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
		if (Ctx.Info.ParentTilePackageName.IsEmpty() || Ctx.Info.ParentTilePackageName == TEXT("None"))
		{
			O->SetField(TEXT("parent_package"), MakeShareable(new FJsonValueNull()));
		}
		else
		{
			O->SetStringField(TEXT("parent_package"), Ctx.Info.ParentTilePackageName);
		}
		O->SetNumberField(TEXT("z_order"), Ctx.Info.ZOrder);
		return O;
	}
}

TSharedPtr<FJsonObject> UBlueprintExportCommandlet::LevelListToJson(const FString& Dir)
{
	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AR = ARM.Get();
	FString Folder = Dir;
	while (Folder.EndsWith(TEXT("/"))) Folder.LeftChopInline(1);
	FARFilter Filter;
	Filter.PackagePaths.Add(FName(*Folder));
	Filter.bRecursivePaths = true;
	Filter.ClassNames.Add(UWorld::StaticClass()->GetFName());
	TArray<FAssetData> Assets;
	AR.GetAssets(Filter, Assets);
	TArray<FString> Names;
	for (const FAssetData& A : Assets)
	{
		Names.Add(A.PackageName.ToString());
	}
	Names.Sort();
	TArray<TSharedPtr<FJsonValue>> Arr;
	for (const FString& N : Names) Arr.Add(MakeShareable(new FJsonValueString(N)));
	TSharedPtr<FJsonObject> Out = MakeShareable(new FJsonObject);
	Out->SetBoolField(TEXT("success"), true);
	Out->SetStringField(TEXT("dir"), Folder + TEXT("/"));
	Out->SetNumberField(TEXT("count"), Arr.Num());
	Out->SetArrayField(TEXT("maps"), Arr);
	return Out;
}

TSharedPtr<FJsonObject> UBlueprintExportCommandlet::LevelTilesToJson(const FString& Path)
{
	FString Err;
	UWorld* World = Level_LoadWorld(Path, Err);
	if (!World) return Level_MakeError(Err);

	TSharedPtr<FJsonObject> Out = MakeShareable(new FJsonObject);
	Out->SetBoolField(TEXT("success"), true);
	Out->SetStringField(TEXT("path"), World->GetOutermost()->GetName());

	// Persistent level actors, bounds-only shape (Task 2 helper).
	{
		TArray<FString> NoFilter;
		TSharedPtr<FJsonObject> Persistent = LevelActorsToJson(World->GetOutermost()->GetName(), NoFilter, /*bBoundsOnly*/true, /*bInstances*/false, /*bUnload*/false);
		TSharedPtr<FJsonObject> P = MakeShareable(new FJsonObject);
		P->SetNumberField(TEXT("actor_count"), Persistent->HasField(TEXT("actor_count")) ? Persistent->GetNumberField(TEXT("actor_count")) : 0);
		if (Persistent->HasField(TEXT("actors")))
		{
			P->SetArrayField(TEXT("actors"), Persistent->GetArrayField(TEXT("actors")));
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
			if (T.Info.ParentTilePackageName.IsEmpty() || T.Info.ParentTilePackageName == TEXT("None"))
				TJ->SetField(TEXT("parent_package"), MakeShareable(new FJsonValueNull()));
			else
				TJ->SetStringField(TEXT("parent_package"), T.Info.ParentTilePackageName);
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
					LJ->SetStringField(TEXT("package"), T.LODPackageNames[L].ToString());
				else
					LJ->SetField(TEXT("package"), MakeShareable(new FJsonValueNull()));
				LJ->SetNumberField(TEXT("relative_streaming_distance"), T.Info.LODList[L].RelativeStreamingDistance);
				LJ->SetNumberField(TEXT("streaming_distance"), T.Info.GetStreamingDistance(L));
				Lods.Add(MakeShareable(new FJsonValueObject(LJ)));
			}
			TJ->SetArrayField(TEXT("lods"), Lods);
			if (WC->TilesStreaming.IsValidIndex(i) && WC->TilesStreaming[i])
				TJ->SetStringField(TEXT("streaming_class"), WC->TilesStreaming[i]->GetClass()->GetName());
			else
				TJ->SetField(TEXT("streaming_class"), MakeShareable(new FJsonValueNull()));
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
```

Note: `LevelActorsToJson` is referenced here and defined in Task 2 — add a stub returning `Level_MakeError(TEXT("not implemented"))` at the bottom of this file in Task 1 so it compiles, replaced in Task 2. Same for `LevelLandscapeToJson`.

- [ ] **Step 4: Server dispatch** — after the `anim.save` block in `BlueprintExportServer.cpp`:

```cpp
	// --- Level / World Composition export (read-only) ---

	if (Method == TEXT("level.list"))
	{
		FString Dir;
		if (!Params->TryGetStringField(TEXT("dir"), Dir) || Dir.IsEmpty())
		{
			return MakeErrorResponse(Id, JSONRPC_INVALID_PARAMS, TEXT("Missing required param: dir"));
		}
		return MakeResponse(Id, Commandlet->LevelListToJson(Dir));
	}

	if (Method == TEXT("level.tiles"))
	{
		FString Path;
		if (!Params->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
		{
			return MakeErrorResponse(Id, JSONRPC_INVALID_PARAMS, TEXT("Missing required param: path"));
		}
		return MakeResponse(Id, Commandlet->LevelTilesToJson(Path));
	}

	if (Method == TEXT("level.actors"))
	{
		FString Path;
		if (!Params->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
		{
			return MakeErrorResponse(Id, JSONRPC_INVALID_PARAMS, TEXT("Missing required param: path"));
		}
		TArray<FString> ClassFilters;
		const TArray<TSharedPtr<FJsonValue>>* ClassArr = nullptr;
		if (Params->TryGetArrayField(TEXT("classes"), ClassArr))
		{
			for (const TSharedPtr<FJsonValue>& V : *ClassArr) ClassFilters.Add(V->AsString());
		}
		bool bBoundsOnly = false, bInstances = false, bUnload = false;
		Params->TryGetBoolField(TEXT("bounds_only"), bBoundsOnly);
		Params->TryGetBoolField(TEXT("instances"), bInstances);
		Params->TryGetBoolField(TEXT("unload"), bUnload);
		return MakeResponse(Id, Commandlet->LevelActorsToJson(Path, ClassFilters, bBoundsOnly, bInstances, bUnload));
	}

	if (Method == TEXT("level.landscape"))
	{
		FString Path;
		if (!Params->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
		{
			return MakeErrorResponse(Id, JSONRPC_INVALID_PARAMS, TEXT("Missing required param: path"));
		}
		int32 GridN = 0;
		Params->TryGetNumberField(TEXT("grid"), GridN);
		TArray<FVector2D> Points;
		const TArray<TSharedPtr<FJsonValue>>* PtsArr = nullptr;
		if (Params->TryGetArrayField(TEXT("points"), PtsArr))
		{
			for (const TSharedPtr<FJsonValue>& V : *PtsArr)
			{
				const TArray<TSharedPtr<FJsonValue>>& Pair = V->AsArray();
				if (Pair.Num() == 2) Points.Add(FVector2D(Pair[0]->AsNumber(), Pair[1]->AsNumber()));
			}
		}
		if (GridN <= 0 && Points.Num() == 0)
		{
			return MakeErrorResponse(Id, JSONRPC_INVALID_PARAMS, TEXT("Need grid (>0) or points ([[x,y],...])"));
		}
		bool bLayers = false, bUnload = false;
		Params->TryGetBoolField(TEXT("layers"), bLayers);
		Params->TryGetBoolField(TEXT("unload"), bUnload);
		return MakeResponse(Id, Commandlet->LevelLandscapeToJson(Path, GridN, Points, bLayers, bUnload));
	}
```

- [ ] **Step 5: Commit** — `git add -A BlueprintAnalyzer && git commit -m "feat(level): scaffold level export; level.list + level.tiles"`

(Compilation is verified at Task 5 via `just sync` + gamedev's editor build; there is no local UBT path per project constraints. Re-read each engine call against headers before committing.)

---

### Task 2: `level.actors`

**Files:**
- Modify: `BlueprintExportLevel.cpp` (replace stub)

**Interfaces:**
- Consumes: `Level_LoadWorld`, `Level_ReadTileCtx`, `Level_TileCtxJson`, `Level_Box`, `Level_Vec`, `Level_Rot`, `Level_UnloadWorld`.
- Produces: `LevelActorsToJson(Path, ClassFilters, bBoundsOnly, bInstances, bUnload)` per spec JSON.

- [ ] **Step 1: Add actor helpers** inside the anonymous namespace:

```cpp
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
		if (Filters.Num() == 0) return true;
		for (const UClass* C = A->GetClass(); C; C = C->GetSuperClass())
		{
			const FString N = C->GetName();
			for (const FString& F : Filters)
			{
				// Match "Foo", "Foo_C", or "/Game/.../Foo.Foo_C".
				if (N == F || N == F + TEXT("_C") || C->GetPathName() == F) return true;
			}
		}
		return false;
	}

	// Make ComponentToWorld valid on an unregistered component tree.
	void Level_RefreshTransforms(AActor* A)
	{
		TArray<USceneComponent*> Comps;
		A->GetComponents<USceneComponent>(Comps);
		// Parents before children: UpdateComponentToWorld walks attach parents itself,
		// so order doesn't matter for correctness, only for cost.
		for (USceneComponent* C : Comps) C->UpdateComponentToWorld();
	}

	FBox Level_ComponentBounds(USceneComponent* C)
	{
		// CalcBounds is virtual and does not need registration for the
		// component classes we care about (StaticMesh/ISM/HISM/Brush/Landscape).
		return C->CalcBounds(C->GetComponentTransform()).GetBox();
	}

	FBox Level_ActorBounds(AActor* A, bool bIncludeNonColliding = true)
	{
		FBox Box(ForceInit);
		TArray<UPrimitiveComponent*> Prims;
		A->GetComponents<UPrimitiveComponent>(Prims);
		for (UPrimitiveComponent* P : Prims)
		{
			if (P->IsEditorOnly()) continue;
			FBox B = Level_ComponentBounds(P);
			if (B.IsValid) Box += B;
		}
		return Box;
	}

	FBox Level_Offset(const FBox& B, const FVector& O)
	{
		return B.IsValid ? FBox(B.Min + O, B.Max + O) : B;
	}

	TSharedPtr<FJsonValue> Level_PathOrNull(const UObject* O)
	{
		return O ? TSharedPtr<FJsonValue>(MakeShareable(new FJsonValueString(O->GetPathName())))
		         : TSharedPtr<FJsonValue>(MakeShareable(new FJsonValueNull()));
	}

	void Level_SetTransform(const TSharedPtr<FJsonObject>& O, const FTransform& T, const TCHAR* Prefix)
	{
		O->SetObjectField(FString(Prefix) + TEXT("location"), Level_Vec(T.GetLocation()));
		O->SetObjectField(FString(Prefix) + TEXT("rotation"), Level_Rot(T.Rotator()));
		O->SetObjectField(FString(Prefix) + TEXT("scale"), Level_Vec(T.GetScale3D()));
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
		else if (UBrushComponent* Brush = Cast<UBrushComponent>(C))
		{
			O->SetField(TEXT("bounds"), Level_Box(Level_ComponentBounds(Brush)));
		}
		else if (ULandscapeComponent* LC = Cast<ULandscapeComponent>(C))
		{
			TSharedPtr<FJsonObject> SB = MakeShareable(new FJsonObject);
			SB->SetNumberField(TEXT("x"), LC->GetSectionBase().X);
			SB->SetNumberField(TEXT("y"), LC->GetSectionBase().Y);
			O->SetObjectField(TEXT("section_base"), SB);
			O->SetField(TEXT("bounds"), Level_Box(Level_ComponentBounds(LC)));
		}
		else if (UPrimitiveComponent* Prim = Cast<UPrimitiveComponent>(C))
		{
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
		FString Native; TArray<FString> Chain;
		Level_ClassChain(A->GetClass(), Native, Chain);
		O->SetStringField(TEXT("native_class"), Native);
		TArray<TSharedPtr<FJsonValue>> ChainArr;
		for (const FString& S : Chain) ChainArr.Add(MakeShareable(new FJsonValueString(S)));
		O->SetArrayField(TEXT("blueprint_chain"), ChainArr);
		O->SetStringField(TEXT("folder"), A->GetFolderPath().ToString());
		TArray<TSharedPtr<FJsonValue>> Tags;
		for (const FName& T : A->Tags) Tags.Add(MakeShareable(new FJsonValueString(T.ToString())));
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
			FIntPoint MinSB(MAX_int32, MAX_int32), MaxSB(MIN_int32, MIN_int32);
			TSet<FName> LayerNames;
			for (ULandscapeComponent* LC : LP->LandscapeComponents)
			{
				if (!LC) continue;
				MinSB.X = FMath::Min(MinSB.X, LC->GetSectionBase().X); MinSB.Y = FMath::Min(MinSB.Y, LC->GetSectionBase().Y);
				MaxSB.X = FMath::Max(MaxSB.X, LC->GetSectionBase().X); MaxSB.Y = FMath::Max(MaxSB.Y, LC->GetSectionBase().Y);
				for (const FWeightmapLayerAllocationInfo& Alloc : LC->GetWeightmapLayerAllocations())
				{
					if (Alloc.LayerInfo) LayerNames.Add(Alloc.LayerInfo->LayerName);
				}
			}
			TArray<TSharedPtr<FJsonValue>> LN;
			for (const FName& N : LayerNames) LN.Add(MakeShareable(new FJsonValueString(N.ToString())));
			LJ->SetArrayField(TEXT("layers"), LN);
			if (LP->LandscapeComponents.Num() > 0)
			{
				TSharedPtr<FJsonObject> A1 = MakeShareable(new FJsonObject); A1->SetNumberField(TEXT("x"), MinSB.X); A1->SetNumberField(TEXT("y"), MinSB.Y);
				TSharedPtr<FJsonObject> A2 = MakeShareable(new FJsonObject); A2->SetNumberField(TEXT("x"), MaxSB.X); A2->SetNumberField(TEXT("y"), MaxSB.Y);
				LJ->SetObjectField(TEXT("section_base_min"), A1);
				LJ->SetObjectField(TEXT("section_base_max"), A2);
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
				if (UFoliageType_InstancedStaticMesh* FT = Cast<UFoliageType_InstancedStaticMesh>(Pair.Key)) Mesh = FT->GetStaticMesh();
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
				if (C->IsEditorOnly()) continue;
				if (!bListLandscapeComponents && C->IsA<ULandscapeComponent>()) continue;
				Comps.Add(MakeShareable(new FJsonValueObject(Level_ComponentJson(C, bInstances))));
			}
			O->SetArrayField(TEXT("components"), Comps);
		}
		return O;
	}
```

Header additions for this step: `#include "FoliageType_InstancedStaticMesh.h"`, `#include "LandscapeLayerInfoObject.h"` (already), `#include "Components/PrimitiveComponent.h"`.

- [ ] **Step 2: Replace the `LevelActorsToJson` stub**

```cpp
TSharedPtr<FJsonObject> UBlueprintExportCommandlet::LevelActorsToJson(const FString& Path, const TArray<FString>& ClassFilters, bool bBoundsOnly, bool bInstances, bool bUnload)
{
	FString Err;
	UWorld* World = Level_LoadWorld(Path, Err);
	if (!World) return Level_MakeError(Err);

	const FString PackageName = World->GetOutermost()->GetName();
	const FLevel_TileCtx Tile = Level_ReadTileCtx(PackageName);
	const FVector WorldOffset = Tile.bFound ? FVector(Tile.AbsolutePosition) : FVector::ZeroVector;

	bool bListLandscapeComps = false;
	for (const FString& F : ClassFilters) if (F.StartsWith(TEXT("Landscape"))) bListLandscapeComps = true;

	TArray<TSharedPtr<FJsonValue>> Actors;
	ULevel* Level = World->PersistentLevel;
	for (AActor* A : Level->Actors)
	{
		if (!A || A->IsPendingKill()) continue;
		if (A->IsA<AWorldSettings>()) continue;
		if (!Level_MatchesFilter(A, ClassFilters)) continue;
		Actors.Add(MakeShareable(new FJsonValueObject(Level_ActorJson(A, WorldOffset, bBoundsOnly, bInstances, bListLandscapeComps))));
	}

	TSharedPtr<FJsonObject> Out = MakeShareable(new FJsonObject);
	Out->SetBoolField(TEXT("success"), true);
	Out->SetStringField(TEXT("path"), PackageName);
	if (Tile.bFound) Out->SetObjectField(TEXT("tile"), Level_TileCtxJson(Tile));
	else Out->SetField(TEXT("tile"), MakeShareable(new FJsonValueNull()));
	Out->SetNumberField(TEXT("actor_count"), Actors.Num());
	Out->SetArrayField(TEXT("actors"), Actors);

	if (bUnload) Level_UnloadWorld(World);
	return Out;
}
```

Add `#include "GameFramework/WorldSettings.h"`.

- [ ] **Step 3: Commit** — `git commit -am "feat(level): level.actors — actor/component dump with tile-offset world bounds"`

---

### Task 3: Go CLI `level.go` + tests

**Files:**
- Create: `cmd/digbp/level.go`, `cmd/digbp/level_test.go`
- Modify: `cmd/digbp/main.go` (add `levelCmd(),` after `animCmd(),`)

**Interfaces:**
- Consumes: `callServer`, `callServerToFile`, `normalizeOutPath`, `rpc.Call`, `server.EnsureRunning`, `cfg`, `flagPretty` from `main.go`.
- Produces: `parsePoints(s string) ([][2]float64, error)`, `writeLandscapeCSV(w io.Writer, result []byte) error`.

- [ ] **Step 1: Write failing tests** `cmd/digbp/level_test.go`:

```go
package main

import (
	"bytes"
	"strings"
	"testing"
)

func TestParsePoints(t *testing.T) {
	pts, err := parsePoints("1,2;3.5,-4")
	if err != nil {
		t.Fatal(err)
	}
	if len(pts) != 2 || pts[0] != [2]float64{1, 2} || pts[1] != [2]float64{3.5, -4} {
		t.Fatalf("got %v", pts)
	}
	if _, err := parsePoints("1,2;3"); err == nil {
		t.Fatal("expected error on malformed pair")
	}
	if _, err := parsePoints(""); err == nil {
		t.Fatal("expected error on empty")
	}
}

func TestWriteLandscapeCSV(t *testing.T) {
	in := []byte(`{"success":true,"landscapes":[{"samples":{"points":[
		{"qx":0,"qy":0,"world":{"x":1,"y":2,"z":3},"slope_deg":4.5,"dominant_layer":"Grass","weights":{"Grass":0.75,"Soil":0.25}},
		{"qx":1,"qy":0,"world":{"x":5,"y":2,"z":6},"slope_deg":0,"dominant_layer":"Soil","weights":{"Grass":0.1,"Soil":0.9}}
	]}}]}`)
	var buf bytes.Buffer
	if err := writeLandscapeCSV(&buf, in); err != nil {
		t.Fatal(err)
	}
	lines := strings.Split(strings.TrimSpace(buf.String()), "\n")
	if lines[0] != "qx,qy,wx,wy,wz,slope_deg,dominant,Grass,Soil" {
		t.Fatalf("header: %q", lines[0])
	}
	if lines[1] != "0,0,1,2,3,4.5,Grass,0.75,0.25" {
		t.Fatalf("row1: %q", lines[1])
	}
	if len(lines) != 3 {
		t.Fatalf("rows: %d", len(lines))
	}
}
```

- [ ] **Step 2: Run** `go test ./cmd/digbp/ -run 'TestParsePoints|TestWriteLandscapeCSV'` → FAIL (undefined).

- [ ] **Step 3: Write `cmd/digbp/level.go`**

```go
package main

import (
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path"
	"path/filepath"
	"sort"
	"strconv"
	"strings"

	"github.com/spf13/cobra"

	"github.com/ooo27/bp-analyzer/internal/rpc"   // adjust to go.mod module path
	"github.com/ooo27/bp-analyzer/internal/server"
)

// levelCmd groups read-only level / World Composition / landscape inspection.
func levelCmd() *cobra.Command {
	cmd := &cobra.Command{
		Use:   "level",
		Short: "Inspect level packages: World Composition tiles, actors, landscape samples (read-only)",
		Long: `Read-only JSON export of .umap packages. Loads the package headless
(no world init, no streaming). World Composition tiles are read from package
summaries, never fully loaded. Tile-level actor transforms are tile-local as
authored; 'world_*' fields add the tile's absolute offset.`,
	}
	cmd.AddCommand(levelListCmd(), levelTilesCmd(), levelActorsCmd(), levelLandscapeCmd())
	return cmd
}

func levelListCmd() *cobra.Command {
	var dir string
	cmd := &cobra.Command{
		Use:   "list",
		Short: "List map packages (UWorld assets) under a folder",
		RunE: func(cmd *cobra.Command, args []string) error {
			return callServer("level.list", map[string]interface{}{"dir": dir})
		},
	}
	cmd.Flags().StringVar(&dir, "dir", "", "Content folder, e.g. /Game/Showdown/Maps/Sunrise/Sunrise_SubLevels (required)")
	_ = cmd.MarkFlagRequired("dir")
	return cmd
}

func levelTilesCmd() *cobra.Command {
	var p, out string
	cmd := &cobra.Command{
		Use:   "tiles",
		Short: "World Composition overview: every tile's layer, position, bounds, LODs, streaming class",
		RunE: func(cmd *cobra.Command, args []string) error {
			params := map[string]interface{}{"path": p}
			if out != "" {
				return callServerToFile("level.tiles", params, out)
			}
			return callServer("level.tiles", params)
		},
	}
	cmd.Flags().StringVar(&p, "path", "", "Persistent level package, e.g. /Game/Showdown/Maps/Sunrise/Sunrise (required)")
	cmd.Flags().StringVar(&out, "out", "", "Write JSON to file instead of stdout")
	_ = cmd.MarkFlagRequired("path")
	return cmd
}

func levelActorsCmd() *cobra.Command {
	var (
		p, dir, out, classes string
		boundsOnly, instances bool
	)
	cmd := &cobra.Command{
		Use:   "actors",
		Short: "Dump a level's actors (class chain, folder, tags, transform, bounds, components)",
		Long: `Single level: --path=<map> [--out=file]
Batch:        --dir=<folder> --out=<dir>   (one <ShortName>.json per map; server unloads each map after export)

--class accepts a comma list; matches the actor class, any Blueprint parent, or
any native ancestor (e.g. --class=Volume,BP_LootGenerator).
--bounds-only omits per-component detail. --instances adds per-instance
transforms for ISM/HISM components (large).`,
		RunE: func(cmd *cobra.Command, args []string) error {
			if (p == "") == (dir == "") {
				return fmt.Errorf("exactly one of --path or --dir is required")
			}
			params := map[string]interface{}{"bounds_only": boundsOnly, "instances": instances}
			if classes != "" {
				params["classes"] = splitCSV(classes)
			}
			if p != "" {
				params["path"] = p
				if out != "" {
					return callServerToFile("level.actors", params, out)
				}
				return callServer("level.actors", params)
			}
			if out == "" {
				return fmt.Errorf("--dir requires --out=<directory>")
			}
			return levelActorsBatch(dir, out, params)
		},
	}
	cmd.Flags().StringVar(&p, "path", "", "Level package path")
	cmd.Flags().StringVar(&dir, "dir", "", "Folder of maps to walk (batch mode)")
	cmd.Flags().StringVar(&out, "out", "", "Output file (--path) or directory (--dir)")
	cmd.Flags().StringVar(&classes, "class", "", "Comma-separated class filter")
	cmd.Flags().BoolVar(&boundsOnly, "bounds-only", false, "Omit component detail")
	cmd.Flags().BoolVar(&instances, "instances", false, "Include per-instance transforms for ISM/HISM")
	return cmd
}

func splitCSV(s string) []string {
	var out []string
	for _, part := range strings.Split(s, ",") {
		if t := strings.TrimSpace(part); t != "" {
			out = append(out, t)
		}
	}
	return out
}

func levelActorsBatch(dir, outDir string, base map[string]interface{}) error {
	if err := server.EnsureRunning(cfg); err != nil {
		return err
	}
	raw, err := rpc.Call(cfg.PipeName, "level.list", map[string]interface{}{"dir": dir})
	if err != nil {
		return err
	}
	var list struct {
		Success bool     `json:"success"`
		Error   string   `json:"error"`
		Maps    []string `json:"maps"`
	}
	if err := json.Unmarshal(raw, &list); err != nil {
		return fmt.Errorf("level.list: %w", err)
	}
	if !list.Success {
		return fmt.Errorf("level.list: %s", list.Error)
	}
	resolved := normalizeOutPath(outDir)
	if err := os.MkdirAll(resolved, 0755); err != nil {
		return err
	}
	failed := 0
	for i, m := range list.Maps {
		params := map[string]interface{}{}
		for k, v := range base {
			params[k] = v
		}
		params["path"] = m
		params["unload"] = true
		res, err := rpc.Call(cfg.PipeName, "level.actors", params)
		name := path.Base(m) + ".json"
		if err != nil {
			failed++
			fmt.Fprintf(os.Stderr, "[%d/%d] %s: %v\n", i+1, len(list.Maps), m, err)
			continue
		}
		payload := []byte(res)
		if flagPretty {
			var v interface{}
			if json.Unmarshal(res, &v) == nil {
				if pp, err := json.MarshalIndent(v, "", "  "); err == nil {
					payload = pp
				}
			}
		}
		if err := os.WriteFile(filepath.Join(resolved, name), payload, 0644); err != nil {
			return err
		}
		fmt.Printf("[%d/%d] %s -> %s (%d bytes)\n", i+1, len(list.Maps), m, name, len(payload))
	}
	fmt.Printf("Wrote %d/%d maps to %s (%d failed)\n", len(list.Maps)-failed, len(list.Maps), resolved, failed)
	if failed > 0 {
		return fmt.Errorf("%d map(s) failed", failed)
	}
	return nil
}

func levelLandscapeCmd() *cobra.Command {
	var (
		p, out, csvPath, points string
		grid                    int
		layers                  bool
	)
	cmd := &cobra.Command{
		Use:   "landscape",
		Short: "Sample landscape height / normal / layer weights on a grid or at world points",
		Long: `--grid=N samples an N x N vertex grid over the landscape extent (N <= 1024).
--points="x,y;x,y" samples nearest vertices at world XY (weights always included).
--layers adds per-layer weights in grid mode (cost scales with layer count).
--csv=file additionally writes grid samples as CSV (qx,qy,wx,wy,wz,slope_deg,dominant,<layers...>).`,
		RunE: func(cmd *cobra.Command, args []string) error {
			if (grid > 0) == (points != "") {
				return fmt.Errorf("exactly one of --grid or --points is required")
			}
			params := map[string]interface{}{"path": p, "layers": layers}
			if grid > 0 {
				params["grid"] = grid
			} else {
				pts, err := parsePoints(points)
				if err != nil {
					return err
				}
				params["points"] = pts
			}
			if csvPath == "" {
				if out != "" {
					return callServerToFile("level.landscape", params, out)
				}
				return callServer("level.landscape", params)
			}
			if err := server.EnsureRunning(cfg); err != nil {
				return err
			}
			res, err := rpc.Call(cfg.PipeName, "level.landscape", params)
			if err != nil {
				return err
			}
			f, err := os.Create(normalizeOutPath(csvPath))
			if err != nil {
				return err
			}
			defer f.Close()
			if err := writeLandscapeCSV(f, res); err != nil {
				return err
			}
			if out != "" {
				if err := os.WriteFile(normalizeOutPath(out), res, 0644); err != nil {
					return err
				}
			}
			fmt.Printf("Wrote CSV to %s\n", normalizeOutPath(csvPath))
			return nil
		},
	}
	cmd.Flags().StringVar(&p, "path", "", "Landscape level package, e.g. /Game/Showdown/Maps/Landscape/Sunrise_Landscape (required)")
	cmd.Flags().IntVar(&grid, "grid", 0, "Grid resolution N (N x N samples)")
	cmd.Flags().StringVar(&points, "points", "", "World XY points: x,y;x,y")
	cmd.Flags().BoolVar(&layers, "layers", false, "Include per-layer weights (grid mode)")
	cmd.Flags().StringVar(&out, "out", "", "Write JSON to file")
	cmd.Flags().StringVar(&csvPath, "csv", "", "Also write samples as CSV")
	_ = cmd.MarkFlagRequired("path")
	return cmd
}

// parsePoints parses "x,y;x,y" into pairs.
func parsePoints(s string) ([][2]float64, error) {
	s = strings.TrimSpace(s)
	if s == "" {
		return nil, fmt.Errorf("--points is empty")
	}
	var out [][2]float64
	for _, pair := range strings.Split(s, ";") {
		xy := strings.Split(strings.TrimSpace(pair), ",")
		if len(xy) != 2 {
			return nil, fmt.Errorf("bad point %q (want x,y)", pair)
		}
		x, err := strconv.ParseFloat(strings.TrimSpace(xy[0]), 64)
		if err != nil {
			return nil, fmt.Errorf("bad x in %q: %w", pair, err)
		}
		y, err := strconv.ParseFloat(strings.TrimSpace(xy[1]), 64)
		if err != nil {
			return nil, fmt.Errorf("bad y in %q: %w", pair, err)
		}
		out = append(out, [2]float64{x, y})
	}
	return out, nil
}

// writeLandscapeCSV flattens level.landscape samples (first landscape) to CSV.
func writeLandscapeCSV(w io.Writer, result []byte) error {
	var r struct {
		Success    bool   `json:"success"`
		Error      string `json:"error"`
		Landscapes []struct {
			Samples struct {
				Points []struct {
					QX, QY   int                `json:"-"`
					Qx       int                `json:"qx"`
					Qy       int                `json:"qy"`
					World    map[string]float64 `json:"world"`
					Slope    float64            `json:"slope_deg"`
					Dominant string             `json:"dominant_layer"`
					Weights  map[string]float64 `json:"weights"`
				} `json:"points"`
			} `json:"samples"`
		} `json:"landscapes"`
	}
	if err := json.Unmarshal(result, &r); err != nil {
		return err
	}
	if !r.Success {
		return fmt.Errorf("level.landscape: %s", r.Error)
	}
	if len(r.Landscapes) == 0 {
		return fmt.Errorf("no landscapes in result")
	}
	pts := r.Landscapes[0].Samples.Points
	layerSet := map[string]bool{}
	for _, p := range pts {
		for k := range p.Weights {
			layerSet[k] = true
		}
	}
	var layers []string
	for k := range layerSet {
		layers = append(layers, k)
	}
	sort.Strings(layers)
	fmt.Fprintf(w, "qx,qy,wx,wy,wz,slope_deg,dominant%s\n", joinPrefixed(layers))
	for _, p := range pts {
		fmt.Fprintf(w, "%d,%d,%s,%s,%s,%s,%s", p.Qx, p.Qy, ff(p.World["x"]), ff(p.World["y"]), ff(p.World["z"]), ff(p.Slope), p.Dominant)
		for _, l := range layers {
			fmt.Fprintf(w, ",%s", ff(p.Weights[l]))
		}
		fmt.Fprintln(w)
	}
	return nil
}

func joinPrefixed(ss []string) string {
	if len(ss) == 0 {
		return ""
	}
	return "," + strings.Join(ss, ",")
}

func ff(v float64) string { return strconv.FormatFloat(v, 'f', -1, 64) }
```

Check `go.mod` for the module path and fix the two internal imports; drop the unused `QX, QY` fields.

- [ ] **Step 4: Register** in `main.go` root.AddCommand: add `levelCmd(),` after `animCmd(),`.

- [ ] **Step 5: Run** `go test ./cmd/digbp/ && go vet ./cmd/digbp/` → PASS.

- [ ] **Step 6: Commit** — `git add cmd/digbp && git commit -m "feat(level): digbp level list/tiles/actors/landscape CLI with batch walker + CSV"`

---

### Task 4: `level.landscape`

**Files:**
- Modify: `BlueprintExportLevel.cpp` (replace stub)

**Interfaces:**
- Consumes: `Level_LoadWorld`, `Level_Vec`, `Level_Rot`, `Level_Box`, `Level_PathOrNull`, `Level_UnloadWorld`.
- Produces: `LevelLandscapeToJson(Path, GridN, Points, bLayers, bUnload)` per spec.

- [ ] **Step 1: Implement**

```cpp
namespace
{
	struct FLevel_Sample
	{
		int32 QX, QY;
		uint16 Raw;
		FVector World;
		FVector Normal;
		float SlopeDeg;
		TMap<FName, float> Weights;
	};

	// Fetch a 3-row band [Y-1..Y+1] x [X1..X2] of heights; out-of-extent rows
	// come back as 0 from the edit interface (we clamp the band instead).
	void Level_FetchBand(FLandscapeEditDataInterface& Edit, int32 X1, int32 X2, int32 Y, int32 MinY, int32 MaxY, TArray<uint16>& Out, int32& OutY0, int32& OutRows)
	{
		OutY0 = FMath::Max(MinY, Y - 1);
		const int32 Y1 = FMath::Min(MaxY, Y + 1);
		OutRows = Y1 - OutY0 + 1;
		const int32 W = X2 - X1 + 1;
		Out.SetNumZeroed(W * OutRows);
		Edit.GetHeightDataFast(X1, OutY0, X2, Y1, Out.GetData(), W);
	}

	float Level_RawToLocalZ(uint16 Raw) { return ((float)Raw - 32768.0f) / 128.0f; }
}

TSharedPtr<FJsonObject> UBlueprintExportCommandlet::LevelLandscapeToJson(const FString& Path, int32 GridN, const TArray<FVector2D>& Points, bool bLayers, bool bUnload)
{
	if (GridN > 1024) return Level_MakeError(TEXT("grid too large (max 1024); use two passes or --points"));
	FString Err;
	UWorld* World = Level_LoadWorld(Path, Err);
	if (!World) return Level_MakeError(Err);

	ULandscapeInfo::RecreateLandscapeInfo(World, /*bMapCheck*/false);

	TArray<TSharedPtr<FJsonValue>> Landscapes;
	ULandscapeInfoMap& InfoMap = ULandscapeInfoMap::GetLandscapeInfoMap(World);
	for (auto& Pair : InfoMap.Map)
	{
		ULandscapeInfo* Info = Pair.Value;
		if (!Info) continue;
		ALandscapeProxy* Proxy = Info->GetLandscapeProxy();
		if (!Proxy) continue;

		int32 MinX, MinY, MaxX, MaxY;
		if (!Info->GetLandscapeExtent(MinX, MinY, MaxX, MaxY)) continue;

		const FTransform L2W = Proxy->LandscapeActorToWorld();
		TSharedPtr<FJsonObject> LJ = MakeShareable(new FJsonObject);
		LJ->SetStringField(TEXT("guid"), Info->LandscapeGuid.ToString());
		LJ->SetStringField(TEXT("actor"), Proxy->GetName());
		TArray<TSharedPtr<FJsonValue>> ProxyNames;
		Info->ForAllLandscapeProxies([&](ALandscapeProxy* P) { ProxyNames.Add(MakeShareable(new FJsonValueString(P->GetName()))); });
		LJ->SetArrayField(TEXT("proxies"), ProxyNames);
		TSharedPtr<FJsonObject> TJ = MakeShareable(new FJsonObject);
		TJ->SetObjectField(TEXT("location"), Level_Vec(L2W.GetLocation()));
		TJ->SetObjectField(TEXT("rotation"), Level_Rot(L2W.Rotator()));
		TJ->SetObjectField(TEXT("scale"), Level_Vec(L2W.GetScale3D()));
		LJ->SetObjectField(TEXT("transform"), TJ);
		LJ->SetNumberField(TEXT("component_size_quads"), Info->ComponentSizeQuads);
		LJ->SetNumberField(TEXT("subsection_size_quads"), Info->SubsectionSizeQuads);
		LJ->SetNumberField(TEXT("num_subsections"), Info->ComponentNumSubsections);
		TSharedPtr<FJsonObject> EJ = MakeShareable(new FJsonObject);
		EJ->SetNumberField(TEXT("min_x"), MinX); EJ->SetNumberField(TEXT("min_y"), MinY);
		EJ->SetNumberField(TEXT("max_x"), MaxX); EJ->SetNumberField(TEXT("max_y"), MaxY);
		LJ->SetObjectField(TEXT("extent_quads"), EJ);
		FBox WB(ForceInit);
		WB += L2W.TransformPosition(FVector(MinX, MinY, Level_RawToLocalZ(0)));
		WB += L2W.TransformPosition(FVector(MaxX, MaxY, Level_RawToLocalZ(65535)));
		LJ->SetField(TEXT("world_bounds"), Level_Box(WB));
		LJ->SetNumberField(TEXT("component_count"), Info->XYtoComponentMap.Num());

		TArray<TSharedPtr<FJsonValue>> LayersArr;
		TArray<ULandscapeLayerInfoObject*> LayerObjs;
		for (const FLandscapeInfoLayerSettings& LS : Info->Layers)
		{
			TSharedPtr<FJsonObject> SJ = MakeShareable(new FJsonObject);
			SJ->SetStringField(TEXT("name"), LS.LayerName.ToString());
			SJ->SetField(TEXT("layer_info"), Level_PathOrNull(LS.LayerInfoObj));
			if (LS.LayerInfoObj)
			{
				SJ->SetNumberField(TEXT("hardness"), LS.LayerInfoObj->Hardness);
				SJ->SetBoolField(TEXT("no_weight_blend"), LS.LayerInfoObj->bNoWeightBlend);
				LayerObjs.Add(LS.LayerInfoObj);
			}
			LayersArr.Add(MakeShareable(new FJsonValueObject(SJ)));
		}
		LJ->SetArrayField(TEXT("layers"), LayersArr);

		// --- Sampling ---
		FLandscapeEditDataInterface Edit(Info, /*bUploadTextureChangesToGPU*/false);
		const bool bPointsMode = Points.Num() > 0;
		const bool bWantWeights = bLayers || bPointsMode;
		TArray<FLevel_Sample> Samples;

		// Build list of (QX,QY) to sample, grouped by row for band fetching.
		TMap<int32, TArray<int32>> RowToXs;
		if (bPointsMode)
		{
			for (const FVector2D& P : Points)
			{
				const FVector Local = L2W.InverseTransformPosition(FVector(P.X, P.Y, 0));
				const int32 QX = FMath::Clamp(FMath::RoundToInt(Local.X), MinX, MaxX);
				const int32 QY = FMath::Clamp(FMath::RoundToInt(Local.Y), MinY, MaxY);
				RowToXs.FindOrAdd(QY).Add(QX);
			}
		}
		else
		{
			const int32 N = FMath::Max(GridN, 2);
			for (int32 j = 0; j < N; ++j)
			{
				const int32 QY = MinY + (int32)FMath::RoundToInt((double)(MaxY - MinY) * j / (N - 1));
				TArray<int32>& Xs = RowToXs.FindOrAdd(QY);
				for (int32 i = 0; i < N; ++i)
				{
					Xs.Add(MinX + (int32)FMath::RoundToInt((double)(MaxX - MinX) * i / (N - 1)));
				}
			}
		}
		TArray<int32> Rows; RowToXs.GetKeys(Rows); Rows.Sort();
		const int32 W = MaxX - MinX + 1;
		const FVector Scale = L2W.GetScale3D();
		for (int32 QY : Rows)
		{
			TArray<uint16> Band; int32 Y0, NRows;
			Level_FetchBand(Edit, MinX, MaxX, QY, MinY, MaxY, Band, Y0, NRows);
			auto H = [&](int32 X, int32 Y) -> float
			{
				X = FMath::Clamp(X, MinX, MaxX); Y = FMath::Clamp(Y, Y0, Y0 + NRows - 1);
				return Level_RawToLocalZ(Band[(Y - Y0) * W + (X - MinX)]);
			};
			TMap<ULandscapeLayerInfoObject*, TArray<uint8>> RowWeights;
			if (bWantWeights)
			{
				for (ULandscapeLayerInfoObject* LO : LayerObjs)
				{
					TArray<uint8>& WData = RowWeights.Add(LO);
					WData.SetNumZeroed(W);
					Edit.GetWeightDataFast(LO, MinX, QY, MaxX, QY, WData.GetData(), W);
				}
			}
			for (int32 QX : RowToXs[QY])
			{
				FLevel_Sample S;
				S.QX = QX; S.QY = QY;
				S.Raw = Band[(QY - Y0) * W + (QX - MinX)];
				const float Z = Level_RawToLocalZ(S.Raw);
				S.World = L2W.TransformPosition(FVector(QX, QY, Z));
				// Central difference in world units (quad spacing = Scale.X/Y).
				const float DzDx = (H(QX + 1, QY) - H(QX - 1, QY)) * Scale.Z / (2.0f * Scale.X);
				const float DzDy = (H(QX, QY + 1) - H(QX, QY - 1)) * Scale.Z / (2.0f * Scale.Y);
				S.Normal = FVector(-DzDx, -DzDy, 1.0f).GetSafeNormal();
				S.SlopeDeg = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(S.Normal.Z, -1.0f, 1.0f)));
				if (bWantWeights)
				{
					for (auto& WP : RowWeights) S.Weights.Add(WP.Key->LayerName, WP.Value[QX - MinX] / 255.0f);
				}
				Samples.Add(MoveTemp(S));
			}
		}

		TArray<TSharedPtr<FJsonValue>> PtsArr;
		for (const FLevel_Sample& S : Samples)
		{
			TSharedPtr<FJsonObject> PJ = MakeShareable(new FJsonObject);
			PJ->SetNumberField(TEXT("qx"), S.QX);
			PJ->SetNumberField(TEXT("qy"), S.QY);
			PJ->SetObjectField(TEXT("world"), Level_Vec(S.World));
			PJ->SetNumberField(TEXT("height_raw"), S.Raw);
			PJ->SetObjectField(TEXT("normal"), Level_Vec(S.Normal));
			PJ->SetNumberField(TEXT("slope_deg"), S.SlopeDeg);
			if (S.Weights.Num() > 0)
			{
				FName Dominant = NAME_None; float Best = -1.0f;
				TSharedPtr<FJsonObject> WJ = MakeShareable(new FJsonObject);
				for (auto& WP : S.Weights)
				{
					WJ->SetNumberField(WP.Key.ToString(), WP.Value);
					if (WP.Value > Best) { Best = WP.Value; Dominant = WP.Key; }
				}
				PJ->SetStringField(TEXT("dominant_layer"), Dominant.ToString());
				PJ->SetObjectField(TEXT("weights"), WJ);
			}
			PtsArr.Add(MakeShareable(new FJsonValueObject(PJ)));
		}
		TSharedPtr<FJsonObject> SJ = MakeShareable(new FJsonObject);
		SJ->SetStringField(TEXT("mode"), bPointsMode ? TEXT("points") : TEXT("grid"));
		if (!bPointsMode) SJ->SetNumberField(TEXT("grid"), FMath::Max(GridN, 2));
		SJ->SetNumberField(TEXT("count"), PtsArr.Num());
		SJ->SetArrayField(TEXT("points"), PtsArr);
		LJ->SetObjectField(TEXT("samples"), SJ);
		Landscapes.Add(MakeShareable(new FJsonValueObject(LJ)));
	}

	if (Landscapes.Num() == 0)
	{
		if (bUnload) Level_UnloadWorld(World);
		return Level_MakeError(FString::Printf(TEXT("No landscape found in %s"), *Path));
	}
	TSharedPtr<FJsonObject> Out = MakeShareable(new FJsonObject);
	Out->SetBoolField(TEXT("success"), true);
	Out->SetStringField(TEXT("path"), World->GetOutermost()->GetName());
	Out->SetArrayField(TEXT("landscapes"), Landscapes);
	if (bUnload) Level_UnloadWorld(World);
	return Out;
}
```

Verify before committing (grep the 4.27 headers): `ULandscapeInfoMap::GetLandscapeInfoMap(UWorld*)` + `.Map`, `ULandscapeInfo::ComponentNumSubsections`, `ALandscapeProxy::LandscapeActorToWorld()`, `ULandscapeLayerInfoObject::LayerName`. Add `#include "LandscapeInfoMap.h"`.

- [ ] **Step 2: Commit** — `git commit -am "feat(level): level.landscape — grid/point height, normal, slope, layer weights"`

---

### Task 5: Docs, ship, acceptance

**Files:**
- Modify: `CLAUDE.md` (digbp commands section, after the anim block; project-structure tree; guidelines bullet)
- Modify: `claude-code-skills/blueprint-export/SKILL.md` (mirror)

- [ ] **Step 1: CLAUDE.md** — add after the anim mutation block:

```bash
# Level / World Composition / landscape inspection (read-only). Loads the map
# package headless (no world init/streaming); WC tiles come from package
# summaries. Tile actors are tile-local as authored; world_* adds the tile's
# absolute offset.
digbp level list --dir=/Game/Showdown/Maps/Sunrise/Sunrise_SubLevels
digbp level tiles --path=/Game/Showdown/Maps/Sunrise/Sunrise --pretty          # layer, position, bounds, LODs, streaming class per tile
digbp level actors --path=/Game/Showdown/Maps/Sunrise/Sunrise_SubLevels/Tile_X3_Y2 --pretty
digbp level actors --path=... --class=Volume,BP_LootGenerator --bounds-only      # class filter matches BP parents + native ancestors
digbp level actors --path=... --instances                                        # per-instance ISM/HISM transforms
digbp level actors --dir=/Game/Showdown/Maps/Sunrise/Sunrise_SubLevels --out=E:/tmp/sunrise_actors   # one JSON per map, server unloads each
digbp level landscape --path=/Game/Showdown/Maps/Landscape/Sunrise_Landscape --grid=64 --layers --csv=E:/tmp/sunrise_hm.csv
digbp level landscape --path=/Game/Showdown/Maps/Landscape/Sunrise_Landscape --points="12000,-3400;0,0"   # world XY → nearest vertex
```

Project-structure tree: add `BlueprintExportLevel.cpp   # Level/WC tiles/actors/landscape export` and `level.go`.

Guidelines bullet: "Level packages load with `LoadPackage` + `UWorld::FindWorldInPackage` only — no `InitWorld`, no streaming. `USceneComponent::UpdateComponentToWorld` + `CalcBounds` work on unregistered components; `ULandscapeInfo::RecreateLandscapeInfo(World,false)` is required before `FLandscapeEditDataInterface`. World Composition tile actors are stored tile-local; apply `FWorldTileInfo` parent-chain positions for world space."

- [ ] **Step 2: SKILL.md** — mirror the command block in the skill's digbp command list.

- [ ] **Step 3: Ship** — `just sync` then `just build`. Reply to spyder #1835: CL number, usage block, and that the editor must be rebuilt (plugin sync doesn't compile).

- [ ] **Step 4: Acceptance (after gamedev's editor build)** — `digbp version`; `digbp level tiles --path=/Game/Showdown/Maps/Sunrise/Sunrise --pretty | head`; one tile `level actors`; `level landscape --grid=4`. Record results in memory `project_level_export_shipped.md`.

- [ ] **Step 5: Commit docs** — `git commit -am "docs(level): digbp level usage"`.
