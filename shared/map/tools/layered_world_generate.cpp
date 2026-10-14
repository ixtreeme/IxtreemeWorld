#include "map/LayeredWorldGenerator.h"

#include <fstream>
#include <iostream>
#include <string>

namespace {

void Usage()
{
    std::cerr << "usage: layered_world_generate <surfaces.txt> <min_x> <min_y> <max_x> <max_y> <output.mx3d>\n"
                 "record: surface id name min_x min_y max_x max_y min_z max_z tag|tag 0|1\n";
}

bool Number(const char* text, float& value)
{
    try {
        std::size_t used = 0;
        value = std::stof(text, &used);
        return used == std::string(text).size();
    } catch (...) {
        return false;
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 7) {
        Usage();
        return 2;
    }
    mx::map::Rect bounds;
    if (!Number(argv[2], bounds.min_x) || !Number(argv[3], bounds.min_y) ||
        !Number(argv[4], bounds.max_x) || !Number(argv[5], bounds.max_y) ||
        !(bounds.min_x < bounds.max_x && bounds.min_y < bounds.max_y)) {
        std::cerr << "invalid world bounds\n";
        return 2;
    }

    std::vector<mx::map::LayerSourceSurface> surfaces;
    std::vector<std::string> source_errors;
    if (!mx::map::ReadLayeredSurfaceSource(argv[1], surfaces, source_errors)) {
        for (const auto& error : source_errors) std::cerr << error << '\n';
        return 1;
    }
    mx::map::LayeredWorld world;
    mx::map::LayerGenerationReport report;
    if (!mx::map::GenerateLayeredWorld(surfaces, mx::map::LayerGenerationOptions{bounds}, world, report)) {
        for (const auto& error : report.errors) std::cerr << error << '\n';
        return 1;
    }
    const auto bytes = mx::map::EncodeLayeredWorld(world);
    if (bytes.empty()) {
        std::cerr << "generated sidecar is empty or exceeds encoding limits\n";
        return 1;
    }
    std::ofstream output(argv[6], std::ios::binary | std::ios::trunc);
    if (!output) {
        std::cerr << "cannot open output " << argv[6] << '\n';
        return 1;
    }
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!output) {
        std::cerr << "cannot write output " << argv[6] << '\n';
        return 1;
    }
    std::cout << "generated volumes=" << report.volumes_generated << " surfaces=" << report.surfaces_seen
              << " merged=" << report.surfaces_merged << " output=" << argv[6] << '\n';
    return 0;
}
