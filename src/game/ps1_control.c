/*
 * PS1 replacement for the jmact KB_*, CONTROL_* and MOUSE_* input layers.
 * Everything is fed from the PlayStation pad (pad / pad_poll() in psx.h).  There is no keyboard and
 * no mouse, the original key bindings (DUKE3D.CFG) are replaced by the fixed mapping below.
 *
 * ---- Default pad mapping --------------------------------------------------------------------
 *  In game (CONTROL_GetInput -> gamefunc_* buttons, same ControlInfo units as the mouse/joystick):
 *    D-pad UP / DOWN            Move_Forward / Move_Backward
 *    D-pad LEFT / RIGHT         Turn_Left / Turn_Right (strafe left/right while SQUARE is held)
 *    Left stick                 Y = forward/back, X = strafe (analog)
 *    Right stick                X = turn, Y = look up/down (analog, "mouse aiming" is always on)
 *    R1 Fire        L1 Jump        R2 Run (auto-run is on by default)      L2 Crouch
 *    CROSS Open/Use   CIRCLE Jump (alt)   TRIANGLE Next_Weapon   SQUARE Strafe modifier
 *    R3 Center_View   L3 Quick_Kick
 *    SELECT (modifier; a tap with nothing else pressed toggles the Map):
 *      SELECT+TRIANGLE Previous_Weapon   SELECT+LEFT/RIGHT Inventory_Left/Right
 *      SELECT+CROSS Inventory (use item)  SELECT+UP Jetpack   SELECT+DOWN MedKit
 *      SELECT+L1 Shrink_Screen   SELECT+R1 Enlarge_Screen   SELECT+START Pause
 *    START                      Escape (opens/closes the menu)
 *
 *  Menus / screens (KB_* scancodes, KB_KeyWaiting/KB_Getch):
 *    CROSS -> sc_Enter (13)   TRIANGLE, START -> sc_Escape (27)   SQUARE -> sc_Space (32)
 *    D-pad -> sc_Up/Down/Left/RightArrow (auto-repeat)   SELECT -> sc_Tab (9)
 *    L1 / R1 -> sc_PgUp / sc_PgDn (menu up / down)
 */

#include "duke3d.h"
#include <psxpad.h>

/* ------------------------------------------------------------------------------------------ */
/* Keyboard emulation                                                                         */
/* ------------------------------------------------------------------------------------------ */

volatile byte KB_KeyDown[MAXKEYBOARDSCAN];
volatile kb_scancode KB_LastScan;

#define KBQ_SIZE 32
static unsigned char kbq[KBQ_SIZE];
static int kbq_head, kbq_tail;

typedef struct
{
    u16 mask;          /* PAD_* bit */
    kb_scancode sc;
    unsigned char ascii;
    unsigned char repeat;
} KeyMap;

static const KeyMap keymap[] =
{
    { PAD_CROSS,    sc_Enter,      asc_Enter,  0 },
    { PAD_TRIANGLE, sc_Escape,     asc_Escape, 0 },
    { PAD_START,    sc_Escape,     asc_Escape, 0 },
    { PAD_SQUARE,   sc_Space,      asc_Space,  0 },
    { PAD_SELECT,   sc_Tab,        asc_Tab,    0 },
    { PAD_UP,       sc_UpArrow,    0,          1 },
    { PAD_DOWN,     sc_DownArrow,  0,          1 },
    { PAD_LEFT,     sc_LeftArrow,  0,          1 },
    { PAD_RIGHT,    sc_RightArrow, 0,          1 },
    { PAD_L1,       sc_PgUp,       0,          1 },
    { PAD_R1,       sc_PgDn,       0,          1 },
};
#define NUMKEYMAP ((int)(sizeof(keymap)/sizeof(keymap[0])))

#define REPEAT_DELAY (TICRATE*2/5)
#define REPEAT_RATE  (TICRATE/9)
static long repeat_next[NUMKEYMAP];

/* gamefunc bits (BUTTON() layout); ButtonState1 = functions 0..31, ButtonState2 = 32..63 */
boolean  CONTROL_RudderEnabled;
boolean  CONTROL_MousePresent;
boolean  CONTROL_JoysPresent[MaxJoys];
boolean  CONTROL_MouseEnabled;
boolean  CONTROL_JoystickEnabled;
byte     CONTROL_JoystickPort;
uint32   CONTROL_ButtonState1;
uint32   CONTROL_ButtonHeldState1;
uint32   CONTROL_ButtonState2;
uint32   CONTROL_ButtonHeldState2;

