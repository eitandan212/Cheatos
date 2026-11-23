#pragma once
#include <cstdint>

namespace offsets {

// --------------------------------------------------------------------------
// IL2CPP string + klass layout
// --------------------------------------------------------------------------
constexpr uintptr_t il2cpp_string_length = 0x10;
constexpr uintptr_t il2cpp_string_chars  = 0x14;

constexpr uintptr_t klass_name          = 0x48;
constexpr uintptr_t klass_static_fields = 0xB8;
constexpr uintptr_t klass_element_class = 0x58;
constexpr uintptr_t klass_cast_class    = 0x60;

// --------------------------------------------------------------------------
// IL2CPP generic Dictionary<TKey,TValue>
// --------------------------------------------------------------------------
constexpr uintptr_t dict_entries        = 0x18;
constexpr uintptr_t dict_count          = 0x40;
constexpr uintptr_t array_items         = 0x20;
constexpr uintptr_t dict_entry_stride   = 0x18;
constexpr uintptr_t dict_entry_hash     = 0x00;
constexpr uintptr_t dict_entry_key      = 0x08;
constexpr uintptr_t dict_entry_value    = 0x10;

// --------------------------------------------------------------------------
// BattleModel root (+0x00)
// --------------------------------------------------------------------------
namespace battle_model {
constexpr uintptr_t entities        = 0x10;   // BattleEntitiesModel
constexpr uintptr_t camera          = 0x20;   // BattleCameraModel
constexpr uintptr_t character_slots = 0x30;
}

// BattleEntitiesModel
namespace battle_entities_model {
constexpr uintptr_t models          = 0x50;   // Dict<int, EntityModel>
constexpr uintptr_t character_slots = 0x68;
constexpr uintptr_t player          = 0x80;   // local PlayerEntityModel
constexpr uintptr_t player_alt      = 0xA0;   // fallback slot
constexpr uintptr_t world_entities  = 0x88;   // WorldEntitiesModel
}

// WorldEntitiesModel
namespace world_entities_model {
constexpr uintptr_t battle_characters = 0x40;
constexpr uintptr_t random_loot       = 0xA0;
constexpr uintptr_t loot_nodes        = 0xB0;
constexpr uintptr_t radius_interacts  = 0xC0;
constexpr uintptr_t cars              = 0xD0;
constexpr uintptr_t helicopters       = 0xD8;
constexpr uintptr_t doors             = 0xE0;
}

// BattleCameraModel
namespace battle_camera_model {
constexpr uintptr_t position = 0x30;
constexpr uintptr_t forward  = 0x50;
constexpr uintptr_t right    = 0x60;
constexpr uintptr_t up       = 0x70;
}

// PlayerEntityModel
namespace player_entity_model {
constexpr uintptr_t server_model = 0x20;      // BattleCharacterModel (server)
}

// BattleCharacterModel (server-side)
namespace battle_character_model {
constexpr uintptr_t private_model   = 0x10;
constexpr uintptr_t server_transform= 0x28;
constexpr uintptr_t replicated_tfm  = 0x30;
constexpr uintptr_t client_transform= 0x38;
constexpr uintptr_t data            = 0x40;
constexpr uintptr_t in_vehicle      = 0x48;
constexpr uintptr_t crouch          = 0x50;
constexpr uintptr_t center_position = 0xF0;
constexpr uintptr_t hitbox_access   = 0x120;
}

// PrivateModel (local player health root)
namespace private_model {
constexpr uintptr_t health_model = 0x1B0;
}

// CommonData chain
namespace common_data {
constexpr uintptr_t position_packed = 0x10;
constexpr uintptr_t role            = 0x50;
constexpr uintptr_t item_in_hands   = 0x60;
}

// DataChain: model + 0x40 -> data
namespace data_chain {
constexpr uintptr_t common_data   = 0x38;
constexpr uintptr_t private_data  = 0x60;
}

// PrivateData -> health
namespace private_data {
constexpr uintptr_t health = 0x48;
}

// HealthModel
namespace health_model {
constexpr uintptr_t value_int32 = 0x18;
}

// LocalHealthModel chain -> valueModel -> float value
namespace health_value_model {
constexpr uintptr_t value_float = 0x18;
}

// Transform (packed or boxed) - +0x10 Vec3 or three IPrimitiveData<float> pointers
namespace transform_packed {
constexpr uintptr_t vec3_packed = 0x10;
constexpr uintptr_t prim_x      = 0x10;
constexpr uintptr_t prim_y      = 0x18;
constexpr uintptr_t prim_z      = 0x20;
constexpr uintptr_t prim_value  = 0x18;
}

// Replicated transform chain
namespace replicated_transform {
constexpr uintptr_t position_model = 0x10;
constexpr uintptr_t position_vec3  = 0x18;
}

// Character slot descriptor
namespace char_slot {
constexpr uintptr_t client_slots   = 0x10;     // ClientCharacterSlots
constexpr uintptr_t nickname_array = 0x10;     // in ClientCharacterSlots
constexpr uintptr_t clan_tag       = 0x18;
}

// Hitbox access chain
namespace hitbox {
constexpr uintptr_t shapes      = 0x50;        // from hitbox_access
constexpr uintptr_t node        = 0x110;       // from shapes
constexpr uintptr_t cap_center  = 0x60;
constexpr uintptr_t cap_top     = 0xB0;
}

// IPrimitiveData<T>
namespace primitive {
constexpr uintptr_t value = 0x18;
}

// CinemachineBrain
namespace cinemachine_brain {
constexpr uintptr_t fov = 0x60;
}

// BattleAimTargetUpdater
namespace aim_target_updater {
constexpr uintptr_t brain = 0x38;              // CinemachineBrain*
}

// BattleEntitiesUpdater (view updater)
namespace entities_updater {
constexpr uintptr_t view_dict = 0x60;
}

// CharacterMovementView - head position
namespace char_view {
constexpr uintptr_t head_position = 0x70;      // Transform
constexpr uintptr_t head_behavior = 0x78;      // behavior
constexpr uintptr_t bone_target   = 0x20;      // from head_behavior
}

// Unity Transform (managed) -> native hierarchy
namespace unity_transform {
constexpr uintptr_t native_ptr    = 0x48;
constexpr uintptr_t hierarchy_ptr = 0x38;      // inside native
constexpr uintptr_t hier_index    = 0x40;      // inside native
}

// TransformHierarchy
namespace transform_hier {
constexpr uintptr_t count    = 0x18;
constexpr uintptr_t matrices = 0x20;
constexpr uintptr_t parents  = 0x28;
constexpr uintptr_t pointers = 0x30;
constexpr uintptr_t trs_stride = 0x30;         // 48 bytes
constexpr uintptr_t trs_pos  = 0x00;
constexpr uintptr_t trs_quat = 0x0C;
constexpr uintptr_t trs_scl  = 0x1C;
}

// Role description
namespace role_desc {
constexpr uintptr_t desc_box = 0x18;           // PlayerRoleDescription*
constexpr uintptr_t id_str   = 0x10;           // string*
}

// Random loot model -> description -> position
namespace loot_model {
constexpr uintptr_t description = 0x20;
constexpr uintptr_t position    = 0x10;
}

// --------------------------------------------------------------------------
// Camera fallback
// --------------------------------------------------------------------------
constexpr float camera_fov_degrees = 60.0f;

// --------------------------------------------------------------------------
// Klass resolution: three candidate RVA sets (metadata pointer relative to
// GameAssembly.dll base). On a fresh dump every set is tried in order and the
// first set whose pointers validate wins; thereafter the hits are name-anchored.
// --------------------------------------------------------------------------
struct TypeInfoSet {
    uintptr_t battle_model;
    uintptr_t battle_entities_model;
    uintptr_t battle_character_model;
    uintptr_t battle_character_client_model;
    uintptr_t battle_camera_model;
    uintptr_t world_entities_model;
    uintptr_t random_loot_object_model;
    uintptr_t loot_node_model;
    uintptr_t radius_interact_entity_model;
    uintptr_t door_model;
    uintptr_t aim_target_updater;
    uintptr_t entities_updater;
    uintptr_t cinemachine_brain;
    uintptr_t weapon_spread;
    uintptr_t sprite_recoil_view;
    uintptr_t weapon_inertia_data;
};

constexpr TypeInfoSet type_info_sets[3] = {
    { 0x03B91A48, 0x03B91A50, 0x03B91A58, 0x03B91A60, 0x03B91A68,
      0x03B91A70, 0x03B91A78, 0x03B91A80, 0x03B91A88, 0x03B91A90,
      0x03B91A98, 0x03B91AA0, 0x03B91AA8, 0x03B91AB0, 0x03B91AB8, 0x03B91AC0 },
    { 0x03C01A48, 0x03C01A50, 0x03C01A58, 0x03C01A60, 0x03C01A68,
      0x03C01A70, 0x03C01A78, 0x03C01A80, 0x03C01A88, 0x03C01A90,
      0x03C01A98, 0x03C01AA0, 0x03C01AA8, 0x03C01AB0, 0x03C01AB8, 0x03C01AC0 },
    { 0x03D01A48, 0x03D01A50, 0x03D01A58, 0x03D01A60, 0x03D01A68,
      0x03D01A70, 0x03D01A78, 0x03D01A80, 0x03D01A88, 0x03D01A90,
      0x03D01A98, 0x03D01AA0, 0x03D01AA8, 0x03D01AB0, 0x03D01AB8, 0x03D01AC0 },
};

// GameObjectManager (UnityPlayer.dll RVA)
constexpr uintptr_t gom_rva = 0x2230768;
constexpr uintptr_t gom_native_ptr = 0x60;

// Klass arena search window
constexpr uintptr_t klass_arena_lo = 0x30000000;
constexpr uintptr_t klass_arena_hi = 0x40000000;

// --------------------------------------------------------------------------
// Skeleton reference heights (meters) for aim/box fallback
// --------------------------------------------------------------------------
namespace skeleton {
constexpr float head_height         = 1.65f;
constexpr float torso_height        = 1.10f;
constexpr float leg_height          = 0.50f;
constexpr float foot_height         = 0.00f;
constexpr float hand_height         = 1.10f;
constexpr float seated_head_height  = 0.60f;
constexpr float seated_torso_height = 0.30f;
constexpr float vehicle_window_h    = 1.20f;
}

// Sanity bounds
constexpr float world_bound    = 1.0e6f;
constexpr float parked_slot_y  = -8000.0f;

} // namespace offsets
