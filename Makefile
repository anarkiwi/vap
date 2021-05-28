all: vap.d64 vap.prg

vap.prg: vap.c vessel.h 
	cl65 -Osir -Cl vap.c -o vap.prg

vap.d64: vap.prg
	c1541 -format diskname,id d64 vap.d64 -attach vap.d64 -write vap.prg vap

upload: all
	ncftpput -p "" -Cv c64 vap.prg /Temp/vap.prg
