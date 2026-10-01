/*
 * plua_svc.s -- primeLua's ARM layer: firmware SVC wrappers, setjmp/longjmp
 * and the ELF entry point.
 *
 * Firmware calling convention (same as DOOM / PureDOOM / primetcc):
 *     push {r0}
 *     push {lr}
 *     svc  <service id>
 * The firmware's SVC dispatcher finds the argument block through the pushed
 * r0, calls the service with r0-r3, and returns through the pushed LR, so to
 * the caller this looks like an ordinary AAPCS call with up to four integer
 * arguments and the result in r0.
 *
 * Target: ARM926EJ-S (ARMv5TEJ), soft float, position-independent.  The ELF
 * is loaded by the calculator's MicroPython shellcode loader, which applies
 * R_ARM_RELATIVE relocations only, hence -fPIC everywhere and no absolute
 * addresses in this file (literals are PC-relative via ldr =sym).
 */
	.arm
	.syntax unified
	.text

/* ---------------- the ones primetcc's hardware libraries need ---------- */
	.globl hp_svc_getlcd
hp_svc_getlcd:                      /* get_lcd() -> LCD struct pointer */
	push {r0}
	push {lr}
	svc 0x1008d

	.globl hp_svc_get_event
hp_svc_get_event:                   /* get_event(ev) -- blocking on the OS side;
	                                     the input hook is what makes it usable */
	push {r0}
	push {lr}
	svc 0x1003f

	.globl hp_svc_create_thread
hp_svc_create_thread:
	push {r0}
	push {lr}
	svc 0x10000

	.globl hp_svc_set_thread_priority
hp_svc_set_thread_priority:         /* set_thread_priority(prio) -- DOOM calls
	                                     it with 60 right after creating the
	                                     thread that runs the game */
	push {r0}
	push {lr}
	svc 0x10003

	.globl hp_svc_terminate_thread
hp_svc_terminate_thread:
	push {r0}
	push {lr}
	svc 0x10001

/* ---------------- file system ---------------- */
	.globl hp_svc_fopen
hp_svc_fopen:                       /* fopen(utf16path, utf16mode) */
	push {r0}
	push {lr}
	svc 0x1026f

	.globl hp_svc_fclose
hp_svc_fclose:                      /* fclose(fd) */
	push {r0}
	push {lr}
	svc 0x100ca

	.globl hp_svc_fseek
hp_svc_fseek:                       /* fseek(fd, offset, whence) */
	push {r0}
	push {lr}
	svc 0x100cf

	.globl hp_svc_ftell
hp_svc_ftell:                       /* ftell(fd) */
	push {r0}
	push {lr}
	svc 0x100d0

	.globl hp_svc_fread
hp_svc_fread:                       /* fread(buf, nmemb, size, fd) */
	push {r0}
	push {lr}
	svc 0x100d4

	.globl hp_svc_fwrite
hp_svc_fwrite:                      /* fwrite(buf, nmemb, size, fd) */
	push {r0}
	push {lr}
	svc 0x100d7

	.globl hp_svc_filesize
hp_svc_filesize:                    /* filesize(fd) */
	push {r0}
	push {lr}
	svc 0x100cb

	.globl hp_svc_remove
hp_svc_remove:                      /* W_REMOVE(utf16path) */
	push {r0}
	push {lr}
	svc 0x10274

/* ---------------- the ones primetcc's hardware libraries need ---------- */

/* ---------------- heap / misc ---------------- */
	.globl hp_svc_malloc
hp_svc_malloc:
	push {r0}
	push {lr}
	svc 0x10037

	.globl hp_svc_calloc
hp_svc_calloc:
	push {r0}
	push {lr}
	svc 0x10038

	.globl hp_svc_realloc
hp_svc_realloc:
	push {r0}
	push {lr}
	svc 0x10039

	.globl hp_svc_free
hp_svc_free:
	push {r0}
	push {lr}
	svc 0x1003a

	.globl hp_svc_sleep
hp_svc_sleep:                       /* os_sleep(ms) */
	push {r0}
	push {lr}
	svc 0x10008

/* gettime(struct *out) -- the firmware clock, svc 0x100A5.  SDKLIB exports it
 * as "GetSysTime" and its body IS this stub, so the one-argument form is the
 * vendor's own: the 2025-09-25 device run (calc/timeprobe.py) got 2026 back in
 * r0 and 16 bytes of u16 date/time fields in the buffer.  Calling it with
 * r0 = 0 (what primetcc did) writes through NULL and resets the machine. */
	.globl hp_svc_gettime
hp_svc_gettime:
	push {r0}
	push {lr}
	svc 0x100a5

/* ---------------- the ones primetcc's hardware libraries need ---------- */

/* ---------------------------------------------------------------------------
 * setjmp / longjmp (AAPCS, soft float).
 *
 * Lua's whole error mechanism (ldo.c LUAI_THROW / LUAI_TRY, and therefore
 * pcall/xpcall/error) is built on these two.  A jmp_buf is 10 words:
 * the callee-saved core registers r4-r11, sp, lr.  Soft float means there
 * are no callee-saved floating-point registers to preserve -- the reason
 * this port is built -mfloat-abi=soft.
 * ------------------------------------------------------------------------- */
	.globl setjmp
setjmp:                             /* r0 = jmp_buf */
	stmia r0!, {r4, r5, r6, r7, r8, r9, r10, r11}
	str   sp, [r0], #4
	str   lr, [r0]
	mov   r0, #0
	bx    lr

	.globl longjmp
longjmp:                            /* r0 = jmp_buf, r1 = value */
	ldmia r0!, {r4, r5, r6, r7, r8, r9, r10, r11}
	ldr   sp, [r0], #4
	ldr   lr, [r0]
	cmp   r1, #0
	moveq r0, #1                    /* longjmp(env, 0) must not return 0 */
	movne r0, r1
	bx    lr

