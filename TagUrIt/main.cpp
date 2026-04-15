#include <iostream>
#include <Windows.h>
#include <TlHelp32.h>
#include <vector>
#include <string>
#include <thread>
#include <chrono>
#include <d2d1.h>
#include <dwrite.h>
#include <fstream>
#include <cmath>

// Include DMALibrary
#include "../DMALibrary/DMALibrary/Memory/Memory.h"

// Global memory instance
Memory mem;

// R6S process name
const std::string PROCESS_NAME = "RainbowSix.exe";

// Offsets (placeholders, update with real RE)
uintptr_t base_address = 0;
uintptr_t entity_list_offset = 0x12345678; // Placeholder - pointer to entity list
uintptr_t view_matrix_offset = 0x87654321; // Placeholder - view matrix
uintptr_t local_player_offset = 0xABCDEF00; // Placeholder - local player

bool espRunning = false;

// Structs
struct Vector3 {
    float x, y, z;
    Vector3 operator-(const Vector3& other) const {
        return {x - other.x, y - other.y, z - other.z};
    }
    float length() const {
        return sqrt(x*x + y*y + z*z);
    }
};

struct ViewMatrix {
    float matrix[16];
};

struct Entity {
    uintptr_t address;
    Vector3 position;
    int health;
    int team;
    bool isAlive;
    // Add more fields as needed
};

std::vector<Entity> entities;
Entity localPlayer;

// World to Screen function
bool WorldToScreen(const Vector3& worldPos, Vector3& screenPos, const ViewMatrix& vm, int screenWidth, int screenHeight) {
    float w = vm.matrix[3] * worldPos.x + vm.matrix[7] * worldPos.y + vm.matrix[11] * worldPos.z + vm.matrix[15];
    if (w < 0.01f) return false;

    screenPos.x = (vm.matrix[0] * worldPos.x + vm.matrix[4] * worldPos.y + vm.matrix[8] * worldPos.z + vm.matrix[12]) / w;
    screenPos.y = (vm.matrix[1] * worldPos.x + vm.matrix[5] * worldPos.y + vm.matrix[9] * worldPos.z + vm.matrix[13]) / w;
    screenPos.z = 0;

    screenPos.x = (screenWidth / 2.0f) * (1.0f + screenPos.x);
    screenPos.y = (screenHeight / 2.0f) * (1.0f - screenPos.y);

    return true;
}

// Direct2D globals
ID2D1Factory* pD2DFactory = nullptr;
ID2D1HwndRenderTarget* pRenderTarget = nullptr;
ID2D1SolidColorBrush* pBrush = nullptr;
HWND hWnd;
HWND hGuiWnd;

// Window proc
LRESULT CALLBACK WindowProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_COMMAND:
        if (LOWORD(wParam) == 1) {
            espRunning = true;
        } else if (LOWORD(wParam) == 2) {
            espRunning = false;
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, message, wParam, lParam);
}