static uint32 ctl_suppress1, ctl_suppress2;  /* buttons cleared with CONTROL_ClearButton while still held */
static int select_used;                      /* SELECT was used as a modifier since it went down */
static int map_pulse;                        /* gamefunc_Map pending: stays set until the game calls CONTROL_ClearButton(Map) */
static int pause_pulse;

static void kbq_push(unsigned char c)
{
    int n = (kbq_head+1)%KBQ_SIZE;
    if(n == kbq_tail) return;
    kbq[kbq_head] = c;
    kbq_head = n;
}

static int gameplay_active(void)
{
    return (ps[myconnectindex].gm&MODE_GAME) && !(ps[myconnectindex].gm&(MODE_MENU|MODE_TYPE));
}

/* Polls the pad (cheap, may be called many times per frame) and updates KB_KeyDown[]. Edge events
   are accumulated, so polling several times between two reads never loses a button press. */
void ps1_poll_input(void)
{
    static u16 down_mask;      /* buttons reflected into KB_KeyDown (for release/repeat) */
    u16 pressed, released;
    int i;
    int game = gameplay_active();

    pad_poll();
    pressed = pad.pressed;
    released = pad.released;

    for(i=0;i<NUMKEYMAP;i++)
    {
        const KeyMap *k = &keymap[i];

        if(pressed&k->mask)
        {
            /* in game the buttons are read directly (CONTROL_GetInput); only START (menu) is a key,
               otherwise TRIANGLE (next weapon) would also open the menu through sc_Escape */
            if(!(k->mask == PAD_SELECT && game) && !(game && k->mask != PAD_START))
            {
                KB_KeyDown[k->sc] = 1;
                KB_LastScan = k->sc;
                if(!game)
                {
                    if(k->ascii) kbq_push(k->ascii);
                    else { kbq_push(0); kbq_push(k->sc); }
                }
            }
            repeat_next[i] = totalclock + REPEAT_DELAY;
        }
        else if( (pad.btn&k->mask) && k->repeat && !game && totalclock >= repeat_next[i] )
        {
            repeat_next[i] = totalclock + REPEAT_RATE;
            KB_KeyDown[k->sc] = 1;
            KB_LastScan = k->sc;
            kbq_push(0); kbq_push(k->sc);
        }
        if(released&k->mask)
        {
            /* START and TRIANGLE share sc_Escape: only release it when neither is down */
            if(!(k->sc == sc_Escape && (pad.btn&(PAD_START|PAD_TRIANGLE))))
                KB_KeyDown[k->sc] = 0;
        }
    }

    /* SELECT+START = pause */
    if( (pressed&PAD_START) && (pad.btn&PAD_SELECT) )
    {
        KB_KeyDown[sc_Escape] = 0;
        pause_pulse = 1;
        select_used = 1;
    }

    if(pressed&PAD_SELECT) select_used = 0;
    if( (released&PAD_SELECT) && !select_used && game ) map_pulse = 1;

    down_mask = pad.btn;
}

boolean KB_KeyWaiting( void )
{
    ps1_poll_input();
    return kbq_head != kbq_tail;
}

char KB_Getch( void )
{
    unsigned char c;

    ps1_poll_input();
    if(kbq_head == kbq_tail) return 0;
    c = kbq[kbq_tail];
    kbq_tail = (kbq_tail+1)%KBQ_SIZE;
    return c;
}

void KB_Addch( char ch )
{
    kbq_push(ch);
}

void KB_FlushKeyboardQueue( void )
{
    ps1_poll_input();
    kbq_head = kbq_tail = 0;
}

void KB_ClearKeysDown( void )
{
    memset((void *)KB_KeyDown,0,sizeof(KB_KeyDown));
}

void KB_KeyEvent( int scancode, boolean keypressed )
{
    if((unsigned)scancode < MAXKEYBOARDSCAN) KB_KeyDown[scancode] = keypressed;
}

char * KB_ScanCodeToString( kb_scancode scancode )
{
    return "";
}

kb_scancode KB_StringToScanCode( char * string )
{
    return 0;
}

void KB_TurnKeypadOn( void ) {}
void KB_TurnKeypadOff( void ) {}
boolean KB_KeypadActive( void ) { return false; }

void KB_Startup( void )
{
    KB_ClearKeysDown();
    kbq_head = kbq_tail = 0;
    KB_LastScan = 0;
}

