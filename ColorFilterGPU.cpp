#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <shlobj.h>
#include <string>
#include <cmath>
#include <vector>
#include <thread>
#include <algorithm>
#include <cstddef> // Для offsetof

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

// Глобальные переменные
HWND g_hWnd = NULL;
HWND g_hColorButton, g_hEyedropperButton;
HWND g_hTrackbarThreshold, g_hTrackbarSmoothing;
HWND g_hLabelColor, g_hLabelThreshold, g_hLabelSmoothing;
HWND g_hButtonStart, g_hButtonStop;
HWND g_hColorList, g_hAddColorButton, g_hRemoveColorButton;
HWND g_hVSyncCheckbox, g_hGrayscaleCombo, g_hAudioCheckbox;
HWND g_hSaveButton, g_hLoadButton, g_hScreenshotButton;
HWND g_hSelectiveCheckbox; // Чекбокс для включения/выключения селективного фильтра
UINT_PTR g_topMostTimer = 0; // Таймер для поддержания поверх всех окон
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
bool g_enableAudioCapture = false;
bool g_enableSelective = true; // Включение/выключение селективного фильтра
HANDLE g_renderThread = NULL;

// Screenshot settings
wchar_t g_screenshotFolder[MAX_PATH] = L"";
int g_screenshotKey = VK_F12; // Default F12
bool g_saveToFolder = true;
bool g_saveToClipboard = false;
int g_screenshotFormat = 0; // 0=BMP, 1=PNG, 2=JPG
bool g_takingScreenshot = false; // Защита от повторных вызовов
bool g_pauseRendering = false; // Пауза рендеринга для скриншота
HHOOK g_keyboardHook = NULL; // Хук клавиатуры

// Color filters settings
struct ColorFilter {
    COLORREF color;
    float brightness;
    float contrast;
    float saturation;
    int presetIndex; // 0=Custom, 1=Grayscale, 2=Inverted, 3=Deuteranopia
    bool enabled;
    
    ColorFilter() : color(RGB(255,255,255)), brightness(1.0f), contrast(1.0f), saturation(1.0f), presetIndex(0), enabled(true) {}
};

// Glow effect settings
struct GlowEffect {
    bool enabled;
    float intensity;        // 0.0 - 5.0 (сила свечения)
    float threshold;        // 0.0 - 1.0 (порог яркости для свечения)
    float radius;           // 0.1 - 10.0 (радиус размытия)
    float saturation;       // 0.0 - 3.0 (насыщенность свечения)
    int blurPasses;         // 1 - 5 (количество проходов размытия)
    float downsample;       // 1, 2, 4 (уменьшение разрешения для размытия)
    
    GlowEffect() : enabled(false), intensity(1.0f), threshold(0.7f), radius(1.0f), saturation(1.2f), blurPasses(2), downsample(2.0f) {}
};

GlowEffect g_glowEffect;

// Preset matrices из Matrices.cs (формат 5x5)
// В C# матрицы хранятся column-major, здесь транспонированы в row-major
// Формат: 4 строки по 5 значений [R_out G_out B_out A_out Offset]
struct FilterPreset {
    const wchar_t* name;
    float matrix[20]; // 4 строки по 5 значений (4x5): последний столбец - offset
};

const FilterPreset g_filterPresets[] = {
    // 0. Identity (без изменений)
    { L"Identity", {
        1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f, 0.0f
    }},
    
    // 1. Protanopia (нет красного)
    { L"Protanopia", {
        0.567f, 0.433f, 0.0f, 0.0f, 0.0f,
        0.558f, 0.442f, 0.0f, 0.0f, 0.0f,
        0.0f, 0.242f, 0.758f, 0.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f, 0.0f
    }},
    
    // 2. Protanomaly (слабое красное)
    { L"Protanomaly", {
        0.817f, 0.183f, 0.0f, 0.0f, 0.0f,
        0.333f, 0.667f, 0.0f, 0.0f, 0.0f,
        0.0f, 0.125f, 0.875f, 0.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f, 0.0f
    }},
    
    // 3. Deuteranomaly (слабое зеленое)
    { L"Deuteranomaly", {
        0.8f, 0.2f, 0.0f, 0.0f, 0.0f,
        0.258f, 0.742f, 0.0f, 0.0f, 0.0f,
        0.0f, 0.142f, 0.858f, 0.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f, 0.0f
    }},
    
    // 4. Deuteranopia (нет зеленого)
    { L"Deuteranopia", {
        0.625f, 0.375f, 0.0f, 0.0f, 0.0f,
        0.7f, 0.3f, 0.0f, 0.0f, 0.0f,
        0.0f, 0.3f, 0.7f, 0.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f, 0.0f
    }},
    
    // 5. Tritanopia (нет синего)
    { L"Tritanopia", {
        0.95f, 0.05f, 0.0f, 0.0f, 0.0f,
        0.0f, 0.433f, 0.567f, 0.0f, 0.0f,
        0.0f, 0.475f, 0.525f, 0.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f, 0.0f
    }},
    
    // 6. Tritanomaly (слабое синее)
    { L"Tritanomaly", {
        0.967f, 0.033f, 0.0f, 0.0f, 0.0f,
        0.0f, 0.733f, 0.267f, 0.0f, 0.0f,
        0.0f, 0.183f, 0.817f, 0.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f, 0.0f
    }},
    
    // 7. Negative (инверсия)
    { L"Negative", {
        -1.0f, 0.0f, 0.0f, 0.0f, 1.0f,
        0.0f, -1.0f, 0.0f, 0.0f, 1.0f,
        0.0f, 0.0f, -1.0f, 0.0f, 1.0f,
        0.0f, 0.0f, 0.0f, 1.0f, 0.0f
    }},
    
    // 8. GrayScale (черно-белый) - BT.601
    // Каждая строка = веса для выходного канала
    { L"GrayScale", {
        0.3f, 0.6f, 0.1f, 0.0f, 0.0f,  // R_out = 0.3*R + 0.6*G + 0.1*B
        0.3f, 0.6f, 0.1f, 0.0f, 0.0f,  // G_out = 0.3*R + 0.6*G + 0.1*B
        0.3f, 0.6f, 0.1f, 0.0f, 0.0f,  // B_out = 0.3*R + 0.6*G + 0.1*B
        0.0f, 0.0f, 0.0f, 1.0f, 0.0f
    }},
    
    // 9. Sepia (сепия)
    { L"Sepia", {
        0.393f, 0.769f, 0.189f, 0.0f, 0.0f,
        0.349f, 0.686f, 0.168f, 0.0f, 0.0f,
        0.272f, 0.534f, 0.131f, 0.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f, 0.0f
    }},
    
    // 10. Red (красный канал) - GrayScale * Red
    { L"Red", {
        0.3f, 0.6f, 0.1f, 0.0f, 0.0f,  // R_out = grayscale (только в красный)
        0.0f, 0.0f, 0.0f, 0.0f, 0.0f,  // G_out = 0
        0.0f, 0.0f, 0.0f, 0.0f, 0.0f,  // B_out = 0
        0.0f, 0.0f, 0.0f, 1.0f, 0.0f
    }},
    
    // 11. HueShift180 (смещение тона на 180°)
    { L"HueShift180", {
        -0.3333333f, 0.6666667f, 0.6666667f, 0.0f, 0.0f,
        0.6666667f, -0.3333333f, 0.6666667f, 0.0f, 0.0f,
        0.6666667f, 0.6666667f, -0.3333333f, 0.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f, 0.0f
    }},
    
    // 12. NegativeGrayScale (инверсия + черно-белый)
    { L"NegativeGrayScale", {
        -0.3f, -0.6f, -0.1f, 0.0f, 1.0f,
        -0.3f, -0.6f, -0.1f, 0.0f, 1.0f,
        -0.3f, -0.6f, -0.1f, 0.0f, 1.0f,
        0.0f, 0.0f, 0.0f, 1.0f, 0.0f
    }},
    
    // 13. NegativeSepia (инверсия + сепия)
    { L"NegativeSepia", {
        -0.393f, -0.769f, -0.189f, 0.0f, 1.0f,
        -0.349f, -0.686f, -0.168f, 0.0f, 1.0f,
        -0.272f, -0.534f, -0.131f, 0.0f, 1.0f,
        0.0f, 0.0f, 0.0f, 1.0f, 0.0f
    }},
    
    // 14. NegativeRed (инверсия + красный)
    { L"NegativeRed", {
        -0.3f, -0.6f, -0.1f, 0.0f, 1.0f,
        0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f, 0.0f
    }},
    
    // 15. NegativeHueShift180 (инверсия + смещение тона)
    { L"NegativeHueShift180", {
        0.3333333f, -0.6666667f, -0.6666667f, 0.0f, 1.0f,
        -0.6666667f, 0.3333333f, -0.6666667f, 0.0f, 1.0f,
        -0.6666667f, -0.6666667f, 0.3333333f, 0.0f, 1.0f,
        0.0f, 0.0f, 0.0f, 1.0f, 0.0f
    }},
    
    // 16. NegativeHueShift180Variation1 (высокая насыщенность)
    { L"NegativeHueShift180Var1", {
        1.0f, -1.0f, -1.0f, 0.0f, 1.0f,
        -1.0f, 1.0f, -1.0f, 0.0f, 1.0f,
        -1.0f, -1.0f, 1.0f, 0.0f, 1.0f,
        0.0f, 0.0f, 0.0f, 1.0f, 0.0f
    }},
    
    // 17. NegativeHueShift180Variation2 (мягкая инверсия)
    { L"NegativeHueShift180Var2", {
        0.39f, -0.62f, -0.62f, 0.0f, 1.0f,
        -1.21f, -0.22f, -1.22f, 0.0f, 1.0f,
        -0.16f, -0.16f, 0.84f, 0.0f, 1.0f,
        0.0f, 0.0f, 0.0f, 1.0f, 0.0f
    }},
    
    // 18. NegativeHueShift180Variation3 (высокая читаемость)
    { L"NegativeHueShift180Var3", {
        1.089508f, -0.9326327f, -0.932633042f, 0.0f, 1.0f,
        -1.81771779f, 0.1683074f, -1.84169245f, 0.0f, 1.0f,
        -0.244589478f, -0.247815639f, 1.7621845f, 0.0f, 1.0f,
        0.0f, 0.0f, 0.0f, 1.0f, 0.0f
    }},
    
    // 19. NegativeHueShift180Variation4 (хорошая цветопередача)
    { L"NegativeHueShift180Var4", {
        0.50f, -0.78f, -0.78f, 0.0f, 1.0f,
        -0.56f, 0.72f, -0.56f, 0.0f, 1.0f,
        -0.94f, -0.94f, 0.34f, 0.0f, 1.0f,
        0.0f, 0.0f, 0.0f, 1.0f, 0.0f
    }},
    
    // 20. Protanopia Correction (Red-Green Correction)
    { L"Protanopia Correction", {
        0.152f, 1.053f, -0.205f, 0.0f, 0.0f,
        0.115f, 0.786f, 0.099f, 0.0f, 0.0f,
        -0.004f, -0.048f, 1.052f, 0.0f, 0.0f,
        0.000f, 0.000f, 0.000f, 1.0f, 0.0f
    }},
    
    // 21. Deuteranopia Correction (Red-Green Correction)
    { L"Deuteranopia Correction", {
        0.501f, 0.796f, -0.297f, 0.0f, 0.0f,
        -0.012f, 0.678f, 0.334f, 0.0f, 0.0f,
        -0.012f, 0.041f, 0.971f, 0.0f, 0.0f,
        0.000f, 0.000f, 0.000f, 1.0f, 0.0f
    }},
    
    // 22. Tritanopia Correction (Blue-Yellow Correction)
    { L"Tritanopia Correction", {
        1.000f, 0.127f, -0.127f, 0.0f, 0.0f,
        0.000f, 1.000f, 0.000f, 0.0f, 0.0f,
        0.000f, 0.860f, 0.140f, 0.0f, 0.0f,
        0.000f, 0.000f, 0.000f, 1.0f, 0.0f
    }}
};

std::vector<ColorFilter> g_colorFilters;
int g_selectedFilterIndex = -1;
bool g_filtersGlobalEnabled = false;
HWND g_hFiltersDlg = NULL;

int g_selectedEffectIndex = -1;
HWND g_hEffectsDlg = NULL;

