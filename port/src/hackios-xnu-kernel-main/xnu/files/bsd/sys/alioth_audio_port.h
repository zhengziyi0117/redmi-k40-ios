/* Audio interfaces from hakios-apollo 620b134a; APSL 2.0, board-specific prefix. */
#ifndef _SYS_ALIOTH_AUDIO_PORT_H_
#define _SYS_ALIOTH_AUDIO_PORT_H_
#include <sys/redwood_audio.h>
void alioth_adsp_init(void);
void alioth_adsp_stop(void);            /* the run ends in order: the sound down, the DSP shut down (loader_return.c) */
void alioth_adsp_quiesce(void);         /* a panic's way back: amplifiers off, the DSP stopped, no lock, no sleep */
bool alioth_adsp_apr(uint8_t service, uint16_t port, uint32_t token, uint32_t opcode, const void *payload, uint32_t length,
    uint32_t wait_ms, struct redwood_apr_answer *answer);
void alioth_adsp_apr_events(redwood_apr_events_t handler);
void alioth_adsp_apr_events_second(redwood_apr_events_t handler);
const uint8_t *alioth_adsp_fw_file(const char *name, uint64_t *size);
void alioth_adsp_fw_free(const uint8_t *data, uint64_t size);
void alioth_sound_dsp_up(void);         /* the DSP's audio services are up */
void alioth_sound_dsp_down(void);       /* the DSP is about to be shut down */
void alioth_sound_quiesce(void);        /* a panic's way back */
/* the main microphone (alioth_mic=N, bsd/dev/hakios.alioth_mic.c): its path through the DSP; /dev/hakios-mic for iOS */
void alioth_mic_dsp_up(void);           /* the DSP's audio services are up */
void alioth_mic_dsp_down(void);         /* the DSP is about to be shut down: what the microphone's path asked for given back */
void alioth_mic_quiesce(void);          /* any way back: the codec into reset, the run's lines said again; no lock, no sleep */
bool alioth_audio_dsp_memory(uintptr_t *memory, uint64_t *dsp_address, uint32_t *bytes);
void alioth_audio_dsp_memory_put(void);
/* the two CS35L41 amplifiers (bsd/dev/hakios.alioth_cs35l41.c), as redwood_speaker_* are for the Awinic ones */
bool alioth_cs35l41_prepare(const uint8_t *config, uint64_t size);
bool alioth_cs35l41_start(void);
bool alioth_cs35l41_volume(uint32_t below, uint32_t step_ms);
bool alioth_cs35l41_healthy(void);
void alioth_cs35l41_stop(void);
void alioth_cs35l41_quiesce(void);
uint32_t alioth_cs35l41_monitor(bool tell);

void alioth_gcc_vote(vm_offset_t vote_register, uint32_t bits);
void alioth_audio_port_init(void);
void alioth_audio_port_cancel(void);
void alioth_audio_port_stop(void);
#endif
