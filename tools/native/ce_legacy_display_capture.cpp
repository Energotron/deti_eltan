#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

template <typename T>
static void ce_release(T *&value) {
    if (value != nullptr) {
        value->Release();
        value = nullptr;
    }
}

static bool ce_frame_is_near_black(
    const unsigned char *pixels, UINT width, UINT height, UINT row_pitch
) {
    unsigned maximum = 0;
    unsigned minimum = 255;
    const UINT step_x = width > 64u ? width / 64u : 1u;
    const UINT step_y = height > 64u ? height / 64u : 1u;
    for (UINT y = 0; y < height; y += step_y) {
        const unsigned char *row = pixels + static_cast<size_t>(y) * row_pitch;
        for (UINT x = 0; x < width; x += step_x) {
            for (UINT channel = 0; channel < 3u; ++channel) {
                const unsigned value = row[x * 4u + channel];
                if (value > maximum) maximum = value;
                if (value < minimum) minimum = value;
            }
        }
    }
    return maximum <= 4u && maximum - minimum <= 4u;
}

static bool ce_write_bmp(
    const wchar_t *path,
    const unsigned char *pixels,
    UINT width,
    UINT height,
    UINT row_pitch,
    bool top_down
) {
    BITMAPFILEHEADER file_header{};
    BITMAPINFOHEADER info_header{};
    const DWORD row_bytes = width * 4u;
    const DWORD image_bytes = row_bytes * height;
    DWORD written = 0;
    HANDLE file = INVALID_HANDLE_VALUE;

    file_header.bfType = 0x4D42;
    file_header.bfOffBits = sizeof(file_header) + sizeof(info_header);
    file_header.bfSize = file_header.bfOffBits + image_bytes;
    info_header.biSize = sizeof(info_header);
    info_header.biWidth = static_cast<LONG>(width);
    info_header.biHeight = static_cast<LONG>(height);
    info_header.biPlanes = 1;
    info_header.biBitCount = 32;
    info_header.biCompression = BI_RGB;
    info_header.biSizeImage = image_bytes;

    file = CreateFileW(
        path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr
    );
    if (file == INVALID_HANDLE_VALUE) return false;
    if (!WriteFile(file, &file_header, sizeof(file_header), &written, nullptr) ||
            written != sizeof(file_header) ||
            !WriteFile(file, &info_header, sizeof(info_header), &written, nullptr) ||
            written != sizeof(info_header)) {
        CloseHandle(file);
        return false;
    }
    for (UINT output_row = 0; output_row < height; ++output_row) {
        const UINT source_row = top_down ? height - 1u - output_row : output_row;
        const unsigned char *row = pixels + static_cast<size_t>(source_row) * row_pitch;
        if (!WriteFile(file, row, row_bytes, &written, nullptr) || written != row_bytes) {
            CloseHandle(file);
            return false;
        }
    }
    if (!FlushFileBuffers(file)) {
        CloseHandle(file);
        return false;
    }
    CloseHandle(file);
    return true;
}

static HRESULT ce_find_output(
    UINT requested_index,
    IDXGIAdapter1 **selected_adapter,
    IDXGIOutput **selected_output
) {
    IDXGIFactory1 *factory = nullptr;
    UINT global_index = 0;
    HRESULT result = CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory));
    if (FAILED(result)) return result;
    for (UINT adapter_index = 0;; ++adapter_index) {
        IDXGIAdapter1 *adapter = nullptr;
        result = factory->EnumAdapters1(adapter_index, &adapter);
        if (result == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(result)) {
            ce_release(factory);
            return result;
        }
        for (UINT output_index = 0;; ++output_index) {
            IDXGIOutput *output = nullptr;
            result = adapter->EnumOutputs(output_index, &output);
            if (result == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(result)) {
                ce_release(adapter);
                ce_release(factory);
                return result;
            }
            if (global_index == requested_index) {
                *selected_adapter = adapter;
                *selected_output = output;
                ce_release(factory);
                return S_OK;
            }
            ++global_index;
            ce_release(output);
        }
        ce_release(adapter);
    }
    ce_release(factory);
    return DXGI_ERROR_NOT_FOUND;
}

