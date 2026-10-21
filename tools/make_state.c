// Host-side image builder using the same locked LittleFS version as firmware.
#include "lfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned char image[1024*1024];
static unsigned char touched[256];
static int rd(const struct lfs_config *c,lfs_block_t b,lfs_off_t o,void *p,lfs_size_t n) {
    (void)c; if(b>=256 || o>4096 || n>4096-o) return LFS_ERR_IO;
    memcpy(p,image+b*4096+o,n); return 0;
}
static int wr(const struct lfs_config *c,lfs_block_t b,lfs_off_t o,const void *p,lfs_size_t n) {
    (void)c; if(b>=256 || o>4096 || n>4096-o) return LFS_ERR_IO;
    touched[b]=1;
    for(unsigned i=0;i<n;i++) image[b*4096+o+i]&=((const unsigned char *)p)[i];
    return 0;
}
static int er(const struct lfs_config *c,lfs_block_t b) {
    (void)c; if(b>=256) return LFS_ERR_IO;
    touched[b]=1; memset(image+b*4096,255,4096); return 0;
}
static int sy(const struct lfs_config *c) {(void)c; return 0;}
static void check(int rc) {if(rc<0) {fprintf(stderr,"LittleFS image error: %d\n",rc); exit(1);}}
int main(int argc,char **argv) {
    if(argc<2 || argc%2!=0) return 2;
    memset(image,255,sizeof(image));
    struct lfs_config c={0};
    c.read=rd;c.prog=wr;c.erase=er;c.sync=sy;
    c.read_size=c.prog_size=c.cache_size=256;c.block_size=4096;c.block_count=256;
    c.block_cycles=500;c.lookahead_size=32;
    lfs_t fs; check(lfs_format(&fs,&c));check(lfs_mount(&fs,&c));
    // Arguments are ordered destination/source pairs. Empty source means directory.
    for(int i=2;i<argc;i+=2) {
        if(!argv[i+1][0]) {check(lfs_mkdir(&fs,argv[i]));continue;}
        FILE *in=fopen(argv[i+1],"rb");if(!in) {perror(argv[i+1]);return 1;}
        lfs_file_t file;check(lfs_file_open(&fs,&file,argv[i],LFS_O_WRONLY|LFS_O_CREAT|LFS_O_EXCL));
        unsigned char block[4096];size_t n;
        while((n=fread(block,1,sizeof(block),in))) {
            int written=lfs_file_write(&fs,&file,block,n);check(written);
            if(written!=(int)n) return 1;
        }
        if(ferror(in)) return 1;
        fclose(in);check(lfs_file_close(&fs,&file));
    }
    check(lfs_unmount(&fs));
    FILE *out=fopen(argv[1],"wb");if(!out) {perror(argv[1]);return 1;}
    if(fwrite(image,1,sizeof(image),out)!=sizeof(image)) return 1;
    if(fclose(out)) return 1;
    // Include every block LittleFS used, even when its file payload is all 0xff.
    size_t length=strlen(argv[1])+sizeof(".blocks");
    char *map=malloc(length);if(!map) return 1;
    snprintf(map,length,"%s.blocks",argv[1]);
    out=fopen(map,"wb");free(map);if(!out) return 1;
    if(fwrite(touched,1,sizeof(touched),out)!=sizeof(touched)) return 1;
    return fclose(out)!=0;
}
