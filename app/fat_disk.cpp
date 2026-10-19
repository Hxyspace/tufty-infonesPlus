#include "fat_disk.hpp"
#include "ff.h"
#include "diskio.h"
namespace flashfat {
Disk disk;
bool Disk::probe() {
    sector_size=0;
    uint8_t b[512];
    if(!read || !read(0,b,sizeof(b))) return false;
    uint16_t ss=b[11]|(uint16_t(b[12])<<8);
    uint32_t total=b[19]|(uint32_t(b[20])<<8);
    if(!total) total=b[32]|(uint32_t(b[33])<<8)|(uint32_t(b[34])<<16)|(uint32_t(b[35])<<24);
    if((ss!=512 && ss!=1024 && ss!=2048 && ss!=4096) || !total ||
       total>tufty::storage::layout::kSystem.bytes/ss || b[510]!=0x55 || b[511]!=0xaa ||
       !b[13] || (b[13]&(b[13]-1)) || !b[16]) return false;
    sector_size=ss; return true;
}
bool Disk::range(uint32_t lba,uint32_t offset,uint32_t size,uint32_t &address) const {
    if(!sector_size || lba>=tufty::storage::layout::kSystem.bytes/sector_size || offset>=sector_size) return false;
    address=lba*sector_size+offset;
    return tufty::storage::layout::kSystem.contains(address,size);
}
bool Disk::sectors(uint32_t lba,uint32_t count,void *out) const {
    uint32_t address;
    return out && count && sector_size && count<=tufty::storage::layout::kSystem.bytes/sector_size &&
        range(lba,0,count*sector_size,address) && read && read(address,out,count*sector_size);
}
}
extern "C" {
DSTATUS disk_initialize(BYTE d) {return disk_status(d);}
DSTATUS disk_status(BYTE d) {
    return d || !flashfat::disk.runtime_owned || !flashfat::disk.sector_size ? STA_NOINIT : STA_PROTECT;
}
DRESULT disk_read(BYTE d,BYTE *out,LBA_t lba,UINT count) {
    if(disk_status(d)&STA_NOINIT) return RES_NOTRDY;
    return flashfat::disk.sectors(lba,count,out)?RES_OK:RES_PARERR;
}
DRESULT disk_write(BYTE,const BYTE *,LBA_t,UINT) {return RES_WRPRT;}
DRESULT disk_ioctl(BYTE d,BYTE cmd,void *out) {
    if(disk_status(d)&STA_NOINIT) return RES_NOTRDY;
    if(cmd==CTRL_SYNC) return RES_OK;
    if(!out) return RES_PARERR;
    switch(cmd) {
        case GET_SECTOR_SIZE: *static_cast<WORD *>(out)=flashfat::disk.sector_size; break;
        case GET_SECTOR_COUNT: *static_cast<LBA_t *>(out)=tufty::storage::layout::kSystem.bytes/flashfat::disk.sector_size; break;
        case GET_BLOCK_SIZE: *static_cast<DWORD *>(out)=4096/flashfat::disk.sector_size; break;
        default: return RES_PARERR;
    }
    return RES_OK;
}
}
