/*
 * SD Memory Card emulation as defined in the "SD Memory Card Physical
 * layer specification, Version 1.10."
 *
 * Copyright (c) 2006 Andrzej Zaborowski  <balrog@zabor.org>
 * Copyright (c) 2007 CodeSourcery
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "qemu/osdep.h"
#include "hw/qdev.h"
#include "hw/hw.h"
#include "sysemu/block-backend.h"
#include "hw/sd/sd.h"
#include "qapi/error.h"
#include "qemu/bitmap.h"
#include "hw/qdev-properties.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "qemu/log.h"

//#define DEBUG_SD 1

#ifdef DEBUG_SD
#define DPRINTF(fmt, ...) \
do { fprintf(stderr, "SD: " fmt , ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) do {} while(0)
#endif

#define ACMD41_ENQUIRY_MASK     0x00ffffff
#define OCR_POWER_UP            0x80000000
#define OCR_POWER_DELAY_NS      500000 /* 0.5ms */

typedef enum {
    sd_r0 = 0,    /* no response */
    sd_r1,        /* normal response command */
    sd_r2_i,      /* CID register */
    sd_r2_s,      /* CSD register */
    sd_r3,        /* OCR register */
    sd_r6 = 6,    /* Published RCA response */
    sd_r7,        /* Operating voltage */
    sd_r1b = -1,
    sd_illegal = -2,
} sd_rsp_type_t;

enum SDCardModes {
    sd_inactive,
    sd_card_identification_mode,
    sd_data_transfer_mode,
};

enum SDCardStates {
    sd_inactive_state = -1,
    sd_idle_state = 0,
    sd_ready_state,
    sd_identification_state,
    sd_standby_state,
    sd_transfer_state,
    sd_sendingdata_state,
    sd_receivingdata_state,
    sd_programming_state,
    sd_disconnect_state,
};

struct SDState {
    DeviceState parent_obj;

    uint32_t mode;    /* current card mode, one of SDCardModes */
    int32_t state;    /* current card state, one of SDCardStates */
    uint32_t ocr;
    QEMUTimer *ocr_power_timer;
    uint8_t scr[8];
    uint8_t cid[16];
    uint8_t csd[16];
    uint16_t rca;
    uint32_t card_status;
    uint8_t sd_status[64];
    uint32_t vhs;
    bool wp_switch;
    unsigned long *wp_groups;
    int32_t wpgrps_size;
    uint64_t size;
    uint32_t blk_len;
    uint32_t multi_blk_cnt;
    uint32_t erase_start;
    uint32_t erase_end;
    uint8_t pwd[16];
    uint32_t pwd_len;
    uint8_t function_group[6];

    bool spi;
    uint8_t current_cmd;
    /* True if we will handle the next command as an ACMD. Note that this does
     * *not* track the APP_CMD status bit!
     */
    bool expecting_acmd;
    uint32_t blk_written;
    uint64_t data_start;
    uint32_t data_offset;
    uint8_t data[512];
    qemu_irq readonly_cb;
    qemu_irq inserted_cb;
    BlockBackend *blk;

    bool enable;
};

static void sd_set_mode(SDState *sd)
{
    switch (sd->state) {
    case sd_inactive_state:
        sd->mode = sd_inactive;
        break;

    case sd_idle_state:
    case sd_ready_state:
    case sd_identification_state:
        sd->mode = sd_card_identification_mode;
        break;

    case sd_standby_state:
    case sd_transfer_state:
    case sd_sendingdata_state:
    case sd_receivingdata_state:
    case sd_programming_state:
    case sd_disconnect_state:
        sd->mode = sd_data_transfer_mode;
        break;
    }
}

static const sd_cmd_type_t sd_cmd_type[64] = {
    sd_bc,   sd_none, sd_bcr,  sd_bcr,  sd_none, sd_none, sd_none, sd_ac,
    sd_bcr,  sd_ac,   sd_ac,   sd_adtc, sd_ac,   sd_ac,   sd_none, sd_ac,
    sd_ac,   sd_adtc, sd_adtc, sd_none, sd_none, sd_none, sd_none, sd_none,
    sd_adtc, sd_adtc, sd_adtc, sd_adtc, sd_ac,   sd_ac,   sd_adtc, sd_none,
    sd_ac,   sd_ac,   sd_none, sd_none, sd_none, sd_none, sd_ac,   sd_none,
    sd_none, sd_none, sd_bc,   sd_none, sd_none, sd_none, sd_none, sd_none,
    sd_none, sd_none, sd_none, sd_none, sd_none, sd_none, sd_none, sd_ac,
    sd_adtc, sd_none, sd_none, sd_none, sd_none, sd_none, sd_none, sd_none,
};

static const int sd_cmd_class[64] = {
    0,  0,  0,  0,  0,  9, 10,  0,  0,  0,  0,  1,  0,  0,  0,  0,
    2,  2,  2,  2,  3,  3,  3,  3,  4,  4,  4,  4,  6,  6,  6,  6,
    5,  5, 10, 10, 10, 10,  5,  9,  9,  9,  7,  7,  7,  7,  7,  7,
    7,  7, 10,  7,  9,  9,  9,  8,  8, 10,  8,  8,  8,  8,  8,  8,
};

static uint8_t sd_crc7(void *message, size_t width)
{
    int i, bit;
    uint8_t shift_reg = 0x00;
    uint8_t *msg = (uint8_t *) message;

    for (i = 0; i < width; i ++, msg ++)
        for (bit = 7; bit >= 0; bit --) {
            shift_reg <<= 1;
            if ((shift_reg >> 7) ^ ((*msg >> bit) & 1))
                shift_reg ^= 0x89;
        }

    return shift_reg;
}

static uint16_t sd_crc16(void *message, size_t width)
{
    int i, bit;
    uint16_t shift_reg = 0x0000;
    uint16_t *msg = (uint16_t *) message;
    width <<= 1;

    for (i = 0; i < width; i ++, msg ++)
        for (bit = 15; bit >= 0; bit --) {
            shift_reg <<= 1;
            if ((shift_reg >> 15) ^ ((*msg >> bit) & 1))
                shift_reg ^= 0x1011;
        }

    return shift_reg;
}

static void sd_set_ocr(SDState *sd)
{
    /* All voltages OK, Standard Capacity SD Memory Card, not yet powered up */
    sd->ocr = 0x00ffff00;
}

static void sd_ocr_powerup(void *opaque)
{
    SDState *sd = opaque;

    /* Set powered up bit in OCR */
    assert(!(sd->ocr & OCR_POWER_UP));
    sd->ocr |= OCR_POWER_UP;
}

static void sd_set_scr(SDState *sd)
{
    sd->scr[0] = 0x00;		/* SCR Structure */
    sd->scr[1] = 0x2f;		/* SD Security Support */
    sd->scr[2] = 0x00;
    sd->scr[3] = 0x00;
    sd->scr[4] = 0x00;
    sd->scr[5] = 0x00;
    sd->scr[6] = 0x00;
    sd->scr[7] = 0x00;
}

#define MID	0xaa
#define OID	"XY"
#define PNM	"QEMU!"
#define PRV	0x01
#define MDT_YR	2006
#define MDT_MON	2

