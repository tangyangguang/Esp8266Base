/* esp8266-4mb-2mfs.ld
 *
 * ESP8266 4MB Flash，2MB Sketch + 2MB LittleFS 分区布局
 *
 *   sketch  @ 0x40200000 (~2MB, 2093040B)
 *   LittleFS@ 0x40400000 (~2MB, 2072576B)
 *   eeprom  @ 0x405FB000 (4KB)
 *   rfcal   @ 0x405FC000 (4KB)
 *   wifi    @ 0x405FD000 (12KB)
 */

MEMORY
{
  dport0_0_seg :   org = 0x3FF00000, len = 0x10
  dram0_0_seg :    org = 0x3FFE8000, len = 0x14000
  irom0_0_seg :    org = 0x40201010, len = 0x1feff0
}

PROVIDE ( _FS_start = 0x40400000 );
PROVIDE ( _FS_end   = 0x405FA000 );
PROVIDE ( _FS_page  = 0x100 );
PROVIDE ( _FS_block = 0x2000 );
PROVIDE ( _EEPROM_start = 0x405fb000 );

INCLUDE "local.eagle.app.v6.common.ld"
