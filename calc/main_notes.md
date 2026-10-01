# main.py -- the prose, moved out of the code

`calc/main.py` is the launcher: it loads lua.elf, starts a run
on a firmware thread, streams the program's output, and draws
the program list.  Its comments and docstrings live here, in
the order they appeared, because the calculator compiles a
module's source into RAM (~6 MB for the 156 KB it was) and none
of this prose has to be in there.

Regenerate with `python3 tools/strip_main.py` -- it extracts, it
does not delete: write the prose in main.py as usual, run the
tool, and it moves across.

## `<module>`

    host / no hpprime: stay importable

    Colored console output (moreprint.py, user-provided -- the same import

    primetcc's launcher uses).  When it is missing, everything still runs, just

    without colour, so the launcher is usable in any MicroPython build.

## `decode_bytes`

    MicroPython's bytes.decode may not accept errors=; try, then fall back.

## `<module>`

    ===========================================================================

    firmware debug interface + shellcode ELF loader

    Same mechanism as the DOOM / primetcc ports: a firmware "debug" file takes

    <op, addr, val> records, so MicroPython can write into the calculator's

    address space and call arbitrary addresses.  Everything the interpreter

    needs (loading its image, handing it argv, draining its output ring) goes

    through here.

    ===========================================================================

    firmware API table: entry for service id N sits at BASE + 12*(N-0x10000)

    os_sleep(ms), svc 0x10008 -- the call DOOM's main thread sleeps in.  While a

    Lua program is running on its own thread, this is how the launcher waits

    WITHOUT taking the single CPU away from the program it is streaming.

    wide-char file API (FileManager.hpappdir/fm.py uses the same entries)

## `MemUtils`

    Small helpers over the firmware heap (UTF-16 strings, readback).

## `get_elf_memory_size`

    Total image size = max(p_vaddr + p_memsz) over the PT_LOADs.

## `file_fingerprint`

    (size, checksum) of the ELF on DISK, or None when it cannot be read.

            The image in the firmware heap is identified by what is IN MEMORY (see
            PrimeLua.fingerprints()).  That answers "is this still our image", and
            it says nothing about the file somebody just copied over lua.elf: an
            old image left resident by an earlier session would still pass, and the
            calculator would keep running the PREVIOUS build (still reporting the
            old sys.version()) after a supposedly successful upgrade.

            So the slot record carries this pair as well and a mismatch forces a
            reload.  Three 32-byte samples (start, middle, end) plus the size are
            enough to tell two builds apart, and they are three short reads -- the
            file is 500 KB and reading all of it through uio would cost far more
            than the check is worth.

## `ElfTools`

    seekable uio: 2 = end

## `find_magic_addr`

    Locate an 8-byte magic inside the image and translate the file
            offset to a runtime address: base + p_vaddr + (off - p_offset).

## `<module>`

    The loader shellcode is firmware-agnostic ARM code that mmaps the ELF's

    PT_LOAD segments from a file into a caller-provided buffer, zero-fills the

    .bss tails, applies the R_ARM_RELATIVE relocations and returns the entry

    point.  Identical bytes to the DOOM / primetcc ports (proven on hardware).

    ---------------------------------------------------------------------------

    the firmware input slot, seen from Python

    primeLua's input hook lives INSIDE lua.elf (primetcc's hp_input_svc.c is

    compiled in): the firmware's get_event slot 0x307FBFA0 is patched to a two

    word trap that jumps into the image, and the hook's shared state ("PRIMEIN")

    sits in the image's data as well.

    That makes the image part of the FIRMWARE's input path, so the launcher has

    to put the slot back before it frees the image or overwrites it with a new

    one.  Leaving the trap in place is the worst case: the next key press jumps

    into memory the firmware heap has handed out again, which is a hang or a

    reset, and input stays dead for the rest of the session.

    ---------------------------------------------------------------------------

    ldr pc, [pc, #-4] -- our patch's first word

    struct plua_in_state (primeLua's own, port/plua_input.h), offsets we must

    know.  The C side asserts them at build time, so a drift is a compile error

    there rather than a wrong write into a live input path here.

    the true original slot bytes, 16 of them

    armed (int): the hook's own "a program is listening" flag.  1 makes it

    consume every event the firmware fetches -- including touch presses, which it

    pushes into its 64-item queue.  Nothing drains that queue once the Lua

    program has exited, so the queue fills up and the hook starts dropping

    EVERYTHING, keys included: the calculator answers a key or two, then goes

    deaf the moment a finger touches the screen.

    ... and the policy word the launcher writes: 1 = interception allowed

    ---------------------------------------------------------------------------

    the RUN state (port/plua_run.h): what the entry hands back, and what makes

    streaming output possible.

    The entry starts the interpreter on a firmware thread and RETURNS AT ONCE

    with this structure's address, so the launcher gets control while the program

    is still running.  From then on it is a poll loop: read the ring, print what

    is new, watch the keyboard, and stop when state says the run is over.  That is

    exactly what DOOM's main.py does with puredoom.elf's log structure.

    All words after the magic; the C side asserts nothing here (it is the PRODUCER

    of this layout), so RUN_* and struct plua_run must be changed together.

    ---------------------------------------------------------------------------

    "PLUARUN"

    0 idle, 1 running, 2 finished

    1 = firmware thread, 0 = inline

    the interpreter's status once finished

    launcher -> interpreter: stop now

    launcher -> interpreter: 0 = run inline

    launcher -> interpreter: arm the abort hook

    &plua_ring

    the ring count when this run began

    The firmware's OWN get_event stub, as this machine has it:

    e52d0004  push {r0}

    e52de004  push {lr}

    ef01003f  svc  0x1003F      (the kernel returns to the address on the stack)

    e49d0004  pop  {r0}

    e49df004  pop  {pc}         <- NOT overwritten by our 16-byte patch

    primetcc documents the same five instructions (rt/hp_input_svc.c) and the

    emulator models them word for word (tests/emu_fixes.py), so this is a known

    constant, not a guess.  It is the last resort for a slot left holding OUR trap

    from a session that died before it could restore it: writing the stub back

    returns the machine to a working input path, where NOPs would silently break

    get_event for every app until the next reboot -- and would leave the hook

    itself with a trampoline that returns nothing.

## `ShellcodeElfLoader`

    the interpreter's run state, together with the ring the address the

    entry hands back on every call (port/plua_run.h).  Kept here because

    the NEXT run has to write its policy words BEFORE the entry runs.

    the firmware's own get_event bytes, snapshotted at startup while the

    slot is still intact (see save_hook_snapshot)

    True only once THIS object malloc'd image_raw.  A block that came out

    of lua.slot belongs to an earlier session (or, if the record was

    stale, to the firmware): it may be reused after the fingerprints

    check out, but it must never be freed here -- free() of memory that

    is not ours corrupts the firmware heap, and that is a reset.

    Where the interpreter's input state is, as published in plua.log

    ("in_state=").  Cached because the hook has to be stood down and the

    slot handed back even after the image is gone.  See find_state_addr().

## `forget`

    Drop every address we hold, without freeing anything.

## `ShellcodeElfLoader`

    ------------------------------------------------------- input hook slot

    --------------------------------------------------- the ORIGINAL bytes

## `save_hook_snapshot`

    Snapshot the firmware's own get_event bytes, once per session.

            Why a file: the hook's in-image copy of these bytes (state->saved) is
            lost whenever lua.elf is reloaded, while the SLOT may still hold our
            patch from the previous session.  Without an outside copy of the
            original bytes there is then no way back: the hook refuses to trust its
            own trap, and input stays dead until the calculator is rebooted.  The
            snapshot has to be taken when the slot is intact -- at startup, before
            anything installs the hook.

## `ShellcodeElfLoader`

    NOTE: no bytes.hex() -- MicroPython's bytes has no such method

    (it raised AttributeError here, and because the whole snapshot is

    wrapped in try/except the hook snapshot silently never happened).

## `restore_stale_hook_slot`

    If the slot still holds OUR patch from a previous session, put the
            firmware's bytes back before anything else runs.

            That patch jumps into the image the previous session had loaded; this
            session's image is somewhere else, so leaving it means the next key
            press jumps into memory the firmware heap has handed out again.

## `ShellcodeElfLoader`

    no snapshot: neutralise instead of restoring (see above) -- the

    machine keeps working, primeLua's own keys stay dead until the

    next run installs a fresh hook

    The snapshot itself is a TRAP.  That can only mean it was taken

    while the slot was already patched (an older launcher could do

    that: it wrote hook.bin even when it had just reported "slot holds

    a stale patch at startup").  Writing it back would reinstate the

    very patch this method exists to remove -- a jump into freed

    memory, i.e. a reset on the next key press.  Neutralise instead.

## `neutralize_hook_slot`

    Get a stale hook patch out of the firmware's get_event slot.

            The firmware's get_event slot is called by the OS input thread on every
            event.  If it still holds OUR trap (0xe51ff004 + an address inside an
            image that is gone), the next event jumps into freed memory: the screen
            tears and the calculator resets.  This runs at startup, before the
            script, so the machine is safe even when the true original bytes are
            not available anywhere.

            RESTORE IF WE CAN, NOP ONLY AS A LAST RESORT.  Writing NOPs is safe but
            destroys the firmware's event getter: every app on the calculator that
            polls events (the OS menu, DOOM, primetcc) would then see a stub that
            returns nothing until the next reboot.  So when a snapshot of the
            firmware's own bytes exists -- hook.bin, or this image's own copy -- put
            those back instead.  Only when neither exists do we fall back to NOPs,
            where "input is degraded" still beats "the machine resets".

## `ShellcodeElfLoader`

    No usable snapshot.  Next best thing is the firmware's own stub, which

    is a documented constant (see FW_EVENT_STUB): it restores a WORKING

    input path, and it gives the hook a real trampoline -- the four words

    of get_event it copies.  NOPs would do neither: get_event would return

    nothing for every app on the machine until the next reboot, and the

    hook would run its trampoline straight off the end of the state

    struct, which on this CPU is a reset.

## `write_slot`

    Write the firmware input slot's 16 bytes, TEAR-SAFE.

            The slot is live code: the firmware's input thread fetches it on every
            event, and its first word is either the firmware's own stub or our
            `ldr pc, [pc, #-4]` trap, which jumps to the address held in the SECOND
            word.  The launcher writes through the debug channel, and every 32-bit
            store is its own call -- so the ORDER matters:

              * when the slot currently holds a trap, DISARM IT FIRST (write the
                address word as 0).  Only after that is it safe to rewrite the
                opcode word; doing it the other way leaves a trap pointing at the
                old, now-wrong target for as long as the remaining writes take.
              * when the slot holds the firmware's own stub, write in plain order:
                a stub is not a jump through memory, so a half-written one just
                executes something wrong for one event instead of branching into
                arbitrary memory.  (Writing the address word first here would
                briefly turn the stub's own instructions into a branch, which is
                strictly worse.)

            The whole window is milliseconds of debug-channel calls, not an
            interrupt gap, which is why the launcher needs its own guard on top of
            the C side's interrupt-disabled one.

## `ShellcodeElfLoader`

    disarm before touching the opcode word

## `hook_snap_path`

    Just the name, like lua.slot: MicroPython's FileIO resolves relative
            to the launcher's working directory, which IS the app folder.

## `snapshot_from_file`

    The state's own copy, read from lua.slot (the last-resort record).

## `restore_hook_slot`

    Put the firmware's own get_event bytes back, when the slot still
            points at the hook inside THIS image.  Called before the image is
            freed or overwritten: after that the trap would jump into memory that
            is no longer ours.  Returns True when it restored something.

            Nothing here is trusted blindly: the slot must look like our trap AND
            its hook_fn word must be the address the image reports, and the state
            must carry a saved copy of the original bytes.

## `ShellcodeElfLoader`

    not our patch: leave it alone

    where the state is: published by the interpreter in plua.log

    (a file scan for the magic does NOT work on the device -- see

    find_state_addr)

    PRIM

    some other hook owns the slot

    the image has no usable copy (fresh load, or its own copy is a

    trap): fall back to the snapshot taken at startup

## `find_state_addr`

    Where the input state is -- from plua.log, NOT from a file scan.

            The interpreter publishes the address of its own input state in
            plua.log ("in_state=305217F8") at startup (port/plua_main.c).  That line
            is the ONLY reliable way to find it on the real calculator: the
            alternative -- scanning lua.elf for the PRIMEIN magic through the
            firmware's file API -- FAILS there, and it failed silently, which is how
            a build whose hook was supposed to hand the firmware's input back after
            every run ended up leaving it armed on the device:

                [hook] stand-down: no PRIMEIN in the image
                [hook] NOT disarmed after clock.lua

            Falls back to the scan (it does work in the emulator and on hosts), so
            nothing regresses where the scan is fine.

## `ShellcodeElfLoader`

    plua.log

    the C side caps it at 4 KB

## `stand_down_hook`

    Make the hook inert AND VERIFY IT, then report the slot's state.

            `armed = 0` makes the hook return immediately, so the firmware gets its
            events back instead of the hook swallowing them (and the hook's queue
            stops filling with presses nothing will ever drain).  restore_hook_slot()
            then puts the firmware's own bytes back so the hook is not even called.

            Both writes are READ BACK here.  A device log showed the hook still
            dropping events after a run (unarmed > 0), which means one of these did
            not take -- and "did not take" is exactly the kind of thing that looks
            identical to "worked" without a read-back.

## `ShellcodeElfLoader`

    and what the firmware will fetch on the next event

## `flag`

    Is the marker file `name` present in the app folder?

            THE on-device way to turn a diagnostic switch on.  The calculator's
            MicroPython has no `os` module, so `os.environ[...]` silently never works
            there; a marker file needs no library and cannot be typo'd into the wrong
            process.  `files` is the FileList (it lives on PrimeLua, NOT on this
            loader).

## `apply_touch_hook_setting`

    No-op for this build.

            The 9-25 interpreter keeps ONE contact per frame, which is exactly the
            single-finger behaviour wanted here, and it has no switch word to turn a
            heavier path on or off.  (The multi-touch version added one; that path is
            the one that misbehaved on hardware, and it is deliberately not part of
            this baseline.)

## `load_elf`

    Load lua.elf and return (entry, ring_addr) -- 0,0 on failure.

            Every failure below logs WHY.  "cannot load lua.elf" on its own cost a
            round trip: the message is the caller's, and this function has three
            silent exits (bad ELF header, no heap block, loader refused) that look
            identical from the outside.

## `ShellcodeElfLoader`

    BEFORE the free: the hook entry lives in that block, and the

    firmware slot still points at it

    the firmware heap only guarantees 4-byte alignment, but the

    image holds 8-byte data (doubles, long longs) that the ARM9

    reads with LDRD/STRD -- an unaligned access faults and the

    calculator resets.  Keep the raw pointer for free().

    the run state too (PLUARUN): needed BEFORE the first call, to say

    "run this on a thread and arm the abort hook".  The entry also hands

    the same address back in r0 on every call, which is what the launcher

    trusts if this scan failed.

    leak the block: see the method's comment

    the shellcode buffer is always ours: upload_loader() mallocs it

## `hand_back_input_before_free`

    Take the firmware out of our image BEFORE the image goes away.

            The input hook works by making the OS's own get_event call jump into
            this image.  Freeing (or overwriting) the image while that trap is still
            installed leaves the OS input path pointing at memory the firmware heap
            is about to hand out again: the next key or touch -- and on this machine
            a TOUCH is what the user does first -- branches into garbage, keys and
            touch die, and the calculator resets.  That is the reported failure, so
            this check is not advisory:

              * slot is not our trap      -> nothing to do, safe to free
              * slot is our trap          -> restore the firmware's own bytes first
              * cannot restore it         -> DO NOT free.  Leaking one 500 KB block
                                             until the next reboot is a broken
                                             memory figure; freeing it is a crash.

            Returns True when it is safe to free the block.

## `ShellcodeElfLoader`

    cannot look: assume it is ours

## `<module>`

    ===========================================================================

    launch config: struct plua_config (port/plua_port.h)

    unsigned magic; int argc; char **argv; long epoch; unsigned heap_free;

    The entry receives it in r0 and reads argv from it, so Lua sees exactly the

    command line it would see on a desktop.

    ===========================================================================

    "PLUA"

    durable diagnostics (C side)

    ===========================================================================

    the output ring: {char magic[8]; int count; char data[32768]}

    ===========================================================================

## `RingReader`

    Reader for the ring the interpreter writes its output into.

        The ring is {magic[8]; count; data[32768]}: `count` is the TOTAL number of
        bytes ever written, so the live bytes start at `count % 32768` once the
        buffer has wrapped and at 0 before that.  Getting that start wrong is
        silent: the launcher reads zeros and the script looks like it produced no
        output at all (which is exactly what the first version did -- it used
        `count % size` even for a fresh ring).

## `_chunk`

    n bytes starting at ring offset `start` (handles the wrap).

## `read_since`

    Everything written after ring count `since` (the newest 32 KB of
            it, if more than that has been written).

## `text`

    Ring contents with the RT_RET trailer removed; returns
            (text, exit_code_or_None).

## `read_from`

    (new bytes, where to continue, how many were lost) for a STREAMING
            reader.

            `pos` is a ring COUNT (not an index): the reader keeps the count it has
            already seen and asks for everything after it.  Two things make that
            work while a program is writing:

              * the write and the read are not synchronised, so `pos` may sit
                outside the window the ring still holds -- if more than 32 KB was
                written between two polls, the oldest bytes are simply gone, and
                the reader must skip to the oldest one that survives instead of
                reading a wrapped mix of two eras (that mix is what makes a
                streaming console print garbled text).  The count of lost bytes is
                RETURNED, because a line the reader had only half of is no longer
                a line: whoever is printing must drop its tail (see stream_run);
              * the ring is EMPTY on a fresh image (count 0, nothing written), so
                "read from 0" must return nothing and not 32 KB of zeros.

## `RingReader`

    ...these bytes are overwritten

## `<module>`

    ---------------------------------------------------------------------------

    the run state: what plua_entry hands back (port/plua_run.h)

    ---------------------------------------------------------------------------

    ---------------------------------------------------------------------------

    streaming: how the launcher waits for a running program

    ---------------------------------------------------------------------------

    How long each poll sleeps (in the firmware, on the launcher's thread) and how

    many serial read failures in a row mean the debug channel is gone.

    50 ms, not 25: the launcher and the interpreter are threads of ONE single-core

    machine, and every poll costs the program real time -- four round trips through

    the debug interface plus a key check.  Measured on the device: at 25 ms with a

    key check that called PPL every poll, mandel.lua ran at less than half speed.

    What the interval buys is how fast output appears on screen and how fast the

    interrupt key is noticed; 50 ms is imperceptible for both.

    ...and how long an IDLE poll may be.  A program that is computing rather than

    printing (mandel, fib, sieve) produces nothing for seconds at a time, and

    polling it 20 times a second for no reason is exactly the tax that halved its

    speed.  After a few empty polls the interval grows: the cost of watching a

    compute-heavy script drops to a third, and the only thing that gets slower is

    how quickly the first line of LATER output appears.

    polls without output before slowing down

    The key check is every Nth poll: N * interval = the worst-case delay between

    pressing the key and the interpreter being told to stop.  1 -- every poll --

    because reading the mask is ONE call and the launcher's own menu polls it every

    8 ms; the device measurement (128.8 ms/poll, i.e. the sleeps really sleep) says

    the poll itself is what costs, not this.  Worst case: 50 ms while output flows,

    150 ms while the program is busy computing.

    The launcher's own key ids: bit N of hpprime.keyboard() -- which for these

    keys is also the PPL GETKEY number.  Read them with the bitmask

    (`keyboard() & (1 << id)`), never with PPL while a program is running.

    THE INTERRUPT KEY, in the launcher's own key space: bit N of

    hpprime.keyboard() -- which for these keys is also the PPL GETKEY number.

    ESC = 4        (and 19 = backspace, if you want a second one)

    These are NOT the device scan codes the Lua side sees (ESC is 0x01 there, ON

    is 0x83): the two tables disagree on purpose, and mixing them up is how "the

    right arrow quits the program" happened once already -- see the key-code table

    in the README.

    WHY NOT ON, WHICH IS WHAT THE KEY IS CALLED.  ON is not ours to take: this

    machine's OS uses ON to interrupt the running PYTHON program, so pressing it

    raises KeyboardInterrupt INSIDE THIS LAUNCHER -- which is exactly what was

    reported from the device: a Python traceback on the console, no interrupt

    message, and mandel.lua still drawing, because the script runs on its own

    firmware thread and survived the launcher's death.  ESC is a key the launcher

    can actually read (its own menu uses it), so it is the interrupt.  ON still

    interrupts the script -- see the KeyboardInterrupt handler in stream_run,

    which translates the OS's interrupt into the same abort word.

    Set by stream_run when the OS interrupted us with KeyboardInterrupt (ON).

    Reported once per run, in the same words as the abort key, because from the

    user's side it IS the same thing.  Also exported for the tests.

## `_ticks`

    Milliseconds since power-on, or 0 when the firmware has no tick source
        (a host run).  Only used to report the poll rate: that number is the one
        that says whether the launcher is stealing time from the program it is
        watching.

## `_stream_pause`

    Yield the CPU to the interpreter's thread for `ms` milliseconds.

        The launcher and the running program are threads of one OS on a single-core
        machine, so a Python busy-wait would starve the very program whose output is
        being streamed.  os_sleep (svc 0x10008) is the firmware's own yield, and the
        debug interface can call it the same way it calls malloc.  If that ever
        fails, fall back to the old flat-out pace rather than to no polling at all.

## `_abort_key_down`

    Is one of STREAM_ABORT_KEYS held?  ONE call, and no PPL evaluation.

        hpprime.keyboard() answers with a bitmask of the keys held down right now,
        and reading it is cheap.  PPL's GETKEY is the expensive one (it goes through
        the expression evaluator), and the launcher's own KeyPad class calls it
        whenever no bit is set -- which during a run is every single poll.  That was
        the streaming loop's biggest cost on the device, and this is the fix: the
        interrupt key is looked for in the mask only.

## `RunState`

    The interpreter's own view of the run that is going on right now.

        The entry starts the program on a firmware thread and returns this
        structure's address immediately, which is why the launcher can print output
        WHILE it is produced and can stop a runaway script.  `valid` is False for an
        older interpreter (one that runs on the launcher's own thread and returns
        only when the program is over): everything then falls back to the
        drain-at-the-end behaviour, which is what lua.elf did before 1.0.

    -1 = "not finished yet"

## `configure`

    The launcher's policy for the NEXT run: run it on a thread (so the
            output can be streamed) and arm the keyboard abort.  Both live in the
            run state because the launch config has no room left, and the state is
            found by magic -- no file scan and no extra python-side state.

## `<module>`

    ===========================================================================

    file listing (firmware wide-char find API, same one FileManager uses)

    ===========================================================================

## `list_dir`

    List *.lua in one directory.  The firmware's attribute mask
            filters entries (0x00 matches only attribute-0 files), and no single
            mask sees everything, so every mask is merged -- the same trick
            FileManager.hpappdir/fm.py arrived at.

## `<module>`

    ===========================================================================

    screen

    ===========================================================================

    ===========================================================================

    the launcher: a console program, like primetcc's

    ===========================================================================

    the snapshot of the firmware's get_event bytes (see

    ShellcodeElfLoader.save_hook_snapshot): the only record that survives an image

    reload, and therefore the only way to clear a stale hook patch

    durable diagnostics (C side)

    The launcher's own trail.  It matters exactly when the machine goes down:

    plua.log says how far the interpreter got, this says what the launcher was

    doing.  Written through the firmware file API, which does not buffer, so the

    last line survives a reset.

    Where a loaded interpreter image is recorded so a LATER session (a fresh

    `import main`, or another run of this file) reuses it instead of loading a

    second 500 KB copy into the firmware heap.  primetcc keeps the same kind of

    record in "tcc.slot": MicroPython objects die with the module while the

    image in the firmware heap does not, and without the record the old copy

    becomes unreachable -- a leak of one interpreter per restart.

    "AULP" little-endian

    magic, loader, image_raw, image_base, image_size, entry, ring, fp0, fp1,

    elf_size, elf_sum     (the last two: ElfTools.file_fingerprint of lua.elf)

    Which script `import main` runs.  Edit this, or call main.run_file("x.lua")

    from the terminal -- the same shape as primetcc's embedded-code constant.

    where lua.elf and the scripts may live: the app folder when the firmware

    gives us a usable cwd, otherwise the conventional location

## `PrimeLua`

    One interpreter session: loads lua.elf, runs scripts, reports results.

        No screen of its own -- everything it has to say goes through moreprint
        to the terminal, and the script's own output follows it.

    a copy of the firmware's get_event bytes, taken while the slot is

    still intact (see ShellcodeElfLoader.save_hook_snapshot)

    set when streaming had to stop while the program was still running

    (see stream_run): the teardown then keeps the config alive

    ---------------------------------------------------------------- startup

    BEFORE anything can load an image or install a hook: keep a copy of

    the firmware's own get_event bytes, and clear a patch a previous

    session left behind (that patch points into ITS image, which is gone)

    Interception is ON by default: the mechanism is DOOM's (see

    port/plua_input.c), and DOOM has been reading this machine's keyboard

    and touch through the same patched slot all along.  The marker file

    "noinputhook" turns it off for a session that wants the firmware's

    input path left completely alone.

    A patch left by a session that died (a script that crashed, or an OS

    restart) points into an image that no longer exists: the next key

    press would jump into freed memory -- "the calculator reboots as soon

    as I touch a key".  Clear it FIRST, before anything else can run.

