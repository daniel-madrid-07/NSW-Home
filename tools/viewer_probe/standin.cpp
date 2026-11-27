// Stand-in for an emulator window, used by probe.cpp to settle how a DWM
// thumbnail of a covered / cloaked window behaves before trying a real one.
//
//   standin.exe <gl|vk> <x> <y> <w> <h> [popup] [novsync]
//
// A top-level window (magenta client, so any of it that reaches the screen is
// obvious) with one child of class "NativeWindow-standin" holding a swap chain.
// Framed: the child sits 60 px below the top of the client and stops 30 px
// above its bottom, like a render surface between a menu bar and a status
// bar. popup: no frame, the child fills the window (an emulator's full screen).
// Every frame is one flat colour that encodes the frame number (red = low
// byte, green = high byte, blue = 0x55), so a single screen pixel says which
// frame is on screen; the gl mode also draws a 64x64 one-pixel checkerboard in
// the top-left corner to tell exact 1:1 copies from scaled ones.
// State for the probe is in the shared memory "Local\NSWHomeStandin".
#define WIN32_LEAN_AND_MEAN
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <gl/GL.h>
#include <shellapi.h>
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

struct Shared {
    volatile LONG frame;     // last frame handed to the swap chain
    volatile LONG fpsTimes10; // presents in the last full second, x10
    volatile LONG keys;      // WM_KEYDOWN received by the top-level window or the child
    volatile LONG clicks;    // WM_LBUTTONDOWN received anywhere
    volatile LONG active;    // 1 while the top-level window is the active one
    volatile LONG framed;    // 1 = switch to the framed layout, 0 = popup (set by the probe)
    volatile LONGLONG qpc;   // time of that frame
};
static Shared* shared;
static HWND top, child;
static bool popup, vsync = true;
static int winW, winH;

static void Layout() {
    RECT rc;
    GetClientRect(top, &rc);
    const bool framed = (GetWindowLongW(top, GWL_STYLE) & WS_CAPTION) != 0;
    if (framed) MoveWindow(child, 0, 60, rc.right, rc.bottom - 90, TRUE);
    else MoveWindow(child, 0, 0, rc.right, rc.bottom, TRUE);
}

static LRESULT CALLBACK TopProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_KEYDOWN:
        InterlockedIncrement(&shared->keys);
        if (w == VK_F11) { // "leave / enter full screen", as an emulator would
            const bool framed = (GetWindowLongW(h, GWL_STYLE) & WS_CAPTION) != 0;
            SetWindowLongW(h, GWL_STYLE, (framed ? WS_POPUP : WS_OVERLAPPEDWINDOW) | WS_VISIBLE | WS_CLIPCHILDREN);
            SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
            Layout();
        }
        return 0;
    case WM_LBUTTONDOWN: InterlockedIncrement(&shared->clicks); return 0;
    case WM_ACTIVATE: shared->active = LOWORD(w) != WA_INACTIVE; break;
    case WM_SIZE: Layout(); return 0;
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(h, &rc);
        HBRUSH b = CreateSolidBrush(RGB(255, 0, 255));
        FillRect(reinterpret_cast<HDC>(w), &rc, b);
        DeleteObject(b);
        return 1;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static LRESULT CALLBACK ChildProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_LBUTTONDOWN) InterlockedIncrement(&shared->clicks);
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_PAINT) {
        ValidateRect(h, nullptr);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void Tick(LONG n) {
    static LARGE_INTEGER freq, since;
    static LONG count;
    LARGE_INTEGER now;
    if (!freq.QuadPart) {
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&since);
    }
    QueryPerformanceCounter(&now);
    shared->qpc = now.QuadPart;
    shared->frame = n;
    count++;
    if (now.QuadPart - since.QuadPart >= freq.QuadPart) {
        shared->fpsTimes10 = static_cast<LONG>(count * 10.0 * freq.QuadPart / (now.QuadPart - since.QuadPart));
        count = 0;
        since = now;
    }
}

static volatile bool running = true;