static HRESULT ce_capture_dxgi(
    const wchar_t *path,
    UINT output_index,
    UINT timeout_ms,
    UINT *captured_width,
    UINT *captured_height
) {
    IDXGIAdapter1 *adapter = nullptr;
    IDXGIOutput *output = nullptr;
    IDXGIOutput1 *output1 = nullptr;
    IDXGIOutputDuplication *duplication = nullptr;
    ID3D11Device *device = nullptr;
    ID3D11DeviceContext *context = nullptr;
    IDXGIResource *resource = nullptr;
    ID3D11Texture2D *desktop = nullptr;
    ID3D11Texture2D *staging = nullptr;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    DXGI_OUTDUPL_FRAME_INFO frame_info{};
    D3D11_TEXTURE2D_DESC description{};
    bool frame_acquired = false;
    HRESULT result = ce_find_output(output_index, &adapter, &output);
    if (FAILED(result)) return result;

    D3D_FEATURE_LEVEL requested[] = {
        D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
    };
    D3D_FEATURE_LEVEL obtained{};
    result = D3D11CreateDevice(
        adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, requested,
        static_cast<UINT>(sizeof(requested) / sizeof(requested[0])),
        D3D11_SDK_VERSION, &device, &obtained, &context
    );
    if (FAILED(result)) goto cleanup;
    result = output->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void **>(&output1));
    if (FAILED(result)) goto cleanup;
    result = output1->DuplicateOutput(device, &duplication);
    if (FAILED(result)) goto cleanup;
    result = duplication->AcquireNextFrame(timeout_ms, &frame_info, &resource);
    if (FAILED(result)) goto cleanup;
    frame_acquired = true;
    result = resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&desktop));
    if (FAILED(result)) goto cleanup;

    desktop->GetDesc(&description);
    if (description.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        result = DXGI_ERROR_UNSUPPORTED;
        goto cleanup;
    }
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    description.MiscFlags = 0;
    description.ArraySize = 1;
    description.MipLevels = 1;
    result = device->CreateTexture2D(&description, nullptr, &staging);
    if (FAILED(result)) goto cleanup;
    context->CopyResource(staging, desktop);
    result = context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(result)) goto cleanup;
    if (ce_frame_is_near_black(
            static_cast<const unsigned char *>(mapped.pData),
            description.Width, description.Height, mapped.RowPitch)) {
        result = DXGI_ERROR_UNSUPPORTED;
    } else if (!ce_write_bmp(
            path, static_cast<const unsigned char *>(mapped.pData),
            description.Width, description.Height, mapped.RowPitch, true)) {
        result = HRESULT_FROM_WIN32(GetLastError() == 0 ? ERROR_WRITE_FAULT : GetLastError());
    } else {
        *captured_width = description.Width;
        *captured_height = description.Height;
        result = S_OK;
    }
    context->Unmap(staging, 0);

cleanup:
    if (frame_acquired) duplication->ReleaseFrame();
    ce_release(staging);
    ce_release(desktop);
    ce_release(resource);
    ce_release(duplication);
    ce_release(output1);
    ce_release(context);
    ce_release(device);
    ce_release(output);
    ce_release(adapter);
    return result;
}

struct ce_monitor_selection {
    UINT wanted;
    UINT current;
    HMONITOR found;
};

static BOOL CALLBACK ce_monitor_by_index(HMONITOR monitor, HDC, LPRECT, LPARAM value) {
    auto *selection = reinterpret_cast<ce_monitor_selection *>(value);
    if (selection->current == selection->wanted) {
        selection->found = monitor;
        return FALSE;
    }
    ++selection->current;
    return TRUE;
}

