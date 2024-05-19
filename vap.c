// Copyright 2021 Josh Bailey (josh@vandervecken.com)

// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "vessel.h"
#include <6502.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

const char VAP_VERSION[] = "VAP" VERSION;

#define CLOCK_ACK VW(0xF8)

#define RUN_BUFFER 0xc000
#define REU_COMMAND (*((volatile unsigned char *)0xdf01))
#define REU_CONTROL (*((volatile unsigned char *)0xdf0a))
#define REU_HOST_BASE ((volatile unsigned char *)0xdf02)
#define REU_ADDR_BASE ((volatile unsigned char *)0xdf04)
#define REU_TRANSFER_LEN ((volatile unsigned char *)0xdf07)
#define VICII ((volatile unsigned char *)0xd011)
#define SCREENMEM ((volatile unsigned char *)0x0400)
#define BORDERCOLOR ((volatile unsigned char *)0xd020)
#define SIDBASE ((volatile unsigned char *)0xd400)
#define SIDBASE2 ((volatile unsigned char *)0xd420)
#define SIDREGSIZE 28
#define FIX_REU_ADDRESS 0x40
#define FIX_HOST_ADDRESS 0x80

#define SYSEX_START 0xf0
#define ASID_MANID 0x2d
#define SYSEX_STOP 0xf7

enum ASID_CMD {
  ASID_CMD_START = 0x4c,
  ASID_CMD_STOP = 0x4d,
  ASID_CMD_UPDATE = 0x4e,
  ASID_CMD_UPDATE2 = 0x50,
  ASID_CMD_UPDATE_BOTH = 0x51,
  ASID_CMD_RUN_BUFFER = 0x52,
  ASID_CMD_LOAD_BUFFER = 0x53,
  ASID_CMD_ADDR_BUFFER = 0x54,
  ASID_CMD_LOAD_RECT_BUFFER = 0x55,
  ASID_CMD_ADDR_RECT_BUFFER = 0x56,
  ASID_CMD_FILL_BUFFER = 0x57,
  ASID_CMD_FILL_RECT_BUFFER = 0x58,
  ASID_CMD_COPY_BUFFER = 0x59,
  ASID_CMD_COPY_RECT_BUFFER = 0x5a,
  ASID_CMD_REU_STASH_BUFFER = 0x5b,
  ASID_CMD_REU_FETCH_BUFFER = 0x5c,
  ASID_CMD_REU_FILL_BUFFER = 0x5d,
  ASID_CMD_REU_STASH_BUFFER_RECT = 0x5e,
  ASID_CMD_REU_FETCH_BUFFER_RECT = 0x5f,
  ASID_CMD_REU_FILL_BUFFER_RECT = 0x60
  // TODO: REU fetch to rectangle.
};

struct {
  unsigned char start; // number of positions to skip to new row (e.g. 40)
  unsigned char size;  // size of a row
  unsigned char inc;   // number of rows to increment
} rectconfig;

struct {
  unsigned char val;
  uint16_t count;
} fillconfig;

struct {
  unsigned char *from;
  uint16_t count;
} copyconfig;

struct {
  unsigned char *hostaddr;
  struct {
    unsigned char reubase[3];
    uint16_t count;
  } reurect;
} reushadow;

unsigned char buf[256] = {};
unsigned char cmd = 0;
unsigned char flagp = 0;
unsigned char msbp = 0;
unsigned char lsbp = 0;
unsigned char regidflags = 0;
unsigned char msbs = 0;
unsigned char val = 0;
unsigned char writep = 0;
unsigned char readp = 0;
unsigned char cmdp = 0;
unsigned char ch = 0;
unsigned char i = 0;
unsigned char loadmsb = 0;
unsigned char loadmask = 0;
unsigned char col = 0;
uint16_t j = 0;

volatile unsigned char *bufferaddr = (volatile unsigned char *)RUN_BUFFER;
volatile unsigned char *loadbuffer = 0;
volatile unsigned char *rowloadbuffer = 0;

void noop() {}

void (*datahandler)(void) = &noop;
void (*stophandler)(void) = &noop;

