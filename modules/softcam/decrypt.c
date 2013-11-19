/*
 * Astra Module: SoftCAM
 * http://cesbo.com/astra
 *
 * Copyright (C) 2012-2013, Andrey Dyldin <and@cesbo.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Module Name:
 *      decrypt
 *
 * Module Options:
 *      upstream    - object, stream instance returned by module_instance:stream()
 *      name        - string, channel name
 *      biss        - string, BISS key, 16 chars length. example: biss = "1122330044556600"
 *      cam         - object, cam instance returned by cam_module_instance:cam()
 *      cas_data    - string, additional paramters for CAS
 */

#include <astra.h>
#include "module_cam.h"
#include "cas/cas_list.h"
#ifdef FFDECSA
#include "FFdecsa/FFdecsa.h"
#endif
#ifdef DVBCSA
#include "libdvbcsa/dvbcsa/dvbcsa.h"
#endif

struct module_data_t
{
    MODULE_LUA_DATA();
    MODULE_STREAM_DATA();
    MODULE_DECRYPT_DATA();

    /* Config */
    const char *name;
    int caid;
    int ecm_pid;
    int ecm_swap_time;
    int algo;
    int reload_delay;

    int ecm_pid_fails;
    int64_t ecm_pid_delay;

    /* Buffer */
    uint8_t *buffer; // r_buffer + s_buffer
    uint8_t *r_buffer;
    uint8_t *s_buffer;
    size_t buffer_skip;

    /* Descambling */
    bool is_keys;
    uint8_t **cluster;
    size_t cluster_size;
    size_t cluster_size_bytes;
#ifdef FFDECSA
    void *ffdecsa;
#endif
#ifdef DVBCSA
    struct dvbcsa_bs_key_s *libdvbcsa_key_even;
    struct dvbcsa_bs_key_s *libdvbcsa_key_odd;
    struct dvbcsa_bs_batch_s *libdvbcsa_tsbbatch_even;
    struct dvbcsa_bs_batch_s *libdvbcsa_tsbbatch_odd;
    int libdvbcsa_fill;
    int libdvbcsa_fill_even;
    int libdvbcsa_fill_odd;
#endif

    int new_key_id; // 0 - not, 1 - first key, 2 - second key
    uint8_t new_key[16];

    /* Base */
    mpegts_psi_t *pat;
    mpegts_psi_t *cat;
    mpegts_psi_t *pmt;
    mpegts_psi_t *custom_pmt;
    mpegts_psi_t *em;

    mpegts_packet_type_t stream[MAX_PID];

    bool force;
};

#define MSG(_msg) "[decrypt %s] " _msg, mod->name

static module_cas_t * module_decrypt_cas_init(module_data_t *mod)
{
    for(int i = 0; cas_init_list[i]; ++i)
    {
        module_cas_t *cas = cas_init_list[i](&mod->__decrypt);
        if(cas)
            return cas;
    }
    return NULL;
}

static void module_decrypt_cas_destroy(module_data_t *mod)
{
    if(!mod->__decrypt.cas)
        return;
    free(mod->__decrypt.cas->self);
    mod->__decrypt.cas = NULL;
}

static void stream_reload(module_data_t *mod)
{
    memset(mod->stream, 0, sizeof(mod->stream));

    mod->stream[0] = MPEGTS_PACKET_PAT;
    mod->stream[1] = MPEGTS_PACKET_CAT;

    mod->pat->crc32 = 0;
    mod->cat->crc32 = 0;
    mod->pmt->crc32 = 0;
    
    mod->force = false;

    module_decrypt_cas_destroy(mod);
}

/*
 * oooooooooo   o   ooooooooooo
 *  888    888 888  88  888  88
 *  888oooo88 8  88     888
 *  888      8oooo88    888
 * o888o   o88o  o888o o888o
 *
 */

