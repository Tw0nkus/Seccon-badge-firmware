#include "badge_files.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hardware/flash.h"
#include "pico/flash.h"
#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "tusb.h"

#ifndef USBD_MANUFACTURER
#define USBD_MANUFACTURER "Badge"
#endif

#ifndef USBD_PRODUCT
#define USBD_PRODUCT "Badge"
#endif

#ifndef BADGE_USB_VID
#define BADGE_USB_VID 0x2e8a
#endif

#ifndef BADGE_USB_CDC_PID
#define BADGE_USB_CDC_PID 0x0009
#endif

#ifndef BADGE_USB_MSC_PID
#define BADGE_USB_MSC_PID 0x000b
#endif

enum {
    FILES_BLOCK_SIZE = 512,
    FILES_BLOCK_COUNT = BADGE_FILES_CAPACITY_BYTES / FILES_BLOCK_SIZE,
    FILES_RESERVED_SECTORS = 1,
    FILES_FAT_COUNT = 2,
    FILES_FAT_SECTORS = 24,
    FILES_ROOT_ENTRIES = 512,
    FILES_ROOT_SECTORS = FILES_ROOT_ENTRIES * 32 / FILES_BLOCK_SIZE,
    FILES_ROOT_START =
        FILES_RESERVED_SECTORS + FILES_FAT_COUNT * FILES_FAT_SECTORS,
    FILES_DATA_START = FILES_ROOT_START + FILES_ROOT_SECTORS,
    FILES_DATA_CLUSTERS = FILES_BLOCK_COUNT - FILES_DATA_START,
    FILES_CACHE_FLUSH_MS = 250,
    FILES_EJECT_DELAY_MS = 500,
};

_Static_assert(BADGE_FILES_CAPACITY_BYTES % FLASH_SECTOR_SIZE == 0,
               "filesystem must fill whole flash erase sectors");
_Static_assert(FILES_DATA_CLUSTERS >= 4085 && FILES_DATA_CLUSTERS < 65525,
               "filesystem geometry must be FAT16");
_Static_assert(FILES_FAT_SECTORS * FILES_BLOCK_SIZE / 2 >=
                   FILES_DATA_CLUSTERS + 2,
               "FAT is too small for the data clusters");

extern char __files_partition_start;

static bool msc_mode;
static bool storage_available = true;
static bool eject_requested;
static uint32_t eject_requested_ms;

static uint32_t cache_offset = UINT32_MAX;
static uint32_t cache_write_ms;
static bool cache_dirty;
static uint8_t cache[FLASH_SECTOR_SIZE] __attribute__((aligned(4)));

static uint16_t read_le16(const uint8_t *value) {
    return (uint16_t)value[0] | (uint16_t)value[1] << 8;
}

static uint32_t read_le32(const uint8_t *value) {
    return (uint32_t)value[0] | (uint32_t)value[1] << 8 |
           (uint32_t)value[2] << 16 | (uint32_t)value[3] << 24;
}

static void write_le16(uint8_t *value, uint16_t number) {
    value[0] = (uint8_t)number;
    value[1] = (uint8_t)(number >> 8);
}

static void write_le32(uint8_t *value, uint32_t number) {
    value[0] = (uint8_t)number;
    value[1] = (uint8_t)(number >> 8);
    value[2] = (uint8_t)(number >> 16);
    value[3] = (uint8_t)(number >> 24);
}

static uint32_t files_flash_offset(void) {
    return (uint32_t)((uintptr_t)&__files_partition_start - XIP_BASE);
}

static const uint8_t *files_flash(void) {
    return (const uint8_t *)(XIP_BASE + files_flash_offset());
}

static bool filesystem_geometry_valid(void) {
    const uint8_t *boot = files_flash();
    uint32_t total_sectors = read_le16(boot + 19);
    if (!total_sectors) {
        total_sectors = read_le32(boot + 32);
    }

    return boot[510] == 0x55 && boot[511] == 0xaa &&
           read_le16(boot + 11) == FILES_BLOCK_SIZE && boot[13] &&
           read_le16(boot + 14) && boot[16] && read_le16(boot + 17) &&
           read_le16(boot + 22) && total_sectors <= FILES_BLOCK_COUNT;
}

static bool flash_is_erased(size_t length) {
    const uint8_t *flash = files_flash();
    for (size_t i = 0; i < length; ++i) {
        if (flash[i] != 0xff) {
            return false;
        }
    }
    return true;
}

