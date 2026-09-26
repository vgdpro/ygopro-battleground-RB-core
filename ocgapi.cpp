/*
 * interface.cpp
 *
 *  Created on: 2010-5-2
 *      Author: Argon
 */
#include <cstdio>
#include <cstring>
#include <set>
#include <unordered_map>
#include "ocgapi.h"
#include "duel.h"
#include "card.h"
#include "group.h"
#include "effect.h"
#include "field.h"
#include "interpreter.h"
#include "buffer.h"

static uint32_t default_card_reader(uint32_t code, card_data *data)
{
    return 0;
}
static uint32_t default_random_card_reader(uint64_t capability, uint32_t count, uint32_t random_seed, uint32_t *codes)
{
    return 0;
}
static uint32_t default_message_handler(intptr_t pduel, uint32_t message_type)
{
    return 0;
}
static script_reader sreader = default_script_reader;
static card_reader creader = default_card_reader;
static random_card_reader random_creader = default_random_card_reader;
static message_handler mhandler = default_message_handler;
static byte buffer[0x100000];
static std::set<duel *> duel_set;

OCGCORE_API void set_script_reader(script_reader f)
{
    sreader = f;
}
OCGCORE_API void set_card_reader(card_reader f)
{
    creader = f;
}
OCGCORE_API void set_random_card_reader(random_card_reader f)
{
    random_creader = f ? f : default_random_card_reader;
}
OCGCORE_API void set_message_handler(message_handler f)
{
    mhandler = f;
}
byte *read_script(const char *script_name, int *len)
{
    return sreader(script_name, len);
}
uint32_t read_card(uint32_t code, card_data *data)
{
    if (code == TEMP_CARD_ID)
    {
        data->clear();
        return 0;
    }
    return creader(code, data);
}
uint32_t handle_message(void *pduel, uint32_t message_type)
{
    return mhandler((intptr_t)pduel, message_type);
}
OCGCORE_API byte *default_script_reader(const char *script_name, int *slen)
{
    FILE *fp;
    fp = std::fopen(script_name, "rb");
    if (!fp)
        return nullptr;
    size_t len = std::fread(buffer, 1, sizeof buffer, fp);
    std::fclose(fp);
    if (len >= sizeof buffer)
        return nullptr;
    *slen = (int)len;
    return buffer;
}
OCGCORE_API intptr_t create_duel(uint_fast32_t seed)
{
    duel *pduel = new duel();
    duel_set.insert(pduel);
    pduel->random.seed(seed);
    pduel->rng_version = 1;
    return (intptr_t)pduel;
}
OCGCORE_API intptr_t create_duel_v2(uint32_t seed_sequence[])
{
    duel *pduel = new duel();
    duel_set.insert(pduel);
    pduel->random.seed(seed_sequence, SEED_COUNT);
    pduel->rng_version = 2;
    return (intptr_t)pduel;
}
OCGCORE_API void start_duel(intptr_t pduel, uint32_t options)
{
    duel *pd = (duel *)pduel;
    uint16_t duel_rule = options >> 16;
    uint16_t duel_options = options & 0xffff;

    // 校验并存储规则到 duel 层级
    if (duel_rule >= 1 && duel_rule <= CURRENT_RULE)
        pd->duel_rule = duel_rule;
    else if (options & DUEL_OBSOLETE_RULING) // provide backward compatibility with replay
        pd->duel_rule = 1;
    if (pd->duel_rule < 1 || pd->duel_rule > CURRENT_RULE)
        pd->duel_rule = CURRENT_RULE;

    pd->duel_options = duel_options;

    // 向全部家园场各推 PROCESSOR_START + PROCESSOR_TURN
    for (int i = 0; i < duel::HOME_FIELD_COUNT; ++i)
    {
        pd->fields[i]->add_process(PROCESSOR_START, 0, 0, 0, 0, 0);
        pd->fields[i]->add_process(PROCESSOR_TURN, 0, 0, 0, 0, 0);
    }
}
OCGCORE_API uint32_t get_random_card(intptr_t pduel, uint64_t capability, uint32_t count, uint32_t *codes)
{
    if (!pduel || !count || !codes)
        return 0;
    duel *pd = (duel *)pduel;
    return random_creader(capability, count, (uint32_t)pd->random.rand(), codes);
}
// 克隆家园场指定 field 的所有卡牌到战斗场
// 遍历 7 个区域：DECK/HAND/MZONE/SZONE/GRAVE/REMOVED/EXTRA
// card_map: 输出参数，记录源卡→克隆卡的映射，供后续效果克隆使用
static void clone_field_cards(duel *pd, uint8_t battle_field, uint8_t home_field, uint8_t src_player, uint8_t dst_player,
                              std::unordered_map<card *, card *> &card_map)
{
    static const uint8_t locations[] = {
        LOCATION_DECK, LOCATION_HAND, LOCATION_MZONE, LOCATION_SZONE,
        LOCATION_GRAVE, LOCATION_REMOVED, LOCATION_EXTRA};
    for (uint8_t loc : locations)
    {
        const card_vector *vec = pd->fields[home_field]->get_field_vector(src_player, loc);
        if (!vec)
            continue;
        // DECK/EXTRA 使用 SEQ_DECKTOP(0) 保持顺序；其他区域保留原位置
        for (size_t si = 0; si < vec->size(); ++si)
        {
            card *pcard = (*vec)[si];
            if (!pcard || pcard->owner != src_player)
                continue;
            uint8_t seq = (loc == LOCATION_DECK || loc == LOCATION_EXTRA) ? 0 : (uint8_t)si;
            uint8_t pos = pcard->current.position;
            card *cloned = pd->clone_card_to_field(pcard, dst_player, loc, seq, pos, battle_field);
            if (cloned)
            {
                card_map[pcard] = cloned;
                // 记录超量素材的映射
                for (size_t mi = 0; mi < pcard->xyz_materials.size() && mi < cloned->xyz_materials.size(); ++mi)
                    card_map[pcard->xyz_materials[mi]] = cloned->xyz_materials[mi];
            }
        }
    }
}

