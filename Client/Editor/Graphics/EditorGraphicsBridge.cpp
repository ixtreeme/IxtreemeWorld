// Generic registry implementation. No backend calls here by design: the
// backend adapter mirrors registrations into UI descriptors.

#include "EditorGraphicsBridge.h"

namespace ixeditor::graphics
{

EditorTextureHandle EditorGraphicsBridge::Register(std::shared_ptr<ixrhi::IXRHITexture> texture,
                                                   std::shared_ptr<ixrhi::IXRHISampler> sampler,
                                                   const std::string& tag)
{
    EditorTextureHandle handle;
    if (!texture || !sampler)
        return handle;
    std::lock_guard<std::mutex> lock(m_mutex);
    handle.value = m_nextHandle++;
    if (handle.value == 0) // wrap guard (practically unreachable)
        handle.value = m_nextHandle++;
    Entry entry;
    entry.texture = std::move(texture);
    entry.sampler = std::move(sampler);
    entry.tag = tag;
    m_entries.emplace(handle.value, std::move(entry));
    return handle;
}

void EditorGraphicsBridge::Unregister(EditorTextureHandle handle)
{
    if (!handle.IsValid())
        return;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries.erase(handle.value);
}

void EditorGraphicsBridge::Clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries.clear();
}

bool EditorGraphicsBridge::Lookup(EditorTextureHandle handle,
                                  std::shared_ptr<ixrhi::IXRHITexture>& outTexture,
                                  std::shared_ptr<ixrhi::IXRHISampler>& outSampler) const
{
    outTexture.reset();
    outSampler.reset();
    if (!handle.IsValid())
        return false;
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_entries.find(handle.value);
    if (it == m_entries.end())
        return false;
    outTexture = it->second.texture.lock();
    outSampler = it->second.sampler.lock();
    return outTexture != nullptr && outSampler != nullptr;
}

std::size_t EditorGraphicsBridge::EntryCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_entries.size();
}

} // namespace ixeditor::graphics
