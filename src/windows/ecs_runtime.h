#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#ifdef KFC_RUNTIME_BUILD
#define KFC_EXPORT __declspec(dllexport)
#else
#define KFC_EXPORT
#endif

namespace EcsRuntime {
bool Initialize();
void Tick();
std::string Status();
void Shutdown();
}

extern "C" {
KFC_EXPORT bool __cdecl ShroudforgeEcsConfigure(
    const char* const* qualified_names, const std::uint32_t* sizes, std::size_t count);
KFC_EXPORT bool __cdecl ShroudforgeEcsReady();
KFC_EXPORT bool __cdecl ShroudforgeEcsCanWrite();
KFC_EXPORT bool __cdecl ShroudforgeEcsDescribe(
    const char* qualified_name, std::uint32_t* size);
KFC_EXPORT std::size_t __cdecl ShroudforgeEcsQuery(
    const char* const* qualified_names, std::size_t component_count,
    std::uint32_t* entities, std::size_t capacity);
KFC_EXPORT std::uint32_t __cdecl ShroudforgeEcsResolve(std::uint32_t entity_id);
KFC_EXPORT bool __cdecl ShroudforgeEcsRead(
    std::uint32_t entity, const char* qualified_name, void* value, std::size_t size);
KFC_EXPORT bool __cdecl ShroudforgeEcsWrite(
    std::uint32_t entity, const char* qualified_name, const void* expected,
    const void* value, std::size_t size);
}
