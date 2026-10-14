/**********************************************************************
   module: FX_MAN.H  (PS1 port: audiolib API subset implemented over the SPU in src/ps1/spu_fx.c)

   Original: James R. Dose, 1994.  The signatures are identical to audiolib's so that the
   game code compiles unchanged.  Differences:
     - the `char *ptr` argument of FX_Play* is NOT VOC/WAV data but a pointer to an fx_sample
       descriptor (see below), created by loadsound()/getsound() in sounds.c;
     - voices live in SPU RAM, there is no mixing buffer;
     - FX_StopSound / FX_StopAllSounds / voice stealing invoke the registered callback
       synchronously (like audiolib's MV_Kill); voices that finish by themselves are
       reported from fx_update() (called by audio_update() once per frame), never from an IRQ.
**********************************************************************/

#ifndef __FX_MAN_H
#define __FX_MAN_H

#include "sndcards.h"

typedef struct
   {
   int MaxVoices;
   int MaxSampleBits;
   int MaxChannels;
   } fx_device;

#define MonoFx   1
#define StereoFx 2

typedef struct
   {
   unsigned long Address;
   unsigned long Type;
   unsigned long Interrupt;
   unsigned long Dma8;
   unsigned long Dma16;
   unsigned long Midi;
   unsigned long Emu;
   } fx_blaster_config;

enum FX_ERRORS
   {
   FX_Warning = -2,
   FX_Error = -1,
   FX_Ok = 0,
   FX_ASSVersion,
   FX_BlasterError,
   FX_SoundCardError,
   FX_InvalidCard,
   FX_MultiVocError,
   FX_DPMI_Error
   };

enum fx_BLASTER_Types
   {
   fx_SB     = 1,
   fx_SBPro  = 2,
   fx_SB20   = 3,
   fx_SBPro2 = 4,
   fx_SB16   = 6
   };

/* Number of simultaneous sound effect voices (SPU voices 10..23). */
#define FX_MAXVOICES 14

/* Sample descriptor passed (as `char *ptr`) to the FX_Play* functions. */
#define FX_SAMPLE_MAGIC 0x44535846u  /* "FXSD" */
typedef struct fx_sample
   {
   unsigned int   magic;     /* FX_SAMPLE_MAGIC */
   unsigned int   sector;    /* first sector of the SPU-ADPCM data inside DUKE.DAT */
   unsigned int   size;      /* bytes of ADPCM data (16 per block, includes the terminator block) */
   unsigned short rate;      /* native sample rate in Hz */
   unsigned char  loop;      /* 1: loop the whole sample (soundm & 1) */
   unsigned char  slot;      /* SPU cache slot + 1 while resident, 0 otherwise (internal) */
   } fx_sample;

/* Make `s` resident in SPU RAM (loads from CD if needed).  Returns 1 on success. */
int  FX_LoadSample( fx_sample *s );
/* Bytes of SPU RAM currently used by the sample cache. */
int  FX_CacheUsed( void );
/* Service finished voices (invokes the callback); called from audio_update(). */
void fx_update( void );

char *FX_ErrorString( int ErrorNumber );
int   FX_SetupCard( int SoundCard, fx_device *device );
int   FX_GetBlasterSettings( fx_blaster_config *blaster );
int   FX_SetupSoundBlaster( fx_blaster_config blaster, int *MaxVoices, int *MaxSampleBits, int *MaxChannels );
int   FX_Init( int SoundCard, int numvoices, int numchannels, int samplebits, unsigned mixrate );
int   FX_Shutdown( void );
int   FX_SetCallBack( void ( *function )( unsigned long ) );
void  FX_SetVolume( int volume );
int   FX_GetVolume( void );

void  FX_SetReverseStereo( int setting );
int   FX_GetReverseStereo( void );
void  FX_SetReverb( int reverb );
void  FX_SetFastReverb( int reverb );
int   FX_GetMaxReverbDelay( void );
int   FX_GetReverbDelay( void );
void  FX_SetReverbDelay( int delay );

int FX_VoiceAvailable( int priority );
int FX_EndLooping( int handle );
int FX_SetPan( int handle, int vol, int left, int right );
int FX_SetPitch( int handle, int pitchoffset );
int FX_SetFrequency( int handle, int frequency );

int FX_PlayVOC( char *ptr, int pitchoffset, int vol, int left, int right,
       int priority, unsigned long callbackval );
int FX_PlayLoopedVOC( char *ptr, long loopstart, long loopend,
       int pitchoffset, int vol, int left, int right, int priority,
       unsigned long callbackval );
int FX_PlayWAV( char *ptr, int pitchoffset, int vol, int left, int right,
       int priority, unsigned long callbackval );
int FX_PlayLoopedWAV( char *ptr, long loopstart, long loopend,
       int pitchoffset, int vol, int left, int right, int priority,
       unsigned long callbackval );
int FX_PlayVOC3D( char *ptr, int pitchoffset, int angle, int distance,
       int priority, unsigned long callbackval );
int FX_PlayWAV3D( char *ptr, int pitchoffset, int angle, int distance,
       int priority, unsigned long callbackval );
int FX_Pan3D( int handle, int angle, int distance );
int FX_SoundActive( int handle );
int FX_SoundsPlaying( void );
int FX_StopSound( int handle );
int FX_StopAllSounds( void );

#endif
