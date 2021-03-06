#include "window_renderer.h"
#include "pointer_updater.h"

#include "renderer.h"

#include "MaskedPixelShader.h"

#include <array>
#include <gsl.h>
#include <memory>

using error = renderer::error;

void window_renderer::init(init_args &&args) {
    dx_m.emplace(std::move(args));
    windowHandle_m = args.windowHandle;

    RECT rect;
    GetClientRect(windowHandle_m, &rect);
    size_m = rectSize(rect);
}

void window_renderer::reset() noexcept { dx_m.reset(); }

bool window_renderer::resize(SIZE size) noexcept {
    if (size.cx == size_m.cx && size.cy == size_m.cy) return false;
    size_m = size;
    pendingResizeBuffers_m = true;
    return true;
}

void window_renderer::setZoom(float zoom) noexcept {
    if (zoom_m == zoom) return;
    zoom_m = zoom;
}

void window_renderer::moveOffset(POINT delta) noexcept {
    if (delta.x == 0 && delta.y == 0) return;
    const auto offset = vec2f{offset_m.x + static_cast<float>(delta.x) / zoom_m,
                              offset_m.y + static_cast<float>(delta.y) / zoom_m};
    offset_m = offset;
}

void window_renderer::moveToBorder(int x, int y) {
    auto next = offset_m;

    const auto &dx = *dx_m;
    D3D11_TEXTURE2D_DESC texture_description;
    dx.backgroundTexture_m->GetDesc(&texture_description);

    if (0 > x)
        next.x = 0;
    else if (0 < x)
        next.x = -static_cast<float>(texture_description.Width) + (size_m.cx / zoom_m);

    if (0 > y)
        next.y = 0;
    else if (0 < y)
        next.y = -static_cast<float>(texture_description.Height) + (size_m.cy / zoom_m);

    moveTo(next);
}

void window_renderer::moveTo(vec2f offset) noexcept {
    if (offset.x == offset_m.x && offset.y == offset_m.y) return;
    offset_m = offset;
}

void window_renderer::render() {
    auto &dx = *dx_m;
    if (pendingResizeBuffers_m) {
        pendingResizeBuffers_m = false;
        dx.renderTarget_m.Reset();
        resizeSwapBuffer();
        dx.createRenderTarget();
    }

    dx.clearRenderTarget({0, 0, 0, 0});

    dx.deviceContext()->GenerateMips(dx.backgroundTextureShaderResource_m.Get());

    setViewPort();
    dx.activateRenderTarget();

    dx.activateLinearSampler();
    dx.activateVertexShader();
    dx.activateDiscreteSampler();
    dx.activatePlainPixelShader();
    dx.activateBackgroundTexture();
    dx.activateNoBlendState();
    dx.activateTriangleList();
    dx.activateBackgroundVertexBuffer();
    dx.deviceContext()->Draw(6, 0);
}

void window_renderer::renderPointer(const pointer_buffer &pointer) {
    if (pointer.position_timestamp == 0) return;
    updatePointerShape(pointer);
    if (!pointer.visible) return;
    updatePointerVertices(pointer);

    auto &dx = *dx_m;
    dx.activatePointerVertexBuffer();

    if (pointer.shape_info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR) {
        dx.activateAlphaBlendState();
        dx.activatePlainPixelShader();
        dx.activatePointerTexture();
    }
    else {
        const auto index = 1;
        // dx.activateNoBlendState(); -- already set
        dx.activatePointerTexture(index);
        dx.activateDiscreteSampler(index);
        dx.activateMaskedPixelShader();
    }

    dx.deviceContext()->Draw(6, 0);
}

void window_renderer::swap() {
    const auto &dx = *dx_m;
    dx.activateNoRenderTarget();

    const auto result = dx.swapChain_m->Present(1, 0);
    if (IS_ERROR(result)) throw error{result, "Failed to swap buffers"};
}

