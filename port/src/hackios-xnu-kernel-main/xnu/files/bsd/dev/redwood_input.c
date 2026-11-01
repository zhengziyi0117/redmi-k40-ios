/*
 * /dev/redwood-touch: a queue of touch samples (see sys/redwood_input.h).
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
#include <libkern/libkern.h>
#include <miscfs/devfs/devfs.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <pexpert/pexpert.h>

#define TOUCH_QUEUE     512             /* records; a power of two */

/*
 * Boot-arg touch_lat=1: for measuring how long a touch takes to reach the panel, a line for every
 * sample posted ("tl: p TYPE FINGER X Y at US", the time in microseconds of mach_absolute_time),
 * to be matched with the window server's lines about reading it (userland/src/inject.c), drawing
 * (eagl.c) and the display driver's about the frame (MiniIOMobileFramebuffer.cpp): see
 * tools/latency.py.
 */
static uint32_t touch_lat = 2;          /* 2: not yet looked up */

extern void loader_log_line(const char *line);
/*
 * A "tl:" line, for the kernel's drivers: into the loader's log as it is written -- the kernel's own
 * log keeps only the last of a run, and a live look at the loader's (XL_FOLLOW) then misses nothing.
 */
void redwood_tl(const char *format, ...) __printflike(1, 2);

void
redwood_tl(const char *format, ...)
{
	char line[192];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	loader_log_line(line);
}

static bool
latency_lines(void)
{
	if (touch_lat == 2) {
		uint32_t on = 0;

		touch_lat = PE_parse_boot_argn("touch_lat", &on, sizeof(on)) && on != 0;
	}
	return touch_lat != 0;
}

static LCK_GRP_DECLARE(touch_lck_grp, "redwood-touch");
static LCK_MTX_DECLARE(touch_lock, &touch_lck_grp);
static struct redwood_touch touch_queue[TOUCH_QUEUE];
static uint32_t touch_head, touch_tail;        /* read from head, add at tail */

/*
 * The way out when nothing else is at hand: five fingers held on the screen for a second and a
 * half end the run -- control goes back to the loader, as when loader_return_after expires.
 * Only a driver's fingers count, not samples a program writes to the device.
 */
#define EXIT_FINGERS    5
#define EXIT_HOLD_MS    1500

extern void loader_return(void);
extern void redwood_cpufreq_touch(void);        /* redwood_cpufreq.c: the processor at full speed while a finger is down */
extern void alioth_wlan_wifi_init(void);
extern void alioth_pcie_init(void);     /* alioth_pcie.c: the Wi-Fi's PCIe controller, with pcie= in the boot-args */

static thread_call_t exit_call;
static uint32_t fingers_down;           /* one bit for each finger a driver says is down */

static void
exit_hold_elapsed(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	printf("\n--- %d fingers held on the screen for %d ms: back to the loader ---\n", EXIT_FINGERS, EXIT_HOLD_MS);
	redwood_adreno_stop();
	loader_return();
}

/* With touch_lock held. */
static void
count_fingers(uint32_t type, uint32_t finger)
{
	int before = __builtin_popcount(fingers_down), now;
	uint64_t deadline;

	if (finger >= 32 || exit_call == NULL || (type != REDWOOD_TOUCH_DOWN && type != REDWOOD_TOUCH_UP)) {
		return;
	}
	if (type == REDWOOD_TOUCH_DOWN) {
		fingers_down |= 1u << finger;
	} else {
		fingers_down &= ~(1u << finger);
	}
	now = __builtin_popcount(fingers_down);
	if (now >= EXIT_FINGERS && before < EXIT_FINGERS) {
		clock_interval_to_deadline(EXIT_HOLD_MS, NSEC_PER_MSEC, &deadline);
		thread_call_enter_delayed(exit_call, deadline);
	} else if (now < EXIT_FINGERS && before >= EXIT_FINGERS) {
		thread_call_cancel(exit_call);
	}
}

static void
touch_post(uint32_t type, uint32_t finger, uint32_t x, uint32_t y, bool from_driver)
{
	struct redwood_touch *t;

	lck_mtx_lock(&touch_lock);
	if (touch_tail - touch_head == TOUCH_QUEUE) {
		touch_head++;                   /* nobody reads: drop the oldest */
	}
	t = &touch_queue[touch_tail++ % TOUCH_QUEUE];
	t->time = mach_absolute_time();
	if (latency_lines()) {
		uint64_t ns;

		absolutetime_to_nanoseconds(t->time, &ns);
		redwood_tl("tl: p %u %u %u %u at %llu\n", type, finger, x, y, ns / 1000);
	}
	t->type = type;
	t->finger = finger;
	t->x = x > 65535 ? 65535 : x;
	t->y = y > 65535 ? 65535 : y;
	if (from_driver) {
		count_fingers(type, finger);
	}
	lck_mtx_unlock(&touch_lock);
	wakeup(&touch_queue);
	if (from_driver) {
		redwood_cpufreq_touch();
	}
}

