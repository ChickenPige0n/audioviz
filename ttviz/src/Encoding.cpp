#include "Main.hpp"

#include <audioviz/media/FfmpegEncoder.hpp>
#include <audioviz/media/FfmpegPopenEncoder.hpp>
#include "ChronoTimer.hpp"

#include <fstream>
#include <libloaderapi.h>
#include <memory>
#include <stdio.h>
#include <stdlib.h>
#include <errhandlingapi.h>

// Structure to hold encoder state
struct EncoderState {
    void* cuContext;
    void* cudaResource;
    void* encoder;
    int width;
    int height;
    bool initialized;
};

// 更新函数指针定义，配合新的 API 设计
typedef EncoderState *(*CreateEncoderFunc)(int, int, int);
typedef int (*RegisterTextureFunc)(EncoderState *, unsigned int);
typedef int (*EncodeFrameFunc)(EncoderState *, unsigned char **, int *);
typedef int (*DestroyEncoderFunc)(EncoderState *, unsigned char **, int *);
typedef void (*FreePacketFunc)(unsigned char *);

// Function pointers
CreateEncoderFunc gcne_create_encoder = nullptr;
RegisterTextureFunc gcne_register_texture = nullptr;
EncodeFrameFunc gcne_encode_frame = nullptr;
DestroyEncoderFunc gcne_destroy_encoder = nullptr;
FreePacketFunc gcne_free_packet = nullptr;

EncoderState* encoder = nullptr;
// we'll send the h264 packet data to this pipe
FILE *nvenc_ffmpeg_pipe = nullptr;

int setup_encoder(unsigned int width, unsigned int height, unsigned int textureId){
    // In main(), add code to load the DLL
    HMODULE dllHandle = LoadLibraryA("GlCudaNvEncoder.dll");
    if (!dllHandle) {
        DWORD error = GetLastError();
        fprintf(stderr, "Failed to load DLL, error code: %lu\n", error);
        return -1;
    }

    gcne_create_encoder =
        (CreateEncoderFunc)GetProcAddress(dllHandle, "gcne_create_encoder");
    if (!gcne_create_encoder) {
        fprintf(stderr, "Failed to get gcne_create_encoder, error: %lu\n",
                GetLastError());
    }

    gcne_register_texture = (RegisterTextureFunc)GetProcAddress(
        dllHandle, "gcne_register_texture");
    if (!gcne_register_texture) {
        fprintf(stderr, "Failed to get gcne_register_texture, error: %lu\n",
                GetLastError());
    }

    gcne_encode_frame =
        (EncodeFrameFunc)GetProcAddress(dllHandle, "gcne_encode_frame");
    if (!gcne_encode_frame) {
        fprintf(stderr, "Failed to get gcne_encode_frame, error: %lu\n",
                GetLastError());
    }

    gcne_destroy_encoder = (DestroyEncoderFunc)GetProcAddress(
        dllHandle, "gcne_destroy_encoder");
    if (!gcne_destroy_encoder) {
        fprintf(stderr, "Failed to get gcne_destroy_encoder, error: %lu\n",
                GetLastError());
    }
    gcne_free_packet =
        (FreePacketFunc)GetProcAddress(dllHandle, "gcne_free_packet");
    if (!gcne_free_packet) {
        fprintf(stderr, "Failed to get gcne_free_packet, error: %lu\n",
                GetLastError());
    }

    // Check if all functions were found
    if (!gcne_create_encoder || !gcne_register_texture ||
        !gcne_encode_frame || !gcne_destroy_encoder || !gcne_free_packet) {
        fprintf(stderr, "Failed to get function pointers from DLL\n");
        FreeLibrary(dllHandle);
        return -1;
    }

	encoder = gcne_create_encoder(width, height, 0); // third gpu id param needs to be adjusted



	printf("Registering texture...\n");
	if (gcne_register_texture(encoder, textureId) != 0) {
		fprintf(stderr, "Failed to register texture\n");
		gcne_destroy_encoder(encoder, nullptr, nullptr);
		return -1;
	}

	char ffmpeg_cmd[512];
	snprintf(ffmpeg_cmd, sizeof(ffmpeg_cmd),
			 "ffmpeg -y -f h264 -i - "
			 "-c:v copy -r 60 nvenc_output.mp4 "
			 "-loglevel error -stats "
			 "2>ffmpeg_nvenc_log.txt");

	printf("Starting FFmpeg for NVENC with command: %s\n", ffmpeg_cmd);
	nvenc_ffmpeg_pipe = _popen(ffmpeg_cmd, "wb");
	if (!nvenc_ffmpeg_pipe) {
		fprintf(stderr, "Failed to open FFmpeg pipe for NVENC\n");
		gcne_destroy_encoder(encoder, nullptr, nullptr);
		return -1;
	}
	return 0;
}


