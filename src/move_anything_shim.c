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

/* Shared memory segment names */
#define SHM_SHADOW_AUDIO   "/move-shadow-audio"
#define SHM_SHADOW_MIDI    "/move-shadow-midi"
#define SHM_SHADOW_DISPLAY "/move-shadow-display"
#define SHM_SHADOW_CONTROL "/move-shadow-control"

/* Shadow control structure - shared between shim and shadow process */
typedef struct {
    volatile uint8_t display_mode;    /* 0=stock Move, 1=shadow display */
    volatile uint8_t shadow_ready;    /* 1 when shadow process is running */
    volatile uint8_t should_exit;     /* 1 to signal shadow to exit */
    volatile uint8_t midi_ready;      /* increments when new MIDI available */
    volatile uint8_t reserved[60];    /* padding for future use */
} shadow_control_t;

/* Shadow shared memory pointers */
static int16_t *shadow_audio_shm = NULL;
static uint8_t *shadow_midi_shm = NULL;
static uint8_t *shadow_display_shm = NULL;
static shadow_control_t *shadow_control = NULL;

/* Shadow shared memory file descriptors */
static int shm_audio_fd = -1;
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

    /* Create/open audio shared memory */
    shm_audio_fd = shm_open(SHM_SHADOW_AUDIO, O_CREAT | O_RDWR, 0666);
    if (shm_audio_fd >= 0) {
        ftruncate(shm_audio_fd, AUDIO_BUFFER_SIZE);
        shadow_audio_shm = (int16_t *)mmap(NULL, AUDIO_BUFFER_SIZE,
                                            PROT_READ | PROT_WRITE,
                                            MAP_SHARED, shm_audio_fd, 0);
        if (shadow_audio_shm == MAP_FAILED) {
            shadow_audio_shm = NULL;
            printf("Shadow: Failed to mmap audio shm\n");
        } else {
            memset(shadow_audio_shm, 0, AUDIO_BUFFER_SIZE);
        }
    } else {
        printf("Shadow: Failed to create audio shm\n");
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

    /* Create/open control shared memory */
    shm_control_fd = shm_open(SHM_SHADOW_CONTROL, O_CREAT | O_RDWR, 0666);
    if (shm_control_fd >= 0) {
        ftruncate(shm_control_fd, CONTROL_BUFFER_SIZE);
        shadow_control = (shadow_control_t *)mmap(NULL, CONTROL_BUFFER_SIZE,
                                                   PROT_READ | PROT_WRITE,
                                                   MAP_SHARED, shm_control_fd, 0);
        if (shadow_control == MAP_FAILED) {
            shadow_control = NULL;
            printf("Shadow: Failed to mmap control shm\n");
        } else {
            memset(shadow_control, 0, CONTROL_BUFFER_SIZE);
        }
    } else {
        printf("Shadow: Failed to create control shm\n");
    }

    shadow_shm_initialized = 1;
    printf("Shadow: Shared memory initialized (audio=%p, midi=%p, display=%p, control=%p)\n",
           shadow_audio_shm, shadow_midi_shm, shadow_display_shm, shadow_control);
}

/* Mix shadow audio into mailbox audio buffer */
static void shadow_mix_audio(void)
{
    if (!shadow_audio_shm || !global_mmap_addr) return;
    if (!shadow_control || !shadow_control->shadow_ready) return;

    int16_t *mailbox_audio = (int16_t *)(global_mmap_addr + AUDIO_OUT_OFFSET);

    /* Mix shadow audio into mailbox with saturation */
    for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
        int32_t mixed = (int32_t)mailbox_audio[i] + (int32_t)shadow_audio_shm[i];
        if (mixed > 32767) mixed = 32767;
        if (mixed < -32768) mixed = -32768;
        mailbox_audio[i] = (int16_t)mixed;
    }
}

/* Copy incoming MIDI from mailbox to shadow shared memory */
static void shadow_forward_midi(void)
{
    if (!shadow_midi_shm || !global_mmap_addr) return;
    if (!shadow_control) return;

    /* Copy MIDI data from mailbox to shared memory */
    memcpy(shadow_midi_shm, global_mmap_addr + MIDI_IN_OFFSET, MIDI_BUFFER_SIZE);

    /* Signal that new MIDI is available */
    shadow_control->midi_ready++;
}

/* Swap display buffer if in shadow mode */
static void shadow_swap_display(void)
{
    if (!shadow_display_shm || !global_mmap_addr) return;
    if (!shadow_control || !shadow_control->shadow_ready) return;
    if (!shadow_control->display_mode) return;  /* Not in shadow mode */

    /* Overwrite mailbox display with shadow display */
    memcpy(global_mmap_addr + DISPLAY_OFFSET, shadow_display_shm, DISPLAY_BUFFER_SIZE);
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
                }
                else
                {
                    printf("Shift off\n");

                    shiftHeld = 0;
                }
            }

        }

        if (midi_0 == 0x90 && midi_1 == 0x07)
        {
            if (midi_2 == 0x7f)
            {
                knob8touched = 1;
                printf("Knob 8 touch start\n");
            }
            else
            {
                knob8touched = 0;
                printf("Knob 8 touch stop\n");
            }
        }

        /* Knob 1 touch detection (Note 0) - for shadow mode toggle */
        if (midi_0 == 0x90 && midi_1 == 0x00)
        {
            if (midi_2 == 0x7f)
            {
                knob1touched = 1;
                printf("Knob 1 touch start\n");
            }
            else
            {
                knob1touched = 0;
                printf("Knob 1 touch stop\n");
            }
        }

        if (midi_0 == 0x90 && midi_1 == 0x08)
        {
            if (midi_2 == 0x7f)
            {
                volumeTouched = 1;
            }
            else
            {
                volumeTouched = 0;
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
        if (shiftHeld && volumeTouched && knob1touched && !shadowModeDebounce)
        {
            shadowModeDebounce = 1;
            if (shadow_control) {
                shadow_control->display_mode = !shadow_control->display_mode;
                printf("Shadow mode toggled: %s\n",
                       shadow_control->display_mode ? "SHADOW" : "STOCK");
            }
        }

        /* Reset debounce when shift released */
        if (!shiftHeld)
        {
            shadowModeDebounce = 0;
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
    /* Mix shadow audio into mailbox BEFORE hardware transaction */
    shadow_mix_audio();

    /* Swap display if in shadow mode BEFORE hardware transaction */
    shadow_swap_display();

    /* === HARDWARE TRANSACTION === */
    int result = real_ioctl(fd, request, argp);

    /* === SHADOW INSTRUMENT: POST-IOCTL PROCESSING === */
    /* Forward incoming MIDI to shadow process AFTER hardware writes it */
    shadow_forward_midi();

    return result;
}
