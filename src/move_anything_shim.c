#define _GNU_SOURCE
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>

#include "host/plugin_api_v1.h"

unsigned char *global_mmap_addr = NULL;
FILE *output_file;
int frame_counter = 0;

/* ============================================================================
 * SHADOW INSTRUMENT SUPPORT
 * ============================================================================
 * The shadow instrument allows a separate DSP process to run alongside stock
 * Move, mixing its audio output with Move's audio and optionally taking over
 * the display when in shadow mode.
 * ============================================================================ */

/* Mailbox layout constants */
#define MAILBOX_SIZE 4096
#define MIDI_OUT_OFFSET 0
#define AUDIO_OUT_OFFSET 256
#define DISPLAY_OFFSET 768
#define MIDI_IN_OFFSET 2048
#define AUDIO_IN_OFFSET 2304

#define AUDIO_BUFFER_SIZE 512      /* 128 frames * 2 channels * 2 bytes */
#define MIDI_BUFFER_SIZE 256
#define DISPLAY_BUFFER_SIZE 1024   /* 128x64 @ 1bpp = 1024 bytes */
#define CONTROL_BUFFER_SIZE 64
#define FRAMES_PER_BLOCK 128

typedef struct shadow_control_t {
    volatile uint8_t display_mode;    /* 0=stock Move, 1=shadow display */
    volatile uint8_t shadow_ready;    /* 1 when shadow process is running */
    volatile uint8_t should_exit;     /* 1 to signal shadow to exit */
    volatile uint8_t midi_ready;      /* increments when new MIDI available */
    volatile uint8_t write_idx;       /* triple buffer: shadow writes here */
    volatile uint8_t read_idx;        /* triple buffer: shim reads here */
    volatile uint32_t shim_counter;   /* increments each ioctl for drift correction */
    volatile uint8_t reserved[53];    /* padding for future use */
} shadow_control_t;

static shadow_control_t *shadow_control = NULL;

/* ============================================================================
 * IN-PROCESS SHADOW SYNTH (DX7 POC)
 * ============================================================================
 * Load DX7 directly inside the shim and render in the ioctl audio cadence.
 * This avoids IPC timing drift and provides a stable audio mix proof-of-concept.
 * ============================================================================ */

#define SHADOW_INPROCESS_POC 1
#define SHADOW_DX7_MODULE_DIR "/data/UserData/move-anything/modules/dx7"
#define SHADOW_DX7_DSP_PATH "/data/UserData/move-anything/modules/dx7/dsp.so"

static void *shadow_dsp_handle = NULL;
static const plugin_api_v1_t *shadow_plugin_v1 = NULL;
static const plugin_api_v2_t *shadow_plugin_v2 = NULL;
static void *shadow_plugin_instance = NULL;
static host_api_v1_t shadow_host_api;
static int shadow_inprocess_ready = 0;

static void shadow_log(const char *msg) {
    FILE *log = fopen("/data/UserData/move-anything/shadow_inprocess.log", "a");
    if (log) {
        fprintf(log, "%s\n", msg ? msg : "(null)");
        fclose(log);
    }
}