void window_renderer::resizeSwapBuffer() {
    const auto &dx = *dx_m;
    DXGI_SWAP_CHAIN_DESC description;
    dx.swapChain_m->GetDesc(&description);
    const auto result = dx.swapChain_m->ResizeBuffers(
        description.BufferCount,
        size_m.cx,
        size_m.cy,
        description.BufferDesc.Format,
        description.Flags);
    if (IS_ERROR(result)) throw error{result, "Failed to resize swap buffers"};
}

void window_renderer::setViewPort() {
    const auto &dx = *dx_m;
    D3D11_TEXTURE2D_DESC texture_description;
    dx.backgroundTexture_m->GetDesc(&texture_description);

    D3D11_VIEWPORT view_port;
    view_port.Width = static_cast<float>(texture_description.Width) * zoom_m;
    view_port.Height = static_cast<float>(texture_description.Height) * zoom_m;
    view_port.MinDepth = 0.0f;
    view_port.MaxDepth = 1.0f;
    view_port.TopLeftX = static_cast<float>(offset_m.x) * zoom_m;
    view_port.TopLeftY = static_cast<float>(offset_m.y) * zoom_m;

    dx.deviceContext()->RSSetViewports(1, &view_port);
}

void window_renderer::updatePointerShape(const pointer_buffer &pointer) {
    if (pointer.shape_timestamp == lastPointerShapeUpdate_m) return;
    lastPointerShapeUpdate_m = pointer.shape_timestamp;

    D3D11_TEXTURE2D_DESC texture_description;
    texture_description.Width = pointer.shape_info.Width;
    texture_description.Height = pointer.shape_info.Height;
    texture_description.MipLevels = 1;
    texture_description.ArraySize = 1;
    texture_description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texture_description.SampleDesc.Count = 1;
    texture_description.SampleDesc.Quality = 0;
    texture_description.Usage = D3D11_USAGE_DEFAULT;
    texture_description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    texture_description.CPUAccessFlags = 0;
    texture_description.MiscFlags = 0;

    D3D11_SUBRESOURCE_DATA resource_data;
    resource_data.pSysMem = pointer.shape_data.data();
    resource_data.SysMemPitch = pointer.shape_info.Pitch;
    resource_data.SysMemSlicePitch = 0;

    // convert monochrome to masked colors
    using color = std::array<uint8_t, 4>;
    std::vector<color> tmpData;
    if (pointer.shape_info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) {
        const auto width = texture_description.Width;
        const auto height = texture_description.Height >> 1;
        const auto pitch = pointer.shape_info.Pitch;
        tmpData.resize(width * height);
        for (auto row = 0u; row < height; ++row) {
            for (auto col = 0u; col < width; ++col) {
                const auto mask = 0x80 >> (col & 7);
                const auto addr = (col >> 3) + (row * pitch);
                const auto and_mask = pointer.shape_data[addr] & mask;
                const auto xor_mask = pointer.shape_data[addr + height * pitch] & mask;
                const auto pixel = and_mask ? (xor_mask ? color{{0xFF, 0xFF, 0xFF, 0xFF}}
                                                        : color{{0x00, 0x00, 0x00, 0xFF}})
                                            : (xor_mask ? color{{0xFF, 0xFF, 0xFF, 0x00}}
                                                        : color{{0x00, 0x00, 0x00, 0x00}});
                tmpData[row * width + col] = pixel;
            }
        }

        texture_description.Height = height;
        resource_data.pSysMem = tmpData.data();
        resource_data.SysMemPitch = width * sizeof(color);
    }
    //{
    //	auto width = texture_description.Width;
    //	auto height = texture_description.Height;
    //	auto pitch = resource_data.SysMemPitch;
    //	const uint8_t* data = reinterpret_cast<const uint8_t*>(resource_data.pSysMem);
    //	for (auto row = 0u; row < height; ++row) {
    //		for (auto col = 0u; col < width; ++col) {
    //			auto addr = (col * 4) + (row * pitch);
    //			auto color = reinterpret_cast<const uint32_t&>(data[addr]);
    //			char ch[2] = {'u', '\0'};
    //			switch (color) {
    //			case 0x00000000: ch[0] = 'b'; break;
    //			case 0x00FFFFFF: ch[0] = 'w'; break;
    //			case 0xFF000000: ch[0] = ' '; break;
    //			case 0xFFFFFFFF: ch[0] = 'x'; break;
    //			default:
    //				if (color & 0xFF000000) ch[0] = 'n';
    //			}
    //			OutputDebugStringA(ch);
    //		}
    //		OutputDebugStringA("\n");
    //	}
    //}

    auto &dx = *dx_m;
    auto result =
        dx.device()->CreateTexture2D(&texture_description, &resource_data, &dx.pointerTexture_m);

    D3D11_SHADER_RESOURCE_VIEW_DESC shader_resource_description;
    shader_resource_description.Format = texture_description.Format;
    shader_resource_description.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    shader_resource_description.Texture2D.MostDetailedMip = texture_description.MipLevels - 1;
    shader_resource_description.Texture2D.MipLevels = texture_description.MipLevels;

    result = dx.device()->CreateShaderResourceView(
        dx.pointerTexture_m.Get(),
        &shader_resource_description,
        &dx.pointerTextureShaderResource_m);
}

