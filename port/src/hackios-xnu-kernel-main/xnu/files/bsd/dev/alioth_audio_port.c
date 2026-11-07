/* K40 startup and shared GCC voter for the Apollo audio drivers (APSL 2.0). */
#if defined(PLATFORM_ALIOTH)
#include <kern/thread.h>
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <sys/alioth_audio.h>
#include <sys/alioth_audio_port.h>
extern void IOSleep(unsigned int milliseconds);
extern kern_return_t thread_terminate(thread_t);
extern void redwood_rpmh_acquire(void);
extern void redwood_rpmh_release(void);
static volatile bool audio_stopping, audio_start_alive;
void alioth_gcc_vote(vm_offset_t vote, uint32_t bits)
{
    redwood_rpmh_acquire();
    *(volatile uint32_t *)vote |= bits;
    __asm__ volatile("dsb sy" ::: "memory");
    redwood_rpmh_release();
}
static void audio_start(void *unused, wait_result_t result)
{
    (void)unused; (void)result;
    uint32_t delay_ms = 30000;
    PE_parse_boot_argn("alioth_audio_delay", &delay_ms, sizeof(delay_ms));
    if (delay_ms > 120000) delay_ms = 120000;
    for (unsigned ms = 0; ms < delay_ms && !audio_stopping; ms += 100) IOSleep(100);
    if (!audio_stopping) alioth_adsp_init();
    __atomic_store_n(&audio_start_alive, false, __ATOMIC_RELEASE);
    (void)thread_terminate(current_thread());
}
void alioth_audio_port_init(void)
{
    uint32_t audio = 0, step = 0;
    PE_parse_boot_argn("alioth_audio", &audio, sizeof(audio));
    PE_parse_boot_argn("alioth_adsp_step", &step, sizeof(step));
    if (audio == 0 && step == 0) return;
    alioth_audio_init();
    thread_t thread;
    audio_start_alive = true;
    if (kernel_thread_start(audio_start, NULL, &thread) == KERN_SUCCESS) {
        thread_set_thread_name(thread, "alioth audio start");
        thread_deallocate(thread);
    } else audio_start_alive = false;
}
void alioth_audio_port_cancel(void)
{
    __atomic_store_n(&audio_stopping, true, __ATOMIC_RELEASE);
}
void alioth_audio_port_stop(void)
{
    alioth_audio_port_cancel();
    for (unsigned ms = 0; ms < 5000 && audio_start_alive; ms++) IOSleep(1);
    alioth_adsp_stop();
}
#endif