## `set_hook_policy`

    Tell the interpreter whether it may intercept input at all.

            The state word is written here rather than read from a C-side file: the
            launcher owns the policy, and the address is already known (plua.log).
            Returns the value it wrote (0 or 1).

## `find_app_dir`

    First directory that actually holds lua.elf.

## `heap_free`

    Free firmware heap in bytes, or 0 when the firmware will not say.
            PPL's memory(1) is the same figure primetcc's launcher checks.

## `split_ppl_date`

    PPL's Date as (year, month, day).

            On the device this is a FLOAT, yyyy.mmdd -- `eval("Date")` returns
            2026.0925 for 25 Sep 2026 (measured with calc/timeprobe.py).  The
            {year,month,day} list form is kept for builds that answer that way, and
            because it is what this function used to assume: subscripting the float
            raised TypeError, read_epoch() fell into its except branch, and the
            interpreter got epoch = -1, i.e. os.time() == 0 on a machine whose
            clock works perfectly.

## `PrimeLua`

    list form

## `split_ppl_time`

    PPL's Time as (hour, minute, second).

            Also a FLOAT, and in DECIMAL HOURS: 20.5322222222 is 20:31:56
            ((31*60+56)/3600 = 0.532222...).  Reading it as hh.mmss would put the
            clock 22 minutes off.