typedef struct {
    uint32_t offset;
    bool erase;
    uint8_t boot[FILES_BLOCK_SIZE];
    uint8_t fat[FILES_BLOCK_SIZE];
    uint8_t root[FILES_BLOCK_SIZE];
    uint8_t zero[FILES_BLOCK_SIZE];
} format_context_t;

static void format_flash(void *parameter) {
    const format_context_t *format = parameter;
    if (format->erase) {
        flash_range_erase(format->offset, BADGE_FILES_CAPACITY_BYTES);
    }
    flash_range_program(format->offset, format->boot, sizeof(format->boot));
    for (uint32_t copy = 0; copy < FILES_FAT_COUNT; ++copy) {
        uint32_t first = FILES_RESERVED_SECTORS + copy * FILES_FAT_SECTORS;
        for (uint32_t sector = 0; sector < FILES_FAT_SECTORS; ++sector) {
            const uint8_t *data = sector ? format->zero : format->fat;
            flash_range_program(
                format->offset + (first + sector) * FILES_BLOCK_SIZE, data,
                FILES_BLOCK_SIZE);
        }
    }
    flash_range_program(format->offset + FILES_ROOT_START * FILES_BLOCK_SIZE,
                        format->root, sizeof(format->root));
    for (uint32_t sector = 1; sector < FILES_ROOT_SECTORS; ++sector) {
        flash_range_program(format->offset +
                                (FILES_ROOT_START + sector) * FILES_BLOCK_SIZE,
                            format->zero, sizeof(format->zero));
    }
}

static bool broken_fat_tails_are_erased(void) {
    const uint8_t *boot = files_flash();
    if (memcmp(boot + 43, "BADGE FILES", 11) ||
        read_le16(boot + 19) != FILES_BLOCK_COUNT ||
        boot[16] != FILES_FAT_COUNT ||
        read_le16(boot + 22) != FILES_FAT_SECTORS) {
        return false;
    }
    for (uint32_t copy = 0; copy < FILES_FAT_COUNT; ++copy) {
        uint32_t first = FILES_RESERVED_SECTORS + copy * FILES_FAT_SECTORS;
        const uint8_t *fat = files_flash() + first * FILES_BLOCK_SIZE;
        if (fat[0] != 0xf8 || fat[1] != 0xff || fat[2] != 0xff ||
            fat[3] != 0xff) {
            return false;
        }
        for (size_t i = FILES_BLOCK_SIZE;
             i < FILES_FAT_SECTORS * FILES_BLOCK_SIZE; ++i) {
            if (fat[i] != 0xff) {
                return false;
            }
        }
    }
    return true;
}

static bool fat_tails_are_zero(void) {
    for (uint32_t copy = 0; copy < FILES_FAT_COUNT; ++copy) {
        uint32_t first = FILES_RESERVED_SECTORS + copy * FILES_FAT_SECTORS;
        const uint8_t *fat = files_flash() + first * FILES_BLOCK_SIZE;
        for (size_t i = FILES_BLOCK_SIZE;
             i < FILES_FAT_SECTORS * FILES_BLOCK_SIZE; ++i) {
            if (fat[i] != 0) {
                return false;
            }
        }
    }
    return true;
}

typedef struct {
    uint32_t offset;
    uint8_t zero[FILES_BLOCK_SIZE];
} fat_repair_t;

static void repair_fat_tails(void *parameter) {
    const fat_repair_t *repair = parameter;
    for (uint32_t copy = 0; copy < FILES_FAT_COUNT; ++copy) {
        uint32_t first = FILES_RESERVED_SECTORS + copy * FILES_FAT_SECTORS;
        for (uint32_t sector = 1; sector < FILES_FAT_SECTORS; ++sector) {
            flash_range_program(
                repair->offset + (first + sector) * FILES_BLOCK_SIZE,
                repair->zero, FILES_BLOCK_SIZE);
        }
    }
}

static bool repair_broken_fat(void) {
    fat_repair_t repair = {.offset = files_flash_offset()};
    int result =
        flash_safe_execute(repair_fat_tails, &repair, UINT32_MAX);
    return result == PICO_OK && fat_tails_are_zero();
}

