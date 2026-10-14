#include "HierarchyCache.h"
#include <iostream>
#include <flecs.h>

int main()
{
    int failures = 0;
    const auto check = [&](bool result, const char* message) {
        if (!result) { std::cerr << message << '\n'; ++failures; }
    };
    HierarchyChangeTracker tracker;
    {
        flecs::world world;
        const auto root = ecs_new(world.c_ptr());
        const auto first = ecs_new(world.c_ptr());
        const auto second = ecs_new(world.c_ptr());
        const auto firstName = HierarchyInternalName(first), secondName = HierarchyInternalName(second);
        ecs_set_name(world.c_ptr(), root, HierarchyInternalName(root).c_str());
        ecs_set_name(world.c_ptr(), first, firstName.c_str());
        ecs_set_name(world.c_ptr(), second, secondName.c_str());
        ecs_add_pair(world.c_ptr(), first, EcsChildOf, root);
        ecs_add_pair(world.c_ptr(), second, EcsChildOf, root);
        check(firstName != secondName && ecs_is_alive(world.c_ptr(), first) && ecs_is_alive(world.c_ptr(), second),
            "duplicate display labels need distinct Flecs identifiers");
        std::vector<HierarchySceneEntity> duplicateLabels{{first, root, HierarchyEntityType::MeshEntity, 1, "Oak"},
            {second, root, HierarchyEntityType::MeshEntity, 2, "Oak"}};
        HierarchyViewCache duplicates;
        duplicates.Rebuild(duplicateLabels, root); duplicates.SetSearch("Oak");
        check(duplicates.Rows().size() == 2 && duplicates.Find(first)->name == "Oak" && duplicates.Find(second)->name == "Oak",
            "duplicate labels stay unchanged and independently searchable");
    }
    HierarchyChangeTracker::Source source{HierarchyEntityType::MeshEntity, 1, "Oak", {}, {}, 0};
    const auto update = [&] { tracker.Begin("Forest", "Forest.scene"); tracker.Observe(source); return tracker.End(); };
    check(update(), "initial scene needs rebuild");
    check(!update(), "unchanged metadata needs no rebuild");
    source.name = "Birch"; check(update(), "rename invalidates hierarchy");
    source.parentType = "mesh_entity"; source.parentId = 2; check(update(), "reparent invalidates hierarchy");
    source.hidden = true; check(update(), "visibility invalidates hierarchy");
    source.note = true; check(update(), "note tag invalidates hierarchy");
    source.prefab = "tree-prefab"; check(update(), "prefab label invalidates hierarchy");
    check(!update(), "unchanged prefab and tags are cached");
    tracker.Begin("Forest", "Forest.scene"); check(tracker.End(), "deletion invalidates hierarchy");
    tracker.Begin("Forest", "Other.scene"); check(tracker.End(), "scene identity invalidates hierarchy");

    std::vector<HierarchySceneEntity> entities;
    entities.push_back({10, 1, HierarchyEntityType::MeshEntity, 10, "Parent"});
    entities.push_back({20, 10, HierarchyEntityType::MeshEntity, 20, "Child"});
    entities.push_back({30, 20, HierarchyEntityType::MeshEntity, 30, "Dragon"});
    entities.push_back({40, 1, HierarchyEntityType::MeshEntity, 40, "Sibling"});
    HierarchyViewCache view;
    view.Rebuild(entities, 1);
    check(view.Rows().size() == 2, "closed parents omit descendants");
    view.SetExpanded(10, true); view.SetExpanded(20, true);
    check(view.Rows().size() == 4 && view.Rows()[2].entity == 30 && view.Rows()[2].depth == 2,
        "expanded preorder retains order and indentation");
    const auto revision = view.RowRevision();
    check(view.Rows().size() == 4 && view.RowRevision() == revision, "unchanged panel reuses row list");
    view.SetSearch("dRaGoN");
    check(view.Rows().size() == 3 && view.Matches(1) && !view.Matches(40), "case insensitive search keeps ancestors");
    view.SetSearch("missing"); check(view.Rows().empty(), "missing search is empty");
    view.SetSearch(""); view.SetExpanded(10, false);
    view.Reveal(30); check(view.Rows().size() == 4, "selecting hidden descendant reveals its path");
    entities[2].parent = 40; view.Rebuild(entities, 1); view.Reveal(30);
    check(view.Children(40).size() == 1 && view.Rows().back().entity == 30, "drag reparent changes path and keeps selection handle");
    entities[2].name = "Renamed"; view.Rebuild(entities, 1); view.SetSearch("renamed");
    check(view.Rows().size() == 2 && view.Rows()[0].entity == 40, "renamed child search follows new parent");
    entities.erase(entities.begin() + 2); view.Rebuild(entities, 1);
    check(!view.Find(30) && view.Rows().empty(), "deleted rows and search results disappear");

    entities.clear(); view.SetSearch("");
    for (std::uint64_t i = 0; i < 10000; ++i)
        entities.push_back({100 + i, i ? 99 + i : 1, HierarchyEntityType::MeshEntity,
            static_cast<std::uint32_t>(i), "Node " + std::to_string(i)});
    view.Rebuild(entities, 1); view.SetSearch("Node 9999");
    check(view.Rows().size() == 10000 && view.Rows().back().depth == 9999,
        "deep 10000 entity search is iterative and retains ancestry");
    view.SetSearch(""); check(view.Rows().size() < 10000, "clearing search restores collapse state");
    std::cout << "Hierarchy cache failures: " << failures << '\n';
    return failures ? 1 : 0;
}