## `PrimeLua`

    list form

    round(59.6) -> 60

## `read_epoch`

    Wall clock for os.time()/os.date(), from PPL's Date/Time.  -1 tells
            Lua "unknown" (time() then returns 0 rather than a lie).

## `PrimeLua`

    ---------------------------------------------------------------- the log

## `log_tail`

    Diagnostics of the LAST run: the file accumulates (so a crash trail
            survives the next launch) and every run starts with a marker.

## `PrimeLua`

    ---------------------------------------------------------------- running

## `run_script`

    Load lua.elf (once) and run one script.  Returns its exit code.

            `args` are extra words handed to the script after its name, so Lua sees
            them in the global `arg` table (arg[1], arg[2], ...) exactly as it
            would on a desktop: examples/imggesture.lua -d turns its diagnostics
            on, for instance.

## `PrimeLua`

    CRITICAL: MicroPython cannot open "C:\\..." absolute paths,

    but the loader's firmware fopen needs one.  So the file is

    named TWICE: the relative name for every uio.FileIO in here,

    the absolute device path for the C side.

    OUTSIDE the load branch on purpose: once the interpreter image is

    resident (adopted from lua.slot, which is the normal case after the

    first run) load_elf() is never called again, and a switch applied in

    there would never take effect.  The image record is valid by now

    either way, which is all this needs.

    Re-read the clock for EVERY run, not once per session: the

    selector stays up for minutes, and os.time() would otherwise be

    frozen at whatever it was when the menu appeared.

    ignore anything from a past run

    THE RUN'S POLICY, before the entry is called: run it on a firmware

    thread (so this launcher gets control back while the program is

    still running) and arm the keyboard abort.  Both words live in the

    run state, which the interpreter exposes by magic -- see RUN_*.

    "nothread" is the escape hatch for a firmware that dislikes

    the interpreter's thread: everything still works, the output

    is just printed when the program is over (which is how

    primeLua behaved before 1.0).

    ON during the (very short) entry call: the interpreter's own

    thread has already been started or will be; note it here and

    let stream_run's own handler deal with the rest (it re-reads

    the run state, so nothing is lost by the interrupt landing

    exactly here).

    The line that says whether the interpreter's OWN thread got as far

    as starting: if a device reboot happens here, plua.log's last line

    is what it reached (the thread writes "thread: entered" first).

    The entry HANDS BACK the run state (that is what the address in r0

    is for).  Trust it over the load-time scan, and remember it: the

    next run's policy write needs it before the call.

    THE PROGRAM IS STILL RUNNING ON ITS OWN THREAD: print what it

    produces as it produces it (see stream_run).

    ...only this run's output

    LEAK THE CONFIG, DO NOT FREE IT.  Something is still running

    on the interpreter's thread and reading that block; a free()

    under it is heap corruption, and heap corruption on this

    machine is a reset.  A few hundred bytes of launch config is

    the cheap side of that trade (the same rule as "never free

    the image while the firmware's get_event slot points into it").

    HAND THE FIRMWARE'S INPUT BACK.  A script that called

    key.install() left the firmware's get_event slot pointing at the

    hook inside our image, and left that hook ARMED.  The image stays

    resident, so nothing is unsafe yet -- but the hook keeps

    consuming events for a program that has already exited, and it

    pushes every touch press into a 64-item queue that nothing

    drains.  Once that queue is full the hook drops EVERYTHING,

    keys included, which is the reported symptom exactly:

    "after leaving primeLua the keys work ... until I touch the

    screen, and then both keys and touch stop responding"

    So two stores, in this order:

    1. armed = 0  -- stop consuming and stop queueing;

    2. the firmware's original slot bytes back -- stop being

    called at all (the next run's key.install() re-patches it).

    Step 1 first, because it is the one that keeps the queue from

    filling even if step 2 cannot find a usable snapshot.

    The policy word lives in the interpreter's state, and the state

    address is only known after the first run of a session (the

    interpreter publishes it in plua.log).  Refresh it here so the

    NEXT run sees the right value even if the first one could not.

    The firmware heap after the run.  A program that reloads the

    interpreter leaks a whole image (500 KB) and this is where that shows

    up; a heap that keeps dropping is also the one condition that makes

    the OS's own input handling fail later ("keys and touch die, then it

    resets").  One PPL call per run, next to the one taken before it.

    ...unless it was already printed WHILE the program ran, which is

    what streaming does; self.out_lines below is still the full text

    (it comes from the ring), so callers and tests see the same list

    either way.

    "failed=0" is a HEALTHY line (the input path's counters say

    so); only a non-zero failure count is worth a warning

    ------------------------------------------------------------- streaming

## `stream_run`

    Print the program's output AS IT ARRIVES, and let ESC stop it.

            This is what running the program on a firmware thread buys (see
            port/plua_run.c): the entry returned to us while the script is still
            going, so this loop IS the console.  Everything it needs is in the run
            state -- where the ring is, where THIS run's output starts, whether the
            run is over, and the abort word the interpreter's debug hook watches.

            Two rules the loop keeps, both learned from what a console has to do:

              * NOTHING IS PRINTED UNTIL ITS LINE IS COMPLETE.  A program writes in
                pieces (print() is one write, but io.write("a"):write("b") is two),
                so a partial line is held back and completed on the next poll;
              * WHILE A PROGRAM RUNS, NOTHING HERE MAY CALL PPL.  The interrupt key
                is looked for in hpprime.keyboard()'s bitmask and nowhere else
                (_abort_key_down): hpprime.eval("GETKEY") goes through the PPL
                expression evaluator and is an order of magnitude more expensive,
                and this loop runs while the interpreter is trying to use the same
                single CPU.  GETKEY is fine in the launcher's own GUI (KeyPad,
                while nothing is running) -- it is not fine here.
              * The key ids are the launcher-side ones (bit numbers), NOT the device
                scan codes the Lua side sees -- see the key-code table in the README.
                ESC asks the interpreter to stop the program (STREAM_ABORT_KEYS);
                ON arrives as a KeyboardInterrupt from the OS instead.  Either way
                the script unwinds with error("interrupted"), like a
                KeyboardInterrupt, and anything it printed stays on screen.

## `PrimeLua`

    polls since the program last wrote anything

    "ESC" or "ON": which interrupt fired

## `interrupt`

    Ask the interpreter to stop, once per run.

## `PrimeLua`

    THE RING FIRST: its byte count is how new output is noticed,

    and it is one round trip through the debug interface.  A poll

    costs two of those at most (count + data), and the run state

    is read every other poll -- the only thing that delays is

    noticing the END of a silent run, by one poll interval.

    The ring overwrote bytes this reader had not seen.  A line

    it was holding half of can never be completed now, and

    glueing the half to whatever comes next would print two

    different lines as one -- the one kind of corruption a

    console must not produce.  Drop it, start on the next

    whole line.

    A transient read failure is possible while the other

    thread owns the bus; a run of them means the channel

    itself is gone.  Even then the loop does not walk away

    from a LIVE thread: the teardown at the end of

    run_script frees the launch config, and free()ing

    memory a running program is still reading corrupts the

    firmware heap.  So it stops printing, notes it, and

    keeps waiting.

    THE ON KEY, TRANSLATED.  This machine's OS interrupts the

    running PYTHON program with ON -- so the user pressing ON

    arrives here as a KeyboardInterrupt raised inside the

    launcher, NOT as a key in the mask above.  Turning it into the

    interpreter's abort word is what makes ON mean "stop this

    script" in practice: the script runs on its own firmware

    thread, so without this it would just keep going (which is

    exactly what the device showed: a Python traceback, no

    interrupt, mandel.lua still drawing).  A KeyboardInterrupt can

    also land in the middle of a debug-interface call; the abort

    word is a single write to a known address, so it is safe to do

    even then -- what a desynchronised channel would break is the

    READS below, which already have their own failure path.

    ONE pause per poll, and it is where the program gets its CPU

    back: short while output is flowing (so the console keeps up),

    long while nothing is being printed (so a computing script is not

    interrupted twenty times a second for nothing -- the "mandel runs

    at half speed" fix).

    IT IS INSIDE THE try, and that is not tidiness.  This pause is

    where the loop spends almost all of its time (the device measures

    ~129 ms a poll), so it is where the OS's ON interrupt actually

    lands -- with the pause outside the try, ON escaped as an uncaught

    KeyboardInterrupt and killed the launcher while the script, on its

    own thread, kept drawing.  Everything the loop does is inside the

    try for that reason.

    MEASURED, because this number answers the one question the device can

    answer and the emulator cannot: is the launcher's pacing actually

    yielding the CPU to the program?  "0 ms/poll" (or far less than

    STREAM_POLL_MS) means os_sleep through the debug interface is not

    sleeping, and the launcher is spinning against the very thread it is

    streaming -- which halves the script's speed.

    whatever happened above, the run must be OVER before the caller frees

    the config it was started with

## `emit_stream`

    Feed streamed bytes through, printing only COMPLETE lines.

            Returns the incomplete tail, which the caller passes back next time.
            The RT_RET trailer is the interpreter's exit-code channel, not
            something the program printed, so it is never shown.

## `emit`

    Print the interpreter's output through moreprint: its error reports
            in red, everything else as ordinary program output.

## `PrimeLua`

    ------------------------------------------------------- image hand-over

## `_word`

    One 32-bit word of the firmware's memory, or None.

## `fingerprints`

    Two words that only a live interpreter image has: the ELF header it
            was loaded from, and the first instruction at its entry point.  Read
            back at adopt time, they say "this memory still holds OUR image, not
            something the firmware has handed out again".

## `adopt_slot`

    Pick up the interpreter an earlier session left in the firmware
            heap, so this session does not load a second copy of it.  Returns True
            when the image is usable.  Every step here is a guard against running
            into memory that no longer belongs to us: after a reboot, or after the
            firmware heap has been handed out again, the record is stale and the
            image is loaded normally instead.

            NOTHING from the record is committed to the loader until every check has
            passed, and a rejected record is FORGOTTEN rather than half-adopted.
            That half-adoption was a real crash: the old code zeroed entry/
            loader_addr/ring but kept image_raw/image_base/image_size, so
            load_elf() saw "we already have a big enough block" and told the
            shellcode loader to write 300 KB of ELF into a block that may have
            belonged to the firmware by then -- an intermittent reset right after
            the yellow "loading again" warning.

## `PrimeLua`

    the ring's magic is the cheap fingerprint of a live image

    verify with LOCALS only: the loader keeps whatever state it had

    (nothing, in a fresh session) until the record has earned its trust

    NOT ours: forget the record completely -- and do NOT free any of

    it, because the pointers may already belong to the firmware.

    The fingerprints above answer "is this still OUR image".  They do NOT

    answer "is this still a valid image": after a cold boot the firmware

    heap is handed out again, and a stale lua.slot record can match two

    words in memory that now belong to something else entirely -- and

    calling ITS entry point is an instant reset.  So check two more things

    that a reused block must satisfy, cheaply and without side effects:

    the ELF header of the image the record describes, and that the ring

    the launcher will read is still where the record says it is.

    The memory IS our image -- but is it our image of the file that is on

    disk RIGHT NOW?  Somebody may have copied a new lua.elf in since the

    record was written, and adopting the old one would silently keep

    running the previous build.  This block is provably ours (fingerprints,

    ELF header and ring all matched), so it is handed back here rather than

    left behind unreachable -- releasing 500 KB of firmware heap.

    The two allocations are freed directly instead of through

    loader.unload(): that one also restores the firmware's input slot, and

    this session has not taken its snapshot of the original slot bytes yet

    (setup() does that), so it must not go near the slot.

    The old image is provably ours, but the OS may still be jumping

    into it: hand the firmware's input back FIRST (see

    hand_back_input_before_free).  Skipping this is how a

    "just copied a new lua.elf" upgrade turns into a touch-triggered

    reset.

## `write_slot`

    Record where this session put the image, for the next one.

## `PrimeLua`

    ... and which lua.elf it came from, so a replaced file is noticed

    instead of being masked by the resident copy (see adopt_slot()).

## `drop_slot`

    Forget the image record -- the memory it points at is gone.  The
            record is zeroed rather than removed: uio may not offer remove(), and a
            cleared magic invalidates it just as well.

## `<module>`

    ===========================================================================

    console entry points

    ===========================================================================

## `session`

    The live interpreter session, created and loaded on first use.

## `run_file`

    Run one .lua file (a bare name from the app folder, or a full path).

        Extra words after the name go to the script: `run_file("x.lua", args=("-d",))`
        makes it see arg[1] == "-d".

## `run_source`

    Write text to a scratch file and run it -- the closest thing to a REPL
        the console offers (Lua's own stdin prompt needs an input source the
        calculator does not provide).

    relative: uio cannot open C:\... paths

## `run`

    Run a script (default: DEFAULT_SCRIPT).  This is what `import main`
        calls, and it is safe to call again from the terminal.

## `unload`

    Give the interpreter image back to the firmware heap.

## `_release_at_exit`

    Last chance to hand the firmware's input slot back.

        Restoring after every run (see run_script) already covers the normal path,
        because a run always ends there.  This covers the ABNORMAL ones -- the user
        leaves the application from the calculator's own menus, or MicroPython
        tears the module down -- where the interpreter image is about to disappear
        while the firmware's get_event slot may still point into it.  Two things
        have to happen in this order, and both are why this exists:

          1. put the firmware's original bytes back (the image is still here, so
             this is the only moment the restore is still possible);
          2. only then let go of the image.

        Neither may raise: an exception while the runtime is shutting down would
        replace "input is odd" with "the app crashed on the way out".

