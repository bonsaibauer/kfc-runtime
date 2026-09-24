#include "ecs_runtime.h"
#include "game_thread_dispatcher.h"
#include "../../third_party/nlohmann/json.hpp"
#include "profile.h"

#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
constexpr std::size_t max_components = 1024;

struct ComponentType { std::uint16_t index; std::uint32_t size; };
struct ResolvedLayout {
    std::uintptr_t count_address{};
    std::uintptr_t table_address{};
    std::size_t entity_layout{};
    std::size_t entity_storage{};
    std::size_t entity_row{};
    std::size_t entity_id{};
    std::size_t entity_generation{};
    std::size_t component_bits{};
    std::size_t component_offsets{};
    std::size_t component_strides{};
};
struct EntityView {
    std::uintptr_t pointer{};
    std::uintptr_t layout{};
    std::uintptr_t storage{};
    std::uint32_t row{};
    std::uint32_t id{};
    std::uint32_t generation{};
};
struct HandleRecord {
    std::uint32_t id{};
    std::uint32_t generation{};
    std::uint64_t epoch{};
    std::uintptr_t pointer{};
};

std::mutex state_mutex;
std::unordered_map<std::string, ComponentType> types;
std::unordered_map<std::string, std::uint32_t> configured_types;
ResolvedLayout live_layout{};
bool layout_ready{};
std::uintptr_t live_table{};
std::unordered_map<std::uint32_t, HandleRecord> handles;
std::unordered_map<std::uint64_t, std::uint32_t> reverse_handles;
struct QueryScanState {
    std::uint64_t epoch{};
    std::uint64_t touched_ms{};
    std::vector<std::uintptr_t> pointers;
    std::size_t cursor{};
    std::vector<std::uint32_t> matches;
};
struct QueryCacheEntry {
    std::uint64_t epoch{};
    std::uint64_t completed_ms{};
    std::vector<std::uint32_t> matches;
};
std::unordered_map<std::string, QueryScanState> query_scans;
std::unordered_map<std::string, QueryCacheEntry> query_cache;
struct ResolveScanState {
    std::uint64_t epoch{};
    std::uint64_t touched_ms{};
    std::vector<std::uintptr_t> pointers;
    std::size_t cursor{};
};
struct ResolveCacheEntry {
    std::uint64_t epoch{};
    std::uint64_t completed_ms{};
    std::uint32_t handle{};
};
std::unordered_map<std::uint32_t, ResolveScanState> resolve_scans;
std::unordered_map<std::uint32_t, ResolveCacheEntry> resolve_cache;
std::uint32_t next_handle{1};
std::uint64_t layout_epoch{1};
std::mutex write_mutex;
std::atomic<bool> stop_requested{};
std::uintptr_t image_base{};

