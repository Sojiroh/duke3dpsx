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

/* PS1: there is no setup file (DUKE3D.CFG) and no writable storage.  CONFIG_ReadSetup() installs fixed
   defaults; the jmact script/key-binding parser is not needed because the pad mapping is built into
   ps1_control.c. */

//
// Sound variables
//
int32 FXDevice;
int32 MusicDevice;
int32 FXVolume;
int32 MusicVolume;
int32 SoundToggle;
int32 MusicToggle;
int32 VoiceToggle;
int32 AmbienceToggle;
fx_blaster_config BlasterConfig;
int32 NumVoices;
int32 NumChannels;
int32 NumBits;
int32 MixRate;
int32 MidiPort;
int32 ReverseStereo;

int32 ControllerType;
int32 MouseAiming;

//
// Screen variables
//

int32 ScreenMode;
int32 ScreenWidth;
int32 ScreenHeight;

void CONFIG_GetSetupFilename( void )
   {
   }

/*
===================
=
= CONFIG_SetDefaults
=
===================
*/

void CONFIG_SetDefaults( void )
   {
   SoundToggle = 1;
   MusicToggle = 1;
   VoiceToggle = 1;
   AmbienceToggle = 1;
   FXVolume = 220;
   MusicVolume = 200;
   ReverseStereo = 0;
   ps[0].aim_mode = 1;
   ud.screen_size = 8;      /* 8 = 3D view with the status bar (the original default) */
   ud.screen_tilting = 1;
   ud.shadows = 1;
   ud.detail = 1;
   ud.lockout = 0;
   ud.pwlockout[0] = '\0';
   ud.crosshair = 1;
   ud.m_marker = 1;
   ud.m_ffire = 1;
   ud.brightness = 0;
   ud.auto_run = 1;
   ud.mouseflip = 0;
   ud.fta_on = 1;
   ud.executions = 1;
}

#include "savegame.h"

void readsavenames(void)
{
    /* PS1: slot names come from the memory card (empty slots have an empty name) */
    sg_readnames();
}

/*
===================
=
= CONFIG_ReadSetup
=
===================
*/

extern long myaimmode;

void CONFIG_ReadSetup( void )
{
   int32 dummy;

   CONFIG_SetDefaults();

   memset(&ud.ridecule[0][0],0,sizeof(ud.ridecule));
   strcpy(myname,"DUKE");
   boardfilename[0] = 0;
   ud.rtsname[0] = 0;

   ScreenMode = 1;
   ScreenWidth = 320;
   ScreenHeight = 240;

   FXDevice = 0;
   MusicDevice = 0;
   NumVoices = 14; /* FX_MAXVOICES */
   NumChannels = 2;
   NumBits = 16;
   MixRate = 22050;
   MidiPort = 0;
   ControllerType = controltype_keyboardandgamepad;
   MouseAiming = 0;
   myaimmode = 1; /* analog look with the right stick is always on */

   ud.wchoice[0][0] = 3;
   ud.wchoice[0][1] = 4;
   ud.wchoice[0][2] = 5;
   ud.wchoice[0][3] = 7;
   ud.wchoice[0][4] = 8;
   ud.wchoice[0][5] = 6;
   ud.wchoice[0][6] = 0;
   ud.wchoice[0][7] = 2;
   ud.wchoice[0][8] = 9;
   ud.wchoice[0][9] = 1;

   CONTROL_ClearAssignments();
}

void CONFIG_WriteSetup( void )
{
   /* PS1: nothing is written */
}