static int shadow_inprocess_load_dx7(void) {
    if (shadow_inprocess_ready) return 0;

    shadow_dsp_handle = dlopen(SHADOW_DX7_DSP_PATH, RTLD_NOW | RTLD_LOCAL);
    if (!shadow_dsp_handle) {
        fprintf(stderr, "Shadow inprocess: failed to load %s: %s\n",
                SHADOW_DX7_DSP_PATH, dlerror());
        return -1;
    }

    memset(&shadow_host_api, 0, sizeof(shadow_host_api));
    shadow_host_api.api_version = MOVE_PLUGIN_API_VERSION;
    shadow_host_api.sample_rate = MOVE_SAMPLE_RATE;
    shadow_host_api.frames_per_block = MOVE_FRAMES_PER_BLOCK;
    shadow_host_api.mapped_memory = global_mmap_addr;
    shadow_host_api.audio_out_offset = MOVE_AUDIO_OUT_OFFSET;
    shadow_host_api.audio_in_offset = MOVE_AUDIO_IN_OFFSET;
    shadow_host_api.log = shadow_log;

    move_plugin_init_v2_fn init_v2 = (move_plugin_init_v2_fn)dlsym(
        shadow_dsp_handle, MOVE_PLUGIN_INIT_V2_SYMBOL);
    if (init_v2) {
        shadow_plugin_v2 = init_v2(&shadow_host_api);
        if (shadow_plugin_v2 && shadow_plugin_v2->create_instance) {
            shadow_plugin_instance = shadow_plugin_v2->create_instance(
                SHADOW_DX7_MODULE_DIR,
                "{\"syx_path\":\"/data/UserData/move-anything/modules/dx7/patches.syx\",\"preset\":0}");
        }
    }

    if (!shadow_plugin_instance) {
        move_plugin_init_v1_fn init_v1 = (move_plugin_init_v1_fn)dlsym(
            shadow_dsp_handle, MOVE_PLUGIN_INIT_SYMBOL);
        if (!init_v1) {
            fprintf(stderr, "Shadow inprocess: failed to find %s: %s\n",
                    MOVE_PLUGIN_INIT_SYMBOL, dlerror());
            dlclose(shadow_dsp_handle);
            shadow_dsp_handle = NULL;
            return -1;
        }

        shadow_plugin_v1 = init_v1(&shadow_host_api);
        if (!shadow_plugin_v1) {
            fprintf(stderr, "Shadow inprocess: plugin init returned NULL\n");
            dlclose(shadow_dsp_handle);
            shadow_dsp_handle = NULL;
            return -1;
        }

        if (shadow_plugin_v1->on_load) {
            int result = shadow_plugin_v1->on_load(
                SHADOW_DX7_MODULE_DIR,
                "{\"syx_path\":\"/data/UserData/move-anything/modules/dx7/patches.syx\",\"preset\":0}");
            if (result != 0) {
                fprintf(stderr, "Shadow inprocess: on_load failed: %d\n", result);
                dlclose(shadow_dsp_handle);
                shadow_dsp_handle = NULL;
                shadow_plugin_v1 = NULL;
                return -1;
            }
        }
    }

    shadow_inprocess_ready = 1;
    if (shadow_control) {
        /* Allow display hotkey when running in-process DSP. */
        shadow_control->shadow_ready = 1;
    }
    shadow_log("Shadow inprocess: DX7 loaded");
    return 0;
}

static void shadow_inprocess_process_midi(void) {
    if (!shadow_inprocess_ready || !global_mmap_addr) return;

    const uint8_t *src = global_mmap_addr + MIDI_IN_OFFSET;
    for (int i = 0; i < MIDI_BUFFER_SIZE; i += 4) {
        const uint8_t *pkt = &src[i];
        if (pkt[0] == 0 && pkt[1] == 0 && pkt[2] == 0 && pkt[3] == 0) continue;

        uint8_t cable = (pkt[0] >> 4) & 0x0F;
        uint8_t cin = pkt[0] & 0x0F;

        if (pkt[1] == 0xFE || pkt[1] == 0xF8 || cin == 0x0F) continue;
        if (cable != 0) continue;
        if (cin < 0x08 || cin > 0x0E) continue;

        if (shadow_plugin_v2 && shadow_plugin_v2->on_midi && shadow_plugin_instance) {
            shadow_plugin_v2->on_midi(shadow_plugin_instance, &pkt[1], 3,
                                      MOVE_MIDI_SOURCE_INTERNAL);
        } else if (shadow_plugin_v1 && shadow_plugin_v1->on_midi) {
            shadow_plugin_v1->on_midi(&pkt[1], 3, MOVE_MIDI_SOURCE_INTERNAL);
        }
    }
}