static void on_pat(void *arg, mpegts_psi_t *psi)
{
    module_data_t *mod = arg;

    // check changes
    const uint32_t crc32 = PSI_GET_CRC32(psi);
    if(crc32 == psi->crc32)
        return;

    // check crc
    if(crc32 != PSI_CALC_CRC32(psi))
    {
        asc_log_error(MSG("PAT checksum mismatch"));
        return;
    }

    // reload stream
    if(psi->crc32 != 0)
    {
        asc_log_warning(MSG("PAT changed. Reload stream info"));
        stream_reload(mod);
    }

    psi->crc32 = crc32;

    const uint8_t *pointer = PAT_ITEMS_FIRST(psi);
    while(!PAT_ITEMS_EOL(psi, pointer))
    {
        const uint16_t pnr = PAT_ITEMS_GET_PNR(psi, pointer);
        if(pnr)
        {
            mod->__decrypt.pnr = pnr;
            const uint16_t pmt_pid = PAT_ITEMS_GET_PID(psi, pointer);
            mod->stream[pmt_pid] = MPEGTS_PACKET_PMT;
            mod->pmt->pid = pmt_pid;
            break;
        }
        PAT_ITEMS_NEXT(psi, pointer);
    }

    if(mod->__decrypt.cam && mod->__decrypt.cam->is_ready)
    {
        mod->__decrypt.cas = module_decrypt_cas_init(mod);
        asc_assert(mod->__decrypt.cas != NULL, "CAS with CAID:0x%04X not found", mod->caid);

        mod->cat->crc32 = 0;
        mod->pmt->crc32 = 0;

        for(int i = 0; i < MAX_PID; ++i)
        {
            if(mod->stream[i] & MPEGTS_PACKET_CA)
                mod->stream[i] = MPEGTS_PACKET_UNKNOWN;
        }
    }
}

/*
 *   oooooooo8     o   ooooooooooo
 * o888     88    888  88  888  88
 * 888           8  88     888
 * 888o     oo  8oooo88    888
 *  888oooo88 o88o  o888o o888o
 *
 */

static void on_cat(void *arg, mpegts_psi_t *psi)
{
    module_data_t *mod = arg;

    // check changes
    const uint32_t crc32 = PSI_GET_CRC32(psi);
    if(crc32 == psi->crc32)
    {
        psi->reload_counter = 0;
        return;
    }

    // check crc
    if(crc32 != PSI_CALC_CRC32(psi))
    {
        asc_log_error(MSG("CAT checksum mismatch"));
        return;
    }

    // reload stream
    if(psi->crc32 != 0)
    {
        if (psi->reload_counter >= mod->reload_delay)
        {
            asc_log_warning(MSG("CAT changed. Reload stream info"));
            stream_reload(mod);
        }
        else
            psi->reload_counter++;

        return;
    }

    psi->crc32 = crc32;

    bool is_emm_selected = false;
    if(mod->__decrypt.cas)
        is_emm_selected = mod->__decrypt.cam->disable_emm;

    const uint8_t *desc_pointer = CAT_DESC_FIRST(psi);
    while(!CAT_DESC_EOL(psi, desc_pointer))
    {
        if(desc_pointer[0] == 0x09)
        {
            const uint16_t pid = DESC_CA_PID(desc_pointer);

            if(mod->stream[pid] == MPEGTS_PACKET_CA)
                mod->stream[pid] = MPEGTS_PACKET_UNKNOWN;

            if(pid == NULL_TS_PID || mod->stream[pid] != MPEGTS_PACKET_UNKNOWN)
                ; /* Skip */
            else if(   mod->__decrypt.cas
                    && !mod->__decrypt.cam->disable_emm
                    && DESC_CA_CAID(desc_pointer) == mod->caid)
            {
                mod->stream[pid] = MPEGTS_PACKET_EMM;
                asc_log_info(MSG("Select EMM pid:%d"), pid);
                is_emm_selected = true;
            }
            else
                mod->stream[pid] = MPEGTS_PACKET_CA;
        }
        CAT_DESC_NEXT(psi, desc_pointer);
    }

    if(mod->__decrypt.cas && !is_emm_selected)
        asc_log_error(MSG("EMM is not found"));
}

/*
 * oooooooooo oooo     oooo ooooooooooo
 *  888    888 8888o   888  88  888  88
 *  888oooo88  88 888o8 88      888
 *  888        88  888  88      888
 * o888o      o88o  8  o88o    o888o
 *
 */

