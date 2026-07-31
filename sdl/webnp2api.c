/* WebNP2 (browser player) control API. Only for Emscripten builds.
   Called synchronously from JS while the main loop is suspended in
   emscripten_sleep(0) at a frame boundary (see np2exec in sdl/np2.c). */
#if defined(EMSCRIPTEN) && !defined(__LIBRETRO__)

#include	<emscripten.h>
#include	<compiler.h>
#include	<pccore.h>
#include	<fdd/diskdrv.h>
#include	<statsave.h>
#include	<vram/scrndraw.h>

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

#endif	/* EMSCRIPTEN && !__LIBRETRO__ */
