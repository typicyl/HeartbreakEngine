// Vfx/EffekseerBackend.cpp - see EffekseerBackend.h. The only TU that includes the Effekseer headers.
#include "Vfx/EffekseerBackend.h"

#include "Assets/VFS.h" // vfs::ReadFile - packs + disk, so effects load in shipped builds too
#include "Core/Log.h"

#ifdef HBE_HAVE_EFFEKSEER

#include <d3d12.h>

#include <Effekseer.h>
#include <EffekseerRendererDX12.h>

#ifdef HBE_EFFEKSEER_VK
#include <vulkan/vulkan.h>
#include <EffekseerRendererVulkan.h>
#endif

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace hbe::vfx {
namespace {

// An Effekseer FileReader over an in-memory byte buffer read from Heartbreak's VFS.
class VfsFileReader : public ::Effekseer::FileReader {
public:
    // By value (not &&): Effekseer's MakeRefPtr does not perfect-forward, so an rvalue-ref ctor
    // can't bind. The caller std::move()s in, so this is a move, not a copy.
    explicit VfsFileReader(std::vector<u8> d) : data_(std::move(d)) {}
    size_t Read(void* buffer, size_t size) override {
        const size_t n = std::min(size, data_.size() - pos_);
        if (n) std::memcpy(buffer, data_.data() + pos_, n);
        pos_ += n;
        return n;
    }
    void Seek(int position) override {
        pos_ = position < 0 ? 0 : static_cast<size_t>(position);
        if (pos_ > data_.size()) pos_ = data_.size();
    }
    int GetPosition() const override { return static_cast<int>(pos_); }
    size_t GetLength() const override { return data_.size(); }

private:
    std::vector<u8> data_;
    size_t pos_ = 0;
};

// Routes ALL Effekseer file reads (effect sub-resources: external textures/models for `.efk`)
// through hbe::vfs::ReadFile, which reads a mounted `.uap` pack in a shipped build and falls back to
// disk in the editor - the same unified path every other Heartbreak runtime loader uses.
class VfsFileInterface : public ::Effekseer::FileInterface {
public:
    ::Effekseer::FileReaderRef OpenRead(const char16_t* path) override {
        const std::filesystem::path p{std::u16string(path)};
        auto bytes = ::hbe::vfs::ReadFile(p);
        if (!bytes) return nullptr;
        return ::Effekseer::MakeRefPtr<VfsFileReader>(std::move(*bytes));
    }
    // Read-only: the runtime never writes effect files back through Effekseer.
    ::Effekseer::FileWriterRef OpenWrite(const char16_t* /*path*/) override { return nullptr; }
};

} // namespace

struct EffekseerBackend::Impl {
    enum class Api { DX12, Vulkan } api = Api::DX12;
    ::Effekseer::ManagerRef manager;
    ::EffekseerRenderer::RendererRef renderer;
    ::Effekseer::RefPtr<::EffekseerRenderer::SingleFrameMemoryPool> memoryPool;
    ::Effekseer::RefPtr<::EffekseerRenderer::CommandList> commandList;
    std::vector<::Effekseer::EffectRef> effects;         // index+1 == effectId (0 = invalid)
    std::unordered_map<std::string, u32> effectByPath;   // path -> effectId
    ::Effekseer::FileInterfaceRef fileInterface;         // VFS-backed (packs + disk)
    f32 time = 0.0f;

    // Effekseer's Matrix44 is row-major with row-vector math, i.e. the transpose of a glm
    // column-major (column-vector) matrix. Transpose on the way in.
    static ::Effekseer::Matrix44 ToEfk(const glm::mat4& m) {
        ::Effekseer::Matrix44 e;
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) e.Values[r][c] = m[c][r];
        return e;
    }

    // Shared once `renderer` exists (DX12 or Vulkan): the manager + its sub-renderers/loaders +
    // per-frame command list. The renderer type is backend-agnostic here, so this is identical
    // across APIs - only the Create call (and Begin/EndCommandList in Draw) differs.
    static constexpr int kSquareMax = 8192;
    void FinishSetup() {
        manager = ::Effekseer::Manager::Create(kSquareMax);
        manager->SetSpriteRenderer(renderer->CreateSpriteRenderer());
        manager->SetRibbonRenderer(renderer->CreateRibbonRenderer());
        manager->SetRingRenderer(renderer->CreateRingRenderer());
        manager->SetTrackRenderer(renderer->CreateTrackRenderer());
        manager->SetModelRenderer(renderer->CreateModelRenderer());
        // VFS-backed loaders so an effect's external textures/models resolve from the mounted packs
        // in a shipped build (and disk in the editor).
        fileInterface = ::Effekseer::MakeRefPtr<VfsFileInterface>();
        manager->SetTextureLoader(renderer->CreateTextureLoader(fileInterface));
        manager->SetModelLoader(renderer->CreateModelLoader(fileInterface));
        manager->SetMaterialLoader(renderer->CreateMaterialLoader(fileInterface));
        manager->SetCoordinateSystem(::Effekseer::CoordinateSystem::RH); // match glm world space
        memoryPool = ::EffekseerRenderer::CreateSingleFrameMemoryPool(renderer->GetGraphicsDevice());
        commandList = ::EffekseerRenderer::CreateCommandList(renderer->GetGraphicsDevice(), memoryPool);
        effects.push_back(nullptr); // reserve id 0 = invalid
    }
};

