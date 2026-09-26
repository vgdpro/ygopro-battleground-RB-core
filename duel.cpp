/*
 * duel.cpp
 *
 *  Created on: 2010-5-2
 *      Author: Argon
 */

#include <cstdio>
#include <cstring>
#include <unordered_map>
#include "duel.h"
#include "interpreter.h"
#include "field.h"
#include "card.h"
#include "effect.h"
#include "group.h"
#include "ocgapi.h"
#include "buffer.h"

// Lua 一次性守卫字段名列表（从全量 13417 个 Lua 脚本审计提取）
const char *duel::GUARD_FIELD_NAMES[] = {
    "global_check",
    "global_flag",
    "globle_check",
    "counter",
    "is_empty",
    "check",
    "discard",
    "gf"};

// Auxiliary.* per-field 隔离字段名列表（M2.10）
const char *duel::AUX_TABLE_FIELD_NAMES[] = {
    "ExtraDeckSummonCountLimit",
    "merge_single_effect_codes",
    "SubGroupCaptured"};

const char *duel::AUX_BOOL_FIELD_NAMES[] = {
    "MulcharmyGlobalFlag",
    "merge_single_global_check"};

namespace
{
    // 清理战斗场克隆对象对家园场原对象的反向引用，避免家园场持久对象悬垂
    template <typename T>
    void clear_home_clone(const std::unordered_set<T *> &objects)
    {
        for (auto *obj : objects)
        {
            if (obj->home_origin)
                obj->home_origin->home_clone = nullptr;
        }
    }
}

