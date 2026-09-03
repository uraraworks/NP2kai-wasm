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
#include	<vram/dispsync.h>
#include	"mousemng.h"
#if defined(CPUCORE_IA32)
#include	<cpu.h>
#include	<generic/unasm.h>
#endif

static int s_dbg_paused;
static int s_pause_redraw;

EMSCRIPTEN_KEEPALIVE void webnp2_request_pause_redraw(void);

/* デバッガ用の一時停止状態を設定する。0以外で一時停止する。
   一時停止に入った瞬間の画面を反映させるため、再描画要求も立てる。 */
EMSCRIPTEN_KEEPALIVE void webnp2_dbg_set_paused(int paused) {
	s_dbg_paused = paused ? 1 : 0;
	if (s_dbg_paused) {
		webnp2_request_pause_redraw();
	}
}

/* デバッガ用の一時停止状態を返す。1なら一時停止中。 */
EMSCRIPTEN_KEEPALIVE int webnp2_dbg_paused(void) {
	return s_dbg_paused;
}

/* ポーズ中に1回だけ画面を描き直させる。ポーズ突入時とステップ実行後に立てる。 */
EMSCRIPTEN_KEEPALIVE void webnp2_request_pause_redraw(void) {
	s_pause_redraw = 1;
}

/* 要求を1つ取り出す(取ったらクリア)。np2exec()のポーズ分岐から呼ぶ。 */
EMSCRIPTEN_KEEPALIVE int webnp2_take_pause_redraw(void) {
	int	v = s_pause_redraw;
	s_pause_redraw = 0;
	return v;
}

/* FDDシーク音のON/OFFを切り替える。
   MOTOR は fdc.c がシークのたびに参照するため即時反映されるが、
   MOTORVOL は sound_init() 実行時に pccore.c の fddmtrsnd_initialize が
   一度だけ読む値。起動時の cfg.Seek_Vol が 0 だとミキサトラック自体が
   登録されず、後からここで MOTOR を立てても無音のままになる点に注意。 */
EMSCRIPTEN_KEEPALIVE void webnp2_seeksnd_set(int on) {
	np2cfg.MOTOR = on ? 1 : 0;
}