static void on_pmt(void *arg, mpegts_psi_t *psi)
{
    module_data_t *mod = arg;

    // check pnr
    const uint16_t pnr = PMT_GET_PNR(psi);
    if(pnr != mod->__decrypt.pnr)
        return;

    // check changes
    const uint32_t crc32 = PSI_GET_CRC32(psi);
    if(crc32 == psi->crc32)
    {
        mpegts_psi_demux(mod->custom_pmt
                         , (void (*)(void *, const uint8_t *))__module_stream_send
                         , &mod->__stream);
        return;
    }

    // check crc
    if(crc32 != PSI_CALC_CRC32(psi))
    {
        asc_log_error(MSG("PMT checksum mismatch"));
        return;
    }

    // reload stream
    if(psi->crc32 != 0)
    {
        asc_log_warning(MSG("PMT changed. Reload stream info"));
        stream_reload(mod);
        return;
    }

    psi->crc32 = crc32;

    // Make custom PMT and set descriptors for CAS
    mod->custom_pmt->pid = psi->pid;

    bool is_ecm_selected = false;
    mod->ecm_pid_fails = 0;

    if(mod->ecm_pid) // skip descriptors checking
    {
        mod->stream[mod->ecm_pid] = MPEGTS_PACKET_ECM;
        asc_log_info(MSG("Select ECM pid:%d"), mod->ecm_pid);
        is_ecm_selected = true;
    }

    uint16_t skip = 12;
    memcpy(mod->custom_pmt->buffer, psi->buffer, 10);

    const uint8_t *desc_pointer = PMT_DESC_FIRST(psi);
    while(!PMT_DESC_EOL(psi, desc_pointer))
    {
        if(desc_pointer[0] == 0x09)
        {
            const uint16_t pid = DESC_CA_PID(desc_pointer);

            if(mod->stream[pid] == MPEGTS_PACKET_CA)
                mod->stream[pid] = MPEGTS_PACKET_UNKNOWN;

            if(pid == NULL_TS_PID || mod->stream[pid] != MPEGTS_PACKET_UNKNOWN)
                ; /* Skip */
            else if(   mod->__decrypt.cas
                    && DESC_CA_CAID(desc_pointer) == mod->caid
                    && module_cas_check_descriptor(mod->__decrypt.cas, desc_pointer))
            {
                if(!is_ecm_selected)
                {
                    mod->stream[pid] = MPEGTS_PACKET_ECM;
                    asc_log_info(MSG("Select ECM pid:%d"), pid);
                    is_ecm_selected = true;
                }
                else
                {
                    asc_log_info(MSG("Backup ECM pid:%d"), pid);
                    mod->stream[pid] = MPEGTS_PACKET_CA;
                }
            }
            else
                mod->stream[pid] = MPEGTS_PACKET_CA;
        }
        else
        {
            const uint8_t size = desc_pointer[1] + 2;
            memcpy(&mod->custom_pmt->buffer[skip], desc_pointer, size);
            skip += size;
        }

        PMT_DESC_NEXT(psi, desc_pointer);
    }
    const uint16_t size = skip - 12; // 12 - PMT header
    mod->custom_pmt->buffer[10] = (mod->pmt->buffer[10] & 0xF0) | ((size >> 8) & 0x0F);
    mod->custom_pmt->buffer[11] = size & 0xFF;

    const uint8_t *pointer = PMT_ITEMS_FIRST(psi);
    while(!PMT_ITEMS_EOL(psi, pointer))
    {
        memcpy(&mod->custom_pmt->buffer[skip], pointer, 5);
        skip += 5;

        const uint16_t skip_last = skip;

        desc_pointer = PMT_ITEM_DESC_FIRST(pointer);
        while(!PMT_ITEM_DESC_EOL(pointer, desc_pointer))
        {
            if(desc_pointer[0] == 0x09)
            {
                const uint16_t pid = DESC_CA_PID(desc_pointer);

                if(mod->stream[pid] == MPEGTS_PACKET_CA)
                    mod->stream[pid] = MPEGTS_PACKET_UNKNOWN;

                if(pid == NULL_TS_PID || mod->stream[pid] != MPEGTS_PACKET_UNKNOWN)
                    ; /* Skip */
                else if(   mod->__decrypt.cas
                        && DESC_CA_CAID(desc_pointer) == mod->caid
                        && module_cas_check_descriptor(mod->__decrypt.cas, desc_pointer))
                {
                    if(mod->stream[pid] == MPEGTS_PACKET_UNKNOWN)
                    {
                        if(!is_ecm_selected)
                        {
                            mod->stream[pid] = MPEGTS_PACKET_ECM;
                            asc_log_info(MSG("Select ECM pid:%d"), pid);
                            is_ecm_selected = true;
                        }
                        else
                        {
                            asc_log_info(MSG("Backup ECM pid:%d"), pid);
                            mod->stream[pid] = MPEGTS_PACKET_CA;
                        }
                    }
                }
                else
                    mod->stream[pid] = MPEGTS_PACKET_CA;
            }
            else
            {
                const uint8_t size = desc_pointer[1] + 2;
                memcpy(&mod->custom_pmt->buffer[skip], desc_pointer, size);
                skip += size;
            }

            PMT_ITEM_DESC_NEXT(pointer, desc_pointer);
        }
        const uint16_t size = skip - skip_last;
        mod->custom_pmt->buffer[skip_last - 2] = (size << 8) & 0x0F;
        mod->custom_pmt->buffer[skip_last - 1] = size & 0xFF;

        PMT_ITEMS_NEXT(psi, pointer);
    }

    if(!mod->__decrypt.cas || is_ecm_selected)
    {
        mod->custom_pmt->buffer_size = skip + CRC32_SIZE;
        PSI_SET_SIZE(mod->custom_pmt);
        PSI_SET_CRC32(mod->custom_pmt);
    }
    else
    {
        asc_log_error(MSG("ECM is not found"));
        memcpy(mod->custom_pmt->buffer, psi->buffer, psi->buffer_size);
        mod->custom_pmt->buffer_size = psi->buffer_size;
    }

    mpegts_psi_demux(mod->custom_pmt
                     , (void (*)(void *, const uint8_t *))__module_stream_send
                     , &mod->__stream);
}