static void shadow_inprocess_mix_audio(void) {
    if (!shadow_inprocess_ready || !global_mmap_addr) return;

    int16_t *mailbox_audio = (int16_t *)(global_mmap_addr + AUDIO_OUT_OFFSET);
    int16_t render_buffer[FRAMES_PER_BLOCK * 2];
    memset(render_buffer, 0, sizeof(render_buffer));

    if (shadow_plugin_v2 && shadow_plugin_v2->render_block && shadow_plugin_instance) {
        shadow_plugin_v2->render_block(shadow_plugin_instance, render_buffer, MOVE_FRAMES_PER_BLOCK);
    } else if (shadow_plugin_v1 && shadow_plugin_v1->render_block) {
        shadow_plugin_v1->render_block(render_buffer, MOVE_FRAMES_PER_BLOCK);
    }

    for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
        int32_t mixed = (int32_t)mailbox_audio[i] + (int32_t)render_buffer[i];
        if (mixed > 32767) mixed = 32767;
        if (mixed < -32768) mixed = -32768;
        mailbox_audio[i] = (int16_t)mixed;
    }
}

/* Shared memory segment names */
#define SHM_SHADOW_AUDIO    "/move-shadow-audio"    /* Shadow's mixed output */
#define SHM_SHADOW_MIDI     "/move-shadow-midi"
#define SHM_SHADOW_DISPLAY  "/move-shadow-display"
#define SHM_SHADOW_CONTROL  "/move-shadow-control"
#define SHM_SHADOW_MOVEIN   "/move-shadow-movein"   /* Move's audio for shadow to read */


#define NUM_AUDIO_BUFFERS 3  /* Triple buffering */

/* Shadow shared memory pointers */
static int16_t *shadow_audio_shm = NULL;    /* Shadow's mixed output */
static int16_t *shadow_movein_shm = NULL;   /* Move's audio for shadow to read */
static uint8_t *shadow_midi_shm = NULL;
static uint8_t *shadow_display_shm = NULL;

/* Shadow shared memory file descriptors */
static int shm_audio_fd = -1;
static int shm_movein_fd = -1;
static int shm_midi_fd = -1;
static int shm_display_fd = -1;
static int shm_control_fd = -1;

/* Shadow initialization state */
static int shadow_shm_initialized = 0;

