// console.h on Linux: no text console window. The debugger console is
// Windows only (-c); here the core always runs with the dummy console, as
// the Windows Release build does. Keys reach the program through the
// window (ui_linux.c), not through con_readkey.

#include "../ultra.h"
#include "../console.h"

void con_init(void)      {}
void con_initdummy(void) {}
void con_deinit(void)    {}
void con_title(const char *title) { (void)title; }

int  con_resized(void)   { return(0); }
int  con_rows(void)      { return(50); }
int  con_cols(void)      { return(80); }

void con_clear(void)                     {}
void con_gotoxy(int x,int y)             { (void)x; (void)y; }
void con_cursorxy(int x,int y,int size)  { (void)x; (void)y; (void)size; }
void con_attr(int fg)                    { (void)fg; }
void con_attr2(int fg,int bg)            { (void)fg; (void)bg; }
void con_printchar(int ch)               { (void)ch; }
void con_print(char *text)               { (void)text; }
void con_tabto(int ch,int x)             { (void)ch; (void)x; }

void con_printf(char *text,...)
{
    (void)text;
}

void con_readmouserelative(int *xp,int *yp,int *bp)
{
    *xp=*yp=*bp=0;
}

int con_readkey(void)         { return(0); }
int con_readkey_noblock(void) { return(0); }