static bool create_empty_filesystem(void) {
    format_context_t format = {
        .offset = files_flash_offset(),
        .erase = !flash_is_erased(BADGE_FILES_CAPACITY_BYTES),
    };
    uint8_t *boot = format.boot;

    boot[0] = 0xeb;
    boot[1] = 0x3c;
    boot[2] = 0x90;
    memcpy(boot + 3, "MSDOS5.0", 8);
    write_le16(boot + 11, FILES_BLOCK_SIZE);
    boot[13] = 1;
    write_le16(boot + 14, FILES_RESERVED_SECTORS);
    boot[16] = FILES_FAT_COUNT;
    write_le16(boot + 17, FILES_ROOT_ENTRIES);
    write_le16(boot + 19, FILES_BLOCK_COUNT);
    boot[21] = 0xf8;
    write_le16(boot + 22, FILES_FAT_SECTORS);
    write_le16(boot + 24, 32);
    write_le16(boot + 26, 64);
    boot[36] = 0x80;
    boot[38] = 0x29;
    write_le32(boot + 39, 0x34434553);
    memcpy(boot + 43, "BADGE FILES", 11);
    memcpy(boot + 54, "FAT16   ", 8);
    boot[510] = 0x55;
    boot[511] = 0xaa;

    format.fat[0] = 0xf8;
    format.fat[1] = 0xff;
    format.fat[2] = 0xff;
    format.fat[3] = 0xff;
    memcpy(format.root, "BADGE FILES", 11);
    format.root[11] = 0x08;

    cache_offset = UINT32_MAX;
    cache_dirty = false;
    int result = flash_safe_execute(format_flash, &format, UINT32_MAX);
    return result == PICO_OK && filesystem_geometry_valid() &&
           fat_tails_are_zero();
}

typedef struct {
    uint32_t offset;
    const uint8_t *data;
    bool erase;
} flash_write_t;

static void write_flash_sector(void *parameter) {
    const flash_write_t *write = parameter;
    if (write->erase) {
        flash_range_erase(write->offset, FLASH_SECTOR_SIZE);
    }
    flash_range_program(write->offset, write->data, FLASH_SECTOR_SIZE);
}

static bool flush_cache(void) {
    if (!cache_dirty) {
        return true;
    }

    const uint8_t *stored = files_flash() + cache_offset;
    bool changed = false;
    bool erase = false;
    for (size_t i = 0; i < sizeof(cache); ++i) {
        changed |= stored[i] != cache[i];
        erase |= (stored[i] & cache[i]) != cache[i];
    }
    if (!changed) {
        cache_dirty = false;
        return true;
    }

    flash_write_t write = {
        .offset = files_flash_offset() + cache_offset,
        .data = cache,
        .erase = erase,
    };
    bool saved = flash_safe_execute(write_flash_sector, &write, UINT32_MAX) ==
                     PICO_OK &&
                 !memcmp(stored, cache, sizeof(cache));
    cache_dirty = !saved;
    storage_available &= saved;
    return saved;
}

static bool load_cache(uint32_t offset) {
    uint32_t sector_offset = offset & ~(uint32_t)(FLASH_SECTOR_SIZE - 1);
    if (cache_offset == sector_offset) {
        return true;
    }
    if (!flush_cache()) {
        return false;
    }
    memcpy(cache, files_flash() + sector_offset, sizeof(cache));
    cache_offset = sector_offset;
    return true;
}

static void make_short_name(const uint8_t *entry, char name[13]) {
    size_t length = 0;
    for (size_t i = 0; i < 8 && entry[i] != ' '; ++i) {
        name[length++] = (char)entry[i];
    }
    if (entry[8] != ' ') {
        name[length++] = '.';
        for (size_t i = 8; i < 11 && entry[i] != ' '; ++i) {
            name[length++] = (char)entry[i];
        }
    }
    name[length] = '\0';
}

static void add_long_name_part(const uint8_t *entry, char name[256]) {
    static const uint8_t offsets[] = {1, 3, 5, 7, 9, 14, 16,
                                      18, 20, 22, 24, 28, 30};
    uint8_t order = entry[0] & 0x1f;
    if (!order || order > 20) {
        name[0] = '\0';
        return;
    }
    if (entry[0] & 0x40) {
        memset(name, 0, 256);
    }
    size_t base = (size_t)(order - 1) * 13;
    for (size_t i = 0; i < sizeof(offsets); ++i) {
        uint16_t character = read_le16(entry + offsets[i]);
        if (base + i >= 255 || character == 0 || character == 0xffff) {
            if (base + i < 256 && character == 0) {
                name[base + i] = '\0';
            }
            continue;
        }
        name[base + i] = character >= 32 && character < 127
                             ? (char)character
                             : '?';
    }
}