## `selftest`

    Walk the whole launch chain and report every step, in the terminal.

        The host test suite exercises lua.elf through the emulator, but the
        launcher's own plumbing (debug interface, firmware malloc, shellcode
        loader, ELF file paths) only exists on the calculator.

        Two rules this function learned the hard way (both cost a calculator
        reboot, and neither shows up in the emulator):
          * NEVER write to a hardcoded address.  0x30000000 is where the app's
            memory begins in the emulator, so poking it there is a no-op -- but on
            the real calculator that address belongs to the firmware, and writing
            it resets the machine the moment this test starts.  Every write below
            goes through a block the firmware's malloc() handed us.
          * Use the session's debug handle instead of opening a second one.

    reuse the live session when there is one (the selector and the console

    menu always have one): same debug handle, same resident image

## `check_debug`

    Prove the debug channel carries writes -- through memory the
            firmware gave us, never through a fixed address.

## `probe`

    Run a self-checking Lua script and print its report.

            >>> import main
            >>> main.probe()

        Every feature is wrapped in pcall, so ONE run says exactly which parts of
        the interpreter work on this machine -- much faster than guessing from
        "script X errored".

## `<module>`

    ===========================================================================

    the console menu: pick a program to run, on the calculator's own terminal

    ===========================================================================

    host tests / scripts can switch it off

