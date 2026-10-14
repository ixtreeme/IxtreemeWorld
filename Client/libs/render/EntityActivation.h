#pragma once
#include "MapEditorTypes.h"
#include <unordered_map>
#include <vector>

// Recompute only after hierarchy/local-enabled mutations. The iterative walk handles deep
// hierarchies without recursion; malformed cycles are inactive. Missing parents act as roots.
inline void ResolveEntityActivation(std::vector<MeshSceneEntity>& meshes,
    std::vector<PointLight>& points, std::vector<SpotLight>& spots,
    std::vector<CameraEntity>* cameras = nullptr)
{
    struct Node { SceneParentRef parent; bool local; bool* effective; bool* changed = nullptr; unsigned state = 0; };
    auto key = [](const std::string& type, std::uint32_t id) -> std::uint64_t {
        const unsigned kind = type == "mesh_entity" ? 1 : type == "point_light" ? 2 : type == "spot_light" ? 3 : type == "camera" ? 4 : 0;
        return (std::uint64_t(kind) << 32) | id;
    };
    std::unordered_map<std::uint64_t, Node> nodes;
    nodes.reserve(meshes.size() + points.size() + spots.size());
    for (auto& m : meshes) nodes.emplace(key("mesh_entity", m.id), Node{m.parent,m.enabled,&m.effectiveEnabled,&m.activationChanged});
    for (auto& p : points) nodes.emplace(key("point_light", p.id), Node{p.parent,true,&p.effectiveEnabled});
    for (auto& s : spots) nodes.emplace(key("spot_light", s.id), Node{s.parent,true,&s.effectiveEnabled});
    if (cameras) for (auto& c : *cameras) nodes.emplace(key("camera", c.id), Node{c.parent,true,&c.effectiveEnabled});
    std::vector<Node*> path;
    for (auto& [id, node] : nodes) {
        if (node.state == 2) continue;
        path.clear(); Node* current = &node; bool inherited = true;
        while (current) {
            if (current->state == 2) { inherited = *current->effective; break; }
            if (current->state == 1) { inherited = false; break; }
            current->state = 1; path.push_back(current);
            const auto parent = nodes.find(key(current->parent.type, current->parent.id));
            current = parent == nodes.end() ? nullptr : &parent->second;
        }
        for (auto it = path.rbegin(); it != path.rend(); ++it) {
            inherited = inherited && (*it)->local;
            if ((*it)->changed && *(*it)->effective != inherited) *(*it)->changed = true;
            *(*it)->effective = inherited; (*it)->state = 2;
        }
    }
}
