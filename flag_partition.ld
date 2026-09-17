/* Flash layout for the Pico 2 board's configured 4 MiB flash.
 * 0x100f0000-0x103effff: USB mass-storage filesystem (3 MiB)
 * 0x103f0000-0x103f0fff: UART quiz provisioning record (4 KiB)
 * 0x103f1000-0x103fbfff: future flag package (44 KiB)
 * 0x103fc000-0x103fcfff: persistent progress records (4 KiB)
 * 0x103fd000-0x103fdfff: persistent badge settings (4 KiB)
 * 0x103fe000-0x103fffff: extended persistent progress records (8 KiB)
 */
__files_partition_start = 0x100f0000;
__files_partition_end = 0x103f0000;
__flag_partition_start = 0x103f0000;
__uart_quiz_record_start = 0x103f0000;
__flag_package_start = 0x103f1000;
__flag_progress_start = 0x103fc000;
__badge_settings_start = 0x103fd000;
__flag_progress_extension_start = 0x103fe000;
__flag_partition_end = 0x10400000;

ASSERT(__flash_binary_end <= __files_partition_start,
       "Firmware overlaps the USB filesystem partition")
ASSERT(__files_partition_end - __files_partition_start == 0x300000,
       "USB filesystem partition must be exactly 3 MiB")
