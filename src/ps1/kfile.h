/* Build/Duke file API (kopen4load & friends) implemented over DUKE.DAT on CD. */
#pragma once

long kopen4load(const char *filename, char searchfirst);
long kread(long handle, void *buffer, long leng);
long klseek(long handle, long offset, long whence);
long kfilelength(long handle);
void kclose(long handle);
void kcloseall(void);
long initgroupfile(const char *filename);
void uninitgroupfile(void);
