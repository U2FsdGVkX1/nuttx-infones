/****************************************************************************
 * apps/games/infones/infones_main.cpp
 * The only platform-facing file in this port.  InfoNES core is unmodified.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <cctype>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <unistd.h>
#ifdef CONFIG_GAMES_INFONES_CONSOLE
#  include <termios.h>
#endif

#include <nuttx/clock.h>
#include <nuttx/video/fb.h>

#ifdef CONFIG_GAMES_INFONES_KBDDEV
#  include <nuttx/input/keyboard.h>
#endif

#ifdef CONFIG_GAMES_INFONES_JOYDEV
#  include <nuttx/input/djoystick.h>
#endif

#ifdef CONFIG_GAMES_INFONES_AUDIODEV
#  include <mqueue.h>
#  include <nuttx/audio/audio.h>
#endif

#include "InfoNES.h"
#include "InfoNES_System.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define AUDIO_BUFFERS       3
#define CONSOLE_HOLD_FRAMES 8                   /* Terminals send no releases */
#define FRAME_NSEC          (NSEC_PER_SEC / 60) /* NTSC is about 60 frames/s */

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* InfoNES shifts the pad out LSB first, in NES controller order. */

enum
{
  NES_A      = 1 << 0,
  NES_B      = 1 << 1,
  NES_SELECT = 1 << 2,
  NES_START  = 1 << 3,
  NES_UP     = 1 << 4,
  NES_DOWN   = 1 << 5,
  NES_LEFT   = 1 << 6,
  NES_RIGHT  = 1 << 7,
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static int g_fbfd = -1;
static uint8_t *g_pixels;
static fb_videoinfo_s g_vinfo;
static fb_planeinfo_s g_pinfo;
static fb_area_s g_view;
static DWORD g_system;
#ifdef CONFIG_GAMES_INFONES_KBDDEV
static int g_kbdfd = -1;
#endif
#ifdef CONFIG_GAMES_INFONES_JOYDEV
static int g_joyfd = -1;
#endif
#ifdef CONFIG_GAMES_INFONES_CONSOLE
static bool g_console;
static int g_stdinflags;
static termios g_termios;
#endif

#ifdef CONFIG_GAMES_INFONES_AUDIODEV
static int g_audiofd = -1;
static mqd_t g_mq = (mqd_t)-1;
static char g_mqname[32];
static ap_buffer_s *g_abuf[AUDIO_BUFFERS];
static bool g_busy[AUDIO_BUFFERS];
static bool g_started;
static int g_samples;
#  ifdef CONFIG_AUDIO_MULTI_SESSION
static void *g_session;
#  endif
#endif

/****************************************************************************
 * Public Data
 ****************************************************************************/

/* InfoNES renders 5:5:5 RGB.  Keep the upstream palette in its native
 * format and convert only at the device boundary.
 */

WORD NesPalette[64] =
{
  0x39ce, 0x1071, 0x0015, 0x2013, 0x440e, 0x5402, 0x5000, 0x3c20,
  0x20a0, 0x0100, 0x0140, 0x00e2, 0x0ceb, 0x0000, 0x0000, 0x0000,
  0x5ef7, 0x01dd, 0x10fd, 0x401e, 0x5c17, 0x700b, 0x6ca0, 0x6521,
  0x45c0, 0x0240, 0x02a0, 0x0247, 0x0211, 0x0000, 0x0000, 0x0000,
  0x7fff, 0x1eff, 0x2e5f, 0x223f, 0x79ff, 0x7dd6, 0x7dcc, 0x7e67,
  0x7ae7, 0x4342, 0x2769, 0x2ff3, 0x03bb, 0x0000, 0x0000, 0x0000,
  0x7fff, 0x579f, 0x635f, 0x6b3f, 0x7f1f, 0x7f1b, 0x7ef6, 0x7f75,
  0x7f94, 0x73f4, 0x57d7, 0x5bf9, 0x4ffe, 0x0000, 0x0000, 0x0000
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int display_open(const char *path)
{
  g_fbfd = open(path, O_RDWR);
  if (g_fbfd < 0 ||
      ioctl(g_fbfd, FBIOGET_VIDEOINFO, &g_vinfo) < 0 ||
      ioctl(g_fbfd, FBIOGET_PLANEINFO, &g_pinfo) < 0)
    {
      perror(path);
      return -1;
    }

  bool supported = (g_vinfo.fmt == FB_FMT_RGB16_555 && g_pinfo.bpp == 16) ||
                   (g_vinfo.fmt == FB_FMT_RGB16_565 && g_pinfo.bpp == 16) ||
                   (g_vinfo.fmt == FB_FMT_RGB32 && g_pinfo.bpp == 32);

  if (!supported)
    {
      fprintf(stderr, "infones: unsupported pixel format %d\n", g_vinfo.fmt);
      return -1;
    }

  /* The visible area, plane offset included, must fit in the mapping */

  size_t right = g_pinfo.xoffset + g_vinfo.xres;
  size_t bottom = g_pinfo.yoffset + g_vinfo.yres;

  if (g_vinfo.xres == 0 || g_vinfo.yres == 0 ||
      right * (g_pinfo.bpp / 8) > g_pinfo.stride ||
      bottom * g_pinfo.stride > g_pinfo.fblen)
    {
      fprintf(stderr, "infones: unsupported framebuffer geometry\n");
      return -1;
    }

  /* Use the largest whole-number scale that fits, preserving sharp pixels.
   * On smaller framebuffers, fit the image while preserving its aspect
   * ratio.
   */

  unsigned scale = MIN(g_vinfo.xres / NES_DISP_WIDTH,
                       g_vinfo.yres / NES_DISP_HEIGHT);
  unsigned w = g_vinfo.xres;
  unsigned h = g_vinfo.yres;

  if (scale)
    {
      w = NES_DISP_WIDTH * scale;
      h = NES_DISP_HEIGHT * scale;
    }
  else if (w * NES_DISP_HEIGHT > h * NES_DISP_WIDTH)
    {
      w = h * NES_DISP_WIDTH / NES_DISP_HEIGHT;
    }
  else
    {
      h = w * NES_DISP_HEIGHT / NES_DISP_WIDTH;
    }

  if (!w || !h)
    {
      fprintf(stderr, "infones: framebuffer too small\n");
      return -1;
    }

  g_view.x = (g_vinfo.xres - w) / 2;
  g_view.y = (g_vinfo.yres - h) / 2;
  g_view.w = w;
  g_view.h = h;

  g_pixels = (uint8_t *)mmap(nullptr, g_pinfo.fblen,
                             PROT_READ | PROT_WRITE, MAP_SHARED, g_fbfd, 0);
  if (g_pixels == MAP_FAILED)
    {
      g_pixels = nullptr;
      perror("infones: mmap framebuffer");
      return -1;
    }

  return 0;
}

static void display_close()
{
  if (g_pixels)
    {
      munmap(g_pixels, g_pinfo.fblen);
      g_pixels = nullptr;
    }

  if (g_fbfd >= 0)
    {
      close(g_fbfd);
      g_fbfd = -1;
    }
}

/* Pace to NTSC, whether or not audio is available.  When running late, or
 * implausibly far ahead, restart the schedule instead of sleeping.
 */

static void pace_frame()
{
  static int64_t deadline;
  timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);

  int64_t now = (int64_t)ts.tv_sec * NSEC_PER_SEC + ts.tv_nsec;

  deadline += FRAME_NSEC;
  if (deadline <= now || deadline - now >= 100 * NSEC_PER_MSEC)
    {
      deadline = now;
      return;
    }

  ts.tv_sec = 0;
  ts.tv_nsec = deadline - now;
  while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
    {
    }
}

#ifdef CONFIG_GAMES_INFONES_KBDDEV
static DWORD keyboard_poll()
{
  static DWORD pad;
  keyboard_event_s event;

  while (read(g_kbdfd, &event, sizeof(event)) == sizeof(event))
    {
      DWORD bit = 0;

      if (event.type == KBD_SPECPRESS || event.type == KBD_SPECREL)
        {
          switch (event.code)
            {
              case KEYCODE_UP:
                bit = NES_UP;
                break;

              case KEYCODE_DOWN:
                bit = NES_DOWN;
                break;

              case KEYCODE_LEFT:
                bit = NES_LEFT;
                break;

              case KEYCODE_RIGHT:
                bit = NES_RIGHT;
                break;

              default:
                break;
            }
        }
      else
        {
          switch (event.code)
            {
              case 'z':
              case 'Z':
                bit = NES_A;
                break;

              case 'x':
              case 'X':
                bit = NES_B;
                break;

              case 'a':
              case 'A':
                bit = NES_SELECT;
                break;

              case 's':
              case 'S':
                bit = NES_START;
                break;

              case 'q':
              case 'Q':
                if (event.type == KBD_PRESS)
                  {
                    g_system |= PAD_SYS_QUIT;
                  }
                break;

              default:
                break;
            }
        }

      if (event.type == KBD_PRESS || event.type == KBD_SPECPRESS)
        {
          pad |= bit;
        }
      else
        {
          pad &= ~bit;
        }
    }

  return pad;
}
#endif

#ifdef CONFIG_GAMES_INFONES_CONSOLE
static void console_open()
{
  termios raw;

  if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &g_termios) < 0)
    {
      return;
    }

  g_stdinflags = fcntl(STDIN_FILENO, F_GETFL);
  if (g_stdinflags < 0)
    {
      return;
    }

  raw = g_termios;
  raw.c_lflag &= ~(ICANON | ECHO);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 0;
  if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0)
    {
      if (fcntl(STDIN_FILENO, F_SETFL, g_stdinflags | O_NONBLOCK) == 0)
        {
          g_console = true;
        }
      else
        {
          tcsetattr(STDIN_FILENO, TCSANOW, &g_termios);
        }
    }
}

