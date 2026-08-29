/*
 * duel.cpp
 *
 *  Created on: 2010-5-2
 *      Author: Argon
 */

#include <cstdio>
#include <cstring>
#include "duel.h"
#include "interpreter.h"
#include "field.h"
#include "card.h"
#include "effect.h"
#include "group.h"
#include "ocgapi.h"
#include "buffer.h"

duel::duel()
{
    lua = new interpreter(this, false);
    init_fields();
    message_buffer.reserve(SIZE_MESSAGE_BUFFER);
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
card *duel::new_card(uint32_t code)
{
    card *pcard = new card(this);
    cards.insert(pcard);
    if (code != TEMP_CARD_ID)
        ::read_card(code, &(pcard->data));
    pcard->data.code = code;
    lua->register_card(pcard);
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
        card *pcard = new_card(src->data.code);
        pcard->owner = dst_player == 1 ? 1-src->owner : src->owner;
        pf->add_card(dst_player, pcard, location, seq);
        pcard->current.position = pos;
        pcard->home_origin = src;
        // 克隆超量素材：素材卡处于 LOCATION_OVERLAY，不属于任何区域向量，随怪兽一并克隆
        for (auto &mat : src->xyz_materials)
        {
            card *mcard = new_card(mat->data.code);
            mcard->owner = dst_player == 1 ? 1-mat->owner : mat->owner; // 克隆素材的控制权与原素材相同
            mcard->home_origin = mat;
            mcard->current.controler = PLAYER_NONE;
            mcard->current.location = LOCATION_OVERLAY;
            mcard->current.sequence = (uint8_t)pcard->xyz_materials.size();
            mcard->overlay_target = pcard;
            pcard->xyz_materials.push_back(mcard);
        }
        return pcard;
    }
    // 不克隆 effect，不调用 enable_field_effect / adjust_all
    return nullptr;
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