static bool names_equal(const char *left, const char *right) {
    while (*left && *right) {
        char a = *left++;
        char b = *right++;
        if (a >= 'A' && a <= 'Z') {
            a += 'a' - 'A';
        }
        if (b >= 'A' && b <= 'Z') {
            b += 'a' - 'A';
        }
        if (a != b) {
            return false;
        }
    }
    return *left == *right;
}

static const uint8_t *find_root_file(const char *wanted) {
    const uint8_t *boot = files_flash();
    uint32_t root_start =
        read_le16(boot + 14) + boot[16] * read_le16(boot + 22);
    uint32_t root_entries = read_le16(boot + 17);
    if ((root_start * FILES_BLOCK_SIZE + root_entries * 32) >
        BADGE_FILES_CAPACITY_BYTES) {
        return NULL;
    }

    const uint8_t *root = files_flash() + root_start * FILES_BLOCK_SIZE;
    char long_name[256] = {0};
    for (uint32_t i = 0; i < root_entries; ++i) {
        const uint8_t *entry = root + i * 32;
        if (entry[0] == 0) {
            break;
        }
        if (entry[0] == 0xe5) {
            long_name[0] = '\0';
            continue;
        }
        if (entry[11] == 0x0f) {
            add_long_name_part(entry, long_name);
            continue;
        }

        char short_name[13];
        make_short_name(entry, short_name);
        bool match = !(entry[11] & (0x08 | 0x10)) &&
                     (names_equal(wanted, short_name) ||
                      (long_name[0] && names_equal(wanted, long_name)));
        long_name[0] = '\0';
        if (match) {
            return entry;
        }
    }
    return NULL;
}

void badge_files_usb_init(void) {
    tusb_init();
}

badge_files_status_t badge_files_prepare(void) {
    if (filesystem_geometry_valid()) {
        if (broken_fat_tails_are_erased()) {
            return repair_broken_fat() ? BADGE_FILES_REPAIRED
                                       : BADGE_FILES_ERROR;
        }
        return BADGE_FILES_READY;
    }
    if (!flash_is_erased(FILES_BLOCK_SIZE)) {
        return BADGE_FILES_UNSUPPORTED;
    }
    return create_empty_filesystem() ? BADGE_FILES_CREATED : BADGE_FILES_ERROR;
}

bool badge_files_print_listing(void) {
    if (!filesystem_geometry_valid()) {
        return false;
    }

    const uint8_t *boot = files_flash();
    uint32_t root_start =
        read_le16(boot + 14) + boot[16] * read_le16(boot + 22);
    uint32_t root_entries = read_le16(boot + 17);
    if ((root_start * FILES_BLOCK_SIZE + root_entries * 32) >
        BADGE_FILES_CAPACITY_BYTES) {
        return false;
    }


    const uint8_t *root = files_flash() + root_start * FILES_BLOCK_SIZE;
    char long_name[256] = {0};
    unsigned files = 0;
    puts("Files in the root directory:");
    for (uint32_t i = 0; i < root_entries; ++i) {
        const uint8_t *entry = root + i * 32;
        if (entry[0] == 0) {
            break;
        }
        if (entry[0] == 0xe5) {
            long_name[0] = '\0';
            continue;
        }
        if (entry[11] == 0x0f) {
            add_long_name_part(entry, long_name);
            continue;
        }
        if (entry[11] & 0x08) {
            long_name[0] = '\0';
            continue;
        }

        char short_name[13];
        make_short_name(entry, short_name);
        const char *name = long_name[0] ? long_name : short_name;
        if (entry[11] & 0x10) {
            printf("  <DIR>       %s/\n", name);
        } else {
            printf("  %10lu  %s\n", (unsigned long)read_le32(entry + 28), name);
        }
        ++files;
        long_name[0] = '\0';
    }
    if (!files) {
        puts("  (none)");
    }
    return true;
}

