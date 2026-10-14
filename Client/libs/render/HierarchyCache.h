#pragma once

#include "MapEditorTypes.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Flecs requires sibling identifiers to be unique. User-facing scene labels
// can repeat (e.g. thousands of "Oak" instances) and stay in HierarchySceneEntity.
inline std::string HierarchyInternalName(std::uint64_t entity)
{
    return "IxtreemeEditorObject_" + std::to_string(entity);
}

// Exact metadata comparisons: unchanged frames allocate/copy nothing. Transform and selection
// changes do not affect the hierarchy. Script-created objects use the same observation path.
class HierarchyChangeTracker
{
public:
    struct Source
    {
        HierarchyEntityType type;
        std::uint32_t id;
        std::string_view name, parentType, prefab;
        std::uint32_t parentId = 0;
        bool hidden = false, note = false, mainCamera = false;
    };
    void Begin(std::string_view scene, std::string_view path)
    {
        changed_ = !initialized_ || scene_ != scene || path_ != path;
        if (changed_) { scene_ = scene; path_ = path; }
        cursor_ = 0;
    }
    void Observe(const Source& source)
    {
        if (cursor_ == sources_.size()) { sources_.emplace_back(source); changed_ = true; }
        else if (!sources_[cursor_].Equals(source)) { sources_[cursor_] = Stamp(source); changed_ = true; }
        ++cursor_;
    }
    bool End()
    {
        if (cursor_ != sources_.size()) { sources_.resize(cursor_); changed_ = true; }
        initialized_ = true;
        return changed_;
    }
private:
    struct Stamp
    {
        HierarchyEntityType type{};
        std::uint32_t id = 0, parentId = 0;
        std::string name, parentType, prefab;
        bool hidden = false, note = false, mainCamera = false;
        Stamp() = default;
        explicit Stamp(const Source& s) : type(s.type), id(s.id), parentId(s.parentId), name(s.name),
            parentType(s.parentType), prefab(s.prefab), hidden(s.hidden), note(s.note), mainCamera(s.mainCamera) {}
        bool Equals(const Source& s) const
        {
            return type == s.type && id == s.id && parentId == s.parentId && name == s.name &&
                parentType == s.parentType && prefab == s.prefab && hidden == s.hidden &&
                note == s.note && mainCamera == s.mainCamera;
        }
    };
    std::vector<Stamp> sources_;
    std::string scene_, path_;
    std::size_t cursor_ = 0;
    bool initialized_ = false, changed_ = false;
};

// Cached preorder rows for ImGuiListClipper. Search and traversal are iterative, including deep
// trees; ancestry is retained for descendant matches. Expansion survives renames/reparenting.
class HierarchyViewCache
{
public:
    struct Row { std::uint64_t entity; std::uint32_t depth; };
    void Rebuild(const std::vector<HierarchySceneEntity>& entities, std::uint64_t root)
    {
        entities_ = &entities; root_ = root;
        index_.clear(); children_.clear(); roots_.clear();
        for (std::size_t i = 0; i < entities.size(); ++i) index_[entities[i].entity] = i;
        for (const auto& entity : entities)
        {
            if (entity.parent == root || !index_.contains(entity.parent)) roots_.push_back(entity.entity);
            else children_[entity.parent].push_back(entity.entity);
        }
        std::erase_if(expanded_, [&](auto id) { return !index_.contains(id); });
        dirty_ = true;
    }
    const HierarchySceneEntity* Find(std::uint64_t id) const
    {
        const auto it = index_.find(id);
        return it == index_.end() ? nullptr : &(*entities_)[it->second];
    }
    const std::vector<std::uint64_t>& Children(std::uint64_t id) const
    {
        static const std::vector<std::uint64_t> empty;
        const auto it = children_.find(id);
        return it == children_.end() ? empty : it->second;
    }
    void SetSearch(std::string_view query)
    {
        if (search_ != query) { search_ = query; dirty_ = true; }
    }
    bool IsExpanded(std::uint64_t id) const { return !search_.empty() || expanded_.contains(id); }
    void SetExpanded(std::uint64_t id, bool open)
    {
        if (open ? expanded_.insert(id).second : expanded_.erase(id) != 0) dirty_ = true;
    }
    void Reveal(std::uint64_t id)
    {
        std::unordered_set<std::uint64_t> visited;
        while (const auto* node = Find(id))
        {
            if (!visited.insert(id).second || node->parent == root_) break;
            SetExpanded(node->parent, true); id = node->parent;
        }
    }
    bool Matches(std::uint64_t id) const { Ensure(); return search_.empty() || matches_.contains(id); }
    const std::vector<Row>& Rows() const { Ensure(); return rows_; }
    std::uint64_t RowRevision() const { Ensure(); return rowRevision_; }
private:
    static bool Contains(std::string_view name, std::string_view query)
    {
        return std::search(name.begin(), name.end(), query.begin(), query.end(), [](unsigned char a, unsigned char b) {
            return std::tolower(a) == std::tolower(b);
        }) != name.end();
    }
    void Ensure() const
    {
        if (!dirty_) return;
        rows_.clear(); matches_.clear(); dirty_ = false; ++rowRevision_;
        if (!entities_) return;
        std::vector<Row> preorder, stack;
        std::unordered_set<std::uint64_t> visited;
        for (auto it = roots_.rbegin(); it != roots_.rend(); ++it) stack.push_back({*it, 0});
        while (!stack.empty())
        {
            const auto row = stack.back(); stack.pop_back();
            if (!visited.insert(row.entity).second) continue;
            preorder.push_back(row);
            const auto& children = Children(row.entity);
            for (auto it = children.rbegin(); it != children.rend(); ++it) stack.push_back({*it, row.depth + 1});
        }
        if (!search_.empty())
        {
            for (auto it = preorder.rbegin(); it != preorder.rend(); ++it)
            {
                const auto* node = Find(it->entity);
                if (Contains(node->name, search_)) matches_.insert(node->entity);
                if (matches_.contains(node->entity)) matches_.insert(node->parent);
            }
            if (!matches_.empty()) matches_.insert(root_);
        }
        std::uint32_t collapsedDepth = ~0u;
        for (const auto& row : preorder)
        {
            if (row.depth > collapsedDepth) continue;
            collapsedDepth = ~0u;
            if (!search_.empty() && !matches_.contains(row.entity)) continue;
            rows_.push_back(row);
            if (!IsExpanded(row.entity)) collapsedDepth = row.depth;
        }
    }
    const std::vector<HierarchySceneEntity>* entities_ = nullptr;
    std::uint64_t root_ = 0;
    std::unordered_map<std::uint64_t, std::size_t> index_;
    std::unordered_map<std::uint64_t, std::vector<std::uint64_t>> children_;
    std::vector<std::uint64_t> roots_;
    std::unordered_set<std::uint64_t> expanded_;
    std::string search_;
    mutable bool dirty_ = true;
    mutable std::vector<Row> rows_;
    mutable std::unordered_set<std::uint64_t> matches_;
    mutable std::uint64_t rowRevision_ = 0;
};
