//-------------------------------------------------------------------------
/*
Copyright (C) 1996, 2003 - 3D Realms Entertainment

This file is part of Duke Nukem 3D version 1.5 - Atomic Edition

Duke Nukem 3D is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

Original Source: 1996 - Todd Replogle
Prepared for public release: 03/21/2003 - Charlie Wiederhold, 3D Realms
*/
//-------------------------------------------------------------------------

#include "duke3d.h"

/* PS1: the RTS (remote ridicule / sound lump) manager is disabled: there is no DUKE.RTS
   in the CD pack and no writable filesystem.  The API is kept so sounds.c links. */

int32 numlumps;
char lumplockbyte[11];

void RTS_Init (char *filename)
   {
   (void)filename;
   numlumps = 0;
   }

int32 RTS_NumSounds (void)
   {
   return 0;
   }

int32 RTS_SoundLength (int32 lump)
   {
   (void)lump;
   return 0;
   }

char * RTS_GetSoundName (int32 i)
   {
   (void)i;
   return "";
   }

void *RTS_GetSound (int32 lump)
   {
   (void)lump;
   return NULL;
   }