void window_renderer::updatePointerVertices(const pointer_buffer &pointer) {
    if (pointer.position_timestamp == lastPointerPositionUpdate_m) return;
    lastPointerPositionUpdate_m = pointer.position_timestamp;

    const auto &dx = *dx_m;

    vertex vertices[] = {vertex{-1.0f, -1.0f, 0.0f, 1.0f},
                         vertex{-1.0f, 1.0f, 0.0f, 0.0f},
                         vertex{1.0f, -1.0f, 1.0f, 1.0f},
                         vertex{1.0f, -1.0f, 1.0f, 1.0f},
                         vertex{-1.0f, 1.0f, 0.0f, 0.0f},
                         vertex{1.0f, 1.0f, 1.0f, 0.0f}};

    D3D11_TEXTURE2D_DESC texture_description;
    dx.backgroundTexture_m->GetDesc(&texture_description);

    const auto texture_size = SIZE{gsl::narrow_cast<int>(texture_description.Width),
                                   gsl::narrow_cast<int>(texture_description.Height)};
    const auto center = POINT{texture_size.cx / 2, texture_size.cy / 2};

    const auto position = pointer.position;

    const auto mouse_to_desktop = [&](vertex &v, int x, int y) {
        v.x = (position.x + x - center.x) / static_cast<float>(center.x);
        v.y = -1 * (position.y + y - center.y) / static_cast<float>(center.y);
    };

    const auto isMonochrome = pointer.shape_info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME;
    const auto size =
        SIZE{gsl::narrow_cast<int>(pointer.shape_info.Width),
             gsl::narrow_cast<int>(
                 isMonochrome ? pointer.shape_info.Height / 2 : pointer.shape_info.Height)};
    mouse_to_desktop(vertices[0], 0, size.cy);
    mouse_to_desktop(vertices[1], 0, 0);
    mouse_to_desktop(vertices[2], size.cx, size.cy);
    mouse_to_desktop(vertices[3], size.cx, size.cy);
    mouse_to_desktop(vertices[4], 0, 0);
    mouse_to_desktop(vertices[5], size.cx, 0);

    dx.deviceContext()->UpdateSubresource(
        dx.pointerVertexBuffer_m.Get(), 0, nullptr, &vertices[0], 0, 0);
}

window_renderer::resources::resources(window_renderer::init_args &&args)
    : base_renderer(std::move(args))
    , backgroundTexture_m(std::move(args.texture)) {

    createBackgroundTextureShaderResource();
    createBackgroundVertexBuffer();
    createSwapChain(args.windowHandle);
    createRenderTarget();
    createMaskedPixelShader();
    createLinearSamplerState();
    createPointerVertexBuffer();
}

