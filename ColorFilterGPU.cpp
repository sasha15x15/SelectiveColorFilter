#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <string>
#include <cmath>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")

// Глобальные переменные
HWND g_hWnd = NULL;
HWND g_hColorButton, g_hEyedropperButton;
HWND g_hTrackbarThreshold, g_hTrackbarSmoothing;
HWND g_hLabelColor, g_hLabelThreshold, g_hLabelSmoothing;
HWND g_hButtonStart, g_hButtonStop;
HWND g_hColorList, g_hAddColorButton, g_hRemoveColorButton;
HWND g_hVSyncCheckbox, g_hGrayscaleCombo;
HWND g_hSaveButton, g_hLoadButton;
std::vector<HWND> g_hCheckboxes; // Чекбоксы для каждого монитора
std::vector<HWND> g_hFPSLabels; // FPS лейблы для каждого монитора

#define MAX_COLORS 10

COLORREF g_currentColor = RGB(255, 0, 0);
std::vector<COLORREF> g_targetColors;
int g_thresholdPercent = 20;
int g_smoothingPercent = 10;

bool g_isRunning = false;
bool g_isDragging = false;
bool g_shouldStopRenderThread = false;
bool g_vsyncEnabled = true;
int g_grayscaleMode = 1; // 0=Average, 1=Luminosity, 2=HD, 3=Desaturation, 4=Max, 5=Green
HANDLE g_renderThread = NULL;

// Структура для каждого монитора
struct MonitorData {
    HWND hOverlayWnd;
    ID3D11Device* pDevice;
    ID3D11DeviceContext* pContext;
    IDXGIOutputDuplication* pDuplication;
    ID3D11Texture2D* pDesktopTexture;
    IDXGISwapChain* pSwapChain;
    ID3D11RenderTargetView* pRenderTargetView;
    ID3D11ShaderResourceView* pTextureSRV;
    ID3D11Buffer* pConstantBuffer;
    ID3D11VertexShader* pVertexShader;
    ID3D11PixelShader* pPixelShader;
    ID3D11Buffer* pVertexBuffer;
    ID3D11InputLayout* pInputLayout;
    ID3D11SamplerState* pSamplerState;
    int screenWidth;
    int screenHeight;
    RECT monitorRect;
    wchar_t deviceName[32];
    wchar_t displayName[256];
    bool isSelected;
    bool isActive;
    bool constantsNeedUpdate;
    DWORD frameCount;
    DWORD lastFPSTime;
    float currentFPS;
    
    MonitorData() {
        hOverlayWnd = NULL;
        pDevice = nullptr;
        pContext = nullptr;
        pDuplication = nullptr;
        pDesktopTexture = nullptr;
        pSwapChain = nullptr;
        pRenderTargetView = nullptr;
        pTextureSRV = nullptr;
        pConstantBuffer = nullptr;
        pVertexShader = nullptr;
        pPixelShader = nullptr;
        pVertexBuffer = nullptr;
        pInputLayout = nullptr;
        pSamplerState = nullptr;
        screenWidth = 0;
        screenHeight = 0;
        monitorRect = {0};
        deviceName[0] = L'\0';
        displayName[0] = L'\0';
        isSelected = false;
        isActive = false;
        constantsNeedUpdate = true;
        frameCount = 0;
        lastFPSTime = 0;
        currentFPS = 0.0f;
    }
};

std::vector<MonitorData> g_monitors;

// FPS счетчик - удален, теперь в MonitorData

// Общие DirectX объекты - удалены, теперь в MonitorData

struct FilterConstants {
    int numColors;
    int thresholdSquared;
    int smoothingRange;
    int grayscaleMode;
    int targetColors[MAX_COLORS][4]; // int4 для каждого цвета (RGB + padding)
};

struct Vertex {
    float pos[3];
    float tex[2];
};

// Шейдеры
const char* g_vertexShaderSource = R"(
struct VS_INPUT {
    float3 pos : POSITION;
    float2 tex : TEXCOORD0;
};

struct VS_OUTPUT {
    float4 pos : SV_POSITION;
    float2 tex : TEXCOORD0;
};

VS_OUTPUT VSMain(VS_INPUT input) {
    VS_OUTPUT output;
    output.pos = float4(input.pos, 1.0f);
    output.tex = input.tex;
    return output;
}
)";

const char* g_pixelShaderSource = R"(
cbuffer FilterConstants : register(b0) {
    int numColors;
    int thresholdSquared;
    int smoothingRange;
    int grayscaleMode;
    int4 targetColors[10];
};

Texture2D screenTexture : register(t0);
SamplerState texSampler : register(s0);

struct PS_INPUT {
    float4 pos : SV_POSITION;
    float2 tex : TEXCOORD0;
};

float4 PSMain(PS_INPUT input) : SV_TARGET {
    float4 color = screenTexture.Sample(texSampler, input.tex);
    
    // Конвертируем в 0-255 диапазон
    int r = (int)(color.r * 255.0f);
    int g = (int)(color.g * 255.0f);
    int b = (int)(color.b * 255.0f);
    
    float bestFactor = 0.0f;
    
    // Цикл по всем выбранным цветам
    for (int i = 0; i < numColors && i < 10; i++) {
        int dr = r - targetColors[i].x;
        int dg = g - targetColors[i].y;
        int db = b - targetColors[i].z;
        int distSquared = dr*dr + dg*dg + db*db;
        
        float currentFactor = 1.0f;
        
        if (distSquared > thresholdSquared) {
            int smoothingEnd = thresholdSquared + smoothingRange;
            if (distSquared < smoothingEnd) {
                currentFactor = 1.0f - (float)(distSquared - thresholdSquared) / (float)smoothingRange;
                currentFactor = max(0.0f, min(1.0f, currentFactor));
            } else {
                currentFactor = 0.0f;
            }
        }
        
        // Берем максимальный фактор
        bestFactor = max(bestFactor, currentFactor);
    }
    
    // Применяем финальный коэффициент
    if (bestFactor > 0.99f) {
        return color;
    } else if (bestFactor < 0.01f) {
        // Различные режимы оттенков серого
        float gray;
        if (grayscaleMode == 0) {
            // Average
            gray = (r + g + b) / 3.0f;
        } else if (grayscaleMode == 1) {
            // Luminosity (BT.601)
            gray = r * 0.299f + g * 0.587f + b * 0.114f;
        } else if (grayscaleMode == 2) {
            // HD (BT.709)
            gray = r * 0.2126f + g * 0.7152f + b * 0.0722f;
        } else if (grayscaleMode == 3) {
            // Desaturation
            gray = (max(max(r, g), b) + min(min(r, g), b)) / 2.0f;
        } else if (grayscaleMode == 4) {
            // Max
            gray = max(max(r, g), b);
        } else {
            // Green channel
            gray = g;
        }
        return float4(gray/255.0f, gray/255.0f, gray/255.0f, color.a);
    } else {
        // Смешивание с учетом режима серого
        float gray;
        if (grayscaleMode == 0) {
            gray = (r + g + b) / 3.0f;
        } else if (grayscaleMode == 1) {
            gray = r * 0.299f + g * 0.587f + b * 0.114f;
        } else if (grayscaleMode == 2) {
            gray = r * 0.2126f + g * 0.7152f + b * 0.0722f;
        } else if (grayscaleMode == 3) {
            gray = (max(max(r, g), b) + min(min(r, g), b)) / 2.0f;
        } else if (grayscaleMode == 4) {
            gray = max(max(r, g), b);
        } else {
            gray = g;
        }
        float4 grayColor = float4(gray/255.0f, gray/255.0f, gray/255.0f, color.a);
        return lerp(grayColor, color, bestFactor);
    }
}
)";

