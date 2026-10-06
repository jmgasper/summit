rgb-19x7.jxr is Summit's own generated RGB test pattern, encoded independently
with Debian-patched JxrEncApp 1.2 using lossless quality (-q 1). No third-party
artwork. Each pixel at (x, y) has R=(x*11)%256, G=(y*27)%256, B=(x*y*7)%256.
It checks the 24-bit RGB path independently of Summit's RGBA encoder.