bool readable(std::uintptr_t address, std::size_t size) {
    if (!address || !size || address > UINTPTR_MAX - size) return false;
    MEMORY_BASIC_INFORMATION memory{};
    if (!VirtualQuery(reinterpret_cast<const void*>(address), &memory, sizeof(memory))) return false;
    const auto end = reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize;
    return memory.State == MEM_COMMIT && !(memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
        address + size <= end;
}

bool read_bytes(std::uintptr_t address, void* value, std::size_t size) {
    SIZE_T received{};
    return readable(address, size) && ReadProcessMemory(GetCurrentProcess(),
        reinterpret_cast<const void*>(address), value, size, &received) && received == size;
}
template<class T> bool read(std::uintptr_t address, T& value) {
    return read_bytes(address, &value, sizeof(value));
}


bool layout_snapshot(ResolvedLayout& result) {
    std::scoped_lock lock(state_mutex);
    if (!layout_ready) return false;
    result = live_layout;
    return true;
}
bool entity_pointers(const ResolvedLayout& layout, std::vector<std::uintptr_t>& pointers) {
    std::uint64_t count{};
    std::uintptr_t table{};
    if (!read(layout.count_address, count) || !count || count > (1u << 20) ||
        !read(layout.table_address, table) || !table) {
        std::scoped_lock lock(state_mutex);
        layout_ready = false;
        ++layout_epoch;
        handles.clear();
        reverse_handles.clear();
        return false;
    }
    pointers.resize(static_cast<std::size_t>(count));
    return read_bytes(table, pointers.data(), pointers.size() * sizeof(pointers[0]));
}
bool entity_view(std::uintptr_t pointer, const ResolvedLayout& layout, EntityView& entity) {
    entity.pointer = pointer;
    return pointer && read(pointer + layout.entity_layout, entity.layout) && entity.layout &&
        read(pointer + layout.entity_storage, entity.storage) && entity.storage &&
        read(pointer + layout.entity_row, entity.row) && entity.row <= (1u << 24) &&
        read(pointer + layout.entity_id, entity.id) && entity.id &&
        read(pointer + layout.entity_generation, entity.generation);
}
bool component_address(const EntityView& entity, const ResolvedLayout& layout,
                       const ComponentType& component, std::uintptr_t& address) {
    std::uint64_t bits{};
    std::uint16_t offset{}, stride{};
    if (component.index >= max_components ||
        !read(entity.layout + layout.component_bits + (component.index / 64) * 8, bits) ||
        !(bits & (std::uint64_t{1} << (component.index % 64))) ||
        !read(entity.layout + ShroudforgeCompatibility::EnshroudedClient::component_offsets + component.index * 2, offset) ||
        !read(entity.layout + ShroudforgeCompatibility::EnshroudedClient::component_strides + component.index * 2, stride) || stride != component.size) return false;
    address = entity.storage + offset + static_cast<std::uintptr_t>(entity.row) * stride;
    return readable(address, component.size);
}
bool resolve_component(const char* name, ComponentType& component) {
    if (!name) return false;
    std::scoped_lock lock(state_mutex);
    const auto found = types.find(name);
    if (found == types.end()) return false;
    component = found->second;
    return true;
}
std::uint64_t identity_key(std::uint32_t id, std::uint32_t generation) {
    return static_cast<std::uint64_t>(generation) << 32 | id;
}
std::uint32_t handle_for(const EntityView& entity) {
    std::scoped_lock lock(state_mutex);
    const auto key = identity_key(entity.id, entity.generation);
    if (const auto found = reverse_handles.find(key); found != reverse_handles.end()) {
        handles.at(found->second).pointer = entity.pointer;
        return found->second;
    }
    // Never recycle an opaque handle during this process, including world changes.
    if (!next_handle) return 0;
    const auto handle = next_handle++;
    handles.emplace(handle, HandleRecord{entity.id, entity.generation, layout_epoch, entity.pointer});
    reverse_handles.emplace(key, handle);
    return handle;
}
bool entity_for_handle(std::uint32_t handle, const ResolvedLayout& layout, EntityView& entity) {
    HandleRecord record{};
    {
        std::scoped_lock lock(state_mutex);
        const auto found = handles.find(handle);
        if (found == handles.end() || found->second.epoch != layout_epoch) return false;
        record = found->second;
    }
    // Re-read the live identity and layout; never cache component addresses.
    // A moved/deleted entity is rediscovered by the next query, not a full-world
    // scan for each individual component read and write.
    return entity_view(record.pointer, layout, entity) && entity.id == record.id &&
        entity.generation == record.generation;
}

struct QueryOperation {
    const char* const* names{};
    std::size_t count{};
    std::uint32_t* entities{};
    std::size_t capacity{};
    std::size_t result{SIZE_MAX};
    std::vector<std::string> owned_names;
    std::vector<const char*> name_pointers;
    std::vector<std::uint32_t> output;
};
std::string query_key(const QueryOperation& operation) {
    std::string key;
    for (const auto& name : operation.owned_names) {
        key.append(std::to_string(name.size()));
        key.push_back(':');
        key.append(name);
        key.push_back(';');
    }
    return key;
}
std::uint64_t current_epoch() {
    std::scoped_lock lock(state_mutex);
    return layout_epoch;
}
void publish_query_result(QueryOperation& operation, const std::vector<std::uint32_t>& matches) {
    operation.result = matches.size();
    if (operation.entities && operation.capacity)
        std::copy_n(matches.begin(), (std::min)(operation.capacity, matches.size()), operation.entities);
}
void query_on_game_thread(void* opaque) {
    auto& operation = *static_cast<QueryOperation*>(opaque);
    std::vector<ComponentType> components(operation.count);
    for (std::size_t index = 0; index < operation.count; ++index)
        if (!resolve_component(operation.names[index], components[index])) return;
    ResolvedLayout layout{};
    if (!layout_snapshot(layout)) return;
    const auto epoch = current_epoch();
    const auto now_ms = GetTickCount64();
    const auto key = query_key(operation);
    for (auto iterator = query_scans.begin(); iterator != query_scans.end();) {
        if (now_ms - iterator->second.touched_ms > 2000) iterator = query_scans.erase(iterator);
        else ++iterator;
    }
    for (auto iterator = query_cache.begin(); iterator != query_cache.end();) {
        if (now_ms - iterator->second.completed_ms > 2000) iterator = query_cache.erase(iterator);
        else ++iterator;
    }
    if (auto cached = query_cache.find(key); cached != query_cache.end()) {
        if (cached->second.epoch == epoch && now_ms - cached->second.completed_ms <= 50) {
            publish_query_result(operation, cached->second.matches);
            return;
        }
        query_cache.erase(cached);
    }
    auto scan = query_scans.find(key);
    if (scan == query_scans.end() || scan->second.epoch != epoch) {
        QueryScanState fresh{};
        fresh.epoch = epoch;
        fresh.touched_ms = now_ms;
        if (!entity_pointers(layout, fresh.pointers)) return;
        scan = query_scans.insert_or_assign(key, std::move(fresh)).first;
    }
    auto& progress = scan->second;
    progress.touched_ms = now_ms;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1);
    std::size_t visited = 0;
    for (; progress.cursor < progress.pointers.size() && visited < 256; ++progress.cursor, ++visited) {
        const auto pointer = progress.pointers[progress.cursor];
        EntityView entity{};
        if (entity_view(pointer, layout, entity)) {
            bool include = true;
            for (const auto& component : components) {
                std::uintptr_t address{};
                if (!component_address(entity, layout, component, address)) { include = false; break; }
            }
            if (include) progress.matches.push_back(handle_for(entity));
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            ++progress.cursor;
            break;
        }
    }
    if (progress.cursor < progress.pointers.size()) {
        // The caller skips this update and retries on its next tick. This
        // distinct sentinel keeps an incomplete scan from looking like an
        // authoritative empty query result.
        operation.result = SIZE_MAX - 1;
        return;
    }
    auto completed = std::move(progress.matches);
    query_scans.erase(scan);
    auto [cached, _] = query_cache.insert_or_assign(key, QueryCacheEntry{epoch, now_ms, std::move(completed)});
    publish_query_result(operation, cached->second.matches);
}

