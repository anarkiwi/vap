VERSION := $(shell git describe --tags --abbrev=0)

all: vap.d64 vap.prg

vap.prg: vap.c vessel.h Makefile
	/usr/local/llvm-mos/bin/mos-c64-clang -Os -fnonreentrant -flto -DVERSION=\"${VERSION}\" -o vap.prg vap.c

vap.d64: vap.prg
	c1541 -format diskname,id d64 vap.d64 -attach vap.d64 -write vap.prg vap

clean:
	rm -f vap.prg vap.d64 vap.prg.elf vap.o vap.crt

upload: all
	ncftpput -p "" -Cv c64 vap.prg /Temp/vap.prg

upload-crt: all
	./prg2crt.py vap.prg vap.crt
	ncftpput -p "" -Cv c64 vap.crt /Flash/carts/vap.crt
