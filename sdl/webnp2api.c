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

/* Push one entry into the PC-98 keyboard BIOS ring buffer (work area
   0x502-0x521, head=0x524, tail=0x526, count=0x528 — standard layout,
   same as bios/bios09.c). entry = (scan << 8) | charcode. This bypasses
   the keyboard hardware, so host-side IME-composed Shift_JIS bytes can
   be fed to guest DOS standard input without a guest FEP.
   Returns 1 when pushed, 0 when the buffer is full (caller retries). */
EMSCRIPTEN_KEEPALIVE int webnp2_push_key_buffer(int entry) {
	UINT kbbuftail;

	if (mem[0x528] >= 0x10) {
		return 0;
	}
	mem[0x528]++;
	kbbuftail = LOADINTELWORD(mem + 0x526);
	if ((kbbuftail < 0x502) || (kbbuftail >= 0x522)) {
		kbbuftail = 0x502;
	}
	STOREINTELWORD(mem + kbbuftail, (UINT16)entry);
	kbbuftail += 2;
	if (kbbuftail >= 0x522) {
		kbbuftail = 0x502;
	}
	STOREINTELWORD(mem + 0x526, kbbuftail);
	return 1;
}

/* Push a 2-byte (DBCS) character atomically: both entries are stored in
   one call so the guest can never consume the lead byte while the trail
   byte is still missing (which would break Shift_JIS pairing).
   Returns 1 when pushed, 0 when fewer than 2 slots are free. */
EMSCRIPTEN_KEEPALIVE int webnp2_push_key_buffer_pair(int e1, int e2) {
	if (mem[0x528] >= 0x0f) {
		return 0;
	}
	webnp2_push_key_buffer(e1);
	webnp2_push_key_buffer(e2);
	return 1;
}

/* Bus-mouse injection for automation.

   The PC-98 bus mouse only reports relative movement, and the guest owns
   the pointer position, so absolute positioning is done on the JS side by
   homing into a screen corner first and then stepping to the target.
   Movement is accumulated in mousemng and drained when the guest polls;
   webnp2_mouse_pending() lets the caller pace the steps so that a large
   move is not truncated by the protocol's per-read range.

   These bypass the pointer-lock capture state on purpose: automation must
   work without the browser holding the real cursor. */
EMSCRIPTEN_KEEPALIVE void webnp2_mouse_move(int dx, int dy) {
	mousemng_sync(dx, dy);
}

EMSCRIPTEN_KEEPALIVE int webnp2_mouse_pending(void) {
	int	x;
	int	y;

	x = mousemng.x;
	y = mousemng.y;
	if (x < 0) {
		x = -x;
	}
	if (y < 0) {
		y = -y;
	}
	return (x > y) ? x : y;
}

/* button: 0=left, 1=right. A cleared bit means "pressed". */
EMSCRIPTEN_KEEPALIVE void webnp2_mouse_button(int button, int down) {
	UINT8	bit;

	switch (button) {
		case 0:
			bit = uPD8255A_LEFTBIT;
			break;
		case 1:
			bit = uPD8255A_RIGHTBIT;
			break;
		default:
			return;
	}
	if (down) {
		mousemng.btn &= ~bit;
	}
	else {
		mousemng.btn |= bit;
	}
}

/* Host-side text paste via the guest-resident helper (PASTE.COM).

   The TSR keeps a mailbox in conventional memory:
     +0  'WEBNP2MB'   signature
     +8  head (word)  read cursor, advanced by the guest
     +10 tail (word)  write cursor, advanced by the host
     +12 size (word)  ring buffer size (256)
     +14 pending(word) 1 while the host is still delivering a line
     +16 buf[size]

   webnp2_find_mailbox() scans conventional memory for a structurally
   valid mailbox and returns its linear address, or -1 when the helper
   is not resident. Only main memory below 640KB is searched, so disk
   images or other host-side buffers can never produce a false hit. */
#define	WEBNP2_MB_SIZE		256
#define	WEBNP2_MB_BUF		18
#define	WEBNP2_MB_INSTALLED	0x4b57		/* set by the TSR once resident */
#define	WEBNP2_MB_SIGOFF	0x104		/* signature offset inside the .COM segment */

static BOOL webnp2_mailbox_valid(UINT32 addr) {
	UINT16	head;
	UINT16	tail;
	UINT16	size;

	if (addr + WEBNP2_MB_BUF + WEBNP2_MB_SIZE > 0xa0000) {
		return FALSE;
	}
	if (mem[addr] != 'W' || mem[addr+1] != 'E' || mem[addr+2] != 'B' ||
		mem[addr+3] != 'N' || mem[addr+4] != 'P' || mem[addr+5] != '2' ||
		mem[addr+6] != 'M' || mem[addr+7] != 'B') {
		return FALSE;
	}
	head = LOADINTELWORD(mem + addr + 8);
	tail = LOADINTELWORD(mem + addr + 10);
	size = LOADINTELWORD(mem + addr + 12);
	/* The "installed" word is zero in the on-disk PASTE.COM image, so a
	   stale copy sitting in a DOS disk buffer is rejected here. */
	if (LOADINTELWORD(mem + addr + 16) != WEBNP2_MB_INSTALLED) {
		return FALSE;
	}
	return (size == WEBNP2_MB_SIZE) && (head < size) && (tail < size);
}

