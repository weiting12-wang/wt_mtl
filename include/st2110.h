
#ifndef WT_SMPTE2110_H
#define WT_SMPTE2110_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	WT_OK = 0,
	WT_ERR_INIT = -1,
	WT_ERR_PARAM = -2,
	WT_ERR_NOMEM = -3,
	WT_ERR_INTERNAL = -4,
	WT_ERR_TIMEOUT = -5
} wt_status_t;

typedef enum {
	WT_PROTOCOL_ST20,
	WT_PROTOCOL_ST22,
	WT_PROTOCOL_ST30,
	WT_PROTOCOL_ST31
} wt_protocol_t;

typedef enum {
	WT_CODEC_RAW,
	WT_CODEC_JPEGXS,
	WT_CODEC_H264,
	WT_CODEC_H265,
	WT_CODEC_PCM,
	WT_CODEC_LPCM,
	// WT_CODEC_TIMECODE,
	// WT_CODEC_SUBTITLE,

	WT_CODEC_UNDEFINED = 0xFFFFFFFF
} wt_codec_t;

typedef enum {
	WT_CONNECTION_STATUS_CONNECTED,
	WT_CONNECTION_STATUS_DISCONNECTED,
} wt_connection_status_t;

/* 不透明 handle 宣告 */
typedef struct wt_tx wt_tx_t; // opaque TX handle
typedef struct wt_rx wt_rx_t; // opaque RX handle


typedef struct {
	wt_codec_t codec;

	void* data;
	size_t size;
	uint64_t timestamp_ns;
} wt_packet_t;

// Rx callback type
typedef void (*wt_rx_connection_status_callback_t)(wt_connection_status_t status, void* user_data);
typedef void (*wt_rx_packet_callback_t)(const wt_packet_t* packet, void* user_data);

//
// TX
//
wt_status_t wt_tx_create(wt_protocol_t proto, const char* port_name, const char* local_ip, const char* dest_ip, int port, wt_tx_t** ppThis);
wt_status_t wt_tx_destroy(wt_tx_t* pThis);

wt_status_t wt_tx_set_video_format(wt_tx_t* pThis, wt_codec_t codec, int width, int height, int fps_num, int fps_den);
wt_status_t wt_tx_set_audio_format(wt_tx_t* pThis, wt_codec_t codec, int bitcount, int channels, int frequency);
// wt_status_t wt_tx_set_timecode_format(wt_tx_t* pThis, wt_codec_t codec, ...);
// wt_status_t wt_tx_set_subtitle_format(wt_tx_t* pThis, wt_codec_t codec, ...);

wt_status_t wt_tx_start(wt_tx_t* pThis);
wt_status_t wt_tx_stop(wt_tx_t* pThis);

wt_status_t wt_tx_send_packet(wt_tx_t* pThis, wt_packet_t* pPacket);

//
// RX
//
wt_status_t wt_rx_create(wt_protocol_t proto, const char* port_name, const char* local_ip, const char* source_ip, int port, wt_rx_t** ppThis);
wt_status_t wt_rx_destroy(wt_rx_t* pThis);

wt_status_t wt_rx_set_connection_status_callback(wt_rx_t* pThis, wt_rx_connection_status_callback_t cb, void* user_data);
wt_status_t wt_rx_set_packet_callback(wt_rx_t* pThis, wt_rx_packet_callback_t cb, void* user_data);

wt_status_t wt_rx_start(wt_rx_t* pThis);
wt_status_t wt_rx_stop(wt_rx_t* pThis);

wt_status_t wt_rx_set_video_format(wt_rx_t* pThis, wt_codec_t codec, int width, int height, int fps_num, int fps_den);
wt_status_t wt_rx_set_audio_format(wt_rx_t* pThis, wt_codec_t codec, int bitcount, int channels, int frequency);

wt_status_t wt_rx_get_video_format(wt_rx_t* pThis, wt_codec_t* pCodec, int* pWidth, int* pHeight, int* pFpsNum, int* pFpsDen);
wt_status_t wt_rx_get_audio_format(wt_rx_t* pThis, wt_codec_t* pCodec, int* pBitCount, int* pChannels, int* pFrequenct);
// wt_status_t wt_rx_get_timecode_format(wt_rt* pThis, wt_codec_t* pCodec, ...);
// wt_status_t wt_rx_get_subtitle_format(wt_rt* pThis, wt_codec_t* pCodec, ...);

#ifdef __cplusplus
}
#endif

#endif // WT_SMPTE2110_H
