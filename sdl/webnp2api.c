/* WebNP2 (browser player) control API. Only for Emscripten builds.
   Called synchronously from JS while the main loop is suspended in
   emscripten_sleep(0) at a frame boundary (see np2exec in sdl/np2.c). */
#if defined(EMSCRIPTEN) && !defined(__LIBRETRO__)

#include	<emscripten.h>
#include	<compiler.h>
#include	<cpucore.h>
#include	<pccore.h>
#include	<io/iocore.h>
#include	<fdd/diskdrv.h>
#include	<statsave.h>
#include	<keystat.h>
#include	<vram/scrndraw.h>
#include	"mousemng.h"

EMSCRIPTEN_KEEPALIVE void webnp2_reset(void) {
	pccore_cfgupdate();
	pccore_reset();
}

/* drive: 0-3, path: MEMFS path. NULL or "" ejects the disk. */
EMSCRIPTEN_KEEPALIVE void webnp2_set_fdd(int drive, const char *path) {
	if (drive < 0 || drive >= 4) {
		return;
	}
	diskdrv_setfdd((REG8)drive, (path != NULL && path[0] != '\0') ? path : NULL, 0);
}

/* statsave_save()/statsave_load() only queue the request (g_u8ControlState)
   for the next main-loop iteration, but the JS caller needs the state file
   to exist when this returns. We are already suspended at a frame boundary,
   so cancel the queued request and run the deferred body synchronously.
   Returns STATFLAG_SUCCESS(0) on success. */
EMSCRIPTEN_KEEPALIVE int webnp2_statsave(const char *path) {
	statsave_save(path);		/* stores path into m_strStateFilename */
	g_u8ControlState = 0;
	return statsave_save_d();
}

EMSCRIPTEN_KEEPALIVE int webnp2_statload(const char *path) {
	int ret;

	statsave_load(path);
	g_u8ControlState = 0;
	ret = statsave_load_d();
	scrndraw_redraw();		/* the load path does not redraw by itself */
	return ret;
}

/* Toggle bus-mouse capture (same as the Ctrl+F12 handler in taskmng.c).
   Under Emscripten, capturing requests browser pointer lock, so this must be
   called from within a user gesture (e.g. a click handler on the JS side).
   Returns the new capture state (1=captured). */
EMSCRIPTEN_KEEPALIVE int webnp2_mouse_toggle(void) {
	mousemng_toggle(MOUSEPROC_SYSTEM);
	return ismouse_captured();
}

EMSCRIPTEN_KEEPALIVE int webnp2_mouse_captured(void) {
	return ismouse_captured();
}

/* Inject a PC-98 keyboard scan code (make when down=1, break when down=0).
   Same path as the SDL keyboard handler and sysmenu's key injection. */
EMSCRIPTEN_KEEPALIVE void webnp2_key(int code, int down) {
	keystat_senddata((REG8)((code & 0x7f) | (down ? 0x00 : 0x80)));
}

/* Text screen (TVRAM) readout for automation.
   The cell addressing (GDC scroll origin + pitch per row) mirrors
   vram/maketext.c so DOS scrolling is followed correctly.

   Buffer layout (little endian):
     [0]    cols (80)
     [1]    rows (25)
     [2..3] cursor cell index as int16 (-1 when hidden/offscreen)
     [4..]  cols*rows cells, 2 bytes each:
              0x0000-0x00FF          ANK (JIS X 0201) code
              hi >= 0x21             JIS X 0208 code (jis1<<8)|jis2;
                                     the right half of a fullwidth char
                                     is emitted as 0x0000 */
#define	WEBNP2_TVRAM_COLS	80
#define	WEBNP2_TVRAM_ROWS	25
static UINT8 s_tvram[4 + WEBNP2_TVRAM_COLS * WEBNP2_TVRAM_ROWS * 2];

EMSCRIPTEN_KEEPALIVE int webnp2_tvram_size(void) {
	return (int)sizeof(s_tvram);
}

EMSCRIPTEN_KEEPALIVE UINT8 *webnp2_read_tvram(void) {
	UINT	pitch;
	UINT	esi;
	UINT16	csrw;
	int		cursor;
	int		curdisp;
	UINT8	*p;
	int		x;
	int		y;

	pitch = gdc.m.para[GDC_PITCH] & 0xfe;
	esi = LOW12(LOADINTELWORD(gdc.m.para + GDC_SCROLL));
	csrw = LOADINTELWORD(gdc.m.para + GDC_CSRW);
	curdisp = ((gdc.m.para[GDC_CSRFORM] & 0x80) != 0);
	cursor = -1;
	p = s_tvram + 4;
	for (y = 0; y < WEBNP2_TVRAM_ROWS; y++) {
		UINT edi = esi;
		BOOL kanji2nd = FALSE;
		for (x = 0; x < WEBNP2_TVRAM_COLS; x++) {
			UINT16 out;
			if ((curdisp) && (edi == csrw)) {
				cursor = y * WEBNP2_TVRAM_COLS + x;
			}
			if (kanji2nd) {
				kanji2nd = FALSE;
				out = 0;
			}
			else if (!(mem[0xa0001 + edi*2] & gdc.bitac)) {
				out = mem[0xa0000 + edi*2];
			}
			else {
				UINT kc = LOADINTELWORD(mem + 0xa0000 + edi*2);
				UINT ku = kc & 0x7f;
				if ((ku == 0x56) || (ku == 0x57)) {
					out = '?';							/* gaiji */
				}
				else {
					out = (UINT16)((((kc & 0x7f) + 0x20) << 8) |
									((kc >> 8) & 0x7f));
					if ((ku < 0x09) || (ku >= 0x0c)) {
						kanji2nd = TRUE;
					}
				}
			}
			STOREINTELWORD(p, out);
			p += 2;
			edi = LOW12(edi + 1);
		}
		esi = LOW12(esi + pitch);
	}
	s_tvram[0] = WEBNP2_TVRAM_COLS;
	s_tvram[1] = WEBNP2_TVRAM_ROWS;
	STOREINTELWORD(s_tvram + 2, (UINT16)cursor);
	return s_tvram;
}

#endif	/* EMSCRIPTEN && !__LIBRETRO__ */
