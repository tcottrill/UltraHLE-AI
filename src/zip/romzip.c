// ROM images inside .zip archives. See romzip.h.

#include <string.h>
#include <stdlib.h>
#include "miniz.h"
#include "romzip.h"

static const char *romexts[]={".z64",".v64",".n64",".rom",".bin",".u64",NULL};

static int hasext(const char *name,const char *ext)
{
    size_t n=strlen(name),e=strlen(ext);
    return(n>=e && !_stricmp(name+n-e,ext));
}

int romzip_iszip(const char *fname)
{
    return(hasext(fname,".zip"));
}

// Opens the archive and finds the ROM entry. Returns the entry index, or -1
// (archive closed again) on failure.
static int openrom(mz_zip_archive *zip,const char *zipname,int *romsize)
{
    mz_zip_archive_file_stat stat;
    int i,e,n,found=-1,firstfile=-1;

    memset(zip,0,sizeof(*zip));
    if(!mz_zip_reader_init_file(zip,zipname,0)) return(-1);

    n=(int)mz_zip_reader_get_num_files(zip);
    for(i=0;i<n && found<0;i++)
    {
        if(mz_zip_reader_is_file_a_directory(zip,i)) continue;
        if(!mz_zip_reader_file_stat(zip,i,&stat)) continue;
        if(firstfile<0) firstfile=i;
        for(e=0;romexts[e];e++)
        {
            if(hasext(stat.m_filename,romexts[e])) { found=i; break; }
        }
    }
    if(found<0) found=firstfile;
    if(found<0 || !mz_zip_reader_file_stat(zip,found,&stat) ||
       stat.m_uncomp_size==0 || stat.m_uncomp_size>0x7fffffff)
    {
        mz_zip_reader_end(zip);
        return(-1);
    }
    *romsize=(int)stat.m_uncomp_size;
    return(found);
}

int romzip_peek(const char *zipname,void *buf,int bytes,int *romsize)
{
    mz_zip_archive zip;
    mz_zip_reader_extract_iter_state *it;
    int    index,size;
    size_t got=0;

    index=openrom(&zip,zipname,&size);
    if(index<0) return(1);

    it=mz_zip_reader_extract_iter_new(&zip,index,0);
    if(it)
    {
        got=mz_zip_reader_extract_iter_read(it,buf,bytes);
        mz_zip_reader_extract_iter_free(it);
    }
    mz_zip_reader_end(&zip);
    if(got!=(size_t)bytes) return(1);
    if(romsize) *romsize=size;
    return(0);
}

void *romzip_load(const char *zipname,int *romsize)
{
    mz_zip_archive zip;
    int   index,size;
    void *data;

    index=openrom(&zip,zipname,&size);
    if(index<0) return(NULL);

    data=malloc(size);
    if(data && !mz_zip_reader_extract_to_mem(&zip,index,data,size,0))
    {
        free(data);
        data=NULL;
    }
    mz_zip_reader_end(&zip);
    if(data && romsize) *romsize=size;
    return(data);
}
