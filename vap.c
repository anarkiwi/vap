// Copyright 2021 Josh Bailey (josh@vandervecken.com)

// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#include <6502.h>
#include <stdio.h>
#include "vessel.h"

#define VICII           ((unsigned char*)0xd011)
#define SCREENMEM       ((unsigned char*)0x0400)
#define BORDERCOLOR     ((unsigned char*)0xd020)
#define SIDBASE         ((unsigned char*)0xd400)
#define SIDREGSIZE      25

#define SYSEX_START     0xf0
#define SYSEX_MANID     0x2d
#define SYSEX_STOP      0xf7

#define ASID_CMD_START  0x4c
#define ASID_CMD_STOP   0x4d
#define ASID_CMD_UPDATE 0x4e

unsigned char buf[256] = {};
const unsigned char regidmap[] = {
  0, // ID 0
  1, // ID 1
  2, // ID 2
  3, // ID 3
  5, // ID 4
  6, // ID 5
  7, // ID 6

  8, // ID 7
  9, // ID 8
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
  4, // ID 22
  11, // ID 23
  18, // ID 24
};


void initvessel(void) {
  VOUT;
  VCMD(0); // reset
  VCMD(4); // config passthrough
  VW(4);
}

void initsid(void) {
  register unsigned char i = 0;
  for (i = 0; i < SIDREGSIZE; ++i) {
    *(SIDBASE + i) = 0;
  }
}

void init(void) {
  initsid();
  initvessel();
}

#define REGMASK(mask, regid)  \
  if (regidflags & mask) { \
    val = buf[lsbp]; \
    if (msbs & mask) { \
      val |= 0x80; \
    } \
    *(SIDBASE + regidmap[regid]) = val; \
    ++lsbp; \
  }

#define BYTEREG(regid) \
    regidflags = buf[flagp++]; \
    msbs = buf[msbp++]; \
    REGMASK(1, regid + 0); \
    REGMASK(2, regid + 1); \
    REGMASK(4, regid + 2); \
    REGMASK(8, regid + 3); \
    REGMASK(16, regid + 4); \
    REGMASK(32, regid + 5); \
    REGMASK(64, regid + 6);


void handlesysex(unsigned char cmdp) {
  register unsigned char cmd = buf[cmdp];
  register unsigned char flagp = cmdp + 1;
  register unsigned char msbp = flagp + 4;
  register unsigned char lsbp = flagp + 8;
  register unsigned char regidflags;
  register unsigned char msbs;
  register unsigned char val;

  if (cmd == ASID_CMD_UPDATE) {
    BYTEREG(0);
    BYTEREG(7);
    BYTEREG(14);
    BYTEREG(21);
  } else if (cmd == ASID_CMD_START || cmd == ASID_CMD_STOP) {
    initsid();
  }
}

void midiloop(void) {
  register unsigned char writep = 0;
  register unsigned char readp = 0;
  register unsigned char cmdp = 0;
  register unsigned char expected_ch = SYSEX_START;
  register unsigned char ch = 0;
  register unsigned char i = 0;

  for (;;) {
    VIN;
    ch = VR;
    if (ch) {
      for (i = 0; i < ch; ++i) {
        buf[++writep] = VR;
      }
      VOUT;
      while (writep != readp) {
        ch = buf[++readp];
        if (expected_ch == SYSEX_STOP) {
           if (ch == expected_ch) {
              handlesysex(cmdp);
              expected_ch = SYSEX_START;
           }
        } else if (expected_ch == ch) {
           if (ch == SYSEX_START) {
             expected_ch = SYSEX_MANID;
           } else if (ch == SYSEX_MANID) {
             expected_ch = SYSEX_STOP;
             cmdp = readp + 1;
           }
        } else {
          expected_ch = SYSEX_START;
        }
      }
    } else {
      VOUT;
    }
  }
}

void main(void) {
  *VICII = 0x0b;
  SEI();
  init();
  midiloop();
}
