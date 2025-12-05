#ifndef _METAFILE_H_
#define _METAFILE_H_

#include "types.h"

void metafile_setup(void);
void metafile_cleanup(void);

void metafile_change(char *name);
void metafile_delete(char *name);

metafile_t *metafile_lookup(const char *name);
void metafile_release(metafile_t *mf);

#endif