struct ResolveOperation { std::uint32_t entity_id{}, result{}; };
void resolve_on_game_thread(void* opaque) {
    auto& operation = *static_cast<ResolveOperation*>(opaque);
    ResolvedLayout layout{};
    if (!layout_snapshot(layout)) return;
    const auto epoch = current_epoch();
    const auto now_ms = GetTickCount64();
    for (auto iterator = resolve_scans.begin(); iterator != resolve_scans.end();) {
        if (now_ms - iterator->second.touched_ms > 2000) iterator = resolve_scans.erase(iterator);
        else ++iterator;
    }
    for (auto iterator = resolve_cache.begin(); iterator != resolve_cache.end();) {
        if (now_ms - iterator->second.completed_ms > 2000) iterator = resolve_cache.erase(iterator);
        else ++iterator;
    }
    if (auto cached = resolve_cache.find(operation.entity_id); cached != resolve_cache.end()) {
        if (cached->second.epoch == epoch && now_ms - cached->second.completed_ms <= 50) {
            operation.result = cached->second.handle;
            return;
        }
        resolve_cache.erase(cached);
    }
    auto scan = resolve_scans.find(operation.entity_id);
    if (scan == resolve_scans.end() || scan->second.epoch != epoch) {
        ResolveScanState fresh{};
        fresh.epoch = epoch;
        fresh.touched_ms = now_ms;
        if (!entity_pointers(layout, fresh.pointers)) return;
        scan = resolve_scans.insert_or_assign(operation.entity_id, std::move(fresh)).first;
    }
    auto& progress = scan->second;
    progress.touched_ms = now_ms;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1);
    std::size_t visited = 0;
    for (; progress.cursor < progress.pointers.size() && visited < 256; ++progress.cursor, ++visited) {
        EntityView candidate{};
        if (entity_view(progress.pointers[progress.cursor], layout, candidate) &&
            candidate.id == operation.entity_id) {
            operation.result = handle_for(candidate);
            resolve_cache.insert_or_assign(operation.entity_id,
                ResolveCacheEntry{epoch, now_ms, operation.result});
            resolve_scans.erase(scan);
            return;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            ++progress.cursor;
            break;
        }
    }
    if (progress.cursor >= progress.pointers.size()) {
        resolve_cache.insert_or_assign(operation.entity_id, ResolveCacheEntry{epoch, now_ms, 0});
        resolve_scans.erase(scan);
    }
}

