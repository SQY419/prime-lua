/*
 * plua_touch.c -- the live contact table and the raw touch-frame ring.
 *
 * See plua_touch.h for why this exists and what a frame looks like.  Nothing
 * here touches hardware or allocates: the input hook runs on the firmware's own
 * thread, inside a call the OS is waiting for, so every function is O(items) or
 * O(contacts^2) with contacts <= 8, and none of them blocks.
 *
 * The frame ring is written by the HOOK (inside the OS input thread) and read by
 * Lua (inside the run loop).  A reader can therefore see a frame that is half
 * written; that is acceptable for a diagnostic -- every frame carries its
 * sequence number, which is written LAST, after the body it describes.
 *
 * IDENTITY: how a frame's items are matched to the contacts already down.
 * The panel identifies a finger only by its position, and the items of a frame
 * arrive in an arbitrary order, so the matching has to be done for the WHOLE
 * frame at once.  Matching item by item does not work, and the failure is easy
 * to reproduce: with fingers at (100,120) and (200,120), a frame carrying their
 * moves to (80,120) and (220,120) has no exact coordinate match at all, so the
 * first item would take a fallback slot and the second would steal the first
 * finger's slot -- leaving the first finger frozen at its old position, which is
 * exactly the "pinch forgets a finger" bug this table exists to prevent.
 *
 * So: nearest previous position wins, one-to-one, and only within
 * PLUA_TOUCH_MATCH px.  A mover that matches nothing is a finger whose press was
 * lost (a full queue can drop one), so it takes a free slot, or the least
 * recently touched one.
 */
#include "plua_touch.h"

#define PLUA_TOUCH_MATCH_SQ 3600          /* 60 px, squared */

static struct plua_contact contacts[PLUA_TOUCH_MAX_CONTACTS];
static struct plua_touch_frame frames[PLUA_TOUCH_RING];
static struct plua_touch_stats stats;
static int ring_head;                     /* next slot to write */
static int ring_used;                     /* slots holding a valid frame */
static unsigned short seq;                /* frames seen this run */
static unsigned long tick;                /* polls, for the staleness clock */
static int polls_since_touch;

void plua_touch_reset(void)
{
	int i;
	for (i = 0; i < PLUA_TOUCH_MAX_CONTACTS; i++) {
		contacts[i].x = 0;
		contacts[i].y = 0;
		contacts[i].phase = 0;
		contacts[i].used = 0;
		contacts[i].stamp = 0;
	}
	for (i = 0; i < PLUA_TOUCH_RING; i++) {
		frames[i].seq = 0;
		frames[i].items = 0;
	}
	stats.frames = 0;
	stats.max_items = 0;
	stats.max_contacts = 0;
	stats.dropped = 0;
	stats.expired = 0;
	stats.seq = 0;
	ring_head = 0;
	ring_used = 0;
	seq = 0;
	tick = 0;
	polls_since_touch = 0;
}

/* oldest first, so a log reads like a story */
static struct plua_touch_frame *ring_slot(int i)
{
	int at = ring_head - ring_used + i;
	while (at < 0)
		at += PLUA_TOUCH_RING;
	return &frames[at % PLUA_TOUCH_RING];
}

static void live_count(void)
{
	int i, n = 0;
	for (i = 0; i < PLUA_TOUCH_MAX_CONTACTS; i++)
		if (contacts[i].used)
			n++;
	if ((unsigned)n > stats.max_contacts)
		stats.max_contacts = (unsigned)n;
}

static int free_slot(void)
{
	int i;
	for (i = 0; i < PLUA_TOUCH_MAX_CONTACTS; i++)
		if (!contacts[i].used)
			return i;
	return -1;
}

/* the contact that has gone longest without a frame, for a mover that matches
 * nothing and finds no free slot */