void KB_Shutdown( void )
{
}

/* ------------------------------------------------------------------------------------------ */
/* CONTROL                                                                                    */
/* ------------------------------------------------------------------------------------------ */

static int32 mouse_sensitivity = DEFAULTMOUSESENSITIVITY;

/* soft response curve for the analog sticks: finer control around the centre, full range at the edge */
static int curve(int v)
{
    int a = v < 0 ? -v : v;
    int r = (a*(a+96))/224;
    return v < 0 ? -r : r;
}

void CONTROL_MapKey( int32 which, kb_scancode key1, kb_scancode key2 ) {}
void CONTROL_MapButton( int32 whichfunction, int32 whichbutton, boolean doubleclicked ) {}
void CONTROL_DefineFlag( int32 which, boolean toggle ) {}
boolean CONTROL_FlagActive( int32 which ) { return false; }
void CONTROL_ClearAssignments( void ) {}
void CONTROL_MapAnalogAxis( int32 whichaxis, int32 whichanalog ) {}
void CONTROL_MapDigitalAxis( int32 whichaxis, int32 whichfunction, int32 direction ) {}
void CONTROL_SetAnalogAxisScale( int32 whichaxis, int32 axisscale ) {}
void CONTROL_PrintAxes( void ) {}
void CONTROL_CenterJoystick( void ( *CenterCenter )( void ), void ( *UpperLeft )( void ), void ( *LowerRight )( void ),
                             void ( *CenterThrottle )( void ), void ( *CenterRudder )( void ) ) {}
void CONTROL_WaitRelease( void ) {}
void CONTROL_Ack( void ) {}

int32 CONTROL_GetMouseSensitivity( void ) { return mouse_sensitivity; }
void CONTROL_SetMouseSensitivity( int32 newsensitivity ) { mouse_sensitivity = newsensitivity; }

void CONTROL_Startup( controltype which, int32 ( *TimeFunction )( void ), int32 ticspersecond )
{
    CONTROL_MousePresent = false;
    CONTROL_MouseEnabled = false;
    CONTROL_JoystickEnabled = false;
    CONTROL_RudderEnabled = false;
    CONTROL_ButtonState1 = CONTROL_ButtonState2 = 0;
    CONTROL_ButtonHeldState1 = CONTROL_ButtonHeldState2 = 0;
    ctl_suppress1 = ctl_suppress2 = 0;
}

void CONTROL_Shutdown( void )
{
}

void CONTROL_ClearButton( int32 whichbutton )
{
    if(whichbutton == gamefunc_Map) map_pulse = 0;   /* consumed by the game */
    if(whichbutton > 31)
    {
        CONTROL_ButtonState2 &= ~(1u<<(whichbutton-32));
        ctl_suppress2 |= 1u<<(whichbutton-32);
    }
    else
    {
        CONTROL_ButtonState1 &= ~(1u<<whichbutton);
        ctl_suppress1 |= 1u<<whichbutton;
    }
}

void CONTROL_ClearUserInput( UserInput *info )
{
    info->button0 = info->button1 = false;
    info->dir = dir_None;
}

void CONTROL_GetUserInput( UserInput *info )
{
    ps1_poll_input();
    info->button0 = (pad.btn&PAD_CROSS) != 0;
    info->button1 = (pad.btn&PAD_TRIANGLE) != 0;
    if(pad.btn&PAD_UP) info->dir = dir_North;
    else if(pad.btn&PAD_DOWN) info->dir = dir_South;
    else if(pad.btn&PAD_LEFT) info->dir = dir_West;
    else if(pad.btn&PAD_RIGHT) info->dir = dir_East;
    else info->dir = dir_None;
}

static void setfn(uint32 *s1, uint32 *s2, int fn)
{
    if(fn > 31) *s2 |= 1u<<(fn-32);
    else *s1 |= 1u<<fn;
}