struct ReadOperation {
    std::uint32_t handle{};
    const char* name{};
    void* value{};
    std::size_t size{};
    bool result{};
    std::string owned_name;
    std::vector<std::uint8_t> output;
};
void read_on_game_thread(void* opaque) {
    auto& operation = *static_cast<ReadOperation*>(opaque);
    ComponentType component{};
    ResolvedLayout layout{};
    EntityView entity{};
    std::uintptr_t address{};
    operation.result = operation.value && resolve_component(operation.name, component) &&
        component.size == operation.size && layout_snapshot(layout) &&
        entity_for_handle(operation.handle, layout, entity) &&
        component_address(entity, layout, component, address) &&
        read_bytes(address, operation.value, operation.size);
}

struct WriteOperation {
    std::uint32_t handle{};
    const char* name{};
    const void* expected{};
    const void* value{};
    std::size_t size{};
    bool result{};
    std::string owned_name;
    std::vector<std::uint8_t> source, destination;
};
void write_on_game_thread(void* opaque) {
    if (stop_requested.load(std::memory_order_acquire)) return;
    auto& operation = *static_cast<WriteOperation*>(opaque);
    ComponentType component{};
    ResolvedLayout layout{};
    EntityView entity{};
    std::uintptr_t address{};
    if (!operation.expected || !operation.value || !resolve_component(operation.name, component) ||
        component.size != operation.size || !layout_snapshot(layout) ||
        !entity_for_handle(operation.handle, layout, entity) ||
        !component_address(entity, layout, component, address)) return;
    std::scoped_lock transaction(write_mutex);
    if (stop_requested.load(std::memory_order_acquire)) return;
    std::vector<std::uint8_t> before(operation.size), verified(operation.size);
    if (!read_bytes(address, before.data(), operation.size) ||
        std::memcmp(before.data(), operation.expected, operation.size)) return;
    SIZE_T written{};
    if (!WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), operation.value,
            operation.size, &written) || written != operation.size ||
        !read_bytes(address, verified.data(), operation.size) ||
        std::memcmp(verified.data(), operation.value, operation.size)) {
        SIZE_T restored{};
        WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), before.data(),
            operation.size, &restored);
        return;
    }
    operation.result = true;
}
}