OCGCORE_API int32_t merge_field_to_bp(intptr_t pduel, uint8_t battle_field,
                                      uint8_t home_field_p0, uint8_t home_field_p1, uint8_t turn_player)
{
    duel *pd = (duel *)pduel;
    if (!pd || turn_player > 1 || !duel::is_battle_field_index(battle_field) || !duel::is_home_field_index(home_field_p0) || !duel::is_home_field_index(home_field_p1) || home_field_p0 == home_field_p1)
        return FALSE;
    if (!pd->reset_field(battle_field) || !pd->bind_battle_field(battle_field, home_field_p0, home_field_p1))
        return FALSE;
    // 重置战斗场 Auxiliary.* per-field 变量为干净初始状态（M2.10）
    // ExtraDeckSummonCountLimit → {0:1, 1:1}
    {
        lua_State *L = pd->lua->lua_state;
        lua_getglobal(L, "Auxiliary"); // +1 Auxiliary
        lua_newtable(L);               // +1 {}
        lua_pushinteger(L, 1);
        lua_setfield(L, -2, "0"); // [0]=1
        lua_pushinteger(L, 1);
        lua_setfield(L, -2, "1");                         // [1]=1
        lua_setfield(L, -2, "ExtraDeckSummonCountLimit"); // -2
        lua_pop(L, 1);                                    // -1
    }
    // merge_single_effect_codes → {}
    pd->lua->set_global_empty_table("Auxiliary", "merge_single_effect_codes");
    // SubGroupCaptured → nil
    pd->lua->set_global_nil("Auxiliary", "SubGroupCaptured");
    // 从家园场同步 LP
    pd->fields[battle_field]->player[0].lp = !pd->fields[home_field_p0]->not_corpse[0] ? CORPSE_PLAYER_LP : pd->fields[home_field_p0]->player[0].lp;
    pd->fields[battle_field]->player[1].lp = !pd->fields[home_field_p1]->not_corpse[0] ? CORPSE_PLAYER_LP : pd->fields[home_field_p1]->player[0].lp;
    pd->fields[battle_field]->not_corpse[0] = pd->fields[home_field_p0]->not_corpse[0];
    pd->fields[battle_field]->not_corpse[1] = pd->fields[home_field_p1]->not_corpse[0];
    // 克隆双方家园场卡牌到战斗场（不注册效果，效果由 clone_effects_to_field 一比一复制）
    std::unordered_map<card *, card *> card_map;
    clone_field_cards(pd, battle_field, home_field_p0, 0, 0, card_map);
    clone_field_cards(pd, battle_field, home_field_p1, 0, 1, card_map);
    // 重映射卡牌 B 组卡→卡指针引用（equiping_target/equiping_cards/effect_target_* 等）和 unique_function
    // 必须在 clone_effects_to_field 之前执行：card::add_effect 注册 EFFECT_TYPE_EQUIP /
    // EFFECT_TYPE_TARGET 效果时会读取 equiping_target / effect_target_cards 建立 disable 检查目标
    pd->remap_card_references(card_map);
    // 克隆所有效果到战斗场卡牌和 temp_card
    pd->clone_effects_to_field(battle_field, home_field_p0, home_field_p1, card_map);
    // 全量克隆所有 group 到战斗场，并建立双向绑定
    pd->clone_groups_to_field(battle_field, card_map);
    // 重映射 effect 额外属性（label_object/required_handorset_effects/active_handler/last_handler）
    // 必须在 card/effect/group 全部克隆完毕后执行，因为 label_object 可能引用 group
    pd->remap_effect_references(card_map);
    // 重映射卡牌卡→effect 引用（unique_effect/reason_effect），依赖 effect 克隆完成
    pd->remap_card_effect_references();
    for (auto &entry : card_map)
        entry.second->apply_field_effect();
    pd->fields[battle_field]->adjust_instant();
    // 刷新战斗场连续效果
    pd->fields[battle_field]->adjust_all();
    // pd->game_field = pd->fields[battle_field];
    //  设置先手玩家并推送处理器
    pd->fields[battle_field]->infos.turn_player = 0;
    pd->fields[battle_field]->add_process(PROCESSOR_START, 0, 0, 0, 0, 0, 0);
    pd->fields[battle_field]->add_process(PROCESSOR_TURN, 0, 0, 0, turn_player, 0, 0);
    return TRUE;
}
OCGCORE_API void set_player_corpse(intptr_t pduel, uint8_t fieldid, uint8_t playerid)
{
    duel *pd = (duel *)pduel;
    pd->fields[fieldid]->player[playerid].lp = CORPSE_PLAYER_LP;
    pd->fields[fieldid]->not_corpse[playerid] = false;
}
OCGCORE_API int32_t return_field_to_main(intptr_t pduel, uint8_t battle_field)
{
    duel *pd = (duel *)pduel;
    if (!pd)
        return FALSE;
    return pd->return_field_to_main(battle_field) ? TRUE : FALSE;
}
OCGCORE_API void end_duel(intptr_t pduel)
{
    duel *pd = (duel *)pduel;
    if (duel_set.count(pd))
    {
        duel_set.erase(pd);
        delete pd;
    }
}
OCGCORE_API void set_player_info(intptr_t pduel, int32_t playerid, int32_t lp, int32_t startcount, int32_t drawcount)
{
    if (!check_playerid(playerid))
        return;
    duel *pd = (duel *)pduel;
    if (lp > 0)
        pd->start_lp[playerid] = lp;
    if (startcount >= 0)
        pd->start_hand[playerid] = startcount;
    if (drawcount >= 0)
        pd->draw_count[playerid] = drawcount;
}
OCGCORE_API void get_log_message(intptr_t pduel, char *buf)
{
    duel *pd = (duel *)pduel;
    std::strncpy(buf, pd->strbuffer, sizeof pd->strbuffer - 1);
    buf[sizeof pd->strbuffer - 1] = 0;
}
OCGCORE_API int32_t get_message(intptr_t pduel, byte *buf)
{
    int32_t len = ((duel *)pduel)->read_buffer(buf);
    ((duel *)pduel)->clear_buffer();
    return len;
}
OCGCORE_API uint32_t process(intptr_t pduel)
{
    duel *pd = (duel *)pduel;
    int field_idx = pd->get_field_index(pd->game_field);
    uint32_t result = 0;
    do
    {
        result = pd->game_field->process();
    } while ((result & PROCESSOR_BUFFER_LEN) == 0 && (result & PROCESSOR_FLAG) == 0);
    return result;
}
OCGCORE_API void new_card(intptr_t pduel, uint32_t code, uint8_t owner, uint8_t playerid, uint8_t location, uint8_t sequence, uint8_t position, uint8_t fieldid)
{
    if (!check_playerid(owner) || !check_playerid(playerid))
        return;
    duel *ptduel = (duel *)pduel;
    if (fieldid >= duel::FIELD_COUNT)
        return;
    auto *pf = ptduel->fields[fieldid];
    if (pf->is_location_useable(playerid, location, sequence))
    {
        card *pcard = ptduel->new_card(code);
        pcard->owner = owner;
        pf->add_card(playerid, pcard, location, sequence);
        pcard->current.position = position;
        if (!(location & LOCATION_ONFIELD) || (position & POS_FACEUP))
        {
            pcard->enable_field_effect(true);
            pf->adjust_instant();
        }
        if (location & LOCATION_ONFIELD)
        {
            if (location == LOCATION_MZONE)
                pcard->set_status(STATUS_PROC_COMPLETE, TRUE);
        }
    }
}
OCGCORE_API void new_tag_card(intptr_t pduel, uint32_t code, uint8_t owner, uint8_t location, uint8_t fieldid)
{
    duel *ptduel = (duel *)pduel;
    if (owner > 1 || !(location & (LOCATION_DECK | LOCATION_EXTRA)))
        return;
    if (fieldid >= duel::FIELD_COUNT)
        return;
    auto *pf = ptduel->fields[fieldid];
    card *pcard = ptduel->new_card(code);
    switch (location)
    {
    case LOCATION_DECK:
        pf->player[owner].tag_list_main.push_back(pcard);
        pcard->owner = owner;
        pcard->current.controler = owner;
        pcard->current.location = LOCATION_DECK;
        pcard->current.sequence = (uint8_t)pf->player[owner].tag_list_main.size() - 1;
        pcard->current.position = POS_FACEDOWN_DEFENSE;
        break;
    case LOCATION_EXTRA:
        pf->player[owner].tag_list_extra.push_back(pcard);
        pcard->owner = owner;
        pcard->current.controler = owner;
        pcard->current.location = LOCATION_EXTRA;
        pcard->current.sequence = (uint8_t)pf->player[owner].tag_list_extra.size() - 1;
        pcard->current.position = POS_FACEDOWN_DEFENSE;
        break;
    }
}
/**
 * @brief Get card information.
 * @param buf int32_t array
 * @return buffer length in bytes
 */
