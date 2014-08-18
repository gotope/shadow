/*
 * The Shadow Simulator
 * See LICENSE for licensing information
 */

#include <time.h>
#include <sys/timerfd.h>

#include "shadow.h"

struct _Timer {
	Descriptor super;

	/* the absolute time the timer will next expire */
	SimulationTime nextExpireTime;
	/* the relative periodic expiration interval */
	SimulationTime expireInterval;
	/* number of expires that happened since the timer was last set */
	guint64 expireCountSinceLastSet;

	/* expire ids are used internally to cancel events that fire after
	 * they have become invalid because the user reset the timer */
	guint nextExpireID;
	guint minValidExpireID;

	guint numEventsScheduled;
	gboolean isClosed;

	MAGIC_DECLARE;
};

static void _timer_close(Timer* timer) {
	MAGIC_ASSERT(timer);
	timer->isClosed = TRUE;
	host_closeDescriptor(worker_getCurrentHost(), timer->super.handle);
}

static void _timer_free(Timer* timer) {
	MAGIC_ASSERT(timer);
	MAGIC_CLEAR(timer);
	g_free(timer);
}

static DescriptorFunctionTable _timerFunctions = {
	(DescriptorFunc) _timer_close,
	(DescriptorFunc) _timer_free,
	MAGIC_VALUE
};

Timer* timer_new(gint handle, gint clockid, gint flags) {
	if(clockid != CLOCK_REALTIME && clockid != CLOCK_MONOTONIC) {
		errno = EINVAL;
		return NULL;
	}

	if(flags != 0 && flags != TFD_NONBLOCK && flags != TFD_CLOEXEC
			&& flags != (TFD_NONBLOCK|TFD_CLOEXEC)) {
		errno = EINVAL;
		return NULL;
	}

	if(!(flags&TFD_NONBLOCK)) {
		warning("Shadow does not support blocking timers, using TFD_NONBLOCK flag implicitly");
	}

	Timer* timer = g_new0(Timer, 1);
	MAGIC_INIT(timer);

	descriptor_init(&(timer->super), DT_TIMER, &_timerFunctions, handle);

	return timer;
}

static void _timer_getCurrentTime(Timer* timer, struct timespec* out) {
	MAGIC_ASSERT(timer);
	utility_assert(out);

	if(timer->nextExpireTime == 0) {
		/* timer is disarmed */
		out->tv_sec = 0;
		out->tv_nsec = 0;
	} else {
		/* timer is armed */
		SimulationTime currentTime = worker_getCurrentTime();
		utility_assert(currentTime <= timer->nextExpireTime);

		/* always return relative value */
		SimulationTime diff = timer->nextExpireTime - currentTime;
		out->tv_sec = (time_t) (diff / SIMTIME_ONE_SECOND);
		out->tv_nsec =(glong) (diff % SIMTIME_ONE_SECOND);
	}
}

static void _timer_getCurrentInterval(Timer* timer, struct timespec* out) {
	MAGIC_ASSERT(timer);
	utility_assert(out);

	if(timer->expireInterval == 0) {
		/* timer is set to expire just once */
		out->tv_sec = 0;
		out->tv_nsec = 0;
	} else {
		out->tv_sec = (time_t) (timer->expireInterval / SIMTIME_ONE_SECOND);
		out->tv_nsec =(glong) (timer->expireInterval % SIMTIME_ONE_SECOND);
	}
}

gint timer_getTime(Timer* timer, struct itimerspec *curr_value) {
	MAGIC_ASSERT(timer);

	if(!curr_value) {
		errno = EFAULT;
		return -1;
	}

	_timer_getCurrentTime(timer, &(curr_value->it_value));
	_timer_getCurrentInterval(timer, &(curr_value->it_interval));

	return 0;
}

static void _timer_disarm(Timer* timer) {
	MAGIC_ASSERT(timer);
	timer->nextExpireTime = 0;
	timer->expireInterval = 0;
	timer->minValidExpireID = timer->nextExpireID;
}

static SimulationTime _timer_timespecToSimTime(const struct timespec* config) {
	utility_assert(config);

	SimulationTime nsecs = config->tv_sec * SIMTIME_ONE_SECOND;
	nsecs += (SimulationTime) config->tv_nsec;

	return nsecs;
}

static void _timer_setCurrentTime(Timer* timer, const struct timespec* config, gint flags) {
	MAGIC_ASSERT(timer);
	utility_assert(config);

	if(flags == TFD_TIMER_ABSTIME) {
		/* config time specifies an absolute time */
		/* XXX what happens if the time they gave us is in the past? the man
		 * page does not specify if this is an error or not.
		 * TODO check this in linux and update this comment. */
		timer->nextExpireTime = _timer_timespecToSimTime(config);
	} else {
		/* config time is relative to current time */
		timer->nextExpireTime = worker_getCurrentTime() + _timer_timespecToSimTime(config);
	}
}