void
redwood_touch_post(uint32_t type, uint32_t finger, uint32_t x, uint32_t y)
{
	touch_post(type, finger, x, y, true);
}

static int
touch_open(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	return 0;
}

static int
touch_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	return 0;
}

static int
touch_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	struct redwood_touch t;
	int error = 0, moved = 0;

	lck_mtx_lock(&touch_lock);
	while (touch_head == touch_tail) {
		error = msleep(&touch_queue, &touch_lock, PZERO | PCATCH, "redwood-touch", NULL);
		if (error) {
			lck_mtx_unlock(&touch_lock);
			return error;
		}
	}
	while (touch_head != touch_tail && uio_resid(uio) >= (user_ssize_t)sizeof(t)) {
		t = touch_queue[touch_head++ % TOUCH_QUEUE];
		lck_mtx_unlock(&touch_lock);
		error = uiomove((caddr_t)&t, sizeof(t), uio);
		lck_mtx_lock(&touch_lock);
		if (error) {
			break;
		}
		moved++;
	}
	lck_mtx_unlock(&touch_lock);
	return moved ? 0 : error;
}

static int
touch_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	struct redwood_touch t;
	int error = 0;

	while (uio_resid(uio) >= (user_ssize_t)sizeof(t)) {
		error = uiomove((caddr_t)&t, sizeof(t), uio);
		if (error) {
			break;
		}
		if (t.type < REDWOOD_TOUCH_DOWN || t.type > REDWOOD_BUTTON_UP) {
			return EINVAL;
		}
		touch_post(t.type, t.finger, t.x, t.y, false);
	}
	return error;
}