static int oldest_slot(void)
{
	int i, best = 0;
	for (i = 0; i < PLUA_TOUCH_MAX_CONTACTS; i++) {
		if (!contacts[i].used)
			return i;
		if (contacts[i].stamp < contacts[best].stamp)
			best = i;
	}
	return best;
}

void plua_touch_feed(int n, plua_touch_item_fn action, plua_touch_valid_fn valid,
                     plua_touch_xy_fn xy, void *ctx)
{
	struct plua_touch_frame *f;
	int i, k, j, first = 1;
	int px[PLUA_TOUCH_MAX_ITEMS], py[PLUA_TOUCH_MAX_ITEMS];
	int act[PLUA_TOUCH_MAX_ITEMS];
	int mover[PLUA_TOUCH_MAX_ITEMS];
	int match[PLUA_TOUCH_MAX_CONTACTS];
	int take[PLUA_TOUCH_MAX_CONTACTS];

	if (n < 0)
		n = 0;
	if (n > PLUA_TOUCH_MAX_ITEMS)
		n = PLUA_TOUCH_MAX_ITEMS;

	stats.frames++;
	seq++;
	polls_since_touch = 0;
	tick++;

	/* ---- one: describe the frame, and publish its body ---------------- */
	f = &frames[ring_head];
	f->seq = 0;                            /* unpublished while filling */
	f->items = (unsigned short)n;
	f->live = 0;
	f->touch = 0;
	f->touchy = 0;
	f->x1 = 0;
	f->y1 = 0;
	for (i = 0; i < PLUA_TOUCH_MAX_ITEMS; i++) {
		f->action[i] = 0;
		f->valid[i] = 1;
		act[i] = 0;
		mover[i] = 0;
		px[i] = 0;
		py[i] = 0;
	}
	for (i = 0; i < n; i++) {
		int a = action(ctx, i);
		int ok = (valid(ctx, i) == 0);
		int x = 0, y = 0;

		f->action[i] = (unsigned char)(a < 0 ? 0 : a);
		f->valid[i] = (unsigned char)(ok ? 0 : 1);
		if (!ok)
			continue;
		if (!xy(ctx, i, &x, &y))
			continue;
		act[i] = a;
		px[i] = x;
		py[i] = y;
		f->live++;
		if (first) {
			f->touch = (unsigned short)x;
			f->touchy = (unsigned short)y;
			first = 0;
		} else if (f->live == 2) {
			f->x1 = (unsigned short)x;
			f->y1 = (unsigned short)y;
		}
	}

	/* ---- two: match the whole frame to the contacts already down ------ */
	for (i = 0; i < PLUA_TOUCH_MAX_CONTACTS; i++) {
		match[i] = -1;
		take[i] = 0;
	}
	for (;;) {
		int bi = -1, bk = -1, bd = PLUA_TOUCH_MATCH_SQ;
		for (i = 0; i < n; i++) {
			if (act[i] != 2 || mover[i])
				continue;
			for (k = 0; k < PLUA_TOUCH_MAX_CONTACTS; k++) {
				int dx, dy, d;
				if (!contacts[k].used || take[k])
					continue;
				dx = px[i] - (int)contacts[k].x;
				dy = py[i] - (int)contacts[k].y;
				d = dx * dx + dy * dy;
				if (d < bd) {
					bd = d;
					bi = i;
					bk = k;
				}
			}
		}
		if (bi < 0)
			break;
		mover[bi] = 1;
		take[bk] = 1;
		match[bk] = bi;
	}

	/* ---- three: apply, presses first (they claim free slots) ---------- */
	for (i = 0; i < n; i++) {
		int slot;
		if (act[i] != 1)
			continue;
		slot = free_slot();
		if (slot < 0) {
			slot = oldest_slot();          /* evict, and say so */
			stats.dropped++;
		}
		contacts[slot].x = (unsigned short)px[i];
		contacts[slot].y = (unsigned short)py[i];
		contacts[slot].phase = 1;              /* landed this frame */
		contacts[slot].used = 1;
		contacts[slot].stamp = tick;
	}
	for (k = 0; k < PLUA_TOUCH_MAX_CONTACTS; k++) {
		if (match[k] < 0)
			continue;
		i = match[k];
		contacts[k].x = (unsigned short)px[i];
		contacts[k].y = (unsigned short)py[i];
		contacts[k].phase = 2;                 /* moving */
		contacts[k].used = 1;
		contacts[k].stamp = tick;
	}
	/* movers with no matching contact: a press the queue dropped */
	for (i = 0; i < n; i++) {
		int slot;
		if (act[i] != 2 || mover[i])
			continue;
		slot = free_slot();
		if (slot < 0) {
			slot = oldest_slot();
			stats.dropped++;
		}
		contacts[slot].x = (unsigned short)px[i];
		contacts[slot].y = (unsigned short)py[i];
		contacts[slot].phase = 2;
		contacts[slot].used = 1;
		contacts[slot].stamp = tick;
	}

	/* ---- four: releases, one-to-one, nearest position wins ------------ */
	for (i = 0; i < n; i++) {
		int bk = -1, bd = PLUA_TOUCH_MATCH_SQ;
		if (act[i] != 8)
			continue;
		for (k = 0; k < PLUA_TOUCH_MAX_CONTACTS; k++) {
			int dx, dy, d;
			if (!contacts[k].used)
				continue;
			dx = px[i] - (int)contacts[k].x;
			dy = py[i] - (int)contacts[k].y;
			d = dx * dx + dy * dy;
			if (d < bd) {
				bd = d;
				bk = k;
			}
		}
		if (bk < 0) {
			/* the panel moved the finger before the release: the
			 * least recently touched contact is the best guess */
			for (k = 0; k < PLUA_TOUCH_MAX_CONTACTS; k++) {
				if (!contacts[k].used)
					continue;
				if (bk < 0 || contacts[k].stamp < contacts[bk].stamp)
					bk = k;
			}
			/* still nothing? a release with no finger down */
		}
		if (bk >= 0)
			contacts[bk].used = 0;
	}

	if ((unsigned)f->live > stats.max_items)
		stats.max_items = (unsigned)f->live;
	f->seq = seq;                              /* publish */

	ring_head = (ring_head + 1) % PLUA_TOUCH_RING;
	if (ring_used < PLUA_TOUCH_RING)
		ring_used++;
	stats.seq = seq;

	live_count();
}