static void _timer_setCurrentInterval(Timer* timer, const struct timespec* config) {
	MAGIC_ASSERT(timer);
	utility_assert(config);

	timer->expireInterval = _timer_timespecToSimTime(config);
}

static void _timer_expire(Timer* timer, gpointer data);

static void _timer_scheduleNewExpireEvent(Timer* timer) {
	MAGIC_ASSERT(timer);

	/* callback to our own node */
	gpointer next = GUINT_TO_POINTER(timer->nextExpireID);
	CallbackEvent* event = callback_new((CallbackFunc)_timer_expire, timer, next);

	SimulationTime nanos = timer->nextExpireTime - worker_getCurrentTime();
	worker_scheduleEvent((Event*)event, nanos, 0);

	timer->nextExpireID++;
	timer->numEventsScheduled++;
}

static void _timer_expire(Timer* timer, gpointer data) {
	MAGIC_ASSERT(timer);
	guint expireID = GPOINTER_TO_UINT(data);

	timer->numEventsScheduled--;

	if(timer->isClosed || expireID < timer->minValidExpireID) {
		/* the timer has been reset since we scheduled this expiration event */
		return;
	}

	/* XXX what happens if a one-time (non-periodic) timer already expired
	 * before they started listening for the event with epoll? when they start
	 * listening via epoll, is the event reported immediately, or is the
	 * one-time expiration lost?
	 * TODO check the behavior on linux, make sure shadow does it correctly,
	 * and update this comment.
	 */
	timer->expireCountSinceLastSet++;
	descriptor_adjustStatus(&(timer->super), DS_READABLE, TRUE);

	if(timer->expireInterval > 0) {
		timer->nextExpireTime += timer->expireInterval;
		if(timer->nextExpireTime > worker_getCurrentTime()) {
			_timer_scheduleNewExpireEvent(timer);
		}
	}
}

static void _timer_arm(Timer* timer, const struct itimerspec *config, gint flags) {
	MAGIC_ASSERT(timer);
	utility_assert(config);

	_timer_setCurrentTime(timer, &(config->it_value), flags);

	if(config->it_interval.tv_sec > 0 || config->it_interval.tv_nsec > 0) {
		_timer_setCurrentInterval(timer, &(config->it_interval));
	}

	if(timer->nextExpireTime > worker_getCurrentTime()) {
		_timer_scheduleNewExpireEvent(timer);
	}
}

static gboolean _timer_timeIsValid(const struct timespec* config) {
	utility_assert(config);
	return (config->tv_nsec < 0 || config->tv_nsec >= SIMTIME_ONE_SECOND) ? FALSE : TRUE;
}

gint timer_setTime(Timer* timer, gint flags,
				   const struct itimerspec *new_value,
				   struct itimerspec *old_value) {
	MAGIC_ASSERT(timer);

	if(!new_value) {
		errno = EFAULT;
		return -1;
	}

	if(!_timer_timeIsValid(&(new_value->it_value)) ||
			!_timer_timeIsValid(&(new_value->it_interval))) {
		errno = EINVAL;
		return -1;
	}

	if(flags != 0 && flags != TFD_TIMER_ABSTIME) {
		errno = EINVAL;
		return -1;
	}

	/* first get the old value if requested */
	if(old_value) {
		timer_gettime(timer, old_value);
	}

	/* always disarm to invalidate old expire events */
	_timer_disarm(timer);

	/* now set the new times as requested */
	if(new_value->it_value.tv_sec > 0 || new_value->it_value.tv_nsec > 0) {
		/* XXX the man page does not specify what to do if it_value says
		 * to disarm the timer, but it_interval is a valid interval.
		 * for now we assume that intervals are only set when it_value actually
		 * requests that we arm the timer, and ignored otherwise.
		 * TODO check that this is correct in linux and update this comment. */
		_timer_arm(timer, new_value, flags);
	}

	/* settings were modified, reset expire count */
	timer->expireCountSinceLastSet = 0;

	return 0;
}

ssize_t timer_read(Timer* timer, void *buf, size_t count) {
	MAGIC_ASSERT(timer);

	if(timer->expireCountSinceLastSet > 0) {
		/* we have something to report, make sure the buf is big enough */
		if(count < sizeof(guint64)) {
			errno = EINVAL;
			return (ssize_t) -1;
		}

		memcpy(buf, &(timer->expireCountSinceLastSet), sizeof(guint64));

		/* reset the expire count since we reported it */
		timer->expireCountSinceLastSet = 0;
		descriptor_adjustStatus(&(timer->super), DS_READABLE, FALSE);

		return (ssize_t) sizeof(guint64);
	} else {
		/* the timer has not yet expired, try again later */
		errno = EAGAIN;
		return (ssize_t) -1;
	}
}
