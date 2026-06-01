@0xd8a46e2b8a831f55;

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("mx::map::schema");

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
}