static void sd_set_cid(SDState *sd)
{
    sd->cid[0] = MID;		/* Fake card manufacturer ID (MID) */
    sd->cid[1] = OID[0];	/* OEM/Application ID (OID) */
    sd->cid[2] = OID[1];
    sd->cid[3] = PNM[0];	/* Fake product name (PNM) */
    sd->cid[4] = PNM[1];
    sd->cid[5] = PNM[2];
    sd->cid[6] = PNM[3];
    sd->cid[7] = PNM[4];
    sd->cid[8] = PRV;		/* Fake product revision (PRV) */
    sd->cid[9] = 0xde;		/* Fake serial number (PSN) */
    sd->cid[10] = 0xad;
    sd->cid[11] = 0xbe;
    sd->cid[12] = 0xef;
    sd->cid[13] = 0x00 |	/* Manufacture date (MDT) */
        ((MDT_YR - 2000) / 10);
    sd->cid[14] = ((MDT_YR % 10) << 4) | MDT_MON;
    sd->cid[15] = (sd_crc7(sd->cid, 15) << 1) | 1;
}

#define HWBLOCK_SHIFT	9			/* 512 bytes */
#define SECTOR_SHIFT	5			/* 16 kilobytes */
#define WPGROUP_SHIFT	7			/* 2 megs */
#define CMULT_SHIFT	9			/* 512 times HWBLOCK_SIZE */
#define WPGROUP_SIZE	(1 << (HWBLOCK_SHIFT + SECTOR_SHIFT + WPGROUP_SHIFT))

static const uint8_t sd_csd_rw_mask[16] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfc, 0xfe,
};

static void sd_set_csd(SDState *sd, uint64_t size)
{
    uint32_t csize = (size >> (CMULT_SHIFT + HWBLOCK_SHIFT)) - 1;
    uint32_t sectsize = (1 << (SECTOR_SHIFT + 1)) - 1;
    uint32_t wpsize = (1 << (WPGROUP_SHIFT + 1)) - 1;

    if (size <= 0x40000000) {	/* Standard Capacity SD */
        sd->csd[0] = 0x00;	/* CSD structure */
        sd->csd[1] = 0x26;	/* Data read access-time-1 */
        sd->csd[2] = 0x00;	/* Data read access-time-2 */
        sd->csd[3] = 0x5a;	/* Max. data transfer rate */
        sd->csd[4] = 0x5f;	/* Card Command Classes */
        sd->csd[5] = 0x50 |	/* Max. read data block length */
            HWBLOCK_SHIFT;
        sd->csd[6] = 0xe0 |	/* Partial block for read allowed */
            ((csize >> 10) & 0x03);
        sd->csd[7] = 0x00 |	/* Device size */
            ((csize >> 2) & 0xff);
        sd->csd[8] = 0x3f |	/* Max. read current */
            ((csize << 6) & 0xc0);
        sd->csd[9] = 0xfc |	/* Max. write current */
            ((CMULT_SHIFT - 2) >> 1);
        sd->csd[10] = 0x40 |	/* Erase sector size */
            (((CMULT_SHIFT - 2) << 7) & 0x80) | (sectsize >> 1);
        sd->csd[11] = 0x00 |	/* Write protect group size */
            ((sectsize << 7) & 0x80) | wpsize;
        sd->csd[12] = 0x90 |	/* Write speed factor */
            (HWBLOCK_SHIFT >> 2);
        sd->csd[13] = 0x20 |	/* Max. write data block length */
            ((HWBLOCK_SHIFT << 6) & 0xc0);
        sd->csd[14] = 0x00;	/* File format group */
        sd->csd[15] = (sd_crc7(sd->csd, 15) << 1) | 1;
    } else {			/* SDHC */
        size /= 512 * 1024;
        size -= 1;
        sd->csd[0] = 0x40;
        sd->csd[1] = 0x0e;
        sd->csd[2] = 0x00;
        sd->csd[3] = 0x32;
        sd->csd[4] = 0x5b;
        sd->csd[5] = 0x59;
        sd->csd[6] = 0x00;
        sd->csd[7] = (size >> 16) & 0xff;
        sd->csd[8] = (size >> 8) & 0xff;
        sd->csd[9] = (size & 0xff);
        sd->csd[10] = 0x7f;
        sd->csd[11] = 0x80;
        sd->csd[12] = 0x0a;
        sd->csd[13] = 0x40;
        sd->csd[14] = 0x00;
        sd->csd[15] = 0x00;
        sd->ocr |= 1 << 30;     /* High Capacity SD Memory Card */
    }
}

static void sd_set_rca(SDState *sd)
{
    sd->rca += 0x4567;
}

/* Card status bits, split by clear condition:
 * A : According to the card current state
 * B : Always related to the previous command
 * C : Cleared by read
 */
#define CARD_STATUS_A	0x02004100
#define CARD_STATUS_B	0x00c01e00
#define CARD_STATUS_C	0xfd39a028

static void sd_set_cardstatus(SDState *sd)
{
    sd->card_status = 0x00000100;
}

static void sd_set_sdstatus(SDState *sd)
{
    memset(sd->sd_status, 0, 64);
}

static int sd_req_crc_validate(SDRequest *req)
{
    uint8_t buffer[5];
    buffer[0] = 0x40 | req->cmd;
    buffer[1] = (req->arg >> 24) & 0xff;
    buffer[2] = (req->arg >> 16) & 0xff;
    buffer[3] = (req->arg >> 8) & 0xff;
    buffer[4] = (req->arg >> 0) & 0xff;
    return 0;
    return sd_crc7(buffer, 5) != req->crc;	/* TODO */
}

static void sd_response_r1_make(SDState *sd, uint8_t *response)
{
    uint32_t status = sd->card_status;
    /* Clear the "clear on read" status bits */
    sd->card_status &= ~CARD_STATUS_C;

    response[0] = (status >> 24) & 0xff;
    response[1] = (status >> 16) & 0xff;
    response[2] = (status >> 8) & 0xff;
    response[3] = (status >> 0) & 0xff;
}

static void sd_response_r3_make(SDState *sd, uint8_t *response)
{
    response[0] = (sd->ocr >> 24) & 0xff;
    response[1] = (sd->ocr >> 16) & 0xff;
    response[2] = (sd->ocr >> 8) & 0xff;
    response[3] = (sd->ocr >> 0) & 0xff;
}

static void sd_response_r6_make(SDState *sd, uint8_t *response)
{
    uint16_t arg;
    uint16_t status;

    arg = sd->rca;
    status = ((sd->card_status >> 8) & 0xc000) |
             ((sd->card_status >> 6) & 0x2000) |
              (sd->card_status & 0x1fff);
    sd->card_status &= ~(CARD_STATUS_C & 0xc81fff);

    response[0] = (arg >> 8) & 0xff;
    response[1] = arg & 0xff;
    response[2] = (status >> 8) & 0xff;
    response[3] = status & 0xff;
}

static void sd_response_r7_make(SDState *sd, uint8_t *response)
{
    response[0] = (sd->vhs >> 24) & 0xff;
    response[1] = (sd->vhs >> 16) & 0xff;
    response[2] = (sd->vhs >>  8) & 0xff;
    response[3] = (sd->vhs >>  0) & 0xff;
}

static inline uint64_t sd_addr_to_wpnum(uint64_t addr)
{
    return addr >> (HWBLOCK_SHIFT + SECTOR_SHIFT + WPGROUP_SHIFT);
}

