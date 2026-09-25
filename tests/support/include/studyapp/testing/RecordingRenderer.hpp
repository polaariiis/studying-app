#pragma once

#include <studyapp/render/Renderer.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace studyapp::testing {

/// Renderer that keeps meshes in memory and records the last frame (docs/TESTING.md:
/// "RecordingRenderer"). Rejects stale handles like the real backend.
class RecordingRenderer final : public render::Renderer {
public:
    core::Result<void> initialize() override {
        initialized_ = true;
        return {};
    }
    bool isInitialized() const noexcept override { return initialized_; }
    void resize(int width, int height) override { framebuffer = {width, height}; }
    render::MeshHandle createMesh(const render::MeshData& mesh) override {
        const render::MeshHandle handle{.index = nextIndex_++, .generation = 1};
        meshes[handle.index] = mesh;
        ++created;
        return handle;
    }
    void updateMesh(render::MeshHandle handle, const render::MeshData& mesh) override {
        if (meshes.contains(handle.index)) {
            meshes[handle.index] = mesh;
        }
    }
    void destroyMesh(render::MeshHandle handle) override {
        destroyed += meshes.erase(handle.index);
    }
    void render(const render::RenderFrame& frame) override {
        lastContent.assign(frame.content.begin(), frame.content.end());
        lastOverlay.assign(frame.overlay.begin(), frame.overlay.end());
        lastBackground = frame.background;
        ++frames;
    }
    render::RenderStats lastFrameStats() const noexcept override { return {}; }
    void releaseAll() noexcept override {
        meshes.clear();
        initialized_ = false;
    }

    std::map<std::uint32_t, render::MeshData> meshes;
    std::vector<render::DrawItem> lastContent;
    std::vector<render::DrawItem> lastOverlay;
    render::Background lastBackground{};
    std::pair<int, int> framebuffer{};
    std::size_t created = 0;
    std::size_t destroyed = 0;
    std::size_t frames = 0;

private:
    bool initialized_ = false;
    std::uint32_t nextIndex_ = 1;
};

} // namespace studyapp::testing
