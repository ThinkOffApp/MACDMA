#pragma once
#include <stdint.h>
namespace cx5 {
enum class CommandWait { complete, removed, timeout };
// Startup/control only. The data path never waits here. Spin for at most
// 200us, then yield in 1ms steps; removal is checked before every DMA poll.
template<class Ready,class Gone,class Delay,class Sleep>
CommandWait wait_command(Ready ready,Gone gone,Delay delay,Sleep sleep,uint32_t &polls) {
    polls=0;
    for (unsigned n=0;n<=5040;++n) {
        if (gone()) return CommandWait::removed;
        ++polls;
        if (ready()) return CommandWait::complete;
        if (n==5040) break;
        if (n<40) delay(5); else sleep(1);
    }
    return CommandWait::timeout;
}
}