static void sd_reset(DeviceState *dev)
{
    SDState *sd = SD_CARD(dev);
    uint64_t size;
    uint64_t sect;

    if (sd->blk) {
        blk_get_geometry(sd->blk, &sect);
    } else {
        sect = 0;
    }
    size = sect << 9;

    sect = sd_addr_to_wpnum(size) + 1;

    sd->state = sd_idle_state;
    sd->rca = 0x0000;
    sd_set_ocr(sd);
    sd_set_scr(sd);
    sd_set_cid(sd);
    sd_set_csd(sd, size);
    sd_set_cardstatus(sd);
    sd_set_sdstatus(sd);

    g_free(sd->wp_groups);
    sd->wp_switch = sd->blk ? blk_is_read_only(sd->blk) : false;
    sd->wpgrps_size = sect;
    sd->wp_groups = bitmap_new(sd->wpgrps_size);
    memset(sd->function_group, 0, sizeof(sd->function_group));
    sd->erase_start = 0;
    sd->erase_end = 0;
    sd->size = size;
    sd->blk_len = 0x200;
    sd->pwd_len = 0;
    sd->expecting_acmd = false;
    sd->multi_blk_cnt = 0;
}

static bool sd_get_inserted(SDState *sd)
{
    return sd->blk && blk_is_inserted(sd->blk);
}

static bool sd_get_readonly(SDState *sd)
{
    return sd->wp_switch;
}

static void sd_cardchange(void *opaque, bool load)
{
    SDState *sd = opaque;
    DeviceState *dev = DEVICE(sd);
    SDBus *sdbus = SD_BUS(qdev_get_parent_bus(dev));
    bool inserted = sd_get_inserted(sd);
    bool readonly = sd_get_readonly(sd);

    if (inserted) {
        sd_reset(dev);
    }

    /* The IRQ notification is for legacy non-QOM SD controller devices;
     * QOMified controllers use the SDBus APIs.
     */
    if (sdbus) {
        sdbus_set_inserted(sdbus, inserted);
        if (inserted) {
            sdbus_set_readonly(sdbus, readonly);
        }
    } else {
        qemu_set_irq(sd->inserted_cb, inserted);
        if (inserted) {
            qemu_set_irq(sd->readonly_cb, readonly);
        }
    }
}

static const BlockDevOps sd_block_ops = {
    .change_media_cb = sd_cardchange,
};

static bool sd_ocr_vmstate_needed(void *opaque)
{
    SDState *sd = opaque;

    /* Include the OCR state (and timer) if it is not yet powered up */
    return !(sd->ocr & OCR_POWER_UP);
}

static const VMStateDescription sd_ocr_vmstate = {
    .name = "sd-card/ocr-state",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sd_ocr_vmstate_needed,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(ocr, SDState),
        VMSTATE_TIMER_PTR(ocr_power_timer, SDState),
        VMSTATE_END_OF_LIST()
    },
};

static int sd_vmstate_pre_load(void *opaque)
{
    SDState *sd = opaque;

    /* If the OCR state is not included (prior versions, or not
     * needed), then the OCR must be set as powered up. If the OCR state
     * is included, this will be replaced by the state restore.
     */
    sd_ocr_powerup(sd);

    return 0;
}

static const VMStateDescription sd_vmstate = {
    .name = "sd-card",
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_load = sd_vmstate_pre_load,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(mode, SDState),
        VMSTATE_INT32(state, SDState),
        VMSTATE_UINT8_ARRAY(cid, SDState, 16),
        VMSTATE_UINT8_ARRAY(csd, SDState, 16),
        VMSTATE_UINT16(rca, SDState),
        VMSTATE_UINT32(card_status, SDState),
        VMSTATE_PARTIAL_BUFFER(sd_status, SDState, 1),
        VMSTATE_UINT32(vhs, SDState),
        VMSTATE_BITMAP(wp_groups, SDState, 0, wpgrps_size),
        VMSTATE_UINT32(blk_len, SDState),
        VMSTATE_UINT32(multi_blk_cnt, SDState),
        VMSTATE_UINT32(erase_start, SDState),
        VMSTATE_UINT32(erase_end, SDState),
        VMSTATE_UINT8_ARRAY(pwd, SDState, 16),
        VMSTATE_UINT32(pwd_len, SDState),
        VMSTATE_UINT8_ARRAY(function_group, SDState, 6),
        VMSTATE_UINT8(current_cmd, SDState),
        VMSTATE_BOOL(expecting_acmd, SDState),
        VMSTATE_UINT32(blk_written, SDState),
        VMSTATE_UINT64(data_start, SDState),
        VMSTATE_UINT32(data_offset, SDState),
        VMSTATE_UINT8_ARRAY(data, SDState, 512),
        VMSTATE_UNUSED_V(1, 512),
        VMSTATE_BOOL(enable, SDState),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription*[]) {
        &sd_ocr_vmstate,
        NULL
    },
};

/* Legacy initialization function for use by non-qdevified callers */
SDState *sd_init(BlockBackend *blk, bool is_spi)
{
    Object *obj;
    DeviceState *dev;
    Error *err = NULL;

    obj = object_new(TYPE_SD_CARD);
    dev = DEVICE(obj);
    qdev_prop_set_drive(dev, "drive", blk, &err);
    if (err) {
        error_report("sd_init failed: %s", error_get_pretty(err));
        return NULL;
    }
    qdev_prop_set_bit(dev, "spi", is_spi);
    object_property_set_bool(obj, true, "realized", &err);
    if (err) {
        error_report("sd_init failed: %s", error_get_pretty(err));
        return NULL;
    }

    return SD_CARD(dev);
}

void sd_set_cb(SDState *sd, qemu_irq readonly, qemu_irq insert)
{
    sd->readonly_cb = readonly;
    sd->inserted_cb = insert;
    qemu_set_irq(readonly, sd->blk ? blk_is_read_only(sd->blk) : 0);
    qemu_set_irq(insert, sd->blk ? blk_is_inserted(sd->blk) : 0);
}

static void sd_erase(SDState *sd)
{
    int i;
    uint64_t erase_start = sd->erase_start;
    uint64_t erase_end = sd->erase_end;

    if (!sd->erase_start || !sd->erase_end) {
        sd->card_status |= ERASE_SEQ_ERROR;
        return;
    }

    if (extract32(sd->ocr, OCR_CCS_BITN, 1)) {
        /* High capacity memory card: erase units are 512 byte blocks */
        erase_start *= 512;
        erase_end *= 512;
    }

    erase_start = sd_addr_to_wpnum(erase_start);
    erase_end = sd_addr_to_wpnum(erase_end);
    sd->erase_start = 0;
    sd->erase_end = 0;
    sd->csd[14] |= 0x40;

    for (i = erase_start; i <= erase_end; i++) {
        if (test_bit(i, sd->wp_groups)) {
            sd->card_status |= WP_ERASE_SKIP;
        }
    }
}

static uint32_t sd_wpbits(SDState *sd, uint64_t addr)
{
    uint32_t i, wpnum;
    uint32_t ret = 0;

    wpnum = sd_addr_to_wpnum(addr);

    for (i = 0; i < 32; i++, wpnum++, addr += WPGROUP_SIZE) {
        if (addr < sd->size && test_bit(wpnum, sd->wp_groups)) {
            ret |= (1 << i);
        }
    }

    return ret;
}

