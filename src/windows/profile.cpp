#include "profile.h"
#include "logging_config.h"
#include "embedded_compatibility_profile_ids.h"
#include <windows.h>
#include <filesystem>
#include <unordered_set>
#include "../../third_party/nlohmann/json.hpp"

namespace ShroudforgeCompatibility::EnshroudedClient {
bool Load() {
    try {
        HMODULE self{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&Load), &self)) return false;
        wchar_t process_path[32768]{};
        if (!GetModuleFileNameW(nullptr, process_path, 32768)) return false;
        const auto process = std::filesystem::path(process_path).filename().string();
        const auto base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
        const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
        const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
        nlohmann::json exact_profile, fallback_profile;
        unsigned exact_count{}, fallback_count{};
        for (const auto resource_id : ShroudforgeEmbeddedProfiles::resource_ids) {
            const auto resource = FindResourceW(self, MAKEINTRESOURCEW(resource_id), MAKEINTRESOURCEW(10));
            if (!resource) throw std::runtime_error("embedded compatibility profile is missing");
            const auto loaded = LoadResource(self, resource);
            if (!loaded) throw std::runtime_error("embedded compatibility profile could not be loaded");
            const auto size = SizeofResource(self, resource);
            const auto* data = static_cast<const char*>(LockResource(loaded));
            if (!data || !size) throw std::runtime_error("embedded compatibility profile is empty");
            auto candidate = nlohmann::json::parse(data, data + size);
            if (candidate.at("schemaVersion") != 1 || candidate.at("target") != process) continue;
            const bool matches = candidate.at("image").at("timestamp") == nt->FileHeader.TimeDateStamp &&
                candidate.at("image").at("size") == nt->OptionalHeader.SizeOfImage;
            if (matches) {
                ++exact_count;
                exact_profile = std::move(candidate);
            } else if (candidate.value("allowStructuralRevalidation", false)) {
                ++fallback_count;
                fallback_profile = std::move(candidate);
            }
        }
        // Exact image identity always takes precedence over every opt-in
        // structural fallback, regardless of file enumeration order.
        const bool exact = exact_count == 1;
        if (exact_count > 1) {
            status = "ambiguous-exact-profile"; return false;
        }
        if (exact_count == 0 && fallback_count != 1) {
            status = "missing-or-ambiguous-profile"; return false;
        }
        const auto& selected = exact ? exact_profile : fallback_profile;
        // Identity is a lookup hint. Both hooks still require unique executable
        // matches and exact overwritten instructions before any patch is made.
        image_timestamp = nt->FileHeader.TimeDateStamp;
        image_size = nt->OptionalHeader.SizeOfImage;
        const auto& layout = selected.at("layout");
        auto offset = [&](const char* key) {
            const auto value = layout.at(key).get<std::size_t>();
            if (value > 0x10000) throw std::runtime_error("layout offset out of range");
            return value;
        };
        entity_manager_count = offset("entity_manager_count");
        entity_manager_table = offset("entity_manager_table");
        component_offsets = offset("component_offsets"); component_strides = offset("component_strides");
        entity_id = offset("entity_id"); entity_generation = offset("entity_generation");
        entity_layout = offset("entity_layout"); entity_storage = offset("entity_storage");
        entity_row = offset("entity_row"); component_bits = offset("component_bits"); lookup_manager = offset("lookup_manager");
        const auto& hooks = selected.at("hooks");
        game_thread_signature = hooks.at("game_thread").at("signature").get<std::string>();
        entity_manager_signature = hooks.at("entity_manager").at("signature").get<std::string>();
        game_thread_original = hooks.at("game_thread").at("original").get<std::vector<std::uint8_t>>();
        entity_manager_original = hooks.at("entity_manager").at("original").get<std::vector<std::uint8_t>>();
        if (game_thread_original.size() < 5 || game_thread_original.size() > 32 ||
            entity_manager_original.size() < 5 || entity_manager_original.size() > 32) throw std::runtime_error("invalid hook length");
        world_prop_update_signature = hooks.at("world_prop_update").at("signature").get<std::string>();
        world_prop_update_original = hooks.at("world_prop_update").at("original").get<std::vector<std::uint8_t>>();
        world_actor_placement_signature = hooks.at("world_actor_placement").at("signature").get<std::string>();
        world_actor_placement_original = hooks.at("world_actor_placement").at("original").get<std::vector<std::uint8_t>>();
        if (world_prop_update_original.size() < 5 || world_prop_update_original.size() > 32 ||
            world_actor_placement_original.size() < 5 || world_actor_placement_original.size() > 32)
            throw std::runtime_error("invalid world context hook length");
        const auto& entity_context = selected.at("worldContexts").at("entityPlacement");
        auto context_offset = [&](const char* key) {
            const auto value = entity_context.at(key).get<std::size_t>();
            if (value > 0x10000) throw std::runtime_error(std::string("world context offset out of range: ") + key);
            return value;
        };
        world_context_layout.actor_frame_service_view = context_offset("actorFrameServiceViewOffset");
        world_context_layout.service_view_world = context_offset("serviceViewWorldOffset");
        world_context_layout.placement_context = context_offset("placementContextOffset");
        world_context_layout.place_queue = context_offset("placeQueueOffset");
        world_context_layout.remove_queue = context_offset("removeQueueOffset");
        world_context_layout.publish_state = context_offset("publishStateOffset");
        world_context_layout.publish_commands = context_offset("publishCommandsOffset");
        world_context_layout.owner = context_offset("ownerOffset");
        runtime_components.clear();
        std::unordered_set<std::string> names;
        std::unordered_set<unsigned> indices;
        for (const auto& component : selected.at("components")) {
            const auto name = component.at("name").get<std::string>();
            const auto index = component.at("index").get<unsigned>();
            const auto size = component.at("size").get<unsigned>();
            if (!name.starts_with("keen::ecs::") || index >= 1024 || !size || size > 65535 ||
                !names.insert(name).second || !indices.insert(index).second) throw std::runtime_error("invalid component mapping");
            if (exact) runtime_components.push_back({name, static_cast<std::uint16_t>(index), size});
        }
        if (exact && runtime_components.empty()) throw std::runtime_error("empty component profile");
        runtime_operations.clear();
        runtime_patches.clear();
        world_finish_event_id_rva = 0;
        const auto operations = selected.find("worldOperations");
        if (operations != selected.end()) {
            std::unordered_set<std::string> operation_names;
            for (auto item = operations->begin(); item != operations->end(); ++item) {
                const auto& value = item.value();
                RuntimeOperation operation{};
                operation.name = item.key();
                operation.function_rva = value.value("functionRva", std::uintptr_t{});
                operation.global_rva = value.value("globalRva", std::uintptr_t{});
                operation.guard_rva = value.value("guardRva", std::uintptr_t{});
                operation.validation_offset = value.value("validationOffset", std::uintptr_t{});
                operation.mode = value.value("mode", std::uint32_t{});
                operation.context_pointer_offset = value.value("contextPointerOffset", std::ptrdiff_t{});
                operation.world_offset = value.value("worldOffset", std::ptrdiff_t{});
                if (operation.name == "runtime.world.entity.finish_building")
                    world_finish_event_id_rva = value.value("eventIdRva", std::uintptr_t{});
                operation.guard_bytes = value.value("guardBytes", std::vector<std::uint8_t>{});
                operation.abi = value.at("abi").get<std::string>();
                operation.thread = value.at("thread").get<std::string>();
                operation.context = value.at("context").get<std::string>();
                if (!operation.name.starts_with("runtime.world.") ||
                    !operation_names.insert(operation.name).second ||
                    operation.abi.empty() || operation.abi.size() > 256 ||
                    operation.thread != "game" || operation.context.empty() || operation.context.size() > 256 ||
                    (operation.function_rva == 0) == (operation.global_rva == 0) ||
                    operation.guard_bytes.size() > 64)
                    throw std::runtime_error("invalid world operation profile entry: " + operation.name);
                if (operation.name == "runtime.world.context.active" &&
                    (!operation.global_rva || !operation.context_pointer_offset || !operation.world_offset))
                    throw std::runtime_error("active world context requires a validated pointer chain");

                const auto selected_image_size = static_cast<std::size_t>(nt->OptionalHeader.SizeOfImage);
                const bool target_in_image = operation.function_rva
                    ? operation.function_rva < selected_image_size
                    : operation.global_rva < selected_image_size;
                const bool guard_in_image = operation.guard_bytes.empty() ||
                    (operation.guard_rva < selected_image_size &&
                     operation.guard_bytes.size() <= selected_image_size - operation.guard_rva);
                const bool guard_matches = guard_in_image &&
                    (operation.guard_bytes.empty() ||
                     std::memcmp(base + operation.guard_rva, operation.guard_bytes.data(),
                                 operation.guard_bytes.size()) == 0);
                const bool event_id_valid = operation.name != "runtime.world.entity.finish_building" ||
                    (world_finish_event_id_rva && world_finish_event_id_rva < selected_image_size &&
                     sizeof(std::uint32_t) <= selected_image_size - world_finish_event_id_rva);
                operation.available = exact && target_in_image && guard_matches && event_id_valid;
                operation.status = !exact ? "requires-exact-image-build" :
                    !target_in_image ? "target-outside-image" :
                    !guard_in_image ? "guard-outside-image" :
                    !guard_matches ? "instruction-guard-mismatch" :
                    !event_id_valid ? "finish-event-id-outside-image" :
                    operation.global_rva ? "runtime-pointer-validation-required" : "verified";
                runtime_operations.push_back(std::move(operation));
            }
        }
        const auto patches = selected.find("runtimePatches");
        if (patches != selected.end()) {
            std::unordered_set<std::string> patch_names;
            for (auto item = patches->begin(); item != patches->end(); ++item) {
                const auto& value = item.value();
                RuntimePatch patch{};
                patch.name = item.key();
                patch.signature = value.at("signature").get<std::string>();
                patch.kind = value.at("kind").get<std::string>();
                patch.overwrite = value.at("overwriteBytes").get<std::size_t>();
                patch.payload = value.at("payload").get<std::vector<std::uint8_t>>();
                patch.return_rel32_offset = value.value("returnRel32Offset", std::size_t{});
                if (!patch.name.starts_with("runtime.patch.") || !patch_names.insert(patch.name).second ||
                    patch.signature.empty() || patch.signature.size() > 256 || patch.overwrite < 3 || patch.overwrite > 32 ||
                    patch.payload.empty() || patch.payload.size() > 256 ||
                    (patch.kind != "bytes" && patch.kind != "detour") ||
                    (patch.kind == "bytes" && patch.payload.size() != patch.overwrite) ||
                    (patch.kind == "detour" && (patch.return_rel32_offset < 1 || patch.return_rel32_offset + 4 > patch.payload.size() ||
                        patch.payload[patch.return_rel32_offset - 1] != 0xe9)))
                    throw std::runtime_error("invalid runtime patch profile entry: " + patch.name);
                if (exact) runtime_patches.push_back(std::move(patch));
            }
        }
        status = selected.at("id").get<std::string>() + (exact ? ":exact-image" : ":structural-revalidation");
        return true;
    } catch (const std::exception& error) {
        status = std::string("profile-error:") + error.what();
        HMODULE self{};
        wchar_t path[32768]{};
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&Load), &self) && GetModuleFileNameW(self,path,32768) &&
            KfcRuntimeConfig::Allows(std::filesystem::path(path).parent_path(),'E')) OutputDebugStringA(status.c_str());
        return false;
    }
}
}
