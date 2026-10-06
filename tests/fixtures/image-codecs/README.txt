rgb-19x7.jxr is Summit's own generated RGB test pattern, encoded independently
with Debian-patched JxrEncApp 1.2 using lossless quality (-q 1). No third-party
artwork. Each pixel at (x, y) has R=(x*11)%256, G=(y*27)%256, B=(x*y*7)%256.
It checks the 24-bit RGB path independently of Summit's RGBA encoder.

quadrants-32x16.jxl and quadrants-32x16.heic are Summit's generated RGBA
pattern. The four 16x8 quadrants are opaque red, opaque green, half-alpha blue,
and transparent white. JPEG XL uses libjxl 0.7.0 through FFmpeg with distance=0;
HEIC uses libheif 1.23.4 through pillow-heif 1.8.0 with quality=-1/chroma=444.
HEIC's YUV conversion decodes the three visible colors to (254,0,0), (0,255,1),
and (0,0,254); alpha is 255,255,128,0. Browser checks allow three color levels
for conversion and premultiplication rounding. No third-party artwork.