void window_renderer::resources::createBackgroundTextureShaderResource() {
    D3D11_TEXTURE2D_DESC texture_description;
    backgroundTexture_m->GetDesc(&texture_description);

    D3D11_SHADER_RESOURCE_VIEW_DESC shader_description;
    shader_description.Format = texture_description.Format;
    shader_description.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    shader_description.Texture2D.MostDetailedMip = texture_description.MipLevels - 1;
    shader_description.Texture2D.MipLevels = texture_description.MipLevels;

    const auto result = device()->CreateShaderResourceView(
        backgroundTexture_m.Get(), nullptr, &backgroundTextureShaderResource_m);
    if (IS_ERROR(result)) throw error{result, "Failed to create shader resource"};
}

void window_renderer::resources::createBackgroundVertexBuffer() {
    static const vertex vertices[] = {vertex{-1.0f, -1.0f, 0.0f, 1.0f},
                                      vertex{-1.0f, 1.0f, 0.0f, 0.0f},
                                      vertex{1.0f, -1.0f, 1.0f, 1.0f},
                                      vertex{1.0f, -1.0f, 1.0f, 1.0f},
                                      vertex{-1.0f, 1.0f, 0.0f, 0.0f},
                                      vertex{1.0f, 1.0f, 1.0f, 0.0f}};

    D3D11_BUFFER_DESC buffer_description;
    RtlZeroMemory(&buffer_description, sizeof(buffer_description));
    buffer_description.Usage = D3D11_USAGE_DEFAULT;
    buffer_description.ByteWidth = sizeof(vertices);
    buffer_description.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    buffer_description.CPUAccessFlags = 0;

    D3D11_SUBRESOURCE_DATA init_data;
    RtlZeroMemory(&init_data, sizeof(init_data));
    init_data.pSysMem = &vertices[0];

    const auto result =
        device()->CreateBuffer(&buffer_description, &init_data, &backgroundVertexBuffer_m);
    if (IS_ERROR(result)) throw error{result, "Failed to create vertex buffer"};
}

void window_renderer::resources::createSwapChain(HWND windowHandle) {
    auto factory = renderer::getFactory(device());
    swapChain_m = renderer::createSwapChain(factory, device(), windowHandle);
}

void window_renderer::resources::createRenderTarget() {
    ComPtr<ID3D11Texture2D> back_buffer;
    const auto buffer = 0;
    auto result = swapChain_m->GetBuffer(buffer, __uuidof(ID3D11Texture2D), &back_buffer);
    if (IS_ERROR(result)) throw error{result, "Failed to get backbuffer"};

    const D3D11_RENDER_TARGET_VIEW_DESC *render_target_description = nullptr;
    result = device()->CreateRenderTargetView(
        back_buffer.Get(), render_target_description, &renderTarget_m);
    if (IS_ERROR(result)) throw error{result, "Failed to create render target for backbuffer"};
}

void window_renderer::resources::createMaskedPixelShader() {
    const auto size = ARRAYSIZE(g_MaskedPixelShader);
    auto shader = &g_MaskedPixelShader[0];
    ID3D11ClassLinkage *linkage = nullptr;
    const auto result = device()->CreatePixelShader(shader, size, linkage, &maskedPixelShader_m);
    if (IS_ERROR(result)) throw error{result, "Failed to create pixel shader"};
}

void window_renderer::resources::createLinearSamplerState() {
    linearSamplerState_m = createLinearSampler();
}

void window_renderer::resources::createPointerVertexBuffer() {
    D3D11_BUFFER_DESC buffer_description;
    RtlZeroMemory(&buffer_description, sizeof(buffer_description));
    buffer_description.Usage = D3D11_USAGE_DEFAULT;
    buffer_description.ByteWidth = 6 * sizeof(vertex);
    buffer_description.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    buffer_description.CPUAccessFlags = 0;

    const auto result =
        device()->CreateBuffer(&buffer_description, nullptr, &pointerVertexBuffer_m);
    if (IS_ERROR(result)) throw error{result, "Failed to create vertex buffer"};
}
