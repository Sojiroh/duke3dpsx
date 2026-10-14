/* PS1: loading screen with a progress bar (loadscreen.c) */
#pragma once

/* Draw the screen at 0%: black (showpic = 0) or the LOADSCREEN art.  line1 / line2: menutext lines centred
   at y = 90 / 114 (NULL = none).  Stays active until loadscreen_end(). */
void loadscreen_begin(const char *line1, const char *line2, int showpic);
/* Replace the text (e.g. once the fonts are resident); redraws at the current percentage. */
void loadscreen_text(const char *line1, const char *line2);
/* Bar to pct (0..100; never backwards).  Redraws and presents only when the percentage changed. */
void loadscreen_set(int pct);
/* Start the next stage, which ends at end_pct: the previous stage is completed first.  While it runs the
   bar moves from there to end_pct as CD bytes are read (expected_bytes; 0 = not tracked by CD bytes: use
   loadscreen_progress() or let the next stage / loadscreen_set() complete it). */
void loadscreen_stage(int end_pct, unsigned long expected_bytes);
/* Progress of the current stage not driven by CD bytes: done out of total units. */
void loadscreen_progress(unsigned long done, unsigned long total);
/* Force 100% and leave the last frame displayed. */
void loadscreen_end(void);