bool badge_files_read_file(const char *name, char *buffer, size_t capacity,
                          size_t *length) {
    if (!name || !buffer || !capacity || !length ||
        !filesystem_geometry_valid()) {
        return false;
    }
    const uint8_t *entry = find_root_file(name);
    if (!entry) {
        return false;
    }

    const uint8_t *boot = files_flash();
    uint32_t file_size = read_le32(entry + 28);
    uint32_t sectors_per_cluster = boot[13];
    uint32_t reserved = read_le16(boot + 14);
    uint32_t fat_sectors = read_le16(boot + 22);
    uint32_t root_sectors =
        (read_le16(boot + 17) * 32 + FILES_BLOCK_SIZE - 1) /
        FILES_BLOCK_SIZE;
    uint32_t data_start = reserved + boot[16] * fat_sectors + root_sectors;
    uint32_t total_sectors = read_le16(boot + 19);
    if (!total_sectors) {
        total_sectors = read_le32(boot + 32);
    }
    if (!file_size || file_size >= capacity || data_start >= total_sectors) {
        return false;
    }

    uint32_t max_cluster =
        (total_sectors - data_start) / sectors_per_cluster + 1;
    uint16_t cluster = read_le16(entry + 26);
    const uint8_t *fat = files_flash() + reserved * FILES_BLOCK_SIZE;
    size_t copied = 0;
    while (copied < file_size) {
        if (cluster < 2 || cluster > max_cluster ||
            (uint32_t)cluster * 2 + 1 >= fat_sectors * FILES_BLOCK_SIZE) {
            return false;
        }
        uint32_t first_sector =
            data_start + (uint32_t)(cluster - 2) * sectors_per_cluster;
        for (uint32_t sector = 0;
             sector < sectors_per_cluster && copied < file_size; ++sector) {
            uint32_t disk_sector = first_sector + sector;
            if (disk_sector >= total_sectors) {
                return false;
            }
            size_t chunk = file_size - copied;
            if (chunk > FILES_BLOCK_SIZE) {
                chunk = FILES_BLOCK_SIZE;
            }
            memcpy(buffer + copied,
                   files_flash() + disk_sector * FILES_BLOCK_SIZE, chunk);
            copied += chunk;
        }
        if (copied < file_size) {
            cluster = read_le16(fat + (uint32_t)cluster * 2);
            if (cluster < 2 || cluster >= 0xfff0) {
                return false;
            }
        }
    }
    buffer[copied] = '\0';
    *length = copied;
    return true;
}

static void switch_usb_mode(bool use_msc) {
    tud_disconnect();
    sleep_ms(250);
    msc_mode = use_msc;
    tud_connect();
}

bool badge_files_enter_usb_mode(void) {
    if (!storage_available || msc_mode) {
        return false;
    }
    eject_requested = false;
    switch_usb_mode(true);
    return true;
}

void badge_files_task(void) {
    tud_task();
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    if (cache_dirty && now_ms - cache_write_ms >= FILES_CACHE_FLUSH_MS) {
        flush_cache();
    }
    if (msc_mode && eject_requested &&
        now_ms - eject_requested_ms >= FILES_EJECT_DELAY_MS) {
        flush_cache();
        eject_requested = false;
        switch_usb_mode(false);
    }
}

enum {
    USB_STRING_LANGUAGE,
    USB_STRING_MANUFACTURER,
    USB_STRING_PRODUCT,
    USB_STRING_SERIAL,
    USB_STRING_INTERFACE,
};

static const tusb_desc_device_t cdc_device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = BADGE_USB_VID,
    .idProduct = BADGE_USB_CDC_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = USB_STRING_MANUFACTURER,
    .iProduct = USB_STRING_PRODUCT,
    .iSerialNumber = USB_STRING_SERIAL,
    .bNumConfigurations = 1,
};

static const tusb_desc_device_t msc_device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_UNSPECIFIED,
    .bDeviceSubClass = 0,
    .bDeviceProtocol = 0,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = BADGE_USB_VID,
    .idProduct = BADGE_USB_MSC_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = USB_STRING_MANUFACTURER,
    .iProduct = USB_STRING_PRODUCT,
    .iSerialNumber = USB_STRING_SERIAL,
    .bNumConfigurations = 1,
};

enum {
    CDC_INTERFACE_CONTROL,
    CDC_INTERFACE_DATA,
    CDC_INTERFACE_COUNT,
};

enum {
    MSC_INTERFACE,
    MSC_INTERFACE_COUNT,
};

#define CDC_CONFIGURATION_LENGTH (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)
#define MSC_CONFIGURATION_LENGTH (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)

static const uint8_t cdc_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, CDC_INTERFACE_COUNT, 0,
                          CDC_CONFIGURATION_LENGTH, 0, 250),
    TUD_CDC_DESCRIPTOR(CDC_INTERFACE_CONTROL, USB_STRING_INTERFACE, 0x81, 8,
                       0x02, 0x82, 64),
};

static const uint8_t msc_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, MSC_INTERFACE_COUNT, 0,
                          MSC_CONFIGURATION_LENGTH, 0, 250),
    TUD_MSC_DESCRIPTOR(MSC_INTERFACE, USB_STRING_INTERFACE, 0x01, 0x81, 64),
};

