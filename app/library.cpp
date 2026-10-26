#include "library.hpp"
#include "cJSON.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <new>
#include <strings.h>

void tufty_service();
namespace library {
namespace {
constexpr size_t kIndexBytes=256*1024,kTextBytes=16*1024,kOrderBytes=64*1024;
constexpr const char *kOrder="/state/game-order.json",*kStamp="/state/library.stamp";
struct Stamp {uint32_t magic=0x3142494c,index=0,order=0,priorities=0; uint64_t sources=0; uint32_t total=0,reserved=0;};
static_assert(sizeof(Stamp)==32);
struct Work {
    storage::Entry entries[storage::kMaxRoms];
    char order[storage::kMaxRoms][256];
    uint8_t index[kIndexBytes];
    char text[kOrderBytes];
    size_t count=0,priorities=0,total=0,used=0,strings=0;
    uint64_t sources=0;
    unsigned warnings=0;
    storage::Progress progress=nullptr;
};
struct Draft {
    uint8_t original[kIndexBytes];
    char order[storage::kMaxRoms][256];
    size_t original_bytes=0,priorities=0,total=0;
    uint64_t sources=0;
};
static_assert(sizeof(Work)+sizeof(Draft)<storage::kMaxRomBytes);
Stamp current;
bool have_stamp=false;
bool editing=false;
Draft &draft() {return *reinterpret_cast<Draft *>(static_cast<uint8_t *>(storage::library_scratch())+sizeof(Work));}
void tick(Work &w) {tufty_service(); if(w.progress) w.progress();}
bool fail(const char *message) {std::snprintf(storage::error,sizeof storage::error,"%s",message); return false;}
bool ascii(const char *s,bool multiline=false) {
    for(;*s;++s) if((*s<32 || uint8_t(*s)>126) && !(multiline && *s=='\n')) return false;
    return true;
}
bool leaf(const char *s) {
    const auto n=std::strlen(s);
    return n>4 && n<=255 && !strcasecmp(s+n-4,".nes") && ascii(s) && !std::strstr(s,"..") &&
        !std::strpbrk(s,"/\\:\"<>|?*") && s[n-1]!=' ';
}
// Numeric runs compare by significant length/value, never by overflowing atoi.
bool natural(const char *a,const char *b) {
    const char *original_a=a,*original_b=b;
    while(*a && *b) {
        if(std::isdigit(uint8_t(*a)) && std::isdigit(uint8_t(*b))) {
            const char *ae=a,*be=b;
            while(std::isdigit(uint8_t(*ae))) ++ae;
            while(std::isdigit(uint8_t(*be))) ++be;
            while(a<ae && *a=='0') ++a;
            while(b<be && *b=='0') ++b;
            if(ae-a!=be-b) return ae-a<be-b;
            const int cmp=std::memcmp(a,b,ae-a);
            if(cmp) return cmp<0;
            a=ae; b=be;
        } else {
            const int x=std::tolower(uint8_t(*a)),y=std::tolower(uint8_t(*b));
            if(x!=y) return x<y;
            ++a; ++b;
        }
    }
    if(*a || *b) return !*a;
    return std::strcmp(original_a,original_b)<0;
}
size_t rank(const Work &w,const char *name) {
    for(size_t i=0;i<w.priorities;++i) if(!strcasecmp(name,w.order[i])) return i;
    return storage::kMaxRoms;
}
bool before(const Work &w,const storage::Entry &a,const storage::Entry &b) {
    const auto x=rank(w,a.name),y=rank(w,b.name);
    return x==y?natural(a.name,b.name):x<y;
}
uint64_t hash(const void *data,size_t n,uint64_t h=14695981039346656037ull) {
    const auto *p=static_cast<const uint8_t *>(data);
    for(size_t i=0;i<n;++i) h=(h^p[i])*1099511628211ull;
    return h;
}
uint32_t crc(const uint8_t *p,size_t n,bool index=false) {
    uint32_t c=~0u;
    for(size_t i=0;i<n;++i) {
        c^=index && i>=20 && i<24?0:p[i];
        for(unsigned bit=0;bit<8;++bit) c=(c>>1)^((c&1)?0xedb88320u:0);
        if(i%4096==0) tufty_service();
    }
    return ~c;
}
void u32(uint8_t *p,uint32_t n) {for(unsigned i=0;i<4;++i) p[i]=uint8_t(n>>(8*i));}
cJSON *parse_text(const char *text,size_t n) {
    if(std::memchr(text,0,n)) return nullptr;
    // cJSON's default nesting limit is too deep for the MCU's 4 KiB stack.
    // Bound recursion before parsing, without interpreting brackets in strings.
    unsigned depth=0; bool quoted=false,escaped=false;
    for(size_t i=0;i<n;++i) {
        const char c=text[i];
        if(quoted) {
            if(escaped) {
                if(c=='u' && i+4<n && !std::memcmp(text+i+1,"0000",4)) return nullptr;
                escaped=false;
            }
            else if(c=='\\') escaped=true;
            else if(c=='"') quoted=false;
        } else if(c=='"') quoted=true;
        else if(c=='{' || c=='[') {if(++depth>8) return nullptr;}
        else if(c=='}' || c==']') {if(!depth) return nullptr; --depth;}
    }
    return cJSON_ParseWithOpts(text,nullptr,true);
}
bool read_order(Work &w) {
    if(editing) {
        const auto &d=draft(); w.priorities=d.priorities;
        std::memcpy(w.order,d.order,sizeof w.order); return true;
    }
    if(!storage::state_available()) return true; // Session-only catalog, no automatic formatting.
    size_t n=0; const auto status=storage::file_size(kOrder,n);
    if(status==storage::FileStatus::Missing) return true;
    if(status!=storage::FileStatus::File || n>=sizeof w.text ||
       !storage::read_file(kOrder,w.text,sizeof w.text-1,n)) return fail("Cannot read game order");
    w.text[n]=0;
    cJSON *root=parse_text(w.text,n);
    bool ok=cJSON_IsArray(root) && cJSON_GetArraySize(root)<=int(storage::kMaxRoms);
    if(ok) for(auto *item=root->child;item;item=item->next) {
        if(!cJSON_IsString(item) || !leaf(item->valuestring)) {ok=false; break;}
        if(rank(w,item->valuestring)==storage::kMaxRoms)
            std::snprintf(w.order[w.priorities++],256,"%s",item->valuestring);
    }
    cJSON_Delete(root);
    return ok || fail("Invalid order; reset order");
}
uint32_t order_crc(const Work &w) {
    // Fixed slots are zero initialized, so unused tails have a stable identity.
    return crc(reinterpret_cast<const uint8_t *>(w.order),w.priorities*256);
}
void visit(const char *name,uint32_t bytes,void *context) {
    auto &w=*static_cast<Work *>(context);
    const auto n=std::strlen(name);
    if(n<4 || strcasecmp(name+n-4,".nes")) return;
    if(!leaf(name)) {++w.warnings; return;}
    ++w.total;
    w.sources+=hash(&bytes,sizeof bytes,hash(name,n+1));
    storage::Entry entry{}; std::snprintf(entry.name,sizeof entry.name,"%s",name); entry.bytes=bytes;
    // Keep the first 128 by the final ordering, independent of directory order.
    size_t at=0; while(at<w.count && before(w,w.entries[at],entry)) ++at;
    if(at<storage::kMaxRoms) {
        const size_t end=std::min(w.count,storage::kMaxRoms-1);
        for(size_t i=end;i>at;--i) w.entries[i]=w.entries[i-1];
        w.entries[at]=entry; w.count=std::min(w.count+1,storage::kMaxRoms);
    }
    tick(w);
}
void begin_index(Work &w) {
    w.strings=32+w.count*16; w.used=w.strings+1;
    std::memset(w.index,0,w.used);
    std::memcpy(w.index,"GIDX",4); w.index[4]=1; w.index[6]=32;
    w.index[8]=uint8_t(w.count); w.index[10]=16;
    u32(w.index+12,w.strings); w.index[28]=58; w.index[29]=80; w.index[30]=1;
}
uint32_t append(Work &w,const char *text) {
    if(!*text) return 0;
    const size_t n=std::strlen(text)+1;
    if(n>sizeof w.index-w.used) return UINT32_MAX;
    const auto offset=w.used-w.strings;
    std::memcpy(w.index+w.used,text,n); w.used+=n; return offset;
}
bool record(Work &w,size_t i,const char *name,const char *title,const char *desc,uint32_t flags) {
    auto *p=w.index+32+i*16;
    for(const auto *s:{name,title,desc}) {
        const auto at=append(w,s); if(at==UINT32_MAX) return fail("Game descriptions exceed 256 KiB");
        u32(p,at); p+=4;
    }
    u32(p,flags); return true;
}
bool metadata(Work &w,size_t i,bool build) {
    const auto &entry=w.entries[i];
    char path[280],fallback[256];
    std::snprintf(fallback,sizeof fallback,"%s",entry.name); *std::strrchr(fallback,'.')=0;
    std::snprintf(path,sizeof path,"/system/roms/%s",entry.name);
    auto *ext=std::strrchr(path,'.'); std::strcpy(ext,".565");
    size_t n=0; auto status=storage::file_size(path,n);
    if(status==storage::FileStatus::Error) return fail("Cannot read game resources");
    uint32_t flags=status==storage::FileStatus::File && n==58*80*2?1:0;
    uint64_t source=hash(&flags,sizeof flags,hash(entry.name,std::strlen(entry.name)+1));
    std::strcpy(ext,".json"); status=storage::file_size(path,n);
    if(status==storage::FileStatus::Error) return fail("Cannot read game metadata");
    cJSON *root=nullptr; const char *title=fallback,*desc="";
    if(status==storage::FileStatus::File) {
        source=hash(&n,sizeof n,source);
        if(n>=kTextBytes) ++w.warnings;
        else {
            if(!storage::read_file(path,w.text,kTextBytes-1,n)) return fail("Cannot read game metadata");
            source=hash(w.text,n,source); w.text[n]=0;
            if(!build) {w.sources+=source; tick(w); return true;}
            root=parse_text(w.text,n);
            auto *name=cJSON_GetObjectItemCaseSensitive(root,"name"),*description=cJSON_GetObjectItemCaseSensitive(root,"desc");
            bool valid=cJSON_IsObject(root) && (!name || (cJSON_IsString(name) && ascii(name->valuestring))) &&
                (!description || (cJSON_IsString(description) && ascii(description->valuestring,true)));
            // Duplicate keys are ambiguous; do not silently choose one.
            if(valid) for(auto *a=root->child;a;a=a->next) for(auto *b=a->next;b;b=b->next)
                if(!std::strcmp(a->string,b->string)) valid=false;
            if(valid) {if(name && *name->valuestring) title=name->valuestring; if(description) desc=description->valuestring;}
            else ++w.warnings;
        }
    }
    if(!build) {w.sources+=source; tick(w); return true;}
    const size_t reserve=(w.count-i-1)*512;
    if(std::strlen(entry.name)+std::strlen(title)+std::strlen(desc)+3+reserve>sizeof w.index-w.used) {
        title=fallback; desc=""; ++w.warnings;
    }
    const bool ok=record(w,i,entry.name,title,desc,flags|(*desc?2:0));
    cJSON_Delete(root); tick(w); return ok;
}
void finish_index(Work &w) {u32(w.index+16,w.used); u32(w.index+20,crc(w.index,w.used,true));}
bool stamp_matches(Stamp &s) {
    size_t n=0;
    return storage::read_file(kStamp,&s,sizeof s,n) && n==sizeof s && s.magic==Stamp{}.magic &&
        s.index==storage::catalog_crc() && s.priorities<=storage::kMaxRoms && !s.reserved && s.total>=storage::count;
}
bool persist(Work &w) {
    Stamp next; next.index=crc(w.index,w.used,true); next.order=order_crc(w);
    next.priorities=w.priorities; next.sources=w.sources; next.total=w.total;
    bool ok=storage::mark_library_changed();
    if(ok) ok=storage::write_state("/state/games.idx",w.index,w.used,w.progress);
    if(ok) ok=storage::write_state(kStamp,&next,sizeof next,w.progress);
    if(ok) ok=storage::remove_state("/state/library.dirty");
    if(ok) {current=next; have_stamp=true;}
    else {have_stamp=false; fail("Game list ready; not saved");}
    return ok;
}
bool write_order(Work &w) {
    cJSON *root=cJSON_CreateArray(); if(!root) return fail("Out of memory");
    bool ok=true;
    for(size_t i=0;ok && i<w.priorities;++i) {
        auto *item=cJSON_CreateString(w.order[i]);
        ok=item && cJSON_AddItemToArray(root,item);
        if(!ok) cJSON_Delete(item);
    }
    ok=ok && cJSON_PrintPreallocated(root,w.text,sizeof w.text,false); cJSON_Delete(root);
    if(!ok) return fail("Cannot encode game order");
    if(!storage::mark_library_changed() || !storage::write_state(kOrder,w.text,std::strlen(w.text),w.progress))
        return fail("Game order not saved");
    return true;
}
bool encode_catalog(Work &w) {
    w.count=storage::count;
    std::copy_n(storage::entries,w.count,w.entries);
    std::sort(w.entries,w.entries+w.count,[&](const auto &a,const auto &b){return before(w,a,b);});
    begin_index(w);
    for(size_t i=0;i<w.count;++i) {
        size_t old=0; while(old<storage::count && std::strcmp(w.entries[i].name,storage::entries[old].name)) ++old;
        if(old==storage::count || !record(w,i,w.entries[i].name,storage::title(old),storage::description(old),w.entries[i].flags)) return false;
    }
    finish_index(w);
    return true;
}
bool reorder(Work &w) {
    // Selection of the first 128 may change; stage the result without writes.
    if(!have_stamp || current.total>storage::kMaxRoms) {
        if(!storage::visit_roms(visit,&w)) return fail("Game directory read failed");
        for(size_t i=0;i<w.count;++i) if(!metadata(w,i,false)) return false;
        begin_index(w);
        for(size_t i=0;i<w.count;++i) if(!metadata(w,i,true)) return false;
        finish_index(w);
    } else {
        if(!encode_catalog(w)) return false;
        w.sources=current.sources; w.total=current.total;
    }
    if(!editing) {
        new(&draft()) Draft{};
        draft().original_bytes=storage::copy_catalog(draft().original,sizeof draft().original);
        if(!draft().original_bytes) return fail("Game list unavailable");
    }
    auto &d=draft();
    d.priorities=w.priorities; d.sources=w.sources; d.total=w.total;
    std::memcpy(d.order,w.order,sizeof d.order);
    editing=true;
    if(!storage::install_catalog(w.index,w.used)) {discard(); return fail("Generated game index invalid");}
    // Restoring the saved priority list needs no Flash write on exit.
    if(have_stamp && order_crc(w)==current.order) discard();
    return true;
}
}
bool custom_order() {return editing?draft().priorities!=0:!have_stamp || current.priorities!=0;}
bool pending() {return editing;}
void discard() {
    if(editing) storage::install_catalog(draft().original,draft().original_bytes);
    editing=false;
}
bool save() {
    if(!editing) return true;
    auto &w=*new(storage::library_scratch()) Work{};
    if(!read_order(w) || !encode_catalog(w)) return false;
    w.sources=draft().sources; w.total=draft().total;
    if(!write_order(w)) return false;
    // The order file is authoritative. After its atomic commit, a failed index
    // cache update is recoverable via dirty on boot, not an unsaved order edit.
    editing=false;
    if(!persist(w)) fail("Game order saved; index pending");
    return true;
}
bool move_to_front(size_t index,storage::Progress progress) {
    storage::error[0]=0;
    if(!storage::indexed() || index>=storage::count) return fail("Game list unavailable");
    if(index==0) return true;
    auto &w=*new(storage::library_scratch()) Work{}; w.progress=progress;
    if(!read_order(w)) return false;
    const char *name=storage::entries[index].name;
    const auto at=rank(w,name);
    if(at==storage::kMaxRoms && w.priorities==storage::kMaxRoms) return fail("Order full; reset order first");
    const auto end=at<w.priorities?at:w.priorities++;
    for(size_t i=end;i>0;--i) std::memcpy(w.order[i],w.order[i-1],256);
    std::memset(w.order[0],0,256); std::snprintf(w.order[0],256,"%s",name);
    return reorder(w);
}
bool reset_order(storage::Progress progress) {
    storage::error[0]=0;
    if(!storage::indexed()) return fail("Rebuild game list first");
    auto &w=*new(storage::library_scratch()) Work{}; w.progress=progress;
    return reorder(w);
}
}
namespace storage {
bool scan(bool force,Progress progress) {
    using namespace library;
    error[0]=0;
    if(pending() && !library::save()) return false; // Explicit rescan must not drop an edit.
    if(!mounted()) return fail("Invalid /system FAT");
    const bool loaded=load_catalog();
    have_stamp=loaded && stamp_matches(current);
    size_t dirty_bytes=0;
    const bool dirty=file_size("/state/library.dirty",dirty_bytes)!=FileStatus::Missing;
    if(loaded && have_stamp && !dirty && !force) return true;
    auto &w=*new(library_scratch()) Work{}; w.progress=progress;
    tick(w);
    const bool order_valid=read_order(w);
    if(!order_valid) {w.priorities=0; std::memset(w.order,0,sizeof w.order);}
    if(!visit_roms(visit,&w)) return fail("Game directory read failed");
    for(size_t i=0;i<w.count;++i) if(!metadata(w,i,false)) return false;
    const bool unchanged=order_valid && loaded && have_stamp && current.sources==w.sources && current.order==order_crc(w) && !force;
    if(unchanged) return remove_state("/state/library.dirty") || fail("Cannot clear library change flag");
    begin_index(w);
    for(size_t i=0;i<w.count;++i) if(!metadata(w,i,true)) return false;
    finish_index(w);
    if(!order_valid) {
        install_catalog(w.index,w.used);
        have_stamp=false;
        return fail("Invalid order; reset order");
    }
    const bool saved=persist(w);
    if(!install_catalog(w.index,w.used)) return fail("Generated game index invalid");
    if(saved && w.total>kMaxRoms) fail("Showing first 128 games");
    else if(saved && w.warnings) fail("Some game details were skipped");
    return saved;
}
}
