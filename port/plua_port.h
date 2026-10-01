/*
 * plua_port.h -- shared internals of the primeLua platform layer.
 *
 * Declares the launch contract between the calculator-side launcher
 * (calc/main.py) and lua.elf, the firmware entry points used by the port,
 * and the unwinding hooks the C library shim needs.
 */
#ifndef PLUA_PORT_H
#define PLUA_PORT_H

#include <setjmp.h>

/* ---------------------------------------------------------------------------
 * Launch contract.
 *
 * calc/main.py builds this structure in the loader's memory and calls the
 * ELF entry (plua_entry) with r0 = &config.  The entry stores that pointer
 * through hp_set_input_state(), so any later code reaches it with
 * hp_get_input_state() -- the same convention primetcc uses for its
 * code.elf/PRIMEIN handshake, kept because it needs no globals (TCC's
 * linker cannot relocate those; GCC's can, but one convention is better).
 *
 * argv strings live in the same block, NUL-terminated, so the C side can
 * hand them straight to Lua's own argument parsing.
 * ------------------------------------------------------------------------- */
#define PLUA_CFG_MAGIC	0x41554C50u	/* "PLUA" */

/*
 * Bumped BY HAND whenever the Lua-visible API changes.  sys.version() reports
 * it, the launcher prints it before every run, and the diagnostic scripts log
 * it first: a script that needs a newer binding than the interpreter on the
 * device then says so in one line ("attempt to call a nil value (field
 * 'target')" was the expensive way to learn it).
 */
#define PLUA_VERSION	"1.0"

/*
 * The default font size.  primeLua ships Montserrat (font.prop) and Cascadia
 * Code Light (font.mono / font.cascadia) at 16 / 24 / 32 px; 16 is what scripts
 * and the examples assume when they ask for no size at all.  font.*(size)
 * accept any size and round to the nearest face that exists, so scripts written
 * against primetcc's four sizes still run.
 */
#define PLUA_FONT_SIZE	16

struct plua_config {
	unsigned magic;
	int argc;			/* number of argv entries */
	char **argv;			/* argv[0..argc-1] */
	long epoch;			/* wall clock at launch, or -1 (32-bit:
					 * see the note in PRIMELUA_README.md) */
	unsigned heap_free;		/* free firmware heap bytes, or 0 */
	const char *logpath;		/* where plua_diag() appends, or NULL */
	const char *homedir;		/* directory a RELATIVE path resolves to,
					 * i.e. the folder holding the scripts */
};

/* ---------------------------------------------------------------------------
 * Durable diagnostics (plua_diag.c).
 *
 * The calculator's console is cleared by a reboot and MicroPython's handles
 * are not flushed when the box dies mid-run, so anything the interpreter
 * writes to the ring is LOST exactly when it matters most (a crash).  Every
 * diagnostic line therefore goes to a file, read-append-rewritten and CLOSED
 * before returning -- the same trick primetcc's diag_log uses.  The launcher
 * shows the tail of that file, so "it rebooted" becomes "it rebooted after
 * step N".
 * ------------------------------------------------------------------------- */
#define PLUA_PATH_MAX	512

void plua_diag(const char *msg);
void plua_set_homedir(const char *dir);
void plua_diag_hex(const char *tag, unsigned v);
void plua_set_logpath(const char *path);

/* Turn a script-relative name into the absolute path the firmware needs
 * (plua_file.c does the same for io.open); `out` must hold PLUA_PATH_MAX. */
const char *plua_resolve_path(const char *path, char *out, int cap);

/* GROB userdata plumbing, shared with port/plua_img.c: pushing a grob uses the
 * same metatable gfx.newgrob does, so every gfx call accepts a loaded image.
 * (Declared with void* so this header stays independent of Lua's headers --
 * plua_port.h is included by files that never see lua.h.) */
struct hp_grob;                  /* hp_gfx.h -- plua_port.h must stay
				  * compilable without primetcc's headers
				  * (the host tests build it with -Iport only) */
int             plua_push_grob(void *L, struct hp_grob *g);
struct hp_grob *plua_current_grob(void);

/* the img library's function table (port/plua_img.c); the luaL_Reg shape is
 * declared by lua.h/lauxlib.h, which plua_hp.c includes before this header */
struct luaL_Reg;
const struct luaL_Reg *plua_img_lib(void);

/* ---------------------------------------------------------------------------
 * Firmware entry points (plua_svc.s).  All use the pushed-r0 SVC
 * convention: arguments in r0-r3, result in r0.
 * ------------------------------------------------------------------------- */
