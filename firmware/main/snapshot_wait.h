#pragma once
#include <stdint.h>
struct SnapshotWait {
    enum Result { WAIT, FRESH, EXPIRED };
    uint32_t after=0, deadline=0;
    void start(uint32_t now) { after=now; deadline=now+20000; }
    Result result(uint32_t now, uint32_t imageAt) const {
        if (imageAt && int32_t(imageAt-after)>=0) return FRESH;
        return int32_t(now-deadline)>=0 ? EXPIRED : WAIT;
    }
};
