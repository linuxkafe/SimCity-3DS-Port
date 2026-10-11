/* Host harness: runs the real 3dsdb server until killed. */
#include "fake3ds.h"
#include <stdio.h>
#include <unistd.h>
void dbgk_init(void);
void dbgk_poll(void);
int main(void) {
    dbgk_init();
    for (;;) dbgk_poll();     /* run until the test kills us */
    return 0;
}
