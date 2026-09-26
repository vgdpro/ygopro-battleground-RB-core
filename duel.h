/*
 * duel.h
 *
 *  Created on: 2010-4-8
 *      Author: Argon
 */

#ifndef DUEL_H_
#define DUEL_H_

#include "common.h"
#include "sort.h"
#include "mtrandom.h"
#include <set>
#include <unordered_set>
#include <unordered_map>
#include <vector>

class card;
class group;
class effect;
class field;
class interpreter;

using card_set = std::set<card *, card_sort>;

struct duel_info
{
    uint64_t card_id{1};
    int32_t field_id{1};
    uint16_t copy_id{1};
};

class duel
{
public:
    static constexpr int HOME_FIELD_COUNT = 4;
    static constexpr int BATTLE_FIELD_COUNT = 2;
    static constexpr int FIELD_COUNT = HOME_FIELD_COUNT + BATTLE_FIELD_COUNT;
    static constexpr uint8_t INVALID_FIELD_INDEX = 0xff;
    struct BattleFieldBinding
    {
        uint8_t home_fields[2]{INVALID_FIELD_INDEX, INVALID_FIELD_INDEX};
    };
    char strbuffer[256]{};
    int32_t rng_version{2};
    interpreter *lua;
    field *fields[FIELD_COUNT]{};
    field *game_field;
    duel_info infos;
    mtrandom random;
    uint64_t next_card_id{1};
    BattleFieldBinding battle_field_bindings[BATTLE_FIELD_COUNT]{};

    uint32_t duel_options{0};
    int32_t duel_rule{CURRENT_RULE};
    int32_t start_lp[2]{8000, 8000};
    int32_t start_hand[2]{5, 5};
    int32_t draw_count[2]{1, 1};
    std::vector<byte> message_buffer;
    std::unordered_set<card *> cards;
    std::unordered_set<card *> assumes;
    std::unordered_set<group *> groups;
    std::unordered_set<group *> sgroups;
    std::unordered_set<effect *> effects;
    std::unordered_set<effect *> uncopy;

    // Lua 一次性守卫字段 per-field 隔离（M2.10）
    static constexpr int GUARD_FIELD_COUNT = 8;
    static const char *GUARD_FIELD_NAMES[];
    // key = (code << 8) | field_index, value = 6-bit 掩码 (bit 0-5 对应 field 0-5)
    std::unordered_map<uint64_t, uint8_t> guard_states;

    // Auxiliary.* per-field 隔离（M2.10）
    static constexpr int AUX_TABLE_FIELD_COUNT = 3;
    static constexpr int AUX_BOOL_FIELD_COUNT = 2;
    static const char *AUX_TABLE_FIELD_NAMES[];
    static const char *AUX_BOOL_FIELD_NAMES[];
    // 表/对象字段：Lua registry ref，LUA_NOREF 表示 nil/未初始化
    int32_t aux_table_refs[AUX_TABLE_FIELD_COUNT][FIELD_COUNT]{};
    // 布尔字段：0/1
    uint8_t aux_bool_values[AUX_BOOL_FIELD_COUNT][FIELD_COUNT]{};

    duel();
    ~duel();
    void clear();

    uint32_t buffer_size() const
    {
        return (uint32_t)message_buffer.size() & PROCESSOR_BUFFER_LEN;
    }
    card *new_card(uint32_t code, bool register_with_lua = true);
    static bool is_home_field_index(uint8_t field_index);
    static bool is_battle_field_index(uint8_t field_index);
    bool is_battle_field(const field *pfield) const;
    bool is_corpse_player(const field *pfield, uint8_t playerid) const;
    uint8_t get_field_index(const field *pfield) const;
    bool bind_battle_field(uint8_t battle_field, uint8_t home_field_p0, uint8_t home_field_p1);
    bool reset_field(uint8_t field_index);
    card *clone_card_to_field(card *src, uint8_t dst_player,
                              uint8_t location, uint8_t seq, uint8_t pos, uint8_t battle_field);
    void clone_card_state(card *src, card *dst);
    void clone_effects_to_field(uint8_t battle_field, uint8_t home_field_p0, uint8_t home_field_p1,
                                const std::unordered_map<card *, card *> &card_map);
    // 全量克隆所有 group 到 battle_field，并建立原 group ↔ 克隆 group 的双向绑定
    void clone_groups_to_field(uint8_t battle_field, const std::unordered_map<card *, card *> &card_map);
    void remap_effect_references(const std::unordered_map<card *, card *> &card_map);
    void remap_card_references(const std::unordered_map<card *, card *> &card_map);
    void remap_card_effect_references();
    bool return_field_to_main(uint8_t battle_field);
    group *new_group();
    group *new_group(card *pcard);
    group *new_group(const card_set &cset);
    effect *new_effect();
    void delete_card(card *pcard);
    void delete_group(group *pgroup);
    void delete_effect(effect *peffect);
    void release_script_group();
    void restore_assumes();
    int32_t read_buffer(byte *buf);
    void write_buffer(const void *data, size_t size);
    void write_buffer32(uint32_t value);
    void write_buffer16(uint16_t value);
    void write_buffer8(uint8_t value);
    void clear_buffer();
    void set_responsei(int32_t resp);
    void set_responseb(byte *resp);
    int32_t get_next_integer(int32_t l, int32_t h);

private:
    void init_field(uint8_t field_index);
    group *register_group(group *pgroup);
    void init_fields();
};

#endif /* DUEL_H_ */