static HRESULT ce_capture_gdi(
    const wchar_t *path,
    UINT output_index,
    UINT *captured_width,
    UINT *captured_height
) {
    ce_monitor_selection selection{output_index, 0, nullptr};
    MONITORINFO monitor_info{};
    HDC screen = nullptr;
    HDC memory = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ old_bitmap = nullptr;
    void *pixels = nullptr;
    HRESULT result = E_FAIL;

    monitor_info.cbSize = sizeof(monitor_info);
    EnumDisplayMonitors(nullptr, nullptr, ce_monitor_by_index, reinterpret_cast<LPARAM>(&selection));
    if (selection.found == nullptr || !GetMonitorInfoW(selection.found, &monitor_info)) {
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }
    const int width = monitor_info.rcMonitor.right - monitor_info.rcMonitor.left;
    const int height = monitor_info.rcMonitor.bottom - monitor_info.rcMonitor.top;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    screen = GetDC(nullptr);
    if (screen == nullptr) goto cleanup;
    memory = CreateCompatibleDC(screen);
    if (memory == nullptr) goto cleanup;
    bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (bitmap == nullptr || pixels == nullptr) goto cleanup;
    old_bitmap = SelectObject(memory, bitmap);
    if (old_bitmap == nullptr || old_bitmap == HGDI_ERROR) goto cleanup;
    if (!BitBlt(
            memory, 0, 0, width, height, screen,
            monitor_info.rcMonitor.left, monitor_info.rcMonitor.top,
            SRCCOPY | CAPTUREBLT)) goto cleanup;
    GdiFlush();
    if (!ce_write_bmp(
            path, static_cast<const unsigned char *>(pixels),
            static_cast<UINT>(width), static_cast<UINT>(height),
            static_cast<UINT>(width) * 4u, true)) goto cleanup;
    *captured_width = static_cast<UINT>(width);
    *captured_height = static_cast<UINT>(height);
    result = S_OK;

cleanup:
    if (old_bitmap != nullptr && old_bitmap != HGDI_ERROR) SelectObject(memory, old_bitmap);
    if (bitmap != nullptr) DeleteObject(bitmap);
    if (memory != nullptr) DeleteDC(memory);
    if (screen != nullptr) ReleaseDC(nullptr, screen);
    return result;
}

static void ce_usage(const wchar_t *program) {
    fwprintf(
        stderr,
        L"Usage: %ls --output FILE.bmp [--monitor N] [--timeout MS] [--gdi-only]\n",
        program
    );
}

int wmain(int argc, wchar_t **argv) {
    const wchar_t *output_path = nullptr;
    UINT monitor_index = 0;
    UINT timeout_ms = 2000;
    bool gdi_only = false;
    UINT width = 0;
    UINT height = 0;
    HRESULT dxgi_result = E_FAIL;
    HRESULT result;
    const char *method;

    for (int index = 1; index < argc; ++index) {
        if (wcscmp(argv[index], L"--output") == 0 && index + 1 < argc) {
            output_path = argv[++index];
        } else if (wcscmp(argv[index], L"--monitor") == 0 && index + 1 < argc) {
            monitor_index = static_cast<UINT>(wcstoul(argv[++index], nullptr, 10));
        } else if (wcscmp(argv[index], L"--timeout") == 0 && index + 1 < argc) {
            timeout_ms = static_cast<UINT>(wcstoul(argv[++index], nullptr, 10));
        } else if (wcscmp(argv[index], L"--gdi-only") == 0) {
            gdi_only = true;
        } else {
            ce_usage(argv[0]);
            return 2;
        }
    }
    if (output_path == nullptr || *output_path == L'\0') {
        ce_usage(argv[0]);
        return 2;
    }

    if (!gdi_only) {
        dxgi_result = ce_capture_dxgi(
            output_path, monitor_index, timeout_ms, &width, &height
        );
    }
    if (SUCCEEDED(dxgi_result)) {
        result = dxgi_result;
        method = "dxgi";
    } else {
        result = ce_capture_gdi(output_path, monitor_index, &width, &height);
        method = "gdi";
    }
    if (FAILED(result)) {
        fprintf(
            stderr,
            "FAIL: capture error=0x%08lX dxgi=0x%08lX\n",
            static_cast<unsigned long>(result),
            static_cast<unsigned long>(dxgi_result)
        );
        return 3;
    }
    printf(
        "{\"schema\":1,\"method\":\"%s\",\"monitor\":%u,"
        "\"width\":%u,\"height\":%u,\"output_written\":true}\n",
        method, monitor_index, width, height
    );
    return 0;
}