static void console_close()
{
  if (g_console)
    {
      tcsetattr(STDIN_FILENO, TCSANOW, &g_termios);
      fcntl(STDIN_FILENO, F_SETFL, g_stdinflags);
      g_console = false;
    }
}

static DWORD console_poll()
{
  static const char keys[] = "kjuiwsad";   /* NES_A..NES_RIGHT, one per bit */
  static uint8_t hold[sizeof(keys) - 1];   /* Frames each key stays down */
  unsigned char ch;
  DWORD pad = 0;

  while (read(STDIN_FILENO, &ch, 1) == 1)
    {
      ch = tolower(ch);
      if (ch == 'q')
        {
          g_system |= PAD_SYS_QUIT;
        }

      for (unsigned i = 0; i < sizeof(hold); ++i)
        {
          if (ch == keys[i])
            {
              hold[i] = CONSOLE_HOLD_FRAMES;
            }
        }
    }

  for (unsigned i = 0; i < sizeof(hold); ++i)
    {
      if (hold[i])
        {
          pad |= (DWORD)1 << i;
          --hold[i];
        }
    }

  return pad;
}
#endif

#ifdef CONFIG_GAMES_INFONES_AUDIODEV
static unsigned long audio_session()
{
#ifdef CONFIG_AUDIO_MULTI_SESSION
  return (unsigned long)(uintptr_t)g_session;
#else
  return 0;
#endif
}