## `launcher_log`

    Append one line to the launcher's durable log.  Never raises: a
        diagnostic that can break the thing it is diagnosing is worse than none.

    keep the tail only

## `clear_console`

    Wipe the console the way primetcc does: PPL's print() with no argument
        clears the terminal output area, so the menu always starts at the top.

## `read_line`

    One line of console input, or None at end of input (a host run, ^D, or
        a terminal that has gone away).

## `menu_show`

    The list the user picks from.

## `menu_pause`

    Hold the program's output on screen until the user has read it.

## `menu_run`

    Run one program and show its output, colours included.

## `menu`

    The CLI: list the programs, run the one that is picked, come back.

        Everything happens on the calculator's terminal, so leaving the menu is
        just leaving a function -- no screen of our own to close.

    a named script is run first, so `import main` after editing

    DEFAULT_SCRIPT still behaves like "run this program"

    anything after the program name goes to the script: "7 -d" runs

    program 7 with arg[1] == "-d" (this is how a demo's diagnostics are

    turned on without a second copy of the file)

## `menu_run_source`

    `run_source`, but reporting on the console instead of a screen.

## `<module>`

    ===========================================================================

    the selector: a dictionary-style list of programs, on the calculator's screen

    Same drawing conventions as the workspace's other apps (nbprime FileManager /

    Oxford dictionary): everything is drawn into one GROB and presented with a

    single BLIT per frame, text through textout only, key polling through

    hpprime.keyboard() with PPL GETKEY as a fallback that must be released before

    it fires again.

    NOTE the two key-code tables in this project -- Python sees one, Lua the

    other, and mixing them up is how "right arrow quits the program" happens:

    * hpprime.keyboard() bits / PPL GETKEY : up 2, down 12, left 7, right 8,

    enter 30, esc 4, backspace 19        <- what this file polls

    * the firmware's device key ids        : up 3, down 5, left 2, right 4,

    enter 13, esc 1, backspace 12        <- what Lua's key.getkey() returns

    ===========================================================================

    red-ish text on the blue title bar

    a cap for tests; None = until a key

    idle pacing between polls

    the built-in actions, listed after the programs

    MicroPython may have no time module

