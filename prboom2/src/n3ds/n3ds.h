//
// [3DS] What is the 3DS's own: startup, the log on the SD card.
//

#ifndef __N3DS__
#define __N3DS__

#ifdef __3DS__

// Before anything else in main: the New 3DS's full speed, the data folder
// (N3DS_DATA_DIR) as the current directory, the log.
void N3DS_Init(void);

#endif

#endif