/*
 * ooooooooooo oooo     oooo
 *  888    88   8888o   888
 *  888ooo8     88 888o8 88
 *  888    oo   88  888  88
 * o888ooo8888 o88o  8  o88o
 *
 */

static void on_em(void *arg, mpegts_psi_t *psi)
{
    module_data_t *mod = arg;

    if(!mod->__decrypt.cam->is_ready)
        return;

    if(psi->buffer_size > EM_MAX_SIZE)
    {
        asc_log_error(MSG("Entitlement message size is greater than %d"), EM_MAX_SIZE);
        return;
    }

    const uint8_t em_type = psi->buffer[0];

    if((em_type & ~0x0F) != 0x80)
    {
        if ((em_type & ~0x0F) != 0x90)
            asc_log_error(MSG("wrong packet type 0x%02X"), em_type);

        return;
    }
    else if(em_type >= 0x82)
    { /* EMM */
        if(mod->__decrypt.cam->disable_emm)
            return;
    }
    else
    { /* ECM */
        if (mod->ecm_pid_delay)
        {
            if (mod->ecm_pid_delay <= asc_utime())
            {
                mod->ecm_pid_delay = 0;
                mod->ecm_pid_fails = 0;
            }
            else
                return;
        }
    }

    if(!module_cas_check_em(mod->__decrypt.cas, psi, mod->force))
        return;

    mod->force = false;

    mod->__decrypt.cam->send_em(mod->__decrypt.cam->self, &mod->__decrypt
                                , psi->buffer, psi->buffer_size);
}

