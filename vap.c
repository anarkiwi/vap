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
#include <conio.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CLOCK_ACK VW(0xF8)

#define RUN_BUFFER 0x0400
#define VICII ((unsigned char *)0xd011)
#define SCREENMEM ((unsigned char *)0x0400)
#define BORDERCOLOR ((unsigned char *)0xd020)
#define SIDBASE ((unsigned char *)0xd400)
#define SIDBASE2 ((unsigned char *)0xd420)
#define SIDREGSIZE 28

#define SYSEX_START 0xf0
#define ASID_MANID 0x2d
#define SYSEX_STOP 0xf7

#define ASID_CMD_START 0x4c
#define ASID_CMD_STOP 0x4d
#define ASID_CMD_UPDATE 0x4e
#define ASID_CMD_UPDATE2 0x50
#define ASID_CMD_UPDATE_BOTH 0x51
#define ASID_CMD_RUN_BUFFER 0x52
#define ASID_CMD_LOAD_BUFFER 0x53
#define ASID_CMD_ADDR_BUFFER 0x54
#define ASID_CMD_LOAD_RECT_BUFFER 0x55
#define ASID_CMD_ADDR_RECT_BUFFER 0x56
#define ASID_CMD_FILL_BUFFER 0x57
#define ASID_CMD_FILL_RECT_BUFFER 0x58

enum EXPECTED_DATA {
  DATA_ANY = 0,
  DATA_MANID,
  DATA_CMD,
  DATA_LOAD,
  DATA_RECT_LOAD,
};

struct {
  unsigned char start;
  unsigned char size;
  unsigned char inc;
} rectconfig;

struct {
  unsigned char val;
  uint16_t count;
} fillconfig;

unsigned char expected_data = DATA_ANY;
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
unsigned char *bufferaddr = (unsigned char *)RUN_BUFFER;
unsigned char *loadbuffer = 0;
unsigned char loadmask = 0;
unsigned char *rowloadbuffer = 0;
unsigned char col = 0;
uint16_t j = 0;

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

void initvessel(void) {
  VOUT;
  VCMD(0); // reset
  VCMD(4); // config passthrough
  VW(4);
}

void initsid(void) {
  for (i = 0; i < SIDREGSIZE; ++i) {
    SIDBASE[i] = 0;
    SIDBASE2[i] = 0;
  }
}

void init(void) {
  initsid();
  initvessel();
  clrscr();
  bzero(&rectconfig, sizeof(rectconfig));
  bzero(&fillconfig, sizeof(fillconfig));
  cputs("VAP");
  cputs(VERSION);
  SEI();
}

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

#define HANDLE_ASID_CMD                                                        \
  switch (cmd) {                                                               \
  case ASID_CMD_UPDATE:                                                        \
    UPDATESID(SIDBASE);                                                        \
    break;                                                                     \
  case ASID_CMD_UPDATE2:                                                       \
    UPDATESID(SIDBASE2);                                                       \
    break;                                                                     \
  case ASID_CMD_UPDATE_BOTH:                                                   \
    UPDATESID(SIDBASE);                                                        \
    UPDATESID(SIDBASE2);                                                       \
    break;                                                                     \
  case ASID_CMD_RUN_BUFFER:                                                    \
    indirect();                                                                \
    break;                                                                     \
  case ASID_CMD_FILL_BUFFER:                                                   \
    HANDLE_FILL_BUFFER(, );                                                    \
    break;                                                                     \
  case ASID_CMD_FILL_RECT_BUFFER:                                              \
    HANDLE_FILL_RECT_BUFFER;                                                   \
    break;                                                                     \
  case ASID_CMD_START:                                                         \
    initsid();                                                                 \
    break;                                                                     \
  case ASID_CMD_STOP:                                                          \
    initsid();                                                                 \
    break;                                                                     \
  }

#define HANDLE_MIDI_CMD                                                        \
  switch (ch) {                                                                \
  case SYSEX_STOP:                                                             \
    if (cmd) {                                                                 \
      HANDLE_ASID_CMD;                                                         \
      CLOCK_ACK;                                                               \
    }                                                                          \
    cmd = 0;                                                                   \
    expected_data = DATA_ANY;                                                  \
    break;                                                                     \
  case SYSEX_START:                                                            \
    expected_data = DATA_MANID;                                                \
    break;                                                                     \
  }