// Forward declarations
LRESULT CALLBACK OverlayWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
void UpdateLabels();
void PopulateMonitorList();
void StartEyedropper();
void ShowColorPicker();
void AddColorToList();
void RemoveColorFromList();
void UpdateColorList();
void SaveSettings();
void LoadSettings();
void SaveSettingsToFile();
void LoadSettingsFromFile();
void RenderFrameForMonitor(MonitorData& monitor);
void StartFilter();
void StopFilter();
void CleanupDirectX();
bool InitDirectXForMonitor(MonitorData& monitor);
bool InitShadersForMonitor(MonitorData& monitor);
bool InitSwapChainForMonitor(MonitorData& monitor);
DWORD WINAPI RenderThreadProc(LPVOID lpParam);

DWORD WINAPI RenderThreadProc(LPVOID lpParam) {
    while (!g_shouldStopRenderThread) {
        if (g_isRunning) {
            for (auto& monitor : g_monitors) {
                if (monitor.isActive) {
                    RenderFrameForMonitor(monitor);
                }
            }
            // Никаких Sleep() - максимальная производительность
        } else {
            Sleep(16);
        }
    }
    return 0;
}

bool InitDirectXForMonitor(MonitorData& monitor) {
    HRESULT hr;
    IDXGIFactory1* pFactory = nullptr;
    hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&pFactory);
    if (FAILED(hr)) return false;
    
    IDXGIAdapter1* pSelectedAdapter = nullptr;
    IDXGIOutput* pSelectedOutput = nullptr;
    bool found = false;
    
    for (UINT adapterIdx = 0; pFactory->EnumAdapters1(adapterIdx, &pSelectedAdapter) != DXGI_ERROR_NOT_FOUND; ++adapterIdx) {
        for (UINT outputIdx = 0; pSelectedAdapter->EnumOutputs(outputIdx, &pSelectedOutput) != DXGI_ERROR_NOT_FOUND; ++outputIdx) {
            DXGI_OUTPUT_DESC outDesc;
            pSelectedOutput->GetDesc(&outDesc);
            
            if (wcscmp(outDesc.DeviceName, monitor.deviceName) == 0) {
                found = true;
                break;
            }
            pSelectedOutput->Release();
            pSelectedOutput = nullptr;
        }
        if (found) break;
        pSelectedAdapter->Release();
        pSelectedAdapter = nullptr;
    }
    
    pFactory->Release();
    if (!found) return false;
    
    D3D_FEATURE_LEVEL featureLevel;
    hr = D3D11CreateDevice(pSelectedAdapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &monitor.pDevice, &featureLevel, &monitor.pContext);
    
    pSelectedAdapter->Release();
    if (FAILED(hr)) {
        pSelectedOutput->Release();
        return false;
    }
    
    IDXGIOutput1* pDXGIOutput1 = nullptr;
    hr = pSelectedOutput->QueryInterface(__uuidof(IDXGIOutput1), (void**)&pDXGIOutput1);
    pSelectedOutput->Release();
    if (FAILED(hr)) return false;
    
    hr = pDXGIOutput1->DuplicateOutput(monitor.pDevice, &monitor.pDuplication);
    pDXGIOutput1->Release();
    
    return SUCCEEDED(hr);
}

bool InitShadersForMonitor(MonitorData& monitor) {
    HRESULT hr;
    ID3DBlob* pVSBlob = nullptr;
    ID3DBlob* pErrorBlob = nullptr;
    
    hr = D3DCompile(g_vertexShaderSource, strlen(g_vertexShaderSource),
        nullptr, nullptr, nullptr, "VSMain", "vs_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS, 0, &pVSBlob, &pErrorBlob);
    
    if (FAILED(hr)) {
        if (pErrorBlob) pErrorBlob->Release();
        return false;
    }
    
    hr = monitor.pDevice->CreateVertexShader(pVSBlob->GetBufferPointer(),
        pVSBlob->GetBufferSize(), nullptr, &monitor.pVertexShader);
    
    if (FAILED(hr)) {
        pVSBlob->Release();
        return false;
    }
    
    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };
    
    hr = monitor.pDevice->CreateInputLayout(layout, 2, pVSBlob->GetBufferPointer(),
        pVSBlob->GetBufferSize(), &monitor.pInputLayout);
    pVSBlob->Release();
    
    if (FAILED(hr)) return false;
    
    ID3DBlob* pPSBlob = nullptr;
    hr = D3DCompile(g_pixelShaderSource, strlen(g_pixelShaderSource),
        nullptr, nullptr, nullptr, "PSMain", "ps_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS, 0, &pPSBlob, &pErrorBlob);
    
    if (FAILED(hr)) {
        if (pErrorBlob) pErrorBlob->Release();
        return false;
    }
    
    hr = monitor.pDevice->CreatePixelShader(pPSBlob->GetBufferPointer(),
        pPSBlob->GetBufferSize(), nullptr, &monitor.pPixelShader);
    pPSBlob->Release();
    
    if (FAILED(hr)) return false;
    
    Vertex vertices[] = {
        { {-1.0f, -1.0f, 0.0f}, {0.0f, 1.0f} },
        { {-1.0f,  1.0f, 0.0f}, {0.0f, 0.0f} },
        { { 1.0f, -1.0f, 0.0f}, {1.0f, 1.0f} },
        { { 1.0f,  1.0f, 0.0f}, {1.0f, 0.0f} }
    };
    
    D3D11_BUFFER_DESC bufferDesc = {};
    bufferDesc.Usage = D3D11_USAGE_DEFAULT;
    bufferDesc.ByteWidth = sizeof(vertices);
    bufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    
    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = vertices;
    
    hr = monitor.pDevice->CreateBuffer(&bufferDesc, &initData, &monitor.pVertexBuffer);
    if (FAILED(hr)) return false;
    
    D3D11_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samplerDesc.MinLOD = 0;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    
    hr = monitor.pDevice->CreateSamplerState(&samplerDesc, &monitor.pSamplerState);
    if (FAILED(hr)) return false;
    
    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth = sizeof(FilterConstants);
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    
    hr = monitor.pDevice->CreateBuffer(&cbDesc, nullptr, &monitor.pConstantBuffer);
    return SUCCEEDED(hr);
}

bool InitSwapChainForMonitor(MonitorData& monitor) {
    IDXGIDevice* pDXGIDevice = nullptr;
    HRESULT hr = monitor.pDevice->QueryInterface(__uuidof(IDXGIDevice), (void**)&pDXGIDevice);
    if (FAILED(hr)) return false;
    
    IDXGIAdapter* pDXGIAdapter = nullptr;
    hr = pDXGIDevice->GetAdapter(&pDXGIAdapter);
    pDXGIDevice->Release();
    if (FAILED(hr)) return false;
    
    IDXGIFactory* pDXGIFactory = nullptr;
    hr = pDXGIAdapter->GetParent(__uuidof(IDXGIFactory), (void**)&pDXGIFactory);
    pDXGIAdapter->Release();
    if (FAILED(hr)) return false;
    
    DXGI_SWAP_CHAIN_DESC swapChainDesc = {};
    swapChainDesc.BufferCount = 1;
    swapChainDesc.BufferDesc.Width = monitor.screenWidth;
    swapChainDesc.BufferDesc.Height = monitor.screenHeight;
    swapChainDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferDesc.RefreshRate.Numerator = 0;
    swapChainDesc.BufferDesc.RefreshRate.Denominator = 0;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.OutputWindow = monitor.hOverlayWnd;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.Windowed = TRUE;
    
    hr = pDXGIFactory->CreateSwapChain(monitor.pDevice, &swapChainDesc, &monitor.pSwapChain);
    pDXGIFactory->Release();
    if (FAILED(hr)) return false;
    
    ID3D11Texture2D* pBackBuffer = nullptr;
    hr = monitor.pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&pBackBuffer);
    if (FAILED(hr)) return false;
    
    hr = monitor.pDevice->CreateRenderTargetView(pBackBuffer, nullptr, &monitor.pRenderTargetView);
    pBackBuffer->Release();
    
    return SUCCEEDED(hr);
}