static void sd_function_switch(SDState *sd, uint32_t arg)
{
    int i, mode, new_func, crc;
    mode = !!(arg & 0x80000000);

    sd->data[0] = 0x00;		/* Maximum current consumption */
    sd->data[1] = 0x01;
    sd->data[2] = 0x80;		/* Supported group 6 functions */
    sd->data[3] = 0x01;
    sd->data[4] = 0x80;		/* Supported group 5 functions */
    sd->data[5] = 0x01;
    sd->data[6] = 0x80;		/* Supported group 4 functions */
    sd->data[7] = 0x01;
    sd->data[8] = 0x80;		/* Supported group 3 functions */
    sd->data[9] = 0x01;
    sd->data[10] = 0x80;	/* Supported group 2 functions */
    sd->data[11] = 0x43;
    sd->data[12] = 0x80;	/* Supported group 1 functions */
    sd->data[13] = 0x03;
    for (i = 0; i < 6; i ++) {
        new_func = (arg >> (i * 4)) & 0x0f;
        if (mode && new_func != 0x0f)
            sd->function_group[i] = new_func;
        sd->data[14 + (i >> 1)] = new_func << ((i * 4) & 4);
    }
    memset(&sd->data[17], 0, 47);
    crc = sd_crc16(sd->data, 64);
    sd->data[65] = crc >> 8;
    sd->data[66] = crc & 0xff;
}

static inline bool sd_wp_addr(SDState *sd, uint64_t addr)
{
    return test_bit(sd_addr_to_wpnum(addr), sd->wp_groups);
}

static void sd_lock_command(SDState *sd)
{
    int erase, lock, clr_pwd, set_pwd, pwd_len;
    erase = !!(sd->data[0] & 0x08);
    lock = sd->data[0] & 0x04;
    clr_pwd = sd->data[0] & 0x02;
    set_pwd = sd->data[0] & 0x01;

    if (sd->blk_len > 1)
        pwd_len = sd->data[1];
    else
        pwd_len = 0;

    if (erase) {
        if (!(sd->card_status & CARD_IS_LOCKED) || sd->blk_len > 1 ||
                        set_pwd || clr_pwd || lock || sd->wp_switch ||
                        (sd->csd[14] & 0x20)) {
            sd->card_status |= LOCK_UNLOCK_FAILED;
            return;
        }
        bitmap_zero(sd->wp_groups, sd->wpgrps_size);
        sd->csd[14] &= ~0x10;
        sd->card_status &= ~CARD_IS_LOCKED;
        sd->pwd_len = 0;
        /* Erasing the entire card here! */
        fprintf(stderr, "SD: Card force-erased by CMD42\n");
        return;
    }

    if (sd->blk_len < 2 + pwd_len ||
                    pwd_len <= sd->pwd_len ||
                    pwd_len > sd->pwd_len + 16) {
        sd->card_status |= LOCK_UNLOCK_FAILED;
        return;
    }

    if (sd->pwd_len && memcmp(sd->pwd, sd->data + 2, sd->pwd_len)) {
        sd->card_status |= LOCK_UNLOCK_FAILED;
        return;
    }

    pwd_len -= sd->pwd_len;
    if ((pwd_len && !set_pwd) ||
                    (clr_pwd && (set_pwd || lock)) ||
                    (lock && !sd->pwd_len && !set_pwd) ||
                    (!set_pwd && !clr_pwd &&
                     (((sd->card_status & CARD_IS_LOCKED) && lock) ||
                      (!(sd->card_status & CARD_IS_LOCKED) && !lock)))) {
        sd->card_status |= LOCK_UNLOCK_FAILED;
        return;
    }

    if (set_pwd) {
        memcpy(sd->pwd, sd->data + 2 + sd->pwd_len, pwd_len);
        sd->pwd_len = pwd_len;
    }

    if (clr_pwd) {
        sd->pwd_len = 0;
    }

    if (lock)
        sd->card_status |= CARD_IS_LOCKED;
    else
        sd->card_status &= ~CARD_IS_LOCKED;
}

