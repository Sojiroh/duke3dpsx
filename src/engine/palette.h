/* Palette + CLUT management for the PS1 Build port. */
#pragma once
#include "psx.h"

void pal_init(void);
const unsigned char *pal_basepal(int idx);            /* 0 normal,1 water,2 slime,3 title,4 drealms,5 ending (6*768 bytes) */
void pal_set_brightness(int bright, const char *dapal); /* select palette contents + gamma; invalidates CLUTs */
void pal_set_remap(int palnum, const unsigned char *remap);
u16 clut_get(int pal, int opaque);                    /* CLUT id for the current palette (lazy, LRU) */
void clut_frame(void);
int pal_shade_bright(int shade);                      /* 0..128 brightness for a Build shade index */
extern unsigned char shadebright[32];
void pal_set_tint(int r, int g, int b, int intensity); /* full-screen overlay 0..63 intensity */
int pal_get_tint(int *r, int *g, int *b);
