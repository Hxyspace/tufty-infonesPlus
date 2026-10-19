#include "disk_ui.hpp"
#include "disk_activity.hpp"
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

static unsigned services=0;
static bool service() {++services; return true;}
static bool cancel() {++services; return false;}

int main() {
    usb::DiskActivity activity;
    assert(!activity.busy(0));
    activity.complete(100); assert(!activity.busy(100)); // A status probe is not a transfer.
    activity.begin(200); assert(activity.busy(100000)); // Stalled data phase.
    activity.complete(100000); assert(activity.busy(100499)); assert(!activity.busy(100500));
    activity.begin(0xfffffff0u); activity.complete(0xfffffff0u);
    assert(activity.busy(483)); assert(!activity.busy(484));
    activity.begin(1000); activity.complete(1001);
    assert(activity.busy(1200)); // Bridge gaps between file writes.
    activity.begin(1200); activity.complete(1201);
    assert(activity.busy(1700)); assert(!activity.busy(1701));
    activity.begin(1000); activity.reset(); assert(!activity.busy(1001)); // USB disconnect.

    constexpr size_t pixels=320*240,guard=32;
    std::vector<uint16_t> frame(pixels+guard*2,0x1234);
    uint16_t *out=frame.data()+guard;
    assert(disk_ui::draw(out,disk_ui::State::Ready,33,service));
    assert(services>=40); // USB is serviced throughout clearing, stars and text.
    services=0;
    assert(!disk_ui::draw(out,disk_ui::State::Ready,33,cancel));
    assert(services==1); // Stop at the first file operation, never publish it.
    const disk_ui::State states[]={disk_ui::State::Ready,disk_ui::State::Transferring,
        disk_ui::State::Unavailable,disk_ui::State::Failed,disk_ui::State::Ejected};
    const char *names[]={"ready","transferring","unavailable","failed","ejected"};
    for(unsigned state=0;state<5;++state) {
        for(int i=0;i<120;++i) disk_ui::draw(out,states[state],i==60?0xffffffffu:33u);
        for(size_t i=0;i<guard;++i) assert(frame[i]==0x1234 && frame[pixels+guard+i]==0x1234);
        // The entire screen remains exactly pixel doubled, including all glyphs.
        for(int y=0;y<240;y+=2) for(int x=0;x<320;x+=2) {
            size_t at=y*320+x;
            assert(out[at]==out[at+1] && out[at]==out[at+320] && out[at]==out[at+321]);
        }
        if(const char *dir=std::getenv("TUFTY_DISK_PREVIEW")) {
            std::ofstream file(std::string(dir)+"/disk-"+names[state]+".ppm",std::ios::binary);
            assert(file); file<<"P6\n320 240\n255\n";
            for(size_t i=0;i<pixels;++i) {
                uint16_t pixel=out[i];
                char rgb[]={char((pixel>>11)*255/31),char(((pixel>>5)&63)*255/63),char((pixel&31)*255/31)};
                file.write(rgb,3);
            }
        }
    }
}