static sd_rsp_type_t sd_normal_command(SDState *sd,
                                       SDRequest req)
{
    uint32_t rca = 0x0000;
    uint64_t addr = (sd->ocr & (1 << 30)) ? (uint64_t) req.arg << 9 : req.arg;

    /* Not interpreting this as an app command */
    sd->card_status &= ~APP_CMD;

    if (sd_cmd_type[req.cmd & 0x3F] == sd_ac
        || sd_cmd_type[req.cmd & 0x3F] == sd_adtc) {
        rca = req.arg >> 16;
    }

    /* CMD23 (set block count) must be immediately followed by CMD18 or CMD25
     * if not, its effects are cancelled */
    if (sd->multi_blk_cnt != 0 && !(req.cmd == 18 || req.cmd == 25)) {
        sd->multi_blk_cnt = 0;
    }

    DPRINTF("CMD%d 0x%08x state %d\n", req.cmd, req.arg, sd->state);
    switch (req.cmd) {
    /* Basic commands (Class 0 and Class 1) */
    case 0:	/* CMD0:   GO_IDLE_STATE */
        switch (sd->state) {
        case sd_inactive_state:
            return sd->spi ? sd_r1 : sd_r0;

        default:
            sd->state = sd_idle_state;
            sd_reset(DEVICE(sd));
            return sd->spi ? sd_r1 : sd_r0;
        }
        break;

    case 1:	/* CMD1:   SEND_OP_CMD */
        if (!sd->spi)
            goto bad_cmd;

        sd->state = sd_transfer_state;
        return sd_r1;

    case 2:	/* CMD2:   ALL_SEND_CID */
        if (sd->spi)
            goto bad_cmd;
        switch (sd->state) {
        case sd_ready_state:
            sd->state = sd_identification_state;
            return sd_r2_i;

        default:
            break;
        }
        break;

    case 3:	/* CMD3:   SEND_RELATIVE_ADDR */
        if (sd->spi)
            goto bad_cmd;
        switch (sd->state) {
        case sd_identification_state:
        case sd_standby_state:
            sd->state = sd_standby_state;
            sd_set_rca(sd);
            return sd_r6;

        default:
            break;
        }
        break;

    case 4:	/* CMD4:   SEND_DSR */
        if (sd->spi)
            goto bad_cmd;
        switch (sd->state) {
        case sd_standby_state:
            break;

        default:
            break;
        }
        break;

    case 5: /* CMD5: reserved for SDIO cards */
        return sd_illegal;

    case 6:	/* CMD6:   SWITCH_FUNCTION */
        if (sd->spi)
            goto bad_cmd;
        switch (sd->mode) {
        case sd_data_transfer_mode:
            sd_function_switch(sd, req.arg);
            sd->state = sd_sendingdata_state;
            sd->data_start = 0;
            sd->data_offset = 0;
            return sd_r1;

        default:
            break;
        }
        break;

    case 7:	/* CMD7:   SELECT/DESELECT_CARD */
        if (sd->spi)
            goto bad_cmd;
        switch (sd->state) {
        case sd_standby_state:
            if (sd->rca != rca)
                return sd_r0;

            sd->state = sd_transfer_state;
            return sd_r1b;

        case sd_transfer_state:
        case sd_sendingdata_state:
            if (sd->rca == rca)
                break;

            sd->state = sd_standby_state;
            return sd_r1b;

        case sd_disconnect_state:
            if (sd->rca != rca)
                return sd_r0;

            sd->state = sd_programming_state;
            return sd_r1b;

        case sd_programming_state:
            if (sd->rca == rca)
                break;

            sd->state = sd_disconnect_state;
            return sd_r1b;

        default:
            break;
        }
        break;

    case 8:	/* CMD8:   SEND_IF_COND */
        /* Physical Layer Specification Version 2.00 command */
        switch (sd->state) {
        case sd_idle_state:
            sd->vhs = 0;

            /* No response if not exactly one VHS bit is set.  */
            if (!(req.arg >> 8) || (req.arg >> (ctz32(req.arg & ~0xff) + 1))) {
                return sd->spi ? sd_r7 : sd_r0;
            }

            /* Accept.  */
            sd->vhs = req.arg;
            return sd_r7;

        default:
            break;
        }
        break;

    case 9:	/* CMD9:   SEND_CSD */
        switch (sd->state) {
        case sd_standby_state:
            if (sd->rca != rca)
                return sd_r0;

            return sd_r2_s;

        case sd_transfer_state:
            if (!sd->spi)
                break;
            sd->state = sd_sendingdata_state;
            memcpy(sd->data, sd->csd, 16);
            sd->data_start = addr;
            sd->data_offset = 0;
            return sd_r1;

        default:
            break;
        }
        break;

    case 10:	/* CMD10:  SEND_CID */
        switch (sd->state) {
        case sd_standby_state:
            if (sd->rca != rca)
                return sd_r0;

            return sd_r2_i;

        case sd_transfer_state:
            if (!sd->spi)
                break;
            sd->state = sd_sendingdata_state;
            memcpy(sd->data, sd->cid, 16);
            sd->data_start = addr;
            sd->data_offset = 0;
            return sd_r1;

        default:
            break;
        }
        break;

    case 11:	/* CMD11:  READ_DAT_UNTIL_STOP */
        if (sd->spi)
            goto bad_cmd;
        switch (sd->state) {
        case sd_transfer_state:
            sd->state = sd_sendingdata_state;
            sd->data_start = req.arg;
            sd->data_offset = 0;

            if (sd->data_start + sd->blk_len > sd->size)
                sd->card_status |= ADDRESS_ERROR;
            return sd_r0;

        default:
            break;
        }
        break;

    case 12:	/* CMD12:  STOP_TRANSMISSION */
        switch (sd->state) {
        case sd_sendingdata_state:
            sd->state = sd_transfer_state;
            return sd_r1b;

        case sd_receivingdata_state:
            sd->state = sd_programming_state;
            /* Bzzzzzzztt .... Operation complete.  */
            sd->state = sd_transfer_state;
            return sd_r1b;

        default:
            break;
        }
        break;

    case 13:	/* CMD13:  SEND_STATUS */
        switch (sd->mode) {
        case sd_data_transfer_mode:
            if (sd->rca != rca)
                return sd_r0;

            return sd_r1;

        default:
            break;
        }
        break;

    case 15:	/* CMD15:  GO_INACTIVE_STATE */
        if (sd->spi)
            goto bad_cmd;
        switch (sd->mode) {
        case sd_data_transfer_mode:
            if (sd->rca != rca)
                return sd_r0;

            sd->state = sd_inactive_state;
            return sd_r0;

        default:
            break;
        }
        break;

    /* Block read commands (Classs 2) */
    case 16:	/* CMD16:  SET_BLOCKLEN */
        switch (sd->state) {
        case sd_transfer_state:
            if (req.arg > (1 << HWBLOCK_SHIFT))
                sd->card_status |= BLOCK_LEN_ERROR;
            else
                sd->blk_len = req.arg;

            return sd_r1;

        default:
            break;
        }
        break;

    case 17:	/* CMD17:  READ_SINGLE_BLOCK */
        switch (sd->state) {
        case sd_transfer_state:
            if (addr + sd->blk_len > sd->size) {
                sd->card_status |= ADDRESS_ERROR;
                return sd_r1;
            }

            sd->state = sd_sendingdata_state;
            sd->data_start = addr;
            sd->data_offset = 0;
            return sd_r1;

        default:
            break;
        }
        break;

    case 18:	/* CMD18:  READ_MULTIPLE_BLOCK */
        switch (sd->state) {
        case sd_transfer_state:
            if (addr + sd->blk_len > sd->size) {
                sd->card_status |= ADDRESS_ERROR;
                return sd_r1;
            }
            sd->state = sd_sendingdata_state;
            sd->data_start = addr;
            sd->data_offset = 0;
            return sd_r1;

        default:
            break;
        }
        break;

    case 23:    /* CMD23: SET_BLOCK_COUNT */
        switch (sd->state) {
        case sd_transfer_state:
            sd->multi_blk_cnt = req.arg;
            return sd_r1;

        default:
            break;
        }
        break;

    /* Block write commands (Class 4) */
    case 24:	/* CMD24:  WRITE_SINGLE_BLOCK */
        if (sd->spi)
            goto unimplemented_cmd;
        switch (sd->state) {
        case sd_transfer_state:
            /* Writing in SPI mode not implemented.  */
            if (sd->spi)
                break;

            if (addr + sd->blk_len > sd->size) {
                sd->card_status |= ADDRESS_ERROR;
                return sd_r1;
            }

            sd->state = sd_receivingdata_state;
            sd->data_start = addr;
            sd->data_offset = 0;
            sd->blk_written = 0;

            if (sd_wp_addr(sd, sd->data_start))
                sd->card_status |= WP_VIOLATION;
            if (sd->csd[14] & 0x30)
                sd->card_status |= WP_VIOLATION;
            return sd_r1;

        default:
            break;
        }
        break;

    case 25:	/* CMD25:  WRITE_MULTIPLE_BLOCK */
        if (sd->spi)
            goto unimplemented_cmd;
        switch (sd->state) {
        case sd_transfer_state:
            /* Writing in SPI mode not implemented.  */
            if (sd->spi)
                break;

            if (addr + sd->blk_len > sd->size) {
                sd->card_status |= ADDRESS_ERROR;
                return sd_r1;
            }

            sd->state = sd_receivingdata_state;
            sd->data_start = addr;
            sd->data_offset = 0;
            sd->blk_written = 0;

            if (sd_wp_addr(sd, sd->data_start))
                sd->card_status |= WP_VIOLATION;
            if (sd->csd[14] & 0x30)
                sd->card_status |= WP_VIOLATION;
            return sd_r1;

        default:
            break;
        }
        break;

    case 26:	/* CMD26:  PROGRAM_CID */
        if (sd->spi)
            goto bad_cmd;
        switch (sd->state) {
        case sd_transfer_state:
            sd->state = sd_receivingdata_state;
            sd->data_start = 0;
            sd->data_offset = 0;
            return sd_r1;

        default:
            break;
        }
        break;

    case 27:	/* CMD27:  PROGRAM_CSD */
        if (sd->spi)
            goto unimplemented_cmd;
        switch (sd->state) {
        case sd_transfer_state:
            sd->state = sd_receivingdata_state;
            sd->data_start = 0;
            sd->data_offset = 0;
            return sd_r1;

        default:
            break;
        }
        break;

    /* Write protection (Class 6) */
    case 28:	/* CMD28:  SET_WRITE_PROT */
        switch (sd->state) {
        case sd_transfer_state:
            if (addr >= sd->size) {
                sd->card_status |= ADDRESS_ERROR;
                return sd_r1b;
            }

            sd->state = sd_programming_state;
            set_bit(sd_addr_to_wpnum(addr), sd->wp_groups);
            /* Bzzzzzzztt .... Operation complete.  */
            sd->state = sd_transfer_state;
            return sd_r1b;

        default:
            break;
        }
        break;

    case 29:	/* CMD29:  CLR_WRITE_PROT */
        switch (sd->state) {
        case sd_transfer_state:
            if (addr >= sd->size) {
                sd->card_status |= ADDRESS_ERROR;
                return sd_r1b;
            }

            sd->state = sd_programming_state;
            clear_bit(sd_addr_to_wpnum(addr), sd->wp_groups);
            /* Bzzzzzzztt .... Operation complete.  */
            sd->state = sd_transfer_state;
            return sd_r1b;

        default:
            break;
        }
        break;

    case 30:	/* CMD30:  SEND_WRITE_PROT */
        switch (sd->state) {
        case sd_transfer_state:
            sd->state = sd_sendingdata_state;
            *(uint32_t *) sd->data = sd_wpbits(sd, req.arg);
            sd->data_start = addr;
            sd->data_offset = 0;
            return sd_r1b;

        default:
            break;
        }
        break;

    /* Erase commands (Class 5) */
    case 32:	/* CMD32:  ERASE_WR_BLK_START */
        switch (sd->state) {
        case sd_transfer_state:
            sd->erase_start = req.arg;
            return sd_r1;

        default:
            break;
        }
        break;

    case 33:	/* CMD33:  ERASE_WR_BLK_END */
        switch (sd->state) {
        case sd_transfer_state:
            sd->erase_end = req.arg;
            return sd_r1;

        default:
            break;
        }
        break;

    case 38:	/* CMD38:  ERASE */
        switch (sd->state) {
        case sd_transfer_state:
            if (sd->csd[14] & 0x30) {
                sd->card_status |= WP_VIOLATION;
                return sd_r1b;
            }

            sd->state = sd_programming_state;
            sd_erase(sd);
            /* Bzzzzzzztt .... Operation complete.  */
            sd->state = sd_transfer_state;
            return sd_r1b;

        default:
            break;
        }
        break;

    /* Lock card commands (Class 7) */
    case 42:	/* CMD42:  LOCK_UNLOCK */
        if (sd->spi)
            goto unimplemented_cmd;
        switch (sd->state) {
        case sd_transfer_state:
            sd->state = sd_receivingdata_state;
            sd->data_start = 0;
            sd->data_offset = 0;
            return sd_r1;

        default:
            break;
        }
        break;

    case 52:
    case 53:
        /* CMD52, CMD53: reserved for SDIO cards
         * (see the SDIO Simplified Specification V2.0)
         * Handle as illegal command but do not complain
         * on stderr, as some OSes may use these in their
         * probing for presence of an SDIO card.
         */
        return sd_illegal;

    /* Application specific commands (Class 8) */
    case 55:	/* CMD55:  APP_CMD */
        if (sd->rca != rca)
            return sd_r0;

        sd->expecting_acmd = true;
        sd->card_status |= APP_CMD;
        return sd_r1;

    case 56:	/* CMD56:  GEN_CMD */
        fprintf(stderr, "SD: GEN_CMD 0x%08x\n", req.arg);

        switch (sd->state) {
        case sd_transfer_state:
            sd->data_offset = 0;
            if (req.arg & 1)
                sd->state = sd_sendingdata_state;
            else
                sd->state = sd_receivingdata_state;
            return sd_r1;

        default:
            break;
        }
        break;

    default:
    bad_cmd:
        qemu_log_mask(LOG_GUEST_ERROR, "SD: Unknown CMD%i\n", req.cmd);
        return sd_illegal;

    unimplemented_cmd:
        /* Commands that are recognised but not yet implemented in SPI mode.  */
        qemu_log_mask(LOG_UNIMP, "SD: CMD%i not implemented in SPI mode\n",
                      req.cmd);
        return sd_illegal;
    }

    qemu_log_mask(LOG_GUEST_ERROR, "SD: CMD%i in a wrong state\n", req.cmd);
    return sd_illegal;
}