#ifdef DVBCSA
void libdvbcsa_decrypt_packets(module_data_t *mod)
{
    unsigned char **clst;

    unsigned char *pkt; //uint8_t
    int xc0;
    int ev_od;
    int len;
    int offset;
    int n;

    clst=mod->cluster;
    pkt=*clst;
    do{ // find a new packet
        if(pkt==NULL){
            break;
        }
        if(pkt>=*(clst+1)){
            // out of this range, try next
            clst++;clst++;
            pkt=*clst;
            continue;
        }

        do { // handle this packet
            xc0 = pkt[3] & 0xc0;
            if(xc0 == 0x00 || xc0 == 0x40) // clear or reserved
                break;
            if(xc0 == 0x80 || xc0 == 0xc0) { // encrypted
                ev_od = (xc0 & 0x40) >> 6; // 0 even, 1 odd
                pkt[3] &= 0x3f;  // consider it decrypted now
                if(pkt[3] & 0x20) { // incomplete packet
                    offset = 4 + pkt[4] + 1;
                    len = 188 - offset;
                    n = len >> 3;
                    if(n == 0){ // decrypted==encrypted!
                        break; // this doesn't need more processing
                    }
                } else {
                    len = 184;
                    offset = 4;
                    n = 23;
                }
                if(ev_od == 0) {
                    mod->libdvbcsa_tsbbatch_even[mod->libdvbcsa_fill_even].data = pkt + offset;
                    mod->libdvbcsa_tsbbatch_even[mod->libdvbcsa_fill_even].len = len;
                    mod->libdvbcsa_fill_even++;
                } else {
                    mod->libdvbcsa_tsbbatch_odd[mod->libdvbcsa_fill_odd].data = pkt + offset;
                    mod->libdvbcsa_tsbbatch_odd[mod->libdvbcsa_fill_odd].len = len;
                    mod->libdvbcsa_fill_odd++;
                }
            }
        } while(0);
    *clst+=188;
    pkt+=188;
    } while(1);
    if(mod->libdvbcsa_fill_even) {
        mod->libdvbcsa_tsbbatch_even[mod->libdvbcsa_fill_even].data = pkt;
        dvbcsa_bs_decrypt(mod->libdvbcsa_key_even, mod->libdvbcsa_tsbbatch_even, 184);
        mod->libdvbcsa_fill_even = 0;
    }
    if(mod->libdvbcsa_fill_odd) {
        mod->libdvbcsa_tsbbatch_odd[mod->libdvbcsa_fill_odd].data = pkt;
        dvbcsa_bs_decrypt(mod->libdvbcsa_key_odd, mod->libdvbcsa_tsbbatch_odd, 184);
        mod->libdvbcsa_fill_odd = 0;
    }
}
#endif

/*
 * ooooooooooo  oooooooo8
 * 88  888  88 888
 *     888      888oooooo
 *     888             888
 *    o888o    o88oooo888
 *
 */

static void on_ts(module_data_t *mod, const uint8_t *ts)
{
    const uint16_t pid = TS_PID(ts);

    switch(mod->stream[pid])
    {
        case MPEGTS_PACKET_PAT:
            mpegts_psi_mux(mod->pat, ts, on_pat, mod);
            break;
        case MPEGTS_PACKET_CAT:
            mpegts_psi_mux(mod->cat, ts, on_cat, mod);
            return;
        case MPEGTS_PACKET_PMT:
            mpegts_psi_mux(mod->pmt, ts, on_pmt, mod);
            return;
        case MPEGTS_PACKET_ECM:
        case MPEGTS_PACKET_EMM:
            if(mod->__decrypt.cas)
                mpegts_psi_mux(mod->em, ts, on_em, mod);
        case MPEGTS_PACKET_CA:
            return;
        default:
            break;
    }

    if(!mod->is_keys)
    {
        module_stream_send(mod, ts);
        return;
    }

    memcpy(&mod->r_buffer[mod->buffer_skip], ts, TS_PACKET_SIZE);
    if(mod->s_buffer)
        module_stream_send(mod, &mod->s_buffer[mod->buffer_skip]);

    mod->buffer_skip += TS_PACKET_SIZE;
    if(mod->buffer_skip < mod->cluster_size_bytes)
        return;

    // fill cluster
    size_t i = 0, p = 0;
    mod->cluster[p] = 0;
    for(; i < mod->cluster_size_bytes; i += TS_PACKET_SIZE, p += 2)
    {
        mod->cluster[p  ] = &mod->r_buffer[i];
        mod->cluster[p+1] = &mod->r_buffer[i+TS_PACKET_SIZE];
    }
    mod->cluster[p] = 0;

    // decrypt
#ifdef DVBCSA
    if (mod->algo)
    {
        libdvbcsa_decrypt_packets(mod);
    }
    else
    {
#endif
#ifdef FFDECSA
        i = 0;
        while(i < mod->cluster_size)
            i += decrypt_packets(mod->ffdecsa, mod->cluster);
#endif
#ifdef DVBCSA
    }
#endif

    // check new key
    if(mod->new_key_id)
    {
        if(mod->new_key_id == 1)
        {
#ifdef DVBCSA
            if (mod->algo)
                dvbcsa_bs_key_set(&mod->new_key[0], mod->libdvbcsa_key_even);
#ifdef FFDECSA
            else
#endif
#endif
#ifdef FFDECSA
                set_even_control_word(mod->ffdecsa, &mod->new_key[0]);
#endif
        }
        else if(mod->new_key_id == 2)
        {
#ifdef DVBCSA
            if (mod->algo)
                dvbcsa_bs_key_set(&mod->new_key[8], mod->libdvbcsa_key_odd);
#ifdef FFDECSA
            else
#endif
#endif
#ifdef FFDECSA
                set_odd_control_word(mod->ffdecsa, &mod->new_key[8]);
#endif
        }
        mod->new_key_id = 0;
    }

    // swap buffers
    uint8_t *tmp = mod->r_buffer;
    if(mod->s_buffer)
        mod->r_buffer = mod->s_buffer;
    else
        mod->r_buffer = &mod->buffer[mod->cluster_size_bytes];
    mod->s_buffer = tmp;

    mod->buffer_skip = 0;
}

