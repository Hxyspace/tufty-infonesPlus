#include "shell.hpp"
#include "storage.hpp"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <map>
#include <string>
#include <vector>

static std::string output,executed;
static bool available=true,blocked=false,cancel_read=false;
static unsigned reads=0,inspections=0;
static std::map<std::string,std::string> files={
    {"/state/note.txt","hello\n世界\n"},
    {"/system/roms/name with spaces.json","{\"title\":\"test\"}"},
    {"/rom/binary",std::string("NES\x1a\0",5)},
    {"/state/empty",""},
    {"/state/bad-utf8",std::string("\xc0\x80",2)},
    {"/state/truncated-utf8",std::string("\xe4\xb8",2)},
    {"/state/terminal","abc\x1b[2J"},
    {"/state/large",std::string(17000,'a')},
    {"/state/split",std::string(511,'a')+"世界\n"},
    {"/state/late-control",std::string(520,'a')+"\x1b[2J"},
    {"/state/file..txt","dots"},
};
static bool write(const char *p,size_t n) {if(blocked)return false;output.append(p,n);return true;}
static void execute(const char *s) {executed=s;}
static bool accessible() {return available;}
static void type(const std::string &s) {for(unsigned char c:s) shell::feed(c);}
static void drain() {for(int i=0;i<200;++i) {shell::service();shell::poll();}}
static std::string command(const std::string &s) {output.clear();type(s+"\r\n");drain();return output;}
namespace storage {
bool inspect_path(const char *p,PathInfo &info) {
    ++inspections;const std::string path=p;
    if(path=="/" || path=="/rom" || path=="/system" || path=="/state" || path=="/system/roms") {info={true,0};return true;}
    auto found=files.find(path);if(found==files.end())return false;
    info={false,found->second.size()};return true;
}
bool list_directory(const char *path,DirectoryVisitor visit,void *ctx) {
    if(!std::strcmp(path,"/"))return visit("rom",true,0,ctx) && visit("system",true,0,ctx) && visit("state",true,0,ctx);
    const auto prefix=std::string(path)+"/";
    for(const auto &file:files) if(file.first.find(prefix)==0 && !visit(file.first.c_str()+prefix.size(),false,file.second.size(),ctx))return false;
    return true;
}
bool read_file_prefix(const char *path,void *out,size_t capacity,size_t &actual) {
    ++reads;auto found=files.find(path);if(found==files.end())return false;
    actual=std::min(capacity,found->second.size());std::memcpy(out,found->second.data(),actual);
    if(cancel_read)shell::feed(3);
    return true;
}
bool read_file(const char *path,void *out,size_t capacity,size_t &actual) {
    return read_file_prefix(path,out,capacity,actual);
}
}
int main() {
    shell::init(write,execute,accessible);
    assert(command("")=="\r\n[/]> ");
    output.clear();blocked=true;shell::prompt();shell::poll();assert(output.empty());
    blocked=false;shell::poll();assert(output=="[/]> ");
    assert(command("   ")=="   \r\n[/]> ");
    const auto top=command("help");assert(top.find("Commands:")!=std::string::npos && top.find("pwd")!=std::string::npos && top.find("cat")!=std::string::npos);
    const auto ble=command("ble");assert(ble.find("ble connect <MAC>")!=std::string::npos && ble.size()<2048);
    assert(command("help ble").substr(10)==command("ble --help").substr(12));
    assert(command("ble connect").find("public|random")!=std::string::npos);
    command("  lcd   te  off  ");assert(executed=="lcd te off");
    command("bootsel extra");assert(executed=="lcd te off");
    command("brightness --help");assert(executed=="lcd te off");
    command("power sleep --help");assert(executed=="lcd te off");
    assert(command("usb").find("usb disk")!=std::string::npos && executed=="lcd te off");
    command("status");assert(executed=="status");
    assert(command("ls")=="ls\r\nrom/\r\nsystem/\r\nstate/\r\n[/]> ");
    assert(command("ls -l").find("         0  system/\r\n")!=std::string::npos);
    assert(command("ls /state/file..txt")=="ls /state/file..txt\r\nfile..txt\r\n[/]> ");
    assert(command("ls -l /state/file..txt").find("         4  file..txt\r\n")!=std::string::npos);
    assert(command("ls /state/file..txt -l").find("         4  file..txt\r\n")!=std::string::npos);
    assert(command("ls /state").find("\r\nfile..txt\r\n")!=std::string::npos); // -l does not persist.
    const auto before_invalid=inspections;
    for(const auto *invalid:{"ls -x","ls -l -l","ls /state /rom"})
        assert(command(invalid).find("ls [-l] [path]")!=std::string::npos);
    assert(inspections==before_invalid);
    assert(command("cd /system//roms/.").find("[/system/roms]> ")!=std::string::npos);
    assert(command("ls -l 'name with spaces.json'").find("        16  name with spaces.json\r\n")!=std::string::npos);
    assert(command("cat 'name with spaces.json'").find("{\"title\":\"test\"}")!=std::string::npos);
    assert(command("cd ../../../../state").find("[/state]> ")!=std::string::npos);
    assert(command("cd missing").find("not found")!=std::string::npos);
    assert(command("pwd").find("\r\n/state\r\n")!=std::string::npos);
    assert(command("cat note.txt").find("hello\r\n世界\r\n")!=std::string::npos);
    assert(command("cat file..txt").find("dots\r\n")!=std::string::npos);
    assert(command("cat empty").find("binary")==std::string::npos);
    assert(command("cat split").find("世界\r\n")!=std::string::npos);
    for(const auto *path:{"/rom/binary","bad-utf8","truncated-utf8","terminal"}) {
        const unsigned before=reads;
        const auto reply=command(std::string("cat ")+path);
        assert(reply.find("\r\nbinary\r\n")!=std::string::npos);
        assert(reads==before+1 && reply.find('\x1b')==std::string::npos);
    }
    const auto late=command("cat late-control");assert(late.find("?[2J")!=std::string::npos && late.find('\x1b')==std::string::npos);
    assert(command("cat large").find("16 KiB")!=std::string::npos);
    files["/state/max"]=std::string(16*1024,'a');
    const auto maximum=command("cat max");assert(maximum.find(std::string(16*1024,'a'))!=std::string::npos);
    files["/state/large-binary"]=std::string(17000,'\0');
    assert(command("cat large-binary").find("\r\nbinary\r\n")!=std::string::npos);
    assert(command("cat /state").find("Is a directory")!=std::string::npos);
    assert(command("cd note.txt").find("Not a directory")!=std::string::npos);
    assert(command("cat 'unfinished").find("Invalid quoting")!=std::string::npos);
    assert(command("cd /state/evil:foo").find("Invalid")!=std::string::npos);
    assert(command("cd /stateevil").find("not found")!=std::string::npos);
    assert(command("cd").find("[/]> ")!=std::string::npos);
    const unsigned before=inspections;
    available=false;assert(command("ls").find("return to the game library")!=std::string::npos);
    assert(inspections==before);command("nes stats");assert(executed=="nes stats");available=true;
    output.clear();type("cat /state/split\n");assert(inspections==before); // Deferred, never reads from feed().
    blocked=true;drain();assert(output.find(std::string(300,'a'))==std::string::npos);
    blocked=false;drain();assert(output.find(std::string(511,'a')+"世界")!=std::string::npos);
    output.clear();type("cat /state/note.txt\n");cancel_read=true;drain();cancel_read=false;
    assert(output.find("^C")!=std::string::npos && output.find("hello")==std::string::npos);
    type("ls\n");blocked=true;drain();shell::reset();blocked=false;
    output.clear();drain();assert(output.empty()); // Disconnect releases queued output.
    assert(command("pwd").find("\r\n/\r\n")!=std::string::npos);
    type("cat /state/note.txt\n");available=false;drain();available=true;
    assert(command("pwd").find("[/]> ")!=std::string::npos);
    shell::reset();
}