/* FDDシーク音のON/OFF状態を返す。1ならON。 */
EMSCRIPTEN_KEEPALIVE int webnp2_seeksnd(void) {
	return np2cfg.MOTOR ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE void webnp2_reset(void) {
	pccore_cfgupdate();
	pccore_reset();
}

/* drive: 0-3, path: MEMFS path. NULL or "" ejects the disk. */
/* ドライブが読み書きできる状態かを返す。0なら挿入遅延中。
   webnp2_set_fdd() の直後は 20 フレーム(約0.4秒)の挿入遅延があり、その間FDCは
   Not Ready を返す。遅延はエミュレート1フレームごとに減るので、実時間で待っても
   足りるとは限らない。挿入後すぐアクセスする側はこれで準備完了を待つこと。 */
EMSCRIPTEN_KEEPALIVE int webnp2_fdd_ready(int drive) {
	if (drive < 0 || drive >= 4) {
		return 0;
	}
	return diskdrv_isfddready((REG8)drive);
}

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

   cols/rows follow the current screen mode; they are NOT constants.
   The guest can switch between 25 lines (16 scanlines per row) and
   20 lines (20 scanlines per row) via INT 18h AH=0Ah, and cols follows
   the GDC pitch. Reporting a fixed 80x25 made mode changes invisible to
   automation and produced wrong measurements, so both are derived here:

     cols = GDC pitch (the row stride in cells; the logical width)
     rows = active text scanlines / scanlines per row
            = (dsync.textymax - dsync.text_vbp) / ((CSRFORM & 0x1f) + 1)

   The buffer is sized for the maximum and the cells are packed to the
   reported cols*rows, so callers must read cols/rows from the header
   rather than assuming a stride.

   Buffer layout (little endian):
     [0]    cols
     [1]    rows
     [2..3] cursor cell index as int16 (-1 when hidden/offscreen)
     [4..]  cols*rows cells, 2 bytes each:
              0x0000-0x00FF          ANK (JIS X 0201) code
              hi >= 0x21             JIS X 0208 code (jis1<<8)|jis2;
                                     the right half of a fullwidth char
                                     is emitted as 0x0000 */
#define	WEBNP2_TVRAM_MAXCOLS	128
#define	WEBNP2_TVRAM_MAXROWS	64
static UINT8 s_tvram[4 + WEBNP2_TVRAM_MAXCOLS * WEBNP2_TVRAM_MAXROWS * 2];

EMSCRIPTEN_KEEPALIVE int webnp2_tvram_size(void) {
	return (int)sizeof(s_tvram);
}

EMSCRIPTEN_KEEPALIVE UINT8 *webnp2_read_tvram(void) {
	UINT	pitch;
	UINT	cols;
	UINT	rows;
	UINT	lr;
	UINT	ylen;
	UINT	esi;
	UINT16	csrw;
	int		cursor;
	int		curdisp;
	UINT8	*p;
	int		x;
	int		y;

	pitch = gdc.m.para[GDC_PITCH] & 0xfe;
	cols = pitch;
	if (cols == 0) {
		cols = 1;
	}
	if (cols > WEBNP2_TVRAM_MAXCOLS) {
		cols = WEBNP2_TVRAM_MAXCOLS;
	}
	lr = (UINT)(gdc.m.para[GDC_CSRFORM] & 0x1f) + 1;
	ylen = (dsync.textymax > dsync.text_vbp)
				? (UINT)(dsync.textymax - dsync.text_vbp) : 0;
	rows = ylen / lr;
	if (rows == 0) {
		rows = 1;
	}
	if (rows > WEBNP2_TVRAM_MAXROWS) {
		rows = WEBNP2_TVRAM_MAXROWS;
	}

	esi = LOW12(LOADINTELWORD(gdc.m.para + GDC_SCROLL));
	csrw = LOADINTELWORD(gdc.m.para + GDC_CSRW);
	curdisp = ((gdc.m.para[GDC_CSRFORM] & 0x80) != 0);
	cursor = -1;
	p = s_tvram + 4;
	for (y = 0; y < (int)rows; y++) {
		UINT edi = esi;
		BOOL kanji2nd = FALSE;
		for (x = 0; x < (int)cols; x++) {
			UINT16 out;
			if ((curdisp) && (edi == csrw)) {
				cursor = y * (int)cols + x;
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
	s_tvram[0] = (UINT8)cols;
	s_tvram[1] = (UINT8)rows;
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

#if defined(CPUCORE_IA32)

/* ia32_step() は残クロックが正の間ループするため、-1にして1命令に限定する。
   例外と割り込みは ia32_step() 内の sigsetjmp 経由で処理させる。 */
EMSCRIPTEN_KEEPALIVE int webnp2_dbg_step(int count) {
	int	executed;

	if (!s_dbg_paused || count <= 0) {
		return 0;
	}
	for (executed = 0; executed < count; executed++) {
		CPU_REMCLOCK = -1;
		ia32_step();
	}
	if (executed > 0) {
		webnp2_request_pause_redraw();
	}
	return executed;
}

/* UINT32配列のレイアウト:
   [0]=EAX, [1]=ECX, [2]=EDX, [3]=EBX, [4]=ESP, [5]=EBP,
   [6]=ESI, [7]=EDI, [8]=EIP, [9]=EFLAGS, [10]=CS, [11]=DS,
   [12]=ES, [13]=SS, [14]=FS, [15]=GS, [16]=CR0。 */
#define	WEBNP2_DBG_REGS_COUNT	17
static UINT32 s_dbg_regs[WEBNP2_DBG_REGS_COUNT];

EMSCRIPTEN_KEEPALIVE int webnp2_dbg_regs_size(void) {
	return (int)sizeof(s_dbg_regs);
}

EMSCRIPTEN_KEEPALIVE UINT32 *webnp2_dbg_regs(void) {
	s_dbg_regs[0] = CPU_EAX;
	s_dbg_regs[1] = CPU_ECX;
	s_dbg_regs[2] = CPU_EDX;
	s_dbg_regs[3] = CPU_EBX;
	s_dbg_regs[4] = CPU_ESP;
	s_dbg_regs[5] = CPU_EBP;
	s_dbg_regs[6] = CPU_ESI;
	s_dbg_regs[7] = CPU_EDI;
	s_dbg_regs[8] = CPU_EIP;
	s_dbg_regs[9] = CPU_EFLAG;
	s_dbg_regs[10] = CPU_CS;
	s_dbg_regs[11] = CPU_DS;
	s_dbg_regs[12] = CPU_ES;
	s_dbg_regs[13] = CPU_SS;
	s_dbg_regs[14] = CPU_FS;
	s_dbg_regs[15] = CPU_GS;
	s_dbg_regs[16] = CPU_CR0;
	return s_dbg_regs;
}

#define	WEBNP2_DBG_DISASM_MAX	128
#define	WEBNP2_DBG_DISASM_SIZE	(WEBNP2_DBG_DISASM_MAX * 352 + 1)
static char s_dbg_disasm[WEBNP2_DBG_DISASM_SIZE];

static void webnp2_dbg_append(char **dst, size_t *remain, const char *src) {
	size_t	len;

	if (*remain <= 1) {
		return;
	}
	len = strlen(src);
	if (len >= *remain) {
		len = *remain - 1;
	}
	memcpy(*dst, src, len);
	*dst += len;
	*remain -= len;
	**dst = '\0';
}

static int webnp2_dbg_get_cs_desc(UINT16 seg, descriptor_t *desc) {
	selector_t	sel;

	if (!CPU_STAT_PM || CPU_STAT_VM86) {
		*desc = CPU_CS_DESC;
		desc->valid = 1;
		desc->p = 1;
		desc->d = 0;
		desc->u.seg.segbase = (UINT32)seg << 4;
		desc->u.seg.limit = 0xffff;
		return 1;
	}
	if (seg == CPU_CS) {
		*desc = CPU_CS_DESC;
		return 1;
	}
	if (parse_selector(&sel, seg) != 0 || !SEG_IS_VALID(&sel.desc) ||
		!SEG_IS_PRESENT(&sel.desc) || !SEG_IS_CODE(&sel.desc)) {
		return 0;
	}
	*desc = sel.desc;
	return 1;
}

static void webnp2_dbg_read_code(const descriptor_t *desc, UINT32 off,
								UINT8 *buf, int size) {
	UINT32	addr;
	UINT32	pde;
	UINT32	pte;
	int		i;

	for (i = 0; i < size; i++) {
		addr = desc->u.seg.segbase + off + (UINT32)i;
		if (CPU_STAT_PAGING) {
			pde = cpu_memoryread_d(CPU_STAT_PDE_BASE + ((addr >> 20) & 0xffc));
			pte = cpu_memoryread_d((pde & CPU_PDE_BASEADDR_MASK) +
								((addr >> 10) & 0xffc));
			addr = (pte & CPU_PTE_BASEADDR_MASK) + (addr & 0x00000fff);
		}
		buf[i] = cpu_memoryread(addr);
	}
}

/* 指定した seg:off から最大128命令を逆アセンブルする。
   戻り値は静的文字列で、各行は
   「命令長<TAB>16進バイト列<TAB>ニーモニックとオペランド<LF>」。
   不正命令は「1<TAB>??<TAB><invalid>」として1バイト進める。 */
EMSCRIPTEN_KEEPALIVE char *webnp2_dbg_disasm(int seg, int off, int count) {
	descriptor_t	desc;
	_UNASM		una;
	UINT8		code[16];
	UINT32		eip;
	char		*p;
	size_t		remain;
	char		tmp[32];
	UINT		len;
	int		i;

	s_dbg_disasm[0] = '\0';
	if (count <= 0 || !webnp2_dbg_get_cs_desc((UINT16)seg, &desc)) {
		return s_dbg_disasm;
	}
	if (count > WEBNP2_DBG_DISASM_MAX) {
		count = WEBNP2_DBG_DISASM_MAX;
	}

	p = s_dbg_disasm;
	remain = sizeof(s_dbg_disasm);
	eip = (UINT32)off;
	for (i = 0; i < count; i++) {
		webnp2_dbg_read_code(&desc, eip, code, sizeof(code));
		len = unasm(&una, code, sizeof(code), desc.d, eip);
		if (len > 0) {
			int j;

			snprintf(tmp, sizeof(tmp), "%u\t", len);
			webnp2_dbg_append(&p, &remain, tmp);
			for (j = 0; j < (int)len; j++) {
				snprintf(tmp, sizeof(tmp), "%02x", code[j]);
				webnp2_dbg_append(&p, &remain, tmp);
			}
			webnp2_dbg_append(&p, &remain, "\t");
			webnp2_dbg_append(&p, &remain, una.mnemonic);
			if (una.operand[0] != '\0') {
				webnp2_dbg_append(&p, &remain, " ");
				webnp2_dbg_append(&p, &remain, una.operand);
			}
			webnp2_dbg_append(&p, &remain, "\n");
			eip += len;
		}
		else {
			webnp2_dbg_append(&p, &remain, "1\t??\t<invalid>\n");
			eip++;
		}
	}
	return s_dbg_disasm;
}

#define	WEBNP2_DBG_BP_MAX	8
typedef struct {
	UINT16	seg;
	UINT32	off;
	int		enabled;
} WEBNP2_DBG_BP;

static WEBNP2_DBG_BP s_dbg_bp[WEBNP2_DBG_BP_MAX];

/* indexは0..7。範囲外の指定は無視する。 */
EMSCRIPTEN_KEEPALIVE void webnp2_dbg_set_bp(int index, int seg, int off, int enabled) {
	if (index < 0 || index >= WEBNP2_DBG_BP_MAX) {
		return;
	}
	s_dbg_bp[index].seg = (UINT16)seg;
	s_dbg_bp[index].off = (UINT32)off;
	s_dbg_bp[index].enabled = enabled ? 1 : 0;
}

/* 1命令ごとに実行後のCS:EIPを照合し、ヒットしたBP indexを返す。
   一時停止中でない場合、または無ヒットの場合は-1を返す。 */
EMSCRIPTEN_KEEPALIVE int webnp2_dbg_run_until_bp(int max_steps) {
	int	index;
	int	step;

	if (!s_dbg_paused || max_steps <= 0) {
		return -1;
	}
	for (step = 0; step < max_steps; step++) {
		if (webnp2_dbg_step(1) != 1) {
			break;
		}
		for (index = 0; index < WEBNP2_DBG_BP_MAX; index++) {
			if (s_dbg_bp[index].enabled && s_dbg_bp[index].seg == CPU_CS &&
				s_dbg_bp[index].off == CPU_EIP) {
				return index;
			}
		}
	}
	return -1;
}

#endif	/* CPUCORE_IA32 */

#endif	/* EMSCRIPTEN && !__LIBRETRO__ */