/*
 *      o      oooooooooo ooooo
 *     888      888    888 888
 *    8  88     888oooo88  888
 *   8oooo88    888        888
 * o88o  o888o o888o      o888o
 *
 */

static void on_cam_ready(module_data_t *mod)
{
    mod->caid = mod->__decrypt.cam->caid;
    stream_reload(mod);
}

static void on_cam_error(module_data_t *mod)
{
    mod->caid = 0x0000;
    mod->is_keys = false;
}

static void on_response(module_data_t *mod, const uint8_t *data, const char *errmsg)
{
    if((data[0] & ~0x01) != 0x80)
        return; /* Skip EMM */

    bool is_keys_ok = false;
    do
    {
        if(errmsg)
            break;

        if(!mod->__decrypt.cas)
        {
            errmsg = "CAS not initialized";
            break;
        }

        if(!module_cas_check_keys(mod->__decrypt.cas, data))
        {
            errmsg = "Wrong ECM id";
            break;
        }

        if(data[2] != 16)
        {
            errmsg = (data[2] == 0) ? "" : "Wrong ECM length";
            break;
        }

        static const char *errmsg_checksum = "Wrong ECM checksum";
        const uint8_t ck1 = (data[3] + data[4] + data[5]) & 0xFF;
        if(ck1 != data[6])
        {
            errmsg = errmsg_checksum;
            break;
        }

        const uint8_t ck2 = (data[7] + data[8] + data[9]) & 0xFF;
        if(ck2 != data[10])
        {
            errmsg = errmsg_checksum;
            break;
        }

        is_keys_ok = true;
    } while(0);

    if(is_keys_ok)
    {
        // Set keys
        if(mod->new_key[3] == data[6] && mod->new_key[7] == data[10])
        {
            mod->new_key_id = 2;
            memcpy(&mod->new_key[8], &data[11], 8);
        }
        else if(mod->new_key[11] == data[14] && mod->new_key[15] == data[18])
        {
            mod->new_key_id = 1;
            memcpy(mod->new_key, &data[3], 8);
        }
        else
        {
            mod->new_key_id = 0;
#ifdef DVBCSA
            if (mod->algo)
            {
                dvbcsa_bs_key_set(&data[3], mod->libdvbcsa_key_even);
                dvbcsa_bs_key_set(&data[11], mod->libdvbcsa_key_odd);
            }
#ifdef FFDECSA
            else
#endif
#endif
#ifdef FFDECSA
               set_control_words(mod->ffdecsa, &data[3], &data[11]);
#endif
            memcpy(mod->new_key, &data[3], 16);
            if(mod->is_keys)
                asc_log_warning(MSG("Both keys changed"));
        }
        mod->is_keys = true;

#if CAS_ECM_DUMP
        char key_1[17], key_2[17];
        hex_to_str(key_1, &data[3], 8);
        hex_to_str(key_2, &data[11], 8);
        asc_log_debug(MSG("ECM Found [%02X:%s:%s]") , data[0], key_1, key_2);
#endif
        mod->ecm_pid_fails = 0;
        mod->ecm_pid_delay = 0;
    }
    else
    {
        if(mod->ecm_swap_time > 0)
        {
            uint8_t pid_count = 0;
            uint8_t pid_pos_old = 0;
            uint16_t first_pid = 0;

            mod->ecm_pid_fails++;
            const uint8_t *desc_pointer = PMT_DESC_FIRST(mod->pmt);
            while(!PMT_DESC_EOL(mod->pmt, desc_pointer))
            {
                if(desc_pointer[0] == 0x09)
                {
                    const uint16_t pid = DESC_CA_PID(desc_pointer);
                    if(pid != NULL_TS_PID
                            && (mod->stream[pid] == MPEGTS_PACKET_CA || mod->stream[pid] == MPEGTS_PACKET_ECM)
                            && DESC_CA_CAID(desc_pointer) == mod->caid
                            && module_cas_check_descriptor(mod->__decrypt.cas, desc_pointer))
                    {
                        if (pid_count == 0)
                            first_pid = pid;

                        if (mod->stream[pid] == MPEGTS_PACKET_ECM)
                        {
                            pid_pos_old = pid_count;
                            mod->stream[pid] = MPEGTS_PACKET_CA;
                            asc_log_info(MSG("Deselect ECM pid:%d"), pid);
                        }

                        if (pid_pos_old < pid_count)
                        {
                            mod->stream[pid] = MPEGTS_PACKET_ECM;
                            asc_log_info(MSG("Select ECM pid:%d"), pid);
                        }

                        pid_count++;
                    }
                }
                PMT_DESC_NEXT(mod->pmt, desc_pointer);
            }

            if (pid_pos_old == pid_count - 1 && first_pid)
            {
                mod->stream[first_pid] = MPEGTS_PACKET_ECM;
                asc_log_info(MSG("Select ECM pid:%d"), first_pid);
            }

            mod->force = true;

            if (mod->ecm_pid_fails >= pid_count)
                mod->ecm_pid_delay = asc_utime() + mod->ecm_swap_time * 1000000;
            else
                return;
        }

        if(!errmsg)
            errmsg = "Unknown";

        asc_log_error(MSG("ECM:0x%02X size:%d Not Found. %s") , data[0], data[2], errmsg);
    }
}