const unsigned char regidmap[] = {
    0, // ID 0
    1, // ID 1
    2, // ID 2
    3, // ID 3
    5, // ID 4
    6, // ID 5
    7, // ID 6

    8,  // ID 7
    9,  // ID 8
    10, // ID 9
    12, // ID 10
    13, // ID 11
    14, // ID 12
    15, // ID 13

    16, // ID 14
    17, // ID 15
    19, // ID 16
    20, // ID 17
    21, // ID 18
    22, // ID 19
    23, // ID 20

    24, // ID 21
    4,  // ID 22
    11, // ID 23
    18, // ID 24
    4,  // ID 25
    11, // ID 26
    18, // ID 27
};

#define REGMASK(base, mask, regid)                                             \
  if (regidflags & mask) {                                                     \
    val = buf[lsbp++];                                                         \
    if (msbs & mask) {                                                         \
      val |= 0x80;                                                             \
    }                                                                          \
    base[regidmap[regid]] = val;                                               \
  }

#define BYTEREG(base, regid)                                                   \
  regidflags = buf[flagp++];                                                   \
  msbs = buf[msbp++];                                                          \
  if (regidflags) {                                                            \
    REGMASK(base, 1, regid + 0);                                               \
    REGMASK(base, 2, regid + 1);                                               \
    REGMASK(base, 4, regid + 2);                                               \
    REGMASK(base, 8, regid + 3);                                               \
    REGMASK(base, 16, regid + 4);                                              \
    REGMASK(base, 32, regid + 5);                                              \
    REGMASK(base, 64, regid + 6);                                              \
  }

#define UPDATESID(base)                                                        \
  flagp = cmdp + 1;                                                            \
  msbp = flagp + 4;                                                            \
  lsbp = flagp + 8;                                                            \
  BYTEREG(base, 0);                                                            \
  BYTEREG(base, 7);                                                            \
  BYTEREG(base, 14);                                                           \
  BYTEREG(base, 21);

inline void rect_skip() {
  if (!--col) {
    col = rectconfig.inc;
    while (col--) {
      rowloadbuffer += rectconfig.start;
    }
    col = rectconfig.size;
    loadbuffer = rowloadbuffer;
  }
}

inline void rect_init() {
  col = rectconfig.size;
  rowloadbuffer = loadbuffer;
}

inline void handle_load_ch(void (*const x)(void)) {
  if (loadmsb) {
    if (loadmask & 0x01) {
      ch |= 0x80;
    }
    loadmask >>= 1;
    *loadbuffer = ch;
    ++loadbuffer;
    --loadmsb;
    if (x) {
      x();
    }
  } else {
    loadmsb = 7;
    loadmask = ch;
  }
}

inline void handle_fill_buffer(void (*const x)(void), void (*const y)(void)) {
  j = fillconfig.count;
  loadbuffer = bufferaddr;
  if (x) {
    x();
  }
  while (j--) {
    *(loadbuffer++) = fillconfig.val;
    if (y) {
      y();
    }
  }
}

inline void handle_copy_buffer(void (*const x)(void), void (*const y)(void)) {
  loadbuffer = bufferaddr;
  if (x) {
    x();
  }
  while (copyconfig.count--) {
    *(loadbuffer++) = *(copyconfig.from++);
    if (y) {
      y();
    }
  }
}

void reufetch() { REU_COMMAND = 0b10010001; }

void reustash() { REU_COMMAND = 0b10010000; }

inline void manage_reurect(void (*const x)(void)) {
  // transfer length must be a multiple of rectconfig.size
  j = reushadow.reurect.count;
  memcpy((void *)REU_HOST_BASE, &reushadow, sizeof(reushadow));
  while (j) {
    *REU_TRANSFER_LEN = rectconfig.size;
    x();
    j -= rectconfig.size;
    i = rectconfig.inc;
    while (i--) {
      *REU_HOST_BASE += rectconfig.start;
    }
  }
}

void reustashrect() { manage_reurect(reustash); }

void reufetchrect() { manage_reurect(reufetch); }

void (*const asidstopcmdhandler[])(void);

void asidstop() {
  (*asidstopcmdhandler[cmd])();
  CLOCK_ACK;
  datahandler = &noop;
  stophandler = &noop;
}

void setasidstop() { stophandler = &asidstop; }

void initsid(void) {
  for (i = 0; i < SIDREGSIZE; ++i) {
    SIDBASE[i] = 0;
    SIDBASE2[i] = 0;
  }
}

void indirect(void) { asm("jmp (bufferaddr)"); }

void updatesid() { UPDATESID(SIDBASE); }

void updatesid2() { UPDATESID(SIDBASE2); }

