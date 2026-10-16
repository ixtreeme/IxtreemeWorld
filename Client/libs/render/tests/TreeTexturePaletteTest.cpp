#include "tools/tree/TreeTexturePalette.h"
#include <stb_image.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace
{
namespace fs = std::filesystem;
int failures = 0;
void Check(bool condition, const char* message)
{
    if (!condition)
    {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}
struct Workspace
{
    fs::path parent = fs::weakly_canonical(fs::temp_directory_path());
    fs::path root = parent / ("ixw_tree_palette_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Workspace() { fs::create_directory(root); }
    ~Workspace()
    {
        if (root.parent_path() == parent && root.filename().string().starts_with("ixw_tree_palette_"))
        {
            std::error_code error;
            fs::remove_all(root, error);
        }
    }
};
struct Image
{
    int width = 0, height = 0;
    std::vector<unsigned char> pixels;
    explicit Image(const fs::path& path)
    {
        int channels = 0;
        auto* decoded = stbi_load(path.string().c_str(), &width, &height, &channels, 4);
        if (!decoded)
            throw std::runtime_error("Cannot decode " + path.string());
        pixels.assign(decoded, decoded + width * height * 4);
        stbi_image_free(decoded);
    }
    int At(int x, int y, int channel) const { return pixels[(y * width + x) * 4 + channel]; }
};
std::vector<char> Bytes(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}
void Test()
{
    Workspace workspace;
    auto& palette = tree_tool::TreeTexturePalette::Instance();
    palette.EnsureLoaded(workspace.root);
    using ixtreemetree::LeafType;
    using ixtreemetree::BarkType;
    const auto palmPath = palette.Leaf(LeafType::Palm).path;
    const auto blossomPath = palette.Leaf(LeafType::Blossom).path;
    const auto snowyPath = palette.Leaf(LeafType::SnowyPine).path;
    const auto barkPath = palette.Bark(BarkType::Palm).path;
    Check(palette.Leaf(LeafType::Palm).gridX == 2 && palette.Leaf(LeafType::Palm).gridY == 1,
        "palm has two tall atlas cells");
    for (const auto& path : {palmPath, blossomPath, snowyPath, barkPath})
    {
        const Image image(path);
        const int expected = path == barkPath ? 256 : 512;
        Check(image.width == expected && image.height == expected, "texture has the intended size");
        const auto relative = fs::relative(path, workspace.root);
        Check(Bytes(path) == Bytes(fs::path(IXTREEME_TREE_INTERNAL_DIR) / relative),
            "committed texture matches the procedural generator");
    }
    const Image palm(palmPath);
    for (int cell = 0; cell < 2; ++cell)
    {
        Check(palm.At(cell * 256 + 128, 495, 3) > 200, "palm has a midrib at its base");
        Check(palm.At(cell * 256 + 32, 495, 3) == 0, "palm's stalk is bare at its base");
        int visible = 0, gaps = 0;
        for (int y = 70; y < 430; ++y)
            (palm.At(cell * 256 + 180, y, 3) > 128 ? visible : gaps)++;
        Check(visible > 30 && gaps > 30, "palm has distinct leaflets and gaps");
    }
    const Image blossom(blossomPath);
    int pink = 0, pale = 0, transparent = 0;
    for (std::size_t i = 0; i < blossom.pixels.size(); i += 4)
    {
        const auto* p = blossom.pixels.data() + i;
        if (p[3] < 128) { ++transparent; continue; }
        pink += p[0] > p[1] + 25 && p[2] > p[1];
        pale += p[0] > 240 && p[1] > 195 && p[2] > 210;
    }
    Check(pink > 1000 && pale > 1000 && transparent > 100000, "blossoms have pink and pale petals with cutout gaps");
    const Image pine(palette.Leaf(LeafType::Pine).path);
    const Image snowy(snowyPath);
    double top = 0, bottom = 0;
    int topCount = 0, bottomCount = 0;
    bool sameSilhouette = true;
    for (int y = 0; y < 512; ++y)
        for (int x = 0; x < 512; ++x)
        {
            sameSilhouette &= pine.At(x, y, 3) == snowy.At(x, y, 3);
            if (snowy.At(x, y, 3) < 128) continue;
            if (y % 256 < 110) { top += snowy.At(x, y, 0); ++topCount; }
            if (y % 256 > 146) { bottom += snowy.At(x, y, 0); ++bottomCount; }
        }
    Check(sameSilhouette, "snow preserves the pine silhouette");
    Check(topCount > 0 && bottomCount > 0 && top / topCount > bottom / bottomCount + 60,
        "snow lies on the upper needles");
    const Image bark(barkPath);
    double darkRow = 0, lightRow = 0;
    for (int x = 0; x < 256; ++x) { darkRow += bark.At(x, 1, 0); lightRow += bark.At(x, 12, 0); }
    Check(lightRow > darkRow + 256 * 30, "palm bark has horizontal rings");
}
}
int main(int argc, char** argv)
{
    try
    {
        if (argc == 2 && std::string(argv[1]) == "--generate-assets")
            tree_tool::TreeTexturePalette::Instance().EnsureLoaded(IXTREEME_TREE_INTERNAL_DIR);
        else
            Test();
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
    if (!failures) std::cout << "TreeTexturePaletteTest: all checks passed\n";
    return failures ? 1 : 0;
}