#define HANDLE_LOAD(x)                                                         \
  if (loadmsb) {                                                               \
    if (loadmask & 0x01) {                                                     \
      ch |= 0x80;                                                              \
    }                                                                          \
    loadmask >>= 1;                                                            \
    *loadbuffer = ch;                                                          \
    ++loadbuffer;                                                              \
    --loadmsb;                                                                 \
    x                                                                          \
  } else {                                                                     \
    loadmsb = 7;                                                               \
    loadmask = ch;                                                             \
  }

#define RECT_SKIP                                                              \
  if (!--col) {                                                                \
    col = rectconfig.inc;                                                      \
    while (col--) {                                                            \
      rowloadbuffer += rectconfig.start;                                       \
    }                                                                          \
    col = rectconfig.size;                                                     \
    loadbuffer = rowloadbuffer;                                                \
  }

#define HANDLE_RECT_LOAD HANDLE_LOAD(RECT_SKIP)

#define HANDLE_FILL_BUFFER(x, y)                                               \
  j = fillconfig.count;                                                        \
  loadbuffer = bufferaddr;                                                     \
  x while (j--) {                                                              \
    *(loadbuffer++) = fillconfig.val;                                          \
    y                                                                          \
  }

#define HANDLE_FILL_RECT_BUFFER                                                \
  HANDLE_FILL_BUFFER(                                                          \
      {                                                                        \
        col = rectconfig.size;                                                 \
        rowloadbuffer = loadbuffer;                                            \
      },                                                                       \
      RECT_SKIP)

#define HANDLE_MIDI_DATA                                                       \
  switch (expected_data) {                                                     \
  case DATA_LOAD:                                                              \
    HANDLE_LOAD();                                                             \
    break;                                                                     \
  case DATA_RECT_LOAD:                                                         \
    HANDLE_RECT_LOAD;                                                          \
    break;                                                                     \
  case DATA_MANID:                                                             \
    if (ch == ASID_MANID) {                                                    \
      expected_data = DATA_CMD;                                                \
    } else {                                                                   \
      expected_data = DATA_ANY;                                                \
    }                                                                          \
    break;                                                                     \
  case DATA_CMD:                                                               \
    cmd = ch;                                                                  \
    cmdp = readp;                                                              \
    loadmsb = 0;                                                               \
    switch (cmd) {                                                             \
    case ASID_CMD_LOAD_BUFFER:                                                 \
      loadbuffer = bufferaddr;                                                 \
      expected_data = DATA_LOAD;                                               \
      break;                                                                   \
    case ASID_CMD_LOAD_RECT_BUFFER:                                            \
      loadbuffer = bufferaddr;                                                 \
      expected_data = DATA_RECT_LOAD;                                          \
      rowloadbuffer = loadbuffer;                                              \
      col = rectconfig.size;                                                   \
      break;                                                                   \
    case ASID_CMD_ADDR_BUFFER:                                                 \
      loadbuffer = (unsigned char *)&bufferaddr;                               \
      expected_data = DATA_LOAD;                                               \
      break;                                                                   \
    case ASID_CMD_ADDR_RECT_BUFFER:                                            \
      loadbuffer = (unsigned char *)&rectconfig;                               \
      expected_data = DATA_LOAD;                                               \
      break;                                                                   \
    case ASID_CMD_FILL_BUFFER:                                                 \
      loadbuffer = (unsigned char *)&fillconfig;                               \
      expected_data = DATA_LOAD;                                               \
      break;                                                                   \
    case ASID_CMD_FILL_RECT_BUFFER:                                            \
      loadbuffer = (unsigned char *)&fillconfig;                               \
      expected_data = DATA_LOAD;                                               \
      break;                                                                   \
    default:                                                                   \
      expected_data = DATA_ANY;                                                \
    }                                                                          \
    break;                                                                     \
  default:                                                                     \
    expected_data = DATA_ANY;                                                  \
  }

void indirect(void) { __asm__("jmp (%v)", bufferaddr); }

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
        HANDLE_MIDI_CMD;
      } else if (expected_data) {
        HANDLE_MIDI_DATA;
      }
    }
  }
}

void main(void) {
  init();
  midiloop();
}
