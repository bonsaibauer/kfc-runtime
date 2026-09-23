#include "ecs_runtime.h"
#include <algorithm>
#include <cstring>
extern "C" {
KFC_EXPORT unsigned __cdecl KfcRuntimeAbi() { return 1; }
KFC_EXPORT bool __cdecl KfcRuntimeInitialize() {
    try { return EcsRuntime::Initialize(); } catch (...) { EcsRuntime::Shutdown(); return false; }
}
KFC_EXPORT void __cdecl KfcRuntimeTick() {
    try { EcsRuntime::Tick(); } catch (...) { EcsRuntime::Shutdown(); }
}
KFC_EXPORT void __cdecl KfcRuntimeShutdown() { EcsRuntime::Shutdown(); }
KFC_EXPORT void __cdecl KfcRuntimeStatus(char* buffer, std::size_t capacity) {
    if (!buffer || !capacity) return;
    try {
        const auto text = EcsRuntime::Status();
        const auto size = (std::min)(text.size(), capacity - 1);
        std::memcpy(buffer, text.data(), size); buffer[size] = 0;
    } catch (...) { buffer[0] = 0; }
}
}