/* Initialize shadow shared memory segments */
static void init_shadow_shm(void)
{
    if (shadow_shm_initialized) return;

    printf("Shadow: Initializing shared memory...\n");

    /* Create/open audio shared memory - triple buffered */
    size_t triple_audio_size = AUDIO_BUFFER_SIZE * NUM_AUDIO_BUFFERS;
    shm_audio_fd = shm_open(SHM_SHADOW_AUDIO, O_CREAT | O_RDWR, 0666);
    if (shm_audio_fd >= 0) {
        ftruncate(shm_audio_fd, triple_audio_size);
        shadow_audio_shm = (int16_t *)mmap(NULL, triple_audio_size,
                                            PROT_READ | PROT_WRITE,
                                            MAP_SHARED, shm_audio_fd, 0);
        if (shadow_audio_shm == MAP_FAILED) {
            shadow_audio_shm = NULL;
            printf("Shadow: Failed to mmap audio shm\n");
        } else {
            memset(shadow_audio_shm, 0, triple_audio_size);
        }
    } else {
        printf("Shadow: Failed to create audio shm\n");
    }

    /* Create/open Move audio input shared memory (for shadow to read Move's audio) */
    shm_movein_fd = shm_open(SHM_SHADOW_MOVEIN, O_CREAT | O_RDWR, 0666);
    if (shm_movein_fd >= 0) {
        ftruncate(shm_movein_fd, AUDIO_BUFFER_SIZE);
        shadow_movein_shm = (int16_t *)mmap(NULL, AUDIO_BUFFER_SIZE,
                                             PROT_READ | PROT_WRITE,
                                             MAP_SHARED, shm_movein_fd, 0);
        if (shadow_movein_shm == MAP_FAILED) {
            shadow_movein_shm = NULL;
            printf("Shadow: Failed to mmap movein shm\n");
        } else {
            memset(shadow_movein_shm, 0, AUDIO_BUFFER_SIZE);
        }
    } else {
        printf("Shadow: Failed to create movein shm\n");
    }

    /* Create/open MIDI shared memory */
    shm_midi_fd = shm_open(SHM_SHADOW_MIDI, O_CREAT | O_RDWR, 0666);
    if (shm_midi_fd >= 0) {
        ftruncate(shm_midi_fd, MIDI_BUFFER_SIZE);
        shadow_midi_shm = (uint8_t *)mmap(NULL, MIDI_BUFFER_SIZE,
                                           PROT_READ | PROT_WRITE,
                                           MAP_SHARED, shm_midi_fd, 0);
        if (shadow_midi_shm == MAP_FAILED) {
            shadow_midi_shm = NULL;
            printf("Shadow: Failed to mmap MIDI shm\n");
        } else {
            memset(shadow_midi_shm, 0, MIDI_BUFFER_SIZE);
        }
    } else {
        printf("Shadow: Failed to create MIDI shm\n");
    }

    /* Create/open display shared memory */
    shm_display_fd = shm_open(SHM_SHADOW_DISPLAY, O_CREAT | O_RDWR, 0666);
    if (shm_display_fd >= 0) {
        ftruncate(shm_display_fd, DISPLAY_BUFFER_SIZE);
        shadow_display_shm = (uint8_t *)mmap(NULL, DISPLAY_BUFFER_SIZE,
                                              PROT_READ | PROT_WRITE,
                                              MAP_SHARED, shm_display_fd, 0);
        if (shadow_display_shm == MAP_FAILED) {
            shadow_display_shm = NULL;
            printf("Shadow: Failed to mmap display shm\n");
        } else {
            memset(shadow_display_shm, 0, DISPLAY_BUFFER_SIZE);
        }
    } else {
        printf("Shadow: Failed to create display shm\n");
    }

    /* Create/open control shared memory - DON'T zero it, shadow_poc owns the state */
    shm_control_fd = shm_open(SHM_SHADOW_CONTROL, O_CREAT | O_RDWR, 0666);
    if (shm_control_fd >= 0) {
        ftruncate(shm_control_fd, CONTROL_BUFFER_SIZE);
        shadow_control = (shadow_control_t *)mmap(NULL, CONTROL_BUFFER_SIZE,
                                                   PROT_READ | PROT_WRITE,
                                                   MAP_SHARED, shm_control_fd, 0);
        if (shadow_control == MAP_FAILED) {
            shadow_control = NULL;
            printf("Shadow: Failed to mmap control shm\n");
        }
        /* Note: We intentionally don't memset control - shadow_poc sets shadow_ready */
    } else {
        printf("Shadow: Failed to create control shm\n");
    }

    shadow_shm_initialized = 1;
    printf("Shadow: Shared memory initialized (audio=%p, midi=%p, display=%p, control=%p)\n",
           shadow_audio_shm, shadow_midi_shm, shadow_display_shm, shadow_control);
}

/* Debug: detailed dump of control regions and offset 256 area */
static void debug_full_mailbox_dump(void) {
    static int dump_count = 0;
    static FILE *dump_file = NULL;

    /* Only dump occasionally */
    if (dump_count++ % 10000 != 0 || dump_count > 50000) return;

    if (!dump_file) {
        dump_file = fopen("/data/UserData/move-anything/mailbox_dump.log", "a");
    }

    if (dump_file && global_mmap_addr) {
        fprintf(dump_file, "\n=== Dump %d ===\n", dump_count);

        /* Dump first 512 bytes in detail (includes offset 256 audio area) */
        fprintf(dump_file, "First 512 bytes (includes audio out @ 256):\n");
        for (int row = 0; row < 512; row += 32) {
            fprintf(dump_file, "%4d: ", row);
            for (int i = 0; i < 32; i++) {
                fprintf(dump_file, "%02x ", global_mmap_addr[row + i]);
            }
            fprintf(dump_file, "\n");
        }

        /* Dump last 128 bytes (offset 3968-4095) for control flags */
        fprintf(dump_file, "\nLast 128 bytes (control region?):\n");
        for (int row = 3968; row < 4096; row += 32) {
            fprintf(dump_file, "%4d: ", row);
            for (int i = 0; i < 32; i++) {
                fprintf(dump_file, "%02x ", global_mmap_addr[row + i]);
            }
            fprintf(dump_file, "\n");
        }
        fflush(dump_file);
    }
}

