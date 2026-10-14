#pragma once

#include "MaterialGraphDocument.h"
#include "MaterialGraphAssets.h"
#include "GeometryAssets.h"
#include "SmartMaterialApplication.h"
#include "GeometryUvAtlas.h"
#include "GraphShader.h"
#include <chrono>
#include "EditorGraphicsBridge.h"
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ax::NodeEditor { struct EditorContext; }
namespace material_tool
{
class MaterialGraphPanel
{
    friend struct MaterialGraphPanelTestAccess;
public:
    static constexpr const char* WindowName = "Material Graph###MaterialGraph";
    MaterialGraphPanel();
    ~MaterialGraphPanel();
    MaterialGraphPanel(const MaterialGraphPanel&) = delete;
    MaterialGraphPanel& operator=(const MaterialGraphPanel&) = delete;
    void RenderView(bool& open);
    void RenderInspector();
    void SetServices(AssetLibrary* library, ixeditor::graphics::IEditorTextureProvider* provider);
    bool TakeInspectorRequest();
    bool OwnsKeyboard(bool inspectorActive) const;
    void HandleHotkeys();
    bool CanUndo() const { return !undo_.empty(); }
    bool CanRedo() const { return !redo_.empty(); }
    void Undo(bool redo = false);
    void RequestOpen(const std::filesystem::path& path);
    bool Load(const std::filesystem::path& path);
    bool Save(const std::filesystem::path& path);
    bool SaveInstance(const std::filesystem::path& path);
    bool LoadInstance(const std::filesystem::path& path);
    bool SelectGeometryMesh(const std::filesystem::path& path,unsigned meshIndex = 0);
    bool StartGeometryBake();
    bool SaveGeometryBake(const std::filesystem::path& path);
    bool BindGeometryBake(const std::string& guid);
    bool AddGeneratorAsset(const std::filesystem::path& path,std::uint32_t layer = 0);
    void SetApplyTarget(const MeshRendererEditorState& mesh, const std::filesystem::path& path, bool allowed);
    bool BakeAndApply();
    void CancelBakeAndApply();
    std::optional<SmartMaterialAssignment> TakeMaterialAssignment();
    const MaterialGraphDocument& Document() const { return document_; }
private:
    void EnsureContext();
    void ResetContext();
    void RecordUndo(const MaterialGraphDocument& before);
    void AddNode(NodeKind kind);
    void DrawNode(const GraphNode& node);
    void RenderNodeMenu();
    void RenderToolbar();
    void SaveDialog(bool saveAs = false);
    void OpenDialog();
    void RequestReplace(bool openFile);
    void RenderUnsavedPopup();
    void RefreshSavedAsset();
    void SyncPositions();
    void RenderPreview();
    void RenderGeometryBake();
    void PollGeometryBake();
    void RenderApplyMaterial();
    void RenderInstanceParameters();
    void RenderLayerStack();
    void RenderSmartGenerators();
    void UpdatePreview();
    void RenderTextureBinding(GraphNode& node);
    void RenderSubgraphBinding(std::uint32_t node);
    bool CompileMaterial(MaterialGraphProgram& program, std::string& error, std::uint32_t previewPin = 0);
    void ExportShader();
    void BakeMaterial();
    ax::NodeEditor::EditorContext* context_ = nullptr;
    MaterialGraphDocument document_ = MaterialGraphDocument::Example();
    MaterialGraphDocument saved_ = document_;
    std::vector<MaterialGraphDocument> undo_, redo_;
    std::optional<MaterialGraphDocument> dragBefore_, propertyBefore_;
    std::filesystem::path path_, pendingPath_;
    std::string status_;
    std::uint32_t selected_ = 0;
    std::array<float, 2> spawn_{180, 100};
    bool requestInspector_ = false, layoutPending_ = true, fitPending_ = true;
    bool requestUnsaved_ = false, pendingOpen_ = false;
    AssetLibrary* library_ = nullptr;
    ixeditor::graphics::IEditorTextureProvider* provider_ = nullptr;
    ixeditor::graphics::EditorTextureHandle previewTexture_;
    GraphTextureCache textures_;
    GraphSubgraphCache subgraphs_;
    GraphGeometryCache geometry_;
    GeometryBakeJob geometryJob_;
    GeometryUvAtlasJob uvAtlasJob_;
    // Ends the pending Bake & Apply; an automatic UV model without its material is removed.
    void DiscardApplyRequest();
    // The material last published from this panel with a compiled graph shader. Parameter edits reach
    // it in the scene at once (GraphLiveValues) and are written into its package shortly after; a
    // change to the graph itself is compiled into a new package in the background once the edits
    // pause, and the scene keeps the previous shader until then (or when the new graph cannot compile).
    struct LiveMaterial
    {
        std::string guid, name;
        std::filesystem::path package, graphFolder;
        std::string geometryMap;        // GUID of the baked geometry map the package samples, if any
        MaterialGraphDocument document; // the authored graph it was compiled from
        std::unordered_map<std::string, std::string> textures; // and its texture overrides
        GraphShaderPackage data;
        std::size_t parameters = 0;     // leading float4s of the uniform block
        MaterialGraphDocument checkedDocument;
        ParameterOverrides checkedParameters;
        bool structureChanged = false, unsaved = false, recheck = false;
        std::chrono::steady_clock::time_point changed{}, edited{};
        // The graph being compiled, and the last one that could not be (not retried until edited).
        MaterialGraphDocument compiling, failed;
        ParameterOverrides compilingParameters, failedParameters;
        std::size_t compilingSlots = 0;
        std::string notice;
    };
    std::optional<LiveMaterial> live_;
    GraphShaderJob liveJob_;
    std::string liveJobGuid_;
    // Ends the link (another graph opened): values waiting to be saved are written first.
    void UnlinkLiveMaterial();
    void LinkLiveMaterial(const AssetLibrary::Entry& material, const MaterialGraphDocument& document,
                          const ParameterOverrides& parameters, const std::filesystem::path& graphFolder);
    void UpdateLiveMaterial(bool persistNow = false);
    GeometryBakeSettings geometrySettings_;
    struct ApplyRequest {
        SmartMaterialAssignment target;
        MaterialGraphDocument document;
        ParameterOverrides parameters;
        std::filesystem::path graphFolder;
        std::string name;
        std::vector<unsigned> submeshes; // Source submeshes drawn with the target material slot.
        bool atlasAttempted = false;
    };
    MeshRendererEditorState applyTarget_;
    std::filesystem::path applyModelPath_;
    bool applyAllowed_ = false;
    int applyResolution_ = 1;
    std::optional<ApplyRequest> applyRequest_;
    std::optional<SmartMaterialAssignment> materialAssignment_;
    bool geometryPreview_ = false;
    MaterialGraphProgram program_;
    MaterialGraphDocument evaluated_;
    ParameterOverrides parameters_;
    PreviewSettings previewSettings_, renderedSettings_;
    std::vector<std::uint8_t> previewPixels_;
    std::string compileError_;
    std::string graphError_;
    bool previewDirty_ = true;
    bool forceCompile_ = false, subgraphMode_ = false;
    bool generatorMode_ = false;
    SmartGenerator generator_;
    std::uint32_t subgraphVersion_ = 1;
    double previewTime_ = -1;
    std::uint64_t libraryRevision_ = 0;
    std::uint32_t previewPin_ = 0, renderedPin_ = 0;
    int bakeResolution_ = 1;
    int pendingPreset_ = 0;
    char bakeName_[128] = "GraphMaterial", nodeSearch_[128]{};
};
}