const uint8_t *tud_descriptor_device_cb(void) {
    return (const uint8_t *)(msc_mode ? &msc_device_descriptor
                                     : &cdc_device_descriptor);
}

const uint8_t *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return msc_mode ? msc_configuration_descriptor
                    : cdc_configuration_descriptor;
}

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t language_id) {
    (void)language_id;
    static uint16_t descriptor[33];
    static char serial[PICO_UNIQUE_BOARD_ID_SIZE_BYTES * 2 + 1];
    const char *text = NULL;

    if (index == USB_STRING_LANGUAGE) {
        descriptor[1] = 0x0409;
        descriptor[0] = (uint16_t)((TUSB_DESC_STRING << 8) | 4);
        return descriptor;
    }
    if (!serial[0]) {
        pico_get_unique_board_id_string(serial, sizeof(serial));
    }
    if (index == USB_STRING_MANUFACTURER) {
        text = USBD_MANUFACTURER;
    } else if (index == USB_STRING_PRODUCT) {
        text = msc_mode ? "Badge Files" : USBD_PRODUCT;
    } else if (index == USB_STRING_SERIAL) {
        text = serial;
    } else if (index == USB_STRING_INTERFACE) {
        text = msc_mode ? "Badge USB Drive" : "Badge Console";
    } else {
        return NULL;
    }

    size_t length = strlen(text);
    if (length > 32) {
        length = 32;
    }
    for (size_t i = 0; i < length; ++i) {
        descriptor[i + 1] = (uint8_t)text[i];
    }
    descriptor[0] =
        (uint16_t)((TUSB_DESC_STRING << 8) | (2 * length + 2));
    return descriptor;
}

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8],
                        uint8_t product_id[16], uint8_t product_rev[4]) {
    (void)lun;
    memset(vendor_id, ' ', 8);
    memset(product_id, ' ', 16);
    memset(product_rev, ' ', 4);
    memcpy(vendor_id, "Badge", 5);
    memcpy(product_id, "File Store", 10);
    memcpy(product_rev, "1.0", 3);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun) {
    (void)lun;
    return storage_available;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count,
                         uint16_t *block_size) {
    (void)lun;
    *block_count = FILES_BLOCK_COUNT;
    *block_size = FILES_BLOCK_SIZE;
}

bool tud_msc_is_writable_cb(uint8_t lun) {
    (void)lun;
    return storage_available;
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start,
                           bool load_eject) {
    (void)lun;
    (void)power_condition;
    if (load_eject && !start) {
        if (!flush_cache()) {
            return false;
        }
        eject_requested = true;
        eject_requested_ms = to_ms_since_boot(get_absolute_time());
    }
    return true;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                          void *buffer, uint32_t buffer_size) {
    (void)lun;
    uint32_t byte_offset = lba * FILES_BLOCK_SIZE + offset;
    if (lba >= FILES_BLOCK_COUNT ||
        offset + buffer_size > FILES_BLOCK_SIZE) {
        tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x21, 0);
        return -1;
    }
    uint32_t sector_offset =
        byte_offset & ~(uint32_t)(FLASH_SECTOR_SIZE - 1);
    if (cache_offset == sector_offset) {
        memcpy(buffer, cache + byte_offset - sector_offset, buffer_size);
    } else {
        memcpy(buffer, files_flash() + byte_offset, buffer_size);
    }
    return (int32_t)buffer_size;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                           uint8_t *buffer, uint32_t buffer_size) {
    uint32_t byte_offset = lba * FILES_BLOCK_SIZE + offset;
    if (!storage_available || lba >= FILES_BLOCK_COUNT ||
        offset + buffer_size > FILES_BLOCK_SIZE || !load_cache(byte_offset)) {
        tud_msc_set_sense(lun, SCSI_SENSE_MEDIUM_ERROR, 0x0c, 0x02);
        return -1;
    }
    memcpy(cache + byte_offset - cache_offset, buffer, buffer_size);
    cache_dirty = true;
    cache_write_ms = to_ms_since_boot(get_absolute_time());
    return (int32_t)buffer_size;
}

int32_t tud_msc_scsi_cb(uint8_t lun, const uint8_t scsi_command[16],
                        void *buffer, uint16_t buffer_size) {
    (void)buffer;
    (void)buffer_size;
    if (scsi_command[0] == 0x35) {
        return flush_cache() ? 0 : -1;
    }
    tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0);
    return -1;
}
