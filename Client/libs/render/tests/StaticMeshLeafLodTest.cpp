#include "StaticMeshRenderer.h"
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <set>

struct StaticMeshLeafLodTestAccess
{
    static int Run()
    {
        int failures = 0;
        const auto check = [&](const char* label, bool ok) {
            std::cout << label << ": " << (ok ? "PASS" : "FAIL") << '\n';
            failures += !ok;
        };
        StaticMeshRenderer renderer;
        // 128 disconnected quads, each with a unique UV offset identifying its source card.
        for (unsigned card = 0; card < 128; ++card)
        {
            const unsigned start = static_cast<unsigned>(renderer.m_vertices.size());
            for (unsigned corner = 0; corner < 4; ++corner)
            {
                StaticMeshRenderer::Vertex v{};
                v.position[0] = card * 3.0f + ((corner == 1 || corner == 2) ? 1.0f : -1.0f);
                v.position[1] = corner >= 2 ? 1.0f : -1.0f;
                v.normal[2] = 1.0f;
                v.uv[0] = card * 2.0f + ((corner == 1 || corner == 2) ? 1.0f : 0.0f);
                v.uv[1] = corner >= 2 ? 1.0f : 0.0f;
                renderer.m_vertices.push_back(v);
            }
            for (unsigned index : {0u, 1u, 2u, 0u, 2u, 3u})
                renderer.m_indices.push_back(start + index);
        }
        renderer.m_draws.push_back({0, 128 * 6, 0, 128 * 4});
        renderer.m_materialDefaults.emplace_back().alphaMode = "mask";
        renderer.BuildLods();
        check("main-view card counts 75/50/25 percent",
            renderer.m_viewCardDraws[0][0].indexCount == 96 * 6 &&
            renderer.m_viewCardDraws[1][0].indexCount == 64 * 6 &&
            renderer.m_viewCardDraws[2][0].indexCount == 32 * 6);
        check("shadow keeps its existing quarter density", renderer.m_shadowCardDraws[0].indexCount == 32 * 6);
        std::array<std::set<unsigned>, 3> kept;
        bool attributes = true, indices = true, area = true;
        for (unsigned level = 0; level < 3; ++level)
        {
            const auto& draw = renderer.m_viewCardDraws[level][0];
            double totalArea = 0;
            for (unsigned i = 0; i < draw.indexCount; i += 6)
            {
                const auto a = renderer.m_lodIndices[draw.firstIndex + i];
                const auto b = renderer.m_lodIndices[draw.firstIndex + i + 1];
                const auto c = renderer.m_lodIndices[draw.firstIndex + i + 2];
                indices &= a < renderer.m_cardLodVertices.size() && b < renderer.m_cardLodVertices.size() && c < renderer.m_cardLodVertices.size();
                if (!indices) break;
                const auto& va = renderer.m_cardLodVertices[a];
                const auto& vb = renderer.m_cardLodVertices[b];
                const auto& vc = renderer.m_cardLodVertices[c];
                kept[level].insert(static_cast<unsigned>(va.uv[0] / 2));
                totalArea += (vb.position[0] - va.position[0]) * (vc.position[1] - vb.position[1]);
            }
            area &= std::abs(totalArea - 128 * 4) < 0.02;
        }
        for (const auto& v : renderer.m_cardLodVertices)
            attributes &= v.normal[0] == 0 && v.normal[1] == 0 && v.normal[2] == 1 &&
                (v.uv[1] == 0 || v.uv[1] == 1) && std::floor(v.uv[0]) == v.uv[0];
        check("all card indices reference uploaded vertex pool", indices);
        check("source normals and UVs preserved", attributes);
        check("main-view card area compensated", area);
        check("coarser subsets nested", std::includes(kept[0].begin(), kept[0].end(), kept[1].begin(), kept[1].end()) &&
            std::includes(kept[1].begin(), kept[1].end(), kept[2].begin(), kept[2].end()));
        check("near cards remain full detail", renderer.SelectViewCardLevel(100, false) == 0);
        check("far cards reach coarsest level", renderer.SelectViewCardLevel(0.1f, false) == 3);
        check("selected cards remain full detail", renderer.SelectViewCardLevel(0.1f, true) == 0);
        check("invalid projection remains full detail", renderer.SelectViewCardLevel(0, false) == 0 &&
            renderer.SelectViewCardLevel(std::numeric_limits<float>::quiet_NaN(), false) == 0);
        for (unsigned level = 0; level < 3; ++level)
        {
            const float threshold = 1.5f / renderer.m_viewCardExpansion[level];
            check("screen-space threshold", renderer.SelectViewCardLevel(threshold * 1.01f, false) == level &&
                renderer.SelectViewCardLevel(threshold * 0.99f, false) == level + 1);
        }
        const auto originalIndices = renderer.m_lodIndices;
        const auto originalVertices = renderer.m_cardLodVertices;
        renderer.BuildLods();
        check("repeat builds deterministic", renderer.m_lodIndices == originalIndices &&
            renderer.m_cardLodVertices.size() == originalVertices.size() &&
            std::memcmp(renderer.m_cardLodVertices.data(), originalVertices.data(), originalVertices.size() * sizeof(originalVertices[0])) == 0);
        renderer.m_materialDefaults[0].alphaMode = "blend";
        renderer.BuildLods();
        check("blend materials stay unchanged", renderer.m_viewCardDraws[0].empty());
        renderer.m_materialDefaults[0].alphaMode = "mask";
        renderer.m_draws[0].indexCount = 63 * 6;
        renderer.BuildLods();
        check("small cutout meshes stay unchanged", renderer.m_viewCardDraws[0].empty());
        renderer.m_draws[0].indexCount = 128 * 6;
        // Join every card through a shared corner: this is one continuous mesh, not a crown.
        for (unsigned i = 0; i < 128 * 6; i += 6)
            renderer.m_indices[i] = renderer.m_indices[i + 3] = 0;
        renderer.BuildLods();
        check("continuous cutout geometry stays unchanged", renderer.m_viewCardDraws[0].empty());
        return failures ? 1 : 0;
    }
};

int main() { return StaticMeshLeafLodTestAccess::Run(); }
