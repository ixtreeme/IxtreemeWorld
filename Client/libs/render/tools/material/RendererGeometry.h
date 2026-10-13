#pragma once

#include "import_export/AssimpImporter.h"
#include <filesystem>

namespace material_tool {
// A static model's geometry exactly as the static renderer draws it. glTF files are read the way
// the renderer reads them: the file's own positions, normals, UVs and winding, one submesh per
// primitive in file order. Other formats come from the importer the renderer uses for them. Each
// submesh's materialIndex is the material slot it is drawn with, and materials are in slot order.
AssimpImporter::ImportResult ImportRendererGeometry(const std::filesystem::path& path);
}