void updatebothsid() {
  updatesid();
  updatesid2();
}

void fillbuffer() { handle_fill_buffer(NULL, NULL); }

void fillrectbuffer() { handle_fill_buffer(&rect_init, &rect_skip); }

void copybuffer() { handle_copy_buffer(NULL, NULL); }

void copyrectbuffer() { handle_copy_buffer(&rect_init, &rect_skip); }

void handle_load() { handle_load_ch(NULL); }

void handle_rect_load() { handle_load_ch(&rect_skip); }

void start_handle_load() {
  datahandler = &handle_load;
  loadbuffer = bufferaddr;
  setasidstop();
}

void start_handle_load_rect() {
  datahandler = &handle_load;
  loadbuffer = bufferaddr;
  datahandler = &handle_rect_load;
  rect_init();
  setasidstop();
}

void start_handle_addr() {
  datahandler = &handle_load;
  loadbuffer = (unsigned char *)&bufferaddr;
  setasidstop();
}

void start_handle_addr_rect() {
  datahandler = &handle_load;
  loadbuffer = (unsigned char *)&rectconfig;
  setasidstop();
}

inline void handle_reu_rect(uint8_t control) {
  datahandler = &handle_load;
  loadbuffer = (unsigned char *)&reushadow.reurect;
  REU_CONTROL = control;
  setasidstop();
}

void start_handle_reu_rect() { handle_reu_rect(0); }

void start_handle_reu_fill_rect() { handle_reu_rect(FIX_REU_ADDRESS); }

inline void start_reu(uint8_t control) {
  datahandler = &handle_load;
  loadbuffer = REU_ADDR_BASE;
  REU_CONTROL = control;
  *(uint16_t *)REU_HOST_BASE = (uint16_t)bufferaddr;
  setasidstop();
}

void start_handle_reu() { start_reu(0); }

void start_handle_reu_fill() {
  start_reu(FIX_REU_ADDRESS); // REU address is fixed.
}

void start_handle_copy() {
  datahandler = &handle_load;
  loadbuffer = (unsigned char *)&copyconfig;
  setasidstop();
}

void start_handle_fill() {
  datahandler = &handle_load;
  loadbuffer = (unsigned char *)&fillconfig;
  setasidstop();
}

