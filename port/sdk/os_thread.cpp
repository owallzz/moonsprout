// Dolphin OS threads, interrupts, mutexes, condition variables, message queues, alarms and the
// video retrace, on host threads.
//
// The GameCube runs game threads on one core with strict priority scheduling, and the game's code
// relies on that (OSDisableInterrupts is its lock). So every OSThread gets a host thread, but only
// one of them runs game code at a time: the "current" thread. The others wait on their own
// condition variable until the scheduler hands them the CPU. Switches happen when the current
// thread blocks, or when it wakes a thread with a higher priority (lower number) and the scheduler
// is enabled, as on the hardware.
//
// Interrupts (alarms and the 60 Hz retrace) come from a timer thread. Handlers must not run in
// parallel with game code, so the timer only queues them; they run on the current game thread at
// the next safe point (an interrupt enable, a mutex operation or a blocking call), or directly on
// the timer thread when every game thread is blocked.
#include <dolphin/os.h>
#include <dolphin/vi.h>

#include "p2_host.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#if defined(_WIN32)
// Declared here instead of <windows.h>, whose macros clash with SDK names.
extern "C" __declspec(dllimport) unsigned long __stdcall GetCurrentThreadId(void);
extern "C" __declspec(dllimport) __declspec(noreturn) void __stdcall ExitThread(unsigned long code);
#else
#include <pthread.h>
#include <unistd.h>
#endif
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

// tools.schedDebug: print the thread table every two seconds (read on the timer thread).
std::atomic<bool> g_schedDebug{false};

enum class HState {
	Created, // OSCreateThread called, host thread waiting for its first turn
	Ready,
	Running,
	Waiting,     // in OSSleepThread
	HostBlocked, // in a blocking host call (frame present); other game threads may run
	Dead,
};

struct HostThread {
	OSThread* os = nullptr;
	std::condition_variable cv;
	HState state      = HState::Created;
	bool interrupts   = true; // per-thread MSR[EE]
	int tid           = 0;    // host thread id (debugging)
	u64 readySeq      = 0;    // FIFO order among equal priorities
	OSThreadQueue* waitQueue = nullptr;
	void* (*entry)(void*) = nullptr;
	void* arg             = nullptr;
};

std::mutex g_lock; // protects all scheduler state below
std::unordered_map<OSThread*, HostThread*> g_threads;
std::unordered_map<OSThreadQueue*, std::deque<HostThread*>> g_waiters;
HostThread* g_current = nullptr; // thread running game code; null while idle
HostThread* g_last    = nullptr; // last thread that ran (interrupt context while idle)
u64 g_readyCounter    = 0;
s32 g_schedulerDisabled = 0;
bool g_inInterrupt      = false;
bool g_switchPending    = false; // a wakeup during an interrupt wants a reschedule
bool g_shuttingDown     = false; // quitting: no more interrupts or scheduling

// ---- pending interrupts
std::vector<OSAlarm*> g_alarms; // armed alarms
u32 g_retracesPending = 0;
std::atomic<bool> g_irqPending{false};
std::vector<std::pair<void (*)(void*), void*>> g_hostIrqs; // raised by host threads (audio)
std::condition_variable g_timerCv;
std::thread g_timerThread;

std::atomic<u32> g_retraceCount{0};
VIRetraceCallback g_preRetrace  = nullptr;
VIRetraceCallback g_postRetrace = nullptr;
OSThreadQueue g_retraceQueue{};
// Retrace grid (fps_design 5.4): steady_clock time and retrace count of the newest scheduled
// retrace, published under a sequence counter so the host reads them without g_lock.
std::atomic<u32> g_gridSeq{0};
std::atomic<long long> g_gridNs{0};
std::atomic<u32> g_gridCount{0};
std::atomic<long long> g_gridStep{0}; // M5.3b: the slew step already in the retrace after g_gridCount
// M5.3b online phase slew (docs/online/M5_3B_LOCKSTEP.md, "Timing"): the remaining correction of the
// retrace grid, applied at most 1 ms per retrace where the grid advances. Always 0 offline (nothing
// calls p2_vi_phase_slew_set), so the grid is exactly as before.
std::atomic<long long> g_phaseSlewNs{0};
constexpr long long kSlewStepMaxNs = 1000000;
// The next retrace time of timerMain, under g_lock (p2_vi_grid_jump moves it).
std::chrono::steady_clock::time_point g_nextRetrace{};
bool g_nextRetraceSet = false;

thread_local HostThread* t_self = nullptr;

