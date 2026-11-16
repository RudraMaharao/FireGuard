#include "snapshot_wait.h"
#include <assert.h>
#include <iostream>
int main(){
 SnapshotWait w;w.start(1000);
 assert(w.result(1100,999)==SnapshotWait::WAIT);
 assert(w.result(1100,1000)==SnapshotWait::FRESH);
 assert(w.result(20999,0)==SnapshotWait::WAIT);
 assert(w.result(21000,0)==SnapshotWait::EXPIRED);
 w.start(UINT32_MAX-9);
 assert(w.result(10,5)==SnapshotWait::FRESH);
 assert(w.result(19989,UINT32_MAX-20)==SnapshotWait::WAIT);
 assert(w.result(19990,0)==SnapshotWait::EXPIRED);
 std::cout<<"Fresh-image delivery rejects stale cache, bounds camera wait and handles timer wrap\n";
}
