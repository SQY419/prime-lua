/*
 * plua_fwmode.c -- C file modes translated to what the firmware accepts.
 *
 * Kept in its own translation unit so the host test can compile it alone:
 * plua_file.c pulls in the firmware SVCs and clashes with the host's stdio.
 */
#include "plua_port.h"

/*
 * The firmware's fopen accepts a NARROW set of mode strings: "rb" to read and
 * "wb+" to write.  primetcc's diagnostics write files this way and record that
 * append ("ab") is not reliable; Lua and the C library, on the other hand, pass
 * the usual C modes ("w", "r", "a", "w+", ...).
 *
 * Passing those through unchanged works in the emulator (whose file model
 * accepts anything) and FAILS ON THE DEVICE: `io.open(name, "w")` returned nil,
 * which is why a diagnostic script's log file never appeared on the calculator.
 *
 * So the modes are translated here:
 *   r / rb            -> "rb"      (read, no truncation)
 *   w / wb / w+ / ... -> "wb+"     (create, truncate)
 *   a / ab / a+ / ... -> "wb+"     after copying the old contents through, so
 *                                   the stream starts where the data ends
 *   r+ / rb+          -> "wb+"     after copying the old contents through
 *
 * The "copy through" cases are the read-append-rewrite cycle plua_diag.c uses.
 */
int plua_fw_mode(const char *mode, char *out, int cap)
{
	int read = 0, write = 0, append = 0, plus = 0, i;
	if (!mode || !out || cap < 4)
		return -1;
	for (i = 0; mode[i]; i++) {
		char c = mode[i];
		if (c == 'r')
			read = 1;
		else if (c == 'w')
			write = 1;
		else if (c == 'a')
			append = 1;
		else if (c == '+')
			plus = 1;
		else if (c != 'b' && c != 't')
			break;		/* anything else: stop reading */
	}
	if (append) {
		out[0] = 'w'; out[1] = 'b'; out[2] = '+'; out[3] = 0;
		return 2;		/* keep the contents, start at the end */
	}
	if (write) {
		out[0] = 'w'; out[1] = 'b'; out[2] = '+'; out[3] = 0;
		return 0;		/* create + truncate */
	}
	if (read && plus) {
		out[0] = 'w'; out[1] = 'b'; out[2] = '+'; out[3] = 0;
		return 1;		/* update in place: keep, start at 0 */
	}
	out[0] = 'r'; out[1] = 'b'; out[2] = 0;
	return 0;			/* read */
}