EMSCRIPTEN_KEEPALIVE int webnp2_find_mailbox(void) {
	UINT32	addr;
	UINT16	seg;

	/* The resident copy lives in the segment the INT 21h vector points at,
	   so look there first: that can never hit a stale disk-buffer copy. */
	seg = LOADINTELWORD(mem + 0x86);
	addr = ((UINT32)seg << 4) + WEBNP2_MB_SIGOFF;
	if (webnp2_mailbox_valid(addr)) {
		return (int)addr;
	}

	for (addr = 0; addr < 0xa0000; addr += 2) {
		if (webnp2_mailbox_valid(addr)) {
			return (int)addr;
		}
	}
	return -1;
}

/* Free space in the mailbox ring buffer (one slot is kept empty so that
   head==tail always means "empty"). */
EMSCRIPTEN_KEEPALIVE int webnp2_mailbox_space(int addr) {
	UINT16	head;
	UINT16	tail;
	int		used;

	if (addr < 0) {
		return 0;
	}
	head = LOADINTELWORD(mem + addr + 8);
	tail = LOADINTELWORD(mem + addr + 10);
	used = (int)tail - (int)head;
	if (used < 0) {
		used += WEBNP2_MB_SIZE;
	}
	return WEBNP2_MB_SIZE - 1 - used;
}

/* Set the "host is still delivering a line" flag. */
EMSCRIPTEN_KEEPALIVE void webnp2_mailbox_pending(int addr, int pending) {
	if (addr < 0) {
		return;
	}
	STOREINTELWORD(mem + addr + 14, (UINT16)(pending ? 1 : 0));
}

/* Append one byte to the mailbox. Returns 1 on success, 0 when full. */
EMSCRIPTEN_KEEPALIVE int webnp2_mailbox_put(int addr, int value) {
	UINT16	tail;
	UINT16	next;

	if ((addr < 0) || (webnp2_mailbox_space(addr) <= 0)) {
		return 0;
	}
	tail = LOADINTELWORD(mem + addr + 10);
	mem[addr + WEBNP2_MB_BUF + tail] = (UINT8)value;
	next = (UINT16)((tail + 1) % WEBNP2_MB_SIZE);
	STOREINTELWORD(mem + addr + 10, next);
	return 1;
}

/* デバッグ/解析用。ホスト側からゲストRAMを直接読むための入口。
   PC-98 メインRAM(0x200000バイト)の先頭ポインタを返す。 */
EMSCRIPTEN_KEEPALIVE UINT8 *webnp2_mem_ptr(void) {
	return mem;
}

/* デバッグ/解析用。webnp2_mem_ptr() が指すメインRAMのサイズ(バイト数)を返す。 */
EMSCRIPTEN_KEEPALIVE int webnp2_mem_size(void) {
	return 0x200000;
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

/* ドライブアクセスランプ用のカウンタ。
   コア側の通知フック sysmng_fddaccess()/sysmng_hddaccess() から呼ばれる
   (sdl/sysmng.h で Emscripten のときだけ本関数へ差し替えている)。
   read/write/readid/writeid のたびに該当ドライブのカウンタを進めるだけで、
   点灯時間の管理はJS側に任せる(呼ばれた回数ではなく「変化したか」を見る)。 */
#define	WEBNP2_FDD_MAX		4
#define	WEBNP2_HDD_SLOT		WEBNP2_FDD_MAX	/* HDDは全ドライブまとめて1つ */
#define	WEBNP2_ACCESS_MAX	(WEBNP2_FDD_MAX + 1)

static UINT32 s_diskaccess[WEBNP2_ACCESS_MAX];

void webnp2_note_fdd_access(UINT8 drv) {
	if (drv < WEBNP2_FDD_MAX) {
		s_diskaccess[drv]++;
	}
}

void webnp2_note_hdd_access(UINT8 drv) {
	(void)drv;			/* WebNP2 は HDD を1台しか載せないためまとめて扱う */
	s_diskaccess[WEBNP2_HDD_SLOT]++;
}

/* アクセスカウンタ配列(UINT32 x WEBNP2_ACCESS_MAX)の先頭ポインタ。
   [0..3]=FDD1..FDD4、[4]=HDD。JS側は前回値との差分で点灯を判断する。 */
EMSCRIPTEN_KEEPALIVE UINT32 *webnp2_disk_access(void) {
	return s_diskaccess;
}

/* webnp2_disk_access() が返す配列の要素数。 */
EMSCRIPTEN_KEEPALIVE int webnp2_disk_access_count(void) {
	return WEBNP2_ACCESS_MAX;
}

#endif	/* EMSCRIPTEN && !__LIBRETRO__ */