static sd_rsp_type_t sd_app_command(SDState *sd,
                                    SDRequest req)
{
    DPRINTF("ACMD%d 0x%08x\n", req.cmd, req.arg);
    sd->card_status |= APP_CMD;
    switch (req.cmd) {
    case 6:	/* ACMD6:  SET_BUS_WIDTH */
        switch (sd->state) {
        case sd_transfer_state:
            sd->sd_status[0] &= 0x3f;
            sd->sd_status[0] |= (req.arg & 0x03) << 6;
            return sd_r1;

        default:
            break;
        }
        break;

    case 13:	/* ACMD13: SD_STATUS */
        switch (sd->state) {
        case sd_transfer_state:
            sd->state = sd_sendingdata_state;
            sd->data_start = 0;
            sd->data_offset = 0;
            return sd_r1;

        default:
            break;
        }
        break;

    case 22:	/* ACMD22: SEND_NUM_WR_BLOCKS */
        switch (sd->state) {
        case sd_transfer_state:
            *(uint32_t *) sd->data = sd->blk_written;

            sd->state = sd_sendingdata_state;
            sd->data_start = 0;
            sd->data_offset = 0;
            return sd_r1;

        default:
            break;
        }
        break;

    case 23:	/* ACMD23: SET_WR_BLK_ERASE_COUNT */
        switch (sd->state) {
        case sd_transfer_state:
            return sd_r1;

        default:
            break;
        }
        break;

    case 41:	/* ACMD41: SD_APP_OP_COND */
        if (sd->spi) {
            /* SEND_OP_CMD */
            sd->state = sd_transfer_state;
            return sd_r1;
        }
        switch (sd->state) {
        case sd_idle_state:
            /* If it's the first ACMD41 since reset, we need to decide
             * whether to power up. If this is not an enquiry ACMD41,
             * we immediately report power on and proceed below to the
             * ready state, but if it is, we set a timer to model a
             * delay for power up. This works around a bug in EDK2
             * UEFI, which sends an initial enquiry ACMD41, but
             * assumes that the card is in ready state as soon as it
             * sees the power up bit set. */
            if (!(sd->ocr & OCR_POWER_UP)) {
                if ((req.arg & ACMD41_ENQUIRY_MASK) != 0) {
                    timer_del(sd->ocr_power_timer);
                    sd_ocr_powerup(sd);
                } else if (!timer_pending(sd->ocr_power_timer)) {
                    timer_mod_ns(sd->ocr_power_timer,
                                 (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)
                                  + OCR_POWER_DELAY_NS));
                }
            }

            /* We accept any voltage.  10000 V is nothing.
             *
             * Once we're powered up, we advance straight to ready state
             * unless it's an enquiry ACMD41 (bits 23:0 == 0).
             */
            if (req.arg & ACMD41_ENQUIRY_MASK) {
                sd->state = sd_ready_state;
            }

            return sd_r3;

        default:
            break;
        }
        break;

    case 42:	/* ACMD42: SET_CLR_CARD_DETECT */
        switch (sd->state) {
        case sd_transfer_state:
            /* Bringing in the 50KOhm pull-up resistor... Done.  */
            return sd_r1;

        default:
            break;
        }
        break;

    case 51:	/* ACMD51: SEND_SCR */
        switch (sd->state) {
        case sd_transfer_state:
            sd->state = sd_sendingdata_state;
            sd->data_start = 0;
            sd->data_offset = 0;
            return sd_r1;

        default:
            break;
        }
        break;

    default:
        /* Fall back to standard commands.  */
        return sd_normal_command(sd, req);
    }

    qemu_log_mask(LOG_GUEST_ERROR, "SD: ACMD%i in a wrong state\n", req.cmd);
    return sd_illegal;
}

static int cmd_valid_while_locked(SDState *sd, SDRequest *req)
{
    /* Valid commands in locked state:
     * basic class (0)
     * lock card class (7)
     * CMD16
     * implicitly, the ACMD prefix CMD55
     * ACMD41 and ACMD42
     * Anything else provokes an "illegal command" response.
     */
    if (sd->expecting_acmd) {
        return req->cmd == 41 || req->cmd == 42;
    }
    if (req->cmd == 16 || req->cmd == 55) {
        return 1;
    }
    return sd_cmd_class[req->cmd & 0x3F] == 0
            || sd_cmd_class[req->cmd & 0x3F] == 7;
}

