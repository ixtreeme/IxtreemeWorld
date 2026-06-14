#pragma once

#include "tree_mesh.h"
#include "tree_options.h"

namespace ixtreemetree
{
class Tree
{
public:
    TreeOptions options = defaultTreeOptions();
    TreeMesh generate() const;
};
}