static const struct cdevsw touch_cdevsw = {
	.d_open = touch_open, .d_close = touch_close, .d_read = touch_read, .d_write = touch_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

/*
 * Boot-arg touch_demo=1: a tap every so many seconds, for when the panel itself answers nobody.
 * Everything above the chip -- the queue, backboardd's reader, the window server -- takes a sample
 * posted here exactly as it takes one read off the chip, so a tap that opens an app says that all of
 * that works and that what is missing is the chip's side of it. It is also how a port is tried on a
 * phone whose touch controller is not wired up yet.
 *
 * touch_demo_x / touch_demo_y: where, 0 to 65535 across and down (the middle by default).
 * touch_demo_ms: how often (5000 by default); touch_demo_pressed_ms: how long the finger stays down
 * (80); touch_demo_after: how many seconds to wait before the first, so that SpringBoard is up and
 * reading (20).
 */
static void
demo_swipe(uint32_t x)
{
	const uint32_t from = 64500, to = 23000, steps = 20;
	uint32_t at;

	redwood_touch_post(REDWOOD_TOUCH_DOWN, 0, x, from);
	for (uint32_t i = 1; i <= steps; i++) {
		at = from - (from - to) * i / steps;
		redwood_touch_post(REDWOOD_TOUCH_MOVE, 0, x, at);
		delay_for_interval(12, NSEC_PER_MSEC);
	}
	redwood_touch_post(REDWOOD_TOUCH_UP, 0, x, to);
}

static void
touch_demo_thread(void *arg __unused, wait_result_t result __unused)
{
	uint32_t x = 32768, y = 32768, period_ms = 5000, after_s = 20, pressed_ms = 80, mode = 1, lock_at = 0;

	PE_parse_boot_argn("touch_demo", &mode, sizeof(mode));
	PE_parse_boot_argn("touch_demo_x", &x, sizeof(x));
	PE_parse_boot_argn("touch_demo_y", &y, sizeof(y));
	PE_parse_boot_argn("touch_demo_ms", &period_ms, sizeof(period_ms));
	PE_parse_boot_argn("touch_demo_after", &after_s, sizeof(after_s));
	PE_parse_boot_argn("touch_demo_pressed_ms", &pressed_ms, sizeof(pressed_ms));
	PE_parse_boot_argn("demo_lock", &lock_at, sizeof(lock_at));
	if (period_ms < 200) {
		period_ms = 200;
	}
	delay_for_interval(after_s, NSEC_PER_SEC);
	for (unsigned n = 1;; n++) {
		if (lock_at != 0 && n == lock_at) {
			/* a control for the taps: the side button goes through the same queue and the same reader, so
			 * if the screen locks, everything from the queue outwards works and only the touch events
			 * themselves are being dropped. */
			redwood_touch_post(REDWOOD_BUTTON_DOWN, REDWOOD_BUTTON_LOCK, 0, 0);
			delay_for_interval(100, NSEC_PER_MSEC);
			redwood_touch_post(REDWOOD_BUTTON_UP, REDWOOD_BUTTON_LOCK, 0, 0);
			redwood_tl("demo: the side button pressed and released\n");
		}
		if (mode == 2) {
			/* touch_demo=2: a sweep down the screen, a tenth of it a step, the same column: which of
			 * the taps lands on something says how the position is being read (a wrong axis, or a
			 * wrong direction, still hits a row somewhere -- a wrong scale alone would not). */
			y = 13107 + ((n - 1) % 7) * 6554;        /* 0.20, 0.30 ... 0.80 of the height */
		}
		if (mode == 0 || mode == 6) {
			/* only the side button was asked for (0), or the button and the unlock (6): leave the
			 * screen to whoever is at it */
			delay_for_interval(period_ms, NSEC_PER_MSEC);
			continue;
		}
		if (mode == 3) {
			/* touch_demo=3: a swipe up from the bottom edge, the home gesture -- the one thing a locked
			 * screen answers with something unmistakable, and it is also the way back to the home screen
			 * from whichever application a tap may have opened. */
			demo_swipe(x);
			redwood_tl("demo: swipe %u up from the bottom edge\n", n);
			delay_for_interval(period_ms, NSEC_PER_MSEC);
			continue;
		}
		if ((mode == 5 || mode == 6) && n > lock_at && n <= lock_at + 3) {
			/* touch_demo=5: wake with the side button (the screen is off until something wakes it),
			 * unlock with the home gesture, and only then tap: the whole way from a dark screen to an
			 * application, with nothing but samples posted here. Three swipes, because the first one
			 * after the wake is sometimes taken for the wake itself. */
			delay_for_interval(1500, NSEC_PER_MSEC);
			demo_swipe(x);
			redwood_tl("demo: swipe up to unlock (%u of 3)\n", n - lock_at);
			delay_for_interval(period_ms, NSEC_PER_MSEC);
			continue;
		}
		redwood_touch_post(REDWOOD_TOUCH_DOWN, 0, x, y);
		/* A finger that lands and lifts without ever moving does not read as a tap to the window
		 * server, while a swipe does: a tap is sent with the small movement a real finger has. */
		delay_for_interval(pressed_ms / 3, NSEC_PER_MSEC);
		redwood_touch_post(REDWOOD_TOUCH_MOVE, 0, x, y + 2);
		delay_for_interval(pressed_ms / 3, NSEC_PER_MSEC);
		redwood_touch_post(REDWOOD_TOUCH_MOVE, 0, x, y);
		delay_for_interval(pressed_ms / 3, NSEC_PER_MSEC);
		redwood_touch_post(REDWOOD_TOUCH_UP, 0, x, y);
		redwood_tl("demo: tap %u at %u, %u\n", n, x, y);
		delay_for_interval(period_ms, NSEC_PER_MSEC);
	}
}

void
redwood_input_demo_init(void)
{
	uint32_t on = 0, lock_at = 0;
	thread_t thread;

	/* touch_demo=1..5 posts samples; demo_lock=N alone only presses the side button (which is what
	 * turns the display, and with it the digitizer, on at boot), leaving the screen to whoever is
	 * looking at it. */
	if (!PE_parse_boot_argn("touch_demo", &on, sizeof(on))) {
		on = 0;
	}
	if (!PE_parse_boot_argn("demo_lock", &lock_at, sizeof(lock_at))) {
		lock_at = 0;
	}
	if (on == 0 && lock_at == 0) {
		return;
	}
	if (kernel_thread_start(touch_demo_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

void
redwood_input_init(void)
{
	int major = cdevsw_add(-1, &touch_cdevsw);

	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "redwood-touch") == NULL) {
		printf("redwood-touch: cannot create the device\n");
	}
	exit_call = thread_call_allocate(exit_hold_elapsed, NULL);
	redwood_cpufreq_init();
	redwood_virtio_input_init();
	redwood_goodix_init();
	redwood_input_demo_init();
	redwood_buttons_init();
	redwood_battery_init();
	redwood_ufs_init();
	redwood_net_init();
	redwood_adreno_init();
	redwood_wifi_init();
	alioth_pcie_init();
	alioth_wlan_wifi_init();
	redwood_adsp_init();
}
