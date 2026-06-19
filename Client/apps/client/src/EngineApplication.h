#pragma once

#include <filesystem>

class NativeWindow;

namespace client::asset
{
class IAssetReader;
}

std::filesystem::path ResolveIxtreemeEngineAssetRoot();

int RunIxtreemeEngine(NativeWindow& window,
                      client::asset::IAssetReader& assets);