// Function to detect if R6S is running
bool IsProcessRunning(const std::string& processName) {
    PROCESSENTRY32 entry;
    entry.dwSize = sizeof(PROCESSENTRY32);

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, NULL);
    if (Process32First(snapshot, &entry)) {
        do {
            if (processName == entry.szExeFile) {
                CloseHandle(snapshot);
                return true;
            }
        } while (Process32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return false;
}

// Function to find base address
uintptr_t FindBaseAddress() {
    return mem.GetBaseDaddy(PROCESS_NAME);
}

// Automatic offset finder
void AutoFindOffsets() {
    std::cout << "Starting automatic offset finding..." << std::endl;

    // Find view matrix: Look for 4x4 matrix with [0,0,0,1] in last row
    uintptr_t vmAddr = 0;
    for (uintptr_t addr = base_address; addr < base_address + 0x2000000; addr += 4) { // Scan 32MB
        float matrix[16];
        if (mem.Read(addr, &matrix, sizeof(matrix))) {
            if (fabs(matrix[12]) < 0.1f && fabs(matrix[13]) < 0.1f && fabs(matrix[14]) < 0.1f && fabs(matrix[15] - 1.0f) < 0.1f) {
                // Check if other values look like a view matrix (perspective)
                if (matrix[11] < -0.1f && matrix[11] > -10.0f) { // Near plane
                    vmAddr = addr;
                    break;
                }
            }
        }
    }
    if (vmAddr) {
        view_matrix_offset = vmAddr - base_address;
        std::ofstream file("offsets.txt", std::ios::app);
        file << "view_matrix_auto: 0x" << std::hex << view_matrix_offset << std::endl;
        file.close();
        std::cout << "Found view matrix at offset 0x" << std::hex << view_matrix_offset << std::endl;
    }

    // Find entity list: Look for pointer to array of pointers, where each points to struct with position floats
    uintptr_t entListAddr = 0;
    for (uintptr_t addr = base_address; addr < base_address + 0x2000000; addr += 8) {
        uintptr_t listPtr = mem.ReadUInt64(addr);
        if (listPtr && listPtr > 0x10000000000 && listPtr < 0xFFFFFFFFFFFFFFFF) { // Valid pointer range
            bool validList = true;
            for (int i = 0; i < 10; ++i) { // Check first 10 entities
                uintptr_t entPtr = mem.ReadUInt64(listPtr + i * 8);
                if (!entPtr) continue;
                float x = mem.ReadFloat(entPtr + 0x100); // Assume position offset
                float y = mem.ReadFloat(entPtr + 0x104);
                float z = mem.ReadFloat(entPtr + 0x108);
                if (x < -10000 || x > 10000 || y < -10000 || y > 10000 || z < -10000 || z > 10000) {
                    validList = false;
                    break;
                }
            }
            if (validList) {
                entListAddr = addr;
                break;
            }
        }
    }
    if (entListAddr) {
        entity_list_offset = entListAddr - base_address;
        std::ofstream file("offsets.txt", std::ios::app);
        file << "entity_list_auto: 0x" << std::hex << entity_list_offset << std::endl;
        file.close();
        std::cout << "Found entity list at offset 0x" << std::hex << entity_list_offset << std::endl;
    }

    // Find local player: Look for a pointer to a struct with position, assume it's local
    uintptr_t localAddr = 0;
    for (uintptr_t addr = base_address; addr < base_address + 0x2000000; addr += 8) {
        uintptr_t ptr = mem.ReadUInt64(addr);
        if (ptr && ptr > 0x10000000000 && ptr < 0xFFFFFFFFFFFFFFFF) {
            float x = mem.ReadFloat(ptr + 0x100);
            float y = mem.ReadFloat(ptr + 0x104);
            float z = mem.ReadFloat(ptr + 0x108);
            if (x > -1000 && x < 1000 && y > -1000 && y < 1000 && z > -1000 && z < 1000 && z > 0) { // Reasonable position
                localAddr = addr;
                break;
            }
        }
    }
    if (localAddr) {
        local_player_offset = localAddr - base_address;
        std::ofstream file("offsets.txt", std::ios::app);
        file << "local_player_auto: 0x" << std::hex << local_player_offset << std::endl;
        file.close();
        std::cout << "Found local player at offset 0x" << std::hex << local_player_offset << std::endl;
    }

    std::cout << "Automatic offset finding complete." << std::endl;
}

// Read entity data
Entity ReadEntity(uintptr_t entityAddr) {
    Entity ent;
    ent.address = entityAddr;
    // Read position (placeholder offsets)
    ent.position.x = mem.ReadFloat(entityAddr + 0x100); // Placeholder
    ent.position.y = mem.ReadFloat(entityAddr + 0x104);
    ent.position.z = mem.ReadFloat(entityAddr + 0x108);
    ent.health = mem.ReadInt(entityAddr + 0x200); // Placeholder
    ent.team = mem.ReadInt(entityAddr + 0x300); // Placeholder
    ent.isAlive = ent.health > 0;
    return ent;
}

// Scan for known entities (opponent team only)
void ScanEntities() {
    entities.clear();
    if (!entity_list_offset) return;

    uintptr_t entityListPtr = mem.ReadUInt64(base_address + entity_list_offset);
    if (!entityListPtr) return;

    // Assume entity list is an array of pointers
    int maxEntities = 64; // Placeholder
    for (int i = 0; i < maxEntities; ++i) {
        uintptr_t entityAddr = mem.ReadUInt64(entityListPtr + i * 8);
        if (entityAddr) {
            Entity ent = ReadEntity(entityAddr);
            if (ent.isAlive && ent.team != localPlayer.team) { // Only opponent team
                entities.push_back(ent);
            }
        }
    }

    // Read local player
    uintptr_t localAddr = mem.ReadUInt64(base_address + local_player_offset);
    if (localAddr) {
        localPlayer = ReadEntity(localAddr);
    }

    // Save entities automatically
    if (!entities.empty()) {
        std::ofstream file("entities.txt");
        for (const auto& ent : entities) {
            file << "Opponent Entity at 0x" << std::hex << ent.address << ": Pos(" << ent.position.x << "," << ent.position.y << "," << ent.position.z << ") Health:" << ent.health << " Team:" << ent.team << std::endl;
        }
        file.close();
    }
}

// Read-only ESP function
void RunESP() {
    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);

    while (true) {
        if (!espRunning) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        if (!IsProcessRunning(PROCESS_NAME)) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        // Scan entities periodically
        static auto lastScan = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - lastScan).count() > 1) {
            ScanEntities();
            lastScan = now;
        }

        // Read view matrix
        ViewMatrix vm;
        if (view_matrix_offset) {
            uintptr_t vmPtr = mem.ReadUInt64(base_address + view_matrix_offset);
            if (vmPtr) {
                mem.Read(vmPtr, &vm, sizeof(ViewMatrix));
            }
        }

        // Drawing
        pRenderTarget->BeginDraw();
        pRenderTarget->Clear(D2D1::ColorF(0, 0, 0, 0)); // Transparent

        for (const auto& ent : entities) {
            if (ent.team == localPlayer.team) continue; // Skip teammates

            Vector3 screenPos;
            if (WorldToScreen(ent.position, screenPos, vm, screenWidth, screenHeight)) {
                // Draw box
                float boxSize = 50.0f; // Placeholder
                D2D1_RECT_F rect = D2D1::RectF(screenPos.x - boxSize/2, screenPos.y - boxSize, screenPos.x + boxSize/2, screenPos.y);
                pRenderTarget->DrawRectangle(rect, pBrush);

                // Health bar
                float healthRatio = (float)ent.health / 100.0f; // Assume max 100
                D2D1_RECT_F healthRect = D2D1::RectF(screenPos.x - boxSize/2 - 10, screenPos.y - boxSize, screenPos.x - boxSize/2 - 5, screenPos.y);
                pRenderTarget->FillRectangle(healthRect, pBrush); // Background
                healthRect.bottom = healthRect.top + (healthRect.bottom - healthRect.top) * healthRatio;
                // Green brush for health
                ID2D1SolidColorBrush* greenBrush;
                pRenderTarget->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Green), &greenBrush);
                pRenderTarget->FillRectangle(healthRect, greenBrush);
                greenBrush->Release();

                // Distance
                float distance = (ent.position - localPlayer.position).length();
                // Note: Would need text rendering for distance, placeholder
            }
        }

        HRESULT hr = pRenderTarget->EndDraw();
        if (FAILED(hr)) {
            // Handle error
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60fps
    }
}

