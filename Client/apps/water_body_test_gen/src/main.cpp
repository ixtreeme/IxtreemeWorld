#include "WaterBodyIO.h"

#include <filesystem>
#include <iostream>
#include <string>

int main(int argc, char** argv)
{
    try
    {
        if (argc < 2 || std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")
        {
            std::cout << "Usage: IwWaterBodyTestGen <map-root-directory>\n"
                      << "Writes water_bodies.mxwater with three static test water bodies.\n";
            return argc < 2 ? 1 : 0;
        }

        const std::filesystem::path mapRoot = argv[1];
        const std::filesystem::path outPath = mapRoot / client::render::kWaterBodiesFilename;
        std::string error;
        const auto bodies = client::render::CreateWaterBodyTestSet();
        if (!client::render::SaveWaterBodiesBinary(outPath, bodies, &error))
        {
            std::cerr << "[WATER-OBJ] failed: " << error << "\n";
            return 2;
        }

        std::cout << "[WATER-OBJ] wrote " << bodies.size()
                  << " test water bodies to " << outPath.generic_string() << "\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "[WATER-OBJ] exception: " << exception.what() << "\n";
        return 3;
    }
}