// Audio capture для Discord
IMMDeviceEnumerator* g_pEnumerator = nullptr;
IMMDevice* g_pDevice = nullptr;
IAudioClient* g_pAudioClient = nullptr;
IAudioCaptureClient* g_pCaptureClient = nullptr;
IAudioClient* g_pPlaybackClient = nullptr;
IAudioRenderClient* g_pRenderClient = nullptr;
HANDLE g_audioThread = NULL;
bool g_shouldStopAudioThread = false;

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
    
    // Glow effect resources
    ID3D11Texture2D* pGlowTexture1;
    ID3D11Texture2D* pGlowTexture2;
    ID3D11Texture2D* pIntermediateTexture; // Промежуточная текстура для glow
    ID3D11RenderTargetView* pGlowRTV1;
    ID3D11RenderTargetView* pGlowRTV2;
    ID3D11RenderTargetView* pIntermediateRTV;
    ID3D11ShaderResourceView* pGlowSRV1;
    ID3D11ShaderResourceView* pGlowSRV2;
    ID3D11ShaderResourceView* pIntermediateSRV;
    ID3D11PixelShader* pBrightPassShader;
    ID3D11PixelShader* pBlurHShader;
    ID3D11PixelShader* pBlurVShader;
    ID3D11PixelShader* pCompositeShader;
    
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
        
        pGlowTexture1 = nullptr;
        pGlowTexture2 = nullptr;
        pIntermediateTexture = nullptr;
        pGlowRTV1 = nullptr;
        pGlowRTV2 = nullptr;
        pIntermediateRTV = nullptr;
        pGlowSRV1 = nullptr;
        pGlowSRV2 = nullptr;
        pIntermediateSRV = nullptr;
        pBrightPassShader = nullptr;
        pBlurHShader = nullptr;
        pBlurVShader = nullptr;
        pCompositeShader = nullptr;
        
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
    int targetColors[MAX_COLORS][4]; // int4 для каждого цвета (RGB + padding) - 10*16 = 160 bytes
    int selectiveEnabled; // Включение/выключения селективного фильтра
    int padding1[3]; // Выравнивание до 16 байт
    float colorMatrix[16]; // 4x4 матрица для трансформации каналов - 64 bytes
    float colorOffset[4]; // Вектор смещения (5-й столбец матрицы 5x5) - 16 bytes
};

// Проверка выравнивания структуры
static_assert(sizeof(FilterConstants) % 16 == 0, "FilterConstants must be 16-byte aligned for constant buffer");
static_assert(offsetof(FilterConstants, colorMatrix) % 16 == 0, "colorMatrix must be 16-byte aligned");
static_assert(offsetof(FilterConstants, colorOffset) % 16 == 0, "colorOffset must be 16-byte aligned");

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
    float brightness;
    float contrast;
    float saturation;
    float hue;
    float gamma;
    int enableFilters;
    int2 padding;
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

// Функции для применения цветовых фильтров
float3 ApplyBrightness(float3 color, float brightness) {
    return color * brightness;
}

float3 ApplyContrast(float3 color, float contrast) {
    return (color - 0.5f) * contrast + 0.5f;
}

float3 ApplyGamma(float3 color, float gamma) {
    return pow(abs(color), gamma);
}

float3 RGBtoHSV(float3 rgb) {
    float4 K = float4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    float4 p = lerp(float4(rgb.bg, K.wz), float4(rgb.gb, K.xy), step(rgb.b, rgb.g));
    float4 q = lerp(float4(p.xyw, rgb.r), float4(rgb.r, p.yzx), step(p.x, rgb.r));
    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return float3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}

float3 HSVtoRGB(float3 hsv) {
    float4 K = float4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    float3 p = abs(frac(hsv.xxx + K.xyz) * 6.0 - K.www);
    return hsv.z * lerp(K.xxx, saturate(p - K.xxx), hsv.y);
}

float3 ApplySaturation(float3 color, float saturation) {
    float3 hsv = RGBtoHSV(color);
    hsv.y *= saturation;
    return HSVtoRGB(hsv);
}

float3 ApplyHue(float3 color, float hueShift) {
    float3 hsv = RGBtoHSV(color);
    hsv.x += hueShift / 360.0f;
    hsv.x = frac(hsv.x);
    return HSVtoRGB(hsv);
}

float4 ApplyColorFilters(float4 color) {
    if (enableFilters == 0) return color;
    
    float3 rgb = color.rgb;
    
    // Применяем фильтры в правильном порядке
    rgb = ApplyBrightness(rgb, brightness);
    rgb = ApplyContrast(rgb, contrast);
    rgb = ApplyGamma(rgb, gamma);
    rgb = ApplySaturation(rgb, saturation);
    rgb = ApplyHue(rgb, hue);
    
    // Ограничиваем значения
    rgb = saturate(rgb);
    
    return float4(rgb, color.a);
}
)";

// Обновленный пиксельный шейдер с применением фильтров
const char* g_pixelShaderSourceWithFilters = R"(
cbuffer FilterConstants : register(b0) {
    int numColors;
    int thresholdSquared;
    int smoothingRange;
    int grayscaleMode;
    int4 targetColors[10];
    int selectiveEnabled;
    int3 padding1;
    row_major float4x4 colorMatrix;
    float4 colorOffset;
};

Texture2D screenTexture : register(t0);
SamplerState texSampler : register(s0);

struct PS_INPUT {
    float4 pos : SV_POSITION;
    float2 tex : TEXCOORD0;
};

float4 PSMain(PS_INPUT input) : SV_TARGET {
    float4 rawColor = screenTexture.Sample(texSampler, input.tex);
    
    float3 rgb;
    rgb.r = dot(rawColor.rgb, colorMatrix[0].rgb) + colorOffset.r;
    rgb.g = dot(rawColor.rgb, colorMatrix[1].rgb) + colorOffset.g;
    rgb.b = dot(rawColor.rgb, colorMatrix[2].rgb) + colorOffset.b;
    float alpha = rawColor.a;
    float4 filteredColor = saturate(float4(rgb, alpha));
    
    // 2. СЕЛЕКТИВНЫЙ ЦВЕТ (только если включен)
    float4 finalColor = filteredColor;
    if (selectiveEnabled == 1) {
        int r = (int)(filteredColor.r * 255.0f);
        int g = (int)(filteredColor.g * 255.0f);
        int b = (int)(filteredColor.b * 255.0f);
        
        float bestFactor = 0.0f;
        
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
            
            bestFactor = max(bestFactor, currentFactor);
        }
        
        if (bestFactor > 0.99f) {
            finalColor = filteredColor;
        } else if (bestFactor < 0.01f) {
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
            finalColor = float4(gray/255.0f, gray/255.0f, gray/255.0f, filteredColor.a);
        } else {
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
            float4 grayColor = float4(gray/255.0f, gray/255.0f, gray/255.0f, filteredColor.a);
            finalColor = lerp(grayColor, filteredColor, bestFactor);
        }
    }
    
    return finalColor;
}
)";

// Bright Pass Shader - выделяет яркие области
const char* g_brightPassShader = R"(
cbuffer GlowConstants : register(b0) {
    float glowThreshold;
    float glowSaturation;
    float2 padding;
};

Texture2D screenTexture : register(t0);
SamplerState texSampler : register(s0);

struct PS_INPUT {
    float4 pos : SV_POSITION;
    float2 tex : TEXCOORD0;
};

float4 PSMain(PS_INPUT input) : SV_TARGET {
    float4 color = screenTexture.Sample(texSampler, input.tex);
    
    // Вычисляем яркость
    float brightness = dot(color.rgb, float3(0.2126, 0.7152, 0.0722));
    
    // Оставляем только яркие области выше порога
    float factor = max(0.0, brightness - glowThreshold) / max(0.001, 1.0 - glowThreshold);
    
    // Усиливаем насыщенность ярких областей
    float gray = brightness;
    float3 saturated = lerp(float3(gray, gray, gray), color.rgb, glowSaturation);
    
    return float4(saturated * factor, color.a);
}
)";

// Horizontal Blur Shader - размытие по горизонтали
const char* g_blurHShader = R"(
cbuffer BlurConstants : register(b0) {
    float2 texelSize;
    float blurRadius;
    float padding;
};

Texture2D screenTexture : register(t0);
SamplerState texSampler : register(s0);

struct PS_INPUT {
    float4 pos : SV_POSITION;
    float2 tex : TEXCOORD0;
};

// Гауссовы веса для размытия
static const float weights[9] = {
    0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216,
    0.016216, 0.054054, 0.1216216, 0.1945946
};

float4 PSMain(PS_INPUT input) : SV_TARGET {
    float3 result = screenTexture.Sample(texSampler, input.tex).rgb * weights[0];
    
    float radius = blurRadius;
    for(int i = 1; i < 9; ++i) {
        float offset = i * radius;
        result += screenTexture.Sample(texSampler, input.tex + float2(texelSize.x * offset, 0)).rgb * weights[i];
        result += screenTexture.Sample(texSampler, input.tex - float2(texelSize.x * offset, 0)).rgb * weights[i];
    }
    
    return float4(result, 1.0);
}
)";

// Vertical Blur Shader - размытие по вертикали
const char* g_blurVShader = R"(
cbuffer BlurConstants : register(b0) {
    float2 texelSize;
    float blurRadius;
    float padding;
};

Texture2D screenTexture : register(t0);
SamplerState texSampler : register(s0);

struct PS_INPUT {
    float4 pos : SV_POSITION;
    float2 tex : TEXCOORD0;
};

static const float weights[9] = {
    0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216,
    0.016216, 0.054054, 0.1216216, 0.1945946
};

float4 PSMain(PS_INPUT input) : SV_TARGET {
    float3 result = screenTexture.Sample(texSampler, input.tex).rgb * weights[0];
    
    float radius = blurRadius;
    for(int i = 1; i < 9; ++i) {
        float offset = i * radius;
        result += screenTexture.Sample(texSampler, input.tex + float2(0, texelSize.y * offset)).rgb * weights[i];
        result += screenTexture.Sample(texSampler, input.tex - float2(0, texelSize.y * offset)).rgb * weights[i];
    }
    
    return float4(result, 1.0);
}
)";

// Composite Shader - финальное смешивание
const char* g_compositeShader = R"(
cbuffer CompositeConstants : register(b0) {
    float glowIntensity;
    float3 padding;
};

Texture2D baseTexture : register(t0);
Texture2D glowTexture : register(t1);
SamplerState texSampler : register(s0);

struct PS_INPUT {
    float4 pos : SV_POSITION;
    float2 tex : TEXCOORD0;
};

float4 PSMain(PS_INPUT input) : SV_TARGET {
    float4 base = baseTexture.Sample(texSampler, input.tex);
    float4 glow = glowTexture.Sample(texSampler, input.tex);
    
    // Additive blending с контролем интенсивности
    float3 finalColor = base.rgb + (glow.rgb * glowIntensity);
    
    return float4(finalColor, base.a);
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
void StartAudioCapture();
void StopAudioCapture();
DWORD WINAPI AudioThreadProc(LPVOID lpParam);
void TakeScreenshot();
void ShowScreenshotSettings();
void ShowColorFiltersDialog();
void ShowEffectsDialog();
LRESULT CALLBACK ColorFiltersDlgProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK EffectsDlgProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
wchar_t* GetKeyName(int vkCode);
void RenderFrameForMonitor(MonitorData& monitor);
void StartFilter();
void StopFilter();
void CleanupDirectX();
LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam);
bool InitDirectXForMonitor(MonitorData& monitor);
bool InitShadersForMonitor(MonitorData& monitor);
bool InitSwapChainForMonitor(MonitorData& monitor);
DWORD WINAPI RenderThreadProc(LPVOID lpParam);
void MultiplyMatrices(const float* A, const float* B, float* Result);

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

