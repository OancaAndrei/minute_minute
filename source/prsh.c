/*
 *  minute - a port of the "mini" IOS replacement for the Wii U.
 *
 *  Copyright (C) 2016          SALT
 *  Copyright (C) 2016          Daz Jones <daz@dazzozo.com>
 *
 *  This code is licensed to you under the terms of the GNU GPL, version 2;
 *  see file COPYING or http://www.gnu.org/licenses/old-licenses/gpl-2.0.txt
 */
 
 #include "prsh.h"

#include "types.h"
#include "utils.h"
#include <string.h>
#include "gfx.h"
#include "crypto.h"
#include "memory.h"
#include "rtc.h"
#include "menu.h"

typedef struct {
    char name[0x100];
    void* data;
    u32 size;
    u32 is_set;
    u8 unk[0x20];
} PACKED prsh_entry;

typedef struct {
    u32 checksum;
    u32 size;
    u32 is_set;
    u32 magic;
} PACKED prst_entry;

typedef struct {
    u32 checksum;
    u32 magic;
    u32 version;
    u32 size;
    u32 is_boot1;
    u32 total_entries;
    u32 entries;
    prsh_entry entry[];
} PACKED prsh_header;

typedef struct {
    u32 is_coldboot;
    u32 boot_flags;
    u32 boot_state;
    u32 boot_count;

    u32 field_10;
    u32 field_14; // set to 0x40000 for recovery
    u32 field_18;
    u32 field_1C;

    u32 field_20;
    u32 field_24;
    u32 field_28;
    u32 field_2C;

    u32 field_30;
    u32 field_34;
    u32 boot1_main;
    u32 boot1_read;

    u32 boot1_verify;
    u32 boot1_decrypt;
    u32 boot0_main;
    u32 boot0_read;

    u32 boot0_verify;
    u32 boot0_decrypt;
} PACKED boot_info_t;

#define PRSH_FLAG_ISSET             (0x80000000)
#define PRSH_FLAG_WARM_BOOT         (0x40000000) // cleared on normal boots -- instructs MCP to not recreate mcp_launch_region, mcp_ramdisk_region, mcp_list_region, mcp_fs_cache_region
#define PRSH_FLAG_TITLES_ON_MLC     (0x20000000) // if unset, use HFIO/"SLC emulation"
#define PRSH_FLAG_10000000          (0x10000000) // unk
#define PRSH_FLAG_IOS_RELAUNCH      (0x08000000) // cleared on normal boots, set on fw.img reloads
#define PRSH_FLAG_HAS_BOOT_TIMES    (0x04000000) // set on normal boots
#define PRSH_FLAG_RETAIL            (0x02000000) // set if /sys/config/system.xml 'dev_mode' is 0
#define PRSH_FLAG_01000000          (0x01000000) // unk, can be read via MCP cmd
#define PRSH_FLAG_00800000          (0x00800000) // unk, has set/clear fn in MCP

static prst_entry* prst = NULL;
static prsh_header* header = NULL;
static bool initialized = false;
extern otp_t otp;

void prsh_set_dev_mode();
void prsh_mcp_recovery();
void prsh_mcp_recovery_alt();

menu menu_prsh = {
    MENU_TITLE, // title
    {
            "Backup and Restore", // subtitles
    },
    1, // number of subtitles
    {
            {"Set dev_mode", &prsh_set_dev_mode},
            {"Trigger mcp_recovery", &prsh_mcp_recovery},
            {"Trigger mcp_recovery (alt)", &prsh_mcp_recovery_alt},
            {"Return to Main Menu", &menu_close},
    },
    4, // number of options
    0,
    0
};

void prsh_menu()
{
    menu_init(&menu_prsh);
}

void prsh_set_dev_mode()
{
    boot_info_t* boot_info = NULL;
    prsh_get_entry("boot_info", &boot_info, NULL);
    if (!boot_info) {
        boot_info = (boot_info_t*)0x10008000;
    }

    boot_info->boot_flags &= ~PRSH_FLAG_RETAIL;

    prsh_recompute_checksum();
}

