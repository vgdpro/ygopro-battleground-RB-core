/*
 * interface.cpp
 *
 *  Created on: 2010-5-2
 *      Author: Argon
 */
#include <cstdio>
#include <cstring>
#include <set>
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
static uint32_t default_message_handler(intptr_t pduel, uint32_t message_type)
{
    return 0;
}
static script_reader sreader = default_script_reader;
static card_reader creader = default_card_reader;
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

    // 向 fields[0]/[1] 各推 PROCESSOR_START + PROCESSOR_TURN
    for (int i = 0; i < 2; ++i)
    {
        pd->fields[i]->add_process(PROCESSOR_START, 0, 0, 0, 0, 0);
        pd->fields[i]->add_process(PROCESSOR_TURN, 0, 0, 0, 0, 0);
    }
}
OCGCORE_API void merge_field_to_bp(intptr_t pduel){
    duel *pd = (duel *)pduel;
    // 步骤1: 清理旧战斗场的卡牌和效果
    pd->fields[2]->clear();
    // 步骤2: 释放旧 field 对象，创建全新的 field
    delete pd->fields[2];
    pd->fields[2] = new field(pd);
    // 步骤3: 无条件设置 temp_card
    pd->fields[2]->temp_card = pd->new_card(TEMP_CARD_ID);
    // 步骤4: 从家园场同步 LP
    pd->fields[2]->player[0].lp = pd->fields[0]->player[0].lp;
    pd->fields[2]->player[1].lp = pd->fields[1]->player[0].lp;
    // 步骤5: 切换活跃场
    pd->game_field = pd->fields[2];
    // 步骤6: 推送处理器
    pd->fields[2]->add_process(PROCESSOR_START, 0, 0, 0, 0, 0);
    pd->fields[2]->add_process(PROCESSOR_TURN, 0, 0, 0, 0, 0);
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
    int field_idx = (pd->game_field == pd->fields[0]) ? 0 : (pd->game_field == pd->fields[1]) ? 1 : 2;
    fprintf(stderr, "[DEBUG] process() field[%d] game_field=%p temp_card=%p\n", field_idx, (void *)pd->game_field, (void *)pd->game_field->temp_card);
    fflush(stderr);
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
        int old_idx = (pd->game_field == pd->fields[0]) ? 0 : (pd->game_field == pd->fields[1]) ? 1
                                                                                                : 2;
        pd->game_field = pd->fields[field_idx];
        fprintf(stderr, "[DEBUG] set_active_field: %d->%d new game_field=%p temp_card=%p\n", old_idx, (int)field_idx, (void *)pd->game_field, (void *)pd->game_field->temp_card);
    }
}
OCGCORE_API int32_t preload_script(intptr_t pduel, const char *script_name)
{
    return ((duel *)pduel)->lua->load_script(script_name);
}
