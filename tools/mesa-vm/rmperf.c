/* Read the GPU's performance state through NVIDIA's resource manager and,
 * when asked, hold a request for the highest one.
 *   rmperf [seconds [boost]]
 * Prints the P-state mask ten times a second (P0 = 0x1 is the fastest,
 * P8 = 0x100 the slowest). With "boost" it issues NV2080_CTRL_CMD_PERF_BOOST
 * (BOOST_TO_MAX, until cleared) first; the request belongs to this client and
 * ends with it.
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "nvRmApi.h"
#include "nvos.h"
#include "nvstatus.h"
#include "class/cl0000.h"
#include "class/cl0080.h"
#include "class/cl2080.h"
#include "ctrl/ctrl0000/ctrl0000gpu.h"
#include "ctrl/ctrl2080/ctrl2080perf.h"

int main(int argc, char** argv)
{
	double seconds = argc > 1 ? atof(argv[1]) : 2;
	int boost = argc > 2 && !strcmp(argv[2], "boost");
	int quiet = argc > 3 && !strcmp(argv[3], "quiet");
	NvRmApi rm = { .fd = open("/dev/nvidiactl", O_RDWR), .nodeName = "/dev/nvidiactl" };
	if (rm.fd < 0) { perror("open"); return 1; }
	nv_ioctl_card_info_t ci[8];
	memset(ci, 0, sizeof(ci));
	NvU32 st = nvRmApiCardInfo(&rm, ci, sizeof(ci));
	if (st) { printf("card info: %#x\n", st); return 1; }
	NvHandle hClient = 0;
	st = nvRmApiAlloc(&rm, 0, &hClient, NV01_ROOT_CLIENT, NULL);
	if (st) { printf("client: %#x\n", st); return 1; }
	rm.hClient = hClient;
	NV0000_CTRL_GPU_GET_ID_INFO_V2_PARAMS id = { .gpuId = ci[0].gpu_id };
	st = nvRmApiControl(&rm, hClient, NV0000_CTRL_CMD_GPU_GET_ID_INFO_V2, &id, sizeof(id));
	if (st) { printf("id info: %#x\n", st); return 1; }
	NV0080_ALLOC_PARAMETERS ap0080 = { .deviceId = id.deviceInstance, .hClientShare = hClient };
	NV2080_ALLOC_PARAMETERS ap2080 = { .subDeviceId = id.subDeviceInstance };
	NvHandle hDevice = 0, hSubdevice = 0;
	st = nvRmApiAlloc(&rm, hClient, &hDevice, NV01_DEVICE_0, &ap0080);
	if (st) { printf("device: %#x\n", st); return 1; }
	st = nvRmApiAlloc(&rm, hDevice, &hSubdevice, NV20_SUBDEVICE_0, &ap2080);
	if (st) { printf("subdevice: %#x\n", st); return 1; }

	NV2080_CTRL_PERF_GET_CURRENT_PSTATE_PARAMS ps = {0};
	st = nvRmApiControl(&rm, hSubdevice, NV2080_CTRL_CMD_PERF_GET_CURRENT_PSTATE, &ps, sizeof(ps));
	printf("current pstate: status %#x mask %#x\n", st, ps.currPstate);
	if (boost) {
		NV2080_CTRL_PERF_BOOST_PARAMS b = { .flags = NV2080_CTRL_PERF_BOOST_FLAGS_CMD_BOOST_TO_MAX, .duration = NV2080_CTRL_PERF_BOOST_DURATION_INFINITE };
		st = nvRmApiControl(&rm, hSubdevice, NV2080_CTRL_CMD_PERF_BOOST, &b, sizeof(b));
		printf("boost to max: status %#x\n", st);
	}
	fflush(stdout);
	NvU32 last = ~0u;
	int samples = (int)(seconds * 10);
	unsigned histogram[16] = {0};
	for (int i = 0; i < samples; i++) {
		usleep(100000);
		memset(&ps, 0, sizeof(ps));
		st = nvRmApiControl(&rm, hSubdevice, NV2080_CTRL_CMD_PERF_GET_CURRENT_PSTATE, &ps, sizeof(ps));
		if (st) { printf("pstate: status %#x\n", st); break; }
		for (int bit = 0; bit < 16; bit++)
			if (ps.currPstate & (1u << bit)) histogram[bit]++;
		if (!quiet && ps.currPstate != last) {
			printf("%6.1f s: pstate mask %#x\n", i / 10.0, ps.currPstate);
			fflush(stdout);
			last = ps.currPstate;
		}
	}
	printf("samples by pstate:");
	for (int bit = 0; bit < 16; bit++)
		if (histogram[bit]) printf(" P%d %u", bit, histogram[bit]);
	printf("\n");
	if (boost) {
		NV2080_CTRL_PERF_BOOST_PARAMS b = { .flags = NV2080_CTRL_PERF_BOOST_FLAGS_CMD_CLEAR, .duration = 0 };
		st = nvRmApiControl(&rm, hSubdevice, NV2080_CTRL_CMD_PERF_BOOST, &b, sizeof(b));
		printf("boost cleared: status %#x\n", st);
	}
	return 0;
}
