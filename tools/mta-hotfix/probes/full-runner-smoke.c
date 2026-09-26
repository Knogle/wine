/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Exercise installed-runner paths that file/API regression probes cannot cover.
 * Creating a real HAL D3D9 device catches missing 32-bit graphics libraries;
 * enumerating audio and opening an unconnected socket catch missing subsystems
 * without playing sound, recording, or contacting a network endpoint.
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <d3d9.h>
#include <mmsystem.h>
#include <dsound.h>
#include <stdio.h>

static unsigned int failures;

static void json_string(const char *value)
{
    const unsigned char *p = (const unsigned char *)value;

    putchar('"');
    for (; *p; ++p)
    {
        if (*p == '"' || *p == '\\')
            printf("\\%c", *p);
        else if (*p < 0x20 || *p >= 0x7f)
            printf("\\u%04x", *p);
        else
            putchar(*p);
    }
    putchar('"');
}

static void result(const char *test, BOOL passed, unsigned long code, BOOL required)
{
    printf("{\"test\":\"%s\",\"status\":\"%s\",\"code\":\"0x%08lx\",\"required\":%s}\n",
           test, passed ? "pass" : "fail", code, required ? "true" : "false");
    if (!passed && required)
        ++failures;
}

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    return DefWindowProcA(window, message, wparam, lparam);
}

static void test_d3d9(void)
{
    static const char class_name[] = "MTAFullRunnerSmoke";
    WNDCLASSA window_class = {0};
    D3DPRESENT_PARAMETERS parameters = {0};
    D3DADAPTER_IDENTIFIER9 identifier;
    D3DCAPS9 caps;
    IDirect3D9 *d3d = NULL;
    IDirect3DDevice9 *device = NULL;
    HINSTANCE instance = GetModuleHandleA(NULL);
    HWND window = NULL;
    MSG message;
    HRESULT hr;
    BOOL registered;

    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.lpszClassName = class_name;
    registered = RegisterClassA(&window_class) != 0;
    result("window_class", registered, registered ? 0 : GetLastError(), TRUE);
    if (!registered)
        return;

    window = CreateWindowExA(0, class_name, "Wine runner smoke test", WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, 160, 120, NULL, NULL, instance, NULL);
    result("window", window != NULL, window ? 0 : GetLastError(), TRUE);
    if (!window)
        goto done;
    ShowWindow(window, SW_SHOWNOACTIVATE);
    UpdateWindow(window);

    d3d = Direct3DCreate9(D3D_SDK_VERSION);
    result("d3d9_create", d3d != NULL, d3d ? 0 : (unsigned long)E_FAIL, TRUE);
    if (!d3d)
        goto done;

    printf("{\"test\":\"d3d9_adapter_count\",\"count\":%u}\n", IDirect3D9_GetAdapterCount(d3d));
    hr = IDirect3D9_GetAdapterIdentifier(d3d, D3DADAPTER_DEFAULT, 0, &identifier);
    result("d3d9_adapter_identifier", SUCCEEDED(hr), (unsigned long)hr, TRUE);
    if (SUCCEEDED(hr))
    {
        printf("{\"test\":\"d3d9_adapter\",\"description\":");
        json_string(identifier.Description);
        printf(",\"driver\":");
        json_string(identifier.Driver);
        printf(",\"device\":");
        json_string(identifier.DeviceName);
        printf(",\"vendor_id\":\"0x%04lx\",\"device_id\":\"0x%04lx\","
               "\"driver_version_high\":\"0x%08lx\",\"driver_version_low\":\"0x%08lx\"}\n",
               (unsigned long)identifier.VendorId, (unsigned long)identifier.DeviceId,
               (unsigned long)identifier.DriverVersion.HighPart,
               (unsigned long)identifier.DriverVersion.LowPart);
    }

    hr = IDirect3D9_GetDeviceCaps(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &caps);
    result("d3d9_hal_caps", SUCCEEDED(hr), (unsigned long)hr, TRUE);
    if (SUCCEEDED(hr))
        printf("{\"test\":\"d3d9_caps\",\"vertex_shader\":\"0x%08lx\","
               "\"pixel_shader\":\"0x%08lx\",\"max_texture_width\":%lu,"
               "\"max_texture_height\":%lu}\n",
               (unsigned long)caps.VertexShaderVersion, (unsigned long)caps.PixelShaderVersion,
               (unsigned long)caps.MaxTextureWidth, (unsigned long)caps.MaxTextureHeight);

    parameters.BackBufferWidth = 64;
    parameters.BackBufferHeight = 64;
    parameters.BackBufferFormat = D3DFMT_UNKNOWN;
    parameters.BackBufferCount = 1;
    parameters.SwapEffect = D3DSWAPEFFECT_DISCARD;
    parameters.hDeviceWindow = window;
    parameters.Windowed = TRUE;
    parameters.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    /* Do not fall back to REF: that could hide a broken graphics installation. */
    hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
                                D3DCREATE_SOFTWARE_VERTEXPROCESSING, &parameters, &device);
    result("d3d9_hal_device", SUCCEEDED(hr), (unsigned long)hr, TRUE);
    if (FAILED(hr))
        goto done;

    hr = IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(25, 70, 100), 1.0f, 0);
    result("d3d9_clear", SUCCEEDED(hr), (unsigned long)hr, TRUE);
    hr = IDirect3DDevice9_Present(device, NULL, NULL, NULL, NULL);
    result("d3d9_present", SUCCEEDED(hr), (unsigned long)hr, TRUE);

    /* Drain messages without an interactive loop or a timed wait. */
    while (PeekMessageA(&message, window, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }

done:
    if (device)
        IDirect3DDevice9_Release(device);
    if (d3d)
        IDirect3D9_Release(d3d);
    if (window)
        DestroyWindow(window);
    UnregisterClassA(class_name, instance);
}

