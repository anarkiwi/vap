all: vap.d64 vap.prg

vap.prg: vap.c vessel.h Makefile
	# /usr/local/llvm-mos/bin/mos-c64-clang -O3 -o vap.prg vap.c
	cl65 -Osir -Cl vap.c -o vap.prg

vap.d64: vap.prg
	c1541 -format diskname,id d64 vap.d64 -attach vap.d64 -write vap.prg vap

clean:
	rm -f vap.prg vap.d64 vap.prg.elf

upload: all
	ncftpput -p "" -Cv c64 vap.prg /Temp/vap.prg
