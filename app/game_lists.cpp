#include "game_lists.hpp"
#include "storage.hpp"
#include "cJSON.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>
#include <strings.h>

namespace game_lists {
namespace {
constexpr const char *path="/state/game-lists.json";
constexpr size_t text_bytes=5500;
struct Names {char names[limit][256]{}; size_t count=0;};
struct Lists {Names favorites,recent;};
struct Session {
    Lists values,saved;
    uint16_t favorites[limit]{},recent[limit]{};
    uint8_t marks[storage::kMaxRoms/8]{};
    size_t favorite_count=0,recent_count=0;
    uint32_t generation=~0u,idle_at=0;
    bool writable=true,dirty=false,retry=true;
};
static_assert(sizeof(Session)+text_bytes<=16*1024);
Session *session=nullptr;
View current=View::All;
bool suspended=false;
int launching=-1;
bool fail(const char *message) {
    std::snprintf(storage::error,sizeof storage::error,"%s",message); return false;
}
int find(const char *name) {
    for(size_t i=0;i<storage::count;++i) if(!strcasecmp(name,storage::entries[i].name)) return int(i);
    return -1;
}
int find(const Names &names,const char *name) {
    for(size_t i=0;i<names.count;++i) if(!strcasecmp(name,names.names[i])) return int(i);
    return -1;
}
bool valid_name(const char *name) {
    const size_t n=std::strlen(name);
    if(n<=4 || n>255 || strcasecmp(name+n-4,".nes") || std::strstr(name,"..") ||
       std::strpbrk(name,"/\\:\"<>|?*") || name[n-1]==' ') return false;
    for(size_t i=0;i<n;++i) if(uint8_t(name[i])<32 || uint8_t(name[i])>126) return false;
    return true;
}
bool decode(Names &names,const cJSON *array) {
    if(!cJSON_IsArray(array) || cJSON_GetArraySize(array)>int(limit)) return false;
    for(auto *item=array->child;item;item=item->next) {
        if(!cJSON_IsString(item) || !valid_name(item->valuestring)) return false;
        if(find(names,item->valuestring)<0)
            std::snprintf(names.names[names.count++],256,"%s",item->valuestring);
    }
    return true;
}
bool load() {
    size_t n=0;
    const auto status=storage::file_size(path,n);
    if(status==storage::FileStatus::Missing) return true;
    if(status!=storage::FileStatus::File || n>=text_bytes) return fail("Cannot read game lists");
    auto *text=new(std::nothrow) char[text_bytes];
    if(!text) return fail("Game lists: out of memory");
    bool ok=storage::read_file(path,text,text_bytes-1,n) && n<text_bytes && !std::memchr(text,0,n);
    if(n>=text_bytes) n=0;
    text[n]=0;
    // Reject excessive recursion and embedded NUL escapes before cJSON parsing.
    unsigned depth=0; bool quoted=false,escaped=false;
    for(size_t i=0;ok && i<n;++i) {
        const char c=text[i];
        if(quoted) {
            if(escaped) {if(c=='u' && i+4<n && !std::memcmp(text+i+1,"0000",4)) ok=false; escaped=false;}
            else if(c=='\\') escaped=true;
            else if(c=='"') quoted=false;
        } else if(c=='"') quoted=true;
        else if(c=='[' || c=='{') {if(++depth>4) ok=false;}
        else if(c==']' || c=='}') {if(!depth) ok=false; else --depth;}
    }
    cJSON *root=ok?cJSON_ParseWithOpts(text,nullptr,true):nullptr;
    delete[] text;
    ok=cJSON_IsObject(root) && cJSON_GetArraySize(root)==2;
    if(ok) ok=decode(session->values.favorites,cJSON_GetObjectItemCaseSensitive(root,"favorites")) &&
        decode(session->values.recent,cJSON_GetObjectItemCaseSensitive(root,"recent"));
    cJSON_Delete(root);
    if(!ok) {session->values={}; return fail("Invalid game lists");}
    session->saved=session->values; return true;
}
bool equal(const Names &a,const Names &b) {
    if(a.count!=b.count) return false;
    for(size_t i=0;i<a.count;++i) if(std::strcmp(a.names[i],b.names[i])) return false;
    return true;
}
void changed() {
    session->dirty=!equal(session->values.favorites,session->saved.favorites) ||
        !equal(session->values.recent,session->saved.recent);
    session->retry=true; session->idle_at=0;
    session->generation=~0u; refresh();
}
void erase(Names &names,size_t at) {
    for(size_t i=at+1;i<names.count;++i) std::memcpy(names.names[i-1],names.names[i],256);
    std::memset(names.names[--names.count],0,256);
}
void front(Names &names,const char *name) {
    const int old=find(names,name);
    if(old>=0) erase(names,size_t(old));
    else if(names.count==limit) erase(names,limit-1);
    for(size_t i=names.count;i>0;--i) std::memcpy(names.names[i],names.names[i-1],256);
    std::snprintf(names.names[0],256,"%s",name); ++names.count;
}
size_t encode(char *out) {
    char *p=out;
    for(unsigned list=0;list<2;++list) {
        p+=std::sprintf(p,list?",\"recent\":[":"{\"favorites\":[");
        const auto &names=list?session->values.recent:session->values.favorites;
        for(size_t i=0;i<names.count;++i) {
            // Valid leaf names contain neither quotes nor backslashes.
            p+=std::sprintf(p,"%s\"%s\"",i?",":"",names.names[i]);
        }
        *p++=']';
    }
    *p++='}'; *p=0; return size_t(p-out);
}
}
bool open() {
    if(session) return session->writable;
    session=new(std::nothrow) Session{};
    if(!session) return fail("Game lists: out of memory");
    session->writable=load(); refresh(); return session->writable;
}
void close() {
    if(!suspended) delete session;
    session=nullptr; suspended=false; launching=-1; current=View::All;
}
void refresh() {
    if(!session || session->generation==storage::generation()) return;
    session->generation=storage::generation();
    session->favorite_count=session->recent_count=0;
    std::memset(session->marks,0,sizeof session->marks);
    for(size_t i=0;i<session->values.favorites.count;++i) {
        const int index=find(session->values.favorites.names[i]);
        if(index>=0) {
            session->favorites[session->favorite_count++]=uint16_t(index);
            session->marks[index/8]|=uint8_t(1u<<(index%8));
        }
    }
    for(size_t i=0;i<session->values.recent.count;++i) {
        const int index=find(session->values.recent.names[i]);
        if(index>=0) session->recent[session->recent_count++]=uint16_t(index);
    }
}
View view() {return current;}
void view(View value) {current=value;}
size_t count() {
    if(current==View::All) return storage::count;
    return !session?0:current==View::Favorites?session->favorite_count:session->recent_count;
}
int game(int position) {
    if(position<0 || size_t(position)>=count()) return -1;
    return current==View::All?position:current==View::Favorites?session->favorites[position]:session->recent[position];
}
int position(int index) {
    if(index<0 || size_t(index)>=storage::count) return -1;
    if(current==View::All) return index;
    for(size_t i=0;i<count();++i) if(game(int(i))==index) return int(i);
    return -1;
}
bool favorite(size_t index) {
    return session && index<storage::count && (session->marks[index/8]&(1u<<(index%8)));
}
bool toggle(size_t index) {
    if(!session || !session->writable) return fail("Game lists unavailable");
    if(index>=storage::count) return false;
    auto &names=session->values.favorites;
    const int old=find(names,storage::entries[index].name);
    if(old>=0) erase(names,size_t(old));
    else {
        if(names.count==limit) {
            int missing=-1;
            for(size_t i=0;i<names.count;++i) if(find(names.names[i])<0) missing=int(i);
            if(missing<0) return fail("Favorites full (10)");
            erase(names,size_t(missing));
        }
        front(names,storage::entries[index].name);
    }
    changed(); return true;
}
bool pending() {return session && session->dirty;}
bool save() {
    if(!pending()) return true;
    session->retry=false;
    if(!session->writable) return fail("Game lists unavailable");
    char *buffer=suspended?reinterpret_cast<char *>(session+1):new(std::nothrow) char[text_bytes];
    if(!buffer) return fail("Game lists: out of memory");
    const auto bytes=encode(buffer);
    const bool ok=storage::write_state(path,buffer,bytes);
    if(!suspended) delete[] buffer;
    session->retry=false;
    if(!ok) return fail("Game lists not saved");
    session->saved=session->values; session->dirty=false; return true;
}
void discard() {
    if(!session) return;
    session->values=session->saved; changed();
}
void idle(uint32_t now,bool busy) {
    if(!session || suspended) return;
    if(busy || !session->idle_at) session->idle_at=now;
    if(!busy && pending() && session->retry && uint32_t(now-session->idle_at)>=2000) save();
}
bool suspend(void *workspace,size_t bytes,int index) {
    if(!session) return true; // Lack of menu memory must not prevent playing.
    if(suspended || pending() || bytes<sizeof(Session)+text_bytes ||
       reinterpret_cast<uintptr_t>(workspace)%alignof(Session)) return fail("Game lists still in use");
    auto *copy=new(workspace) Session(*session);
    delete session; session=copy; suspended=true; launching=index; return true;
}
void started() {
    if(!suspended || launching<0) return;
    const int index=launching; launching=-1;
    if(!session->writable || size_t(index)>=storage::count) return;
    front(session->values.recent,storage::entries[index].name); changed(); save();
}
bool resume() {
    if(!suspended) return true;
    auto *copy=new(std::nothrow) Session(*session);
    if(!copy) return fail("Game lists: out of memory");
    session=copy; suspended=false; launching=-1;
    return true;
}
size_t session_bytes() {return session && !suspended?sizeof(Session):0;}
}
