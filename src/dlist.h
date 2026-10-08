#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// GRAPHICS - display lists
int  dlist_execute(OSTask_t* task); // execute a display list; 1 = paused (a branch to itself)
int  dlist_resume(void);            // continue a paused list: 1 = still waiting
void dlist_addtestdot(int y); // add a dot to the framerate graph in debug mode
void dlist_cammove(float x, float y, float z);
void dlist_ignoregraphics(int ignore);
void dlist_newgame(void);           // microcodes are named in the log once per game again

#ifdef __cplusplus
};
#endif