/* Debug: continuously log non-zero audio regions */
static void debug_audio_offset(void) {
    /* DISABLED - using ioctl logging instead */
    return;
}

/* Mix shadow audio into mailbox audio buffer - TRIPLE BUFFERED */
static void shadow_mix_audio(void)
{
    if (!shadow_audio_shm || !global_mmap_addr) return;
    if (!shadow_control || !shadow_control->shadow_ready) return;

    int16_t *mailbox_audio = (int16_t *)(global_mmap_addr + AUDIO_OUT_OFFSET);

    /* Increment shim counter for shadow's drift correction */
    shadow_control->shim_counter++;

    /* Copy Move's audio to shared memory so shadow can mix it */
    if (shadow_movein_shm) {
        memcpy(shadow_movein_shm, mailbox_audio, AUDIO_BUFFER_SIZE);
    }

    /*
     * Triple buffering read strategy:
     * - Read from buffer that's 2 behind write (gives shadow time to render)
     * - This adds ~6ms latency but smooths out timing jitter
     */
    uint8_t write_idx = shadow_control->write_idx;
    uint8_t read_idx = (write_idx + NUM_AUDIO_BUFFERS - 2) % NUM_AUDIO_BUFFERS;

    /* Update read index for shadow's reference */
    shadow_control->read_idx = read_idx;

    /* Get pointer to the buffer we should read */
    int16_t *src_buffer = shadow_audio_shm + (read_idx * FRAMES_PER_BLOCK * 2);

    /* 0 = mix shadow with Move, 1 = replace Move audio entirely */
    #define SHADOW_AUDIO_REPLACE 0

    #if SHADOW_AUDIO_REPLACE
    /* Replace Move's audio entirely with shadow audio */
    memcpy(mailbox_audio, src_buffer, AUDIO_BUFFER_SIZE);
    #else
    /* Mix shadow audio with Move's audio */
    for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
        int32_t mixed = (int32_t)mailbox_audio[i] + (int32_t)src_buffer[i];
        /* Clip to int16 range */
        if (mixed > 32767) mixed = 32767;
        if (mixed < -32768) mixed = -32768;
        mailbox_audio[i] = (int16_t)mixed;
    }
    #endif
}

/* Copy incoming MIDI from mailbox to shadow shared memory */
static void shadow_forward_midi(void)
{
    if (!shadow_midi_shm || !global_mmap_addr) return;
    if (!shadow_control) return;

    uint8_t *src = global_mmap_addr + MIDI_IN_OFFSET;

    /* Only copy if there's actual MIDI data (check first 64 bytes for non-zero) */
    int has_midi = 0;
    for (int i = 0; i < 64 && !has_midi; i += 4) {
        /* Look for valid USB-MIDI packets (CIN in byte 0) */
        uint8_t cin = src[i] & 0x0F;
        if (cin >= 0x08 && cin <= 0x0E) {
            has_midi = 1;
        }
    }

    if (has_midi) {
        /* Copy MIDI data from mailbox to shared memory */
        memcpy(shadow_midi_shm, src, MIDI_BUFFER_SIZE);
        /* Signal that new MIDI is available */
        shadow_control->midi_ready++;
    }
}

/* Debug counter for display swap */
static int display_swap_debug_counter = 0;