## `_pace`

    Idle between polls.  Without `time` the loop simply runs flat out (it
        is bounded by key presses anyway).

## `Screen`

    Drawing into GROB 1 and presenting it once per frame.

## `measure`

    Width of one character, measured once per character (TEXTSIZE).

## `KeyPad`

    Edge-triggered key polling FOR THE LAUNCHER'S OWN GUI.

        SCOPE: this class is for the selector and the console waits -- moments when
        NOTHING ELSE is running and the launcher owns the machine.  PPL's GETKEY is
        used there as a fallback (sampled only while no bit is set, and a GETKEY code
        must be seen released before it can fire again, because some firmware repeats
        it forever) and it is affordable there.

        DO NOT USE THIS WHILE A LUA PROGRAM RUNS.  GETKEY goes through the PPL
        expression evaluator and costs far more than hpprime.keyboard(); with the
        interpreter on the same single CPU that is real time taken from the program
        -- measured on the device as mandel.lua at less than half speed.  The
        streaming loop therefore reads the bitmask only (see _abort_key_down).

        hpprime.keyboard() reports the keys held down RIGHT NOW as a bitmask: bit id
        N set means the key with id N is down, so `keyboard() & (1 << id)` is the
        test -- and the ids are the launcher-side ones (K_ESC = 4 ...), not the
        device scan codes Lua sees.

