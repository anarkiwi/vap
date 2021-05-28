// Copyright 2021 Josh Bailey (josh@vandervecken.com)

// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.


#define	PORTA		((unsigned char*)0xdd00)
#define	PORTB		((unsigned char*)0xdd01)
#define	PORTB_DDR	((unsigned char*)0xdd03)

#define SBIT(x)		(x & 0b01111111)

#define VOUT		{ *PORTA |= 0b00000100; *PORTB_DDR = 0xff; }
#define	VIN		{ *PORTA &= 0b11111011; *PORTB_DDR = 0x00; }
#define VW(x)		*PORTB = x
#define VR		*PORTB
#define VR7		SBIT(VR)
#define	VCMD(cmd)	{ VW(0xfd); VW(cmd); }
#define	VRESET		VCMD(0);
