#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace ShroudforgeCompatibility::EnshroudedClient {
struct RuntimeComponent { std::string qualified_name; std::uint16_t index; std::uint32_t size; };
inline std::uint32_t image_timestamp{}, image_size{};
inline std::size_t entity_manager_count{}, entity_manager_table{}, component_offsets{}, component_strides{};
inline std::size_t entity_id{}, entity_generation{}, entity_layout{}, entity_storage{}, entity_row{}, component_bits{}, lookup_manager{};
inline std::string game_thread_signature, entity_manager_signature, status{"not-initialized"};
inline std::vector<std::uint8_t> game_thread_original, entity_manager_original;
inline std::vector<RuntimeComponent> runtime_components;
bool Load();
}