void prsh_mcp_recovery()
{
    boot_info_t* boot_info = NULL;
    prsh_get_entry("boot_info", &boot_info, NULL);
    if (!boot_info) {
        boot_info = (boot_info_t*)0x10008000;
    }

    boot_info->boot_flags &= ~PRSH_FLAG_RETAIL;
    boot_info->boot_state = PON_POWER_BTN | PON_EJECT_BTN | PFLAG_PON_COLDBOOT;

    prsh_recompute_checksum();
}

void prsh_mcp_recovery_alt()
{
    boot_info_t* boot_info = NULL;
    prsh_get_entry("boot_info", &boot_info, NULL);
    if (!boot_info) {
        boot_info = (boot_info_t*)0x10008000;
    }

    boot_info->boot_flags &= ~PRSH_FLAG_RETAIL;
    boot_info->boot_state = PON_POWER_BTN | PON_EJECT_BTN | PFLAG_PON_COLDBOOT;
    boot_info->field_14 = 0x40000;

    prsh_recompute_checksum();
}

void prsh_dump_entry(char* name) {
    void* data = NULL;
    size_t size;
    if (!prsh_get_entry(name, &data, &size)) {
        if (size > 0x200) {
            size = 0x200;
        }
        for (size_t i = 0; i < size; i++) {
            if (i && i % 16 == 0) {
                printf("\n        ");
            }
            else if (!i) {
                printf("        ");
            }
            printf("%02x ", *(u8*)((u32)data + i));
        }
        printf("\n");
    }
}

void prsh_reset(void)
{
    prst = NULL;
    header = NULL;
    initialized = false;
}

void prsh_set_bootinfo()
{
    if (!header) return;

    /*if (!header->entries) {
        header->entries++;
    }*/

    header->entries = 1;

    // create boot_info
    prsh_entry* boot_info_ent = &header->entry[0];
    strncpy(boot_info_ent->name, "boot_info", 0x100);
    boot_info_ent->data = (void *)0x10008000;
    boot_info_ent->size = 0x58;
    boot_info_ent->is_set = 0x80000000;

    boot_info_t* boot_info = (boot_info_t*)0x10008000;
    boot_info->is_coldboot = 1;
    boot_info->boot_flags = PRSH_FLAG_ISSET | PRSH_FLAG_TITLES_ON_MLC | PRSH_FLAG_HAS_BOOT_TIMES | PRSH_FLAG_RETAIL | 0x80;//; // 0xA6000000;//
    boot_info->boot_state = 0;
    boot_info->boot_count = 1;
    boot_info->field_10 = 0; // 1?
    boot_info->field_14 = 0;
    boot_info->field_18 = 0xFFFFFFFF;
    boot_info->field_1C = 0xFFFFFFFF;
    boot_info->field_20 = 0xFFFFFFFF;
    boot_info->field_24 = 0xFFFFFFFF;
    boot_info->field_28 = 0xFFFFFFFF;
    boot_info->field_2C = 0xFFFFFFFF;
    boot_info->field_30 = 0;
    boot_info->field_34 = 0;

    // Dummy values
    // TODO just get the real ones
    boot_info->boot1_main = 0x00369F6B;
    boot_info->boot1_read = 0x00297268;
    boot_info->boot1_verify = 0x0005FCFE;
    boot_info->boot1_decrypt = 0x00053CE8;
    boot_info->boot0_main = 0x00012030;
    boot_info->boot0_read = 0x000029D2;
    boot_info->boot0_verify = 0x0000D281;
    boot_info->boot0_decrypt = 0x0000027A;
}