HostThread* hostOf(OSThread* thread)
{
	auto it = g_threads.find(thread);
	return it == g_threads.end() ? nullptr : it->second;
}

bool runnable(const HostThread* t)
{
	return (t->state == HState::Ready || t->state == HState::Created) && t->os->suspend <= 0;
}

// Highest priority runnable thread (GameCube: lower value wins), FIFO among equals.
HostThread* pickNext()
{
	HostThread* best = nullptr;
	for (auto& [os, t] : g_threads) {
		if (!runnable(t)) {
			continue;
		}
		if (!best || t->os->priority < best->os->priority
		    || (t->os->priority == best->os->priority && t->readySeq < best->readySeq)) {
			best = t;
		}
	}
	return best;
}

void makeReady(HostThread* t)
{
	t->state    = HState::Ready;
	t->readySeq = ++g_readyCounter;
}

// Hand the CPU to the best runnable thread and, if that is not `self`, wait until `self` is
// scheduled again. With no runnable thread the system goes idle (g_current = null) and the timer
// thread schedules the next one after delivering interrupts. `self` is null on the timer thread.
void reschedule(std::unique_lock<std::mutex>& lk, HostThread* self)
{
	HostThread* next = g_shuttingDown ? nullptr : pickNext(); // quitting: park everyone
	g_current        = next;
	if (next) {
		next->state = HState::Running;
		g_last      = next;
		if (next != self) {
			next->cv.notify_one();
		}
	} else if (g_irqPending.load(std::memory_order_relaxed)) {
		g_timerCv.notify_one(); // idle with interrupts still pending: the timer delivers them now
	}
	if (self == nullptr || next == self) {
		return;
	}
	self->cv.wait(lk, [self] { return g_current == self; });
}

// After waking threads: switch if a higher-priority thread became runnable.
void maybePreempt(std::unique_lock<std::mutex>& lk)
{
	HostThread* self = t_self;
	if (g_inInterrupt) {
		g_switchPending = true;
		return;
	}
	if (self == nullptr || self != g_current || g_schedulerDisabled > 0) {
		return;
	}
	HostThread* next = pickNext();
	if (next && next != self && next->os->priority < self->os->priority) {
		makeReady(self);
		reschedule(lk, self);
	}
}

void wakeQueueLocked(OSThreadQueue* queue)
{
	auto it = g_waiters.find(queue);
	if (it == g_waiters.end()) {
		return;
	}
	for (HostThread* t : it->second) {
		if (t->state == HState::Waiting) {
			t->waitQueue = nullptr;
			makeReady(t);
		}
	}
	g_waiters.erase(it);
}

void sleepOnLocked(std::unique_lock<std::mutex>& lk, OSThreadQueue* queue)
{
	HostThread* self = t_self;
	if (self == nullptr) {
		std::fprintf(stderr, "mppm: OSSleepThread from a non-game thread\n");
		return;
	}
	self->state     = HState::Waiting;
	self->waitQueue = queue;
	g_waiters[queue].push_back(self);
	reschedule(lk, self);
}

// ---- interrupt delivery
void fireRetraceLocked(std::unique_lock<std::mutex>& lk)
{
	const u32 count = ++g_retraceCount;
	VIRetraceCallback pre  = g_preRetrace;
	VIRetraceCallback post = g_postRetrace;
	lk.unlock();
	if (pre) {
		pre(count);
	}
	if (post) {
		post(count);
	}
	lk.lock();
	wakeQueueLocked(&g_retraceQueue);
}

void runInterruptsLocked(std::unique_lock<std::mutex>& lk)
{
	if (g_inInterrupt) {
		return;
	}
	g_inInterrupt = true;
	while (g_irqPending.exchange(false)) {
		const OSTime now = OSGetTime();
		// Alarms, earliest first.
		for (;;) {
			OSAlarm* due = nullptr;
			for (OSAlarm* a : g_alarms) {
				if (a->fire <= now && (!due || a->fire < due->fire)) {
					due = a;
				}
			}
			if (!due) {
				break;
			}
			OSAlarmHandler handler = due->handler;
			if (due->period > 0) {
				const OSTime missed = (now - due->fire) / due->period + 1;
				due->fire += missed * due->period;
			} else {
				g_alarms.erase(std::find(g_alarms.begin(), g_alarms.end(), due));
				due->handler = nullptr;
			}
			if (handler) {
				lk.unlock();
				OSContext ctx{};
				handler(due, &ctx);
				lk.lock();
			}
		}
		while (g_retracesPending > 0) {
			g_retracesPending--;
			fireRetraceLocked(lk);
		}
		while (!g_hostIrqs.empty()) {
			auto irq = g_hostIrqs.front();
			g_hostIrqs.erase(g_hostIrqs.begin());
			lk.unlock();
			irq.first(irq.second);
			lk.lock();
		}
	}
	g_inInterrupt = false;
}