## `poll`

    Keys pressed since the previous poll (PPL codes).

## `KeyPad`

    still the same, un-released key

## `wait`

    Block (on the console) until any key is pressed.  Bounded, so a
            calculator whose key source has gone quiet still returns instead of
            leaving the user with no way out.  The key that got us here is
            absorbed first, so it cannot end the wait instantly.

## `MousePad`

    Touch, through hpprime.mouse() -- the same source the calculator's other
        Python apps use (nbprime FileManager, the Oxford dictionary).

        WHY NOT THE INPUT HOOK.  primeLua's Lua side gets its touch from the
        firmware's get_event slot, which primeLua patches; that hook lives in the
        loaded image and is a great deal of machinery to put in the OS's input path
        for a list of programs.  The launcher's own screen has a much simpler
        source available: hpprime.mouse(), which the firmware answers directly.

            m = hpprime.mouse()   -> ((2, 66), (3, 99))   two fingers
                                  -> ((2, 9), ())         one finger
                                  -> ((), ())             no touch

        One tap = a frame with a finger down after a frame with none (rising edge),
        which is what makes "press and hold" one tap and not a stream of them.
        Coordinates are reported at a different scale from the 320x240 drawing
        surface, so they are converted here (see PLUA_TOUCH_* below).

    The panel's own coordinate space.  hpprime.mouse() reports in it and the

    drawing surface is WIDTH x HEIGHT, so a tap has to be scaled.  Overridable

    because the exact numbers are a property of the firmware; if they are

    wrong the taps land on the wrong row, NOT on the wrong part of the screen,

    which is what makes a wrong value easy to spot and easy to fix.

## `_raw`

    ((x, y) or None, and how many fingers the panel reports).

## `sample`

    (x, y) in screen coordinates WHILE a finger is down, or None.

            No edge semantics at all: this is the raw position, once per frame, for
            the list's drag-to-scroll (ListGesture below).  A finger that goes down
            and stays down reports the same position every frame -- which is what a
            drag needs and what poll() deliberately hides.

## `poll`

    A tap as (x, y) in screen coordinates, or None.  One per touch.

            Rising edge only: a finger that goes down and stays down is one tap,
            and a finger that slid to another row while held is still the same
            gesture, not a new one.  The selector uses sample() instead, because it
            has to tell a tap from a drag; this stays for callers that only want
            "was there a touch?".

## `drain`

    Swallow a touch that was already on the glass when the caller last
            had a chance to look -- i.e. one that is down RIGHT NOW.

            Called once, right after the (multi-second) interpreter load and before
            the first frame is drawn: without it, the frame that draws the list
            would immediately "tap" whatever row happens to be first.  The first
            poll decides everything: a queue that is empty means there is nothing
            stale, so it returns at once and the user's first real tap is safe.
            Only a finger that is genuinely down costs more polling (one per _pace
            until it lifts, bounded by `rounds`).

## `<module>`

    ---------------------------------------------------------------------------

    what the list holds

    ---------------------------------------------------------------------------

## `selector_items`

    The program list, then the built-in actions.

## `selector_layout`

    (header height, first row y, row height, visible rows).