void CleanupDirectX() {
    for (auto& monitor : g_monitors) {
        if (monitor.pRenderTargetView) { monitor.pRenderTargetView->Release(); monitor.pRenderTargetView = nullptr; }
        if (monitor.pSwapChain) { monitor.pSwapChain->Release(); monitor.pSwapChain = nullptr; }
        if (monitor.pTextureSRV) { monitor.pTextureSRV->Release(); monitor.pTextureSRV = nullptr; }
        if (monitor.pConstantBuffer) { monitor.pConstantBuffer->Release(); monitor.pConstantBuffer = nullptr; }
        if (monitor.pDesktopTexture) { monitor.pDesktopTexture->Release(); monitor.pDesktopTexture = nullptr; }
        if (monitor.pDuplication) { monitor.pDuplication->Release(); monitor.pDuplication = nullptr; }
        if (monitor.pSamplerState) { monitor.pSamplerState->Release(); monitor.pSamplerState = nullptr; }
        if (monitor.pVertexBuffer) { monitor.pVertexBuffer->Release(); monitor.pVertexBuffer = nullptr; }
        if (monitor.pInputLayout) { monitor.pInputLayout->Release(); monitor.pInputLayout = nullptr; }
        if (monitor.pPixelShader) { monitor.pPixelShader->Release(); monitor.pPixelShader = nullptr; }
        if (monitor.pVertexShader) { monitor.pVertexShader->Release(); monitor.pVertexShader = nullptr; }
        if (monitor.pContext) { monitor.pContext->Release(); monitor.pContext = nullptr; }
        if (monitor.pDevice) { monitor.pDevice->Release(); monitor.pDevice = nullptr; }
        if (monitor.hOverlayWnd) { DestroyWindow(monitor.hOverlayWnd); monitor.hOverlayWnd = NULL; }
    }
}



void PopulateMonitorList() {
    g_monitors.clear();
    g_hCheckboxes.clear();
    g_hFPSLabels.clear();
    
    struct MonitorEnumData {
        std::vector<MonitorData>* pMonitors;
    } data = { &g_monitors };
    
    auto MonitorEnumProc = [](HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData) -> BOOL {
        MonitorEnumData* pData = (MonitorEnumData*)dwData;
        
        MONITORINFOEXW mi;
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(hMonitor, &mi)) {
            MonitorData monitor;
            monitor.monitorRect = mi.rcMonitor;
            monitor.screenWidth = mi.rcMonitor.right - mi.rcMonitor.left;
            monitor.screenHeight = mi.rcMonitor.bottom - mi.rcMonitor.top;
            wcscpy_s(monitor.deviceName, mi.szDevice);
            
            DEVMODEW devMode;
            devMode.dmSize = sizeof(devMode);
            int refreshRate = 60;
            if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &devMode)) {
                refreshRate = devMode.dmDisplayFrequency;
            }
            
            if (mi.dwFlags & MONITORINFOF_PRIMARY) {
                swprintf_s(monitor.displayName, L"Primary - %dx%d @%dHz", 
                    monitor.screenWidth, monitor.screenHeight, refreshRate);
            } else {
                swprintf_s(monitor.displayName, L"Monitor %d - %dx%d @%dHz", 
                    (int)pData->pMonitors->size() + 1, monitor.screenWidth, monitor.screenHeight, refreshRate);
            }
            
            pData->pMonitors->push_back(monitor);
        }
        return TRUE;
    };
    
    EnumDisplayMonitors(NULL, NULL, MonitorEnumProc, (LPARAM)&data);
    
    if (g_monitors.empty()) {
        MonitorData monitor;
        monitor.monitorRect = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
        monitor.screenWidth = GetSystemMetrics(SM_CXSCREEN);
        monitor.screenHeight = GetSystemMetrics(SM_CYSCREEN);
        wcscpy_s(monitor.deviceName, L"\\\\.\\DISPLAY1");
        wcscpy_s(monitor.displayName, L"Primary Monitor");
        g_monitors.push_back(monitor);
    }
}
void UpdateLabels() {
    wchar_t buffer[128];
    swprintf_s(buffer, L"Current Color: RGB(%d, %d, %d)", 
        GetRValue(g_currentColor), GetGValue(g_currentColor), GetBValue(g_currentColor));
    SetWindowTextW(g_hLabelColor, buffer);
    
    swprintf_s(buffer, L"Threshold: %d%%", g_thresholdPercent);
    SetWindowTextW(g_hLabelThreshold, buffer);
    
    swprintf_s(buffer, L"Smoothing: %d%%", g_smoothingPercent);
    SetWindowTextW(g_hLabelSmoothing, buffer);
}

void RemoveColorFromList() {
    int selected = (int)SendMessage(g_hColorList, LB_GETCURSEL, 0, 0);
    if (selected >= 0 && selected < (int)g_targetColors.size()) {
        g_targetColors.erase(g_targetColors.begin() + selected);
        UpdateColorList();
        
        // Помечаем все активные мониторы для обновления констант
        for (auto& monitor : g_monitors) {
            if (monitor.isActive) {
                monitor.constantsNeedUpdate = true;
            }
        }
    }
}

void UpdateColorList() {
    SendMessage(g_hColorList, LB_RESETCONTENT, 0, 0);
    
    for (size_t i = 0; i < g_targetColors.size(); i++) {
        wchar_t buffer[64];
        swprintf_s(buffer, L"RGB(%d, %d, %d)", 
            GetRValue(g_targetColors[i]), GetGValue(g_targetColors[i]), GetBValue(g_targetColors[i]));
        SendMessage(g_hColorList, LB_ADDSTRING, 0, (LPARAM)buffer);
    }
    
    // Принудительно перерисовываем список
    InvalidateRect(g_hColorList, NULL, TRUE);
}

