#include "profile.h"
#include "logging_config.h"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <unordered_set>
#include "../../third_party/nlohmann/json.hpp"

namespace ShroudforgeCompatibility::EnshroudedClient {
bool Load() {
    try {
        HMODULE self{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&Load), &self)) return false;
        wchar_t module_path[32768]{}, process_path[32768]{};
        if (!GetModuleFileNameW(self, module_path, 32768) || !GetModuleFileNameW(nullptr, process_path, 32768)) return false;
        const auto root = std::filesystem::path(module_path).parent_path();
        const auto process = std::filesystem::path(process_path).filename().string();
        const auto base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
        const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
        const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
        const auto directory = root / "runtime/compatibility/profiles";
        nlohmann::json exact_profile, fallback_profile;
        unsigned exact_count{}, fallback_count{};
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            if (entry.path().extension() != ".json") continue;
            auto candidate = nlohmann::json::parse(std::ifstream(entry.path()));
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
        runtime_components.clear();
        std::unordered_set<std::string> names;
        std::unordered_set<unsigned> indices;
        for (const auto& component : selected.at("components")) {
            const auto name = component.at("name").get<std::string>();
            const auto index = component.at("index").get<unsigned>();
            const auto size = component.at("size").get<unsigned>();
            if (!name.starts_with("keen::ecs::") || index >= 1024 || !size || size > 65535 ||
                !names.insert(name).second || !indices.insert(index).second) throw std::runtime_error("invalid component mapping");
            runtime_components.push_back({name, static_cast<std::uint16_t>(index), size});
        }
        if (runtime_components.empty()) throw std::runtime_error("empty component profile");
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
