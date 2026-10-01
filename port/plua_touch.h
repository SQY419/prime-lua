/*
 * plua_touch.h -- what the firmware's touch frames actually contain.
 *
 * WHY THIS EXISTS
 * primetcc's input hook (rt/hp_input_svc.c) turns every touch frame into ONE
 * queued item: it writes *(u16 *)(ev + 24) = 1, i.e. it claims the frame has a
 * single contact, and it drops action 2 (move) items because its own widgets
 * only need press and release.  For a widget toolkit that is right.  For a
 * two-finger pinch it means Lua can never see a second finger, and never sees
 * a finger move at all -- the count is manufactured, not reported.
 *
 * primeLua therefore does three things of its own here:
 *
 *   1. it keeps the count and every item of the frame, so key.event() /
 *      key.gettouches() report what the panel really sent;
 *   2. it maintains a LIVE CONTACT TABLE: press adds a finger, move updates it,
 *      release removes it.  That is what a gesture wants (current position of
 *      each finger), and it does not flood the event queue with the ~100 moves
 *      per second a drag generates;
 *   3. it records the last few raw frames (item count, actions, coordinates,
 *      validity) in a fixed-size ring, so a script can answer the question this
 *      whole feature depends on -- does the panel report more than one contact
 *      at all? -- without a serial cable, and without doing file I/O inside the
 *      input hook (which runs on the firmware's own thread and must stay
 *      bounded and non-blocking).
 *
 * Layout of a touch event, measured on the device and kept in primetcc's
 * hp_input.h (primeLua compiles that header):
 *     ev + 4                u32   event type: 15 = touch
 *     ev + 24               u16   item count (the firmware's own field)
 *     ev + 28 + 12*i        item i:
 *         +0  u32  action   1 = press, 2 = move, 8 = release
 *         +4  u16  valid    0 = the item is live, non-zero = padding
 *         +6  u16  x
 *         +8  u16  y
 */
#ifndef PLUA_TOUCH_H
#define PLUA_TOUCH_H

#define PLUA_TOUCH_MAX_CONTACTS 8      /* the firmware's own limit */
#define PLUA_TOUCH_MAX_ITEMS    8      /* items it may pack into one frame */
#define PLUA_TOUCH_RING         16     /* raw frames remembered for the log */

/* one frame, as the panel sent it (before anybody "helpfully" rewrote it) */
struct plua_touch_frame {
	unsigned short seq;                    /* 1-based counter, 0 = unused */
	unsigned short items;                  /* raw item count */
	unsigned short live;                   /* items with valid == 0 */
	unsigned short touch;                  /* item 0: x */
	unsigned short touchy;                 /* item 0: y */
	unsigned short x1, y1;                 /* item 1, when the frame has one */
	unsigned char action[PLUA_TOUCH_MAX_ITEMS];   /* per item */
	unsigned char valid[PLUA_TOUCH_MAX_ITEMS];
};

/* one live finger.  Identity is positional: the panel reports no contact id,
 * so a contact is "the finger that was here" (see the matching notes in
 * plua_touch.c), and stamp is the poll counter of its last frame -- it is what
 * "least recently touched" means. */
struct plua_contact {
	unsigned short x, y;
	unsigned char phase;                   /* 1 = landed this frame, 2 = moving */
	unsigned char used;
	unsigned long stamp;
};

/* what key.touchstats() reports */
struct plua_touch_stats {
	unsigned frames;                       /* touch frames seen this run */
	unsigned max_items;                    /* most items in any ONE frame */
	unsigned max_contacts;                 /* most live contacts at once */
	unsigned dropped;                      /* contacts evicted by the cap */
	unsigned expired;                      /* contacts expired by staleness */
	unsigned seq;                          /* frames, as the ring counts them */
};

void plua_touch_reset(void);

/* Feed ONE raw touch frame.  n is the firmware's item count; plua_item()
 * returns item i's action (0 when i is out of range), plua_valid() its valid
 * field, plua_xy() its coordinates. */
typedef int (*plua_touch_item_fn)(void *ctx, int i);
typedef int (*plua_touch_valid_fn)(void *ctx, int i);
typedef int (*plua_touch_xy_fn)(void *ctx, int i, int *x, int *y);
void plua_touch_feed(int n, plua_touch_item_fn action, plua_touch_valid_fn valid,
                     plua_touch_xy_fn xy, void *ctx);

/* Age the contact table.  One poll of the input queue = roughly 5 ms, so a
 * finger that has sent neither a move nor a press for PLUA_TOUCH_EXPIRE_MS is
 * gone: without this, a release lost to a full queue would leave a phantom
 * finger that breaks every later gesture. */
void plua_touch_tick(void);
#define PLUA_TOUCH_EXPIRE_POLLS 200       /* ~1 s at 5 ms per poll */

/* the live contact table, compacted (no holes), newest press last */
int plua_touch_count(void);
const struct plua_contact *plua_touch_at(int i);

/* the raw-frame ring, oldest first; NULL when there is no such frame yet */
const struct plua_touch_frame *plua_touch_frame_at(int i);
int plua_touch_frames(void);              /* how many the ring holds now */
void plua_touch_frames_clear(void);

const struct plua_touch_stats *plua_touch_stats(void);

#endif /* PLUA_TOUCH_H */