// Функция для перемножения матриц 5x5 (в формате 4x5)
void MultiplyMatrices(const float* A, const float* B, float* Result) {
    float temp[20];
    
    // Перемножаем как 4x5 матрицы
    // Каждая строка имеет 5 элементов: [R G B A Offset]
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 5; col++) {
            float sum = 0.0f;
            
            if (col < 4) {
                // Для первых 4 столбцов: стандартное умножение матриц
                for (int k = 0; k < 4; k++) {
                    sum += A[row * 5 + k] * B[k * 5 + col];
                }
            } else {
                // Для 5-го столбца (offset): A_offset + A * B_offset
                sum = A[row * 5 + 4]; // Offset из A
                for (int k = 0; k < 4; k++) {
                    sum += A[row * 5 + k] * B[k * 5 + 4];
                }
            }
            
            temp[row * 5 + col] = sum;
        }
    }
    
    memcpy(Result, temp, sizeof(float) * 20);
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
    hr = D3DCompile(g_pixelShaderSourceWithFilters, strlen(g_pixelShaderSourceWithFilters),
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
    
    // Компилируем Glow шейдеры
    bool glowShadersOK = true;
    
    // Bright Pass Shader
    pPSBlob = nullptr;
    pErrorBlob = nullptr;
    hr = D3DCompile(g_brightPassShader, strlen(g_brightPassShader),
        nullptr, nullptr, nullptr, "PSMain", "ps_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS, 0, &pPSBlob, &pErrorBlob);
    if (SUCCEEDED(hr)) {
        hr = monitor.pDevice->CreatePixelShader(pPSBlob->GetBufferPointer(),
            pPSBlob->GetBufferSize(), nullptr, &monitor.pBrightPassShader);
        pPSBlob->Release();
        if (FAILED(hr)) glowShadersOK = false;
    } else {
        if (pErrorBlob) {
            OutputDebugStringA("Bright Pass Shader compilation failed: ");
            OutputDebugStringA((char*)pErrorBlob->GetBufferPointer());
            pErrorBlob->Release();
        }
        glowShadersOK = false;
    }
    
    // Horizontal Blur Shader
    pPSBlob = nullptr;
    pErrorBlob = nullptr;
    hr = D3DCompile(g_blurHShader, strlen(g_blurHShader),
        nullptr, nullptr, nullptr, "PSMain", "ps_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS, 0, &pPSBlob, &pErrorBlob);
    if (SUCCEEDED(hr)) {
        hr = monitor.pDevice->CreatePixelShader(pPSBlob->GetBufferPointer(),
            pPSBlob->GetBufferSize(), nullptr, &monitor.pBlurHShader);
        pPSBlob->Release();
        if (FAILED(hr)) glowShadersOK = false;
    } else {
        if (pErrorBlob) {
            OutputDebugStringA("Blur H Shader compilation failed: ");
            OutputDebugStringA((char*)pErrorBlob->GetBufferPointer());
            pErrorBlob->Release();
        }
        glowShadersOK = false;
    }
    
    // Vertical Blur Shader
    pPSBlob = nullptr;
    pErrorBlob = nullptr;
    hr = D3DCompile(g_blurVShader, strlen(g_blurVShader),
        nullptr, nullptr, nullptr, "PSMain", "ps_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS, 0, &pPSBlob, &pErrorBlob);
    if (SUCCEEDED(hr)) {
        hr = monitor.pDevice->CreatePixelShader(pPSBlob->GetBufferPointer(),
            pPSBlob->GetBufferSize(), nullptr, &monitor.pBlurVShader);
        pPSBlob->Release();
        if (FAILED(hr)) glowShadersOK = false;
    } else {
        if (pErrorBlob) {
            OutputDebugStringA("Blur V Shader compilation failed: ");
            OutputDebugStringA((char*)pErrorBlob->GetBufferPointer());
            pErrorBlob->Release();
        }
        glowShadersOK = false;
    }
    
    // Composite Shader
    pPSBlob = nullptr;
    pErrorBlob = nullptr;
    hr = D3DCompile(g_compositeShader, strlen(g_compositeShader),
        nullptr, nullptr, nullptr, "PSMain", "ps_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS, 0, &pPSBlob, &pErrorBlob);
    if (SUCCEEDED(hr)) {
        hr = monitor.pDevice->CreatePixelShader(pPSBlob->GetBufferPointer(),
            pPSBlob->GetBufferSize(), nullptr, &monitor.pCompositeShader);
        pPSBlob->Release();
        if (FAILED(hr)) glowShadersOK = false;
    } else {
        if (pErrorBlob) {
            OutputDebugStringA("Composite Shader compilation failed: ");
            OutputDebugStringA((char*)pErrorBlob->GetBufferPointer());
            pErrorBlob->Release();
        }
        glowShadersOK = false;
    }
    
    // Создаем Glow текстуры (уменьшенное разрешение для производительности)
    int glowWidth = monitor.screenWidth / (int)g_glowEffect.downsample;
    int glowHeight = monitor.screenHeight / (int)g_glowEffect.downsample;
    
    if (glowWidth > 0 && glowHeight > 0) {
        D3D11_TEXTURE2D_DESC glowTexDesc = {};
        glowTexDesc.Width = glowWidth;
        glowTexDesc.Height = glowHeight;
        glowTexDesc.MipLevels = 1;
        glowTexDesc.ArraySize = 1;
        glowTexDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        glowTexDesc.SampleDesc.Count = 1;
        glowTexDesc.Usage = D3D11_USAGE_DEFAULT;
        glowTexDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        
        hr = monitor.pDevice->CreateTexture2D(&glowTexDesc, nullptr, &monitor.pGlowTexture1);
        if (FAILED(hr)) {
            OutputDebugStringA("Failed to create GlowTexture1\n");
            glowShadersOK = false;
        }
        
        hr = monitor.pDevice->CreateTexture2D(&glowTexDesc, nullptr, &monitor.pGlowTexture2);
        if (FAILED(hr)) {
            OutputDebugStringA("Failed to create GlowTexture2\n");
            glowShadersOK = false;
        }
        
        if (monitor.pGlowTexture1) {
            monitor.pDevice->CreateRenderTargetView(monitor.pGlowTexture1, nullptr, &monitor.pGlowRTV1);
            monitor.pDevice->CreateShaderResourceView(monitor.pGlowTexture1, nullptr, &monitor.pGlowSRV1);
        }
        
        if (monitor.pGlowTexture2) {
            monitor.pDevice->CreateRenderTargetView(monitor.pGlowTexture2, nullptr, &monitor.pGlowRTV2);
            monitor.pDevice->CreateShaderResourceView(monitor.pGlowTexture2, nullptr, &monitor.pGlowSRV2);
        }
    }
    
    if (!glowShadersOK) {
        OutputDebugStringA("WARNING: Glow effect initialization failed. Glow will be disabled.\n");
    }
    
    // Создаем промежуточную текстуру для glow (полное разрешение)
    D3D11_TEXTURE2D_DESC intermediateTexDesc = {};
    intermediateTexDesc.Width = monitor.screenWidth;
    intermediateTexDesc.Height = monitor.screenHeight;
    intermediateTexDesc.MipLevels = 1;
    intermediateTexDesc.ArraySize = 1;
    intermediateTexDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    intermediateTexDesc.SampleDesc.Count = 1;
    intermediateTexDesc.Usage = D3D11_USAGE_DEFAULT;
    intermediateTexDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    
    hr = monitor.pDevice->CreateTexture2D(&intermediateTexDesc, nullptr, &monitor.pIntermediateTexture);
    if (SUCCEEDED(hr) && monitor.pIntermediateTexture) {
        monitor.pDevice->CreateRenderTargetView(monitor.pIntermediateTexture, nullptr, &monitor.pIntermediateRTV);
        monitor.pDevice->CreateShaderResourceView(monitor.pIntermediateTexture, nullptr, &monitor.pIntermediateSRV);
    }
    
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
        
        // Glow resources cleanup
        if (monitor.pGlowSRV1) { monitor.pGlowSRV1->Release(); monitor.pGlowSRV1 = nullptr; }
        if (monitor.pGlowSRV2) { monitor.pGlowSRV2->Release(); monitor.pGlowSRV2 = nullptr; }
        if (monitor.pGlowRTV1) { monitor.pGlowRTV1->Release(); monitor.pGlowRTV1 = nullptr; }
        if (monitor.pGlowRTV2) { monitor.pGlowRTV2->Release(); monitor.pGlowRTV2 = nullptr; }
        if (monitor.pGlowTexture1) { monitor.pGlowTexture1->Release(); monitor.pGlowTexture1 = nullptr; }
        if (monitor.pGlowTexture2) { monitor.pGlowTexture2->Release(); monitor.pGlowTexture2 = nullptr; }
        if (monitor.pIntermediateSRV) { monitor.pIntermediateSRV->Release(); monitor.pIntermediateSRV = nullptr; }
        if (monitor.pIntermediateRTV) { monitor.pIntermediateRTV->Release(); monitor.pIntermediateRTV = nullptr; }
        if (monitor.pIntermediateTexture) { monitor.pIntermediateTexture->Release(); monitor.pIntermediateTexture = nullptr; }
        if (monitor.pBrightPassShader) { monitor.pBrightPassShader->Release(); monitor.pBrightPassShader = nullptr; }
        if (monitor.pBlurHShader) { monitor.pBlurHShader->Release(); monitor.pBlurHShader = nullptr; }
        if (monitor.pBlurVShader) { monitor.pBlurVShader->Release(); monitor.pBlurVShader = nullptr; }
        if (monitor.pCompositeShader) { monitor.pCompositeShader->Release(); monitor.pCompositeShader = nullptr; }
        
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

DWORD WINAPI AudioThreadProc(LPVOID lpParam) {
    CoInitialize(NULL);
    
    while (!g_shouldStopAudioThread) {
        if (g_pCaptureClient && g_pRenderClient) {
            UINT32 packetLength = 0;
            HRESULT hr = g_pCaptureClient->GetNextPacketSize(&packetLength);
            
            if (SUCCEEDED(hr) && packetLength > 0) {
                BYTE* pData;
                UINT32 numFramesAvailable;
                DWORD flags;
                
                hr = g_pCaptureClient->GetBuffer(&pData, &numFramesAvailable, &flags, NULL, NULL);
                if (SUCCEEDED(hr)) {
                    // Получаем буфер для воспроизведения
                    BYTE* pRenderData;
                    hr = g_pRenderClient->GetBuffer(numFramesAvailable, &pRenderData);
                    if (SUCCEEDED(hr)) {
                        // Копируем данные на полную громкость для Discord (но поток заглушен локально)
                        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                            memset(pRenderData, 0, numFramesAvailable * 4 * 2); // 4 bytes * 2 channels
                        } else {
                            // Копируем оригинальные данные на полную громкость
                            // Discord увидит полный звук, но локально он заглушен через ISimpleAudioVolume
                            memcpy(pRenderData, pData, numFramesAvailable * 4 * 2);
                        }
                        g_pRenderClient->ReleaseBuffer(numFramesAvailable, 0);
                    }
                    g_pCaptureClient->ReleaseBuffer(numFramesAvailable);
                }
            }
        }
        Sleep(1);
    }
    
    CoUninitialize();
    return 0;
}

void StartAudioCapture() {
    if (g_pEnumerator) return; // Уже запущен
    
    CoInitialize(NULL);
    
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, 
        __uuidof(IMMDeviceEnumerator), (void**)&g_pEnumerator);
    if (FAILED(hr)) return;
    
    // Получаем устройство по умолчанию для захвата (loopback)
    hr = g_pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &g_pDevice);
    if (FAILED(hr)) return;
    
    // Создаем клиент для захвата
    hr = g_pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&g_pAudioClient);
    if (FAILED(hr)) return;
    
    WAVEFORMATEX* pwfx = NULL;
    hr = g_pAudioClient->GetMixFormat(&pwfx);
    if (FAILED(hr)) return;
    
    // Инициализируем захват в loopback режиме
    hr = g_pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
        10000000, 0, pwfx, NULL);
    if (FAILED(hr)) return;
    
    hr = g_pAudioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&g_pCaptureClient);
    if (FAILED(hr)) return;
    
    // Создаем клиент для воспроизведения (чтобы Discord видел)
    IMMDevice* pPlaybackDevice = NULL;
    hr = g_pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pPlaybackDevice);
    if (SUCCEEDED(hr)) {
        hr = pPlaybackDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&g_pPlaybackClient);
        if (SUCCEEDED(hr)) {
            hr = g_pPlaybackClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, pwfx, NULL);
            if (SUCCEEDED(hr)) {
                g_pPlaybackClient->GetService(__uuidof(IAudioRenderClient), (void**)&g_pRenderClient);
                
                // Получаем интерфейс громкости
                ISimpleAudioVolume* pVolume = NULL;
                if (SUCCEEDED(g_pPlaybackClient->GetService(__uuidof(ISimpleAudioVolume), (void**)&pVolume))) {
                    // Устанавливаем громкость на 0 для локального воспроизведения
                    pVolume->SetMasterVolume(0.0f, NULL);
                    pVolume->Release();
                }
                
                g_pPlaybackClient->Start();
            }
        }
        pPlaybackDevice->Release();
    }
    
    CoTaskMemFree(pwfx);
    
    g_pAudioClient->Start();
    
    // Запускаем поток обработки аудио
    g_shouldStopAudioThread = false;
    g_audioThread = CreateThread(NULL, 0, AudioThreadProc, NULL, 0, NULL);
}

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0 && wParam == WM_KEYDOWN) {
        KBDLLHOOKSTRUCT* pKeyboard = (KBDLLHOOKSTRUCT*)lParam;
        if (pKeyboard->vkCode == g_screenshotKey && g_isRunning) {
            TakeScreenshot();
        }
    }
    // Передаем событие дальше (не блокируем клавишу)
    return CallNextHookEx(g_keyboardHook, nCode, wParam, lParam);
}