OCGCORE_API int32_t query_card(intptr_t pduel, uint8_t fieldid, uint8_t playerid, uint8_t location, uint8_t sequence, uint32_t query_flag, byte *buf, int32_t use_cache)
{
    if (!check_playerid(playerid))
        return LEN_FAIL;
    duel *ptduel = (duel *)pduel;
    if (fieldid >= duel::FIELD_COUNT)
        return LEN_FAIL;
    auto *pf = ptduel->fields[fieldid];
    card *pcard = nullptr;
    location &= 0x7f;
    if (location == LOCATION_MZONE || location == LOCATION_SZONE)
        pcard = pf->get_field_card(playerid, location, sequence);
    else
    {
        auto ptr = pf->get_field_vector(playerid, location);
        if (!ptr)
            return LEN_FAIL;
        auto &lst = *ptr;
        if (sequence >= lst.size())
            return LEN_FAIL;
        pcard = lst[sequence];
    }
    if (pcard)
    {
        return pcard->get_infos(buf, query_flag, use_cache);
    }
    else
    {
        buffer_write<int32_t>(buf, LEN_EMPTY);
        return LEN_EMPTY;
    }
}
OCGCORE_API int32_t query_field_count(intptr_t pduel, uint8_t fieldid, uint8_t playerid, uint8_t location)
{
    duel *ptduel = (duel *)pduel;
    if (fieldid >= duel::FIELD_COUNT || !check_playerid(playerid))
        return 0;
    auto *pf = ptduel->fields[fieldid];
    auto &player = pf->player[playerid];
    if (location == LOCATION_HAND)
        return (int32_t)player.list_hand.size();
    if (location == LOCATION_GRAVE)
        return (int32_t)player.list_grave.size();
    if (location == LOCATION_REMOVED)
        return (int32_t)player.list_remove.size();
    if (location == LOCATION_EXTRA)
        return (int32_t)player.list_extra.size();
    if (location == LOCATION_DECK)
        return (int32_t)player.list_main.size();
    if (location == LOCATION_MZONE)
    {
        int32_t count = 0;
        for (auto &pcard : player.list_mzone)
            if (pcard)
                ++count;
        return count;
    }
    if (location == LOCATION_SZONE)
    {
        int32_t count = 0;
        for (auto &pcard : player.list_szone)
            if (pcard)
                ++count;
        return count;
    }
    return 0;
}
OCGCORE_API int32_t query_field_card(intptr_t pduel, uint8_t fieldid, uint8_t playerid, uint8_t location, uint32_t query_flag, byte *buf, int32_t use_cache)
{
    if (!check_playerid(playerid))
        return LEN_FAIL;
    duel *ptduel = (duel *)pduel;
    if (fieldid >= duel::FIELD_COUNT)
        return LEN_FAIL;
    auto *pf = ptduel->fields[fieldid];
    auto &player = pf->player[playerid];
    byte *p = buf;
    if (location == LOCATION_MZONE)
    {
        for (auto &pcard : player.list_mzone)
        {
            if (pcard)
            {
                int32_t clen = pcard->get_infos(p, query_flag, use_cache);
                p += clen;
            }
            else
            {
                buffer_write<int32_t>(p, LEN_EMPTY);
            }
        }
    }
    else if (location == LOCATION_SZONE)
    {
        for (auto &pcard : player.list_szone)
        {
            if (pcard)
            {
                int32_t clen = pcard->get_infos(p, query_flag, use_cache);
                p += clen;
            }
            else
            {
                buffer_write<int32_t>(p, LEN_EMPTY);
            }
        }
    }
    else
    {
        card_vector *lst = nullptr;
        if (location == LOCATION_HAND)
            lst = &player.list_hand;
        else if (location == LOCATION_GRAVE)
            lst = &player.list_grave;
        else if (location == LOCATION_REMOVED)
            lst = &player.list_remove;
        else if (location == LOCATION_EXTRA)
            lst = &player.list_extra;
        else if (location == LOCATION_DECK)
            lst = &player.list_main;
        else
            return LEN_FAIL;
        for (auto &pcard : *lst)
        {
            int32_t clen = pcard->get_infos(p, query_flag, use_cache);
            p += clen;
        }
    }
    return (int32_t)(p - buf);
}
OCGCORE_API int32_t query_field_info(intptr_t pduel, uint8_t fieldid, byte *buf)
{
    duel *ptduel = (duel *)pduel;
    if (fieldid >= duel::FIELD_COUNT)
        return LEN_FAIL;
    auto *pf = ptduel->fields[fieldid];
    byte *p = buf;
    *p++ = MSG_RELOAD_FIELD;
    *p++ = (uint8_t)pf->core.duel_rule;
    for (int playerid = 0; playerid < 2; ++playerid)
    {
        auto &player = pf->player[playerid];
        buffer_write<int32_t>(p, player.lp);
        for (auto &pcard : player.list_mzone)
        {
            if (pcard)
            {
                *p++ = 1;
                *p++ = pcard->current.position;
                *p++ = (uint8_t)pcard->xyz_materials.size();
            }
            else
            {
                *p++ = 0;
            }
        }
        for (auto &pcard : player.list_szone)
        {
            if (pcard)
            {
                *p++ = 1;
                *p++ = pcard->current.position;
            }
            else
            {
                *p++ = 0;
            }
        }
        *p++ = (uint8_t)player.list_main.size();
        *p++ = (uint8_t)player.list_hand.size();
        *p++ = (uint8_t)player.list_grave.size();
        *p++ = (uint8_t)player.list_remove.size();
        *p++ = (uint8_t)player.list_extra.size();
        *p++ = (uint8_t)player.extra_p_count;
    }
    *p++ = (uint8_t)pf->core.current_chain.size();
    for (const auto &ch : pf->core.current_chain)
    {
        effect *peffect = ch.triggering_effect;
        buffer_write<uint32_t>(p, peffect->get_handler()->data.code);
        buffer_write<uint32_t>(p, peffect->get_handler()->get_info_location());
        *p++ = ch.triggering_controler;
        *p++ = (uint8_t)ch.triggering_location;
        *p++ = ch.triggering_sequence;
        buffer_write<uint32_t>(p, peffect->description);
    }
    return (int32_t)(p - buf);
}
OCGCORE_API void set_responsei(intptr_t pduel, int32_t value)
{
    ((duel *)pduel)->set_responsei(value);
}
OCGCORE_API void set_responseb(intptr_t pduel, byte *buf)
{
    ((duel *)pduel)->set_responseb(buf);
}
OCGCORE_API void set_active_field(intptr_t pduel, uint8_t field_idx)
{
    duel *pd = (duel *)pduel;
    if (field_idx < pd->FIELD_COUNT)
    {
        int old_idx = pd->get_field_index(pd->game_field);
        // 保存旧 field 的 PendulumChecklist（从 Lua 全局读回，包含脚本的修改）
        if (old_idx < pd->FIELD_COUNT)
            pd->fields[old_idx]->infos.pendulum_checklist =
                pd->lua->get_global_int("Auxiliary", "PendulumChecklist");
        pd->game_field = pd->fields[field_idx];
        // 恢复新 field 的 PendulumChecklist（写入 Lua 全局）
        pd->lua->set_global_int("Auxiliary", "PendulumChecklist",
                                pd->fields[field_idx]->infos.pendulum_checklist);
        // 快照/恢复 c{code}.* 守卫字段（M2.10：per-field 隔离）
        for (auto &kv : pd->guard_states)
        {
            uint64_t key = kv.first;
            uint8_t &mask = kv.second;
            uint32_t code = (uint32_t)(key >> 8);
            int fi = key & 0xff;
            // 读回旧 field 的守卫值（从 Lua 全局）
            char class_name[20];
            sprintf(class_name, "c%d", code);
            lua_getglobal(pd->lua->lua_state, class_name);                     // +1 c{code}
            lua_getfield(pd->lua->lua_state, -1, duel::GUARD_FIELD_NAMES[fi]); // +1 value
            bool current_val = lua_toboolean(pd->lua->lua_state, -1);
            lua_pop(pd->lua->lua_state, 2); // -2
            // 更新 mask 中旧 field 的 bit
            if (current_val)
                mask |= (1 << old_idx);
            else
                mask &= ~(1 << old_idx);
            // 写入新 field 的守卫值到 Lua 全局
            bool new_val = (mask >> field_idx) & 1;
            lua_getglobal(pd->lua->lua_state, class_name);                     // +1 c{code}
            lua_pushboolean(pd->lua->lua_state, new_val);                      // +1 bool
            lua_setfield(pd->lua->lua_state, -2, duel::GUARD_FIELD_NAMES[fi]); // -1
            lua_pop(pd->lua->lua_state, 1);                                    // -1
        }
        // 快照/恢复 Auxiliary.* 表/对象字段（M2.10：per-field 隔离）
        for (int i = 0; i < duel::AUX_TABLE_FIELD_COUNT; ++i)
        {
            const char *name = duel::AUX_TABLE_FIELD_NAMES[i];
            // 保存旧 field 的当前值到 registry ref
            if (old_idx < pd->FIELD_COUNT)
            {
                if (pd->aux_table_refs[i][old_idx] != LUA_NOREF)
                    pd->lua->free_global_ref(pd->aux_table_refs[i][old_idx]);
                pd->aux_table_refs[i][old_idx] = pd->lua->save_global_ref("Auxiliary", name);
            }
            // 恢复新 field 的值
            if (pd->aux_table_refs[i][field_idx] != LUA_NOREF)
                pd->lua->restore_global_ref("Auxiliary", name, pd->aux_table_refs[i][field_idx]);
            else
                pd->lua->set_global_nil("Auxiliary", name);
        }
        // 快照/恢复 Auxiliary.* 布尔字段（M2.10：per-field 隔离）
        for (int i = 0; i < duel::AUX_BOOL_FIELD_COUNT; ++i)
        {
            const char *name = duel::AUX_BOOL_FIELD_NAMES[i];
            if (old_idx < pd->FIELD_COUNT)
                pd->aux_bool_values[i][old_idx] = pd->lua->get_global_int("Auxiliary", name) ? 1 : 0;
            pd->lua->set_global_int("Auxiliary", name, pd->aux_bool_values[i][field_idx]);
        }
        fprintf(stderr, "[DEBUG] set_active_field: %d->%d new game_field=%p temp_card=%p\n", old_idx, (int)field_idx, (void *)pd->game_field, (void *)pd->game_field->temp_card);
    }
}
OCGCORE_API int32_t preload_script(intptr_t pduel, const char *script_name)
{
    return ((duel *)pduel)->lua->load_script(script_name);
}
