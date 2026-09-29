@0xd8a46e2b8a831f55;

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("mx::map::schema");

# World package manifest. Format contract: docs/map-data-format.md.
#
# formatVersion 2 (legacy, read-only transition): fields @0..@11 only; the
#   chunk grid is ceil(worldSizeCells / chunkSizeCells) per axis and
#   zoneGridDims / zoneSizeCells are NOT chunk or zone information for the
#   server (ignored; historically the generator wrote the chunk count there).
# formatVersion 3 (current): adds @12..@17 below. zoneGridDims and
#   zoneSizeCells must be zero (the map format carries no server-zone
#   concept), worldLogicFile / environmentFile must be empty (references come
#   from `layers`). New fields are appended only, so a v2 reader parses a v3
#   message without error -- it must still refuse formatVersion != 2.

enum HeightUnit {
  centimeters @0;
}

struct TexturePaletteEntry {
  id @0 :UInt16;
  path @1 :Text;
}

struct ZoneGridDims {
  x @0 :UInt32;
  y @1 :UInt32;
}

struct GridDims {
  x @0 :UInt32;
  y @1 :UInt32;
}

struct WorldOrigin {
  x @0 :Float64;
  y @1 :Float64;
}

# Kinds a package can declare. Chunk-section kinds live inside the chunk
# files (declaration has no `file`); file kinds reference one package file.
enum LayerKind {
  height @0;       # chunk section 1, int16 cm, (n+1)^2 samples
  attributes @1;   # chunk section 3, uint16 bitfield, n^2 cells
  splatA @2;       # chunk section 2, RGBA8 (client render data)
  splatB @3;       # chunk section 4, RGBA8 (client render data)
  worldLogic @4;   # MXL1 binary: bootstrap zones, spawn regions, warps
  mobSpawns @5;    # text spawn table (mob_spawns format v1)
  water @6;        # MXWB water bodies (client render data; the server does not read it)
  waterBodies @7;  # MXWS server water bodies (MAP-3; with water.model = bodies)
}

enum LayerAudience {
  server @0;
  client @1;
  shared @2;
}

struct LayerDecl {
  kind @0 :LayerKind;
  required @1 :Bool;
  audience @2 :LayerAudience;
  version @3 :UInt32;   # per-layer encoding version (1 for every kind today)
  file @4 :Text;        # package-relative, '/' separated; empty for chunk sections
}

struct ChunkRef {
  x @0 :UInt32;
  y @1 :UInt32;
  file @2 :Text;        # package-relative, '/' separated
  byteSize @3 :UInt64;  # exact file size
  crc32 @4 :UInt32;     # CRC-32 (IEEE 802.3, zlib crc32) of the whole file
}

# Height layer version 2 (MAP-2 range extension): meters = offsetMeters +
# raw * metersPerUnit, raw stored as the chosen sample type. Height layer
# version 1 is fixed: int16, 0.01 m per unit, offset 0 (field unset).
enum HeightSampleType {
  int16 @0;
  int32 @1;
}

struct HeightEncoding {
  sampleType @0 :HeightSampleType;
  metersPerUnit @1 :Float64;
  offsetMeters @2 :Float64;
}

# Water capability (MAP-3). Undeclared (the field absent or model =
# undeclared) means UNKNOWN: server water queries answer UnsupportedLayer,
# never "land".
enum WaterModel {
  undeclared @0;
  none @1;        # the world has no water
  seaLevel @2;    # one global water surface at seaLevelMeters
  bodies @3;      # local water bodies from the waterBodies layer
}

struct WaterDecl {
  model @0 :WaterModel;
  seaLevelMeters @1 :Float64;
}

struct MapManifest {
  formatVersion @0 :UInt32;
  worldId @1 :Text;
  worldName @2 :Text;
  worldSizeCells @3 :UInt32;     # cells along X
  cellSizeMeters @4 :Float32;
  heightUnit @5 :HeightUnit;
  chunkSizeCells @6 :UInt32;
  zoneGridDims @7 :ZoneGridDims; # legacy (v2 ignored, v3 must be 0)
  zoneSizeCells @8 :UInt32;      # legacy (v2 ignored, v3 must be 0)
  texturePalette @9 :List(TexturePaletteEntry); # client render data
  worldLogicFile @10 :Text;      # legacy (v2: empty = "worldlogic.dat"; v3 must be empty)
  environmentFile @11 :Text;     # legacy (unused; v3 must be empty)
  worldSizeCellsY @12 :UInt32;   # v3: cells along Y
  origin @13 :WorldOrigin;       # v3: world coordinate of cell (0,0)'s corner
  chunkGrid @14 :GridDims;       # v3: chunk count per axis
  layers @15 :List(LayerDecl);   # v3
  chunks @16 :List(ChunkRef);    # v3: exactly one entry per chunk grid cell
  heightEncoding @17 :HeightEncoding; # v3, height layer version 2 only
  water @18 :WaterDecl;          # v3 (MAP-3): the water capability of the world
}
