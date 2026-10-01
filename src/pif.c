// PIF joybus commands: controller status and buttons, Controller Pak
// read/write backed by a 32KB image file per game, cartridge EEPROM
// (channel 4) backed by a 2KB .eep file.

#include "ultra.h"
#include <stdio.h>
#include <direct.h>

#define MEMPAK_SIZE 0x8000
#define EEPROM_SIZE 0x800  // file is always 16Kbit, as Mupen64Plus .eep

static byte mempak[MEMPAK_SIZE];
static char mempakfile[MAXFILE+48];
static byte eeprom[EEPROM_SIZE];
static char eepromfile[MAXFILE+48];

/****************************************************************************
** PIF RAM access (64 bytes in RPIF[0x1f0..0x1ff], big-endian words)
*/

static byte pif_get(int i) { dword w=RPIF[0x1f0+(i>>2)]; return((byte)(w>>(24-8*(i&3)))); }
static void pif_set(int i,byte b)
{
    dword *w=&RPIF[0x1f0+(i>>2)];
    int    s=24-8*(i&3);
    *w=(*w&~(0xffu<<s))|((dword)b<<s);
}

/****************************************************************************
** Controller Pak image
*/

// CRC the pak returns for a 32-byte block (polynomial 0x85)
static byte mempak_crc(const byte *data)
{
    byte crc=0;
    int  i,mask;
    for(i=0;i<=32;i++)
    {
        for(mask=0x80;mask;mask>>=1)
        {
            byte tap=(crc&0x80)?0x85:0;
            crc<<=1;
            if(i<32 && (data[i]&mask)) crc|=1;
            crc^=tap;
        }
    }
    return(crc);
}

// a freshly formatted pak, as libultra's osPfsInit expects to find it:
// ID block (and its three backups), empty inode table (and backup) and
// an empty note table
static void mempak_format(void)
{
    static const byte id[28]={
        0xff,0xff,0xff,0xff, 0x05,0x1a,0x5f,0x13,    // repaired, random
        0,0,0,0,0,0,0,0,                             // serial
        0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,     // serial
        0x00,0x01, 0x01, 0x00 };                     // device id, banks, version
    static const int idpos[4]={0x20,0x60,0x80,0xc0}; // main block and backups
    dword sum=0;
    int   i,j;

    memset(mempak,0,sizeof(mempak));
    for(i=0;i<32;i++) mempak[i]=(byte)i;             // label area
    mempak[0]=0x81;
    for(i=0;i<28;i+=2) sum+=(id[i]<<8)|id[i+1];      // __osIdCheckSum
    sum&=0xffff;
    for(i=0;i<4;i++)
    {
        byte *b=mempak+idpos[i];
        memcpy(b,id,28);
        b[28]=(byte)(sum>>8);          b[29]=(byte)sum;
        b[30]=(byte)((0xfff2-sum)>>8); b[31]=(byte)(0xfff2-sum);
    }
    for(i=1;i<=2;i++)                                // inode table and backup
    {
        byte *b=mempak+i*0x100;
        for(j=5;j<128;j++) { b[2*j]=0x00; b[2*j+1]=0x03; } // PFS_EMPTY_PAGE
        b[1]=0x71;                                   // byte sum of entries 5..127
    }
}

static void mempak_save(void)
{
    FILE *f=fopen(mempakfile,"wb");
    if(!f) { warning("pif: can't write %s",mempakfile); return; }
    fwrite(mempak,1,sizeof(mempak),f);
    fclose(f);
}

void pif_savename(char *dst,const char *ext)
{
    char title[33];
    int  i;

    // file name from the rom title, trailing spaces and bad characters removed
    memcpy(title,cart.title,32);
    title[32]=0;
    for(i=31;i>=0 && (title[i]==' ' || !title[i]);i--) title[i]=0;
    for(i=0;title[i];i++) if(strchr("\\/:*?\"<>|",title[i]) || title[i]<32) title[i]='_';
    if(!title[0]) strcpy(title,"unknown");
    if(*init.savepath) sprintf(dst,"%s%s.%s",init.savepath,title,ext);
    else
    {
        // default: a saves folder next to the exe, made when missing
        char dir[MAXFILE+8];
        sprintf(dir,"%ssaves",init.rootpath);
        _mkdir(dir);
        sprintf(dst,"%s\\%s.%s",dir,title,ext);
    }
}

static void eeprom_save(void)
{
    FILE *f=fopen(eepromfile,"wb");
    if(!f) { warning("pif: can't write %s",eepromfile); return; }
    fwrite(eeprom,1,sizeof(eeprom),f);
    fclose(f);
}