int sd_do_command(SDState *sd, SDRequest *req,
                  uint8_t *response) {
    int last_state;
    sd_rsp_type_t rtype;
    int rsplen;

    if (!sd->blk || !blk_is_inserted(sd->blk) || !sd->enable) {
        return 0;
    }

    if (sd_req_crc_validate(req)) {
        sd->card_status |= COM_CRC_ERROR;
        rtype = sd_illegal;
        goto send_response;
    }

    if (sd->card_status & CARD_IS_LOCKED) {
        if (!cmd_valid_while_locked(sd, req)) {
            sd->card_status |= ILLEGAL_COMMAND;
            sd->expecting_acmd = false;
            qemu_log_mask(LOG_GUEST_ERROR, "SD: Card is locked\n");
            rtype = sd_illegal;
            goto send_response;
        }
    }

    last_state = sd->state;
    sd_set_mode(sd);

    if (sd->expecting_acmd) {
        sd->expecting_acmd = false;
        rtype = sd_app_command(sd, *req);
    } else {
        rtype = sd_normal_command(sd, *req);
    }

    if (rtype == sd_illegal) {
        sd->card_status |= ILLEGAL_COMMAND;
    } else {
        /* Valid command, we can update the 'state before command' bits.
         * (Do this now so they appear in r1 responses.)
         */
        sd->current_cmd = req->cmd;
        sd->card_status &= ~CURRENT_STATE;
        sd->card_status |= (last_state << 9);
    }

send_response:
    switch (rtype) {
    case sd_r1:
    case sd_r1b:
        sd_response_r1_make(sd, response);
        rsplen = 4;
        break;

    case sd_r2_i:
        memcpy(response, sd->cid, sizeof(sd->cid));
        rsplen = 16;
        break;

    case sd_r2_s:
        memcpy(response, sd->csd, sizeof(sd->csd));
        rsplen = 16;
        break;

    case sd_r3:
        sd_response_r3_make(sd, response);
        rsplen = 4;
        break;

    case sd_r6:
        sd_response_r6_make(sd, response);
        rsplen = 4;
        break;

    case sd_r7:
        sd_response_r7_make(sd, response);
        rsplen = 4;
        break;

    case sd_r0:
    case sd_illegal:
    default:
        rsplen = 0;
        break;
    }

    if (rtype != sd_illegal) {
        /* Clear the "clear on valid command" status bits now we've
         * sent any response
         */
        sd->card_status &= ~CARD_STATUS_B;
    }

#ifdef DEBUG_SD
    if (rsplen) {
        int i;
        DPRINTF("Response:");
        for (i = 0; i < rsplen; i++)
            fprintf(stderr, " %02x", response[i]);
        fprintf(stderr, " state %d\n", sd->state);
    } else {
        DPRINTF("No response %d\n", sd->state);
    }
#endif

    return rsplen;
}

static void sd_blk_read(SDState *sd, uint64_t addr, uint32_t len)
{
    DPRINTF("sd_blk_read: addr = 0x%08llx, len = %d\n",
            (unsigned long long) addr, len);
    if (!sd->blk || blk_pread(sd->blk, addr, sd->data, len) < 0) {
        fprintf(stderr, "sd_blk_read: read error on host side\n");
    }
}

static void sd_blk_write(SDState *sd, uint64_t addr, uint32_t len)
{
    if (!sd->blk || blk_pwrite(sd->blk, addr, sd->data, len, 0) < 0) {
        fprintf(stderr, "sd_blk_write: write error on host side\n");
    }
}

#define BLK_READ_BLOCK(a, len)	sd_blk_read(sd, a, len)
#define BLK_WRITE_BLOCK(a, len)	sd_blk_write(sd, a, len)
#define APP_READ_BLOCK(a, len)	memset(sd->data, 0xec, len)
#define APP_WRITE_BLOCK(a, len)

void sd_write_data(SDState *sd, uint8_t value)
{
    int i;

    if (!sd->blk || !blk_is_inserted(sd->blk) || !sd->enable)
        return;

    if (sd->state != sd_receivingdata_state) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sd_write_data: not in Receiving-Data state\n");
        return;
    }

    if (sd->card_status & (ADDRESS_ERROR | WP_VIOLATION))
        return;

    switch (sd->current_cmd) {
    case 24:	/* CMD24:  WRITE_SINGLE_BLOCK */
        sd->data[sd->data_offset ++] = value;
        if (sd->data_offset >= sd->blk_len) {
            /* TODO: Check CRC before committing */
            sd->state = sd_programming_state;
            BLK_WRITE_BLOCK(sd->data_start, sd->data_offset);
            sd->blk_written ++;
            sd->csd[14] |= 0x40;
            /* Bzzzzzzztt .... Operation complete.  */
            sd->state = sd_transfer_state;
        }
        break;

    case 25:	/* CMD25:  WRITE_MULTIPLE_BLOCK */
        if (sd->data_offset == 0) {
            /* Start of the block - let's check the address is valid */
            if (sd->data_start + sd->blk_len > sd->size) {
                sd->card_status |= ADDRESS_ERROR;
                break;
            }
            if (sd_wp_addr(sd, sd->data_start)) {
                sd->card_status |= WP_VIOLATION;
                break;
            }
        }
        sd->data[sd->data_offset++] = value;
        if (sd->data_offset >= sd->blk_len) {
            /* TODO: Check CRC before committing */
            sd->state = sd_programming_state;
            BLK_WRITE_BLOCK(sd->data_start, sd->data_offset);
            sd->blk_written++;
            sd->data_start += sd->blk_len;
            sd->data_offset = 0;
            sd->csd[14] |= 0x40;

            /* Bzzzzzzztt .... Operation complete.  */
            if (sd->multi_blk_cnt != 0) {
                if (--sd->multi_blk_cnt == 0) {
                    /* Stop! */
                    sd->state = sd_transfer_state;
                    break;
                }
            }

            sd->state = sd_receivingdata_state;
        }
        break;

    case 26:	/* CMD26:  PROGRAM_CID */
        sd->data[sd->data_offset ++] = value;
        if (sd->data_offset >= sizeof(sd->cid)) {
            /* TODO: Check CRC before committing */
            sd->state = sd_programming_state;
            for (i = 0; i < sizeof(sd->cid); i ++)
                if ((sd->cid[i] | 0x00) != sd->data[i])
                    sd->card_status |= CID_CSD_OVERWRITE;

            if (!(sd->card_status & CID_CSD_OVERWRITE))
                for (i = 0; i < sizeof(sd->cid); i ++) {
                    sd->cid[i] |= 0x00;
                    sd->cid[i] &= sd->data[i];
                }
            /* Bzzzzzzztt .... Operation complete.  */
            sd->state = sd_transfer_state;
        }
        break;

    case 27:	/* CMD27:  PROGRAM_CSD */
        sd->data[sd->data_offset ++] = value;
        if (sd->data_offset >= sizeof(sd->csd)) {
            /* TODO: Check CRC before committing */
            sd->state = sd_programming_state;
            for (i = 0; i < sizeof(sd->csd); i ++)
                if ((sd->csd[i] | sd_csd_rw_mask[i]) !=
                    (sd->data[i] | sd_csd_rw_mask[i]))
                    sd->card_status |= CID_CSD_OVERWRITE;

            /* Copy flag (OTP) & Permanent write protect */
            if (sd->csd[14] & ~sd->data[14] & 0x60)
                sd->card_status |= CID_CSD_OVERWRITE;

            if (!(sd->card_status & CID_CSD_OVERWRITE))
                for (i = 0; i < sizeof(sd->csd); i ++) {
                    sd->csd[i] |= sd_csd_rw_mask[i];
                    sd->csd[i] &= sd->data[i];
                }
            /* Bzzzzzzztt .... Operation complete.  */
            sd->state = sd_transfer_state;
        }
        break;

    case 42:	/* CMD42:  LOCK_UNLOCK */
        sd->data[sd->data_offset ++] = value;
        if (sd->data_offset >= sd->blk_len) {
            /* TODO: Check CRC before committing */
            sd->state = sd_programming_state;
            sd_lock_command(sd);
            /* Bzzzzzzztt .... Operation complete.  */
            sd->state = sd_transfer_state;
        }
        break;

    case 56:	/* CMD56:  GEN_CMD */
        sd->data[sd->data_offset ++] = value;
        if (sd->data_offset >= sd->blk_len) {
            APP_WRITE_BLOCK(sd->data_start, sd->data_offset);
            sd->state = sd_transfer_state;
        }
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR, "sd_write_data: unknown command\n");
        break;
    }
}