EffekseerBackend::EffekseerBackend() = default;

EffekseerBackend::~EffekseerBackend() { Shutdown(); }

bool EffekseerBackend::Init(void* d3d12Device, void* d3d12CommandQueue, u32 rtFormatDXGI,
                            u32 depthFormatDXGI, bool hasDepth, int swapBufferCount) {
    if (impl_) return true;
    if (!d3d12Device || !d3d12CommandQueue) return false;
    auto* device = static_cast<ID3D12Device*>(d3d12Device);
    auto* queue = static_cast<ID3D12CommandQueue*>(d3d12CommandQueue);

    auto impl = std::make_unique<Impl>();
    DXGI_FORMAT rt = static_cast<DXGI_FORMAT>(rtFormatDXGI);
    const DXGI_FORMAT depth = hasDepth ? static_cast<DXGI_FORMAT>(depthFormatDXGI)
                                       : DXGI_FORMAT_UNKNOWN;
    impl->renderer = ::EffekseerRendererDX12::Create(device, queue, swapBufferCount, &rt, 1, depth,
                                                     /*isReversedDepth=*/false, Impl::kSquareMax);
    if (impl->renderer == nullptr) {
        HBE_ERROR("EffekseerBackend: DX12 renderer creation failed.");
        return false;
    }
    impl->api = Impl::Api::DX12;
    impl->FinishSetup();
    impl_ = impl.release();
    HBE_INFO("EffekseerBackend: DX12 VFX runtime initialised.");
    return true;
}

bool EffekseerBackend::InitVulkan(void* physicalDevice, void* device, void* queue, void* commandPool,
                                  const u32* rtFormatsVk, int colorAttachmentCount, u32 depthFormatVk,
                                  int swapBufferCount) {
#ifdef HBE_EFFEKSEER_VK
    if (impl_) return true;
    if (!physicalDevice || !device || !queue || !commandPool || !rtFormatsVk) return false;
    auto impl = std::make_unique<Impl>();
    ::EffekseerRendererVulkan::RenderPassInformation rp;
    rp.DoesPresentToScreen = false;
    rp.DepthFormat = static_cast<VkFormat>(depthFormatVk);
    const int n = colorAttachmentCount < 1 ? 1 : (colorAttachmentCount > 8 ? 8 : colorAttachmentCount);
    for (int i = 0; i < 8; ++i)
        rp.RenderTextureFormats[i] =
            i < n ? static_cast<VkFormat>(rtFormatsVk[i]) : VK_FORMAT_UNDEFINED;
    impl->renderer = ::EffekseerRendererVulkan::Create(
        static_cast<VkPhysicalDevice>(physicalDevice), static_cast<VkDevice>(device),
        static_cast<VkQueue>(queue), static_cast<VkCommandPool>(commandPool), swapBufferCount, rp,
        Impl::kSquareMax);
    if (impl->renderer == nullptr) {
        HBE_ERROR("EffekseerBackend: Vulkan renderer creation failed.");
        return false;
    }
    impl->api = Impl::Api::Vulkan;
    impl->FinishSetup();
    impl_ = impl.release();
    HBE_INFO("EffekseerBackend: Vulkan VFX runtime initialised.");
    return true;
#else
    (void)physicalDevice; (void)device; (void)queue; (void)commandPool; (void)rtFormatsVk;
    (void)depthFormatVk; (void)colorAttachmentCount; (void)swapBufferCount;
    return false;
#endif
}

void EffekseerBackend::Shutdown() {
    if (!impl_) return;
    // Ref-counted Effekseer objects release in destruction order; clear effects first.
    impl_->effects.clear();
    impl_->effectByPath.clear();
    impl_->commandList.Reset();
    impl_->memoryPool.Reset();
    impl_->manager.Reset();
    impl_->renderer.Reset();
    delete impl_;
    impl_ = nullptr;
}

bool EffekseerBackend::Available() const { return impl_ != nullptr; }