duel::duel()
{
    lua = new interpreter(this, false);
    init_fields();
    message_buffer.reserve(SIZE_MESSAGE_BUFFER);
    // 初始化 Auxiliary per-field 隔离存储（M2.10）
    for (int i = 0; i < AUX_TABLE_FIELD_COUNT; ++i)
        for (int j = 0; j < FIELD_COUNT; ++j)
            aux_table_refs[i][j] = LUA_NOREF;
#ifdef _WIN32
    _set_error_mode(_OUT_TO_MSGBOX);
#endif // _WIN32
}
void duel::init_fields()
{
    for (int i = 0; i < FIELD_COUNT; ++i)
        init_field(i);
    game_field = fields[0];
}
duel::~duel()
{
    // 释放 Auxiliary per-field 隔离的 registry ref（M2.10）
    for (int i = 0; i < AUX_TABLE_FIELD_COUNT; ++i)
        for (int j = 0; j < FIELD_COUNT; ++j)
            if (aux_table_refs[i][j] != LUA_NOREF)
                lua->free_global_ref(aux_table_refs[i][j]);
    for (auto &pcard : cards)
        delete pcard;
    for (auto &pgroup : groups)
        delete pgroup;
    for (auto &peffect : effects)
        delete peffect;
    for (int i = 0; i < FIELD_COUNT; ++i)
        delete fields[i];
    delete lua;
}
void duel::clear()
{
    // 释放 Auxiliary per-field 隔离的 registry ref（M2.10）
    for (int i = 0; i < AUX_TABLE_FIELD_COUNT; ++i)
        for (int j = 0; j < FIELD_COUNT; ++j)
            if (aux_table_refs[i][j] != LUA_NOREF)
            {
                lua->free_global_ref(aux_table_refs[i][j]);
                aux_table_refs[i][j] = LUA_NOREF;
            }
    for (auto &pcard : cards)
    {
        lua->unregister_card(pcard);
        delete pcard;
    }
    for (auto &pgroup : groups)
    {
        lua->unregister_group(pgroup);
        delete pgroup;
    }
    for (auto &peffect : effects)
    {
        lua->unregister_effect(peffect);
        delete peffect;
    }
    for (int i = 0; i < FIELD_COUNT; ++i)
        delete fields[i];
    cards.clear();
    groups.clear();
    effects.clear();
    assumes.clear();
    sgroups.clear();
    uncopy.clear();
    next_card_id = 1;
    for (int i = 0; i < BATTLE_FIELD_COUNT; ++i)
        battle_field_bindings[i] = BattleFieldBinding{};
    init_fields();
}
card *duel::new_card(uint32_t code, bool register_with_lua)
{
    card *pcard = new card(this);
    cards.insert(pcard);
    if (code != TEMP_CARD_ID)
        ::read_card(code, &(pcard->data));
    pcard->data.code = code;
    lua->register_card(pcard, register_with_lua);
    return pcard;
}
void duel::init_field(uint8_t field_index)
{
    fields[field_index] = new field(this);
    fields[field_index]->temp_card = new_card(TEMP_CARD_ID);
    fields[field_index]->not_corpse[1] = false;
}
bool duel::is_home_field_index(uint8_t field_index)
{
    return field_index < HOME_FIELD_COUNT;
}
bool duel::is_battle_field_index(uint8_t field_index)
{
    return field_index >= HOME_FIELD_COUNT && field_index < FIELD_COUNT;
}
bool duel::is_battle_field(const field *pfield) const
{
    const uint8_t field_index = get_field_index(pfield);
    return is_battle_field_index(field_index);
}
bool duel::is_corpse_player(const field *pfield, uint8_t playerid) const
{
    return playerid <= 1 && !pfield->not_corpse[playerid];
}
uint8_t duel::get_field_index(const field *pfield) const
{
    for (uint8_t field_index = 0; field_index < FIELD_COUNT; ++field_index)
    {
        if (fields[field_index] == pfield)
            return field_index;
    }
    return INVALID_FIELD_INDEX;
}
bool duel::bind_battle_field(uint8_t battle_field, uint8_t home_field_p0, uint8_t home_field_p1)
{
    if (!is_battle_field_index(battle_field) || !is_home_field_index(home_field_p0) || !is_home_field_index(home_field_p1) || home_field_p0 == home_field_p1)
        return false;
    BattleFieldBinding &binding = battle_field_bindings[battle_field - HOME_FIELD_COUNT];
    binding.home_fields[0] = home_field_p0;
    binding.home_fields[1] = home_field_p1;
    return true;
}
bool duel::reset_field(uint8_t field_index)
{
    if (!is_battle_field_index(field_index))
        return false;
    field *old_field = fields[field_index];
    if (!old_field)
        return false;
    const bool was_active = game_field == old_field;
    // 清理战斗场克隆对象对家园场原对象的反向引用，避免家园场持久对象悬垂
    clear_home_clone(cards);
    clear_home_clone(effects);
    clear_home_clone(groups);
    old_field->clear();
    if (old_field->temp_card)
        delete_card(old_field->temp_card);
    delete old_field;
    init_field(field_index);
    battle_field_bindings[field_index - HOME_FIELD_COUNT] = BattleFieldBinding{};
    if (was_active)
        game_field = fields[field_index];
    return true;
}
card *duel::clone_card_to_field(card *src, uint8_t dst_player,
                                uint8_t location, uint8_t seq, uint8_t pos, uint8_t battle_field)
{
    if (!src || !is_battle_field_index(battle_field) || dst_player > 1)
        return nullptr;
    auto *pf = fields[battle_field];
    if (pf->is_location_useable(dst_player, location, seq))
    {
        card *pcard = new_card(src->data.code, false);
        pcard->owner = dst_player == 1 ? 1 - src->owner : src->owner;
        pf->add_card(dst_player, pcard, location, seq);
        pcard->current.position = pos;
        pcard->home_origin = src;
        src->home_clone = pcard; // 反向绑定：家园场原卡 → 战斗场克隆卡
        // 克隆运行时状态（A/C/D/E 组标量 + B 组指针/容器，unique_function 除外）
        clone_card_state(src, pcard);
        // 克隆超量素材：素材卡处于 LOCATION_OVERLAY，不属于任何区域向量，随怪兽一并克隆
        for (auto &mat : src->xyz_materials)
        {
            card *mcard = new_card(mat->data.code, false);
            mcard->owner = dst_player == 1 ? 1 - mat->owner : mat->owner; // 克隆素材的控制权与原素材相同
            mcard->home_origin = mat;
            mat->home_clone = mcard; // 反向绑定：家园场素材卡 → 战斗场克隆素材卡
            clone_card_state(mat, mcard);
            // clone_card_state 复制了 B 组的 overlay_target（指向家园场怪兽），此处覆盖为战斗场克隆怪兽
            mcard->current.controler = PLAYER_NONE;
            mcard->current.location = LOCATION_OVERLAY;
            mcard->current.sequence = (uint8_t)pcard->xyz_materials.size();
            mcard->overlay_target = pcard;
            pcard->xyz_materials.push_back(mcard);
        }
        return pcard;
    }
    return nullptr;
}
// 复制卡牌运行时状态
// A 组 current/previous/spsummon、C 组计数器、D 组唯一性标量、E 组其他标量；
// B 组指针/容器（equiping_target/equiping_cards/effect_target_* 等）原样复制后由
// remap_card_references 映射到战斗场克隆对象；unique_function 与卡→effect 引用由
// remap_card_references / remap_card_effect_references 单独处理。
void duel::clone_card_state(card *src, card *dst)
{
    // A组：运行时状态
    dst->current.code = src->current.code;
    dst->current.code2 = src->current.code2;
    dst->current.setcode = src->current.setcode;
    dst->current.type = src->current.type;
    dst->current.level = src->current.level;
    dst->current.rank = src->current.rank;
    dst->current.link = src->current.link;
    dst->current.lscale = src->current.lscale;
    dst->current.rscale = src->current.rscale;
    dst->current.attribute = src->current.attribute;
    dst->current.race = src->current.race;
    dst->current.attack = src->current.attack;
    dst->current.defense = src->current.defense;
    dst->current.base_attack = src->current.base_attack;
    dst->current.base_defense = src->current.base_defense;
    dst->current.pzone = src->current.pzone;
    dst->previous = src->previous;
    dst->spsummon = src->spsummon;
    // C组：攻击/回合计数器
    dst->attack_announce_count = src->attack_announce_count;
    dst->attacked_count = src->attacked_count;
    dst->announce_count = src->announce_count;
    dst->direct_attackable = src->direct_attackable;
    dst->attack_all_target = src->attack_all_target;
    dst->attack_controler = src->attack_controler;
    dst->turnid = src->turnid;
    dst->turn_counter = src->turn_counter;
    dst->spsummon_counter[0] = src->spsummon_counter[0];
    dst->spsummon_counter[1] = src->spsummon_counter[1];
    dst->spsummon_code = src->spsummon_code;
    // D组：唯一性追踪（unique_function 除外）
    dst->unique_pos[0] = src->unique_pos[0];
    dst->unique_pos[1] = src->unique_pos[1];
    dst->unique_fieldid = src->unique_fieldid;
    dst->unique_code = src->unique_code;
    dst->unique_location = src->unique_location;
    // E组：其他标量
    dst->fieldid = src->fieldid;
    dst->fieldid_r = src->fieldid_r;
    dst->activate_count_id = src->activate_count_id;
    dst->assume_type = src->assume_type;
    dst->assume_value = src->assume_value;
    dst->summon_info = src->summon_info;
    dst->summon_player = src->summon_player;
    dst->xyz_materials_previous_count_onfield = src->xyz_materials_previous_count_onfield;
    dst->status = src->status;
    // 值类型容器直接复制
    dst->counters = src->counters;
    dst->indestructable_effects = src->indestructable_effects;
    // B组：指针引用与容器
    // 此处原样复制家园场指针，随后由 remap_card_references 映射到战斗场克隆对象；
    // 卡→卡映射必须在 clone_effects_to_field 之前完成，因为 card::add_effect 依赖
    // equiping_target / effect_target_cards 建立 disable 检查目标。
    dst->equiping_target = src->equiping_target;
    dst->pre_equip_target = src->pre_equip_target;
    dst->overlay_target = src->overlay_target;
    dst->relations = src->relations;
    dst->announced_cards = src->announced_cards;
    dst->attacked_cards = src->attacked_cards;
    dst->battled_cards = src->battled_cards;
    dst->equiping_cards = src->equiping_cards;
    dst->material_cards = src->material_cards;
    dst->effect_target_owner = src->effect_target_owner;
    dst->effect_target_cards = src->effect_target_cards;
}
void duel::clone_effects_to_field(uint8_t battle_field, uint8_t home_field_p0, uint8_t home_field_p1,
                                  const std::unordered_map<card *, card *> &card_map)
{
    field *battle = fields[battle_field];
    card *temp_p0 = fields[home_field_p0]->temp_card;
    card *temp_p1 = fields[home_field_p1]->temp_card;
    // 遍历所有 effect，克隆属于家园场卡牌（含 temp_card）的效果
    auto it = effects.begin();
    while (it != effects.end())
    {
        effect *pe = *it;
        ++it;
        // 判断 effect 是否属于被克隆的家园场卡牌或 temp_card
        bool owner_in_home = card_map.count(pe->owner) || pe->owner == temp_p0 || pe->owner == temp_p1;
        bool handler_in_home = card_map.count(pe->handler) || pe->handler == temp_p0 || pe->handler == temp_p1;
        if (!owner_in_home && !handler_in_home)
            continue;
        if (pe->is_flag(EFFECT_FLAG_UNCOPYABLE))
            continue;
        effect *ceffect = pe->clone();
        // 双向绑定：原 effect ↔ 克隆 effect
        ceffect->home_origin = pe;
        pe->home_clone = ceffect;
        // 重绑 owner：temp_card → 战斗场 temp_card，普通卡牌 → card_map 映射
        if (pe->owner == temp_p0 || pe->owner == temp_p1)
            ceffect->owner = battle->temp_card;
        else
        {
            auto oit = card_map.find(pe->owner);
            if (oit != card_map.end())
                ceffect->owner = oit->second;
        }
        // 重绑 handler：同上逻辑
        if (pe->handler == temp_p0 || pe->handler == temp_p1)
            ceffect->handler = battle->temp_card;
        else if (pe->handler)
        {
            auto hit = card_map.find(pe->handler);
            if (hit != card_map.end())
                ceffect->handler = hit->second;
        }
        // temp_card 效果注册到战斗 field；普通卡牌效果通过 card::add_effect 挂载
        if (ceffect->owner == battle->temp_card)
        {
            uint8_t owner_player = ceffect->effect_owner;
            if (owner_player == PLAYER_NONE)
                owner_player = ceffect->get_owner_player();
            battle->add_effect(ceffect, owner_player);
        }
        else
        {
            card *target_card = ceffect->handler ? ceffect->handler : ceffect->owner;
            if (target_card)
                target_card->add_effect(ceffect);
        }
    }
}
// 全量克隆所有 group 到 battle_field，并建立原 group ↔ 克隆 group 的双向绑定
// 每个 group 的 container 成员通过 card_map 映射到战斗场克隆卡；
// 不在 card_map 中的卡（如家园场 temp_card）保持原指针，由调用方保证其生命周期
void duel::clone_groups_to_field(uint8_t battle_field, const std::unordered_map<card *, card *> &card_map)
{
    if (!is_battle_field_index(battle_field))
        return;
    // 遍历全量 group 集合（含调用深度内的 sgroups），为每个 group 创建战斗场克隆
    for (auto *pgroup : groups)
    {
        if (!pgroup || pgroup->home_clone)
            continue;
        group *cgroup = new_group();
        cgroup->is_readonly = pgroup->is_readonly;
        for (auto *pc : pgroup->container)
        {
            auto it = card_map.find(pc);
            if (it != card_map.end())
                cgroup->container.insert(it->second);
            else
                cgroup->container.insert(pc);
        }
        cgroup->it = cgroup->container.begin();
        // 双向绑定：原 group ↔ 克隆 group
        cgroup->home_origin = pgroup;
        pgroup->home_clone = cgroup;
    }
}
void duel::remap_effect_references(const std::unordered_map<card *, card *> &card_map)
{
    for (auto *pe : effects)
    {
        // 只处理克隆 effect（home_origin 非空表示这是战斗场克隆）
        if (!pe->home_origin)
            continue;
        // 重映射 label_object：通过 ref_handle 获取家园场原始对象，再通过 home_clone/card_map 找到战斗场克隆对象
        if (pe->label_object && pe->object_type != PARAM_TYPE_INT)
        {
            void *obj = lua->get_ref_object(pe->label_object);
            if (obj)
            {
                int32_t new_ref = 0;
                if (pe->object_type == PARAM_TYPE_CARD)
                {
                    auto *orig_card = static_cast<card *>(obj);
                    auto it = card_map.find(orig_card);
                    if (it != card_map.end())
                        new_ref = it->second->ref_handle;
                }
                else if (pe->object_type == PARAM_TYPE_EFFECT)
                {
                    auto *orig_effect = static_cast<effect *>(obj);
                    if (orig_effect->home_clone)
                        new_ref = orig_effect->home_clone->ref_handle;
                }
                else if (pe->object_type == PARAM_TYPE_GROUP)
                {
                    auto *orig_group = static_cast<group *>(obj);
                    if (orig_group->home_clone)
                        new_ref = orig_group->home_clone->ref_handle;
                }
                if (new_ref)
                    pe->label_object = new_ref;
            }
        }
        // 重映射 required_handorset_effects：每个 effect* 通过 home_clone 替换
        for (auto &eptr : pe->required_handorset_effects)
        {
            if (eptr && eptr->home_clone)
                eptr = eptr->home_clone;
        }
        // 重映射 active_handler：通过 card_map 替换
        if (pe->active_handler)
        {
            auto it = card_map.find(pe->active_handler);
            if (it != card_map.end())
                pe->active_handler = it->second;
        }
        // 重映射 last_handler：通过 card_map 替换
        if (pe->last_handler)
        {
            auto it = card_map.find(pe->last_handler);
            if (it != card_map.end())
                pe->last_handler = it->second;
        }
    }
}
// 重映射克隆卡牌的 B 组卡→卡指针引用和 unique_function
// 必须在全部卡牌克隆完毕后、克隆 effect 之前执行：card::add_effect 注册 EFFECT_TYPE_EQUIP /
// EFFECT_TYPE_TARGET 效果时会读取 equiping_target / effect_target_cards 建立 disable 检查目标。
void duel::remap_card_references(const std::unordered_map<card *, card *> &card_map)
{
    // 单指针重映射：不在映射中的保持原指针
    auto remap_card_ptr = [&card_map](card *&ptr)
    {
        if (!ptr)
            return;
        auto it = card_map.find(ptr);
        if (it != card_map.end())
            ptr = it->second;
    };
    // 重映射 card_set 容器：每个 card* 通过 card_map 替换，不在映射中的保持原指针
    auto remap_card_set = [&card_map](card_set &cset)
    {
        card_set remapped;
        for (auto *pc : cset)
        {
            auto it = card_map.find(pc);
            remapped.insert(it != card_map.end() ? it->second : pc);
        }
        cset.swap(remapped);
    };
    // 重映射 attacker_map 容器：每个 pair 中的 card* 通过 card_map 替换
    auto remap_attacker_map = [&card_map](card::attacker_map &amap)
    {
        for (auto &kv : amap)
        {
            auto it = card_map.find(kv.second.first);
            if (it != card_map.end())
                kv.second.first = it->second;
        }
    };
    for (auto *pc : cards)
    {
        // 只处理克隆卡（home_origin 非空表示这是战斗场克隆）
        if (!pc->home_origin)
            continue;
        card *src = pc->home_origin;
        // 重映射单指针：equiping_target / pre_equip_target / overlay_target
        remap_card_ptr(pc->equiping_target);
        remap_card_ptr(pc->pre_equip_target);
        remap_card_ptr(pc->overlay_target);
        // 重映射卡→卡 reason_card（previous/spsummon 由 clone_card_state 整体复制而来）
        remap_card_ptr(pc->previous.reason_card);
        remap_card_ptr(pc->spsummon.reason_card);
        // 重映射 card_set 容器
        remap_card_set(pc->equiping_cards);
        remap_card_set(pc->material_cards);
        remap_card_set(pc->effect_target_owner);
        remap_card_set(pc->effect_target_cards);
        // 重映射 relations：每个 key（card*）通过 card_map 替换
        {
            card::relation_map remapped;
            for (auto &kv : pc->relations)
            {
                auto it = card_map.find(kv.first);
                remapped[it != card_map.end() ? it->second : kv.first] = kv.second;
            }
            pc->relations.swap(remapped);
        }
        // 重映射 attacker_map 容器
        remap_attacker_map(pc->announced_cards);
        remap_attacker_map(pc->attacked_cards);
        remap_attacker_map(pc->battled_cards);
        // 克隆 unique_function：Lua 函数引用需独立复制
        if (src->unique_function)
            pc->unique_function = lua->clone_function_ref(src->unique_function);
    }
}
// 重映射克隆卡牌的卡→effect 引用
// 必须在 effect 克隆与双向绑定完成之后执行（依赖 effect::home_clone）
void duel::remap_card_effect_references()
{
    for (auto *pc : cards)
    {
        // 只处理克隆卡（home_origin 非空表示这是战斗场克隆）
        if (!pc->home_origin)
            continue;
        // 重映射 unique_effect：通过 home_clone 替换
        if (pc->unique_effect && pc->unique_effect->home_clone)
            pc->unique_effect = pc->unique_effect->home_clone;
        // 重映射卡→effect reason_effect（previous/spsummon 由 clone_card_state 整体复制而来）
        if (pc->previous.reason_effect && pc->previous.reason_effect->home_clone)
            pc->previous.reason_effect = pc->previous.reason_effect->home_clone;
        if (pc->spsummon.reason_effect && pc->spsummon.reason_effect->home_clone)
            pc->spsummon.reason_effect = pc->spsummon.reason_effect->home_clone;
    }
}
bool duel::return_field_to_main(uint8_t battle_field)
{
    if (!is_battle_field_index(battle_field))
        return false;
    const BattleFieldBinding &binding = battle_field_bindings[battle_field - HOME_FIELD_COUNT];
    const uint8_t home_field_p0 = binding.home_fields[0];
    const uint8_t home_field_p1 = binding.home_fields[1];
    if (!is_home_field_index(home_field_p0) || !is_home_field_index(home_field_p1))
        return false;
    field *battle = fields[battle_field];
    field *home_p0 = fields[home_field_p0];
    field *home_p1 = fields[home_field_p1];
    // 将活人侧 LP 按绑定关系镜像回家园场（尸体侧不写回，死血写回 23333）
    for (int player = 0; player < 2; ++player)
    {
        if (battle->not_corpse[0])
            home_p0->player[player].lp = (battle->player[player].lp > 0) ? battle->player[player].lp : CORPSE_PLAYER_LP;
        if (battle->not_corpse[1])
            home_p1->player[player].lp = (battle->player[1 - player].lp > 0) ? battle->player[1 - player].lp : CORPSE_PLAYER_LP;
    }
    return true;
}
group *duel::register_group(group *pgroup)
{
    groups.insert(pgroup);
    if (lua->call_depth)
        sgroups.insert(pgroup);
    lua->register_group(pgroup);
    return pgroup;
}
group *duel::new_group()
{
    group *pgroup = new group(this);
    return register_group(pgroup);
}
group *duel::new_group(card *pcard)
{
    group *pgroup = new group(this, pcard);
    return register_group(pgroup);
}
group *duel::new_group(const card_set &cset)
{
    group *pgroup = new group(this, cset);
    return register_group(pgroup);
}
effect *duel::new_effect()
{
    effect *peffect = new effect(this);
    effects.insert(peffect);
    lua->register_effect(peffect);
    return peffect;
}
void duel::delete_card(card *pcard)
{
    lua->unregister_card(pcard);
    cards.erase(pcard);
    delete pcard;
}
void duel::delete_group(group *pgroup)
{
    lua->unregister_group(pgroup);
    groups.erase(pgroup);
    sgroups.erase(pgroup);
    delete pgroup;
}
void duel::delete_effect(effect *peffect)
{
    lua->unregister_effect(peffect);
    uncopy.erase(peffect);
    effects.erase(peffect);
    delete peffect;
}
int32_t duel::read_buffer(byte *buf)
{
    auto size = buffer_size();
    if (size)
        std::memcpy(buf, message_buffer.data(), size);
    return (int32_t)size;
}
void duel::release_script_group()
{
    for (auto &pgroup : sgroups)
    {
        if (pgroup->is_readonly == GTYPE_DEFAULT)
        {
            lua->unregister_group(pgroup);
            groups.erase(pgroup);
            delete pgroup;
        }
    }
    sgroups.clear();
}
void duel::restore_assumes()
{
    for (auto &pcard : assumes)
        pcard->assume_type = 0;
    assumes.clear();
}
void duel::write_buffer(const void *data, size_t size)
{
    vector_write_block(message_buffer, data, size);
}
void duel::write_buffer32(uint32_t value)
{
    vector_write<uint32_t>(message_buffer, value);
}
void duel::write_buffer16(uint16_t value)
{
    vector_write<uint16_t>(message_buffer, value);
}
void duel::write_buffer8(uint8_t value)
{
    vector_write<unsigned char>(message_buffer, value);
}
void duel::clear_buffer()
{
    message_buffer.clear();
}
void duel::set_responsei(int32_t resp)
{
    game_field->returns.ivalue[0] = resp;
}
void duel::set_responseb(byte *resp)
{
    std::memcpy(game_field->returns.bvalue, resp, SIZE_RETURN_VALUE);
}
int32_t duel::get_next_integer(int32_t l, int32_t h)
{
    if (rng_version == 1)
        return random.get_random_integer_v1(l, h);
    return random.get_random_integer_v2(l, h);
}