int main() {
    // Check if R6S is running
    if (!IsProcessRunning(PROCESS_NAME)) {
        return 1; // Silent exit
    }

    // Initialize DMA with retry
    if (!mem.Init(PROCESS_NAME, true, false)) {
        Sleep(2000); // Wait for DMA card
        if (!mem.Init(PROCESS_NAME, true, false)) {
            return 1;
        }
    }

    base_address = FindBaseAddress();
    if (!base_address) {
        return 1;
    }

    // Automatically find offsets
    AutoFindOffsets();

    // Create overlay window (transparent, topmost)
    HINSTANCE hInstance = GetModuleHandle(NULL);
    WNDCLASSEX wc = { sizeof(WNDCLASSEX), CS_HREDRAW | CS_VREDRAW, WindowProc, 0, 0, hInstance, NULL, NULL, NULL, NULL, L"TagUrItOverlay", NULL };
    if (!RegisterClassEx(&wc)) return 1;
    hWnd = CreateWindowEx(WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED, wc.lpszClassName, L"TagUrIt", WS_POPUP, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), NULL, NULL, hInstance, NULL);
    if (!hWnd) return 1;
    SetLayeredWindowAttributes(hWnd, RGB(0, 0, 0), 0, LWA_COLORKEY);
    ShowWindow(hWnd, SW_SHOW);

    // Create GUI window
    WNDCLASSEX wcGui = { sizeof(WNDCLASSEX), CS_HREDRAW | CS_VREDRAW, WindowProc, 0, 0, hInstance, NULL, NULL, NULL, NULL, L"TagUrItGUI", NULL };
    if (!RegisterClassEx(&wcGui)) return 1;
    hGuiWnd = CreateWindowEx(0, wcGui.lpszClassName, L"TagUrIt Control", WS_OVERLAPPEDWINDOW, 100, 100, 400, 300, NULL, NULL, hInstance, NULL);
    if (!hGuiWnd) return 1;
    CreateWindow(L"BUTTON", L"Start ESP", WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON, 10, 10, 100, 30, hGuiWnd, (HMENU)1, hInstance, NULL);
    CreateWindow(L"BUTTON", L"Stop ESP", WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON, 120, 10, 100, 30, hGuiWnd, (HMENU)2, hInstance, NULL);
    ShowWindow(hGuiWnd, SW_SHOW);

    // Init D2D
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &pD2DFactory);
    if (FAILED(hr)) return 1;
    hr = pD2DFactory->CreateHwndRenderTarget(D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED)), D2D1::HwndRenderTargetProperties(hWnd, D2D1::SizeU(GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN))), &pRenderTarget);
    if (FAILED(hr)) return 1;
    hr = pRenderTarget->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Red), &pBrush);
    if (FAILED(hr)) return 1;

    // Start ESP thread
    std::thread esp_thread(RunESP);

    // Message loop
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // Cleanup
    esp_thread.join();
    if (pBrush) pBrush->Release();
    if (pRenderTarget) pRenderTarget->Release();
    if (pD2DFactory) pD2DFactory->Release();

    return 0;
}

    esp_thread.join();

    // Cleanup
    pBrush->Release();
    pRenderTarget->Release();
    pD2DFactory->Release();

    return 0;
}