u32 EffekseerBackend::LoadEffect(const std::string& path) {
    if (!impl_ || path.empty()) return 0;
    if (auto it = impl_->effectByPath.find(path); it != impl_->effectByPath.end()) return it->second;
    // Read the effect bytes through the VFS (a mounted pack in a shipped build, disk in the editor)
    // and create from memory. A self-contained `.efkefc` needs nothing else; a `.efk`'s external
    // textures/models resolve through the VFS file interface, using the effect's directory as base.
    const std::filesystem::path fspath(path);
    auto bytes = ::hbe::vfs::ReadFile(fspath);
    if (!bytes || bytes->empty()) {
        HBE_WARN("EffekseerBackend: effect file not found '{}'.", path);
        return 0;
    }
    const std::u16string baseDir = fspath.parent_path().u16string() + u"/";
    ::Effekseer::EffectRef effect = ::Effekseer::Effect::Create(
        impl_->manager, bytes->data(), static_cast<int32_t>(bytes->size()), 1.0f, baseDir.c_str());
    if (effect == nullptr) {
        HBE_WARN("EffekseerBackend: failed to parse effect '{}'.", path);
        return 0;
    }
    const u32 id = static_cast<u32>(impl_->effects.size());
    impl_->effects.push_back(effect);
    impl_->effectByPath.emplace(path, id);
    return id;
}

int EffekseerBackend::Play(u32 effectId, const glm::vec3& pos) {
    if (!impl_ || effectId == 0 || effectId >= impl_->effects.size()) return -1;
    const ::Effekseer::EffectRef& e = impl_->effects[effectId];
    if (e == nullptr) return -1;
    return impl_->manager->Play(e, pos.x, pos.y, pos.z);
}

void EffekseerBackend::Stop(int handle) {
    if (impl_ && handle >= 0) impl_->manager->StopEffect(handle);
}
void EffekseerBackend::StopAll() {
    if (impl_) impl_->manager->StopAllEffects();
}
void EffekseerBackend::SetLocation(int handle, const glm::vec3& pos) {
    if (impl_ && handle >= 0) impl_->manager->SetLocation(handle, pos.x, pos.y, pos.z);
}
bool EffekseerBackend::Exists(int handle) const {
    return impl_ && handle >= 0 && impl_->manager->Exists(handle);
}

void EffekseerBackend::Update(f32 dt) {
    if (!impl_) return;
    impl_->time += dt;
    impl_->manager->Update(dt * 60.0f); // Effekseer counts in 1/60s frames
}

void EffekseerBackend::Draw(void* d3d12CommandList, const glm::mat4& view, const glm::mat4& proj) {
    if (!impl_ || !d3d12CommandList) return;
    if (impl_->manager->GetTotalInstanceCount() == 0) return; // nothing live -> skip the pass

    impl_->memoryPool->NewFrame();
    // Wrap the caller's NATIVE command list/buffer in Effekseer's CommandList (backend-specific).
    if (impl_->api == Impl::Api::DX12) {
        ::EffekseerRendererDX12::BeginCommandList(
            impl_->commandList, static_cast<ID3D12GraphicsCommandList*>(d3d12CommandList));
    }
#ifdef HBE_EFFEKSEER_VK
    else {
        ::EffekseerRendererVulkan::BeginCommandList(
            impl_->commandList, static_cast<VkCommandBuffer>(d3d12CommandList));
    }
#endif
    impl_->renderer->SetCommandList(impl_->commandList);
    impl_->renderer->SetTime(impl_->time);
    impl_->renderer->SetCameraMatrix(Impl::ToEfk(view));
    impl_->renderer->SetProjectionMatrix(Impl::ToEfk(proj));
    impl_->renderer->BeginRendering();
    impl_->manager->Draw();
    impl_->renderer->EndRendering();
    impl_->renderer->SetCommandList(nullptr);
    if (impl_->api == Impl::Api::DX12) {
        ::EffekseerRendererDX12::EndCommandList(impl_->commandList);
    }
#ifdef HBE_EFFEKSEER_VK
    else {
        ::EffekseerRendererVulkan::EndCommandList(impl_->commandList);
    }
#endif
}

int EffekseerBackend::LiveInstanceCount() const {
    return impl_ ? impl_->manager->GetTotalInstanceCount() : 0;
}

} // namespace hbe::vfx

#else // !HBE_HAVE_EFFEKSEER - inert stubs so the engine builds + runs without the dependency.

namespace hbe::vfx {
struct EffekseerBackend::Impl {};
EffekseerBackend::EffekseerBackend() = default;
EffekseerBackend::~EffekseerBackend() = default;
bool EffekseerBackend::Init(void*, void*, u32, u32, bool, int) { return false; }
bool EffekseerBackend::InitVulkan(void*, void*, void*, void*, const u32*, int, u32, int) {
    return false;
}
void EffekseerBackend::Shutdown() {}
bool EffekseerBackend::Available() const { return false; }
u32 EffekseerBackend::LoadEffect(const std::string&) { return 0; }
int EffekseerBackend::Play(u32, const glm::vec3&) { return -1; }
void EffekseerBackend::Stop(int) {}
void EffekseerBackend::StopAll() {}
void EffekseerBackend::SetLocation(int, const glm::vec3&) {}
bool EffekseerBackend::Exists(int) const { return false; }
void EffekseerBackend::Update(f32) {}
void EffekseerBackend::Draw(void*, const glm::mat4&, const glm::mat4&) {}
int EffekseerBackend::LiveInstanceCount() const { return 0; }
} // namespace hbe::vfx

#endif
