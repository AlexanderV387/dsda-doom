//
// [3DS] What is the 3DS's own (see n3ds.h).
//

#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include <3ds.h>

#include "n3ds/n3ds.h"

// libctru's main thread stack is 32 KiB by default: too small for Doom.
u32 __stacksize__ = 1024 * 1024;

void N3DS_Init(void)
{
  // 804 MHz and the extra cache on the New 3DS (nothing on the Old 3DS).
  osSetSpeedupEnable(true);

  mkdir("sdmc:/3ds", 0777);
  mkdir(N3DS_DATA_DIR, 0777);
  chdir(N3DS_DATA_DIR);

  // Everything the game prints goes to a file next to it: the 3DS has no
  // terminal. Line buffered, so a crash keeps what came before it.
  if (freopen(N3DS_DATA_DIR "/dsda-doom-log.txt", "w", stdout))
  {
    setvbuf(stdout, NULL, _IOLBF, 0);
    dup2(fileno(stdout), fileno(stderr));
  }
}