/* Swap display buffer if in shadow mode */
static void shadow_swap_display(void)
{
    static FILE *debug_log = NULL;

    if (!shadow_display_shm || !global_mmap_addr) {
        return;
    }
    if (!shadow_control || !shadow_control->shadow_ready) {
        return;
    }
    if (!shadow_control->display_mode) {
        return;  /* Not in shadow mode */
    }

    /* Log every 100th swap attempt */
    if (display_swap_debug_counter++ % 100 == 0) {
        if (!debug_log) {
            debug_log = fopen("/data/UserData/move-anything/shadow_debug.log", "a");
        }
        if (debug_log) {
            fprintf(debug_log, "swap #%d: display_shm=%p, mailbox=%p, display_mode=%d, shadow_ready=%d\n",
                    display_swap_debug_counter, shadow_display_shm, global_mmap_addr,
                    shadow_control->display_mode, shadow_control->shadow_ready);
            fflush(debug_log);
        }
    }

    /* Overwrite mailbox display with shadow display */
    /* Try offset 84 where Move Anything writes display (sliced at 172 bytes per frame) */
    /* Also try offset 768 where we see stock Move data */
    static int slice = 0;
    int slice_offset = slice * 172;
    int slice_bytes = (slice == 5) ? 164 : 172;  /* Last slice is smaller */

    /* Write slice indicator at offset 80 */
    global_mmap_addr[80] = slice + 1;

    /* Write display slice to offset 84 */
    if (slice_offset + slice_bytes <= DISPLAY_BUFFER_SIZE) {
        memcpy(global_mmap_addr + 84, shadow_display_shm + slice_offset, slice_bytes);
    }

    slice = (slice + 1) % 6;

    /* Also write to offset 768 in case that's where stock Move reads */
    memcpy(global_mmap_addr + DISPLAY_OFFSET, shadow_display_shm, DISPLAY_BUFFER_SIZE);

    /* Dump full mailbox once to find display location */
    static int test_counter = 0;
    if (test_counter++ == 1000) {  /* After some time so Move has drawn something */
        FILE *dump = fopen("/data/UserData/move-anything/mailbox_full.log", "w");
        if (dump) {
            fprintf(dump, "Full mailbox dump (4096 bytes):\n");
            for (int i = 0; i < 4096; i++) {
                if (i % 256 == 0) fprintf(dump, "\n=== OFFSET %d (0x%x) ===\n", i, i);
                fprintf(dump, "%02x ", (unsigned char)global_mmap_addr[i]);
                if ((i+1) % 32 == 0) fprintf(dump, "\n");
            }
            fclose(dump);
        }
    }
}

void print_mem()
{
    printf("\033[H\033[J");
    for (int i = 0; i < 4096; ++i)
    {
        printf("%02x ", (unsigned char)global_mmap_addr[i]);
        if (i == 2048 - 1)
        {
            printf("\n\n");
        }

        if (i == 2048 + 256 - 1)
        {
            printf("\n\n");
        }

        if (i == 2048 + 256 + 512 - 1)
        {
            printf("\n\n");
        }
    }
    printf("\n\n");
}

void write_mem()
{
    if (!output_file)
    {
        return;
    }

    // printf("\033[H\033[J");
    fprintf(output_file, "--------------------------------------------------------------------------------------------------------------");
    fprintf(output_file, "Frame: %d\n", frame_counter);
    for (int i = 0; i < 4096; ++i)
    {
        fprintf(output_file, "%02x ", (unsigned char)global_mmap_addr[i]);
        if (i == 2048 - 1)
        {
            fprintf(output_file, "\n\n");
        }

        if (i == 2048 + 256 - 1)
        {
            fprintf(output_file, "\n\n");
        }

        if (i == 2048 + 256 + 512 - 1)
        {
            fprintf(output_file, "\n\n");
        }
    }
    fprintf(output_file, "\n\n");

    sync();

    frame_counter++;
}

void *(*real_mmap)(void *, size_t, int, int, int, off_t) = NULL;

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{

    printf(">>>>>>>>>>>>>>>>>>>>>>>> Hooked mmap...\n");
    if (!real_mmap)
    {
        real_mmap = dlsym(RTLD_NEXT, "mmap");
        if (!real_mmap)
        {
            fprintf(stderr, "Error: dlsym failed to find mmap\n");
            exit(1);
        }
    }

    void *result = real_mmap(addr, length, prot, flags, fd, offset);

    if (length == 4096)
    {
        global_mmap_addr = result;
        /* Initialize shadow shared memory when we detect the SPI mailbox */
        init_shadow_shm();
#if SHADOW_INPROCESS_POC
        shadow_inprocess_load_dx7();
#endif
    }

    printf("mmap hooked! addr=%p, length=%zu, prot=%d, flags=%d, fd=%d, offset=%lld, result=%p\n",
           addr, length, prot, flags, fd, (long long)offset, result);

    // output_file = fopen("spi_memory.txt", "w+");

    return result;
}

