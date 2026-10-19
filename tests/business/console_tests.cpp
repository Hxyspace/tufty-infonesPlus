#include "console.hpp"
#include <cassert>
#include <string>
#include <vector>

static std::string output;
static std::vector<std::string> commands;
static void echo(const char *p,size_t n) {output.append(p,n);}
static void command(const char *s) {commands.emplace_back(s); output+="OK\r\n";}
static void type(console::Line &line,const std::string &text) {
    for(unsigned char c:text) line.feed(c);
}
int main() {
    console::Line line(echo,command);
    type(line,"help\r\nstatus\nstatus\r");
    assert((commands==std::vector<std::string>{"help","status","status"}));
    assert(output=="help\r\nOK\r\nstatus\r\nOK\r\nstatus\r\nOK\r\n");
    output.clear(); commands.clear();
    // Both common terminal Backspace encodings erase the visible character.
    type(line,"statux\bstatu\x7f");
    type(line,"\x03");
    assert(commands.empty() && output.find("\b \b")!=std::string::npos);
    assert(output.substr(output.size()-4)=="^C\r\n");
    output.clear();
    type(line,"statux\bs\r\n");
    assert(commands.size()==1 && commands[0]=="status");
    assert(output=="statux\b \bs\r\nOK\r\n");
    commands.clear(); output.clear();
    type(line,"statux\x7fs\n");
    assert(commands.size()==1 && commands[0]=="status");
    commands.clear(); output.clear();
    // A disconnected terminal's partial command must not execute after reopen.
    type(line,"usb "); line.reset(); type(line,"status\n");
    assert(commands.size()==1 && commands[0]=="status");
    commands.clear(); output.clear();
    // Invalid/long input cannot execute a valid-looking command prefix.
    type(line,"usb disk"+std::string(100,' ')+'\n');
    assert(commands.empty() && output.find("line too long")!=std::string::npos);
    type(line,"bootsel\x1b[A\n");
    assert(commands.empty() && output.find("unsupported character")!=std::string::npos);
    type(line,"usb disk\x03\n");
    assert(commands.empty());
    type(line,"help\n");
    assert(commands.size()==1 && commands[0]=="help");
}