namespace EcsRuntime {
bool Initialize() {
    if (!ShroudforgeCompatibility::EnshroudedClient::Load()) return false;
    image_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (!image_base) return false;
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image_base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image_base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    stop_requested.store(false, std::memory_order_release);
    return GameThreadDispatcher::Initialize();
}
void Tick() {
    const auto manager = GameThreadDispatcher::EntityManager();
    if (!manager) return;
    std::scoped_lock lock(state_mutex);
    const auto count_address = manager + ShroudforgeCompatibility::EnshroudedClient::entity_manager_count;
    const auto table_address = manager + ShroudforgeCompatibility::EnshroudedClient::entity_manager_table;
    std::uint64_t count{};
    std::uintptr_t table{};
    if (!read(count_address, count) || !count || count > (1u << 20) ||
        !read(table_address, table) || !readable(table, sizeof(std::uintptr_t))) {
        if (layout_ready) {
            layout_ready = false;
            ++layout_epoch;
            handles.clear();
            reverse_handles.clear();
        }
        return;
    }
    if (layout_ready && live_table == table && live_layout.count_address == count_address &&
        live_layout.table_address == table_address) return;
    live_layout = {};
    live_table = table;
    live_layout.count_address = count_address;
    live_layout.table_address = table_address;
    live_layout.entity_id = ShroudforgeCompatibility::EnshroudedClient::entity_id;
    live_layout.entity_generation = ShroudforgeCompatibility::EnshroudedClient::entity_generation;
    live_layout.entity_layout = ShroudforgeCompatibility::EnshroudedClient::entity_layout;
    live_layout.entity_storage = ShroudforgeCompatibility::EnshroudedClient::entity_storage;
    live_layout.entity_row = ShroudforgeCompatibility::EnshroudedClient::entity_row;
    live_layout.component_bits = ShroudforgeCompatibility::EnshroudedClient::component_bits;
    live_layout.component_offsets = ShroudforgeCompatibility::EnshroudedClient::component_offsets;
    live_layout.component_strides = ShroudforgeCompatibility::EnshroudedClient::component_strides;
    layout_ready = true;
    ++layout_epoch;
    handles.clear();
    reverse_handles.clear();
}
std::string Status() {
    std::scoped_lock lock(state_mutex);
    std::ostringstream text;
    text << "types=" << types.size() << '/' << configured_types.size()
         << " profile=" << ShroudforgeCompatibility::EnshroudedClient::status
         << " game_thread=" << GameThreadDispatcher::Status();
    if (types.empty()) return text.str() + " registry=unresolved layout=unavailable";
    if (!layout_ready) return text.str() + " registry=ready layout=discovering";
    text << " layout=ready"
         << " entity{id=0x" << std::hex << live_layout.entity_id
         << ",generation=0x" << live_layout.entity_generation
         << ",layout=0x" << live_layout.entity_layout
         << ",storage=0x" << live_layout.entity_storage
         << ",row=0x" << live_layout.entity_row << std::dec << '}'
         << " components{bits=0x" << std::hex << live_layout.component_bits
         << ",offsets=0x" << live_layout.component_offsets
         << ",strides=0x" << live_layout.component_strides << std::dec << '}'
         << " epoch=" << layout_epoch;
    return text.str();
}
std::string Diagnostics() {
    std::scoped_lock lock(state_mutex);
    return nlohmann::json({
        {"schemaVersion",1}, {"providerAbi",1},
        {"profile",ShroudforgeCompatibility::EnshroudedClient::status},
        {"configuredTypes",configured_types.size()}, {"resolvedTypes",types.size()},
        {"layoutReady",layout_ready}, {"layoutEpoch",layout_epoch},
        {"dispatcher",nlohmann::json::parse(GameThreadDispatcher::Diagnostics())}
    }).dump();
}
void Shutdown() {
    stop_requested.store(true, std::memory_order_release);
    GameThreadDispatcher::Shutdown();
    std::scoped_lock lock(state_mutex);
    types.clear();
    configured_types.clear();
    live_layout = {};
    layout_ready = false;
    handles.clear();
    reverse_handles.clear();
    query_scans.clear();
    query_cache.clear();
    resolve_scans.clear();
    resolve_cache.clear();
}
}

