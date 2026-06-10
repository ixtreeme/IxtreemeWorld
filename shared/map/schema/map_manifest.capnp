@0xd8a46e2b8a831f55;

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("mx::map::schema");

enum HeightUnit {
  centimeters @0;
}

struct TexturePaletteEntry {
  id @0 :UInt16;
  path @1 :Text;
  tilingX @2 :Float32 = 1.0;
  tilingY @3 :Float32 = 1.0;
  normalStrength @4 :Float32 = 1.0;
  roughnessStrength @5 :Float32 = 1.0;
  tintR @6 :Float32 = 1.0;
  tintG @7 :Float32 = 1.0;
  tintB @8 :Float32 = 1.0;
  metallicStrength @9 :Float32 = 0.0;
  aoStrength @10 :Float32 = 1.0;
  uvOffsetX @11 :Float32 = 0.0;
  uvOffsetY @12 :Float32 = 0.0;
  uvRotationDegrees @13 :Float32 = 0.0;
}

struct ZoneGridDims {
  x @0 :UInt32;
  y @1 :UInt32;
}

struct MapManifest {
  formatVersion @0 :UInt32;
  worldId @1 :Text;
  worldName @2 :Text;
  worldSizeCells @3 :UInt32;
  cellSizeMeters @4 :Float32;
  heightUnit @5 :HeightUnit;
  chunkSizeCells @6 :UInt32;
  zoneGridDims @7 :ZoneGridDims;
  zoneSizeCells @8 :UInt32;
  texturePalette @9 :List(TexturePaletteEntry);
  worldLogicFile @10 :Text;
  environmentFile @11 :Text;
  triplanarSlopeThreshold @12 :Float32 = 0.18;
  triplanarSlopeTransition @13 :Float32 = 0.20;
}
