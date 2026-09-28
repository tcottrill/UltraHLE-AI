#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// SOUND - sound lists
void slist_execute(OSTask_t* task); // execute a sound list
int  slist_hle(OSTask_t* task);   // 0: audio microcode without HLE (MusyX), run it on the RSP
int  slist_nextbuffer(dword m_addr, int bytes); // osAiSetNextBuffer: AI FIFO + playback
int  slist_getlength(void); // osAiGetLength: AI_LEN
void slist_aiupdate(void);  // drain the AI FIFO up to now
void slist_aiplay(dword m_addr,int len); // the AI starts playing this buffer

#ifdef __cplusplus
};
#endif