unsigned hp_svc_fopen(unsigned path16, unsigned mode16);
unsigned hp_svc_fclose(unsigned fd);
unsigned hp_svc_fseek(unsigned fd, unsigned off, unsigned whence);
unsigned hp_svc_ftell(unsigned fd);
unsigned hp_svc_fread(unsigned buf, unsigned nmemb, unsigned size, unsigned fd);
unsigned hp_svc_fwrite(unsigned buf, unsigned nmemb, unsigned size, unsigned fd);
unsigned hp_svc_filesize(unsigned fd);
unsigned hp_svc_remove(unsigned path16);
unsigned hp_svc_malloc(unsigned n);
unsigned hp_svc_calloc(unsigned n, unsigned sz);
unsigned hp_svc_realloc(unsigned p, unsigned n);
void     hp_svc_free(unsigned p);
void     hp_svc_sleep(unsigned ms);
unsigned hp_svc_gettime(unsigned buf);   /* svc 0x100A5, writes the struct */
/* the firmware's own get_event (svc 0x1003F), called BY the input hook: the
 * kernel takes the SVC's return address off the stack, so this returns to its
 * caller like any other function (see port/plua_input.c) */
unsigned hp_svc_get_event(unsigned ev);
unsigned hp_svc_create_thread(unsigned fn, unsigned arg, unsigned stack);
/* set_thread_priority(handle, prio): the handle comes FIRST and the priority
 * second (puredoom.elf's main() passes r0 = create_thread's return value,
 * r1 = 60).  Getting the order wrong asks the firmware to reprioritise an
 * arbitrary thread and reboots this calculator -- see port/plua_run.c. */
void     hp_svc_set_thread_priority(unsigned handle, unsigned prio);
void     hp_flush_icache(void);

/* the interpreter's own entry pieces (plua_main.c), used by the entry in
 * plua_svc.s and by the thread body in plua_run.c */
void plua_begin(unsigned cfg, unsigned dbg_sp);
int  plua_main(void);
void plua_log_ret(unsigned v);
unsigned plua_stack_alloc(unsigned *scratch);
void plua_stack_free(unsigned *scratch);
void plua_save_a8(unsigned a8);
unsigned plua_get_a8(void);

/* from plua_rt.o (primetcc's rt/hp_rt.c) */
void     hp_set_input_state(unsigned v, unsigned dbg_sp);
unsigned hp_get_input_state(void);

/* ---------------------------------------------------------------------------
 * Program-exit unwinding (plua_libc.c / plua_main.c).
 *
 * Lua's standalone driver calls exit() on fatal errors, and the calculator
 * has nowhere to exit TO: exit() unwinds to the jmp_buf that plua_main()
 * installed, so the entry sequence can still hand the program stack back to
 * the firmware heap and return a sane value to the launcher.
 * ------------------------------------------------------------------------- */
extern jmp_buf plua_exit_env;
extern int plua_exit_code;
extern int plua_exit_armed;

void plua_set_epoch(long long epoch);

/* ---------------------------------------------------------------------------
 * the firmware clock (plua_time.c)
 *
 * svc 0x100A5 (SDKLIB's GetSysTime) fills a 16-byte structure whose layout was
 * measured on the calculator (calc/timeprobe.py, 2025-09-25):
 *
 *   u16 @ 0  year    @ 2 month   @ 4 day-of-week (0=Sunday)
 *   u16 @ 6  day     @ 8 hour    @10 minute      @12 second
 *   u16 @14  ?       @16 sub-second counter (u32)
 *
 * plua_fw_now() reads it once and returns Unix seconds (-1 when the machine
 * does not answer), with *tail -- when given -- set to the raw sub-second
 * counter, which is what os.clock() is built from.
 * ------------------------------------------------------------------------- */
long long plua_fw_now(unsigned long long *tail);
long long plua_systime_seconds(const unsigned short *f);

/* ---------------------------------------------------------------------------
 * file modes (plua_fwmode.c)
 *
 * The firmware's fopen accepts "rb" and "wb+" and nothing else that can be
 * relied on; Lua and the C library speak the usual C modes.  plua_fw_mode()
 * translates, and returns whether the old contents must be carried over first
 * (1 = update in place, 2 = append).
 * ------------------------------------------------------------------------- */
int plua_fw_mode(const char *mode, char *out, int cap);


#endif /* PLUA_PORT_H */