void TakeScreenshot() {
    if (!g_isRunning || g_takingScreenshot) return;
    
    g_takingScreenshot = true;
    
    // Приостанавливаем рендеринг
    g_pauseRendering = true;
    Sleep(50); // Даем время завершить текущий кадр
    
    // Находим первый активный монитор
    MonitorData* activeMonitor = nullptr;
    for (auto& monitor : g_monitors) {
        if (monitor.isActive) {
            activeMonitor = &monitor;
            break;
        }
    }
    
    if (!activeMonitor || !activeMonitor->pSwapChain) {
        g_pauseRendering = false;
        g_takingScreenshot = false;
        return;
    }
    
    // Получаем back buffer из SwapChain (отрендеренный кадр с фильтром)
    ID3D11Texture2D* pBackBuffer = nullptr;
    HRESULT hr = activeMonitor->pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&pBackBuffer);
    if (FAILED(hr)) {
        g_pauseRendering = false;
        g_takingScreenshot = false;
        return;
    }
    
    D3D11_TEXTURE2D_DESC desc;
    pBackBuffer->GetDesc(&desc);
    
    // Создаем staging texture
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    
    ID3D11Texture2D* pStagingTexture = nullptr;
    hr = activeMonitor->pDevice->CreateTexture2D(&desc, nullptr, &pStagingTexture);
    if (FAILED(hr)) {
        pBackBuffer->Release();
        g_pauseRendering = false;
        g_takingScreenshot = false;
        return;
    }
    
    // Копируем отрендеренный кадр
    activeMonitor->pContext->CopyResource(pStagingTexture, pBackBuffer);
    
    // Читаем данные (теперь без DO_NOT_WAIT, так как рендеринг приостановлен)
    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = activeMonitor->pContext->Map(pStagingTexture, 0, D3D11_MAP_READ, 0, &mapped);
    
    if (SUCCEEDED(hr)) {
        // Создаем копию данных и исправляем цветовые каналы
        UINT dataSize = desc.Height * mapped.RowPitch;
        BYTE* pixelData = new BYTE[dataSize];
        memcpy(pixelData, mapped.pData, dataSize);
        
        activeMonitor->pContext->Unmap(pStagingTexture, 0);
        
        // Исправляем порядок цветовых каналов (BGRA -> RGBA)
        for (UINT y = 0; y < desc.Height; y++) {
            BYTE* row = pixelData + y * mapped.RowPitch;
            for (UINT x = 0; x < desc.Width; x++) {
                BYTE* pixel = row + x * 4;
                std::swap(pixel[0], pixel[2]); // Меняем B и R местами
            }
        }
        
        // Создаем bitmap
        BITMAPINFOHEADER bi = {};
        bi.biSize = sizeof(BITMAPINFOHEADER);
        bi.biWidth = desc.Width;
        bi.biHeight = -(LONG)desc.Height;
        bi.biPlanes = 1;
        bi.biBitCount = 32;
        bi.biCompression = BI_RGB;
        
        HDC hdc = GetDC(NULL);
        HBITMAP hBitmap = CreateDIBitmap(hdc, &bi, CBM_INIT, pixelData, (BITMAPINFO*)&bi, DIB_RGB_COLORS);
        ReleaseDC(NULL, hdc);
        
        if (hBitmap) {
            // Сохраняем в буфер обмена
            if (g_saveToClipboard) {
                if (OpenClipboard(g_hWnd)) {
                    EmptyClipboard();
                    SetClipboardData(CF_BITMAP, hBitmap);
                    CloseClipboard();
                }
            }
            
            // Сохраняем в файл
            if (g_saveToFolder && wcslen(g_screenshotFolder) > 0) {
                SYSTEMTIME st;
                GetLocalTime(&st);
                wchar_t fileName[MAX_PATH];
                swprintf_s(fileName, L"%s\\Screenshot_%04d%02d%02d_%02d%02d%02d.bmp", 
                    g_screenshotFolder, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
                
                HANDLE hFile = CreateFileW(fileName, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                if (hFile != INVALID_HANDLE_VALUE) {
                    BITMAPFILEHEADER bfh = {};
                    bfh.bfType = 0x4D42;
                    bfh.bfSize = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + desc.Width * desc.Height * 4;
                    bfh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
                    
                    DWORD written;
                    WriteFile(hFile, &bfh, sizeof(bfh), &written, NULL);
                    WriteFile(hFile, &bi, sizeof(bi), &written, NULL);
                    WriteFile(hFile, pixelData, desc.Width * desc.Height * 4, &written, NULL);
                    CloseHandle(hFile);
                }
            }
            
            if (!g_saveToClipboard) {
                DeleteObject(hBitmap);
            }
        }
        
        delete[] pixelData;
    }
    
    pStagingTexture->Release();
    pBackBuffer->Release();
    
    // Возобновляем рендеринг
    g_pauseRendering = false;
    g_takingScreenshot = false;
}

INT_PTR CALLBACK ScreenshotSettingsDlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam) {
    static HWND hFolderEdit, hKeyEdit, hFormatCombo, hSaveFolderCheck, hSaveClipboardCheck;
    
    switch (message) {
        case WM_INITDIALOG: {
            // Создаем элементы управления
            CreateWindowW(L"STATIC", L"Folder:", WS_CHILD | WS_VISIBLE, 10, 10, 50, 20, hDlg, NULL, NULL, NULL);
            hFolderEdit = CreateWindowW(L"EDIT", g_screenshotFolder, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_READONLY,
                70, 10, 200, 20, hDlg, NULL, NULL, NULL);
            CreateWindowW(L"BUTTON", L"Browse", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                280, 10, 60, 20, hDlg, (HMENU)1005, NULL, NULL);
            
            CreateWindowW(L"STATIC", L"Hotkey:", WS_CHILD | WS_VISIBLE, 10, 40, 50, 20, hDlg, NULL, NULL, NULL);
            hKeyEdit = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_READONLY,
                70, 40, 100, 20, hDlg, (HMENU)1004, NULL, NULL);
            
            CreateWindowW(L"STATIC", L"Format:", WS_CHILD | WS_VISIBLE, 10, 70, 50, 20, hDlg, NULL, NULL, NULL);
            hFormatCombo = CreateWindowW(L"COMBOBOX", NULL, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                70, 70, 100, 100, hDlg, (HMENU)1006, NULL, NULL);
            SendMessage(hFormatCombo, CB_ADDSTRING, 0, (LPARAM)L"BMP");
            SendMessage(hFormatCombo, CB_ADDSTRING, 0, (LPARAM)L"PNG");
            SendMessage(hFormatCombo, CB_ADDSTRING, 0, (LPARAM)L"JPG");
            SendMessage(hFormatCombo, CB_SETCURSEL, g_screenshotFormat, 0);
            
            hSaveFolderCheck = CreateWindowW(L"BUTTON", L"Save to folder", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                10, 100, 120, 20, hDlg, (HMENU)1002, NULL, NULL);
            CheckDlgButton(hDlg, 1002, g_saveToFolder ? BST_CHECKED : BST_UNCHECKED);
            
            hSaveClipboardCheck = CreateWindowW(L"BUTTON", L"Save to clipboard", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                10, 125, 120, 20, hDlg, (HMENU)1003, NULL, NULL);
            CheckDlgButton(hDlg, 1003, g_saveToClipboard ? BST_CHECKED : BST_UNCHECKED);
            
            CreateWindowW(L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                200, 160, 60, 25, hDlg, (HMENU)IDOK, NULL, NULL);
            CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                270, 160, 60, 25, hDlg, (HMENU)IDCANCEL, NULL, NULL);
            
            // Отображаем клавишу
            wchar_t keyName[32];
            swprintf_s(keyName, L"F%d", g_screenshotKey - VK_F1 + 1);
            SetWindowTextW(hKeyEdit, keyName);
            
            return TRUE;
        }
        
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case 1005: { // Browse folder
                    BROWSEINFO bi = {};
                    bi.hwndOwner = hDlg;
                    bi.lpszTitle = L"Select screenshot folder";
                    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
                    
                    LPITEMIDLIST pidl = SHBrowseForFolder(&bi);
                    if (pidl) {
                        wchar_t path[MAX_PATH];
                        if (SHGetPathFromIDList(pidl, path)) {
                            wcscpy_s(g_screenshotFolder, path);
                            SetWindowTextW(hFolderEdit, g_screenshotFolder);
                        }
                        CoTaskMemFree(pidl);
                    }
                    break;
                }
                
                case 1006: // Format combo
                    if (HIWORD(wParam) == CBN_SELCHANGE) {
                        g_screenshotFormat = (int)SendMessage(hFormatCombo, CB_GETCURSEL, 0, 0);
                    }
                    break;
                
                case IDOK:
                    g_saveToFolder = (IsDlgButtonChecked(hDlg, 1002) == BST_CHECKED);
                    g_saveToClipboard = (IsDlgButtonChecked(hDlg, 1003) == BST_CHECKED);
                    SaveSettings();
                    EndDialog(hDlg, IDOK);
                    break;
                    
                case IDCANCEL:
                    EndDialog(hDlg, IDCANCEL);
                    break;
            }
            break;
            
        case WM_KEYDOWN:
            if (wParam >= VK_F1 && wParam <= VK_F12) {
                g_screenshotKey = (int)wParam;
                wchar_t keyName[32];
                swprintf_s(keyName, L"F%d", g_screenshotKey - VK_F1 + 1);
                SetWindowTextW(hKeyEdit, keyName);
            }
            break;
    }
    return FALSE;
}

// Глобальные переменные для диалога настроек
HWND g_hScreenshotDlg = NULL;
HWND g_hFolderEdit, g_hKeyEdit, g_hFormatCombo, g_hSaveFolderCheck, g_hSaveClipboardCheck;
bool g_waitingForKey = false;

LRESULT CALLBACK ScreenshotDlgProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case 1005: { // Browse folder
                    BROWSEINFOW bi = {};
                    bi.hwndOwner = hWnd;
                    bi.lpszTitle = L"Select screenshot folder";
                    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
                    
                    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
                    if (pidl) {
                        wchar_t path[MAX_PATH];
                        if (SHGetPathFromIDListW(pidl, path)) {
                            wcscpy_s(g_screenshotFolder, path);
                            SetWindowTextW(g_hFolderEdit, g_screenshotFolder);
                        }
                        CoTaskMemFree(pidl);
                    }
                    break;
                }
                
                case 1007: // Change key button
                    g_waitingForKey = true;
                    SetWindowTextW(g_hKeyEdit, L"Press any key...");
                    SetFocus(hWnd);
                    break;
                
                case IDOK:
                    g_saveToFolder = (SendMessage(g_hSaveFolderCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    g_saveToClipboard = (SendMessage(g_hSaveClipboardCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    SaveSettings();
                    
                    DestroyWindow(hWnd);
                    g_hScreenshotDlg = NULL;
                    break;
                    
                case IDCANCEL:
                    DestroyWindow(hWnd);
                    g_hScreenshotDlg = NULL;
                    break;
            }
            break;
            
        case WM_KEYDOWN:
            if (g_waitingForKey) {
                if (wParam != VK_ESCAPE && wParam != VK_RETURN) {
                    g_screenshotKey = (int)wParam;
                    SetWindowTextW(g_hKeyEdit, GetKeyName(g_screenshotKey));
                    g_waitingForKey = false;
                } else if (wParam == VK_ESCAPE) {
                    SetWindowTextW(g_hKeyEdit, GetKeyName(g_screenshotKey));
                    g_waitingForKey = false;
                }
                return 0;
            }
            break;
            
        case WM_NCHITTEST: {
            LRESULT hit = DefWindowProc(hWnd, message, wParam, lParam);
            if (hit == HTCLIENT) {
                return HTCAPTION; // Позволяет перетаскивать окно за любую область
            }
            return hit;
        }
            
        case WM_SYSCOMMAND:
            if (wParam == SC_CLOSE) {
                DestroyWindow(hWnd);
                g_hScreenshotDlg = NULL;
                return 0;
            }
            return DefWindowProc(hWnd, message, wParam, lParam);
            
        case WM_CLOSE:
            DestroyWindow(hWnd);
            g_hScreenshotDlg = NULL;
            break;
            
        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

void ShowColorFiltersDialog() {
    if (g_hFiltersDlg) {
        SetForegroundWindow(g_hFiltersDlg);
        return;
    }
    
    // Регистрируем класс окна
    WNDCLASSW wc = {};
    wc.lpfnWndProc = ColorFiltersDlgProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = L"ColorFiltersClass";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);
    
    // Создаем окно рядом с главным окном
    RECT mainRect;
    GetWindowRect(g_hWnd, &mainRect);
    g_hFiltersDlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"ColorFiltersClass", L"Color Filters",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        mainRect.right + 10, mainRect.top, 450, 360,
        g_hWnd, NULL, GetModuleHandle(NULL), NULL);
}

void ShowEffectsDialog() {
    if (g_hEffectsDlg) {
        SetForegroundWindow(g_hEffectsDlg);
        return;
    }
    
    // Регистрируем класс окна
    WNDCLASSW wc = {};
    wc.lpfnWndProc = EffectsDlgProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = L"EffectsClass";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);
    
    // Создаем окно рядом с главным окном
    RECT mainRect;
    GetWindowRect(g_hWnd, &mainRect);
    g_hEffectsDlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"EffectsClass", L"Glow Effect",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        mainRect.right + 10, mainRect.top + 370, 470, 550,
        g_hWnd, NULL, GetModuleHandle(NULL), NULL);
}