void prsh_init(void)
{
    // corrupt
    if (header && header->total_entries > 0x100) {
        printf("prsh: Detected corruption, %u total entries.\nprsh: Reinitializing.\n", header->total_entries);
        initialized = false;
    }

    if(initialized) return;

    void* buffer = (void*)0x10000400;
    size_t size = 0x7C00;
    while(size) {
        if(!memcmp(buffer, "PRSH", sizeof(u32))) break;
        buffer += sizeof(u32);
        size -= sizeof(u32);

        //printf("%08x: %08x\n", buffer, *(u32*)buffer);
    }

    if (!size) {
        // clear bad PRSH data
        memset((u8 *)0x10000400, 0,0x7C00);

        u32 total_entries = 0x20;

        /* create PRSH */
        header = (prsh_header*)0x10005A54;
        header->magic = 0x50525348; // "PRSH"
        header->version = 1;
        header->is_boot1 = 1;
        header->total_entries = total_entries;
        header->entries = 0x1;
        header->size = sizeof(*header);
        header->size += total_entries * sizeof(prsh_entry);

        memset((u8 *)&header->entry[0], 0, total_entries * sizeof(prsh_entry));

        prst = (prst_entry*)&header->entry[total_entries];
        prst->size = header->size;
        prst->is_set = 1;
        prst->magic= 0x50525354; // "PRST"

#if 0
        // create mcp_crash_region
        {
            prsh_entry* prsh_ent = &header->entry[header->entries++];
            strncpy(prsh_ent->name, "mcp_crash_region", 0x100);
            prsh_ent->data = (void *)0x100f7f60;
            prsh_ent->size = 0x80a0;
            prsh_ent->is_set = 0x80000000;
        }

        {
            prsh_entry* prsh_ent = &header->entry[header->entries++];
            strncpy(prsh_ent->name, "mcp_syslog_region", 0x100);
            prsh_ent->data = (void *)0x1ff7ffd0;
            prsh_ent->size = 0x80030;
            prsh_ent->is_set = 0x80000000;
        }

        {
            prsh_entry* prsh_ent = &header->entry[header->entries++];
            strncpy(prsh_ent->name, "mcp_fs_cache_region", 0x100);
            prsh_ent->data = (void *)0x100d7ee0;
            prsh_ent->size = 0x20080;
            prsh_ent->is_set = 0x80000000;
        }

        {
            prsh_entry* prsh_ent = &header->entry[header->entries++];
            strncpy(prsh_ent->name, "mcp_list_region", 0x100);
            prsh_ent->data = (void *)0x1fe62c40;
            prsh_ent->size = 0x11d390;
            prsh_ent->is_set = 0x80000000;
        }
#endif

        // TODO: we could pass in a ram-only OTP here, maybe?
        printf("prsh: No header found, made a new one.\n");
    }
    else {
        header = buffer - sizeof(u32);
        prst = (prst_entry*)&header->entry[header->total_entries];

        //header->entries = 0x4;
    }

    initialized = true;

#ifdef MINUTE_BOOT1
    prsh_set_bootinfo();
    prsh_recompute_checksum();
#endif

#ifndef MINUTE_BOOT1
    printf("prsh: Header at %08x, PRST at %08x, %u entries (%u capacity):\n", header, prst, header->entries, header->total_entries);
    for (int i = 0; i < header->entries; i++) {
        printf("    %u: %s %p %x\n", i, header->entry[i].name, header->entry[i].size, header->entry[i].data);
        //prsh_dump_entry(header->entry[i].name);
    }

#if 0
    void* data = header;
    size = sizeof(*header);
    for (size_t i = 0; i < size; i++) {
        if (i && i % 16 == 0) {
            printf("\n");
        }
        else if (!i) {
            printf("");
        }
        printf("%02x ", *(u8*)((u32)data + i));
    }

    data = prst;
    size = sizeof(*prst);
    for (size_t i = 0; i < size; i++) {
        if (i && i % 16 == 0) {
            printf("\n");
        }
        else if (!i) {
            printf("");
        }
        printf("%02x ", *(u8*)((u32)data + i));
    }
#endif
#endif

    // TODO: verify checksums
    // TODO: increment counts as boot1
    
    
}

int prsh_get_entry(const char* name, void** data, size_t* size)
{
    prsh_init();
    if(!name) return -1;
    if (header->total_entries > 0x100) return -1; // corrupt 

    for(int i = 0; i < header->entries; i++) {
        prsh_entry* entry = &header->entry[i];

        if(!strncmp(name, entry->name, sizeof(entry->name))) {
            if(data) *data = entry->data;
            if(size) *size = entry->size;
            return 0;
        }
    }

    return -2;
}