void (*const asidstartcmdhandler[])(void) = {
    &noop,                       // 0
    &noop,                       // 1
    &noop,                       // 2
    &noop,                       // 3
    &noop,                       // 4
    &noop,                       // 5
    &noop,                       // 6
    &noop,                       // 7
    &noop,                       // 8
    &noop,                       // 9
    &noop,                       // a
    &noop,                       // b
    &noop,                       // c
    &noop,                       // d
    &noop,                       // e
    &noop,                       // f
    &noop,                       // 10
    &noop,                       // 11
    &noop,                       // 12
    &noop,                       // 13
    &noop,                       // 14
    &noop,                       // 15
    &noop,                       // 16
    &noop,                       // 17
    &noop,                       // 18
    &noop,                       // 19
    &noop,                       // 1a
    &noop,                       // 1b
    &noop,                       // 1c
    &noop,                       // 1d
    &noop,                       // 1e
    &noop,                       // 1f
    &noop,                       // 20
    &noop,                       // 21
    &noop,                       // 22
    &noop,                       // 23
    &noop,                       // 24
    &noop,                       // 25
    &noop,                       // 26
    &noop,                       // 27
    &noop,                       // 28
    &noop,                       // 29
    &noop,                       // 2a
    &noop,                       // 2b
    &noop,                       // 2c
    &noop,                       // 2d
    &noop,                       // 2e
    &noop,                       // 2f
    &noop,                       // 30
    &noop,                       // 31
    &noop,                       // 32
    &noop,                       // 33
    &noop,                       // 34
    &noop,                       // 35
    &noop,                       // 36
    &noop,                       // 37
    &noop,                       // 38
    &noop,                       // 39
    &noop,                       // 3a
    &noop,                       // 3b
    &noop,                       // 3c
    &noop,                       // 3d
    &noop,                       // 3e
    &noop,                       // 3f
    &noop,                       // 40
    &noop,                       // 41
    &noop,                       // 42
    &noop,                       // 43
    &noop,                       // 44
    &noop,                       // 45
    &noop,                       // 46
    &noop,                       // 47
    &noop,                       // 48
    &noop,                       // 49
    &noop,                       // 4a
    &noop,                       // 4b
    &setasidstop,                // 4c ASID_CMD_START
    &setasidstop,                // 4d ASID_CMD_STOP
    &setasidstop,                // 4e ASID_CMD_UPDATE
    &noop,                       // 4f
    &setasidstop,                // 50 ASID_CMD_UPDATE2
    &setasidstop,                // 51 ASID_CMD_UPDATE_BOTH
    &setasidstop,                // 52 ASID_CMD_RUN_BUFFER
    &start_handle_load,          // 53 ASID_CMD_LOAD_BUFFER
    &start_handle_addr,          // 54 ASID_CMD_ADDR_BUFFER
    &start_handle_load_rect,     // 55 ASID_CMD_LOAD_RECT_BUFFER
    &start_handle_addr_rect,     // 56 ASID_CMD_ADDR_RECT_BUFFER
    &start_handle_fill,          // 57 ASID_CMD_FILL_BUFFER
    &start_handle_fill,          // 58 ASID_CMD_FILL_RECT_BUFFER
    &start_handle_copy,          // 59 ASID_CMD_COPY_BUFFER
    &start_handle_copy,          // 5a ASID_CMD_COPY_RECT_BUFFER
    &start_handle_reu,           // 5b ASID_CMD_REU_STASH_BUFFER
    &start_handle_reu,           // 5c ASID_CMD_REU_FETCH_BUFFER
    &start_handle_reu_fill,      // 5d ASID_CMD_REU_FILL_BUFFER
    &start_handle_reu_rect,      // 5e ASID_CMD_REU_STASH_BUFFER_RECT
    &start_handle_reu_rect,      // 5f ASID_CMD_REU_FETCH_BUFFER_RECT
    &start_handle_reu_fill_rect, // 60 ASID_CMD_REU_FILL_BUFFER_RECT
    &noop,                       // 61
    &noop,                       // 62
    &noop,                       // 63
    &noop,                       // 64
    &noop,                       // 65
    &noop,                       // 66
    &noop,                       // 67
    &noop,                       // 68
    &noop,                       // 69
    &noop,                       // 6a
    &noop,                       // 6b
    &noop,                       // 6c
    &noop,                       // 6d
    &noop,                       // 6e
    &noop,                       // 6f
    &noop,                       // 70
    &noop,                       // 71
    &noop,                       // 72
    &noop,                       // 73
    &noop,                       // 74
    &noop,                       // 75
    &noop,                       // 76
    &noop,                       // 77
    &noop,                       // 78
    &noop,                       // 79
    &noop,                       // 7a
    &noop,                       // 7b
    &noop,                       // 7c
    &noop,                       // 7d
    &noop,                       // 7e
    &noop,                       // 7f
};