void AddColorToList() {
    // Проверяем что такого цвета еще нет
    for (const auto& existing : g_targetColors) {
        if (existing == g_currentColor) {
            return; // Цвет уже есть
        }
    }
    
    // Проверяем лимит
    if (g_targetColors.size() >= MAX_COLORS) {
        MessageBoxW(g_hWnd, L"Maximum 10 colors allowed", L"Limit", MB_OK);
        return;
    }
    
    g_targetColors.push_back(g_currentColor);
    UpdateColorList();
    
    // Помечаем все активные мониторы для обновления констант
    for (auto& monitor : g_monitors) {
        if (monitor.isActive) {
            monitor.constantsNeedUpdate = true;
        }
    }
}

void SaveSettings() {
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"SOFTWARE\\ColorFilterGPU", 0, NULL, 0, KEY_WRITE, NULL, &hKey, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(hKey, L"ThresholdPercent", 0, REG_DWORD, (BYTE*)&g_thresholdPercent, sizeof(DWORD));
        RegSetValueExW(hKey, L"SmoothingPercent", 0, REG_DWORD, (BYTE*)&g_smoothingPercent, sizeof(DWORD));
        RegSetValueExW(hKey, L"VSyncEnabled", 0, REG_DWORD, (BYTE*)&g_vsyncEnabled, sizeof(DWORD));
        RegSetValueExW(hKey, L"GrayscaleMode", 0, REG_DWORD, (BYTE*)&g_grayscaleMode, sizeof(DWORD));
        
        // Сохраняем цвета
        DWORD colorCount = (DWORD)g_targetColors.size();
        RegSetValueExW(hKey, L"ColorCount", 0, REG_DWORD, (BYTE*)&colorCount, sizeof(DWORD));
        if (colorCount > 0) {
            RegSetValueExW(hKey, L"Colors", 0, REG_BINARY, (BYTE*)g_targetColors.data(), colorCount * sizeof(COLORREF));
        }
        
        RegCloseKey(hKey);
    }
}

void LoadSettings() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"SOFTWARE\\ColorFilterGPU", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD size = sizeof(DWORD);
        RegQueryValueExW(hKey, L"ThresholdPercent", NULL, NULL, (BYTE*)&g_thresholdPercent, &size);
        RegQueryValueExW(hKey, L"SmoothingPercent", NULL, NULL, (BYTE*)&g_smoothingPercent, &size);
        RegQueryValueExW(hKey, L"VSyncEnabled", NULL, NULL, (BYTE*)&g_vsyncEnabled, &size);
        RegQueryValueExW(hKey, L"GrayscaleMode", NULL, NULL, (BYTE*)&g_grayscaleMode, &size);
        
        // Загружаем цвета
        DWORD colorCount = 0;
        if (RegQueryValueExW(hKey, L"ColorCount", NULL, NULL, (BYTE*)&colorCount, &size) == ERROR_SUCCESS && colorCount > 0) {
            g_targetColors.resize(colorCount);
            size = colorCount * sizeof(COLORREF);
            RegQueryValueExW(hKey, L"Colors", NULL, NULL, (BYTE*)g_targetColors.data(), &size);
        }
        
        RegCloseKey(hKey);
    }
}