void pif_reset(void)
{
    FILE *f;

    // cartridge EEPROM (Mupen64Plus eeprom.c): erased is all 0xff, the
    // file is created on the first write
    pif_savename(eepromfile,"eep");
    memset(eeprom,0xff,sizeof(eeprom));
    f=fopen(eepromfile,"rb");
    if(f)
    {
        fread(eeprom,1,sizeof(eeprom),f);
        fclose(f);
    }

    pif_savename(mempakfile,"mpk");

    f=fopen(mempakfile,"rb");
    if(f && fread(mempak,1,sizeof(mempak),f)==sizeof(mempak))
    {
        fclose(f);
        return;
    }
    if(f) fclose(f);
    mempak_format(); // created on the first write
}

/****************************************************************************
** joybus commands
*/

void pif_process(int padport)
{
    int ch=0,p=0;
    while(p<63 && ch<5)
    {
        int t=pif_get(p);
        if(t==0xfe) break;             // end of commands
        if(t==0xff || t==0xfd) { p++; continue; } // padding
        if(t==0x00) { ch++; p++; continue; }       // skip channel
        {
            int tx=t&0x3f,rx=pif_get(p+1)&0x3f;
            int cmd=pif_get(p+2);
            int r=p+2+tx;              // reply bytes
            if(r+rx>63) break;
            if(ch==padport && (cmd==0x00 || cmd==0xff) && rx>=3)
            { // controller status: standard pad, an accessory inserted or not
                pif_set(r,0x05); pif_set(r+1,0x00); pif_set(r+2,cart.pak==PAK_NONE?0x02:0x01);
            }
            else if(ch==padport && (cmd==0x02 || cmd==0x03) && cart.pak==PAK_NONE)
            { // no accessory: nothing answers the pak read/write
                pif_set(p+1,(byte)(pif_get(p+1)|0x80));
            }
            else if(ch==padport && cmd==0x02 && tx>=3 && rx>=33 && cart.pak==PAK_RUMBLE)
            { // Rumble Pak read (Mupen64Plus rumblepak.c): its ID area at
              // 0x8000 reads 0x80, what libultra's osMotorInit looks for;
              // a Controller Pak echoes the 0xFE it writes first, and rumble
              // only games then say that pak isn't for them (Chameleon Twist)
                int  addr=((pif_get(p+3)<<8)|pif_get(p+4))&0xffe0;
                byte data[32];
                int  i;
                memset(data,(addr>=0x8000 && addr<0x9000)?0x80:0x00,32);
                for(i=0;i<32;i++) pif_set(r+i,data[i]);
                pif_set(r+32,mempak_crc(data));
            }
            else if(ch==padport && cmd==0x03 && tx>=35 && rx>=1 && cart.pak==PAK_RUMBLE)
            { // Rumble Pak write: the motor at 0xC000 (no host rumble)
                int  addr=((pif_get(p+3)<<8)|pif_get(p+4))&0xffe0;
                byte data[32];
                int  i;
                static int seen;
                for(i=0;i<32;i++) data[i]=pif_get(p+5+i);
                if(addr==0xc000 && data[31] && !seen) { seen=1; print("note: Rumble Pak motor on\n"); }
                pif_set(r,mempak_crc(data));
            }
            else if(ch==padport && cmd==0x01 && rx>=4)
            { // read buttons and stick
                dword d=pad_getdata(0);
                pif_set(r,(byte)(d>>24)); pif_set(r+1,(byte)(d>>16));
                pif_set(r+2,(byte)(d>>8)); pif_set(r+3,(byte)d);
            }
            else if(ch==padport && cmd==0x02 && tx>=3 && rx>=33)
            { // Controller Pak read: 32 bytes and their CRC
                int  addr=((pif_get(p+3)<<8)|pif_get(p+4))&0xffe0;
                byte data[32];
                int  i;
                if(addr<MEMPAK_SIZE) memcpy(data,mempak+addr,32);
                else memset(data,0,32); // accessory id area: not a Rumble Pak
                for(i=0;i<32;i++) pif_set(r+i,data[i]);
                pif_set(r+32,mempak_crc(data));
            }
            else if(ch==padport && cmd==0x03 && tx>=35 && rx>=1)
            { // Controller Pak write: 32 bytes, reply is their CRC
                int  addr=((pif_get(p+3)<<8)|pif_get(p+4))&0xffe0;
                byte data[32];
                int  i;
                for(i=0;i<32;i++) data[i]=pif_get(p+5+i);
                if(addr<MEMPAK_SIZE && memcmp(mempak+addr,data,32))
                {
                    memcpy(mempak+addr,data,32);
                    mempak_save();
                }
                pif_set(r,mempak_crc(data));
            }
            else if(ch==4 && (cmd==0x00 || cmd==0xff) && rx>=3)
            { // cartridge EEPROM status: type 0x8000 (4K) or 0xC000 (16K)
                static int seen;
                if(!seen) { seen=1; print("note: first EEPROM access (%iKbit)\n",cart.eeprom16k?16:4); }
                pif_set(r,0x00); pif_set(r+1,cart.eeprom16k?0xc0:0x80); pif_set(r+2,0x00);
            }
            else if(ch==4 && cmd==0x04 && tx>=2 && rx>=8)
            { // EEPROM read: one 8-byte block
                int addr=pif_get(p+3)*8,i;
                int size=cart.eeprom16k?EEPROM_SIZE:0x200;
                for(i=0;i<8;i++) pif_set(r+i,addr<size?eeprom[addr+i]:0);
            }
            else if(ch==4 && cmd==0x05 && tx>=10 && rx>=1)
            { // EEPROM write: one 8-byte block, reply is the status (0)
                int addr=pif_get(p+3)*8,i,changed=0;
                int size=cart.eeprom16k?EEPROM_SIZE:0x200;
                if(addr<size)
                {
                    for(i=0;i<8;i++)
                    {
                        byte b=pif_get(p+4+i);
                        if(eeprom[addr+i]!=b) { eeprom[addr+i]=b; changed=1; }
                    }
                    if(changed) eeprom_save();
                }
                pif_set(r,0x00);
            }
            else
            { // no device / unsupported: set the error bit in the rx byte
                pif_set(p+1,(byte)(pif_get(p+1)|0x80));
                if(ch==padport || ch==4)
                {
                    static int seen[256];
                    if(!seen[cmd]) { seen[cmd]=1; print("pif: command %02X on channel %i not supported\n",cmd,ch); }
                }
            }
            p+=2+tx+rx;
            ch++;
        }
    }
}