void (*const asidstopcmdhandler[])(void) = {
    &noop,           // 0
    &noop,           // 1
    &noop,           // 2
    &noop,           // 3
    &noop,           // 4
    &noop,           // 5
    &noop,           // 6
    &noop,           // 7
    &noop,           // 8
    &noop,           // 9
    &noop,           // a
    &noop,           // b
    &noop,           // c
    &noop,           // d
    &noop,           // e
    &noop,           // f
    &noop,           // 10
    &noop,           // 11
    &noop,           // 12
    &noop,           // 13
    &noop,           // 14
    &noop,           // 15
    &noop,           // 16
    &noop,           // 17
    &noop,           // 18
    &noop,           // 19
    &noop,           // 1a
    &noop,           // 1b
    &noop,           // 1c
    &noop,           // 1d
    &noop,           // 1e
    &noop,           // 1f
    &noop,           // 20
    &noop,           // 21
    &noop,           // 22
    &noop,           // 23
    &noop,           // 24
    &noop,           // 25
    &noop,           // 26
    &noop,           // 27
    &noop,           // 28
    &noop,           // 29
    &noop,           // 2a
    &noop,           // 2b
    &noop,           // 2c
    &noop,           // 2d
    &noop,           // 2e
    &noop,           // 2f
    &noop,           // 30
    &noop,           // 31
    &noop,           // 32
    &noop,           // 33
    &noop,           // 34
    &noop,           // 35
    &noop,           // 36
    &noop,           // 37
    &noop,           // 38
    &noop,           // 39
    &noop,           // 3a
    &noop,           // 3b
    &noop,           // 3c
    &noop,           // 3d
    &noop,           // 3e
    &noop,           // 3f
    &noop,           // 40
    &noop,           // 41
    &noop,           // 42
    &noop,           // 43
    &noop,           // 44
    &noop,           // 45
    &noop,           // 46
    &noop,           // 47
    &noop,           // 48
    &noop,           // 49
    &noop,           // 4a
    &noop,           // 4b
    &initsid,        // 4c ASID_CMD_START
    &initsid,        // 4d ASID_CMD_STOP
    &updatesid,      // 4e ASID_CMD_UPDATE
    &noop,           // 4f
    &updatesid2,     // 50 ASID_CMD_UPDATE2
    &updatebothsid,  // 51 ASID_CMD_UPDATE_BOTH
    &indirect,       // 52 ASID_CMD_RUN_BUFFER
    &noop,           // 53 ASID_CMD_LOAD_BUFFER
    &noop,           // 54 ASID_CMD_ADDR_BUFFER
    &noop,           // 55 ASID_CMD_LOAD_RECT_BUFFER
    &noop,           // 56 ASID_CMD_ADDR_RECT_BUFFER
    &fillbuffer,     // 57 ASID_CMD_FILL_BUFFER
    &fillrectbuffer, // 58 ASID_CMD_FILL_RECT_BUFFER
    &copybuffer,     // 59 ASID_CMD_COPY_BUFFER
    &copyrectbuffer, // 5a ASID_CMD_COPY_RECT_BUFFER
    &reustash,       // 5b ASID_CMD_REU_STASH_BUFFER
    &reufetch,       // 5c ASID_CMD_REU_FETCH_BUFFER
    &reufetch,       // 5d ASID_CMD_REU_FILL_BUFFER
    &reustashrect,   // 5e ASID_CMD_REU_STASH_BUFFER_RECT
    &reufetchrect,   // 5f ASID_CMD_REU_FETCH_BUFFER_RECT
    &reufetchrect,   // 60 ASID_CMD_REU_FILL_BUFFER_RECT
    &noop,           // 61
    &noop,           // 62
    &noop,           // 63
    &noop,           // 64
    &noop,           // 65
    &noop,           // 66
    &noop,           // 67
    &noop,           // 68
    &noop,           // 69
    &noop,           // 6a
    &noop,           // 6b
    &noop,           // 6c
    &noop,           // 6d
    &noop,           // 6e
    &noop,           // 6f
    &noop,           // 70
    &noop,           // 71
    &noop,           // 72
    &noop,           // 73
    &noop,           // 74
    &noop,           // 75
    &noop,           // 76
    &noop,           // 77
    &noop,           // 78
    &noop,           // 79
    &noop,           // 7a
    &noop,           // 7b
    &noop,           // 7c
    &noop,           // 7d
    &noop,           // 7e
    &noop,           // 7f
};

void initvessel(void) {
  VOUT;
  VRESET;
  VIN;
  VOUT;
  VCMD(4); // config passthrough
  VW(4);
}

void init(void) {
  asm("jsr $e544"); // clear screen
  initsid();
  initvessel();
  memset(&rectconfig, 0, sizeof(rectconfig));
  memset(&fillconfig, 0, sizeof(fillconfig));
  memset(&copyconfig, 0, sizeof(copyconfig));
  memset(&reushadow, 0, sizeof(reushadow));
  const char *c = VAP_VERSION;
  while (*c) {
    putchar(*c++);
  }
  SEI();
}

void handle_cmd() {
  cmd = ch;
  cmdp = readp;
  loadmsb = 0;
  datahandler = &noop;
  stophandler = &noop;
  (*asidstartcmdhandler[cmd])();
}

void handle_manid() {
  if (ch == ASID_MANID) {
    datahandler = &handle_cmd;
  } else {
    datahandler = &noop;
  }
}

void midiloop(void) {
  for (;;) {
    VIN;
    for (i = VR; i; --i) {
      buf[++writep] = VR;
    }
    VOUT;
    while (writep != readp) {
      ch = buf[++readp];
      if (ch & 0x80) {
        switch (ch) {
        case SYSEX_STOP:
          (*stophandler)();
          break;
        case SYSEX_START:
          datahandler = &handle_manid;
          break;
        }
      } else {
        datahandler();
      }
    }
  }
}

int main(void) {
  init();
  midiloop();
  return 0;
}
