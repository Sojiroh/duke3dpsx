#include <psxapi.h>
#include <psxpad.h>
#include "psx.h"

static u8 padbuf[2][34];
PadState pad;

void pad_init(void)
{
	memset(padbuf, 0xff, sizeof(padbuf));
	InitPAD(padbuf[0], 34, padbuf[1], 34);
	StartPAD();
	ChangeClearPAD(0);
	memset(&pad, 0, sizeof(pad));
}

static s8 axis(u8 v)
{
	int a = (int)v - 128;
	if (a > -16 && a < 16) return 0; /* deadzone */
	if (a > 127) a = 127;
	return (s8)a;
}

void pad_poll(void)
{
	PADTYPE *p = (PADTYPE *)padbuf[0];
	u16 old = pad.btn;
	u16 btn = 0;
	pad.analog = 0;
	pad.lx = pad.ly = pad.rx = pad.ry = 0;
	if (p->stat == 0 && p->len != 0 && p->type != PAD_ID_NONE) {
		btn = ~p->btn;
		if (p->type == PAD_ID_ANALOG || p->type == PAD_ID_ANALOG_STICK) {
			pad.analog = 1;
			pad.lx = axis(p->ls_x); pad.ly = axis(p->ls_y);
			pad.rx = axis(p->rs_x); pad.ry = axis(p->rs_y);
		}
	}
	pad.btn = btn;
	pad.pressed = btn & ~old;
	pad.released = old & ~btn;
}