int prsh_set_entry(const char* name, void* data, size_t size)
{
    prsh_init();
    if(!name) return -1;
    if (header->total_entries > 0x100) return -1; // corrupt

    for(int i = 0; i < header->entries; i++) {
        prsh_entry* entry = &header->entry[i];

        if(!strncmp(name, entry->name, sizeof(entry->name))) {
            entry->data = data;
            entry->size = size;
            entry->is_set = 0x80000000;
            prsh_recompute_checksum();
            return 0;
        }
    }

    return -2;
}

/*
prsh: Header at 10005a54, PRST at 10007ff0, 5 entries (32 capacity):
    0: boot_info 0x58 100fffa8
        00 00 00 01 80 00 00 00 00 00 00 00 00 00 00 00 
        00 00 00 01 00 00 00 00 ff ff ff ff ff ff ff ff 
        ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff ff 
        00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 
        00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 
        00 00 00 00 00 00 00 00 
    1: mcp_crash_region 0x80a0 100f7f08
        e1 41 22 14 80 00 00 00 00 00 00 01 00 00 00 00 
        00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 
        00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 
        00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 
        00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 
        00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 
        00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 
        00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 
        00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 
        00 00 00 00 00 00 40 00 10 0f 7f a4 00 00 00 00 
        00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 
        00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 
        00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 
        00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 
        00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 
        00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 
        00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 
        00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 
        00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 
        00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 
        00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 
        00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 
        00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 
        00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 
        00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 
        00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 
        00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 
        00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 
        00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 
        00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 
        00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 
        00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 
    2: mcp_syslog_region 0x80030 1ff7ffd0
        de 51 51 09 00 00 00 02 00 00 00 01 de 51 51 0a 
    3: mcp_fs_cache_region 0x20080 100d7e88
        10 0d 81 18 05 07 e1 9c 00 00 00 00 00 00 00 00 

    4: mcp_ramdisk_region 0x6c 100b743c
        00 00 00 01 00 00 00 01 20 00 00 00 07 ff f8 00 
        ef 7f ef fb eb 37 fe ff ff 9f df ff ef bf df cf 
*/

void prsh_recompute_checksum()
{
    prsh_init();
    // Re-calculate PRSH checksum
    u32 checksum = 0;
    u32 word_counter = 0;
    
    void* checksum_start = (void*)(&header->magic);
    while (word_counter < ((header->entries * sizeof(prsh_entry))>>2))
    {
        checksum ^= *(u32 *)(checksum_start + word_counter * 0x04);
        word_counter++;
    }
    
    printf("%08x %08x\n", header->checksum, checksum);
    
    header->checksum = checksum;

    checksum = 0;
    word_counter = 0;
    void* prst_checksum_start = (void*)(&prst->size);
    while (word_counter < (sizeof(prst_entry)/sizeof(u32)-1))
    {
        u32 val = *(u32 *)(prst_checksum_start + word_counter * 0x04);
        //printf("%08x\n", val);
        checksum ^= val;
        word_counter++;
    }
    
    printf("%08x %08x\n", prst->checksum, checksum);
    
    prst->checksum = checksum;
}

void prsh_decrypt()
{
    if (!(otp.security_level & 0x80000000)) {
        return;
    }

    aes_reset();
    aes_set_key(otp.fw_ancast_key);

    static const u8 iv[16] = {0x0A, 0xAB, 0xA5, 0x30, 0x2E, 0x90, 0x12, 0xD9, 
                              0x08, 0x51, 0x74, 0xE8, 0x6B, 0x83, 0xEC, 0x22};

    aes_set_iv((u8*)iv);
    aes_decrypt((void*)0x10000400, (void*)0x10000400, 0x7C00 / 0x10, 0);
}


void prsh_encrypt()
{
    if (!(otp.security_level & 0x80000000)) {
        return;
    }

    aes_reset();
    aes_set_key(otp.fw_ancast_key);

    static const u8 iv[16] = {0x0A, 0xAB, 0xA5, 0x30, 0x2E, 0x90, 0x12, 0xD9, 
                              0x08, 0x51, 0x74, 0xE8, 0x6B, 0x83, 0xEC, 0x22};

    aes_set_iv((u8*)iv);
    aes_encrypt((void*)0x10000400, (void*)0x10000400, 0x7C00 / 0x10, 0);

    dc_flushrange((void*)0x10000400, 0x7C00);
}
