/*
 * fbcap: read a region of the screen's frame buffer many times in a row.
 * Read-only; made from the X399 fork's ScanoutTest.cpp.
 *
 * usage: fbcap x y w h step count interval_ms file
 *   Every step-th pixel of every step-th row of the region is copied, count
 *   times, at least interval_ms apart. The file gets a text header line
 *   "fbcap W H COUNT\n", then per frame an int64 system_time() and W*H
 *   pixels of 4 bytes (B_RGB32).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <OS.h>
#include <smmintrin.h>
#include <GraphicsDefs.h>

#include <ErrorUtils.h>
#include <NvRmApi.h>
#include <NvRmDevice.h>

extern "C" {
#include "nv-haiku.h"
}


int main(int argc, char **argv)
{
	if (argc != 9) {
		fprintf(stderr, "usage: fbcap x y w h step count interval_ms file\n");
		return 1;
	}
	int x = atoi(argv[1]), y = atoi(argv[2]), w = atoi(argv[3]), h = atoi(argv[4]);
	int step = atoi(argv[5]), count = atoi(argv[6]);
	bigtime_t interval = atoi(argv[7]) * 1000LL;
	const char *path = argv[8];

	int ctlFd = open("/dev/" NVIDIA_CONTROL_DEVICE_NAME, O_RDWR | O_CLOEXEC);
	if (ctlFd < 0) {
		perror("opening the control device");
		return 1;
	}
	nv_haiku_scanout_info info {};
	if (ioctl(ctlFd, NV_HAIKU_BASE + NV_HAIKU_GET_SCANOUT, &info, sizeof(info)) < 0) {
		perror("asking for the frame buffer");
		return 1;
	}
	if (x < 0 || y < 0 || step < 1 || x + w > (int)info.width || y + h > (int)info.height) {
		fprintf(stderr, "region outside the %ux%u frame buffer\n", (unsigned)info.width, (unsigned)info.height);
		return 1;
	}
	int outW = (w + step - 1) / step, outH = (h + step - 1) / step;
	size_t frameBytes = (size_t)outW * outH * 4;
	uint8 *frames = (uint8*)malloc(frameBytes * count);
	bigtime_t *times = (bigtime_t*)malloc(sizeof(bigtime_t) * count);
	if (frames == NULL || times == NULL) {
		fprintf(stderr, "no memory\n");
		return 1;
	}
	memset(frames, 0, frameBytes * count);

	try {
		NvRmApi rm;
		NvRmDevice rmDev(rm, 0);
		NvRmObject framebuffer(rm, rm.DupObject(rmDev.Device().Get(), info.client, info.memory));
		NvRmMemoryMapping mapping = rmDev.MapMemory(framebuffer.Get(), false, 0, info.size, 0);
		const uint8 *bits = (const uint8*)mapping.Address();

		uint8 *rowBuffer = (uint8*)aligned_alloc(64, ((size_t)w * 4 + 191) & ~(size_t)63);
		bigtime_t total = 0;
		for (int i = 0; i < count; i++) {
			bigtime_t start = system_time();
			times[i] = start;
			uint32 *out = (uint32*)(frames + frameBytes * i);
			for (int row = 0; row < outH; row++) {
				// The frame buffer is write-combined memory: ordinary loads
				// fetch every access anew, streaming loads take a cache line
				// at a time.
				const uint8 *line = bits + (uint64)(y + row * step) * info.bytes_per_row;
				size_t first = ((size_t)x * 4) & ~(size_t)63;
				size_t last = (((size_t)(x + w) * 4) + 63) & ~(size_t)63;
				for (size_t offset = first; step == 1 && offset < last; offset += 16) {
					_mm_store_si128((__m128i*)(rowBuffer + (offset - first)),
						_mm_stream_load_si128((__m128i*)(line + offset)));
				}
				// Every load costs the same, so a sparse sample reads directly.
				const uint32 *in = step == 1 ? (const uint32*)(rowBuffer + ((size_t)x * 4 - first)) : (const uint32*)line + x;
				if (step == 1) {
					memcpy(out, in, (size_t)outW * 4);
					out += outW;
				} else {
					for (int column = 0; column < outW; column++)
						*out++ = in[column * step];
				}
			}
			bigtime_t took = system_time() - start;
			total += took;
			if (took < interval)
				snooze(interval - took);
		}
		printf("%d frames of %dx%d, %.1f ms each to read\n", count, outW, outH, total / 1000.0 / count);
	} catch (const std::system_error &ex) {
		fprintf(stderr, "[!] %s\n", ex.what());
		return 1;
	}

	FILE *file = fopen(path, "wb");
	if (file == NULL) {
		perror("opening the output file");
		return 1;
	}
	fprintf(file, "fbcap %d %d %d\n", outW, outH, count);
	for (int i = 0; i < count; i++) {
		fwrite(&times[i], sizeof(bigtime_t), 1, file);
		fwrite(frames + frameBytes * i, 1, frameBytes, file);
	}
	fclose(file);
	return 0;
}
