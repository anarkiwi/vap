# vmp
Vessel ASID Player
==================

VAP enables remote access to SID registers over MIDI SysEx messages, using the [ASID](http://paulus.kapsi.fi/asid_protocol.txt)
protocol, which was introduced with the Elektron SIDStation.  Any SID player supporting ASID can use this
application with Vessel as a C64 hardware sound source.


Sample applications
-------------------

* [Inesid](https://inesid.fazibear.me/) - Chrome browser based (plays slowly, same behavior on a real SIDStation)
* [AsidXP](http://www.elektron.se/support/) - works under Wine on Ubuntu 20.04 (after libasound2-plugins:i386)
