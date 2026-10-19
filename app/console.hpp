#pragma once
#include <cstddef>
#include <cstdint>

namespace console {
// Terminal behavior follows native's RtcConsole: raw echo, CRLF and BS/DEL.
// CDC1 remains a byte stream and must never pass through this editor.
class Line {
public:
    using Write = void (*)(const char *, size_t);
    using Command = void (*)(const char *);
    Line(Write write, Command command) : write_(write), command_(command) {}
    void reset() {used_=0; rejected_=Rejection::None; after_cr_=false;}
    void feed(uint8_t c) {
        if(c=='\n' && after_cr_) {after_cr_=false; return;}
        after_cr_=c=='\r';
        if(c=='\r' || c=='\n') {
            write_("\r\n",2);
            if(rejected_==Rejection::TooLong) {
                constexpr char message[]="Console: line too long; command discarded\r\n";
                write_(message,sizeof(message)-1);
            } else if(rejected_==Rejection::Unsupported) {
                constexpr char message[]="Console: unsupported character; command discarded\r\n";
                write_(message,sizeof(message)-1);
            } else if(used_) {
                line_[used_]=0;
                command_(line_);
            }
            used_=0; rejected_=Rejection::None; return;
        }
        if(c==3) {reset(); write_("^C\r\n",4); return;}
        if(rejected_!=Rejection::None) return;
        if(c=='\b' || c==127) {
            if(used_) {--used_; write_("\b \b",3);}
            return;
        }
        if(c=='\t') c=' ';
        if(c<32 || c>126) {rejected_=Rejection::Unsupported; return;}
        if(used_+1>=sizeof(line_)) {rejected_=Rejection::TooLong; return;}
        char character=static_cast<char>(c);
        line_[used_++]=character;
        write_(&character,1);
    }
private:
    enum class Rejection {None, TooLong, Unsupported};
    Write write_;
    Command command_;
    char line_[96]{};
    size_t used_=0;
    Rejection rejected_=Rejection::None;
    bool after_cr_=false;
};
}
