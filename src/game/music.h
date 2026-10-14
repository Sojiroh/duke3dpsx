/**********************************************************************
   module: MUSIC.H  (PS1 port: MUSIC_* API subset implemented by the SPU sequencer in
                     src/ps1/spu_music.c)

   Original: James R. Dose, 1994.
   Differences: MUSIC_PlaySong() takes the NAME of a song stored in DUKE.DAT (e.g. "STALKER.MID",
   passed as `unsigned char *`), not MIDI bytes.  Songs are streamed from the CD pack.
**********************************************************************/

#ifndef __MUSIC_H
#define __MUSIC_H

#include "sndcards.h"

extern int MUSIC_ErrorCode;

enum MUSIC_ERRORS
   {
   MUSIC_Warning = -2,
   MUSIC_Error   = -1,
   MUSIC_Ok      = 0,
   MUSIC_ASSVersion,
   MUSIC_SoundCardError,
   MUSIC_MPU401Error,
   MUSIC_InvalidCard,
   MUSIC_MidiError,
   MUSIC_TaskManError,
   MUSIC_FMNotDetected,
   MUSIC_DPMI_Error
   };

#define MUSIC_LoopSong ( 1 == 1 )
#define MUSIC_PlayOnce ( !MUSIC_LoopSong )

char *MUSIC_ErrorString( int ErrorNumber );
int   MUSIC_Init( int SoundCard, int Address );
int   MUSIC_Shutdown( void );
void  MUSIC_SetVolume( int volume );
int   MUSIC_GetVolume( void );
void  MUSIC_SetLoopFlag( int loopflag );
int   MUSIC_SongPlaying( void );
void  MUSIC_Continue( void );
void  MUSIC_Pause( void );
int   MUSIC_StopSong( void );
int   MUSIC_PlaySong( unsigned char *song, int loopflag );
void  MUSIC_RegisterTimbreBank( unsigned char *timbres );

/* Service the song stream (called from audio_update() once per frame). */
void  music_update( void );

/* The original menu code spells it Music_SetVolume (Watcom links case-insensitively). */
#define Music_SetVolume MUSIC_SetVolume

#endif