void Main::encode(audioviz::Base &viz, const std::string &outfile, const std::string &vcodec, const std::string &acodec)
{
	// const auto ffmpeg = std::make_unique<audioviz::FfmpegPopenEncoder>(viz, outfile, vcodec, acodec);
	int framecount = 0;
	ChronoTimer timer("Encoding using nvenc");

	if (enc_window)
	{
		sf::RenderWindow window{
			sf::VideoMode{viz.size},
			"audioviz - encoding...",
			sf::Style::Titlebar,
			sf::State::Windowed,
			{.antiAliasingLevel = 4},
		};
		sf::Texture txr{viz.size};
		setup_encoder(viz.size.x, viz.size.y, txr.getNativeHandle()); // 0 is the texture ID, needs to be set correctly

		while (viz.next_frame())
		{
			window.draw(viz);
			window.display();
			txr.update(window);
			// ffmpeg->send_frame(txr);

			// 使用更新后的 API 编码帧
			unsigned char *packet_data = nullptr;
			int packet_size = 0;

			if (gcne_encode_frame(encoder, &packet_data, &packet_size) != 0) {
				fprintf(stderr, "Failed to encode frame\n");
				break;
			}

			// 如果有数据，发送到 FFmpeg 管道
			if (packet_data && packet_size > 0) {
				fwrite(packet_data, 1, packet_size, nvenc_ffmpeg_pipe);
				fflush(nvenc_ffmpeg_pipe);

				// 使用新的 API 释放数据包内存
				gcne_free_packet(packet_data);
			}

		}
	}
	else
	{
		audioviz::RenderTexture rt{viz.size, 4};
		setup_encoder(viz.size.x, viz.size.y, rt.getTexture().getNativeHandle()); // 0 is the texture ID, needs to be set correctly

		while (viz.next_frame())
		{
			rt.draw(viz);
			rt.display();
			// ffmpeg->send_frame(rt.getTexture());


			// 使用更新后的 API 编码帧
			unsigned char *packet_data = nullptr;
			int packet_size = 0;

			if (gcne_encode_frame(encoder, &packet_data, &packet_size) != 0) {
				fprintf(stderr, "Failed to encode frame\n");
				break;
			}
			fprintf(stdout, "Encoding frame %d\r", framecount);
			framecount ++;
			// 如果有数据，发送到 FFmpeg 管道
			if (packet_data && packet_size > 0) {
				fwrite(packet_data, 1, packet_size, nvenc_ffmpeg_pipe);
				fflush(nvenc_ffmpeg_pipe);

				// 使用新的 API 释放数据包内存
				gcne_free_packet(packet_data);
			}

		}
	}

	// 获取最后的编码数据
	unsigned char *packet_data = nullptr;
	int packet_size = 0;

	// 销毁编码器并获取最后的数据包
	printf("Destroying encoder...\n");
	gcne_destroy_encoder(encoder, &packet_data, &packet_size);

	// 如果有最后的数据，发送到 FFmpeg
	if (packet_data && packet_size > 0) {
		fwrite(packet_data, 1, packet_size, nvenc_ffmpeg_pipe);
		fflush(nvenc_ffmpeg_pipe);

		// 释放内存
		gcne_free_packet(packet_data);
	}
	fprintf(stdout, "Encoding elapsed: %d ms, total fps: %f\n", (int)timer.tillNow(), (float)framecount / (timer.tillNow() / 1000.0f));
	// 关闭 FFmpeg 管道
	printf("Closing FFmpeg pipe for NVENC...\n");
	_pclose(nvenc_ffmpeg_pipe);
}
