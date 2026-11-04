#include "shell.hpp"
#include "console.hpp"
#include "storage.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

namespace shell {
namespace {
constexpr size_t capacity=16*1024;
Write write_output=nullptr;
Execute execute_device=nullptr;
Available files_available=nullptr;
char cwd[256]="/",path[256]{};
enum class Job {None,Change,List,ListLong,Cat};
Job job=Job::None;
char *output=nullptr;
size_t used=0,sent=0;
bool running=false,aborted=false,prompt_pending=false;
struct Command {const char *name,*args,*description; bool required=false;};
// Single discovery table for built-ins and existing device handlers. Business
// actions remain in their owning modules; this table supplies routing/help.
constexpr Command commands[]={
    {"help","[command]","Show commands or command help"},
    {"pwd","","Print current directory"},
    {"ls","[-l] [path]","List names; -l includes size in bytes"},
    {"cd","[path]","Change directory (default /)"},
    {"cat","<file>","Print UTF-8 text, at most 16 KiB",true},
    {"status","","Show storage and controller status"},
    {"nes stats","","Show NES/LCD timing and memory statistics"},
    {"menu stats","","Show library timing and cover statistics"},
    {"lcd te","[on|off]","Query/set TE sync for this session"},
    {"usb disk","","Restart in USB disk mode"},
    {"bootsel","","Restart in firmware update mode"},
    {"rescan","","Rebuild game index"},
    {"save","","Save configuration"},
    {"brightness","<1..100>","Set screen brightness",true},
    {"padid","<1..65535>","Set serial/TGP controller ID",true},
    {"swapab","<0|1>","Set A/B swap",true},
    {"pad status","","Show input sources"},
    {"serial","<on|off>","Enable/disable serial controller",true},
    {"power status","","Show sleep and wake status"},
    {"power sleep","[seconds]","Sleep; timed duration 1..4294967 seconds"},
    {"power idle","<0..86400>","Set menu idle timeout; 0 disables",true},
    {"ble status","","Show Bluetooth status and diagnostics"},
    {"ble on","","Enable Bluetooth"},
    {"ble off","","Disable Bluetooth"},
    {"ble scan","","Reconnect using current policy"},
    {"ble discover","[stop]","Discover nearby controllers / stop discovery"},
    {"ble devices","","List discovered controllers"},
    {"ble connect","<MAC> <public|random>","Connect without changing whitelist",true},
    {"ble whitelist","[clear]","Show/clear whitelist"},
    {"ble allow","<MAC> <public|random>","Add to whitelist",true},
    {"ble forget","<MAC> <public|random>","Remove from whitelist",true},
    {"ble prefer","<MAC> <public|random>","Set preferred whitelist controller",true},
    {"ble mode","<auto|whitelist>","Set connection policy",true},
    {"ble target","auto | <MAC> <public|random>","Set automatic or explicit target",true},
    {"ble trace","<on|off>","Enable/disable HID diagnostics",true},
};
void print(const char *s) {if(write_output) write_output(s,std::strlen(s));}
void echo(const char *s,size_t n) {if(write_output) write_output(s,n);}
bool busy() {return job!=Job::None || output || running;}
void ready() {if(!busy()) prompt();}
bool prefix(const char *s,const char *p) {
    const size_t n=std::strlen(p);return !std::strncmp(s,p,n) && (!s[n] || s[n]==' ');
}
void usage(const Command &c) {
    char line[256];std::snprintf(line,sizeof line,"  %s%s%s\r\n    %s\r\n",c.name,*c.args?" ":"",c.args,c.description);print(line);
}
void help(const char *topic) {
    if(!*topic) {
        print("Commands:\r\n");
        unsigned column=0;
        for(size_t i=0;i<sizeof commands/sizeof *commands;++i) {
            const size_t n=std::strcspn(commands[i].name," ");bool seen=false;
            for(size_t j=0;j<i;++j)
                if(std::strcspn(commands[j].name," ")==n && !std::strncmp(commands[j].name,commands[i].name,n)) {seen=true;break;}
            if(seen) continue;
            char cell[24];std::snprintf(cell,sizeof cell,"  %-12.*s",int(n),commands[i].name);print(cell);
            if(++column%5==0) print("\r\n");
        }
        if(column%5) print("\r\n");
        print("Use <command> --help or help <command>.\r\n"
              "Paths: /rom /system /state (read-only shell). Quote names with spaces.\r\n");return;
    }
    bool found=false;
    for(const auto &c:commands) if(prefix(c.name,topic)) {usage(c);found=true;}
    if(!found) print("Unknown command; type help\r\n");
}
// Bounded argv parser: whitespace and single/double quotes, no expansion.
int tokenize(char *text,char **args,int limit) {
    char *read=text,*out=text;int count=0;
    while(*read) {
        while(*read==' ' || *read=='\t') ++read;
        if(!*read) break;
        if(count==limit) return -1;
        args[count++]=out;char quote=0;
        while(*read && (quote || (*read!=' ' && *read!='\t'))) {
            const char c=*read++;
            if(c=='\'' || c=='"') {if(!quote) quote=c;else if(quote==c) quote=0;else *out++=c;}
            else *out++=c;
        }
        if(quote) return -1;
        while(*read==' ' || *read=='\t') ++read;
        *out++=0;
    }
    return count;
}
bool resolve(const char *input) {
    std::strcpy(path,*input=='/'?"/":cwd);
    const char *p=input;
    while(*p) {
        while(*p=='/') ++p;
        const char *start=p;while(*p && *p!='/') ++p;
        const size_t n=p-start;if(!n) break;
        if(n==1 && *start=='.') continue;
        if(n==2 && start[0]=='.' && start[1]=='.') {
            char *slash=std::strrchr(path,'/');if(slash==path) path[1]=0;else *slash=0;
            continue;
        }
        for(size_t i=0;i<n;++i) if(static_cast<unsigned char>(start[i])<32 || start[i]==':' || start[i]=='\\') return false;
        size_t length=std::strlen(path);const bool separator=length>1;
        if(length+separator+n>=sizeof path) return false;
        if(separator) path[length++]='/';
        std::memcpy(path+length,start,n);path[length+n]=0;
    }
    return true;
}
void dispatch(const char *line) {
    char text[256];std::strcpy(text,line);char *args[8];const int argc=tokenize(text,args,8);
    if(argc<0) {print("Invalid quoting or too many arguments\r\n");return;}
    if(!argc) return;
    char canonical[256]{};
    const bool explicit_help=!std::strcmp(args[0],"help");
    bool requested_help=explicit_help;
    for(int i=explicit_help?1:0;i<argc;++i) {
        if(!std::strcmp(args[i],"--help") || !std::strcmp(args[i],"-h")) {requested_help=true;continue;}
        if(*canonical) std::strcat(canonical," ");
        std::strcat(canonical,args[i]);
    }
    if(requested_help) {help(canonical);return;}
    if(!*canonical) return;
    const Command *match=nullptr;bool group=false;
    for(const auto &c:commands) {
        if(prefix(canonical,c.name) && (!match || std::strlen(c.name)>std::strlen(match->name))) match=&c;
        if(prefix(c.name,canonical)) group=true;
    }
    if(!match) {if(group) help(canonical);else print("Unknown command; type help\r\n");return;}
    if(match->required && !std::strcmp(canonical,match->name)) {usage(*match);return;}
    if(!std::strcmp(args[0],"pwd")) {if(argc!=1) usage(*match);else {print(cwd);print("\r\n");}return;}
    Job next=Job::None;
    if(!std::strcmp(args[0],"ls")) next=Job::List;
    if(!std::strcmp(args[0],"cd")) next=Job::Change;
    if(!std::strcmp(args[0],"cat")) next=Job::Cat;
    if(next!=Job::None) {
        const char *target=nullptr;
        if(next==Job::List) {
            for(int i=1;i<argc;++i) {
                if(!std::strcmp(args[i],"-l") && next==Job::List) next=Job::ListLong;
                else if(args[i][0]=='-' || target) {usage(*match);return;}
                else target=args[i];
            }
        } else {
            if(argc>2) {usage(*match);return;}
            if(argc==2) target=args[1];
        }
        if(!files_available || !files_available()) {print("Files unavailable; return to the game library\r\n");return;}
        if(!resolve(target?target:(next==Job::Change?"/":"."))) {print("Invalid or too long path\r\n");return;}
        job=next;return;
    }
    if(!*match->args && std::strcmp(canonical,match->name)) {usage(*match);return;}
    if(execute_device) execute_device(canonical);
}
console::Line editor(echo,dispatch,ready);
bool append(const char *s,size_t n) {
    if(aborted || n>capacity-used) return false;
    std::memcpy(output+used,s,n);used+=n;return true;
}
bool entry(const char *name,bool directory,size_t bytes,void *context) {
    char row[320];
    const int n=*static_cast<const bool *>(context)
        ?std::snprintf(row,sizeof row,"%10lu  %s%s\r\n",static_cast<unsigned long>(bytes),name,directory?"/":"")
        :std::snprintf(row,sizeof row,"%s%s\r\n",name,directory?"/":"");
    return n>0 && size_t(n)<sizeof row && append(row,n);
}
// A prefix heuristic, not a file-format detector. A sample may end mid-codepoint.
bool text_prefix(const char *data,size_t size,bool partial) {
    for(size_t i=0;i<size;) {
        uint32_t c=static_cast<unsigned char>(data[i++]);unsigned continuation=0;uint32_t minimum=0;
        if(c>=0xc2 && c<=0xdf) {c&=31;continuation=1;minimum=0x80;}
        else if(c>=0xe0 && c<=0xef) {c&=15;continuation=2;minimum=0x800;}
        else if(c>=0xf0 && c<=0xf4) {c&=7;continuation=3;minimum=0x10000;}
        else if(c>=0x80) return false;
        if(continuation>size-i) {
            if(!partial) return false;
            while(i<size) if((static_cast<unsigned char>(data[i++])&0xc0)!=0x80) return false;
            return true;
        }
        for(unsigned j=0;j<continuation;++j) {const auto next=static_cast<unsigned char>(data[i++]);if((next&0xc0)!=0x80)return false;c=(c<<6)|(next&63);}
        if(c<minimum || c>0x10ffff || (c>=0xd800 && c<=0xdfff)) return false;
        if((c<32 && c!='\n' && c!='\r' && c!='\t') || (c>=127 && c<=159)) return false;
    }
    return true;
}
}
void init(Write write,Execute execute,Available available) {
    write_output=write;execute_device=execute;files_available=available;reset();
}
void reset() {cancel();editor.reset();std::strcpy(cwd,"/");}
void prompt() {prompt_pending=true;poll();}
void cancel() {
    aborted=true;job=Job::None;prompt_pending=false;
    if(!running) {std::free(output);output=nullptr;used=sent=0;}
}
void feed(uint8_t byte) {
    if(busy()) {
        if(byte==3) {cancel();editor.reset();print("^C\r\n");if(!running) prompt();}
        return;
    }
    editor.feed(byte);
}
void service() {
    if(!busy() || running) return;
    if(!files_available || !files_available()) {cancel();print("Files unavailable\r\n");prompt();return;}
    if(job!=Job::None) {
        running=true;aborted=false;
        const Job current=job;job=Job::None;
        storage::PathInfo info{};
        const char *error=nullptr;
        if(!storage::inspect_path(path,info)) error="Path not found or unavailable\r\n";
        else if(current==Job::Change) {
            if(info.directory) std::strcpy(cwd,path);else error="Not a directory\r\n";
        } else if(current==Job::Cat && info.directory) error="Is a directory\r\n";
        else if(current==Job::Cat) {
            char sample[512];size_t n=0;
            if(!storage::read_file_prefix(path,sample,sizeof sample,n)) error="Read failed\r\n";
            else if(!text_prefix(sample,n,info.bytes>n)) error="binary\r\n";
            else if(info.bytes>capacity) error="File too large (cat limit: 16 KiB)\r\n";
        }
        const size_t allocation=current==Job::Cat?std::max<size_t>(1,info.bytes):capacity;
        if(!error && !aborted && current!=Job::Change && !(output=static_cast<char *>(std::malloc(allocation)))) error="Out of memory\r\n";
        if(!error && !aborted && (current==Job::List || current==Job::ListLong)) {
            bool detailed=current==Job::ListLong;
            if(info.directory) {if(!storage::list_directory(path,entry,&detailed)) error="Listing failed or exceeds 16 KiB\r\n";}
            else entry(std::strrchr(path,'/')+1,false,info.bytes,&detailed);
        } else if(!error && !aborted && current==Job::Cat) {
            if(!storage::read_file(path,output,allocation,used)) error="Read failed\r\n";
        }
        running=false;
        if(aborted || error || current==Job::Change) {
            const bool cancelled=aborted;
            cancel();if(error && !cancelled) print(error);
            prompt();return;
        }
    }
    poll();
}
void poll() {
    if(running) return;
    if(prompt_pending) {
        char line[260];const int n=std::snprintf(line,sizeof line,"[%s]> ",cwd);
        if(write_output && write_output(line,n)) prompt_pending=false;
        return;
    }
    // At most one bounded chunk per call. Backpressure does not block
    // USB, input or rendering, and no open filesystem objects survive a call.
    if(output && sent<used) {
        char chunk[256];size_t n=0,consumed=0;
        while(sent+consumed<used && n+2<=sizeof chunk) {
            const char c=output[sent+consumed];
            if(c=='\n' && ((sent==0 && consumed==0) || output[sent+consumed-1]!='\r')) chunk[n++]='\r';
            const auto byte=static_cast<unsigned char>(c);
            chunk[n++]=((byte<32 && c!='\n' && c!='\r' && c!='\t') || byte==127)?'?':c;
            ++consumed;
        }
        if(!write_output(chunk,n)) return;
        sent+=consumed;
    }
    if(output && sent==used) {
        if(used && output[used-1]!='\n' && !write_output("\r\n",2)) return;
        cancel();prompt();
    }
}
}
