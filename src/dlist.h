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

#ifdef __cplusplus
};
#endif