// Safe point on a game thread: run queued interrupts if this thread has them enabled.
void safePoint()
{
	HostThread* self = t_self;
	if (!g_irqPending.load(std::memory_order_relaxed) || self == nullptr || !self->interrupts) {
		return;
	}
	std::unique_lock lk(g_lock);
	if (g_current != self) {
		return;
	}
	runInterruptsLocked(lk);
	if (g_switchPending) {
		g_switchPending = false;
		maybePreempt(lk);
	}
}

constexpr auto kRetracePeriod = std::chrono::nanoseconds(1000000000LL * 1001 / 60000); // NTSC 59.94 Hz

void timerMain()
{
	using clock = std::chrono::steady_clock;
	std::unique_lock lk(g_lock);
	g_nextRetrace    = clock::now() + kRetracePeriod;
	g_nextRetraceSet = true;
	auto& nextRetrace = g_nextRetrace;
	for (;;) {
		// Sleep until the next retrace or the earliest alarm.
		auto wake = nextRetrace;
		const OSTime now = OSGetTime();
		// Due alarms already flagged in g_irqPending wait for the current game thread's next safe
		// point. Waking for them again would spin here with g_lock held (wait_until returns at once),
		// and the game thread needs g_lock to reach that safe point: on Windows, whose SRW locks
		// hand the lock straight back to the spinning thread, it never gets it and the game freezes.
		// With no game thread current the timer delivers them itself below, so they still count.
		const bool flagged = g_irqPending.load(std::memory_order_relaxed) && g_current != nullptr;
		for (OSAlarm* a : g_alarms) {
			const OSTime delta = a->fire - now;
			if (delta <= 0 && flagged) {
				continue;
			}
			const auto at = clock::now() + std::chrono::nanoseconds(delta <= 0 ? 0 : (s64)((double)delta * 1e9 / OS_TIMER_CLOCK));
			wake          = std::min(wake, at);
		}
		g_timerCv.wait_until(lk, wake);
		{
			static auto s_last = clock::now();
			if (g_schedDebug.load(std::memory_order_relaxed) && clock::now() - s_last > std::chrono::seconds(2)) {
				s_last = clock::now();
				std::fprintf(stderr, "sched: current=%p pending=%d hostirqs=%zu\n", (void*)g_current, (int)g_irqPending.load(), g_hostIrqs.size());
				for (auto& [os, t] : g_threads) {
					std::fprintf(stderr, "  thread %p tid %d prio %d state %d ints %d waitq %p entry %p arg %p\n", (void*)t, t->tid, os->priority, (int)t->state, (int)t->interrupts, (void*)t->waitQueue, (void*)t->entry, t->arg);
				}
			}
		}

		if (g_shuttingDown) {
			return;
		}
		bool any = false;
		if (clock::now() >= nextRetrace) {
			g_retracesPending++;
			// M5.3b phase slew: step = clamp(remaining, -1 ms, +1 ms), subtracted from the remaining
			// correction (a concurrent p2_vi_phase_slew_set is not lost). 0 offline.
			long long slew = g_phaseSlewNs.load(std::memory_order_relaxed);
			long long step = 0;
			while (slew != 0) {
				step = std::clamp(slew, -kSlewStepMaxNs, kSlewStepMaxNs);
				if (g_phaseSlewNs.compare_exchange_weak(slew, slew - step, std::memory_order_relaxed)) {
					break;
				}
				step = 0;
			}
			// This retrace becomes count g_retraceCount + g_retracesPending once delivered. The step of
			// the next one is published with it, so p2_vi_retrace_time_ns predicts a slewed grid.
			g_gridSeq.fetch_add(1, std::memory_order_acq_rel);
			g_gridNs.store(std::chrono::duration_cast<std::chrono::nanoseconds>(nextRetrace.time_since_epoch()).count(), std::memory_order_relaxed);
			g_gridCount.store(g_retraceCount.load() + g_retracesPending, std::memory_order_relaxed);
			g_gridStep.store(step, std::memory_order_relaxed);
			g_gridSeq.fetch_add(1, std::memory_order_acq_rel);
			nextRetrace += kRetracePeriod + std::chrono::nanoseconds(step);
			if (clock::now() > nextRetrace + kRetracePeriod * 4) {
				nextRetrace = clock::now() + kRetracePeriod; // don't try to catch up after a stall
			}
			any = true;
		}
		const OSTime t = OSGetTime();
		for (OSAlarm* a : g_alarms) {
			if (a->fire <= t) {
				any = true;
			}
		}
		if (!g_hostIrqs.empty()) {
			any = true;
		}
		if (!any) {
			continue;
		}
		g_irqPending = true;
		if (g_current == nullptr) {
			// Every game thread is blocked: deliver here, then run whoever became ready.
			runInterruptsLocked(lk);
			g_switchPending = false;
			// The lock is dropped around each handler; a thread may have been scheduled meanwhile.
			// Rescheduling over it would leave it marked Running but never current.
			if (g_current == nullptr) {
				reschedule(lk, nullptr);
			}
		}
	}
}