void launchChildAndKillThisProcess(char *pBinPath, char*pBinName, char* pArgs)
{
    int pid = fork();

    if (pid < 0)
    {
        printf("Fork failed\n");
        exit(1);
    }
    else if (pid == 0)
    {
        // Child process
        setsid();
        // Perform detached task
        printf("Child process running in the background...\n");

        printf("Args: %s\n", pArgs);

        // Close all file descriptors, otherwise /dev/ablspi0.0 is held open
        // and the control surface code can't open it.
        printf("Closing file descriptors...\n");
        int fdlimit = (int)sysconf(_SC_OPEN_MAX);
        for (int i = STDERR_FILENO + 1; i < fdlimit; i++)
        {
            close(i);
        }

        // Let's a go!
        int ret = execl(pBinPath, pBinName, pArgs, (char *)0);
    }
    else
    {
        // parent
        kill(getpid(), SIGINT);
    }
}

int (*real_ioctl)(int, unsigned long, char *) = NULL;

int shiftHeld = 0;
int volumeTouched = 0;
int wheelTouched = 0;
int knob8touched = 0;
int knob1touched = 0;
int alreadyLaunched = 0;       /* Prevent multiple launches */
int shadowModeDebounce = 0;    /* Debounce for shadow mode toggle */

static FILE *hotkey_state_log = NULL;
static void log_hotkey_state(const char *tag)
{
    if (!hotkey_state_log)
    {
        hotkey_state_log = fopen("/data/UserData/move-anything/hotkey_state.log", "a");
    }
    if (hotkey_state_log)
    {
        time_t now = time(NULL);
        fprintf(hotkey_state_log, "%ld %s shift=%d vol=%d knob1=%d knob8=%d debounce=%d\n",
                (long)now, tag, shiftHeld, volumeTouched, knob1touched, knob8touched, shadowModeDebounce);
        fflush(hotkey_state_log);
    }
}