static audio_buf_desc_s audio_desc()
{
  audio_buf_desc_s desc;

  memset(&desc, 0, sizeof(desc));
#ifdef CONFIG_AUDIO_MULTI_SESSION
  desc.session = g_session;
#endif
  return desc;
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void InfoNES_LoadFrame()
{
  size_t bpp = g_pinfo.bpp / 8;
  size_t left = (g_pinfo.xoffset + g_view.x) * bpp;
  size_t top = g_pinfo.yoffset + g_view.y;

  /* Nearest-neighbour scale the NES screen into the view */

  for (unsigned y = 0; y < g_view.h; ++y)
    {
      uint8_t *row = g_pixels + (top + y) * g_pinfo.stride + left;
      unsigned sy = y * NES_DISP_HEIGHT / g_view.h;
      const WORD *src = &WorkFrame[sy * NES_DISP_WIDTH];

      for (unsigned x = 0; x < g_view.w; ++x)
        {
          /* Bit 15 marks backdrop pixels for sprite priority, not colour */

          WORD c = src[x * NES_DISP_WIDTH / g_view.w] & 0x7fff;

          if (g_vinfo.fmt == FB_FMT_RGB32)
            {
              uint32_t rgb = ((uint32_t)(c & 0x7c00) << 9) |
                             ((uint32_t)(c & 0x03e0) << 6) |
                             ((uint32_t)(c & 0x001f) << 3);

              memcpy(row + x * bpp, &rgb, sizeof(rgb));
            }
          else
            {
              uint16_t rgb = c;

              if (g_vinfo.fmt == FB_FMT_RGB16_565)
                {
                  /* Green widens to 6 bits: move red and green up one */

                  rgb = ((c & 0x7fe0) << 1) | (c & 0x001f);
                }

              memcpy(row + x * bpp, &rgb, sizeof(rgb));
            }
        }
    }

  ioctl(g_fbfd, FBIO_UPDATE, &g_view);
  pace_frame();
}

void InfoNES_PadState(DWORD *pad1, DWORD *pad2, DWORD *system)
{
  *pad1 = 0;
  *pad2 = 0;
#ifdef CONFIG_GAMES_INFONES_CONSOLE
  if (g_console)
    {
      *pad1 |= console_poll();
    }
#endif

#ifdef CONFIG_GAMES_INFONES_KBDDEV
  if (g_kbdfd >= 0)
    {
      *pad1 |= keyboard_poll();
    }
#endif

#ifdef CONFIG_GAMES_INFONES_JOYDEV
  if (g_joyfd >= 0)
    {
      /* Keep the last state if a read fails */

      static DWORD joypad;
      djoy_buttonset_t buttons;

      if (read(g_joyfd, &buttons, sizeof(buttons)) == sizeof(buttons))
        {
          joypad = ((buttons & DJOY_UP_BIT) ? NES_UP : 0) |
                   ((buttons & DJOY_DOWN_BIT) ? NES_DOWN : 0) |
                   ((buttons & DJOY_LEFT_BIT) ? NES_LEFT : 0) |
                   ((buttons & DJOY_RIGHT_BIT) ? NES_RIGHT : 0) |
                   ((buttons & DJOY_BUTTON_1_BIT) ? NES_SELECT : 0) |
                   ((buttons & DJOY_BUTTON_2_BIT) ? NES_START : 0) |
                   ((buttons & DJOY_BUTTON_3_BIT) ? NES_A : 0) |
                   ((buttons & DJOY_BUTTON_4_BIT) ? NES_B : 0);
        }

      *pad1 |= joypad;
    }
#endif

  *system = g_system;
}

int InfoNES_Menu()
{
  return (g_system & PAD_SYS_QUIT) ? -1 : 0;
}

int InfoNES_ReadRom(const char *path)
{
  NesHeader_tag header;
  size_t trainer;
  size_t prgsize;
  size_t chrsize;
  FILE *file;

  file = fopen(path, "rb");
  if (!file)
    {
      perror(path);
      return -1;
    }

  /* iNES 1.0 only (NES 2.0 sets bits 2-3 of byInfo2 to 10): a 16-byte
   * header, an optional 512-byte trainer, PRG-ROM in 16 KiB banks, then
   * CHR-ROM in 8 KiB banks.
   */

  if (fread(&header, sizeof(header), 1, file) != 1 ||
      memcmp(header.byID, "NES\x1a", 4) != 0 ||
      (header.byInfo2 & 0x0c) == 0x08 ||
      header.byRomSize == 0)
    {
      goto invalid;
    }

  trainer = (header.byInfo1 & 0x04) ? 512 : 0;
  prgsize = header.byRomSize * 0x4000;
  chrsize = header.byVRomSize * 0x2000;

  ROM = (BYTE *)malloc(prgsize);
  VROM = chrsize ? (BYTE *)malloc(chrsize) : nullptr;
  if (!ROM || (chrsize && !VROM))
    {
      fprintf(stderr, "infones: out of memory\n");
      goto errout;
    }

  /* SRAM starts at $6000 and the trainer belongs at $7000 */

  NesHeader = header;
  memset(SRAM, 0, SRAM_SIZE);
  if (fread(SRAM + 0x1000, 1, trainer, file) != trainer ||
      fread(ROM, 1, prgsize, file) != prgsize ||
      (chrsize && fread(VROM, 1, chrsize, file) != chrsize))
    {
      goto invalid;
    }

  fclose(file);
  return 0;

invalid:
  fprintf(stderr, "infones: invalid or truncated iNES ROM: %s\n", path);
errout:
  InfoNES_ReleaseRom();
  fclose(file);
  return -1;
}

void InfoNES_ReleaseRom()
{
  free(ROM);
  free(VROM);
  ROM = nullptr;
  VROM = nullptr;
}

void *InfoNES_MemoryCopy(void *dst, const void *src, int n)
{
  return memcpy(dst, src, n);
}

void *InfoNES_MemorySet(void *dst, int c, int n)
{
  return memset(dst, c, n);
}

void InfoNES_DebugPrint(char *msg)
{
  fputs(msg, stderr);
}

void InfoNES_MessageBox(char *fmt, ...)
{
  va_list ap;

  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
}

void InfoNES_Wait()
{
}

void InfoNES_SoundInit()
{
  APU_Mute = 1;
}

#ifdef CONFIG_GAMES_INFONES_AUDIODEV
/* Every failure path ends here, so audio just stays muted */

void InfoNES_SoundClose()
{
  if (g_audiofd >= 0)
    {
      if (g_started)
        {
          ioctl(g_audiofd, AUDIOIOC_STOP, audio_session());
          g_started = false;
        }

      if (g_mq != (mqd_t)-1)
        {
          ioctl(g_audiofd, AUDIOIOC_UNREGISTERMQ, (unsigned long)g_mq);
        }

      for (int i = 0; i < AUDIO_BUFFERS; ++i)
        {
          if (g_abuf[i])
            {
              audio_buf_desc_s desc = audio_desc();

              desc.u.buffer = g_abuf[i];
              ioctl(g_audiofd, AUDIOIOC_FREEBUFFER, &desc);
              g_abuf[i] = nullptr;
            }

          g_busy[i] = false;
        }

      ioctl(g_audiofd, AUDIOIOC_RELEASE, audio_session());
      close(g_audiofd);
      g_audiofd = -1;
    }

  if (g_mq != (mqd_t)-1)
    {
      mq_close(g_mq);
      mq_unlink(g_mqname);
      g_mq = (mqd_t)-1;
    }

  APU_Mute = 1;
}

int InfoNES_SoundOpen(int samples_per_sync, int sample_rate)
{
  audio_caps_desc_s caps;
  mq_attr attr;
  int ret;

  if (samples_per_sync <= 0 || samples_per_sync > 2048)
    {
      return 0;
    }

  g_audiofd = open(CONFIG_GAMES_INFONES_AUDIODEV, O_RDWR);
  if (g_audiofd < 0)
    {
      return 0;
    }

#ifdef CONFIG_AUDIO_MULTI_SESSION
  ret = ioctl(g_audiofd, AUDIOIOC_RESERVE, &g_session);
#else
  ret = ioctl(g_audiofd, AUDIOIOC_RESERVE, 0);
#endif
  if (ret < 0)
    {
      goto errout;
    }

  memset(&caps, 0, sizeof(caps));
#ifdef CONFIG_AUDIO_MULTI_SESSION
  caps.session = g_session;
#endif
  caps.caps.ac_len = sizeof(caps.caps);
  caps.caps.ac_type = AUDIO_TYPE_OUTPUT;
  caps.caps.ac_channels = 1;
  caps.caps.ac_controls.hw[0] = sample_rate; /* Sample rate, low 16 bits */
  caps.caps.ac_controls.b[2] = 16;           /* Bits per sample */
  if (ioctl(g_audiofd, AUDIOIOC_CONFIGURE, &caps) < 0)
    {
      goto errout;
    }

  /* The driver hands finished buffers back through this queue */

  snprintf(g_mqname, sizeof(g_mqname), "/infones-%ld", (long)getpid());
  memset(&attr, 0, sizeof(attr));
  attr.mq_maxmsg = AUDIO_BUFFERS + 2;
  attr.mq_msgsize = sizeof(audio_msg_s);
  g_mq = mq_open(g_mqname, O_CREAT | O_RDONLY | O_NONBLOCK, 0600, &attr);
  if (g_mq == (mqd_t)-1 ||
      ioctl(g_audiofd, AUDIOIOC_REGISTERMQ, (unsigned long)g_mq) < 0)
    {
      goto errout;
    }

  for (int i = 0; i < AUDIO_BUFFERS; ++i)
    {
      audio_buf_desc_s desc = audio_desc();

      desc.numbytes = samples_per_sync * sizeof(int16_t);
      desc.u.pbuffer = &g_abuf[i];
      if (ioctl(g_audiofd, AUDIOIOC_ALLOCBUFFER, &desc) < 0 || !g_abuf[i])
        {
          goto errout;
        }
    }

  g_samples = samples_per_sync;
  APU_Mute = 0;
  return 1;

errout:
  InfoNES_SoundClose();
  return 0;
}

void InfoNES_SoundOutput(int samples, BYTE *a, BYTE *b, BYTE *c,
                         BYTE *d, BYTE *e)
{
  if (g_audiofd < 0 || samples != g_samples)
    {
      return;
    }

  /* Reclaim the buffers the driver has finished playing */

  audio_msg_s msg;

  while (mq_receive(g_mq, (char *)&msg, sizeof(msg), nullptr) == sizeof(msg))
    {
      for (int i = 0; i < AUDIO_BUFFERS; ++i)
        {
          if (msg.msg_id == AUDIO_MSG_DEQUEUE && msg.u.ptr == g_abuf[i])
            {
              g_busy[i] = false;
            }
        }
    }

  /* Drop these samples if every buffer is still queued */

  int slot = 0;

  while (slot < AUDIO_BUFFERS && g_busy[slot])
    {
      ++slot;
    }

  if (slot == AUDIO_BUFFERS)
    {
      return;
    }

  /* Mix the five unsigned 8-bit channels into signed 16-bit PCM */

  ap_buffer_s *buffer = g_abuf[slot];
  int16_t *pcm = (int16_t *)buffer->samp;

  for (int i = 0; i < samples; ++i)
    {
      int mix = (a[i] + b[i] + c[i] + d[i] + e[i]) / 5;

      pcm[i] = (int16_t)((mix - 128) * 256);
    }

  buffer->nbytes = samples * sizeof(int16_t);
  buffer->curbyte = 0;
  buffer->flags = 0;

  audio_buf_desc_s desc = audio_desc();

  desc.numbytes = buffer->nbytes;
  desc.u.buffer = buffer;
  if (ioctl(g_audiofd, AUDIOIOC_ENQUEUEBUFFER, &desc) < 0)
    {
      InfoNES_SoundClose();
      return;
    }

  g_busy[slot] = true;
  if (!g_started && ioctl(g_audiofd, AUDIOIOC_START, audio_session()) < 0)
    {
      InfoNES_SoundClose();
      return;
    }

  g_started = true;
}
#else
int InfoNES_SoundOpen(int, int)
{
  return 0;
}

void InfoNES_SoundClose()
{
}

void InfoNES_SoundOutput(int, BYTE *, BYTE *, BYTE *, BYTE *, BYTE *)
{
}
#endif

extern "C" int main(int argc, char **argv)
{
  if (argc != 2)
    {
      fprintf(stderr, "usage: %s <rom.nes>\n", argv[0]);
      return EXIT_FAILURE;
    }

  /* Static data survives between runs in a flat build: drop a stale quit */

  g_system = 0;
  if (display_open(CONFIG_GAMES_INFONES_FBDEV) < 0)
    {
      display_close();
      return EXIT_FAILURE;
    }

#ifdef CONFIG_GAMES_INFONES_KBDDEV
  g_kbdfd = open(CONFIG_GAMES_INFONES_KBDDEV, O_RDONLY | O_NONBLOCK);
#endif

#ifdef CONFIG_GAMES_INFONES_JOYDEV
  g_joyfd = open(CONFIG_GAMES_INFONES_JOYDEV, O_RDONLY | O_NONBLOCK);
#endif

#ifdef CONFIG_GAMES_INFONES_CONSOLE
  console_open();
#endif

  InfoNES_Init();
  int status = EXIT_FAILURE;

  if (InfoNES_Load(argv[1]) == 0)
    {
      while (InfoNES_Menu() == 0)
        {
          InfoNES_Cycle();
        }

      status = EXIT_SUCCESS;
    }

  InfoNES_Fin();
#ifdef CONFIG_GAMES_INFONES_KBDDEV
  if (g_kbdfd >= 0)
    {
      close(g_kbdfd);
    }
#endif

#ifdef CONFIG_GAMES_INFONES_JOYDEV
  if (g_joyfd >= 0)
    {
      close(g_joyfd);
    }
#endif

#ifdef CONFIG_GAMES_INFONES_CONSOLE
  console_close();
#endif
  display_close();
  return status;
}
