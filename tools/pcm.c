#define PROGRAM_NAME "pcm"
#define USAGE_OPTS "[-d|--decompress] [-h|--help] infile.wav outfile.pcm"

#include "common.h"

#define CHUNKID(b1, b2, b3, b4) (uint32_t)((uint32_t)(b1) | ((uint32_t)(b2) << 8) | ((uint32_t)(b3) << 16) | ((uint32_t)(b4) << 24))

#define LE16(n) (uint8_t)((n) & 0xff), (uint8_t)(((n) & 0xff00) >> 8)
#define LE32(n) (uint8_t)((n) & 0xff), (uint8_t)(((n) & 0xff00) >> 8), \
		(uint8_t)(((n) & 0xff0000) >> 16), (uint8_t)(((n) & 0xff000000) >> 24)

void parse_args(int argc, char *argv[], bool *decompress) {
	struct option long_options[] = {
		{"decompress", no_argument, 0, 'd'},
		{"help", no_argument, 0, 'h'},
		{0}
	};
	for (int opt; (opt = getopt_long(argc, argv, "dh", long_options)) != -1;) {
		switch (opt) {
		case 'd':
			*decompress = true;
			break;
		case 'h':
			usage_exit(0);
			break;
		default:
			usage_exit(1);
		}
	}
}

int32_t get_uint16le(uint8_t *data, long size, long i) {
	return i + 2 < size ? (int32_t)data[i] | ((int32_t)data[i + 1] << 8) : -1;
}

int64_t get_uint32le(uint8_t *data, long size, long i) {
	return i + 4 < size ? (int64_t)data[i] | ((int64_t)data[i + 1] << 8) | ((int64_t)data[i + 2] << 16) | ((int64_t)data[i + 3] << 24) : -1;
}

uint8_t *wav2pcm(uint8_t *wavdata, long wavsize, size_t *pcmsize) {
	int64_t fourcc = get_uint32le(wavdata, wavsize, 0);
	if (fourcc != CHUNKID('R', 'I', 'F', 'F')) {
		error_exit("WAV file does not start with 'RIFF'\n");
	}

	int64_t waveid = get_uint32le(wavdata, wavsize, 8);
	if (waveid != CHUNKID('W', 'A', 'V', 'E')) {
		error_exit("RIFF chunk does not start with 'WAVE'\n");
	}

	long sample_offset = 0;
	int64_t num_samples = 0;

	int64_t riffsize = get_uint32le(wavdata, wavsize, 4) + 8;
	for (long i = 12; i < riffsize;) {
		int64_t chunkid = get_uint32le(wavdata, wavsize, i);
		int64_t chunksize = get_uint32le(wavdata, wavsize, i + 4);
		if (chunksize == -1) {
			error_exit("Failed to read sub-chunk size\n");
		}
		i += 8;

		// Require 22050 Hz 8-bit PCM WAV audio
		if (chunkid == CHUNKID('f', 'm', 't', ' ')) {
			int32_t audio_format = get_uint16le(wavdata, wavsize, i);
			if (audio_format != 1) {
				error_exit("WAV data is not PCM format\n");
			}
			int32_t num_channels = get_uint16le(wavdata, wavsize, i + 2);
			if (num_channels != 1) {
				error_exit("WAV data is not mono\n");
			}
			int64_t sample_rate = get_uint32le(wavdata, wavsize, i + 4);
			if (sample_rate != 22050) {
				error_exit("WAV data is not 22050 Hz\n");
			}
			int32_t bits_per_sample = get_uint16le(wavdata, wavsize, i + 14);
			if (bits_per_sample != 8) {
				error_exit("WAV data is not 8-bit\n");
			}
		}

		else if (chunkid == CHUNKID('d', 'a', 't', 'a')) {
			sample_offset = i;
			num_samples = chunksize;
			break;
		}

		i += chunksize;
	}

	if (!num_samples) {
		error_exit("WAV data has no PCM samples\n");
	}

	// Pack 8 WAV samples per PCM byte, clamping each to 0 or 1
	*pcmsize = (size_t)((num_samples + 7) / 8);
	uint8_t *pcmdata = xmalloc(*pcmsize);
	for (int64_t i = 0; i < num_samples; i += 8) {
		uint8_t v = 0;
		for (int64_t j = 0; j < 8 && i + j < num_samples; j++) {
			v |= (wavdata[sample_offset + i + j] > 0x80) << (7 - j);
		}
		pcmdata[i / 8] = v;
	}
	return pcmdata;
}

uint8_t *pcm2wav(uint8_t *pcmdata, long pcmsize, size_t *wavsize) {
	size_t num_samples = pcmsize * 8;
	*wavsize = 44 + num_samples;
	uint8_t *wavdata = xmalloc(*wavsize);

	uint8_t header[44] = {
		'R', 'I', 'F', 'F', // chunk ID "RIFF"
		LE32(*wavsize - 8), // chunk size
		'W', 'A', 'V', 'E', // format "WAVE"
		'f', 'm', 't', ' ', // subchunk ID "fmt "
		LE32(16),           // subchunk size
		LE16(1),            // audio format
		LE16(1),            // num channels
		LE32(22050),        // sample rate
		LE32(22050),        // byte rate
		LE16(1),            // block align
		LE16(8),            // bits per sample
		'd', 'a', 't', 'a', // subchunk ID "data"
		LE32(num_samples)   // subchunk size
	};
	memcpy(wavdata, header, sizeof(header));

	for (size_t i = 0; i < num_samples; i++) {
		uint8_t bit = pcmdata[i / 8] & (1 << (7 - i % 8));
		wavdata[sizeof(header) + i] = bit ? 0xff : 0x00;
	}

	return wavdata;
}

int main(int argc, char *argv[]) {
	bool decompress = false;
	parse_args(argc, argv, &decompress);

	argc -= optind;
	argv += optind;
	if (argc != 2) {
		usage_exit(1);
	}

	long in_size;
	uint8_t *in_data = read_u8(argv[0], &in_size);
	size_t out_size;
	uint8_t *out_data = (decompress ? pcm2wav : wav2pcm)(in_data, in_size, &out_size);
	write_u8(argv[1], out_data, out_size);

	free(in_data);
	free(out_data);
	return 0;
}