void plua_touch_tick(void)
{
	int i;
	tick++;
	if (++polls_since_touch < PLUA_TOUCH_EXPIRE_POLLS)
		return;
	polls_since_touch = 0;
	for (i = 0; i < PLUA_TOUCH_MAX_CONTACTS; i++) {
		if (contacts[i].used) {
			contacts[i].used = 0;
			stats.expired++;
		}
	}
}

int plua_touch_count(void)
{
	int i, n = 0;
	for (i = 0; i < PLUA_TOUCH_MAX_CONTACTS; i++)
		if (contacts[i].used)
			n++;
	return n;
}

const struct plua_contact *plua_touch_at(int i)
{
	int k, n = 0;
	if (i < 0)
		return 0;
	for (k = 0; k < PLUA_TOUCH_MAX_CONTACTS; k++) {
		if (!contacts[k].used)
			continue;
		if (n == i)
			return &contacts[k];
		n++;
	}
	return 0;
}

const struct plua_touch_frame *plua_touch_frame_at(int i)
{
	if (i < 0 || i >= ring_used)
		return 0;
	return ring_slot(i);
}

int plua_touch_frames(void)
{
	return ring_used;
}

void plua_touch_frames_clear(void)
{
	int i;
	for (i = 0; i < PLUA_TOUCH_RING; i++)
		frames[i].seq = 0;
	ring_head = 0;
	ring_used = 0;
}

const struct plua_touch_stats *plua_touch_stats(void)
{
	return &stats;
}