uint8_t sd_read_data(SDState *sd)
{
    /* TODO: Append CRCs */
    uint8_t ret;
    int io_len;

    if (!sd->blk || !blk_is_inserted(sd->blk) || !sd->enable)
        return 0x00;

    if (sd->state != sd_sendingdata_state) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sd_read_data: not in Sending-Data state\n");
        return 0x00;
    }

    if (sd->card_status & (ADDRESS_ERROR | WP_VIOLATION))
        return 0x00;

    io_len = (sd->ocr & (1 << 30)) ? 512 : sd->blk_len;

    switch (sd->current_cmd) {
    case 6:	/* CMD6:   SWITCH_FUNCTION */
        ret = sd->data[sd->data_offset ++];

        if (sd->data_offset >= 64)
            sd->state = sd_transfer_state;
        break;

    case 9:	/* CMD9:   SEND_CSD */
    case 10:	/* CMD10:  SEND_CID */
        ret = sd->data[sd->data_offset ++];

        if (sd->data_offset >= 16)
            sd->state = sd_transfer_state;
        break;

    case 11:	/* CMD11:  READ_DAT_UNTIL_STOP */
        if (sd->data_offset == 0)
            BLK_READ_BLOCK(sd->data_start, io_len);
        ret = sd->data[sd->data_offset ++];

        if (sd->data_offset >= io_len) {
            sd->data_start += io_len;
            sd->data_offset = 0;
            if (sd->data_start + io_len > sd->size) {
                sd->card_status |= ADDRESS_ERROR;
                break;
            }
        }
        break;

    case 13:	/* ACMD13: SD_STATUS */
        ret = sd->sd_status[sd->data_offset ++];

        if (sd->data_offset >= sizeof(sd->sd_status))
            sd->state = sd_transfer_state;
        break;

    case 17:	/* CMD17:  READ_SINGLE_BLOCK */
        if (sd->data_offset == 0)
            BLK_READ_BLOCK(sd->data_start, io_len);
        ret = sd->data[sd->data_offset ++];

        if (sd->data_offset >= io_len)
            sd->state = sd_transfer_state;
        break;

    case 18:	/* CMD18:  READ_MULTIPLE_BLOCK */
        if (sd->data_offset == 0)
            BLK_READ_BLOCK(sd->data_start, io_len);
        ret = sd->data[sd->data_offset ++];

        if (sd->data_offset >= io_len) {
            sd->data_start += io_len;
            sd->data_offset = 0;

            if (sd->multi_blk_cnt != 0) {
                if (--sd->multi_blk_cnt == 0) {
                    /* Stop! */
                    sd->state = sd_transfer_state;
                    break;
                }
            }

            if (sd->data_start + io_len > sd->size) {
                sd->card_status |= ADDRESS_ERROR;
                break;
            }
        }
        break;

    case 22:	/* ACMD22: SEND_NUM_WR_BLOCKS */
        ret = sd->data[sd->data_offset ++];

        if (sd->data_offset >= 4)
            sd->state = sd_transfer_state;
        break;

    case 30:	/* CMD30:  SEND_WRITE_PROT */
        ret = sd->data[sd->data_offset ++];

        if (sd->data_offset >= 4)
            sd->state = sd_transfer_state;
        break;

    case 51:	/* ACMD51: SEND_SCR */
        ret = sd->scr[sd->data_offset ++];

        if (sd->data_offset >= sizeof(sd->scr))
            sd->state = sd_transfer_state;
        break;

    case 56:	/* CMD56:  GEN_CMD */
        if (sd->data_offset == 0)
            APP_READ_BLOCK(sd->data_start, sd->blk_len);
        ret = sd->data[sd->data_offset ++];

        if (sd->data_offset >= sd->blk_len)
            sd->state = sd_transfer_state;
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR, "sd_read_data: unknown command\n");
        return 0x00;
    }

    return ret;
}

bool sd_data_ready(SDState *sd)
{
    return sd->state == sd_sendingdata_state;
}

void sd_enable(SDState *sd, bool enable)
{
    sd->enable = enable;
}

static void sd_instance_init(Object *obj)
{
    SDState *sd = SD_CARD(obj);

    sd->enable = true;
    sd->ocr_power_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, sd_ocr_powerup, sd);
}

static void sd_instance_finalize(Object *obj)
{
    SDState *sd = SD_CARD(obj);

    timer_del(sd->ocr_power_timer);
    timer_free(sd->ocr_power_timer);
}

static void sd_realize(DeviceState *dev, Error **errp)
{
    SDState *sd = SD_CARD(dev);

    if (sd->blk && blk_is_read_only(sd->blk)) {
        error_setg(errp, "Cannot use read-only drive as SD card");
        return;
    }

    if (sd->blk) {
        blk_set_dev_ops(sd->blk, &sd_block_ops, sd);
    }
}

static Property sd_properties[] = {
    DEFINE_PROP_DRIVE("drive", SDState, blk),
    /* We do not model the chip select pin, so allow the board to select
     * whether card should be in SSI or MMC/SD mode.  It is also up to the
     * board to ensure that ssi transfers only occur when the chip select
     * is asserted.  */
    DEFINE_PROP_BOOL("spi", SDState, spi, false),
    DEFINE_PROP_END_OF_LIST()
};

static void sd_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SDCardClass *sc = SD_CARD_CLASS(klass);

    dc->realize = sd_realize;
    dc->props = sd_properties;
    dc->vmsd = &sd_vmstate;
    dc->reset = sd_reset;
    dc->bus_type = TYPE_SD_BUS;

    sc->do_command = sd_do_command;
    sc->write_data = sd_write_data;
    sc->read_data = sd_read_data;
    sc->data_ready = sd_data_ready;
    sc->enable = sd_enable;
    sc->get_inserted = sd_get_inserted;
    sc->get_readonly = sd_get_readonly;
}

static const TypeInfo sd_info = {
    .name = TYPE_SD_CARD,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(SDState),
    .class_size = sizeof(SDCardClass),
    .class_init = sd_class_init,
    .instance_init = sd_instance_init,
    .instance_finalize = sd_instance_finalize,
};

static void sd_register_types(void)
{
    type_register_static(&sd_info);
}

type_init(sd_register_types)