/*
 * oooo     oooo  ooooooo  ooooooooo  ooooo  oooo ooooo       ooooooooooo
 *  8888o   888 o888   888o 888    88o 888    88   888         888    88
 *  88 888o8 88 888     888 888    888 888    88   888         888ooo8
 *  88  888  88 888o   o888 888    888 888    88   888      o  888    oo
 * o88o  8  o88o  88ooo88  o888ooo88    888oo88   o888ooooo88 o888ooo8888
 *
 */

static void module_init(module_data_t *mod)
{
    module_stream_init(mod, on_ts);

    module_option_string("name", &mod->name);
    asc_assert(mod->name != NULL, "[decrypt] option 'name' is required");

    mod->force = false;

    mod->pat = mpegts_psi_init(MPEGTS_PACKET_PAT, 0);
    mod->cat = mpegts_psi_init(MPEGTS_PACKET_CAT, 1);
    mod->pmt = mpegts_psi_init(MPEGTS_PACKET_PMT, MAX_PID);
    mod->em = mpegts_psi_init(MPEGTS_PACKET_CA, MAX_PID);
    mod->custom_pmt = mpegts_psi_init(MPEGTS_PACKET_PMT, MAX_PID);

    module_option_number("algo", &mod->algo);
    module_option_number("reload_delay", &mod->reload_delay);

#ifdef DVBCSA
    if (mod->algo)
    {
        asc_log_info(MSG("using libdvbcsa implementation"));
        mod->libdvbcsa_key_even = dvbcsa_bs_key_alloc();
        mod->libdvbcsa_key_odd = dvbcsa_bs_key_alloc();
        mod->cluster_size = dvbcsa_bs_batch_size();
        mod->cluster_size_bytes = mod->cluster_size * TS_PACKET_SIZE;
        mod->cluster = malloc(sizeof(void *) * (mod->cluster_size * 2 + 2));
        mod->libdvbcsa_tsbbatch_even = malloc((mod->cluster_size + 1) * sizeof(struct dvbcsa_bs_batch_s));
        mod->libdvbcsa_tsbbatch_odd  = malloc((mod->cluster_size + 1) * sizeof(struct dvbcsa_bs_batch_s));
    }
    else
    {
#endif
#ifdef FFDECSA
        asc_log_info(MSG("using ffdecsa implementation"));
        mod->ffdecsa = get_key_struct();
        mod->cluster_size = get_suggested_cluster_size();
        mod->cluster_size_bytes = mod->cluster_size * TS_PACKET_SIZE;
        mod->cluster = malloc(sizeof(void *) * (mod->cluster_size * 2 + 2));
#endif
#ifdef DVBCSA
    }
#endif

    mod->buffer = malloc(mod->cluster_size_bytes * 2);
    mod->r_buffer = mod->buffer; // s_buffer = NULL

    uint8_t first_key[8] = { 0 };
    const char *string_value = NULL;
    const int biss_length = module_option_string("biss", &string_value);
    if(string_value)
    {
        if(biss_length != 16)
        {
            asc_log_error(MSG("biss key must be 16 chars length"));
            astra_abort();
        }
        str_to_hex(string_value, first_key, sizeof(first_key));
        first_key[3] = (first_key[0] + first_key[1] + first_key[2]) & 0xFF;
        first_key[7] = (first_key[4] + first_key[5] + first_key[6]) & 0xFF;
        mod->is_keys = true;
        mod->caid = 0x2600;
    }
#ifdef DVBCSA
    if (mod->algo)
    {
        dvbcsa_bs_key_set(first_key, mod->libdvbcsa_key_even);
        dvbcsa_bs_key_set(first_key, mod->libdvbcsa_key_odd);
    }
    else
    {
#endif
#ifdef FFDECSA
    set_control_words(mod->ffdecsa, first_key, first_key);
#endif
#ifdef DVBCSA
    }
#endif

    mod->__decrypt.self = mod;
    mod->__decrypt.on_cam_ready = on_cam_ready;
    mod->__decrypt.on_cam_error = on_cam_error;
    mod->__decrypt.on_response = on_response;

    if(!mod->is_keys)
    {
        lua_getfield(lua, 2, "cam");
        if(!lua_isnil(lua, -1))
        {
            asc_assert(lua_type(lua, -1) == LUA_TLIGHTUSERDATA
                       , "option 'cam' required cam-module instance");
            mod->__decrypt.cam = lua_touserdata(lua, -1);
        }
        lua_pop(lua, 1);
    }

    if(mod->__decrypt.cam)
    {
        const char *value = NULL;
        module_option_string("cas_data", &value);
        if(value)
            str_to_hex(value, mod->__decrypt.cas_data, sizeof(mod->__decrypt.cas_data));

        module_cam_attach_decrypt(mod->__decrypt.cam, &mod->__decrypt);
    }

    module_option_number("ecm_pid", &mod->ecm_pid);
    module_option_number("ecm_swap_time", &mod->ecm_swap_time);

    stream_reload(mod);
}

static void module_destroy(module_data_t *mod)
{
    module_stream_destroy(mod);

    if(mod->__decrypt.cam)
    {
        module_cam_detach_decrypt(mod->__decrypt.cam, &mod->__decrypt);
        module_decrypt_cas_destroy(mod);
    }

#ifdef DVBCSA
    dvbcsa_bs_key_free(mod->libdvbcsa_key_even);
    dvbcsa_bs_key_free(mod->libdvbcsa_key_odd);
#endif
#ifdef FFDECSA
    free_key_struct(mod->ffdecsa);
#endif
    free(mod->cluster);
    free(mod->buffer);

    mpegts_psi_destroy(mod->pat);
    mpegts_psi_destroy(mod->cat);
    mpegts_psi_destroy(mod->pmt);
    mpegts_psi_destroy(mod->em);
    mpegts_psi_destroy(mod->custom_pmt);
}

MODULE_STREAM_METHODS()
MODULE_LUA_METHODS()
{
    MODULE_STREAM_METHODS_REF()
};
MODULE_LUA_REGISTER(decrypt)
