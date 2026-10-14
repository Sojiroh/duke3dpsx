/* Save game serialisation to the memory card (see savegame.c for the file format). */
#pragma once

#define SG_SLOTS 10

typedef struct
{
    char name[20];               /* player entered name (ud.savegame[spot]) */
    unsigned char vol, lvl, skill, nump;
} sg_info;

void sg_readnames(void);                        /* fill ud.savegame[] from the card ("EMPTY" for free slots) */
int  sg_peek(int spot, sg_info *info);          /* header only: 0 ok, -1 absent/invalid */
int  sg_write(int spot);                        /* 0 ok, -1 error (sg_error says why) */
int  sg_read(int spot);                         /* 0 ok, 1 bad version/script, 2 wrong player count, -1 card error, SG_DESTROYED */
#define SG_DESTROYED (-2)                       /* decode failed after the live game state was overwritten: the caller must restart the level */
extern const char *sg_error;                    /* short message for the last failure */
extern const char *sg_warning;                  /* non-empty after a successful but degraded save (shown instead of "GAME SAVED") */
const char *sg_card_message(void);              /* NULL, or why the card is unusable (no card / not formatted / error): last known state */

#ifdef PS1_SAVETEST
void sg_savetest_tick(void);                    /* called every frame from getpackets() */
#endif