void CONTROL_GetInput( ControlInfo *info )
{
    uint32 raw1 = 0, raw2 = 0;
    u16 b;
    int sel, strafe;

    ps1_poll_input();
    b = pad.btn;
    sel = (b&PAD_SELECT) != 0;
    strafe = (b&PAD_SQUARE) != 0;

    CONTROL_ButtonHeldState1 = CONTROL_ButtonState1;
    CONTROL_ButtonHeldState2 = CONTROL_ButtonState2;

    if(sel)
    {
        /* SELECT modifier layer */
        if(b&PAD_TRIANGLE) { setfn(&raw1,&raw2,gamefunc_Previous_Weapon); select_used = 1; }
        if(b&PAD_LEFT)     { setfn(&raw1,&raw2,gamefunc_Inventory_Left); select_used = 1; }
        if(b&PAD_RIGHT)    { setfn(&raw1,&raw2,gamefunc_Inventory_Right); select_used = 1; }
        if(b&PAD_CROSS)    { setfn(&raw1,&raw2,gamefunc_Inventory); select_used = 1; }
        if(b&PAD_UP)       { setfn(&raw1,&raw2,gamefunc_Jetpack); select_used = 1; }
        if(b&PAD_DOWN)     { setfn(&raw1,&raw2,gamefunc_MedKit); select_used = 1; }
        if(b&PAD_L1)       { setfn(&raw1,&raw2,gamefunc_Shrink_Screen); select_used = 1; }
        if(b&PAD_R1)       { setfn(&raw1,&raw2,gamefunc_Enlarge_Screen); select_used = 1; }
    }
    else
    {
        if(b&PAD_UP)       setfn(&raw1,&raw2,gamefunc_Move_Forward);
        if(b&PAD_DOWN)     setfn(&raw1,&raw2,gamefunc_Move_Backward);
        if(b&PAD_LEFT)     setfn(&raw1,&raw2,gamefunc_Turn_Left);
        if(b&PAD_RIGHT)    setfn(&raw1,&raw2,gamefunc_Turn_Right);
        if(b&PAD_CROSS)    setfn(&raw1,&raw2,gamefunc_Open);
        if(b&PAD_TRIANGLE) setfn(&raw1,&raw2,gamefunc_Next_Weapon);
        if(b&PAD_L1)       setfn(&raw1,&raw2,gamefunc_Jump);
    }
    if(b&PAD_CIRCLE)   setfn(&raw1,&raw2,gamefunc_Jump);
    if(b&PAD_R1 && !sel) setfn(&raw1,&raw2,gamefunc_Fire);
    if(b&PAD_R2)       setfn(&raw1,&raw2,gamefunc_Run);
    if(b&PAD_L2)       setfn(&raw1,&raw2,gamefunc_Crouch);
    if(b&PAD_R3)       setfn(&raw1,&raw2,gamefunc_Center_View);
    if(b&PAD_L3)       setfn(&raw1,&raw2,gamefunc_Quick_Kick);
    if(strafe)         setfn(&raw1,&raw2,gamefunc_Strafe);

    if(map_pulse) setfn(&raw1,&raw2,gamefunc_Map);   /* held until the game consumes it (CONTROL_ClearButton) */
    if(pause_pulse)
    {
        pause_pulse = 0;
        KB_KeyDown[sc_Pause] = 1;
    }
    else if(KB_KeyDown[sc_Pause] && !(b&PAD_START)) KB_KeyDown[sc_Pause] = 0;

    /* buttons cleared by the game stay cleared until released */
    ctl_suppress1 &= raw1;
    ctl_suppress2 &= raw2;
    CONTROL_ButtonState1 = raw1 & ~ctl_suppress1;
    CONTROL_ButtonState2 = raw2 & ~ctl_suppress2;

    /* analog axes in the units the original mouse/joystick code produced (see getinput() in player.c):
       angvel = dyaw/64, vel = -dz>>6, svel -= dx, horiz += dpitch/186 */
    info->dx = info->dy = info->dz = info->dyaw = info->dpitch = info->droll = 0;
    if(pad.analog)
    {
        info->dx = (curve(pad.lx)*5)>>3;
        info->dz = curve(pad.ly)*40;
        info->dyaw = curve(pad.rx)*20;
        info->dpitch = -curve(pad.ry)*15;
    }
}

/* ------------------------------------------------------------------------------------------ */
/* MOUSE (not present)                                                                        */
/* ------------------------------------------------------------------------------------------ */

boolean MOUSE_Init( void ) { return false; }
void    MOUSE_Shutdown( void ) {}
void    MOUSE_ShowCursor( void ) {}
void    MOUSE_HideCursor( void ) {}
int32   MOUSE_GetButtons( void ) { return 0; }
void    MOUSE_GetPosition( int32*x, int32*y ) { *x = *y = 0; }
void    MOUSE_GetDelta( int32*x, int32*y ) { *x = *y = 0; }