/* ---------------- the ones primetcc's hardware libraries need ---------- */

/* ---------------------------------------------------------------------------
 * ELF entry: plua_entry.
 *
 * The launcher calls the entry with r0 = &struct plua_config on the debug
 * interface's stack, which is (a) shallow and (b) only 4-byte aligned.
 *
 * TWO WAYS TO RUN, decided by the run state's policy word (plua_run.c):
 *
 *   THREAD (default) -- start the interpreter on a FIRMWARE THREAD (svc
 *      0x10000, the same call PureDOOM's main() makes) and return its run
 *      state address to the launcher AT ONCE.  The launcher then streams the
 *      output ring while the program runs, which is the whole point: with the
 *      old "run it here" entry the debug call did not come back until Lua had
 *      finished, so nothing a script printed was visible before it exited.
 *      The thread's stack is the firmware's (512 KB), so no program stack is
 *      malloc'd at all in this mode.
 *
 *   INLINE (policy 0, marker file "nothread") -- the old behaviour, kept
 *      verbatim as the escape hatch: everything runs on the caller's stack,
 *      which is why the program stack is malloc'd from the firmware heap for
 *      the duration of the run and given back afterwards.
 *
 * In BOTH modes the entry hands ALL callee-saved registers back exactly as the
 * loader left them: the loader's shellcode trusts AAPCS and dies if a longjmp
 * or the interpreter leaves r4-r11/r7 shredded.  (In thread mode it is trivially
 * true -- the interpreter lives on another stack -- but the frame stays, because
 * the inline path needs it and one entry is better than two.)
 *
 * Inline layout (r7 = frame base, parked in plua_a8 because r7 is clobbered):
 *     [a8 .. a8+8)     scratch: { raw malloc pointer, 0 }
 *     [a8+8 .. a8+48)  the ten saved words (r4-r11, ip, lr)
 * ------------------------------------------------------------------------- */
	.globl plua_entry
	.type  plua_entry, %function
plua_entry:
	mov   ip, sp
	bic   sp, sp, #7                /* 8-byte align (LDRD/STRD) */
	stmfd sp!, {r4, r5, r6, r7, r8, r9, r10, r11, ip, lr}
	sub   sp, sp, #8                /* scratch slot for the stack pointer */
	mov   r7, sp                    /* r7 = frame base */
	mov   r4, r0                    /* r4 = config pointer */

	mov   r0, r4
	bl    plua_run_thread_start     /* thread mode: 0 = run it here instead */
	cmp   r0, #0
	beq   .Lplua_inline

	add   sp, sp, #8                /* drop the scratch slot */
	ldmfd sp!, {r4, r5, r6, r7, r8, r9, r10, r11, ip, lr}
	mov   sp, ip                    /* restore the loader's stack pointer */
	bx    lr                        /* r0 = &plua_run: the launcher's handle */

.Lplua_inline:
	mov   r0, r7
	bl    plua_save_a8              /* park the frame base in memory */
	mov   r1, r7                    /* r1 = debug sp (diagnostics) */
	mov   r0, r4
	bl    plua_begin                /* config first: it carries the log path,
	                                 * so a stack failure can be reported */
	mov   r0, r4
	bl    plua_run_inline_begin     /* state=ACTIVE, mode=0, ring_start */
	mov   r0, r7
	bl    plua_stack_alloc          /* r0 = program stack top (0 = keep sp) */
	cmp   r0, #0
	movne sp, r0                    /* switch to the program stack */
	bl    plua_main                 /* run the interpreter */
	mov   r5, r0                    /* keep main()'s result across the teardown */
	mov   r0, r5
	bl    plua_log_ret              /* RT_RET:<n> into the ring */
	mov   r0, r5
	bl    plua_run_finish           /* the launcher reads the exit code here */
	bl    plua_get_a8               /* r0 = frame base (r7 was clobbered) */
	mov   sp, r0                    /* back onto the debug stack frame */
	bl    plua_stack_free           /* hand the program stack back */
	mov   r0, r5                    /* main()'s result */
	add   sp, sp, #8                /* drop the scratch slot */
	ldmfd sp!, {r4, r5, r6, r7, r8, r9, r10, r11, ip, lr}
	mov   sp, ip                    /* restore the loader's stack pointer */
	bx    lr                        /* return with r0 = exit code */

	.size plua_entry, . - plua_entry

/* ---------------------------------------------------------------------------
 * The firmware thread's entry: plua_thread_entry(cfg), started by
 * plua_run_thread_start() with the same r0 the launcher passed to plua_entry.
 *
 * Only two jobs: make sure the stack the firmware gave this thread is 8-byte
 * aligned (the ARM926 faults on an unaligned LDRD/STRD, and one such fault
 * reboots the calculator), and call the C body.  Returning from here ends the
 * thread -- that is what DOOM's doom_main_thread does, so it is known to be
 * the right way to finish one on this machine.
 * ------------------------------------------------------------------------- */
	.globl plua_thread_entry
	.type  plua_thread_entry, %function
plua_thread_entry:
	bic   sp, sp, #7
	push  {r4, lr}                  /* TWO words: AAPCS wants SP 8-aligned at
	                                   every call, and one word would leave it
	                                   at 4 -- the emulator's LDRD/STRD check
	                                   catches exactly the fault the real
	                                   ARM926 takes as a Data Abort */
	bl    plua_thread_main          /* r0 = cfg, straight through */
	pop   {r4, pc}                  /* returning ends the thread */

	.size plua_thread_entry, . - plua_thread_entry

	.end