HostThread* registerMainThread()
{
	static OSThread sMainThread{};
	auto* t        = new HostThread();
	t->os          = &sMainThread;
	t->state       = HState::Running;
	sMainThread.priority = 16;
	sMainThread.base     = 16;
	sMainThread.state    = OS_THREAD_STATE_RUNNING;
	g_threads[&sMainThread] = t;
	g_current               = t;
	g_last                  = t;
	return t;
}

void ensureInit()
{
	static std::once_flag once;
	std::call_once(once, [] {
		std::lock_guard lk(g_lock);
		t_self       = registerMainThread();
		g_timerThread = std::thread(timerMain);
		g_timerThread.detach();
	});
}

void threadMain(HostThread* self)
{
#if defined(_WIN32)
	self->tid = (int)GetCurrentThreadId();
#elif defined(__APPLE__)
	uint64_t tid = 0;
	pthread_threadid_np(nullptr, &tid);
	self->tid = (int)tid;
#else
	self->tid = (int)gettid();
#endif
	t_self = self;
	{
		std::unique_lock lk(g_lock);
		self->cv.wait(lk, [self] { return g_current == self; });
		self->state = HState::Running;
	}
	void* ret = self->entry(self->arg);
	OSExitThread(ret);
}

} // namespace

extern "C" {

void p2_os_thread_init(void) { ensureInit(); }

// Quitting: stop delivering interrupts and keep every other game thread parked where it is, so
// nothing runs game code while Aurora shuts down. Called with the caller still the current thread.
void p2_os_shutdown(void)
{
	std::unique_lock lk(g_lock);
	g_shuttingDown = true;
	g_hostIrqs.clear();
	g_alarms.clear();
	g_timerCv.notify_all();
	// Another game thread may be running (the caller is blocked in the host frame loop); give it a
	// moment to reach its next blocking call, where reschedule() now parks it.
	for (int i = 0; i < 100 && g_current != nullptr && g_current != t_self; i++) {
		lk.unlock();
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		lk.lock();
	}
}

// A game thread spinning on a flag: deliver interrupts and let any other ready thread run (on the
// GameCube the spinner would be preempted). Sleeps briefly when nothing else can run.
void p2_spin_yield(void)
{
	HostThread* self = t_self;
	if (self != nullptr) {
		std::unique_lock lk(g_lock);
		if (g_current == self && !g_inInterrupt) {
			if (g_irqPending.load() && self->interrupts) {
				runInterruptsLocked(lk);
				g_switchPending = false;
			}
			if (g_schedulerDisabled == 0 && pickNext() != nullptr) {
				makeReady(self);
				reschedule(lk, self);
				return;
			}
		}
	}
	std::this_thread::sleep_for(std::chrono::microseconds(50));
}

// Queue an interrupt handler from a host thread (e.g. the audio device): it runs on a game thread
// at the next safe point, like an alarm, or on the timer thread when every game thread is blocked.
void p2_raise_interrupt(void (*fn)(void*), void* arg)
{
	ensureInit();
	std::unique_lock lk(g_lock);
	if (g_shuttingDown) {
		return;
	}
	g_hostIrqs.emplace_back(fn, arg);
	{ static int n; if ((++n % 50) == 0 && p2_audio_debug()) std::fprintf(stderr, "mppm irq: raised %d, queued %zu, current %p inInt %d\n", n, g_hostIrqs.size(), (void*)g_current, (int)g_inInterrupt); }
	g_irqPending = true;
	g_timerCv.notify_all();
}

void p2_sched_set_debug(int debug)
{
	g_schedDebug.store(debug != 0, std::memory_order_relaxed);
}

// The main thread blocks in the host frame loop (waiting for the GPU or vsync) where the GameCube
// would sleep in GXDrawDone/VIWaitForRetrace. Hand the game CPU to other threads meanwhile, so
// lower-priority threads (memory card, DVD, audio tasks) are not starved when the game runs
// behind 60 fps and never reaches a retrace wait.
void p2_host_block_begin(void)
{
	std::unique_lock lk(g_lock);
	HostThread* self = t_self;
	if (self == nullptr || g_current != self || g_schedulerDisabled > 0 || g_inInterrupt) {
		return;
	}
	self->state = HState::HostBlocked;
	reschedule(lk, nullptr); // run the next thread (or go idle) without waiting for our turn
}

void p2_host_block_end(void)
{
	std::unique_lock lk(g_lock);
	HostThread* self = t_self;
	if (self == nullptr || self->state != HState::HostBlocked) {
		return;
	}
	makeReady(self);
	if (g_current == nullptr && !g_inInterrupt) {
		reschedule(lk, self);
		return;
	}
	// Otherwise the running thread (or the timer thread, after delivering interrupts) picks us.
	// The running thread yields to us (higher priority) at its next safe point.
	g_switchPending = true;
	g_irqPending.store(true, std::memory_order_relaxed);
	self->cv.wait(lk, [self] { return g_current == self; });
}

// ---- interrupts
BOOL OSDisableInterrupts(void)
{
	HostThread* self = t_self;
	if (!self) {
		return FALSE;
	}
	BOOL old         = self->interrupts;
	self->interrupts = false;
	return old;
}
BOOL OSEnableInterrupts(void)
{
	HostThread* self = t_self;
	if (!self) {
		return FALSE;
	}
	BOOL old         = self->interrupts;
	self->interrupts = true;
	safePoint();
	return old;
}
BOOL OSRestoreInterrupts(BOOL level)
{
	HostThread* self = t_self;
	if (!self) {
		return FALSE;
	}
	BOOL old         = self->interrupts;
	self->interrupts = level != FALSE;
	if (level) {
		safePoint();
	}
	return old;
}

s32 OSDisableScheduler(void)
{
	std::lock_guard lk(g_lock);
	return g_schedulerDisabled++;
}
s32 OSEnableScheduler(void)
{
	std::unique_lock lk(g_lock);
	s32 old = g_schedulerDisabled--;
	if (g_schedulerDisabled == 0) {
		maybePreempt(lk);
	}
	return old;
}

// ---- threads
OSThread* OSGetCurrentThread(void)
{
	HostThread* self = t_self;
	if (self) {
		return self->os;
	}
	return g_last ? g_last->os : nullptr; // interrupt handler on the timer thread
}

BOOL OSCreateThread(OSThread* thread, void* (*func)(void*), void* param, void* stack, u32 stackSize, OSPriority priority,
                    u16 attr)
{
	ensureInit();
	std::lock_guard lk(g_lock);
	std::memset(thread, 0, sizeof(*thread));
	thread->priority  = priority;
	thread->base      = priority;
	thread->attr      = attr;
	thread->suspend   = 1;
	thread->state     = OS_THREAD_STATE_READY;
	thread->stackBase = static_cast<u8*>(stack);
	thread->stackEnd  = static_cast<u8*>(stack) - stackSize;

	if (HostThread* old = hostOf(thread)) {
		g_threads.erase(thread); // reused OSThread storage; the old host thread is gone or parked
		(void)old;
	}
	auto* t   = new HostThread();
	t->os     = thread;
	t->entry  = func;
	t->arg    = param;
	t->state  = HState::Created;
	t->readySeq = ++g_readyCounter;
	g_threads[thread] = t;
	std::thread(threadMain, t).detach();
	return TRUE;
}

s32 OSResumeThread(OSThread* thread)
{
	std::unique_lock lk(g_lock);
	s32 old = thread->suspend;
	if (thread->suspend > 0 && --thread->suspend == 0) {
		if (HostThread* t = hostOf(thread)) {
			if (t->state != HState::Created && t->state != HState::Waiting && t->state != HState::Dead) {
				makeReady(t);
			}
		}
		maybePreempt(lk);
	}
	return old;
}

s32 OSSuspendThread(OSThread* thread)
{
	std::unique_lock lk(g_lock);
	s32 old = thread->suspend++;
	if (old == 0) {
		HostThread* t = hostOf(thread);
		if (t && t == t_self && t == g_current && !g_inInterrupt) {
			makeReady(t); // stays out of pickNext() while suspend > 0
			reschedule(lk, t);
		}
	}
	return old;
}

BOOL OSIsThreadSuspended(OSThread* thread) { return thread->suspend > 0; }

void OSSleepThread(OSThreadQueue* queue)
{
	std::unique_lock lk(g_lock);
	sleepOnLocked(lk, queue);
}

void OSWakeupThread(OSThreadQueue* queue)
{
	std::unique_lock lk(g_lock);
	wakeQueueLocked(queue);
	maybePreempt(lk);
}

void OSInitThreadQueue(OSThreadQueue* queue) { std::memset(queue, 0, sizeof(*queue)); }

void OSYieldThread(void)
{
	safePoint();
	std::unique_lock lk(g_lock);
	HostThread* self = t_self;
	if (self && self == g_current) {
		makeReady(self);
		reschedule(lk, self);
	}
}

void OSExitThread(void* val)
{
	std::unique_lock lk(g_lock);
	HostThread* self = t_self;
	if (!self) {
		return;
	}
	self->state     = HState::Dead;
	self->os->state = OS_THREAD_STATE_MORIBUND;
	self->os->val   = val;
	wakeQueueLocked(&self->os->queueJoin);
	HostThread* next = pickNext();
	g_current        = next;
	if (next) {
		next->state = HState::Running;
		g_last      = next;
		next->cv.notify_one();
	} else if (g_irqPending.load(std::memory_order_relaxed)) {
		g_timerCv.notify_one(); // idle with interrupts still pending: the timer delivers them now
	}
	lk.unlock();
	// The host thread ends here; its OSThread storage belongs to the game.
	t_self = nullptr;
#if defined(_WIN32)
	ExitThread(0);
#else
	pthread_exit(nullptr);
#endif
}

void OSCancelThread(OSThread* thread)
{
	std::unique_lock lk(g_lock);
	HostThread* t = hostOf(thread);
	if (!t) {
		return;
	}
	if (t == t_self) {
		lk.unlock();
		OSExitThread(nullptr);
		return;
	}
	// A parked host thread can't be unwound safely; it stays blocked forever.
	t->state        = HState::Dead;
	thread->state   = OS_THREAD_STATE_MORIBUND;
	if (t->waitQueue) {
		auto& q = g_waiters[t->waitQueue];
		q.erase(std::remove(q.begin(), q.end(), t), q.end());
	}
	wakeQueueLocked(&thread->queueJoin);
}

BOOL OSJoinThread(OSThread* thread, void** val)
{
	std::unique_lock lk(g_lock);
	HostThread* t = hostOf(thread);
	while (t && t->state != HState::Dead) {
		sleepOnLocked(lk, &thread->queueJoin);
	}
	if (val) {
		*val = thread->val;
	}
	return TRUE;
}

void OSDetachThread(OSThread*) { }
BOOL OSIsThreadTerminated(OSThread* thread)
{
	std::lock_guard lk(g_lock);
	HostThread* t = hostOf(thread);
	return t && t->state == HState::Dead;
}
s32 OSGetThreadPriority(OSThread* thread) { return thread->priority; }
BOOL OSSetThreadPriority(OSThread* thread, OSPriority priority)
{
	std::unique_lock lk(g_lock);
	thread->priority = priority;
	thread->base     = priority;
	maybePreempt(lk);
	return TRUE;
}

// ---- contexts (no register state on the host)
static OSContext s_dummyContext;
OSContext* OSGetCurrentContext(void) { return &s_dummyContext; }
void OSFillFPUContext(OSContext*) { }
u32 OSGetStackPointer(void) { return 0; }

// ---- mutexes
void OSInitMutex(OSMutex* mutex) { std::memset(mutex, 0, sizeof(*mutex)); }

void OSLockMutex(OSMutex* mutex)
{
	safePoint();
	std::unique_lock lk(g_lock);
	OSThread* self = t_self ? t_self->os : nullptr;
	for (;;) {
		if (mutex->thread == nullptr) {
			mutex->thread = self;
			mutex->count  = 1;
			return;
		}
		if (mutex->thread == self) {
			mutex->count++;
			return;
		}
		sleepOnLocked(lk, &mutex->queue);
	}
}

BOOL OSTryLockMutex(OSMutex* mutex)
{
	std::lock_guard lk(g_lock);
	OSThread* self = t_self ? t_self->os : nullptr;
	if (mutex->thread == nullptr) {
		mutex->thread = self;
		mutex->count  = 1;
		return TRUE;
	}
	if (mutex->thread == self) {
		mutex->count++;
		return TRUE;
	}
	return FALSE;
}

void OSUnlockMutex(OSMutex* mutex)
{
	{
		std::unique_lock lk(g_lock);
		OSThread* self = t_self ? t_self->os : nullptr;
		if (mutex->thread == self && --mutex->count <= 0) {
			mutex->count  = 0;
			mutex->thread = nullptr;
			wakeQueueLocked(&mutex->queue);
			maybePreempt(lk);
		}
	}
	safePoint();
}

// ---- condition variables
void OSInitCond(OSCond* cond) { std::memset(cond, 0, sizeof(*cond)); }

void OSWaitCond(OSCond* cond, OSMutex* mutex)
{
	std::unique_lock lk(g_lock);
	OSThread* self = t_self ? t_self->os : nullptr;
	if (mutex->thread != self) {
		return;
	}
	const s32 count = mutex->count;
	mutex->count    = 0;
	mutex->thread   = nullptr;
	wakeQueueLocked(&mutex->queue);
	sleepOnLocked(lk, &cond->queue);
	while (mutex->thread != nullptr && mutex->thread != self) {
		sleepOnLocked(lk, &mutex->queue);
	}
	mutex->thread = self;
	mutex->count  = count;
}

void OSSignalCond(OSCond* cond) { OSWakeupThread(&cond->queue); }

// ---- message queues (SDK semantics)
void OSInitMessageQueue(OSMessageQueue* mq, OSMessage* msgArray, s32 msgCount)
{
	std::memset(mq, 0, sizeof(*mq));
	mq->msgArray = msgArray;
	mq->msgCount = msgCount;
}

BOOL OSSendMessage(OSMessageQueue* mq, OSMessage msg, s32 flags)
{
	std::unique_lock lk(g_lock);
	while (mq->usedCount >= mq->msgCount) {
		if (!(flags & OS_MESSAGE_BLOCK) || g_inInterrupt || !t_self) {
			return FALSE;
		}
		sleepOnLocked(lk, &mq->queueSend);
	}
	mq->msgArray[(mq->firstIndex + mq->usedCount) % mq->msgCount] = msg;
	mq->usedCount++;
	wakeQueueLocked(&mq->queueReceive);
	maybePreempt(lk);
	return TRUE;
}

BOOL OSJamMessage(OSMessageQueue* mq, OSMessage msg, s32 flags)
{
	std::unique_lock lk(g_lock);
	while (mq->usedCount >= mq->msgCount) {
		if (!(flags & OS_MESSAGE_BLOCK) || g_inInterrupt || !t_self) {
			return FALSE;
		}
		sleepOnLocked(lk, &mq->queueSend);
	}
	mq->firstIndex               = (mq->firstIndex + mq->msgCount - 1) % mq->msgCount;
	mq->msgArray[mq->firstIndex] = msg;
	mq->usedCount++;
	wakeQueueLocked(&mq->queueReceive);
	maybePreempt(lk);
	return TRUE;
}

BOOL OSReceiveMessage(OSMessageQueue* mq, OSMessage* msg, s32 flags)
{
	safePoint();
	std::unique_lock lk(g_lock);
	while (mq->usedCount == 0) {
		if (!(flags & OS_MESSAGE_BLOCK) || g_inInterrupt || !t_self) {
			return FALSE;
		}
		sleepOnLocked(lk, &mq->queueReceive);
	}
	if (msg) {
		*msg = mq->msgArray[mq->firstIndex];
	}
	mq->firstIndex = (mq->firstIndex + 1) % mq->msgCount;
	mq->usedCount--;
	wakeQueueLocked(&mq->queueSend);
	maybePreempt(lk);
	return TRUE;
}

// ---- alarms
void OSInitAlarm(void) { ensureInit(); }
void OSCreateAlarm(OSAlarm* alarm)
{
	std::memset(alarm, 0, sizeof(*alarm));
}

static void armLocked(OSAlarm* alarm)
{
	if (std::find(g_alarms.begin(), g_alarms.end(), alarm) == g_alarms.end()) {
		g_alarms.push_back(alarm);
	}
	g_timerCv.notify_one();
}

void OSSetAlarm(OSAlarm* alarm, OSTime tick, OSAlarmHandler handler)
{
	ensureInit();
	std::lock_guard lk(g_lock);
	alarm->handler = handler;
	alarm->period  = 0;
	alarm->fire    = OSGetTime() + tick;
	armLocked(alarm);
}

void OSSetPeriodicAlarm(OSAlarm* alarm, OSTime start, OSTime period, OSAlarmHandler handler)
{
	ensureInit();
	std::lock_guard lk(g_lock);
	alarm->handler = handler;
	alarm->period  = period;
	alarm->start   = start;
	const OSTime now = OSGetTime();
	alarm->fire      = start;
	if (period > 0 && alarm->fire < now) {
		alarm->fire += ((now - alarm->fire) / period + 1) * period;
	}
	armLocked(alarm);
}

void OSCancelAlarm(OSAlarm* alarm)
{
	std::lock_guard lk(g_lock);
	g_alarms.erase(std::remove(g_alarms.begin(), g_alarms.end(), alarm), g_alarms.end());
	alarm->handler = nullptr;
}

// ---- video retrace (the VI interrupt)
void VIWaitForRetrace(void)
{
	ensureInit();
	safePoint();
	std::unique_lock lk(g_lock);
	const u32 start = g_retraceCount.load();
	while (g_retraceCount.load() == start) {
		sleepOnLocked(lk, &g_retraceQueue);
	}
}

// steady_clock nanoseconds at which retrace `count` happens (or happened) on the 59.94 Hz grid.
// Before the first retrace: now plus the distance in periods.
long long p2_vi_retrace_time_ns(unsigned count)
{
	long long ns;
	long long step;
	u32 gridCount;
	u32 seq;
	do {
		seq       = g_gridSeq.load(std::memory_order_acquire);
		ns        = g_gridNs.load(std::memory_order_relaxed);
		gridCount = g_gridCount.load(std::memory_order_relaxed);
		step      = g_gridStep.load(std::memory_order_relaxed);
	} while ((seq & 1) != 0 || seq != g_gridSeq.load(std::memory_order_acquire));
	if (seq == 0) {
		ns        = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		gridCount = g_retraceCount.load();
		step      = 0;
	}
	const s32 k = (s32)(count - gridCount);
	ns += (long long)k * kRetracePeriod.count();
	if (k >= 1) {
		// M5.3b: a future retrace includes the phase slew (0 offline): the step already in the next
		// retrace, then up to 1 ms per further retrace of the remaining correction. An unslewed
		// prediction lets the late-sampling wait sleep past an early retrace while the one-slot
		// retrace queue still holds the previous count, and the tick then takes three retraces.
		const long long remaining = g_phaseSlewNs.load(std::memory_order_relaxed);
		const long long reach     = (long long)(k - 1) * kSlewStepMaxNs;
		ns += step + std::clamp(remaining, -reach, reach);
	}
	return ns;
}

// M5.3b (p2_net.h): the remaining grid correction, applied +-1 ms per retrace by timerMain.
void p2_vi_phase_slew_set(long long remainingNs)
{
	g_phaseSlewNs.store(remainingNs, std::memory_order_relaxed);
}

long long p2_vi_phase_slew_get(void)
{
	return g_phaseSlewNs.load(std::memory_order_relaxed);
}

// M5.3b load barrier: the next retrace fires at localNs (now when that is past), the slew is
// cleared, and p2_vi_retrace_time_ns of that retrace's count returns the new time at once.
u32 p2_vi_grid_jump(long long localNs)
{
	ensureInit();
	using clock = std::chrono::steady_clock;
	std::lock_guard lk(g_lock);
	const long long nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now().time_since_epoch()).count();
	const long long at    = std::max(localNs, nowNs);
	g_phaseSlewNs.store(0, std::memory_order_relaxed);
	const u32 count = g_retraceCount.load() + g_retracesPending + 1;
	g_nextRetrace    = clock::time_point(std::chrono::duration_cast<clock::duration>(std::chrono::nanoseconds(at)));
	g_nextRetraceSet = true;
	g_gridSeq.fetch_add(1, std::memory_order_acq_rel);
	g_gridNs.store(at, std::memory_order_relaxed);
	g_gridCount.store(count, std::memory_order_relaxed);
	g_gridStep.store(0, std::memory_order_relaxed);
	g_gridSeq.fetch_add(1, std::memory_order_acq_rel);
	g_timerCv.notify_all();
	return count;
}

u32 VIGetRetraceCount(void)
{
	// Some code busy-waits on this (JUTException); let pending retraces in.
	safePoint();
	return g_retraceCount.load();
}

VIRetraceCallback VISetPreRetraceCallback(VIRetraceCallback cb)
{
	std::lock_guard lk(g_lock);
	VIRetraceCallback old = g_preRetrace;
	g_preRetrace          = cb;
	return old;
}

VIRetraceCallback VISetPostRetraceCallback(VIRetraceCallback cb)
{
	std::lock_guard lk(g_lock);
	VIRetraceCallback old = g_postRetrace;
	g_postRetrace         = cb;
	return old;
}

} // extern "C"