static DWORD WINAPI GlThread(void*) {
    HDC dc = GetDC(child);
    PIXELFORMATDESCRIPTOR pfd = {sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32};
    SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd);
    HGLRC rc = wglCreateContext(dc);
    wglMakeCurrent(dc, rc);
    using SwapFn = BOOL(WINAPI*)(int);
    if (auto swap = reinterpret_cast<SwapFn>(wglGetProcAddress("wglSwapIntervalEXT"))) swap(vsync ? 1 : 0);
    static unsigned char checker[64 * 64 * 4];
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            unsigned char v = ((x ^ y) & 1) ? 255 : 0;
            unsigned char* p = checker + (y * 64 + x) * 4;
            p[0] = p[1] = p[2] = v, p[3] = 255;
        }
    for (LONG n = 1; running; n++) {
        RECT r;
        GetClientRect(child, &r);
        glViewport(0, 0, r.right, r.bottom);
        glClearColor((n & 255) / 255.0f, ((n >> 8) & 255) / 255.0f, 0x55 / 255.0f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0, r.right, 0, r.bottom, -1, 1);
        glRasterPos2i(0, r.bottom - 64); // top-left corner
        glDrawPixels(64, 64, GL_RGBA, GL_UNSIGNED_BYTE, checker);
        SwapBuffers(dc);
        Tick(n);
    }
    return 0;
}