static BOOL CALLBACK enumerate_directsound(LPGUID guid, LPCSTR description, LPCSTR module, LPVOID context)
{
    unsigned int *count = context;

    ++*count;
    printf("{\"test\":\"directsound_device\",\"default\":%s,\"description\":", guid ? "false" : "true");
    json_string(description ? description : "");
    printf(",\"module\":");
    json_string(module ? module : "");
    printf("}\n");
    return TRUE;
}

static void test_audio(void)
{
    UINT i, count = waveOutGetNumDevs();
    unsigned int directsound_count = 0;
    WAVEOUTCAPSA wave_caps;
    DSCAPS ds_caps = {0};
    IDirectSound8 *directsound = NULL;
    MMRESULT mm;
    HRESULT hr;

    printf("{\"test\":\"waveout_count\",\"count\":%u}\n", count);
    result("waveout_enumeration", count > 0, count ? MMSYSERR_NOERROR : MMSYSERR_NODRIVER, TRUE);
    for (i = 0; i < count; ++i)
    {
        mm = waveOutGetDevCapsA(i, &wave_caps, sizeof(wave_caps));
        result("waveout_caps", mm == MMSYSERR_NOERROR, mm, TRUE);
        if (mm == MMSYSERR_NOERROR)
        {
            printf("{\"test\":\"waveout_device\",\"index\":%u,\"name\":", i);
            json_string(wave_caps.szPname);
            printf(",\"channels\":%u,\"formats\":\"0x%08lx\"}\n",
                   (unsigned int)wave_caps.wChannels, (unsigned long)wave_caps.dwFormats);
        }
    }

    hr = DirectSoundEnumerateA(enumerate_directsound, &directsound_count);
    result("directsound_enumeration", SUCCEEDED(hr) && directsound_count > 0, (unsigned long)hr, TRUE);
    /* GetCaps checks the default output without allocating or playing a buffer. */
    hr = DirectSoundCreate8(NULL, &directsound, NULL);
    result("directsound_default", SUCCEEDED(hr), (unsigned long)hr, TRUE);
    if (SUCCEEDED(hr))
    {
        ds_caps.dwSize = sizeof(ds_caps);
        hr = IDirectSound8_GetCaps(directsound, &ds_caps);
        result("directsound_default_caps", SUCCEEDED(hr), (unsigned long)hr, TRUE);
        if (SUCCEEDED(hr))
            printf("{\"test\":\"directsound_caps\",\"flags\":\"0x%08lx\","
                   "\"min_secondary_rate\":%lu,\"max_secondary_rate\":%lu}\n",
                   (unsigned long)ds_caps.dwFlags, (unsigned long)ds_caps.dwMinSecondarySampleRate,
                   (unsigned long)ds_caps.dwMaxSecondarySampleRate);
        IDirectSound8_Release(directsound);
    }
}

static void test_winsock(void)
{
    WSADATA data;
    SOCKET sock;
    int status = WSAStartup(MAKEWORD(2, 2), &data);

    result("winsock_startup", status == 0, (unsigned long)status, TRUE);
    if (status)
        return;
    /* No bind, send, receive or connect: this only initializes the socket path. */
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    result("winsock_socket", sock != INVALID_SOCKET,
           sock == INVALID_SOCKET ? (unsigned long)WSAGetLastError() : 0, TRUE);
    if (sock != INVALID_SOCKET)
    {
        status = closesocket(sock);
        result("winsock_close", status == 0, status ? (unsigned long)WSAGetLastError() : 0, TRUE);
    }
    status = WSACleanup();
    result("winsock_cleanup", status == 0, status ? (unsigned long)WSAGetLastError() : 0, TRUE);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    test_d3d9();
    test_audio();
    test_winsock();
    printf("{\"test\":\"summary\",\"status\":\"%s\",\"failures\":%u}\n",
           failures ? "fail" : "pass", failures);
    return failures ? 1 : 0;
}