void SaveSettingsToFile() {
    OPENFILENAMEW ofn = {};
    wchar_t fileName[MAX_PATH] = L"settings.cfg";
    wchar_t initialDir[MAX_PATH];
    
    // Получаем папку где находится программа
    GetModuleFileNameW(NULL, initialDir, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(initialDir, L'\\');
    if (lastSlash) *lastSlash = L'\0';
    
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hWnd;
    ofn.lpstrFile = fileName;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"Config Files\0*.cfg\0All Files\0*.*\0";
    ofn.lpstrDefExt = L"cfg";
    ofn.lpstrInitialDir = initialDir;
    ofn.Flags = OFN_OVERWRITEPROMPT;
    
    if (GetSaveFileNameW(&ofn)) {
        HANDLE hFile = CreateFileW(fileName, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile != INVALID_HANDLE_VALUE) {
            DWORD written;
            WriteFile(hFile, &g_thresholdPercent, sizeof(int), &written, NULL);
            WriteFile(hFile, &g_smoothingPercent, sizeof(int), &written, NULL);
            WriteFile(hFile, &g_vsyncEnabled, sizeof(bool), &written, NULL);
            WriteFile(hFile, &g_grayscaleMode, sizeof(int), &written, NULL);
            
            DWORD colorCount = (DWORD)g_targetColors.size();
            WriteFile(hFile, &colorCount, sizeof(DWORD), &written, NULL);
            if (colorCount > 0) {
                WriteFile(hFile, g_targetColors.data(), colorCount * sizeof(COLORREF), &written, NULL);
            }
            
            CloseHandle(hFile);
            MessageBoxW(g_hWnd, L"Settings saved successfully!", L"Save", MB_OK);
        }
    }
}

void LoadSettingsFromFile() {
    OPENFILENAMEW ofn = {};
    wchar_t fileName[MAX_PATH] = L"";
    wchar_t initialDir[MAX_PATH];
    
    // Получаем папку где находится программа
    GetModuleFileNameW(NULL, initialDir, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(initialDir, L'\\');
    if (lastSlash) *lastSlash = L'\0';
    
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hWnd;
    ofn.lpstrFile = fileName;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"Config Files\0*.cfg\0All Files\0*.*\0";
    ofn.lpstrInitialDir = initialDir;
    ofn.Flags = OFN_FILEMUSTEXIST;
    
    if (GetOpenFileNameW(&ofn)) {
        HANDLE hFile = CreateFileW(fileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile != INVALID_HANDLE_VALUE) {
            DWORD read;
            ReadFile(hFile, &g_thresholdPercent, sizeof(int), &read, NULL);
            ReadFile(hFile, &g_smoothingPercent, sizeof(int), &read, NULL);
            ReadFile(hFile, &g_vsyncEnabled, sizeof(bool), &read, NULL);
            ReadFile(hFile, &g_grayscaleMode, sizeof(int), &read, NULL);
            
            DWORD colorCount;
            ReadFile(hFile, &colorCount, sizeof(DWORD), &read, NULL);
            if (colorCount > 0 && colorCount <= MAX_COLORS) {
                g_targetColors.resize(colorCount);
                ReadFile(hFile, g_targetColors.data(), colorCount * sizeof(COLORREF), &read, NULL);
            }
            
            CloseHandle(hFile);
            
            // Обновляем интерфейс
            SendMessage(g_hTrackbarThreshold, TBM_SETPOS, TRUE, g_thresholdPercent);
            SendMessage(g_hTrackbarSmoothing, TBM_SETPOS, TRUE, g_smoothingPercent);
            SendMessage(g_hVSyncCheckbox, BM_SETCHECK, g_vsyncEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
            SendMessage(g_hGrayscaleCombo, CB_SETCURSEL, g_grayscaleMode, 0);
            UpdateLabels();
            UpdateColorList();
            
            MessageBoxW(g_hWnd, L"Settings loaded successfully!", L"Load", MB_OK);
        }
    }
}
void StartEyedropper() {
    // Временно останавливаем фильтр для удобства выбора цвета
    bool wasRunning = g_isRunning;
    if (wasRunning) {
        StopFilter();
    }
    
    SetCapture(g_hWnd);
    SetCursor(LoadCursor(NULL, IDC_CROSS));
    
    MSG msg;
    bool picking = true;
    
    while (picking && GetMessage(&msg, NULL, 0, 0)) {
        if (msg.message == WM_LBUTTONDOWN) {
            POINT pt;
            GetCursorPos(&pt);
            
            HDC hdcScreen = GetDC(NULL);
            COLORREF color = GetPixel(hdcScreen, pt.x, pt.y);
            ReleaseDC(NULL, hdcScreen);
            
            if (color != CLR_INVALID) {
                g_currentColor = color;
                UpdateLabels();
                InvalidateRect(g_hColorButton, NULL, TRUE);
                
                // Автоматически добавляем выбранный цвет в список
                AddColorToList();
            }
            
            picking = false;
        }
        else if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
            picking = false;
        }
        else {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
    
    ReleaseCapture();
    SetCursor(LoadCursor(NULL, IDC_ARROW));
    
    // Возобновляем фильтр если он был запущен
    if (wasRunning) {
        StartFilter();
    }
}

void ShowColorPicker() {
    CHOOSECOLORW cc = {};
    static COLORREF customColors[16] = {
        RGB(255,0,0), RGB(0,255,0), RGB(0,0,255), RGB(255,255,0),
        RGB(255,0,255), RGB(0,255,255), RGB(128,0,0), RGB(0,128,0),
        RGB(0,0,128), RGB(128,128,0), RGB(128,0,128), RGB(0,128,128),
        RGB(192,192,192), RGB(128,128,128), RGB(255,255,255), RGB(0,0,0)
    };
    
    cc.lStructSize = sizeof(cc);
    cc.hwndOwner = g_hWnd;
    cc.lpCustColors = customColors;
    cc.rgbResult = g_currentColor;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT | CC_ANYCOLOR | CC_SOLIDCOLOR;
    
    // Делаем главное окно поверх всех оверлеев на время выбора цвета
    SetWindowPos(g_hWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    
    if (ChooseColorW(&cc)) {
        g_currentColor = cc.rgbResult;
        UpdateLabels();
        InvalidateRect(g_hColorButton, NULL, TRUE);
        
        // Автоматически добавляем выбранный цвет в список
        AddColorToList();
    }
    
    // Возвращаем главное окно в обычный режим
    SetWindowPos(g_hWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
}

LRESULT CALLBACK OverlayWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                PostMessage(g_hWnd, WM_COMMAND, 4, 0);
            }
            return 0;
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_MOUSEMOVE:
            return HTTRANSPARENT;
        case WM_DESTROY:
            return 0;
        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
    }
}


void RenderFrameForMonitor(MonitorData& monitor) {
    if (!monitor.pDuplication || !monitor.pSwapChain) return;
    
    bool hasNewFrame = false;
    IDXGIResource* pDesktopResource = nullptr;
    DXGI_OUTDUPL_FRAME_INFO frameInfo;
    
    HRESULT hr = monitor.pDuplication->AcquireNextFrame(0, &frameInfo, &pDesktopResource);
    if (SUCCEEDED(hr)) {
        if (frameInfo.LastPresentTime.QuadPart != 0) {
            ID3D11Texture2D* pAcquiredDesktopImage = nullptr;
            hr = pDesktopResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&pAcquiredDesktopImage);
            
            if (SUCCEEDED(hr)) {
                if (!monitor.pDesktopTexture) {
                    D3D11_TEXTURE2D_DESC desc;
                    pAcquiredDesktopImage->GetDesc(&desc);
                    desc.Usage = D3D11_USAGE_DEFAULT;
                    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    desc.CPUAccessFlags = 0;
                    desc.MiscFlags = 0;
                    
                    monitor.pDevice->CreateTexture2D(&desc, nullptr, &monitor.pDesktopTexture);
                    
                    if (monitor.pDesktopTexture) {
                        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
                        srvDesc.Format = desc.Format;
                        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                        srvDesc.Texture2D.MipLevels = 1;
                        monitor.pDevice->CreateShaderResourceView(monitor.pDesktopTexture, &srvDesc, &monitor.pTextureSRV);
                    }
                }
                
                if (monitor.pDesktopTexture) {
                    monitor.pContext->CopyResource(monitor.pDesktopTexture, pAcquiredDesktopImage);
                    hasNewFrame = true;
                }
                pAcquiredDesktopImage->Release();
            }
        }
        pDesktopResource->Release();
        monitor.pDuplication->ReleaseFrame();
    } else if (hr == DXGI_ERROR_ACCESS_LOST) {
        // Переинициализируем дубликацию
        if (monitor.pDuplication) {
            monitor.pDuplication->Release();
            monitor.pDuplication = nullptr;
        }
        InitDirectXForMonitor(monitor);
        return;
    }
    
    // Рисуем только если есть новый кадр
    if (!hasNewFrame) {
        return;
    }
    
    if (!monitor.pDesktopTexture || !monitor.pTextureSRV) {
        return;
    }
    
    // Обновляем константы только при необходимости
    if (monitor.constantsNeedUpdate) {
        D3D11_MAPPED_SUBRESOURCE mapped;
        hr = monitor.pContext->Map(monitor.pConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr)) {
            FilterConstants* constants = (FilterConstants*)mapped.pData;
            constants->numColors = (int)g_targetColors.size();
            
            int maxDistance = 441;
            int thresholdDistance = (maxDistance * g_thresholdPercent) / 100;
            constants->thresholdSquared = thresholdDistance * thresholdDistance;
            
            int smoothingDistance = (thresholdDistance * g_smoothingPercent) / 100;
            constants->smoothingRange = smoothingDistance * smoothingDistance;
            constants->grayscaleMode = g_grayscaleMode;
            
            // Копируем цвета в массив
            for (size_t i = 0; i < g_targetColors.size() && i < MAX_COLORS; i++) {
                constants->targetColors[i][0] = GetRValue(g_targetColors[i]); // R
                constants->targetColors[i][1] = GetGValue(g_targetColors[i]); // G
                constants->targetColors[i][2] = GetBValue(g_targetColors[i]); // B
                constants->targetColors[i][3] = 0; // Padding
            }
            
            monitor.pContext->Unmap(monitor.pConstantBuffer, 0);
            monitor.constantsNeedUpdate = false;
        }
    }
    
    D3D11_VIEWPORT viewport = {};
    viewport.Width = (float)monitor.screenWidth;
    viewport.Height = (float)monitor.screenHeight;
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    
    monitor.pContext->RSSetViewports(1, &viewport);
    monitor.pContext->OMSetRenderTargets(1, &monitor.pRenderTargetView, nullptr);
    
    static const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    monitor.pContext->ClearRenderTargetView(monitor.pRenderTargetView, clearColor);
    
    monitor.pContext->IASetInputLayout(monitor.pInputLayout);
    monitor.pContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    
    static const UINT stride = sizeof(Vertex);
    static const UINT offset = 0;
    monitor.pContext->IASetVertexBuffers(0, 1, &monitor.pVertexBuffer, &stride, &offset);
    
    monitor.pContext->VSSetShader(monitor.pVertexShader, nullptr, 0);
    monitor.pContext->PSSetShader(monitor.pPixelShader, nullptr, 0);
    monitor.pContext->PSSetConstantBuffers(0, 1, &monitor.pConstantBuffer);
    monitor.pContext->PSSetShaderResources(0, 1, &monitor.pTextureSRV);
    monitor.pContext->PSSetSamplers(0, 1, &monitor.pSamplerState);
    
    monitor.pContext->Draw(4, 0);
    monitor.pSwapChain->Present(g_vsyncEnabled ? 1 : 0, 0);
    
    // Обновляем FPS для этого монитора
    if (hasNewFrame) {
        monitor.frameCount++;
        DWORD currentTime = GetTickCount();
        if (currentTime - monitor.lastFPSTime >= 1000) {
            monitor.currentFPS = (float)monitor.frameCount * 1000.0f / (currentTime - monitor.lastFPSTime);
            monitor.frameCount = 0;
            monitor.lastFPSTime = currentTime;
            PostMessage(g_hWnd, WM_USER + 1, 0, 0);
        }
    }
}

void StartFilter() {
    if (!g_isRunning) {
        g_isRunning = true;
        
        // Добавляем текущий цвет в список если список пуст
        if (g_targetColors.empty()) {
            g_targetColors.push_back(g_currentColor);
            UpdateColorList();
        }
        
        SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
        
        HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtr(g_hWnd, GWLP_HINSTANCE);
        
        WNDCLASSW wcOverlay = {};
        wcOverlay.lpfnWndProc = OverlayWndProc;
        wcOverlay.hInstance = hInstance;
        wcOverlay.lpszClassName = L"OverlayWindowClass";
        wcOverlay.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wcOverlay.hCursor = LoadCursor(NULL, IDC_ARROW);
        
        WNDCLASSW existingClass;
        if (!GetClassInfoW(hInstance, L"OverlayWindowClass", &existingClass)) {
            RegisterClassW(&wcOverlay);
        }
        
        bool anySuccess = false;
        
        for (auto& monitor : g_monitors) {
            if (!monitor.isSelected) continue;
            
            wchar_t debugMsg[256];
            swprintf_s(debugMsg, L"Initializing monitor: %s\n", monitor.displayName);
            OutputDebugStringW(debugMsg);
            
            if (!InitDirectXForMonitor(monitor)) {
                OutputDebugStringW(L"Failed InitDirectXForMonitor\n");
                continue;
            }
            
            if (!InitShadersForMonitor(monitor)) {
                OutputDebugStringW(L"Failed InitShadersForMonitor\n");
                continue;
            }
            
            monitor.hOverlayWnd = CreateWindowExW(
                WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW,
                L"OverlayWindowClass", L"ColorFilterOverlay",
                WS_POPUP | WS_VISIBLE,
                monitor.monitorRect.left, monitor.monitorRect.top,
                monitor.screenWidth, monitor.screenHeight,
                NULL, NULL, hInstance, NULL);
            
            if (!monitor.hOverlayWnd) {
                OutputDebugStringW(L"Failed to create overlay window\n");
                continue;
            }
            
            SetLayeredWindowAttributes(monitor.hOverlayWnd, 0, 255, LWA_ALPHA);
            SetWindowDisplayAffinity(monitor.hOverlayWnd, WDA_EXCLUDEFROMCAPTURE);
            
            if (!InitSwapChainForMonitor(monitor)) {
                OutputDebugStringW(L"Failed InitSwapChainForMonitor\n");
                DestroyWindow(monitor.hOverlayWnd);
                monitor.hOverlayWnd = NULL;
                continue;
            }
            
            ShowWindow(monitor.hOverlayWnd, SW_SHOW);
            UpdateWindow(monitor.hOverlayWnd);
            
            SetWindowLongPtr(monitor.hOverlayWnd, GWL_EXSTYLE, 
                GetWindowLongPtr(monitor.hOverlayWnd, GWL_EXSTYLE) | WS_EX_TRANSPARENT);
            
            monitor.isActive = true;
            monitor.constantsNeedUpdate = true; // Помечаем что нужно обновить константы
            monitor.frameCount = 0;
            monitor.lastFPSTime = GetTickCount();
            monitor.currentFPS = 0.0f;
            anySuccess = true;
            OutputDebugStringW(L"Monitor initialized successfully\n");
        }
        
        if (!anySuccess) {
            MessageBoxW(NULL, L"Failed to initialize any selected monitors", L"Error", MB_OK);
            g_isRunning = false;
            return;
        }
        
        // Запускаем поток рендеринга
        g_shouldStopRenderThread = false;
        g_renderThread = CreateThread(NULL, 0, RenderThreadProc, NULL, 0, NULL);
        if (!g_renderThread) {
            MessageBoxW(NULL, L"Failed to create render thread", L"Error", MB_OK);
            g_isRunning = false;
            CleanupDirectX();
            return;
        }
        
        // Сбрасываем FPS лейблы
        for (auto& hFPS : g_hFPSLabels) {
            SetWindowTextW(hFPS, L"0.0 FPS");
        }
        
        EnableWindow(g_hButtonStart, FALSE);
        EnableWindow(g_hButtonStop, TRUE);
    }
}
void StopFilter() {
    if (g_isRunning) {
        g_isRunning = false;
        
        // Останавливаем поток рендеринга
        if (g_renderThread) {
            g_shouldStopRenderThread = true;
            WaitForSingleObject(g_renderThread, 5000); // Ждем до 5 секунд
            CloseHandle(g_renderThread);
            g_renderThread = NULL;
        }
        
        SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);
        
        for (auto& monitor : g_monitors) {
            monitor.isActive = false;
        }
        
        CleanupDirectX();
        
        // Сбрасываем FPS лейблы
        for (auto& hFPS : g_hFPSLabels) {
            SetWindowTextW(hFPS, L"0.0 FPS");
        }
        
        EnableWindow(g_hButtonStart, TRUE);
        EnableWindow(g_hButtonStop, FALSE);
    }
}
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE: {
            HINSTANCE hInstance = ((LPCREATESTRUCT)lParam)->hInstance;
            
            g_hLabelColor = CreateWindowW(L"STATIC", L"Current Color: RGB(255, 0, 0)", WS_CHILD | WS_VISIBLE,
                10, 10, 250, 20, hWnd, NULL, hInstance, NULL);
            
            g_hColorButton = CreateWindowW(L"BUTTON", L"Choose Color", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_OWNERDRAW,
                10, 35, 100, 30, hWnd, (HMENU)1, hInstance, NULL);
            
            g_hEyedropperButton = CreateWindowW(L"BUTTON", L"Pick Color", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                120, 35, 100, 30, hWnd, (HMENU)5, hInstance, NULL);
            
            g_hColorList = CreateWindowW(L"LISTBOX", NULL, 
                WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
                10, 75, 200, 100, hWnd, (HMENU)8, hInstance, NULL);
            
            g_hAddColorButton = CreateWindowW(L"BUTTON", L"Add Color", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                220, 75, 80, 30, hWnd, (HMENU)7, hInstance, NULL);
            
            g_hRemoveColorButton = CreateWindowW(L"BUTTON", L"Remove", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                220, 110, 80, 30, hWnd, (HMENU)9, hInstance, NULL);
            
            g_hLabelThreshold = CreateWindowW(L"STATIC", L"Threshold: 20%", WS_CHILD | WS_VISIBLE,
                10, 185, 150, 20, hWnd, NULL, hInstance, NULL);
            
            g_hTrackbarThreshold = CreateWindowW(TRACKBAR_CLASSW, NULL,
                WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_AUTOTICKS,
                10, 210, 300, 30, hWnd, (HMENU)2, hInstance, NULL);
            SendMessage(g_hTrackbarThreshold, TBM_SETRANGE, TRUE, MAKELONG(1, 100));
            SendMessage(g_hTrackbarThreshold, TBM_SETPOS, TRUE, 20);
            
            g_hLabelSmoothing = CreateWindowW(L"STATIC", L"Smoothing: 10%", WS_CHILD | WS_VISIBLE,
                10, 250, 150, 20, hWnd, NULL, hInstance, NULL);
            
            g_hTrackbarSmoothing = CreateWindowW(TRACKBAR_CLASSW, NULL,
                WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_AUTOTICKS,
                10, 275, 300, 30, hWnd, (HMENU)6, hInstance, NULL);
            SendMessage(g_hTrackbarSmoothing, TBM_SETRANGE, TRUE, MAKELONG(0, 200));
            SendMessage(g_hTrackbarSmoothing, TBM_SETPOS, TRUE, 10);
            
            g_hVSyncCheckbox = CreateWindowW(L"BUTTON", L"V-Sync Enabled",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                10, 315, 150, 20, hWnd, (HMENU)10, hInstance, NULL);
            SendMessage(g_hVSyncCheckbox, BM_SETCHECK, BST_CHECKED, 0);
            
            CreateWindowW(L"STATIC", L"Grayscale:", WS_CHILD | WS_VISIBLE,
                10, 345, 70, 20, hWnd, NULL, hInstance, NULL);
            
            g_hGrayscaleCombo = CreateWindowW(L"COMBOBOX", NULL,
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                85, 343, 140, 200, hWnd, (HMENU)11, hInstance, NULL);
            
            SendMessage(g_hGrayscaleCombo, CB_ADDSTRING, 0, (LPARAM)L"Average");
            SendMessage(g_hGrayscaleCombo, CB_ADDSTRING, 0, (LPARAM)L"Luminosity");
            SendMessage(g_hGrayscaleCombo, CB_ADDSTRING, 0, (LPARAM)L"HD (BT.709)");
            SendMessage(g_hGrayscaleCombo, CB_ADDSTRING, 0, (LPARAM)L"Desaturation");
            SendMessage(g_hGrayscaleCombo, CB_ADDSTRING, 0, (LPARAM)L"Max Channel");
            SendMessage(g_hGrayscaleCombo, CB_ADDSTRING, 0, (LPARAM)L"Green Only");
            SendMessage(g_hGrayscaleCombo, CB_SETCURSEL, 1, 0); // Luminosity по умолчанию
            
            g_hSaveButton = CreateWindowW(L"BUTTON", L"Save", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                240, 343, 50, 25, hWnd, (HMENU)12, hInstance, NULL);
            
            g_hLoadButton = CreateWindowW(L"BUTTON", L"Load", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                295, 343, 50, 25, hWnd, (HMENU)13, hInstance, NULL);
            
            // Создаем чекбоксы и FPS лейблы для мониторов
            PopulateMonitorList();
            
            int yPos = 375;
            for (size_t i = 0; i < g_monitors.size(); i++) {
                HWND hCheck = CreateWindowW(L"BUTTON", g_monitors[i].displayName,
                    WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                    10, yPos, 250, 20, hWnd, (HMENU)(100 + i), hInstance, NULL);
                g_hCheckboxes.push_back(hCheck);
                
                HWND hFPS = CreateWindowW(L"STATIC", L"FPS: 0.0",
                    WS_CHILD | WS_VISIBLE,
                    270, yPos, 80, 20, hWnd, NULL, hInstance, NULL);
                g_hFPSLabels.push_back(hFPS);
                
                yPos += 25;
            }
            
            // Выбираем первый монитор по умолчанию
            if (!g_hCheckboxes.empty()) {
                SendMessage(g_hCheckboxes[0], BM_SETCHECK, BST_CHECKED, 0);
                g_monitors[0].isSelected = true;
            }
            
            g_hButtonStart = CreateWindowW(L"BUTTON", L"Start Filter",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                10, yPos + 10, 145, 30, hWnd, (HMENU)3, hInstance, NULL);
            
            g_hButtonStop = CreateWindowW(L"BUTTON", L"Stop Filter",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                165, yPos + 10, 145, 30, hWnd, (HMENU)4, hInstance, NULL);
            EnableWindow(g_hButtonStop, FALSE);
            
            // Убираем автоматическое добавление начального цвета
            // g_targetColors.push_back(g_currentColor);
            // UpdateColorList();
            
            // Загружаем сохраненные настройки
            LoadSettings();
            
            // Обновляем интерфейс после загрузки
            SendMessage(g_hTrackbarThreshold, TBM_SETPOS, TRUE, g_thresholdPercent);
            SendMessage(g_hTrackbarSmoothing, TBM_SETPOS, TRUE, g_smoothingPercent);
            SendMessage(g_hVSyncCheckbox, BM_SETCHECK, g_vsyncEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
            SendMessage(g_hGrayscaleCombo, CB_SETCURSEL, g_grayscaleMode, 0);
            UpdateLabels();
            UpdateColorList();
            
            return 0;
        }
        
        case WM_USER + 1: {
            // Обновляем FPS для каждого монитора
            for (size_t i = 0; i < g_monitors.size() && i < g_hFPSLabels.size(); i++) {
                if (g_monitors[i].isActive) {
                    wchar_t buffer[32];
                    swprintf_s(buffer, L"%.1f FPS", g_monitors[i].currentFPS);
                    SetWindowTextW(g_hFPSLabels[i], buffer);
                } else {
                    SetWindowTextW(g_hFPSLabels[i], L"0.0 FPS");
                }
            }
            return 0;
        }
        
        case WM_TIMER: {
            // Убираем обработку таймера - теперь используем цикл в WinMain
            return 0;
        }
        
        case WM_MEASUREITEM: {
            LPMEASUREITEMSTRUCT mis = (LPMEASUREITEMSTRUCT)lParam;
            if (mis->CtlID == 8) { // Color list
                mis->itemHeight = 20; // Высота элемента списка
            }
            return TRUE;
        }
        
        case WM_DRAWITEM: {
            LPDRAWITEMSTRUCT dis = (LPDRAWITEMSTRUCT)lParam;
            if (dis->CtlID == 1) { // Color button
                HBRUSH hBrush = CreateSolidBrush(g_currentColor);
                FillRect(dis->hDC, &dis->rcItem, hBrush);
                DeleteObject(hBrush);
                
                SetBkMode(dis->hDC, TRANSPARENT);
                SetTextColor(dis->hDC, RGB(255, 255, 255));
                DrawTextW(dis->hDC, L"Choose Color", -1, &dis->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
            else if (dis->CtlID == 8) { // Color list
                if (dis->itemID != -1 && dis->itemID < (UINT)g_targetColors.size()) {
                    COLORREF itemColor = g_targetColors[dis->itemID];
                    
                    // Фон элемента
                    COLORREF bgColor = (dis->itemState & ODS_SELECTED) ? 
                        GetSysColor(COLOR_HIGHLIGHT) : GetSysColor(COLOR_WINDOW);
                    HBRUSH hBgBrush = CreateSolidBrush(bgColor);
                    FillRect(dis->hDC, &dis->rcItem, hBgBrush);
                    DeleteObject(hBgBrush);
                    
                    // Цветная иконка (квадрат 16x16) - исправлен размер
                    RECT colorRect = {
                        dis->rcItem.left + 2,
                        dis->rcItem.top + 2,
                        dis->rcItem.left + 18,
                        dis->rcItem.top + 18
                    };
                    
                    // Создаем кисть с правильным цветом
                    HBRUSH hColorBrush = CreateSolidBrush(itemColor);
                    if (hColorBrush) {
                        FillRect(dis->hDC, &colorRect, hColorBrush);
                        DeleteObject(hColorBrush);
                    }
                    
                    // Рамка вокруг иконки
                    HPEN hPen = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
                    if (hPen) {
                        HPEN hOldPen = (HPEN)SelectObject(dis->hDC, hPen);
                        SelectObject(dis->hDC, GetStockObject(NULL_BRUSH)); // Убираем заливку для рамки
                        Rectangle(dis->hDC, colorRect.left, colorRect.top, colorRect.right, colorRect.bottom);
                        SelectObject(dis->hDC, hOldPen);
                        DeleteObject(hPen);
                    }
                    
                    // Текст с цветом
                    wchar_t colorText[64];
                    swprintf_s(colorText, L"RGB(%d, %d, %d)", 
                        GetRValue(itemColor), GetGValue(itemColor), GetBValue(itemColor));
                    
                    RECT textRect = dis->rcItem;
                    textRect.left += 22; // Отступ после иконки
                    
                    SetBkMode(dis->hDC, TRANSPARENT);
                    SetTextColor(dis->hDC, (dis->itemState & ODS_SELECTED) ? 
                        GetSysColor(COLOR_HIGHLIGHTTEXT) : GetSysColor(COLOR_WINDOWTEXT));
                    DrawTextW(dis->hDC, colorText, -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                }
            }
            return TRUE;
        }
        
        case WM_HSCROLL: {
            if ((HWND)lParam == g_hTrackbarThreshold) {
                g_thresholdPercent = (int)SendMessage(g_hTrackbarThreshold, TBM_GETPOS, 0, 0);
                UpdateLabels();
                // Помечаем все активные мониторы для обновления констант
                for (auto& monitor : g_monitors) {
                    if (monitor.isActive) {
                        monitor.constantsNeedUpdate = true;
                    }
                }
            }
            else if ((HWND)lParam == g_hTrackbarSmoothing) {
                g_smoothingPercent = (int)SendMessage(g_hTrackbarSmoothing, TBM_GETPOS, 0, 0);
                UpdateLabels();
                // Помечаем все активные мониторы для обновления констант
                for (auto& monitor : g_monitors) {
                    if (monitor.isActive) {
                        monitor.constantsNeedUpdate = true;
                    }
                }
            }
            return 0;
        }
        
        case WM_COMMAND: {
            if (LOWORD(wParam) == 1) {
                ShowColorPicker();
            }
            else if (LOWORD(wParam) == 5) {
                StartEyedropper();
            }
            else if (LOWORD(wParam) == 7) {
                AddColorToList();
            }
            else if (LOWORD(wParam) == 9) {
                RemoveColorFromList();
            }
            else if (LOWORD(wParam) == 10) {
                g_vsyncEnabled = (SendMessage(g_hVSyncCheckbox, BM_GETCHECK, 0, 0) == BST_CHECKED);
                // V-Sync изменяется мгновенно, перезапуск не нужен
            }
            else if (LOWORD(wParam) == 11 && HIWORD(wParam) == CBN_SELCHANGE) {
                g_grayscaleMode = (int)SendMessage(g_hGrayscaleCombo, CB_GETCURSEL, 0, 0);
                // Помечаем все активные мониторы для обновления констант
                for (auto& monitor : g_monitors) {
                    if (monitor.isActive) {
                        monitor.constantsNeedUpdate = true;
                    }
                }
            }
            else if (LOWORD(wParam) == 12) {
                SaveSettingsToFile();
            }
            else if (LOWORD(wParam) == 13) {
                LoadSettingsFromFile();
            }
            else if (LOWORD(wParam) == 3) {
                StartFilter();
            }
            else if (LOWORD(wParam) == 4) {
                StopFilter();
            }
            else if (LOWORD(wParam) >= 100 && LOWORD(wParam) < 100 + g_monitors.size()) {
                // Обработка чекбоксов мониторов
                int monitorIndex = LOWORD(wParam) - 100;
                bool isChecked = (SendMessage(g_hCheckboxes[monitorIndex], BM_GETCHECK, 0, 0) == BST_CHECKED);
                g_monitors[monitorIndex].isSelected = isChecked;
                
                if (g_isRunning) {
                    StopFilter();
                    Sleep(100);
                    StartFilter();
                }
            }

            return 0;
        }
        
        case WM_KEYDOWN: {
            if (wParam == VK_ESCAPE && g_isRunning) {
                StopFilter();
            }
            return 0;
        }
        
        case WM_ENTERSIZEMOVE: {
            g_isDragging = true;
            return 0;
        }
        
        case WM_EXITSIZEMOVE: {
            g_isDragging = false;
            return 0;
        }
        
        case WM_DESTROY:
            SaveSettings(); // Автосохранение при закрытии
            StopFilter();
            PostQuitMessage(0);
            return 0;
            
        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
    }
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(INITCOMMONCONTROLSEX);
    icex.dwICC = ICC_BAR_CLASSES;
    InitCommonControlsEx(&icex);
    
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"ColorFilterGPUClass";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    
    RegisterClassW(&wc);
    
    g_hWnd = CreateWindowW(
        L"ColorFilterGPUClass",
        L"Selective Color Filter GPU - Multiple Colors",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 380, 580,
        NULL, NULL, hInstance, NULL
    );
    
    if (!g_hWnd) {
        return 0;
    }
    
    ShowWindow(g_hWnd, nCmdShow);
    UpdateWindow(g_hWnd);
    
    MSG msg;
    // Простой цикл сообщений - рендеринг теперь в отдельном потоке
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    
    return (int)msg.wParam;
}