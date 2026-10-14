#include "SceneManager.h"
#include "asset/AssetDatabase.h"
#include "asset/AssetLibrary.h"
#include "asset/MaterialAssetManager.h"
#include "import_export/AssimpImporter.h"
#include "import_export/tree/TreeGlbExporter.h"
#include <ixtreemetree/tree.h>
#include <ixtreemetree/preset.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace fs = std::filesystem;
fs::path Utf8Path(const std::string& value) { return fs::path(std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size())); }
void Require(bool condition, const std::string& message) { if (!condition) throw std::runtime_error(message); }
template<class T> std::vector<T> Read(const fs::path& path, size_t count)
{
    std::ifstream in(path, std::ios::binary); std::vector<T> values(count);
    Require(bool(in.read(reinterpret_cast<char*>(values.data()), count * sizeof(T))), "Cannot read " + path.string());
    return values;
}
int main(int argc, char** argv)
{
    try {
        std::cout << std::unitbuf;
        Require(argc == 3, "Usage: BlackwaterHollowBuilder <project-root> <staging-dir>");
        const fs::path root = fs::absolute(argv[1]), staging = fs::absolute(argv[2]);
        Require(fs::exists(root / "project.ixproj"), "An existing project is required");
        auto& db = AssetDatabase::Instance(); db.scan(root);
        const fs::path engine = BWH_ENGINE_ROOT;
        const char* presets[] = {"pine_medium", "pine_small", "oak_medium", "aspen_medium"};
        const char* names[] = {"BWH_PineTall", "BWH_PineYoung", "BWH_Oak", "BWH_Aspen"};
        for (int i = 0; i < 4; ++i) {
            // The tree exporter owns a complete self-contained model/material/atlas folder.
            if (fs::exists(root / "Assets" / names[i] / (std::string(names[i]) + ".glb.impostor.json"))) continue;
            std::string error; auto preset = ixtreemetree::loadPresetFile(engine / "assets/internal/tree_presets" / (std::string(presets[i]) + ".json"), &error);
            Require(preset.has_value(), error);
            ixtreemetree::Tree tree; tree.options = preset->options; tree.options.seed += 20261010 + i * 71;
            for (auto& length : tree.options.branch.length) length *= i < 2 ? 0.42f : 0.48f;
            for (auto& radius : tree.options.branch.radius) radius *= 0.52f;
            tree.options.leaves.size *= i < 2 ? 2.1f : 0.95f;
            if (i < 2) {
                tree.options.branch.length[1] *= 0.58f;
                tree.options.branch.radius[1] *= 0.48f;
                tree.options.leaves.count = 38;
                tree.options.leaves.cardsPerCluster = 3;
            }
            tree_tool::TreeMaterialBinding binding;
            const std::string bark = i < 2 ? "pine" : i == 2 ? "oak" : "birch";
            const std::string leaf = i < 2 ? "pine" : i == 2 ? "oak" : "ash";
            binding.barkBaseColorTexturePath = engine / ("assets/internal/textures/bark/" + bark + "_bark.png");
            binding.leafBaseColorTexturePath = engine / ("assets/internal/textures/leaves/" + leaf + "_leaf.png");
            binding.leafAlphaCutoff = 0.4f;
            binding.impostor.resolution = 128; binding.impostor.distance = 115; binding.impostor.transition = 25;
            const auto mesh = tree.generate();
            const auto result = tree_tool::TreeGlbExporter::SaveAsAsset(mesh, root, names[i], binding);
            Require(result.ok && result.impostorBaked, result.error + result.impostorWarning);
            const auto slots = db.loadDefaultMaterials(result.modelPath);
            for (size_t slot = 0; slot < slots.size(); ++slot) {
                auto* mat = MaterialAssetManager::Instance().getOrLoad(slots[slot]);
                Require(mat != nullptr, "Missing tree material");
                mat->baseColor = slot == 0 ? std::array<float,4>{.28f,.29f,.25f,1} : std::array<float,4>{.28f,.38f,.24f,1};
                mat->roughness = .95f;
                Require(MaterialAssetManager::Instance().save(*mat), "Cannot finish tree material");
            }
            std::vector<Guid> deps = db.loadDependencies(result.modelPath);
            Require(tree_tool::BakeTreeImpostor(mesh, result.modelPath, slots, binding.impostor, deps, error), error);
            Require(db.writeDependencies(result.modelPath, deps), "Cannot update tree atlas dependencies");
            std::cout << "TREE " << result.modelPath << " atlas baked\n";
        }
        db.scan(root);
        // Import original Blender meshes through the same material pipeline as the editor.
        auto& materials = MaterialAssetManager::Instance(); AssimpImporter importer;
        for (const auto& file : fs::directory_iterator(root / "Assets/BlackwaterHollow/Models")) {
            if (file.path().extension() != ".glb") continue;
            AssimpImporter::ImportOptions options; options.extractTextures = true;
            options.textureOutputDir = file.path().parent_path() / (file.path().stem().string() + "_textures");
            const auto imported = importer.importFile(file.path(), options);
            Require(imported.success, imported.errorMessage);
            std::vector<Guid> slots, deps;
            for (size_t i = 0; i < imported.materials.size(); ++i) {
                const auto& source = imported.materials[i];
                const auto guid = materials.createFromGltfMaterial(source, root / "Assets/BlackwaterHollow/Materials", source.name.empty() ? "slot_" + std::to_string(i) : source.name);
                slots.push_back(guid); deps.push_back(guid);
            }
            db.getOrCreateGuid(file.path()); Require(db.writeDefaultMaterials(file.path(), slots), "Cannot bind model materials");
            Require(db.writeDependencies(file.path(), deps), "Cannot bind model dependencies");
            std::cout << "MODEL " << file.path().stem() << " parts=" << imported.meshes.size() << " materials=" << slots.size() << '\n';
        }
        db.scan(root);
        AssetLibrary library(engine, root / "Assets");
        Require(library.Initialize(), "Cannot register new assets in the project library");
        std::unordered_map<std::string, std::string> libraryIds;
        for (const auto& entry : library.Entries()) if (!entry.guid.empty()) libraryIds[entry.guid] = entry.id;
        auto& manager = SceneManager::Instance();
        Require(manager.LoadScene((staging / "draft.scene").string()), "Cannot load authored draft");
        SceneData scene = manager.GetCurrentScene(); manager.CloseScene();
        auto& terrain = scene.terrain;
        terrain.exists = true; terrain.name = "Blackwater valley / 1000 x 1000 m";
        terrain.widthMeters = terrain.depthMeters = 1000; terrain.cellSizeMeters = 2;
        terrain.cellsX = terrain.cellsZ = 500; terrain.chunkSizeCells = 64;
        terrain.heightCmGrid = Read<float>(staging / "height_cm.f32", 501 * 501);
        terrain.splatABytes = Read<std::uint8_t>(staging / "splat_a.rgba", 500 * 500 * 4);
        terrain.splatBBytes.assign(500 * 500 * 4, 0);
        terrain.triplanarEnabled = true; terrain.triplanarSharpness = 5;
        terrain.triplanarSlopeThreshold = 0.55f; terrain.triplanarSlopeTransition = 0.2f;
        std::set<std::string> preload;
        auto assetId = [&](const std::string& relative) {
            const auto path = root / Utf8Path(relative);
            Require(fs::is_regular_file(path), "Missing asset " + path.string());
            const auto guid = db.getOrCreateGuid(path).toString();
            const auto found = libraryIds.find(guid);
            Require(found != libraryIds.end(), "Asset missing from library: " + relative);
            auto id = found->second; preload.insert(id); return id;
        };
        size_t trees = 0, colliders = 0, emitters = 0, sources = 0;
        for (auto& mesh : scene.meshEntities) {
            if (mesh.HasMeshGeometry()) {
                mesh.meshAssetId = assetId(mesh.meshAssetPath);
                for (const auto& mat : db.loadDefaultMaterials(root / Utf8Path(mesh.meshAssetPath))) mesh.materialSlots.push_back(mat.toString());
                if (mesh.meshAssetPath.find("Assets/BWH_") == 0) ++trees;
            }
            if (mesh.hasAudioSource) { mesh.audioSource.clipAssetId = assetId(mesh.audioSource.clipAssetId); ++sources; }
            if (mesh.hasScript) {
                mesh.script.scriptAssetId = assetId(mesh.script.scriptAssetId);
                for (auto& [key, value] : mesh.script.parameters) if (value.starts_with("Assets/")) value = assetId(value);
            }
            if (mesh.hasParticleSystem) {
                ++emitters;
                if (!mesh.particleSystem.textureAssetId.empty()) mesh.particleSystem.textureAssetId = assetId(mesh.particleSystem.textureAssetId);
            }
            colliders += mesh.hasCollider;
        }
        WaterBody water; water.id = 1; water.name = "Blackwater Lake";
        water.bboxMin[0] = -12; water.bboxMin[1] = -44;
        water.bboxMax[0] = 178; water.bboxMax[1] = 207;
        water.waterLevelY = water.config.waterLevelY = 0.6f;
        water.maskWidth = 190; water.maskHeight = 251;
        water.shapeMask = Read<std::uint8_t>(staging / "lake.mask", 190 * 251);
        auto& w = water.config;
        w.baseColor[0] = 0.035f; w.baseColor[1] = 0.11f; w.baseColor[2] = 0.105f; w.baseColor[3] = 0.93f;
        w.shallowColor[0] = 0.10f; w.shallowColor[1] = 0.22f; w.shallowColor[2] = 0.17f;
        w.deepColor[0] = 0.018f; w.deepColor[1] = 0.038f; w.deepColor[2] = 0.044f;
        w.reflectionQuality = WaterConfig::ReflectionQuality::Quarter;
        w.normalStrength = 0.32f; w.waveSpeedSmall = 0.012f; w.waveSpeedLarge = 0.008f;
        w.foamEnabled = false; w.foamIntensity = 0.05f; w.foamTerrainThickness = 0.18f;
        w.depthColorMax = 5; w.edgeFadeDistance = 1.5f; w.causticMode = WaterConfig::CausticMode::Off;
        // A body without a material inherits the project's default water. Persist every tuned
        // parameter in its own water material (scene water_config stores only a small subset).
        WaterMaterialData lakeMaterial; lakeMaterial.config = w;
        AssetLibrary::Entry lakeEntry; std::string waterError, previousWaterId;
        for (const auto& entry : library.Entries())
            if (entry.category == AssetLibrary::Category::WaterMaterial && entry.displayName == "BWH_BlackwaterLake") previousWaterId = entry.id;
        if (previousWaterId.empty()) {
            AssetLibrary::ImportOptions options; options.displayName = "BWH_BlackwaterLake"; options.subpath = "BlackwaterHollow/Materials";
            Require(library.CreateWaterMaterial(options, lakeMaterial, lakeEntry, waterError), waterError);
        } else Require(library.UpdateWaterMaterial(previousWaterId, lakeMaterial, lakeEntry, waterError), waterError);
        water.materialId = lakeEntry.id; preload.insert(lakeEntry.id);
        scene.waterBodies = {water};
        scene.preloadAssets.assign(preload.begin(), preload.end());
        const auto path = root / "Scenes/BlackwaterHollow/BlackwaterHollow.scene";
        fs::create_directories(path.parent_path());
        // Do not open ProjectManager here: saving this new scene must not replace the user's startup scene.
        manager.RestoreSceneSnapshot(scene, {}, false);
        Require(manager.SaveSceneAs(path.string()), "Cannot save native scene/chunks"); manager.CloseScene();
        Require(manager.LoadScene(path.string()), "Saved scene cannot be reopened");
        const auto& loaded = manager.GetCurrentScene();
        Require(loaded.terrain.heightCmGrid == terrain.heightCmGrid && loaded.meshEntities.size() == scene.meshEntities.size(), "Native scene round-trip failed");
        std::cout << "VALIDATED scene=" << path << " entities=" << scene.meshEntities.size() << " trees=" << trees
            << " colliders=" << colliders << " emitters=" << emitters << " sounds=" << sources << " terrain=1000x1000m / 64 chunks\n";
        return 0;
    } catch (const std::exception& ex) { std::cerr << ex.what() << '\n'; return 1; }
}