void midi_monitor()
{
    if (!global_mmap_addr)
    {
        return;
    }

    int startByte = 2048;
    int length = 256;
    int endByte = startByte + length;

    for (int i = startByte; i < endByte; i += 4)
    {
        if ((unsigned int)global_mmap_addr[i] == 0)
        {
            continue;
        }

        unsigned char *byte = &global_mmap_addr[i];
        unsigned char cable = (*byte & 0b11110000) >> 4;
        unsigned char code_index_number = (*byte & 0b00001111);
        unsigned char midi_0 = *(byte + 1);
        unsigned char midi_1 = *(byte + 2);
        unsigned char midi_2 = *(byte + 3);

        if (code_index_number == 2 || code_index_number == 1 || (cable == 0xf && code_index_number == 0xb && midi_0 == 176))
        {
            continue;
        }

        if (midi_0 + midi_1 + midi_2 == 0)
        {
            continue;
        }

        int controlMessage = 0xb0;
        if (midi_0 == controlMessage)
        {
            printf("Control message\n");

            if (midi_1 == 0x31)
            {
                if (midi_2 == 0x7f)
                {
                    printf("Shift on\n");

                    shiftHeld = 1;
                    log_hotkey_state("shift_on");
                }
                else
                {
                    printf("Shift off\n");

                    shiftHeld = 0;
                    log_hotkey_state("shift_off");
                }
            }

        }

        if (midi_0 == 0x90 && midi_1 == 0x07)
        {
            if (midi_2 == 0x7f)
            {
                knob8touched = 1;
                printf("Knob 8 touch start\n");
                log_hotkey_state("knob8_on");
            }
            else
            {
                knob8touched = 0;
                printf("Knob 8 touch stop\n");
                log_hotkey_state("knob8_off");
            }
        }

        /* Knob 1 touch detection (Note 0) - for shadow mode toggle */
        if (midi_0 == 0x90 && midi_1 == 0x00)
        {
            if (midi_2 == 0x7f)
            {
                knob1touched = 1;
                printf("Knob 1 touch start\n");
                log_hotkey_state("knob1_on");
            }
            else
            {
                knob1touched = 0;
                printf("Knob 1 touch stop\n");
                log_hotkey_state("knob1_off");
            }
        }

        if (midi_0 == 0x90 && midi_1 == 0x08)
        {
            if (midi_2 == 0x7f)
            {
                volumeTouched = 1;
                log_hotkey_state("vol_on");
            }
            else
            {
                volumeTouched = 0;
                log_hotkey_state("vol_off");
            }
        }

        if (midi_0 == 0x90 && midi_1 == 0x09)
        {
            if (midi_2 == 0x7f)
            {
                wheelTouched = 1;
            }
            else
            {
                wheelTouched = 0;
            }
        }

        if (shiftHeld && volumeTouched && knob8touched && !alreadyLaunched)
        {
            alreadyLaunched = 1;
            printf("Launching Move Anything!\n");
            launchChildAndKillThisProcess("/data/UserData/move-anything/start.sh", "start.sh", "");
        }

        /* Shadow mode toggle: Shift + Volume + Knob 1 */
        /* Debug: Log hotkey state every so often */
        {
            static int hotkey_log_counter = 0;
            static FILE *hotkey_debug = NULL;
            if (hotkey_log_counter++ % 500 == 0) {
                if (!hotkey_debug) {
                    hotkey_debug = fopen("/data/UserData/move-anything/hotkey_debug.log", "a");
                }
                if (hotkey_debug) {
                    fprintf(hotkey_debug, "hotkey #%d: shift=%d vol=%d knob1=%d knob8=%d debounce=%d\n",
                            hotkey_log_counter, shiftHeld, volumeTouched, knob1touched, knob8touched, shadowModeDebounce);
                    fflush(hotkey_debug);
                }
            }
        }

        if (shiftHeld && volumeTouched && knob1touched && !shadowModeDebounce)
        {
            shadowModeDebounce = 1;
            log_hotkey_state("toggle");
            if (shadow_control) {
                shadow_control->display_mode = !shadow_control->display_mode;
                printf("Shadow mode toggled: %s\n",
                       shadow_control->display_mode ? "SHADOW" : "STOCK");
            }
        }

        /* Reset debounce once any part of the combo is released */
        if (shadowModeDebounce && (!shiftHeld || !volumeTouched || !knob1touched))
        {
            shadowModeDebounce = 0;
            log_hotkey_state("debounce_reset");
        }

        printf("move-anything: cable: %x,\tcode index number:%x,\tmidi_0:%x,\tmidi_1:%x,\tmidi_2:%x\n", cable, code_index_number, midi_0, midi_1, midi_2);
    }
}

// unsigned long ioctlCounter = 0;
int ioctl(int fd, unsigned long request, char *argp)
{
    if (!real_ioctl)
    {
        real_ioctl = dlsym(RTLD_NEXT, "ioctl");
        if (!real_ioctl)
        {
            fprintf(stderr, "Error: dlsym failed to find ioctl\n");
            exit(1);
        }
    }

    // print_mem();
    // write_mem();

    // TODO: Consider using move-anything host code and quickjs for flexibility
    midi_monitor();

    /* === SHADOW INSTRUMENT: PRE-IOCTL PROCESSING === */
    /* Forward MIDI BEFORE ioctl - hardware clears the buffer during transaction */
    shadow_forward_midi();

    /* Mix shadow audio into mailbox BEFORE hardware transaction */
    shadow_mix_audio();

#if SHADOW_INPROCESS_POC
    shadow_inprocess_process_midi();
    shadow_inprocess_mix_audio();
#endif

    /* Swap display if in shadow mode BEFORE hardware transaction */
    shadow_swap_display();

    /* === HARDWARE TRANSACTION === */
    int result = real_ioctl(fd, request, argp);

    return result;
}