extern "C" bool __cdecl ShroudforgeEcsConfigure(const char* const* names,
                                                const std::uint32_t* sizes,
                                                std::size_t count) {
    if (!names || !sizes || !count || count > 20'000) return false;
    std::unordered_map<std::string, std::uint32_t> contract;
    for (std::size_t index = 0; index < count; ++index) {
        if (!names[index] || !sizes[index]) return false;
        const std::string name{names[index]};
        if (!name.starts_with("keen::ecs::")) return false;
        contract.emplace(name, sizes[index]);
    }
    std::scoped_lock lock(state_mutex);
    if (configured_types == contract) return true;
    configured_types = std::move(contract);
    types.clear();
    for (const auto& component : ShroudforgeCompatibility::EnshroudedClient::runtime_components) {
        const auto configured = configured_types.find(std::string(component.qualified_name));
        if (configured != configured_types.end() && configured->second == component.size)
            types.emplace(configured->first, ComponentType{component.index, component.size});
    }
    live_layout = {};
    layout_ready = false;
    handles.clear();
    reverse_handles.clear();
    query_scans.clear();
    query_cache.clear();
    resolve_scans.clear();
    resolve_cache.clear();
    return true;
}

extern "C" bool __cdecl ShroudforgeEcsReady() {
    std::scoped_lock lock(state_mutex);
    return GameThreadDispatcher::Ready() && layout_ready && !types.empty();
}
extern "C" bool __cdecl ShroudforgeEcsCanWrite() {
    std::scoped_lock lock(state_mutex);
    return GameThreadDispatcher::Ready() && layout_ready && !types.empty();
}
extern "C" bool __cdecl ShroudforgeEcsDescribe(const char* name, std::uint32_t* size) {
    ComponentType component{};
    if (!size || !resolve_component(name, component)) return false;
    *size = component.size;
    return true;
}
extern "C" std::size_t __cdecl ShroudforgeEcsQuery(const char* const* names, std::size_t count,
                                                    std::uint32_t* entities, std::size_t capacity) {
    if (!names || !count || count > max_components || capacity > (1u << 20) || !ShroudforgeEcsReady()) return SIZE_MAX;
    auto operation = std::make_shared<QueryOperation>();
    for (std::size_t index = 0; index < count; ++index) {
        if (!names[index]) return SIZE_MAX;
        operation->owned_names.emplace_back(names[index]);
    }
    for (const auto& name : operation->owned_names) operation->name_pointers.push_back(name.c_str());
    operation->names = operation->name_pointers.data();
    operation->count = count;
    operation->output.resize(capacity);
    operation->entities = operation->output.data();
    operation->capacity = capacity;
    if (!GameThreadDispatcher::Invoke(query_on_game_thread, operation)) return SIZE_MAX;
    if (operation->result >= SIZE_MAX - 1) return operation->result;
    if (entities) std::copy_n(operation->output.begin(), (std::min)(capacity, operation->result), entities);
    return operation->result;
}
extern "C" std::uint32_t __cdecl ShroudforgeEcsResolve(std::uint32_t entity_id) {
    if (!entity_id || !ShroudforgeEcsReady()) return 0;
    auto operation = std::make_shared<ResolveOperation>();
    operation->entity_id = entity_id;
    return GameThreadDispatcher::Invoke(resolve_on_game_thread, operation) ? operation->result : 0;
}
extern "C" bool __cdecl ShroudforgeEcsRead(std::uint32_t handle, const char* name,
                                           void* value, std::size_t size) {
    if (!name || !value || !size || size > (1u << 20) || !ShroudforgeEcsReady()) return false;
    auto operation = std::make_shared<ReadOperation>();
    operation->handle = handle;
    operation->owned_name = name;
    operation->name = operation->owned_name.c_str();
    operation->output.resize(size);
    operation->value = operation->output.data();
    operation->size = size;
    if (!GameThreadDispatcher::Invoke(read_on_game_thread, operation) || !operation->result) return false;
    std::memcpy(value, operation->output.data(), size);
    return true;
}
extern "C" bool __cdecl ShroudforgeEcsWrite(std::uint32_t handle, const char* name,
                                            const void* expected, const void* value,
                                            std::size_t size) {
    if (!name || !expected || !value || !size || size > (1u << 20) || !ShroudforgeEcsCanWrite()) return false;
    auto operation = std::make_shared<WriteOperation>();
    operation->handle = handle;
    operation->owned_name = name;
    operation->name = operation->owned_name.c_str();
    const auto first = static_cast<const std::uint8_t*>(expected);
    const auto last = static_cast<const std::uint8_t*>(value);
    operation->source.assign(first, first + size);
    operation->destination.assign(last, last + size);
    operation->expected = operation->source.data();
    operation->value = operation->destination.data();
    operation->size = size;
    return GameThreadDispatcher::Invoke(write_on_game_thread, operation) && operation->result;
}
