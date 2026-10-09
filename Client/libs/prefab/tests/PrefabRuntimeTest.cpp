#include "PrefabDocument.h"
#include <cmath>
#include <iostream>
#include <sstream>

namespace pf = ixtreeme::prefab;
int main()
{
    int failures = 0;
    const auto check = [&](bool ok, const char* message) {
        if (!ok) { std::cerr << message << '\n'; ++failures; }
    };
    pf::PrefabDocument document;
    pf::PrefabEntity root;
    root.kind = pf::PrefabTemplate::Kind::Mesh;
    root.localId = 1;
    root.name = "Root";
    root.mesh.position[0] = 10;
    root.mesh.animatorControllerId = "controller_\"test\"";
    root.mesh.skinned = true;
    root.mesh.enabled = false;
    root.mesh.hasCollider = true;
    document.entities.push_back(root);
    pf::PrefabEntity child = root;
    child.localId = 3;
    child.parentLocalId = 2; // parent follows child in the file
    child.mesh.position[0] = 13;
    child.mesh.hasFixedJoint = true;
    child.mesh.fixedJoint.enabled = true;
    child.mesh.fixedJoint.connectedEntityId = 1;
    document.entities.push_back(child);
    pf::PrefabEntity point;
    point.kind = pf::PrefabTemplate::Kind::PointLight;
    point.localId = 2;
    point.parentLocalId = 1;
    point.point.position[0] = 12;
    document.entities.push_back(point);
    pf::PrefabEntity spot = point;
    spot.kind = pf::PrefabTemplate::Kind::SpotLight;
    spot.localId = 4;
    spot.spot.position[0] = 14;
    document.entities.push_back(spot);
    std::ostringstream output;
    pf::WriteDocument(output, document);
    auto roundtrip = pf::ParseDocument(output.str(), "fixture");
    check(roundtrip.entities.size() == 4, "multi-entity roundtrip");
    check(!roundtrip.entities[0].mesh.enabled, "prefab local enabled roundtrip");
    check(roundtrip.entities[0].mesh.animatorControllerId == root.mesh.animatorControllerId,
        "escaped animator controller roundtrip");
    std::ostringstream legacy;
    pf::WriteMesh(legacy, root.mesh, "Root");
    check(pf::ParseTemplate(legacy.str(), "fixture").mesh.animatorControllerId == root.mesh.animatorControllerId,
        "legacy single-entity animator roundtrip");
    check(pf::ParseTemplate("{\"type\":\"mesh_entity\"}", "old").mesh.animatorControllerId.empty(),
        "old prefab has no controller");
    check(pf::ParseTemplate("{\"type\":\"mesh_entity\"}", "old").mesh.enabled, "old prefab enabled by default");
    auto captured=document;
    captured.entities[0].mesh.id=900;
    captured.entities[1].mesh.id=901;
    captured.entities[1].mesh.fixedJoint.connectedEntityId=900;
    std::ostringstream capturedText; pf::WriteDocument(capturedText,captured);
    check(pf::ParseDocument(capturedText.str(),"captured").entities[1].mesh.fixedJoint.connectedEntityId==1,
        "scene joint target converted to prefab local id");
    std::uint32_t meshId = 101, lightId = 10;
    const auto meshAllocator = [&] { return meshId++; };
    const auto lightAllocator = [&] { return lightId++; };
    const float position[3] = {100, 20, 30};
    pf::RuntimePrefabInstance instance;
    std::string error;
    check(pf::InstantiateRuntime(roundtrip, "prefab_test", 100, position, meshAllocator, lightAllocator,
        instance, error), "valid runtime instantiation");
    if (instance.meshes.size() == 2 && instance.points.size() == 1 && instance.spots.size() == 1)
    {
        check(instance.meshes[0].id == 100, "preallocated root id reused");
        check(instance.meshes[1].position[0] == 103 && instance.meshes[1].position[1] == 20, "world offset preserved");
        check(instance.meshes[1].parent.id == instance.points[0].id &&
            instance.meshes[1].parent.type == "point_light", "forward parent reference remapped");
        check(instance.meshes[1].fixedJoint.connectedEntityId == 100, "joint target remapped");
        check(instance.meshes[0].prefabInstance.linked && instance.meshes[1].prefabInstance.localId == 3,
            "prefab links preserved");
        check(instance.meshes[1].renderRecordSlot == 0xffffffffu, "no borrowed render slot");
    }
    else check(false, "all meshes and lights instantiated");
    const auto expectInvalid = [&](pf::PrefabDocument invalid) {
        const auto beforeMesh = meshId, beforeLight = lightId;
        check(!pf::InstantiateRuntime(invalid, "bad", 200, position, meshAllocator, lightAllocator,
            instance, error), "invalid document rejected");
        check(!error.empty() && instance.meshes.empty() && meshId == beforeMesh && lightId == beforeLight,
            "invalid document has no partial output/allocations");
    };
    auto bad = roundtrip; bad.entities[2].parentLocalId = 3; expectInvalid(bad); // cycle
    bad = roundtrip; bad.entities[1].parentLocalId = 999; expectInvalid(bad);
    bad = roundtrip; bad.entities[1].localId = 1; expectInvalid(bad);
    bad = roundtrip; bad.entities[1].mesh.fixedJoint.connectedEntityId = 999; expectInvalid(bad);
    bad = roundtrip; bad.entities[0].kind = pf::PrefabTemplate::Kind::PointLight; expectInvalid(bad);
    return failures == 0 ? 0 : 1;
}