#define VKC(x)                                                                                                         \
    if ((x) != VK_SUCCESS) {                                                                                           \
        OutputDebugStringA("vk fail: " #x "\n");                                                                       \
        ExitProcess(3);                                                                                                \
    }

static DWORD WINAPI VkThread(void*) {
    VkInstance inst;
    const char* iext[] = {"VK_KHR_surface", "VK_KHR_win32_surface"};
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = iext;
    VKC(vkCreateInstance(&ici, nullptr, &inst));
    VkWin32SurfaceCreateInfoKHR sci = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    sci.hinstance = GetModuleHandleW(nullptr);
    sci.hwnd = child;
    VkSurfaceKHR surf;
    VKC(vkCreateWin32SurfaceKHR(inst, &sci, nullptr, &surf));
    uint32_t n = 8;
    VkPhysicalDevice pds[8];
    vkEnumeratePhysicalDevices(inst, &n, pds);
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    uint32_t qf = 0;
    for (uint32_t i = 0; i < n && !pd; i++) {
        uint32_t qn = 16;
        VkQueueFamilyProperties qp[16];
        vkGetPhysicalDeviceQueueFamilyProperties(pds[i], &qn, qp);
        for (uint32_t q = 0; q < qn; q++) {
            VkBool32 ok = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(pds[i], q, surf, &ok);
            if (ok && (qp[q].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                pd = pds[i], qf = q;
                break;
            }
        }
    }
    if (!pd) ExitProcess(4);
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = qf;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char* dext[] = {"VK_KHR_swapchain"};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = dext;
    VkDevice dev;
    VKC(vkCreateDevice(pd, &dci, nullptr, &dev));
    VkQueue queue;
    vkGetDeviceQueue(dev, qf, 0, &queue);
    VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = qf;
    VkCommandPool pool;
    VKC(vkCreateCommandPool(dev, &cpi, nullptr, &pool));
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    VKC(vkAllocateCommandBuffers(dev, &cai, &cmd));
    VkSemaphoreCreateInfo sem = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore acquired, done;
    vkCreateSemaphore(dev, &sem, nullptr, &acquired);
    vkCreateSemaphore(dev, &sem, nullptr, &done);

    VkSwapchainKHR sc = VK_NULL_HANDLE;
    std::vector<VkImage> images;
    VkExtent2D extent = {};
    auto build = [&] {
        vkDeviceWaitIdle(dev);
        VkSurfaceCapabilitiesKHR caps;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surf, &caps);
        extent = caps.currentExtent;
        if (!extent.width || !extent.height) return false;
        VkSwapchainCreateInfoKHR s = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        s.surface = surf;
        s.minImageCount = caps.minImageCount < 3 ? 3 : caps.minImageCount;
        s.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
        s.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        s.imageExtent = extent;
        s.imageArrayLayers = 1;
        s.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        s.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        s.preTransform = caps.currentTransform;
        s.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        s.presentMode = vsync ? VK_PRESENT_MODE_FIFO_KHR : VK_PRESENT_MODE_IMMEDIATE_KHR;
        s.clipped = VK_TRUE;
        s.oldSwapchain = sc;
        VkSwapchainKHR fresh;
        if (vkCreateSwapchainKHR(dev, &s, nullptr, &fresh) != VK_SUCCESS) return false;
        if (sc) vkDestroySwapchainKHR(dev, sc, nullptr);
        sc = fresh;
        uint32_t c = 0;
        vkGetSwapchainImagesKHR(dev, sc, &c, nullptr);
        images.resize(c);
        vkGetSwapchainImagesKHR(dev, sc, &c, images.data());
        return true;
    };
    build();
    for (LONG f = 1; running; f++) {
        if (!sc && !build()) {
            Sleep(10);
            continue;
        }
        uint32_t idx = 0;
        VkResult r = vkAcquireNextImageKHR(dev, sc, UINT64_MAX, acquired, VK_NULL_HANDLE, &idx);
        if (r == VK_ERROR_OUT_OF_DATE_KHR) {
            build();
            continue;
        }
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(cmd, &bi);
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = images[idx];
        b.subresourceRange = range;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        VkClearColorValue col = {{(f & 255) / 255.0f, ((f >> 8) & 255) / 255.0f, 0x55 / 255.0f, 1.0f}};
        vkCmdClearColorImage(cmd, images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &col, 1, &range);
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = 0;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        vkEndCommandBuffer(cmd);
        VkPipelineStageFlags wait = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &acquired;
        si.pWaitDstStageMask = &wait;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &done;
        vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
        VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &done;
        pi.swapchainCount = 1;
        pi.pSwapchains = &sc;
        pi.pImageIndices = &idx;
        r = vkQueuePresentKHR(queue, &pi);
        vkQueueWaitIdle(queue);
        Tick(f);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) build();
        else {
            VkSurfaceCapabilitiesKHR caps;
            vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surf, &caps);
            if (caps.currentExtent.width != extent.width || caps.currentExtent.height != extent.height) build();
        }
    }
    return 0;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc < 6) return 2;
    const bool vk = !wcscmp(argv[1], L"vk");
    int x = _wtoi(argv[2]), y = _wtoi(argv[3]);
    winW = _wtoi(argv[4]), winH = _wtoi(argv[5]);
    for (int i = 6; i < argc; i++) {
        if (!wcscmp(argv[i], L"popup")) popup = true;
        if (!wcscmp(argv[i], L"novsync")) vsync = false;
    }
    HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Shared), L"Local\\NSWHomeStandin");
    shared = static_cast<Shared*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
    if (!shared) return 5;
    memset(shared, 0, sizeof(Shared));

    WNDCLASSW wc = {};
    wc.lpfnWndProc = TopProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"NSWHomeStandinTop";
    RegisterClassW(&wc);
    wc.lpfnWndProc = ChildProc;
    wc.style = CS_OWNDC;
    wc.lpszClassName = L"NativeWindow-standin";
    RegisterClassW(&wc);
    // Shown without being activated: the probe runs beside other work.
    top = CreateWindowExW(WS_EX_NOACTIVATE * 0, L"NSWHomeStandinTop", L"Standin 1.0 - title bar", (popup ? WS_POPUP : WS_OVERLAPPEDWINDOW) | WS_CLIPCHILDREN, x, y,
                          winW, winH, nullptr, nullptr, inst, nullptr);
    child = CreateWindowExW(0, L"NativeWindow-standin", L"NativeWindow", WS_CHILD | WS_VISIBLE, 0, 0, 16, 16, top, nullptr, inst, nullptr);
    Layout();
    ShowWindow(top, SW_SHOWNOACTIVATE);
    HANDLE th = CreateThread(nullptr, 0, vk ? VkThread : GlThread, nullptr, 0, nullptr);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    running = false;
    WaitForSingleObject(th, 2000);
    return 0;
}