LRESULT CALLBACK ColorFiltersDlgProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    static HWND hFilterList, hAddButton, hRemoveButton;
    static HWND hGlobalEnableCheck, hFilterEnableCheck;
    static HWND hPresetCombo;
    
    switch (message) {
        case WM_CREATE: {
            // Глобальный чекбокс
            hGlobalEnableCheck = CreateWindowW(L"BUTTON", L"Enable All Filters", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                10, 10, 200, 20, hWnd, (HMENU)3020, NULL, NULL);
            SendMessage(hGlobalEnableCheck, BM_SETCHECK, g_filtersGlobalEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
            
            // Список фильтров
            CreateWindowW(L"STATIC", L"Active Filters:", WS_CHILD | WS_VISIBLE, 10, 40, 100, 20, hWnd, NULL, NULL, NULL);
            hFilterList = CreateWindowW(L"LISTBOX", NULL, WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | LBS_NOTIFY,
                10, 60, 280, 150, hWnd, (HMENU)3001, NULL, NULL);
            
            // Preset dropdown
            CreateWindowW(L"STATIC", L"Add Preset:", WS_CHILD | WS_VISIBLE, 10, 220, 80, 20, hWnd, NULL, NULL, NULL);
            hPresetCombo = CreateWindowW(L"COMBOBOX", NULL, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                100, 218, 190, 400, hWnd, (HMENU)3024, NULL, NULL);
            
            // Заполняем пресеты (включая Identity)
            for (int i = 0; i < sizeof(g_filterPresets) / sizeof(g_filterPresets[0]); i++) {
                SendMessageW(hPresetCombo, CB_ADDSTRING, 0, (LPARAM)g_filterPresets[i].name);
            }
            SendMessage(hPresetCombo, CB_SETCURSEL, 0, 0);
            
            hAddButton = CreateWindowW(L"BUTTON", L"Add", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                10, 250, 135, 30, hWnd, (HMENU)3002, NULL, NULL);
            hRemoveButton = CreateWindowW(L"BUTTON", L"Remove", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                155, 250, 135, 30, hWnd, (HMENU)3003, NULL, NULL);
            
            hFilterEnableCheck = CreateWindowW(L"BUTTON", L"Enable Selected", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                10, 290, 150, 20, hWnd, (HMENU)3006, NULL, NULL);
            SendMessage(hFilterEnableCheck, BM_SETCHECK, BST_CHECKED, 0);
            
            CreateWindowW(L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                300, 10, 130, 30, hWnd, (HMENU)IDCANCEL, NULL, NULL);
            
            // Заполняем список существующих фильтров
            for (const auto& filter : g_colorFilters) {
                wchar_t text[64];
                swprintf_s(text, L"%s%s", 
                    g_filterPresets[filter.presetIndex].name,
                    filter.enabled ? L"" : L" (Disabled)");
                SendMessageW(hFilterList, LB_ADDSTRING, 0, (LPARAM)text);
            }
            
            return 0;
        }
        
        case WM_HSCROLL: {
            return 0;
        }
        
        case WM_COMMAND: {
            switch (LOWORD(wParam)) {
                case 3020: // Global enable
                    g_filtersGlobalEnabled = (SendMessage(hGlobalEnableCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    // Помечаем все активные мониторы для обновления констант
                    for (auto& monitor : g_monitors) {
                        if (monitor.isActive) {
                            monitor.constantsNeedUpdate = true;
                        }
                    }
                    break;
                    
                case 3001: // Filter list
                    if (HIWORD(wParam) == LBN_SELCHANGE) {
                        int sel = (int)SendMessage(hFilterList, LB_GETCURSEL, 0, 0);
                        if (sel >= 0 && sel < (int)g_colorFilters.size()) {
                            g_selectedFilterIndex = sel;
                            const auto& filter = g_colorFilters[sel];
                            SendMessage(hFilterEnableCheck, BM_SETCHECK, filter.enabled ? BST_CHECKED : BST_UNCHECKED, 0);
                        }
                    }
                    break;
                    
                case 3002: // Add
                    {
                        int presetIdx = (int)SendMessage(hPresetCombo, CB_GETCURSEL, 0, 0); // Индекс напрямую из списка
                        
                        ColorFilter newFilter;
                        newFilter.presetIndex = presetIdx;
                        newFilter.color = RGB(255, 255, 255);
                        newFilter.brightness = 1.0f;
                        newFilter.contrast = 1.0f;
                        newFilter.saturation = 1.0f;
                        newFilter.enabled = true;
                        
                        g_colorFilters.push_back(newFilter);
                        
                        wchar_t text[64];
                        swprintf_s(text, L"%s", g_filterPresets[presetIdx].name);
                        SendMessageW(hFilterList, LB_ADDSTRING, 0, (LPARAM)text);
                        
                        // Помечаем все активные мониторы для обновления констант
                        for (auto& monitor : g_monitors) {
                            if (monitor.isActive) {
                                monitor.constantsNeedUpdate = true;
                            }
                        }
                    }
                    break;
                    
                case 3003: // Remove
                    {
                        int sel = (int)SendMessage(hFilterList, LB_GETCURSEL, 0, 0);
                        if (sel >= 0 && sel < (int)g_colorFilters.size()) {
                            g_colorFilters.erase(g_colorFilters.begin() + sel);
                            SendMessage(hFilterList, LB_DELETESTRING, sel, 0);
                            g_selectedFilterIndex = -1;
                            
                            // Помечаем все активные мониторы для обновления констант
                            for (auto& monitor : g_monitors) {
                                if (monitor.isActive) {
                                    monitor.constantsNeedUpdate = true;
                                }
                            }
                        }
                    }
                    break;
                    
                case 3006: // Filter enable checkbox
                    {
                        int sel = (int)SendMessage(hFilterList, LB_GETCURSEL, 0, 0);
                        if (sel >= 0 && sel < (int)g_colorFilters.size()) {
                            g_colorFilters[sel].enabled = (SendMessage(hFilterEnableCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
                            
                            // Обновляем текст в списке
                            wchar_t text[64];
                            swprintf_s(text, L"%s%s", 
                                g_filterPresets[g_colorFilters[sel].presetIndex].name,
                                g_colorFilters[sel].enabled ? L"" : L" (Disabled)");
                            SendMessage(hFilterList, LB_DELETESTRING, sel, 0);
                            SendMessage(hFilterList, LB_INSERTSTRING, sel, (LPARAM)text);
                            SendMessage(hFilterList, LB_SETCURSEL, sel, 0);
                            
                            // Помечаем все активные мониторы для обновления констант
                            for (auto& monitor : g_monitors) {
                                if (monitor.isActive) {
                                    monitor.constantsNeedUpdate = true;
                                }
                            }
                        }
                    }
                    break;
                    
                case IDCANCEL:
                    DestroyWindow(hWnd);
                    g_hFiltersDlg = NULL;
                    break;
            }
            return 0;
        }
        
        case WM_NCHITTEST: {
            LRESULT hit = DefWindowProc(hWnd, message, wParam, lParam);
            if (hit == HTCLIENT) return HTCAPTION;
            return hit;
        }
        
        case WM_CLOSE:
            DestroyWindow(hWnd);
            g_hFiltersDlg = NULL;
            break;
            
        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

LRESULT CALLBACK EffectsDlgProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    static HWND hEnableCheck;
    static HWND hIntensityTrackbar, hIntensityLabel;
    static HWND hThresholdTrackbar, hThresholdLabel;
    static HWND hRadiusTrackbar, hRadiusLabel;
    static HWND hSaturationTrackbar, hSaturationLabel;
    static HWND hBlurPassesTrackbar, hBlurPassesLabel;
    static HWND hDownsampleCombo, hDownsampleLabel;
    
    switch (message) {
        case WM_CREATE: {
            // Заголовок
            CreateWindowW(L"STATIC", L"Glow Effect Settings", WS_CHILD | WS_VISIBLE | SS_CENTER,
                10, 10, 430, 25, hWnd, NULL, NULL, NULL);
            HFONT hFont = CreateFontW(18, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            SendMessage(GetDlgItem(hWnd, -1), WM_SETFONT, (WPARAM)hFont, TRUE);
            
            // Enable checkbox
            hEnableCheck = CreateWindowW(L"BUTTON", L"Enable Glow Effect", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                10, 45, 200, 25, hWnd, (HMENU)5001, NULL, NULL);
            SendMessage(hEnableCheck, BM_SETCHECK, g_glowEffect.enabled ? BST_CHECKED : BST_UNCHECKED, 0);
            
            int yPos = 85;
            
            // Intensity slider (0.0 - 1.0)
            hIntensityLabel = CreateWindowW(L"STATIC", L"Intensity: 1.000", WS_CHILD | WS_VISIBLE,
                10, yPos, 200, 20, hWnd, NULL, NULL, NULL);
            hIntensityTrackbar = CreateWindowW(TRACKBAR_CLASSW, NULL, WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_AUTOTICKS,
                10, yPos + 20, 430, 35, hWnd, (HMENU)5002, NULL, NULL);
            SendMessage(hIntensityTrackbar, TBM_SETRANGE, TRUE, MAKELONG(0, 1000));
            SendMessage(hIntensityTrackbar, TBM_SETPOS, TRUE, (int)(g_glowEffect.intensity * 1000));
            SendMessage(hIntensityTrackbar, TBM_SETTICFREQ, 100, 0);
            yPos += 65;
            
            // Threshold slider (0.0 - 1.0)
            hThresholdLabel = CreateWindowW(L"STATIC", L"Threshold: 0.700", WS_CHILD | WS_VISIBLE,
                10, yPos, 200, 20, hWnd, NULL, NULL, NULL);
            hThresholdTrackbar = CreateWindowW(TRACKBAR_CLASSW, NULL, WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_AUTOTICKS,
                10, yPos + 20, 430, 35, hWnd, (HMENU)5003, NULL, NULL);
            SendMessage(hThresholdTrackbar, TBM_SETRANGE, TRUE, MAKELONG(0, 1000));
            SendMessage(hThresholdTrackbar, TBM_SETPOS, TRUE, (int)(g_glowEffect.threshold * 1000));
            SendMessage(hThresholdTrackbar, TBM_SETTICFREQ, 100, 0);
            yPos += 65;
            
            // Radius slider (0.1 - 10.0)
            hRadiusLabel = CreateWindowW(L"STATIC", L"Radius: 1.000", WS_CHILD | WS_VISIBLE,
                10, yPos, 200, 20, hWnd, NULL, NULL, NULL);
            hRadiusTrackbar = CreateWindowW(TRACKBAR_CLASSW, NULL, WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_AUTOTICKS,
                10, yPos + 20, 430, 35, hWnd, (HMENU)5004, NULL, NULL);
            SendMessage(hRadiusTrackbar, TBM_SETRANGE, TRUE, MAKELONG(100, 10000));
            SendMessage(hRadiusTrackbar, TBM_SETPOS, TRUE, (int)(g_glowEffect.radius * 1000));
            SendMessage(hRadiusTrackbar, TBM_SETTICFREQ, 1000, 0);
            yPos += 65;
            
            // Saturation slider (0.0 - 3.0)
            hSaturationLabel = CreateWindowW(L"STATIC", L"Saturation: 1.200", WS_CHILD | WS_VISIBLE,
                10, yPos, 200, 20, hWnd, NULL, NULL, NULL);
            hSaturationTrackbar = CreateWindowW(TRACKBAR_CLASSW, NULL, WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_AUTOTICKS,
                10, yPos + 20, 430, 35, hWnd, (HMENU)5005, NULL, NULL);
            SendMessage(hSaturationTrackbar, TBM_SETRANGE, TRUE, MAKELONG(0, 3000));
            SendMessage(hSaturationTrackbar, TBM_SETPOS, TRUE, (int)(g_glowEffect.saturation * 1000));
            SendMessage(hSaturationTrackbar, TBM_SETTICFREQ, 300, 0);
            yPos += 65;
            
            // Blur Passes slider (1 - 5)
            hBlurPassesLabel = CreateWindowW(L"STATIC", L"Blur Passes: 2", WS_CHILD | WS_VISIBLE,
                10, yPos, 200, 20, hWnd, NULL, NULL, NULL);
            hBlurPassesTrackbar = CreateWindowW(TRACKBAR_CLASSW, NULL, WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_AUTOTICKS,
                10, yPos + 20, 430, 35, hWnd, (HMENU)5006, NULL, NULL);
            SendMessage(hBlurPassesTrackbar, TBM_SETRANGE, TRUE, MAKELONG(1, 5));
            SendMessage(hBlurPassesTrackbar, TBM_SETPOS, TRUE, g_glowEffect.blurPasses);
            SendMessage(hBlurPassesTrackbar, TBM_SETTICFREQ, 1, 0);
            yPos += 65;
            
            // Downsample dropdown
            hDownsampleLabel = CreateWindowW(L"STATIC", L"Quality (Downsample):", WS_CHILD | WS_VISIBLE,
                10, yPos, 200, 20, hWnd, NULL, NULL, NULL);
            hDownsampleCombo = CreateWindowW(L"COMBOBOX", NULL, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                220, yPos - 3, 220, 100, hWnd, (HMENU)5007, NULL, NULL);
            SendMessageW(hDownsampleCombo, CB_ADDSTRING, 0, (LPARAM)L"Full Resolution (1x)");
            SendMessageW(hDownsampleCombo, CB_ADDSTRING, 0, (LPARAM)L"Half Resolution (2x) - Recommended");
            SendMessageW(hDownsampleCombo, CB_ADDSTRING, 0, (LPARAM)L"Quarter Resolution (4x) - Fast");
            int downsampleIdx = (g_glowEffect.downsample == 1.0f) ? 0 : (g_glowEffect.downsample == 2.0f) ? 1 : 2;
            SendMessage(hDownsampleCombo, CB_SETCURSEL, downsampleIdx, 0);
            yPos += 40;
            
            // Close button
            CreateWindowW(L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                170, yPos + 10, 110, 35, hWnd, (HMENU)IDCANCEL, NULL, NULL);
            
            return 0;
        }
        
        case WM_HSCROLL: {
            bool needUpdate = false;
            
            if ((HWND)lParam == hIntensityTrackbar) {
                int pos = (int)SendMessage(hIntensityTrackbar, TBM_GETPOS, 0, 0);
                g_glowEffect.intensity = pos / 1000.0f;
                wchar_t buffer[64];
                swprintf_s(buffer, L"Intensity: %.3f (Glow strength)", g_glowEffect.intensity);
                SetWindowTextW(hIntensityLabel, buffer);
                needUpdate = true;
            }
            else if ((HWND)lParam == hThresholdTrackbar) {
                int pos = (int)SendMessage(hThresholdTrackbar, TBM_GETPOS, 0, 0);
                g_glowEffect.threshold = pos / 1000.0f;
                wchar_t buffer[64];
                swprintf_s(buffer, L"Threshold: %.3f (Brightness cutoff)", g_glowEffect.threshold);
                SetWindowTextW(hThresholdLabel, buffer);
                needUpdate = true;
            }
            else if ((HWND)lParam == hRadiusTrackbar) {
                int pos = (int)SendMessage(hRadiusTrackbar, TBM_GETPOS, 0, 0);
                g_glowEffect.radius = pos / 1000.0f;
                wchar_t buffer[64];
                swprintf_s(buffer, L"Radius: %.3f (Glow spread)", g_glowEffect.radius);
                SetWindowTextW(hRadiusLabel, buffer);
                needUpdate = true;
            }
            else if ((HWND)lParam == hSaturationTrackbar) {
                int pos = (int)SendMessage(hSaturationTrackbar, TBM_GETPOS, 0, 0);
                g_glowEffect.saturation = pos / 1000.0f;
                wchar_t buffer[64];
                swprintf_s(buffer, L"Saturation: %.3f (Color intensity)", g_glowEffect.saturation);
                SetWindowTextW(hSaturationLabel, buffer);
                needUpdate = true;
            }
            else if ((HWND)lParam == hBlurPassesTrackbar) {
                int pos = (int)SendMessage(hBlurPassesTrackbar, TBM_GETPOS, 0, 0);
                g_glowEffect.blurPasses = pos;
                wchar_t buffer[64];
                swprintf_s(buffer, L"Blur Passes: %d (Quality)", pos);
                SetWindowTextW(hBlurPassesLabel, buffer);
                needUpdate = true;
            }
            
            if (needUpdate) {
                for (auto& monitor : g_monitors) {
                    if (monitor.isActive) {
                        monitor.constantsNeedUpdate = true;
                    }
                }
            }
            return 0;
        }
        
        case WM_COMMAND: {
            switch (LOWORD(wParam)) {
                case 5001: { // Enable checkbox
                    g_glowEffect.enabled = (SendMessage(hEnableCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);
                    
                    bool shadersReady = false;
                    for (auto& monitor : g_monitors) {
                        if (monitor.isActive && monitor.pBrightPassShader && monitor.pGlowRTV1) {
                            shadersReady = true;
                            break;
                        }
                    }
                    
                    if (g_glowEffect.enabled && !shadersReady) {
                        MessageBoxW(hWnd, L"Glow shaders not initialized! Please restart the application.", L"Glow Error", MB_OK | MB_ICONWARNING);
                        g_glowEffect.enabled = false;
                        SendMessage(hEnableCheck, BM_SETCHECK, BST_UNCHECKED, 0);
                    }
                    
                    for (auto& monitor : g_monitors) {
                        if (monitor.isActive) {
                            monitor.constantsNeedUpdate = true;
                        }
                    }
                    break;
                }
                    
                case 5007: { // Downsample combobox
                    if (HIWORD(wParam) == CBN_SELCHANGE) {
                        int sel = (int)SendMessage(hDownsampleCombo, CB_GETCURSEL, 0, 0);
                        float oldDownsample = g_glowEffect.downsample;
                        g_glowEffect.downsample = (sel == 0) ? 1.0f : (sel == 1) ? 2.0f : 4.0f;
                        
                        // Если изменился downsample, нужно пересоздать glow текстуры
                        if (oldDownsample != g_glowEffect.downsample) {
                            for (auto& monitor : g_monitors) {
                                if (monitor.isActive) {
                                    // Освобождаем старые текстуры
                                    if (monitor.pGlowSRV1) { monitor.pGlowSRV1->Release(); monitor.pGlowSRV1 = nullptr; }
                                    if (monitor.pGlowSRV2) { monitor.pGlowSRV2->Release(); monitor.pGlowSRV2 = nullptr; }
                                    if (monitor.pGlowRTV1) { monitor.pGlowRTV1->Release(); monitor.pGlowRTV1 = nullptr; }
                                    if (monitor.pGlowRTV2) { monitor.pGlowRTV2->Release(); monitor.pGlowRTV2 = nullptr; }
                                    if (monitor.pGlowTexture1) { monitor.pGlowTexture1->Release(); monitor.pGlowTexture1 = nullptr; }
                                    if (monitor.pGlowTexture2) { monitor.pGlowTexture2->Release(); monitor.pGlowTexture2 = nullptr; }
                                    
                                    // Создаем новые с новым разрешением
                                    int glowWidth = monitor.screenWidth / (int)g_glowEffect.downsample;
                                    int glowHeight = monitor.screenHeight / (int)g_glowEffect.downsample;
                                    
                                    D3D11_TEXTURE2D_DESC glowTexDesc = {};
                                    glowTexDesc.Width = glowWidth;
                                    glowTexDesc.Height = glowHeight;
                                    glowTexDesc.MipLevels = 1;
                                    glowTexDesc.ArraySize = 1;
                                    glowTexDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                                    glowTexDesc.SampleDesc.Count = 1;
                                    glowTexDesc.Usage = D3D11_USAGE_DEFAULT;
                                    glowTexDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                                    
                                    monitor.pDevice->CreateTexture2D(&glowTexDesc, nullptr, &monitor.pGlowTexture1);
                                    monitor.pDevice->CreateTexture2D(&glowTexDesc, nullptr, &monitor.pGlowTexture2);
                                    
                                    if (monitor.pGlowTexture1) {
                                        monitor.pDevice->CreateRenderTargetView(monitor.pGlowTexture1, nullptr, &monitor.pGlowRTV1);
                                        monitor.pDevice->CreateShaderResourceView(monitor.pGlowTexture1, nullptr, &monitor.pGlowSRV1);
                                    }
                                    
                                    if (monitor.pGlowTexture2) {
                                        monitor.pDevice->CreateRenderTargetView(monitor.pGlowTexture2, nullptr, &monitor.pGlowRTV2);
                                        monitor.pDevice->CreateShaderResourceView(monitor.pGlowTexture2, nullptr, &monitor.pGlowSRV2);
                                    }
                                }
                            }
                        }
                    }
                    break;
                }
                    
                case IDCANCEL:
                    SaveSettings();
                    DestroyWindow(hWnd);
                    g_hEffectsDlg = NULL;
                    break;
            }
            return 0;
        }
        
        case WM_NCHITTEST: {
            LRESULT hit = DefWindowProc(hWnd, message, wParam, lParam);
            if (hit == HTCLIENT) return HTCAPTION;
            return hit;
        }
        
        case WM_CLOSE:
            SaveSettings();
            DestroyWindow(hWnd);
            g_hEffectsDlg = NULL;
            break;
            
        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

void ShowScreenshotSettings() {
    if (g_hScreenshotDlg) {
        SetForegroundWindow(g_hScreenshotDlg);
        return;
    }
    
    // Регистрируем класс окна
    WNDCLASSW wc = {};
    wc.lpfnWndProc = ScreenshotDlgProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = L"ScreenshotSettingsClass";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);
    
    // Создаем окно рядом с главным окном
    RECT mainRect;
    GetWindowRect(g_hWnd, &mainRect);
    g_hScreenshotDlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"ScreenshotSettingsClass", L"Screenshot Settings",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        mainRect.right + 10, mainRect.top, 350, 250,
        g_hWnd, NULL, GetModuleHandle(NULL), NULL);
    
    if (!g_hScreenshotDlg) return;
    
    // Создаем элементы управления
    CreateWindowW(L"STATIC", L"Save folder:", WS_CHILD | WS_VISIBLE, 10, 15, 80, 20, g_hScreenshotDlg, NULL, NULL, NULL);
    g_hFolderEdit = CreateWindowW(L"EDIT", g_screenshotFolder, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_READONLY,
        10, 35, 200, 22, g_hScreenshotDlg, NULL, NULL, NULL);
    CreateWindowW(L"BUTTON", L"Browse", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        220, 35, 70, 22, g_hScreenshotDlg, (HMENU)1005, NULL, NULL);
    
    CreateWindowW(L"STATIC", L"Hotkey:", WS_CHILD | WS_VISIBLE, 10, 70, 60, 20, g_hScreenshotDlg, NULL, NULL, NULL);
    g_hKeyEdit = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_READONLY,
        10, 90, 100, 22, g_hScreenshotDlg, NULL, NULL, NULL);
    CreateWindowW(L"BUTTON", L"Change", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        120, 90, 70, 22, g_hScreenshotDlg, (HMENU)1007, NULL, NULL);
    
    g_hSaveFolderCheck = CreateWindowW(L"BUTTON", L"Save to folder", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        10, 125, 150, 20, g_hScreenshotDlg, (HMENU)1002, NULL, NULL);
    SendMessage(g_hSaveFolderCheck, BM_SETCHECK, g_saveToFolder ? BST_CHECKED : BST_UNCHECKED, 0);
    
    g_hSaveClipboardCheck = CreateWindowW(L"BUTTON", L"Save to clipboard", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        10, 150, 150, 20, g_hScreenshotDlg, (HMENU)1003, NULL, NULL);
    SendMessage(g_hSaveClipboardCheck, BM_SETCHECK, g_saveToClipboard ? BST_CHECKED : BST_UNCHECKED, 0);
    
    CreateWindowW(L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        180, 180, 70, 30, g_hScreenshotDlg, (HMENU)IDOK, NULL, NULL);
    CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        260, 180, 70, 30, g_hScreenshotDlg, (HMENU)IDCANCEL, NULL, NULL);
    
    // Отображаем текущую клавишу
    SetWindowTextW(g_hKeyEdit, GetKeyName(g_screenshotKey));
}

wchar_t* GetKeyName(int vkCode) {
    static wchar_t keyName[32];
    switch (vkCode) {
        case VK_F1: case VK_F2: case VK_F3: case VK_F4: case VK_F5: case VK_F6:
        case VK_F7: case VK_F8: case VK_F9: case VK_F10: case VK_F11: case VK_F12:
            swprintf_s(keyName, L"F%d", vkCode - VK_F1 + 1);
            break;
        case VK_SPACE: wcscpy_s(keyName, L"Space"); break;
        case VK_RETURN: wcscpy_s(keyName, L"Enter"); break;
        case VK_ESCAPE: wcscpy_s(keyName, L"Esc"); break;
        case VK_TAB: wcscpy_s(keyName, L"Tab"); break;
        case VK_BACK: wcscpy_s(keyName, L"Backspace"); break;
        case VK_DELETE: wcscpy_s(keyName, L"Delete"); break;
        case VK_INSERT: wcscpy_s(keyName, L"Insert"); break;
        case VK_HOME: wcscpy_s(keyName, L"Home"); break;
        case VK_END: wcscpy_s(keyName, L"End"); break;
        case VK_PRIOR: wcscpy_s(keyName, L"Page Up"); break;
        case VK_NEXT: wcscpy_s(keyName, L"Page Down"); break;
        case VK_UP: wcscpy_s(keyName, L"Up"); break;
        case VK_DOWN: wcscpy_s(keyName, L"Down"); break;
        case VK_LEFT: wcscpy_s(keyName, L"Left"); break;
        case VK_RIGHT: wcscpy_s(keyName, L"Right"); break;
        case VK_SHIFT: wcscpy_s(keyName, L"Shift"); break;
        case VK_CONTROL: wcscpy_s(keyName, L"Ctrl"); break;
        case VK_MENU: wcscpy_s(keyName, L"Alt"); break;
        default:
            if (vkCode >= '0' && vkCode <= '9') {
                swprintf_s(keyName, L"%c", vkCode);
            } else if (vkCode >= 'A' && vkCode <= 'Z') {
                swprintf_s(keyName, L"%c", vkCode);
            } else {
                swprintf_s(keyName, L"Key %d", vkCode);
            }
            break;
    }
    return keyName;
}

void StopAudioCapture() {
    g_shouldStopAudioThread = true;
    
    if (g_audioThread) {
        WaitForSingleObject(g_audioThread, 1000);
        CloseHandle(g_audioThread);
        g_audioThread = NULL;
    }
    
    if (g_pAudioClient) { g_pAudioClient->Stop(); g_pAudioClient->Release(); g_pAudioClient = NULL; }
    if (g_pPlaybackClient) { g_pPlaybackClient->Stop(); g_pPlaybackClient->Release(); g_pPlaybackClient = NULL; }
    if (g_pCaptureClient) { g_pCaptureClient->Release(); g_pCaptureClient = NULL; }
    if (g_pRenderClient) { g_pRenderClient->Release(); g_pRenderClient = NULL; }
    if (g_pDevice) { g_pDevice->Release(); g_pDevice = NULL; }
    if (g_pEnumerator) { g_pEnumerator->Release(); g_pEnumerator = NULL; }
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
        RegSetValueExW(hKey, L"EnableAudioCapture", 0, REG_DWORD, (BYTE*)&g_enableAudioCapture, sizeof(DWORD));
        RegSetValueExW(hKey, L"EnableSelective", 0, REG_DWORD, (BYTE*)&g_enableSelective, sizeof(DWORD));
        RegSetValueExW(hKey, L"ScreenshotKey", 0, REG_DWORD, (BYTE*)&g_screenshotKey, sizeof(DWORD));
        RegSetValueExW(hKey, L"SaveToFolder", 0, REG_DWORD, (BYTE*)&g_saveToFolder, sizeof(DWORD));
        RegSetValueExW(hKey, L"SaveToClipboard", 0, REG_DWORD, (BYTE*)&g_saveToClipboard, sizeof(DWORD));
        RegSetValueExW(hKey, L"ScreenshotFormat", 0, REG_DWORD, (BYTE*)&g_screenshotFormat, sizeof(DWORD));
        RegSetValueExW(hKey, L"ScreenshotFolder", 0, REG_SZ, (BYTE*)g_screenshotFolder, ((DWORD)wcslen(g_screenshotFolder) + 1) * sizeof(wchar_t));
        
        // Сохраняем цвета
        DWORD colorCount = (DWORD)g_targetColors.size();
        RegSetValueExW(hKey, L"ColorCount", 0, REG_DWORD, (BYTE*)&colorCount, sizeof(DWORD));
        if (colorCount > 0) {
            RegSetValueExW(hKey, L"Colors", 0, REG_BINARY, (BYTE*)g_targetColors.data(), colorCount * sizeof(COLORREF));
        }
        
        // Сохраняем настройки фильтров
        RegSetValueExW(hKey, L"FiltersGlobalEnabled", 0, REG_DWORD, (BYTE*)&g_filtersGlobalEnabled, sizeof(DWORD));
        DWORD filterCount = (DWORD)g_colorFilters.size();
        RegSetValueExW(hKey, L"FilterCount", 0, REG_DWORD, (BYTE*)&filterCount, sizeof(DWORD));
        
        // Сохраняем каждый фильтр
        for (size_t i = 0; i < g_colorFilters.size(); i++) {
            wchar_t keyName[64];
            swprintf_s(keyName, L"Filter%d_PresetIndex", (int)i);
            RegSetValueExW(hKey, keyName, 0, REG_DWORD, (BYTE*)&g_colorFilters[i].presetIndex, sizeof(DWORD));
            
            swprintf_s(keyName, L"Filter%d_Enabled", (int)i);
            DWORD enabled = g_colorFilters[i].enabled ? 1 : 0;
            RegSetValueExW(hKey, keyName, 0, REG_DWORD, (BYTE*)&enabled, sizeof(DWORD));
        }
        
        // Сохраняем настройки Glow эффекта
        DWORD glowEnabled = g_glowEffect.enabled ? 1 : 0;
        RegSetValueExW(hKey, L"GlowEnabled", 0, REG_DWORD, (BYTE*)&glowEnabled, sizeof(DWORD));
        RegSetValueExW(hKey, L"GlowIntensity", 0, REG_BINARY, (BYTE*)&g_glowEffect.intensity, sizeof(float));
        RegSetValueExW(hKey, L"GlowThreshold", 0, REG_BINARY, (BYTE*)&g_glowEffect.threshold, sizeof(float));
        RegSetValueExW(hKey, L"GlowRadius", 0, REG_BINARY, (BYTE*)&g_glowEffect.radius, sizeof(float));
        RegSetValueExW(hKey, L"GlowSaturation", 0, REG_BINARY, (BYTE*)&g_glowEffect.saturation, sizeof(float));
        DWORD blurPasses = g_glowEffect.blurPasses;
        RegSetValueExW(hKey, L"GlowBlurPasses", 0, REG_DWORD, (BYTE*)&blurPasses, sizeof(DWORD));
        RegSetValueExW(hKey, L"GlowDownsample", 0, REG_BINARY, (BYTE*)&g_glowEffect.downsample, sizeof(float));
        
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
        RegQueryValueExW(hKey, L"EnableAudioCapture", NULL, NULL, (BYTE*)&g_enableAudioCapture, &size);
        RegQueryValueExW(hKey, L"EnableSelective", NULL, NULL, (BYTE*)&g_enableSelective, &size);
        RegQueryValueExW(hKey, L"ScreenshotKey", NULL, NULL, (BYTE*)&g_screenshotKey, &size);
        RegQueryValueExW(hKey, L"SaveToFolder", NULL, NULL, (BYTE*)&g_saveToFolder, &size);
        RegQueryValueExW(hKey, L"SaveToClipboard", NULL, NULL, (BYTE*)&g_saveToClipboard, &size);
        RegQueryValueExW(hKey, L"ScreenshotFormat", NULL, NULL, (BYTE*)&g_screenshotFormat, &size);
        
        // Загружаем папку скриншотов
        size = sizeof(g_screenshotFolder);
        if (RegQueryValueExW(hKey, L"ScreenshotFolder", NULL, NULL, (BYTE*)g_screenshotFolder, &size) != ERROR_SUCCESS) {
            // По умолчанию - папка Pictures
            SHGetFolderPathW(NULL, CSIDL_MYPICTURES, NULL, SHGFP_TYPE_CURRENT, g_screenshotFolder);
        }
        
        // Загружаем цвета
        DWORD colorCount = 0;
        if (RegQueryValueExW(hKey, L"ColorCount", NULL, NULL, (BYTE*)&colorCount, &size) == ERROR_SUCCESS && colorCount > 0) {
            g_targetColors.resize(colorCount);
            size = colorCount * sizeof(COLORREF);
            RegQueryValueExW(hKey, L"Colors", NULL, NULL, (BYTE*)g_targetColors.data(), &size);
        }
        
        // Загружаем настройки фильтров
        RegQueryValueExW(hKey, L"FiltersGlobalEnabled", NULL, NULL, (BYTE*)&g_filtersGlobalEnabled, &size);
        
        DWORD filterCount = 0;
        if (RegQueryValueExW(hKey, L"FilterCount", NULL, NULL, (BYTE*)&filterCount, &size) == ERROR_SUCCESS && filterCount > 0) {
            g_colorFilters.clear();
            for (DWORD i = 0; i < filterCount; i++) {
                ColorFilter filter;
                
                wchar_t keyName[64];
                swprintf_s(keyName, L"Filter%d_PresetIndex", i);
                DWORD presetIndex = 0;
                if (RegQueryValueExW(hKey, keyName, NULL, NULL, (BYTE*)&presetIndex, &size) == ERROR_SUCCESS) {
                    filter.presetIndex = presetIndex;
                }
                
                swprintf_s(keyName, L"Filter%d_Enabled", i);
                DWORD enabled = 1;
                if (RegQueryValueExW(hKey, keyName, NULL, NULL, (BYTE*)&enabled, &size) == ERROR_SUCCESS) {
                    filter.enabled = (enabled != 0);
                }
                
                g_colorFilters.push_back(filter);
            }
        }
        
        // Загружаем настройки эффектов
        // Загружаем настройки Glow эффекта
        DWORD glowEnabled = 0;
        if (RegQueryValueExW(hKey, L"GlowEnabled", NULL, NULL, (BYTE*)&glowEnabled, &size) == ERROR_SUCCESS) {
            g_glowEffect.enabled = (glowEnabled != 0);
        }
        
        size = sizeof(float);
        RegQueryValueExW(hKey, L"GlowIntensity", NULL, NULL, (BYTE*)&g_glowEffect.intensity, &size);
        
        size = sizeof(float);
        RegQueryValueExW(hKey, L"GlowThreshold", NULL, NULL, (BYTE*)&g_glowEffect.threshold, &size);
        
        size = sizeof(float);
        RegQueryValueExW(hKey, L"GlowRadius", NULL, NULL, (BYTE*)&g_glowEffect.radius, &size);
        
        size = sizeof(float);
        RegQueryValueExW(hKey, L"GlowSaturation", NULL, NULL, (BYTE*)&g_glowEffect.saturation, &size);
        
        DWORD blurPasses = 2;
        size = sizeof(DWORD);
        if (RegQueryValueExW(hKey, L"GlowBlurPasses", NULL, NULL, (BYTE*)&blurPasses, &size) == ERROR_SUCCESS) {
            g_glowEffect.blurPasses = blurPasses;
        }
        
        size = sizeof(float);
        RegQueryValueExW(hKey, L"GlowDownsample", NULL, NULL, (BYTE*)&g_glowEffect.downsample, &size);
        
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
            WriteFile(hFile, &g_enableAudioCapture, sizeof(bool), &written, NULL);
            
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
            ReadFile(hFile, &g_enableAudioCapture, sizeof(bool), &read, NULL);
            
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
            SendMessage(g_hSelectiveCheckbox, BM_SETCHECK, g_enableSelective ? BST_CHECKED : BST_UNCHECKED, 0);
            SendMessage(g_hGrayscaleCombo, CB_SETCURSEL, g_grayscaleMode, 0);
            SendMessage(g_hAudioCheckbox, BM_SETCHECK, g_enableAudioCapture ? BST_CHECKED : BST_UNCHECKED, 0);
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
    
    // Проверяем паузу для скриншота
    if (g_pauseRendering) return;
    
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
            
            // Заполняем данные цветовых фильтров
            constants->selectiveEnabled = g_enableSelective ? 1 : 0;
            constants->padding1[0] = 0;
            constants->padding1[1] = 0;
            constants->padding1[2] = 0;
            
            // Комбинируем матрицы всех активных фильтров
            // Начинаем с единичной матрицы (Identity) в формате 5x5 (4x5)
            float combinedMatrix[20] = {
                1, 0, 0, 0, 0,
                0, 1, 0, 0, 0,
                0, 0, 1, 0, 0,
                0, 0, 0, 1, 0
            };
            
            // Если фильтры включены, перемножаем матрицы всех активных фильтров
            if (g_filtersGlobalEnabled && !g_colorFilters.empty()) {
                for (const auto& filter : g_colorFilters) {
                    if (filter.enabled) {
                        // Берем матрицу текущего пресета (формат 4x5)
                        const float* filterMatrix = g_filterPresets[filter.presetIndex].matrix;
                        // Накладываем фильтр: Combined = Combined * Filter
                        MultiplyMatrices(combinedMatrix, filterMatrix, combinedMatrix);
                    }
                }
            }
            
            // Разделяем комбинированную матрицу 5x5 на 4x4 + offset
            for (int row = 0; row < 4; row++) {
                constants->colorMatrix[row * 4 + 0] = combinedMatrix[row * 5 + 0];
                constants->colorMatrix[row * 4 + 1] = combinedMatrix[row * 5 + 1];
                constants->colorMatrix[row * 4 + 2] = combinedMatrix[row * 5 + 2];
                constants->colorMatrix[row * 4 + 3] = combinedMatrix[row * 5 + 3];
                constants->colorOffset[row] = combinedMatrix[row * 5 + 4];
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
    
    static const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    
    monitor.pContext->IASetInputLayout(monitor.pInputLayout);
    monitor.pContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    
    static const UINT stride = sizeof(Vertex);
    static const UINT offset = 0;
    monitor.pContext->IASetVertexBuffers(0, 1, &monitor.pVertexBuffer, &stride, &offset);
    monitor.pContext->VSSetShader(monitor.pVertexShader, nullptr, 0);
    monitor.pContext->PSSetSamplers(0, 1, &monitor.pSamplerState);
    
    // Если Glow включен, рендерим в промежуточную текстуру, иначе сразу в backbuffer
    ID3D11RenderTargetView* pBaseRTV = monitor.pRenderTargetView;
    if (g_glowEffect.enabled && monitor.pBrightPassShader && monitor.pGlowRTV1 && monitor.pIntermediateRTV) {
        pBaseRTV = monitor.pIntermediateRTV;
    }
    
    // PASS 1: Основной рендеринг с фильтрами
    monitor.pContext->OMSetRenderTargets(1, &pBaseRTV, nullptr);
    monitor.pContext->ClearRenderTargetView(pBaseRTV, clearColor);
    monitor.pContext->PSSetShader(monitor.pPixelShader, nullptr, 0);
    monitor.pContext->PSSetConstantBuffers(0, 1, &monitor.pConstantBuffer);
    monitor.pContext->PSSetShaderResources(0, 1, &monitor.pTextureSRV);
    monitor.pContext->Draw(4, 0);
    
    // Если Glow включен, применяем многопроходный эффект
    if (g_glowEffect.enabled && monitor.pBrightPassShader && monitor.pGlowRTV1 && monitor.pIntermediateSRV) {
            // PASS 2: Bright Pass - выделяем яркие области
            D3D11_VIEWPORT glowViewport = {};
            glowViewport.Width = (float)(monitor.screenWidth / (int)g_glowEffect.downsample);
            glowViewport.Height = (float)(monitor.screenHeight / (int)g_glowEffect.downsample);
            glowViewport.MinDepth = 0.0f;
            glowViewport.MaxDepth = 1.0f;
            monitor.pContext->RSSetViewports(1, &glowViewport);
            
            monitor.pContext->OMSetRenderTargets(1, &monitor.pGlowRTV1, nullptr);
            monitor.pContext->ClearRenderTargetView(monitor.pGlowRTV1, clearColor);
            monitor.pContext->PSSetShader(monitor.pBrightPassShader, nullptr, 0);
            
            // Константы для Bright Pass
            struct GlowConstants {
                float threshold;
                float saturation;
                float padding[2];
            } glowConst = { g_glowEffect.threshold, g_glowEffect.saturation, {0, 0} };
            
            D3D11_MAPPED_SUBRESOURCE mapped;
            ID3D11Buffer* pTempCB = nullptr;
            D3D11_BUFFER_DESC cbDesc = {};
            cbDesc.ByteWidth = sizeof(GlowConstants);
            cbDesc.Usage = D3D11_USAGE_DYNAMIC;
            cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            
            if (SUCCEEDED(monitor.pDevice->CreateBuffer(&cbDesc, nullptr, &pTempCB))) {
                if (SUCCEEDED(monitor.pContext->Map(pTempCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                    memcpy(mapped.pData, &glowConst, sizeof(GlowConstants));
                    monitor.pContext->Unmap(pTempCB, 0);
                }
                monitor.pContext->PSSetConstantBuffers(0, 1, &pTempCB);
            }
            
            monitor.pContext->PSSetShaderResources(0, 1, &monitor.pIntermediateSRV);
            monitor.pContext->Draw(4, 0);
            
            // PASS 3-N: Blur passes (ping-pong между текстурами)
            struct BlurConstants {
                float texelSize[2];
                float radius;
                float padding;
            } blurConst;
            
            blurConst.texelSize[0] = 1.0f / glowViewport.Width;
            blurConst.texelSize[1] = 1.0f / glowViewport.Height;
            blurConst.radius = g_glowEffect.radius;
            blurConst.padding = 0;
            
            ID3D11Buffer* pBlurCB = nullptr;
            cbDesc.ByteWidth = sizeof(BlurConstants);
            if (SUCCEEDED(monitor.pDevice->CreateBuffer(&cbDesc, nullptr, &pBlurCB))) {
                if (SUCCEEDED(monitor.pContext->Map(pBlurCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                    memcpy(mapped.pData, &blurConst, sizeof(BlurConstants));
                    monitor.pContext->Unmap(pBlurCB, 0);
                }
                monitor.pContext->PSSetConstantBuffers(0, 1, &pBlurCB);
            }
            
            for (int pass = 0; pass < g_glowEffect.blurPasses; pass++) {
                // Horizontal blur
                monitor.pContext->OMSetRenderTargets(1, &monitor.pGlowRTV2, nullptr);
                monitor.pContext->PSSetShader(monitor.pBlurHShader, nullptr, 0);
                monitor.pContext->PSSetShaderResources(0, 1, &monitor.pGlowSRV1);
                monitor.pContext->Draw(4, 0);
                
                // Vertical blur
                monitor.pContext->OMSetRenderTargets(1, &monitor.pGlowRTV1, nullptr);
                monitor.pContext->PSSetShader(monitor.pBlurVShader, nullptr, 0);
                monitor.pContext->PSSetShaderResources(0, 1, &monitor.pGlowSRV2);
                monitor.pContext->Draw(4, 0);
            }
            
            // PASS FINAL: Composite - смешиваем base + glow
            monitor.pContext->RSSetViewports(1, &viewport);
            monitor.pContext->OMSetRenderTargets(1, &monitor.pRenderTargetView, nullptr);
            monitor.pContext->PSSetShader(monitor.pCompositeShader, nullptr, 0);
            
            struct CompositeConstants {
                float intensity;
                float padding[3];
            } compConst = { g_glowEffect.intensity, {0, 0, 0} };
            
            ID3D11Buffer* pCompCB = nullptr;
            cbDesc.ByteWidth = sizeof(CompositeConstants);
            if (SUCCEEDED(monitor.pDevice->CreateBuffer(&cbDesc, nullptr, &pCompCB))) {
                if (SUCCEEDED(monitor.pContext->Map(pCompCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                    memcpy(mapped.pData, &compConst, sizeof(CompositeConstants));
                    monitor.pContext->Unmap(pCompCB, 0);
                }
                monitor.pContext->PSSetConstantBuffers(0, 1, &pCompCB);
            }
            
            ID3D11ShaderResourceView* srvs[2] = { monitor.pIntermediateSRV, monitor.pGlowSRV1 };
            monitor.pContext->PSSetShaderResources(0, 2, srvs);
            monitor.pContext->Draw(4, 0);
            
            // Cleanup temporary resources
            if (pCompCB) pCompCB->Release();
            if (pBlurCB) pBlurCB->Release();
            if (pTempCB) pTempCB->Release();
            
            // Unbind SRVs
            ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
            monitor.pContext->PSSetShaderResources(0, 2, nullSRVs);
    }
    
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
        
        // Проверяем есть ли выбранные мониторы
        int selectedCount = 0;
        for (auto& monitor : g_monitors) {
            if (monitor.isSelected) selectedCount++;
        }
        
        if (selectedCount == 0) {
            MessageBoxW(NULL, L"No monitors selected. Please select at least one monitor.", L"Error", MB_OK);
            g_isRunning = false;
            return;
        }
        
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
        
        // Запускаем аудио захват если включен
        if (g_enableAudioCapture) {
            StartAudioCapture();
        }
        
        // Устанавливаем хук клавиатуры для скриншота
        if (!g_keyboardHook) {
            g_keyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandle(NULL), 0);
        }
        
        // Запускаем таймер для поддержания оверлея поверх всех окон
        g_topMostTimer = SetTimer(g_hWnd, 1001, 1000, NULL); // Каждую секунду
        
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
        
        // Останавливаем аудио захват
        StopAudioCapture();
        
        // Снимаем хук клавиатуры
        if (g_keyboardHook) {
            UnhookWindowsHookEx(g_keyboardHook);
            g_keyboardHook = NULL;
        }
        
        // Останавливаем таймер поддержания поверх всех окон
        if (g_topMostTimer) {
            KillTimer(g_hWnd, 1001);
            g_topMostTimer = 0;
        }
        
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
            
            // Кнопки Save, Load, Filters перенесены выше
            g_hSaveButton = CreateWindowW(L"BUTTON", L"Save", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                220, 145, 40, 25, hWnd, (HMENU)12, hInstance, NULL);
            
            g_hLoadButton = CreateWindowW(L"BUTTON", L"Load", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                265, 145, 40, 25, hWnd, (HMENU)13, hInstance, NULL);
            
            HWND hFiltersButton = CreateWindowW(L"BUTTON", L"Filters", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                10, 180, 70, 30, hWnd, (HMENU)17, hInstance, NULL);
            
            g_hScreenshotButton = CreateWindowW(L"BUTTON", L"Screenshot", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                85, 180, 90, 30, hWnd, (HMENU)16, hInstance, NULL);
            
            HWND hEffectsButton = CreateWindowW(L"BUTTON", L"Effects", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                180, 180, 70, 30, hWnd, (HMENU)19, hInstance, NULL);
            
            g_hLabelThreshold = CreateWindowW(L"STATIC", L"Threshold: 20%", WS_CHILD | WS_VISIBLE,
                10, 225, 150, 20, hWnd, NULL, hInstance, NULL);
            
            g_hTrackbarThreshold = CreateWindowW(TRACKBAR_CLASSW, NULL,
                WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_AUTOTICKS,
                10, 250, 300, 30, hWnd, (HMENU)2, hInstance, NULL);
            SendMessage(g_hTrackbarThreshold, TBM_SETRANGE, TRUE, MAKELONG(1, 100));
            SendMessage(g_hTrackbarThreshold, TBM_SETPOS, TRUE, 20);
            
            g_hLabelSmoothing = CreateWindowW(L"STATIC", L"Smoothing: 10%", WS_CHILD | WS_VISIBLE,
                10, 285, 150, 20, hWnd, NULL, hInstance, NULL);
            
            g_hTrackbarSmoothing = CreateWindowW(TRACKBAR_CLASSW, NULL,
                WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_AUTOTICKS,
                10, 310, 300, 30, hWnd, (HMENU)6, hInstance, NULL);
            SendMessage(g_hTrackbarSmoothing, TBM_SETRANGE, TRUE, MAKELONG(0, 200));
            SendMessage(g_hTrackbarSmoothing, TBM_SETPOS, TRUE, 10);
            
            g_hVSyncCheckbox = CreateWindowW(L"BUTTON", L"V-Sync Enabled",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                10, 350, 150, 20, hWnd, (HMENU)10, hInstance, NULL);
            SendMessage(g_hVSyncCheckbox, BM_SETCHECK, BST_CHECKED, 0);
            
            g_hSelectiveCheckbox = CreateWindowW(L"BUTTON", L"Enable Selective",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                170, 350, 140, 20, hWnd, (HMENU)18, hInstance, NULL);
            SendMessage(g_hSelectiveCheckbox, BM_SETCHECK, BST_CHECKED, 0);
            
            g_hAudioCheckbox = CreateWindowW(L"BUTTON", L"Capture Audio",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                10, 375, 120, 20, hWnd, (HMENU)14, hInstance, NULL);
            
            CreateWindowW(L"STATIC", L"Grayscale:", WS_CHILD | WS_VISIBLE,
                10, 405, 70, 20, hWnd, NULL, hInstance, NULL);
            
            g_hGrayscaleCombo = CreateWindowW(L"COMBOBOX", NULL,
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                85, 403, 225, 200, hWnd, (HMENU)11, hInstance, NULL);
            
            SendMessage(g_hGrayscaleCombo, CB_ADDSTRING, 0, (LPARAM)L"Average");
            SendMessage(g_hGrayscaleCombo, CB_ADDSTRING, 0, (LPARAM)L"Luminosity");
            SendMessage(g_hGrayscaleCombo, CB_ADDSTRING, 0, (LPARAM)L"HD (BT.709)");
            SendMessage(g_hGrayscaleCombo, CB_ADDSTRING, 0, (LPARAM)L"Desaturation");
            SendMessage(g_hGrayscaleCombo, CB_ADDSTRING, 0, (LPARAM)L"Max Channel");
            SendMessage(g_hGrayscaleCombo, CB_ADDSTRING, 0, (LPARAM)L"Green Only");
            SendMessage(g_hGrayscaleCombo, CB_SETCURSEL, 1, 0); // Luminosity по умолчанию
            
            // Создаем чекбоксы и FPS лейблы для мониторов
            PopulateMonitorList();
            
            int yPos = 435;
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
            
            // Инициализируем папку скриншотов если пуста
            if (wcslen(g_screenshotFolder) == 0) {
                SHGetFolderPath(NULL, CSIDL_MYPICTURES, NULL, SHGFP_TYPE_CURRENT, g_screenshotFolder);
            }
            
            // Обновляем интерфейс после загрузки
            SendMessage(g_hTrackbarThreshold, TBM_SETPOS, TRUE, g_thresholdPercent);
            SendMessage(g_hTrackbarSmoothing, TBM_SETPOS, TRUE, g_smoothingPercent);
            SendMessage(g_hVSyncCheckbox, BM_SETCHECK, g_vsyncEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
            SendMessage(g_hSelectiveCheckbox, BM_SETCHECK, g_enableSelective ? BST_CHECKED : BST_UNCHECKED, 0);
            SendMessage(g_hGrayscaleCombo, CB_SETCURSEL, g_grayscaleMode, 0);
            SendMessage(g_hAudioCheckbox, BM_SETCHECK, g_enableAudioCapture ? BST_CHECKED : BST_UNCHECKED, 0);
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
            if (wParam == 1001 && g_isRunning) {
                // Поддерживаем оверлеи поверх всех окон
                for (auto& monitor : g_monitors) {
                    if (monitor.isActive && monitor.hOverlayWnd) {
                        SetWindowPos(monitor.hOverlayWnd, HWND_TOPMOST, 0, 0, 0, 0, 
                            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                    }
                }
            }
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
            else if (LOWORD(wParam) == 14) {
                g_enableAudioCapture = (SendMessage(g_hAudioCheckbox, BM_GETCHECK, 0, 0) == BST_CHECKED);
                if (g_isRunning) {
                    if (g_enableAudioCapture) {
                        StartAudioCapture();
                    } else {
                        StopAudioCapture();
                    }
                }
            }
            else if (LOWORD(wParam) == 16) {
                ShowScreenshotSettings();
            }
            else if (LOWORD(wParam) == 17) {
                ShowColorFiltersDialog();
            }
            else if (LOWORD(wParam) == 18) {
                g_enableSelective = (SendMessage(g_hSelectiveCheckbox, BM_GETCHECK, 0, 0) == BST_CHECKED);
                // Помечаем все активные мониторы для обновления констант
                for (auto& monitor : g_monitors) {
                    if (monitor.isActive) {
                        monitor.constantsNeedUpdate = true;
                    }
                }
            }
            else if (LOWORD(wParam) == 19) {
                ShowEffectsDialog();
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