## `selector_draw`

    Dictionary-style page: title bar, then the list.  No footer -- the row
        itself says what it is, and the title bar carries the state.

    a display helper must not be able to kill the UI: anything that

    is not a (text, colour) pair is shown as plain text

    keep the selection inside the window

    the rule between the programs and the actions sits in the GAP

    above the row -- drawn at y + row_h//2 it ran straight through

    the first action's text

    leave room for the size column on the right

    scrollbar on the right edge (not a footer: nothing is written on it)

## `<module>`

    How far a finger may travel and still count as a TAP rather than a DRAG, in

    screen pixels.  A few px of wobble is normal on a resistive panel; the

    calculator's own apps use the same idea.

## `ListGesture`

    Drag-to-scroll and tap-to-open for the list, from hpprime.mouse().

        WHY THIS EXISTS: the list could be tapped but not scrolled -- a finger that
        went down and moved was still read as the tap it started as (MousePad.poll
        is edge-triggered on the PRESS), so a dragging finger opened whatever row it
        happened to start on.  A list longer than the window was therefore unusable
        without the arrow keys.

        WHAT IT DOES: it watches the finger every frame and only decides at RELEASE:

          * moved no more than TOUCH_DRAG_SLOP -> a TAP, reported where it started
            (so tapping a row still opens it, exactly as before);
          * moved further -> a DRAG, which SCROLLS: the movement is converted into
            rows (one row per row_h of finger travel, the sign flipped, so dragging
            the list down reveals earlier entries) and reported as a new selection.
            Scrolling by moving the selection rather than by keeping a separate
            window offset is deliberate: the highlight can never be left off-screen,
            so ENTER, tapping and the arrow keys keep meaning exactly what they
            meant before, and the scrollbar on the right edge follows for free.

        One gesture at a time, and it ends when the finger lifts.

    where the finger went down

    the selection when it went down

## `update`

    One frame.  pos is MousePad.sample()'s answer, sel/row_h/count
            describe the list.  Returns (tap_or_None, sel_or_None).

## `ListGesture`

    a drag: the scroll already happened

    a tap: act where it started

    finger just went down

    still could be either

    finger down -> earlier entries

## `selector_hit`

    Which row a tap at (x, y) landed on, or None.

        Uses the same layout the drawing pass used, so the rows and the touch
        targets can never drift apart.  The title bar and the area past the last
        row are deliberately not tappable -- a tap on the header must not run
        whatever happens to sit under it.

## `selector_activate`

    Run whatever a row means.  Returns (quit, items, sel, top, status).

        Shared by the ENTER key and by a touch tap, so the two input paths cannot
        drift apart: a row does the same thing however it was chosen.

## `selector_status`

    (text, colour) for the title bar after a run.

## `selector_action`

    Run a built-in action.  Everything that is not a Lua program keeps the
        screen; the console is used for programs and for text the user must read.

## `run_log`

    Both trails: the C side's plua.log (how far the interpreter got) and the
        launcher's plua_launcher.log (what the launcher was doing).

    60 lines, not 30: one run logs [selector]/[run]/[stream]/[slot]/[load]/

    [hook] x3/[exit]/[mem], and the window has to reach back to

    "[selector] picked" for the row to answer the question it exists for

    ("what did the launcher do before this run?").

## `run_console`

    Hand the display to the calculator's own console, run something whose
        output is text, and wait for a key before the list comes back.

        Clearing the console is the caller's job (`clear_console`), because that is
        the moment the terminal becomes the visible screen again.

## `selector`

    The screen the calculator shows on `import main`.

        Programs are picked from a dictionary-style list; the built-in console is
        used only to RUN a program (and to show things that must be read), and the
        list comes back afterwards.

    Absorb any touch that was already on the glass while the session was

    being set up.  This has to happen here, BEFORE the first frame is drawn

    and before anything else polls: session() loads lua.elf, which takes

    seconds, so a finger that was down during it is stale and must not be

    read as a tap on whatever row happens to be first.

    Anything down while lua.elf was loading is stale: drop it BEFORE the

    first frame, so the list cannot tap its own first row.  This is where the

    multi-second window actually is -- doing it later would throw away a tap

    the user made at the list.

    TOUCH: one gesture at a time, and it only decides at RELEASE -- a tap

    opens the row it started on, a drag scrolls the list (see

    ListGesture).  The raw position is what a drag needs, so this uses

    sample() rather than the edge-triggered poll().

    the finger is driving; drop stale status

    TOUCH FIRST: tapping a row selects it and runs it, which is what the

    same tap would do in every other app on this calculator.

## `_hex16`

    Lower-case hex of up to 16 bytes, without str.hex().

        MicroPython's `bytes` has no `.hex()`, and the calculator runs MicroPython
        ("bytes object has no attribute hex").  This is the only place the launcher
        wanted it, so a tiny helper beats pulling in ubinascii.

## `_marker_present`

    A marker file `name` in the app folder, or PLUA_<NAME> on a host.

        Standalone (no session, no image): `main()` needs this before it has
        decided whether to build a session at all.  The calculator's MicroPython has
        no `os` module, so on the device only the marker file can work.

        Two ways to look, both failure-safe:
          * uio.FileIO with a BARE RELATIVE name -- the same addressing hook.bin,
            lua.slot and plua.log already use successfully on this firmware;
          * an absolute path built from APP_DIRS, in case the working directory is
            not the app folder.
        Anything else is swallowed: a diagnostic convenience must never be able to
        break the thing it is helping to diagnose.  (An earlier version let a
        uio.FileIO created in a write mode touch the filesystem, and an AttributeError
        inside the session-based check was silently swallowed -- both were bugs of
        exactly that kind.)

## `main`

    What `import main` does: the dictionary-style selector on the
        calculator's screen, and the console only while a program runs.

        MENU_ENABLED = False (or PLUA_NOMENU in the environment) runs
        DEFAULT_SCRIPT once and returns instead -- what the host tests and any
        scripted use of this module want.  Without hpprime (a host run) that is the
        only possible path, and `menu()` offers the same choices on stdin.

    PRINT WHERE THE PREVIOUS RUN DIED, on the terminal, before anything else

    happens.  The launcher log already survives a reset, but on the device it

    takes a menu trip and a scroll to read it -- and when the machine resets

    on "run almost any script" the report that matters is a single line.  This

    puts the tail of that log on screen at startup instead, so the last thing

    the launcher managed to do before the machine went down is simply visible.

    `nomenu` in the app folder, or PLUA_NOMENU on a host (there is no `os`

    module on the calculator -- see PrimeLua.flag for why env vars are the

    host path and marker files are the device path)

## `<module>`

    HP Prime's MicroPython does not run __main__ reliably, so start on import,

    like every other app in this folder.  Dropping the module from sys.modules

    afterwards is what makes a LATER `import main` start a fresh run instead of

    silently returning the cached module (MicroPython only; CPython's import

    machinery re-inserts the module itself, so the pop is skipped there).

    If the runtime ever runs an exit hook, give the interpreter image (and

    with it the firmware's input slot) back cleanly instead of leaving the

    firmware pointing at memory that is about to be reclaimed.  Registered

    only on the device: a host run has no firmware slot to protect, and the

    test suite drives unload() itself.