/****************************************************************************
** control byte (PIF RAM 0x3F) and the CIC-NUS-6105 challenge, as in
** Mupen64Plus process_pif_ram and n64_cic_nus_6105 (X-Scale's algorithm)
*/

static int challenged; // last write was a challenge: no joybus channels

static void cic_6105(const char *chl,char *rsp,int len)
{
    static const char lut0[16]={4,7,10,7,14,5,14,1,12,15,8,15,6,3,6,9};
    static const char lut1[16]={4,1,10,7,14,5,14,1,12,9,8,5,6,3,12,9};
    const char *lut=lut0;
    char key=0xB;
    int  i,sgn,mag,mod;
    for(i=0;i<len;i++)
    {
        rsp[i]=(key+5*chl[i])&0xF;
        key=lut[(int)rsp[i]];
        sgn=(rsp[i]>>3)&1;
        mag=((sgn==1)?~rsp[i]:rsp[i])&7;
        mod=(mag%3==1)?sgn:1-sgn;
        if(lut==lut1 && (rsp[i]==0x1 || rsp[i]==0x9)) mod=1;
        if(lut==lut1 && (rsp[i]==0xB || rsp[i]==0xE)) mod=0;
        lut=(mod==1)?lut1:lut0;
    }
}

static void pif_challenge(void)
{
    char chl[30],rsp[30];
    int  i;
    for(i=0;i<15;i++)
    {
        chl[i*2]  =(pif_get(0x30+i)>>4)&15;
        chl[i*2+1]= pif_get(0x30+i)    &15;
    }
    // all 30 nibbles (Mupen64Plus CHL_LEN-2): with 28 the answer's last
    // byte was whatever the stack held, and Banjo-Tooie's check failed
    cic_6105(chl,rsp,30);
    pif_set(0x2e,0);
    pif_set(0x2f,0);
    for(i=0;i<15;i++) pif_set(0x30+i,(byte)((rsp[i*2]<<4)+rsp[i*2+1]));
}

void pif_write(int padport)
{
    int flags=pif_get(63),clr=0;
    challenged=0;
    if(flags&0x02)
    { // challenge instead of joybus commands
        static int seen;
        if(!seen) { seen=1; print("pif: CIC-6105 challenge\n"); }
        pif_challenge();
        challenged=1;
        clr|=0x02;
    }
    else pif_process(padport);
    clr|=flags&0x09; // channel format and 0x08 acknowledged
    if(flags&0x30) flags=0x80;
    pif_set(63,(byte)(flags&~clr));
}

void pif_read(int padport)
{
    if(!challenged) pif_process(padport);
}
