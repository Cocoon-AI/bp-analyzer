# `digbp level` — read-only level / world-composition / landscape export

**Date:** 2026-08-22
**Requester:** gamedev (spyder #1835) — procedural gold-deposit placement research for Sunrise.
**Status:** approved by standing pre-authorization for gamedev bp-analyzer requests.

## Goal

Give AI tooling a headless, JSON view of how a UE4.27 World-Composition map is
built: the tile layout, the actors inside any level package, and sampled
landscape height/weight data. Read-only; nothing is saved or mutated.

Priority order (ship in this order, each independently useful):

1. `level tiles`
2. `level actors` (+ `--dir` batch form)
3. `level landscape`

## Non-goals

- Any mutation of levels/landscape.
- Streaming sublevels into the persistent world (we load packages, not worlds).
- PNG export of landscape data (CSV only; PNG deferred — gamedev marked it optional).
- World Partition / UE5 concepts.

## Architecture

Follows the `anim export` precedent exactly:

```
digbp level <sub> --flags   →  JSON-RPC level.<method>  →  UBlueprintExportCommandlet::Level*ToJson  →  JSON
```

| Piece | File |
|---|---|
| C++ export | `Private/BlueprintExportLevel.cpp` (new; `Level_` prefix on file-local helpers — unity build) |
| Declarations | `Private/BlueprintExportCommandlet.h` (new `// --- Level export ---` block) |
| RPC dispatch | `Private/BlueprintExportServer.cpp` (new `level.*` block after `anim.*`) |
| Build deps | `BlueprintAnalyzer.Build.cs`: add `Landscape`, `Foliage` (editor branch) |
| CLI | `cmd/digbp/level.go` (new), registered in `main.go` |
| Docs | `CLAUDE.md`, `claude-code-skills/blueprint-export/SKILL.md` |

### Loading model

Every command takes a **level package path** (`/Game/.../Sunrise`) and does
`LoadPackage(nullptr, *Path, LOAD_None)` + `UWorld::FindWorldInPackage`. No
world initialization, no actor registration, no streaming. This is what the
editor does when opening a sublevel standalone, and every field we emit is
available from serialized data:

- World Composition tiles: `UWorldComposition::PostInitProperties` runs
  `Rescan()` in the editor (showdown-patched engine; stock 4.27 does the
  same), which reads every tile's `FWorldTileInfo` from the package summary
  via `FWorldTileInfo::Read` — **tiles are never fully loaded**.
- Actor transforms: `USceneComponent::UpdateComponentToWorld()` is safe on
  unregistered components; bounds come from `CalcBounds(ComponentToWorld)`
  (StaticMesh → mesh bounds; HISM → serialized cluster tree; Landscape →
  `CachedLocalBox`; Brush → serialized brush).
- Landscape data: `ULandscapeInfo::RecreateLandscapeInfo(World, false)` then
  `FLandscapeEditDataInterface(Info, /*upload*/false)` reads heightmap /
  weightmap **texture source** data (editor-only, on disk).

**Memory / batch:** an optional `unload` param (default `false`; the CLI
batch form passes `true`) calls `UPackageTools::UnloadPackages` on the
loaded map package after export so a 537-map walk doesn't accumulate worlds
in the server. Single-shot calls keep the package resident (repeat queries
are then instant).

### Tile-local vs world coordinates

World Composition stores tile actors in **tile-local** coordinates; the
tile's absolute offset is applied at streaming time. `level actors` therefore:

- reads the level's own `FWorldTileInfo` (if the package has one) and walks
  `ParentTilePackageName` to compute `absolute_position`;
- emits each actor's stored `location`/`bounds` **and** `world_location` /
  `world_bounds` = stored + `absolute_position`.

For a non-composition level both are equal and `tile` is `null`.

## Commands

### 1. `digbp level tiles --path=<persistent map> [--out=f]`  → `level.tiles`

```json
{
  "success": true,
  "path": "/Game/Showdown/Maps/Sunrise/Sunrise",
  "world_composition": true,
  "world_root": "/Game/Showdown/Maps/Sunrise/",
  "persistent_level": { "actor_count": 12, "actors": [ ...same shape as level.actors, bounds_only... ] },
  "tile_count": 537,
  "layers": [ { "name": "Interiors", "streaming_distance": 50000, "distance_streaming_enabled": true, "tile_count": 120 } ],
  "tiles": [
    {
      "package": "/Game/Showdown/Maps/Sunrise/Sunrise_SubLevels/Tile_X3_Y2",
      "short_name": "Tile_X3_Y2",
      "parent_package": "/Game/.../Sunrise_Landscape" | null,
      "layer": { "name": "...", "streaming_distance": 50000, "distance_streaming_enabled": true },
      "position": { "x": 0, "y": 0, "z": 0 },
      "absolute_position": { "x": 0, "y": 0, "z": 0 },
      "bounds": { "min": {x,y,z}, "max": {x,y,z}, "size": {x,y,z} },
      "z_order": 0,
      "hide_in_tile_view": false,
      "lods": [ { "index": 1, "package": "/Game/..._LOD1", "relative_streaming_distance": 10000, "streaming_distance": 60000 } ],
      "streaming_class": "LevelStreamingDynamic" | "LevelStreamingAlwaysLoaded" | null,
      "distance_dependent": true
    }
  ]
}
```

`streaming_class` comes from `TilesStreaming[i]->GetClass()->GetName()` when
`PopulateStreamingLevels` has run (it does on Rescan); `distance_dependent` is
`UWorldComposition::IsDistanceDependentLevel(PackageName)`.
If `World->WorldComposition == nullptr` → `world_composition:false`, `tiles:[]`,
persistent actors still emitted (still useful for plain maps).

### 2. `digbp level actors --path=<map> [--class=A,B] [--bounds-only] [--instances] [--out=f]` → `level.actors`

Also: `digbp level actors --dir=<folder> --out=<dir> [same filters]` — CLI
calls `level.list` (asset registry, `World` class under `--dir`, recursive)
then `level.actors` per map with `unload:true`, writing `<out>/<ShortName>.json`
and printing a one-line progress/summary. Per-map failures are reported and
skipped, not fatal.

```json
{
  "success": true,
  "path": "/Game/.../Tile_X3_Y2",
  "tile": { "position": {...}, "absolute_position": {...}, "bounds": {...}, "layer": {...}, "parent_package": "..." } | null,
  "actor_count": 40,
  "actors": [
    {
      "name": "SM_Rock_12", "label": "SM_Rock_12",
      "class": "StaticMeshActor", "class_path": "/Script/Engine.StaticMeshActor",
      "native_class": "StaticMeshActor",
      "blueprint_chain": ["/Game/.../BP_LootGen_Child.BP_LootGen_Child_C", "/Game/.../BP_LootGen.BP_LootGen_C"],
      "folder": "Props/Rocks", "tags": ["gold"],
      "location": {x,y,z}, "rotation": {pitch,yaw,roll}, "scale": {x,y,z},
      "world_location": {x,y,z},
      "bounds": { "min":{}, "max":{}, "size":{} } | null,
      "world_bounds": { ... } | null,
      "components": [   // omitted with --bounds-only
        { "name": "StaticMeshComponent0", "class": "StaticMeshComponent",
          "relative_location": {...}, "relative_rotation": {...}, "relative_scale": {...},
          "static_mesh": "/Game/.../SM_Rock.SM_Rock", "materials": ["/Game/.../M_Rock.M_Rock"],
          "bounds": {...} },
        { "name": "HISM", "class": "HierarchicalInstancedStaticMeshComponent", "static_mesh": "...",
          "materials": [...], "instance_count": 812,
          "instances": [ {location, rotation, scale}, ... ]   // only with --instances (world-space per-instance transforms, tile-local) },
        { "name": "ChildActor", "class": "ChildActorComponent", "child_actor_class": "/Game/.../BP_LootGenerator.BP_LootGenerator_C" },
        { "name": "Spline", "class": "SplineComponent", "closed_loop": false,
          "points": [ { "location": {...}, "arrive_tangent": {...}, "leave_tangent": {...}, "type": "Curve" } ] },
        { "name": "BrushComponent0", "class": "BrushComponent", "bounds": {...} },
        { "name": "LandscapeComponent_3_2", "class": "LandscapeComponent", "section_base": {x,y}, "bounds": {...} }
      ],
      // type-specific summaries (present only when applicable):
      "landscape": { "component_count": 64, "component_size_quads": 63, "subsection_size_quads": 63, "num_subsections": 1,
                     "landscape_guid": "...", "layers": ["Grass","Soil"], "section_base_min": {x,y}, "section_base_max": {x,y} },
      "foliage": [ { "foliage_type": "/Game/.../FT_Grass.FT_Grass", "static_mesh": "/Game/.../SM_Grass", "instance_count": 5400 } ],
      "volume": { "class": "NavMeshBoundsVolume", "bounds": {...} }
    }
  ]
}
```

Component positions are reported relative (as authored) plus the actor
transform; `instances` are in component space transformed into actor/tile
space (`GetInstanceTransform(i, /*WorldSpace*/true)` — valid after
`UpdateComponentToWorld`).

`--class` filter matches the actor's class name, any class in its
`blueprint_chain`, or any native ancestor (so `--class=Volume` catches every
volume, `--class=BP_LootGenerator` catches child BPs).

Landscape actors: a proxy's `LandscapeComponent` entries are summarized in
`landscape` and **not** listed one-by-one in `components` unless `--class`
explicitly names `Landscape*` (keeps tile JSON small).

### 3. `digbp level landscape --path=<map> (--grid=N | --points=x,y;x,y) [--layers] [--out=f] [--csv=f]` → `level.landscape`

Loads the map, `RecreateLandscapeInfo`, and for each `ULandscapeInfo` in the
world (normally one):

```json
{
  "success": true,
  "path": "/Game/Showdown/Maps/Landscape/Sunrise_Landscape",
  "landscapes": [
    {
      "guid": "...", "actor": "Landscape_1", "proxies": ["Landscape_1"],
      "transform": { location, rotation, scale },               // LandscapeActorToWorld
      "component_size_quads": 63, "subsection_size_quads": 63, "num_subsections": 1,
      "extent_quads": { "min_x":0, "min_y":0, "max_x":4032, "max_y":4032 },
      "world_bounds": { min, max, size },
      "component_count": 4096,
      "layers": [ { "name": "Grass", "layer_info": "/Game/.../Grass_LayerInfo", "hardness": 0.5, "no_weight_blend": false } ],
      "samples": {
        "mode": "grid" | "points",
        "grid": N,                       // grid mode
        "points": [
          { "qx": 0, "qy": 0,           // landscape quad coords (nearest vertex)
            "world": {x,y,z},           // z = sampled height in world units
            "height_raw": 32768,        // uint16
            "normal": {x,y,z}, "slope_deg": 12.3,
            "dominant_layer": "Grass",
            "weights": { "Grass": 0.8, "Soil": 0.2 }   // 0..1, only with --layers (or always in points mode)
          }
        ]
      }
    }
  ]
}
```

- **grid mode:** N×N vertices evenly spaced over `extent_quads` (inclusive of
  both edges). Heights are fetched per grid row as a 3-row band
  (`GetHeightDataFast(minX, y-1, maxX, y+1)`) so memory stays O(width), and
  the normal is a central-difference over ±1 quad scaled by `DrawScale`.
  Weights via `GetWeightDataFast(LayerInfo, ...)` per layer per row, only
  when `--layers` (layer reads multiply cost by layer count).
- **points mode:** `--points=x,y;x,y` are **world** XY; inverse-transform
  into quad space, round to nearest vertex, 3×3 fetch. Weights always on.
- Height → local Z: `(raw - 32768) / 128`; world via landscape transform.
- `--csv=f` (CLI-side, grid mode): writes `qx,qy,wx,wy,wz,slope_deg,dominant,<layer weights...>` rows from the JSON.
- Grid cap: N ≤ 1024 (1M samples); larger → error suggesting `--out` + two passes. Not silent.

### `level.list` (internal)

`{ "dir": "/Game/..." }` → `{ "maps": ["/Game/.../A", ...] }` via asset
registry class filter `World`. Exposed as `digbp level list --dir=` for
convenience.

## Error handling

Same envelope as anim: `{ "success": false, "error": "..." }` for: package
not found / not a map, no World in package, bad grid/points, no landscape in
map, unknown class filter (warning, not error — empty result). Server stays
up on every failure (no `check`s on user input).

## Testing

No UE automation tests exist in this repo; verification is acceptance-style
against steamdev content via the pipe server, same as anim:

1. `level tiles` on Sunrise: `tile_count` matches `ls Sunrise_SubLevels | wc`
   (537 .umap incl. LODs — compare after LOD folding), a known tile's layer
   and position match the editor's Levels/World Composition panel.
2. `level actors` on one interior tile: ChildActorComponent `child_actor_class`
   resolves to the loot generator BP; a StaticMeshActor's `world_bounds`
   matches editor-reported bounds + tile offset.
3. `--dir` batch over `Sunrise_SubLevels` completes, server memory returns
   to baseline after (unload works), one JSON per map.
4. `level landscape --grid=8`: corner samples' world Z matches editor
   landscape height at those points; `--points` at one known location
   matches; weights sum ≈ 1 where painted.

Go side: unit test for `--points` parsing and CSV writer (pure functions).

## Rollout

`just sync` (plugin → D:/sd/steamdev, auto-submits a CL) + `just build`
(CLI → C:/tools). Tell gamedev they must rebuild the editor (plugin sync
doesn't compile). Reply on spyder thread #1835 with CL + usage per phase.
