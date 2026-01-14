#include "st2110.h"
#include "mtl_api.h"  /* Public MTL API */
#include "st20_api.h"
#include "st30_api.h"
#include "../../app/src/app_platform.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <unistd.h>
#include <errno.h>
#include "st_api.h"  // 確保有 enum st_fps 定義
#include "st30_pipeline_api.h"

static enum st_fps wt_map_fps(int fps_num, int fps_den) {
    if (fps_den == 1) {
        switch (fps_num) {
        case 120: return ST_FPS_P120;
        case 100: return ST_FPS_P100;
        case 60:  return ST_FPS_P60;
        case 50:  return ST_FPS_P50;
        case 30:  return ST_FPS_P30;
        case 25:  return ST_FPS_P25;
        case 24:  return ST_FPS_P24;
        default:  return ST_FPS_MAX; // 無效
        }
    } else if (fps_den == 1001) {
        switch (fps_num) {
        case 60000: return ST_FPS_P59_94;
        case 30000: return ST_FPS_P29_97;
        case 24000: return ST_FPS_P23_98;
        case 119880: return ST_FPS_P119_88;
        default:    return ST_FPS_MAX;
        }
    }
    return ST_FPS_MAX; // 其他情況暫不支援
}

/* 全域 log level 變數 */
static int g_st2110_mtl_log_level = MTL_LOG_LEVEL_ERROR;  // 預設只顯示錯誤
static bool g_st2110_disable_stats = true;                 // 預設關閉統計

/* 設定 log level */
void st2110_set_log_level(st2110_log_level_t level) {
    switch (level) {
        case ST2110_LOG_LEVEL_ERROR:
            g_st2110_mtl_log_level = MTL_LOG_LEVEL_ERROR;
            g_st2110_disable_stats = true;
            break;
        case ST2110_LOG_LEVEL_WARNING:
            g_st2110_mtl_log_level = MTL_LOG_LEVEL_WARNING;
            g_st2110_disable_stats = true;
            break;
        case ST2110_LOG_LEVEL_INFO:
            g_st2110_mtl_log_level = MTL_LOG_LEVEL_INFO;
            g_st2110_disable_stats = false;  // INFO 以上顯示統計
            break;
        case ST2110_LOG_LEVEL_DEBUG:
            g_st2110_mtl_log_level = MTL_LOG_LEVEL_DEBUG;
            g_st2110_disable_stats = false;
            break;
        default:
            g_st2110_mtl_log_level = MTL_LOG_LEVEL_ERROR;
            g_st2110_disable_stats = true;
            break;
    }
}

/* 取得 log level */
st2110_log_level_t st2110_get_log_level(void) {
    switch (g_st2110_mtl_log_level) {
        case MTL_LOG_LEVEL_ERROR:   return ST2110_LOG_LEVEL_ERROR;
        case MTL_LOG_LEVEL_WARNING: return ST2110_LOG_LEVEL_WARNING;
        case MTL_LOG_LEVEL_INFO:    return ST2110_LOG_LEVEL_INFO;
        case MTL_LOG_LEVEL_DEBUG:   return ST2110_LOG_LEVEL_DEBUG;
        default:                     return ST2110_LOG_LEVEL_ERROR;
    }
}



/* ====================== 內部結構 ====================== */
/* 傳送端結構 */
struct wt_tx {
    /* MTL 基本資訊 */
    mtl_handle mtl;      /* MTL handle */
    void* session;       /* ST22 session handle */
    wt_protocol_t proto; /* 協議類型，例如 WT_PROTOCOL_ST22 */
    wt_codec_t codec;    /* 編解碼類型 */
    int width, height;   /* 畫面解析度 */
    int fps_num, fps_den;/* 幀率分數形式，例如 30000/1001 */
    int bitcount;        /* 每像素位元數，例如 8、10 */
    int channels;        /* 音訊通道數（如果有音訊） */
    int frequency;       /* 音訊取樣頻率（如果有音訊） */

    enum st_fps mtl_fps;   /* 新增: MTL API 用的 FPS 枚舉 */


    /* ===== ST30 音訊專用 ===== */
    enum st30_fmt st30_fmt;            /* ST30 音訊格式（PCM16 / PCM24） */
    enum st30_sampling st30_sampling;  /* ST30 取樣率（48K / 96K） */
    enum st30_ptime st30_ptime;        /* packet time，若不指定可為 0 */

    /* 網路參數 */
    char port_name[MTL_PORT_MAX_LEN]; /* 本地 port 名稱，例如 "kernel:eth0" */
    char dest_ip[64];    /* 目標 IP（unicast/multicast） */
    char local_ip[64];   /* 本地綁定 IP（unicast 時必須） */
    int udp_port;                     /* 目標 UDP port */
    int payload_type;                 /* RTP Payload Type */

    /* Buffer / 同步管理 */
    int framebuff_cnt;                /* buffer 數量，例如 8 */
    uint16_t framebuff_producer_idx;  /* 當前生產者索引 */
    uint16_t framebuff_consumer_idx;
    struct st_tx_frame* framebuffs;   /* buffer 陣列（malloc 分配） */

    void** audio_frame_addrs; /* array of buffer addresses for st30p */


        /* callbacks */
    wt_rx_connection_status_callback_t conn_cb;
    void* conn_user;
    wt_rx_packet_callback_t pkt_cb;
    void* pkt_user;
    int connected;
    
    bool stop;
    pthread_t encode_thread;

    pthread_mutex_t wake_mutex;       /* buffer 鎖 */
    pthread_cond_t  wake_cond;        /* buffer 條件變數 */

};

struct wt_rx {
    /* MTL 基本資訊 */
    mtl_handle mtl;      /* MTL handle */
    void* session;       /* ST22 session handle */
    wt_protocol_t proto; /* 協議類型，例如 WT_PROTOCOL_ST22 */
    wt_codec_t codec;    /* 編解碼類型 */
    int width, height;   /* 畫面解析度 */
    int fps_num, fps_den;/* 幀率分數形式，例如 30000/1001 */
    int bitcount;        /* 每像素位元數，例如 8、10 */
    int channels;        /* 音訊通道數（如果有音訊） */
    int frequency;       /* 音訊取樣頻率（如果有音訊） */

    enum st_fps mtl_fps;   /* 新增: MTL API 用的 FPS 枚舉 */


    /* ===== ST30 音訊專用 ===== */
    enum st30_fmt st30_fmt;            /* ST30 音訊格式（PCM16 / PCM24） */
    enum st30_sampling st30_sampling;  /* ST30 取樣率（48K / 96K） */
    enum st30_ptime st30_ptime;        /* packet time，若不指定可為 0 */

    /* 網路參數 */
    char port_name[MTL_PORT_MAX_LEN]; /* 本地 port 名稱，例如 "kernel:eth0" */
    char dest_ip[64];    /* 目標 IP（unicast/multicast） */
    char local_ip[64];   /* 本地綁定 IP（unicast 時必須） */
    int udp_port;                     /* 目標 UDP port */
    int payload_type;                 /* RTP Payload Type */

    /* Buffer / 同步管理 */
    int framebuff_cnt;                /* buffer 數量，例如 8 */
    uint16_t framebuff_producer_idx;  /* 當前生產者索引 */
    uint16_t framebuff_consumer_idx;
    struct st_tx_frame* framebuffs;   /* buffer 陣列（malloc 分配） */

    void** audio_frame_addrs; /* array of buffer addresses for st30p */

        /* callbacks */
    wt_rx_connection_status_callback_t conn_cb;
    void* conn_user;
    wt_rx_packet_callback_t pkt_cb;
    void* pkt_user;
    int connected;




    bool stop;
    pthread_t encode_thread;

    pthread_mutex_t wake_mutex;       /* buffer 鎖 */
    pthread_cond_t  wake_cond;        /* buffer 條件變數 */

};

wt_status_t wt_tx_create(wt_protocol_t proto, const char* port_name,
                         const char* local_ip, const char* dest_ip,
                         int port, wt_tx_t** ppThis) {
    if (!ppThis || !port_name || !local_ip || !dest_ip)
        return WT_ERR_PARAM;

    struct mtl_init_params mtl_p;
    memset(&mtl_p, 0, sizeof(mtl_p));

    snprintf(mtl_p.port[MTL_PORT_P], MTL_PORT_MAX_LEN, "%s", port_name);
    mtl_p.num_ports = 1;
    mtl_p.flags |= MTL_FLAG_TASKLET_THREAD;
    mtl_p.flags |= MTL_FLAG_TASKLET_SLEEP;
    mtl_p.flags |= MTL_FLAG_TX_VIDEO_MIGRATE;
    mtl_p.flags |= MTL_FLAG_DEV_AUTO_START_STOP;

    // 關鍵：啟用 kernel socket 模式
    mtl_p.flags |= MTL_FLAG_BIND_NUMA;
    mtl_p.pmd[MTL_PORT_P] = MTL_PMD_KERNEL_SOCKET;

    // ===== 加入這段 =====
    mtl_p.log_level = g_st2110_mtl_log_level;
    // ===================

    mtl_handle mtl = mtl_init(&mtl_p);
    if (!mtl) return WT_ERR_INIT;

    wt_tx_t* tx = calloc(1, sizeof(*tx));
    if (!tx) {
        mtl_uninit(mtl);
        return WT_ERR_NOMEM;
    }
    tx->mtl = mtl;
    tx->proto = proto;

    // 儲存 IP（確保 '\0' 結尾）
    snprintf(tx->dest_ip, sizeof(tx->dest_ip), "%s", dest_ip);
    snprintf(tx->local_ip, sizeof(tx->local_ip), "%s", local_ip);

    tx->udp_port = port;

    // audio 30p
    tx->audio_frame_addrs = calloc(tx->framebuff_cnt, sizeof(void*));

    /* ==== 新增的 buffer 初始化 ==== */
    tx->framebuff_cnt = 8;
    tx->framebuffs = malloc(sizeof(struct st_tx_frame) * tx->framebuff_cnt);
    if (!tx->framebuffs) {
        fprintf(stderr, "framebuff malloc fail\n");
        mtl_uninit(mtl);
        free(tx);
        return WT_ERR_NOMEM;
    }
    for (uint16_t j = 0; j < tx->framebuff_cnt; j++) {
        tx->framebuffs[j].stat = ST_TX_FRAME_FREE;
    }
    st_pthread_mutex_init(&tx->wake_mutex, NULL);
    st_pthread_cond_init(&tx->wake_cond, NULL);
    tx->framebuff_consumer_idx = 0;

    // 儲存 port name
    strncpy(tx->port_name, port_name, sizeof(tx->port_name) - 1);
    tx->port_name[sizeof(tx->port_name) - 1] = '\0';

    *ppThis = tx;
    return WT_OK;
}


wt_status_t wt_rx_create(wt_protocol_t proto, const char* port_name,
                         const char* local_ip, const char* dest_ip,
                         int port, wt_rx_t** ppThis) {
    if (!ppThis || !port_name || !local_ip || !dest_ip)
        return WT_ERR_PARAM;

    struct mtl_init_params mtl_p;
    memset(&mtl_p, 0, sizeof(mtl_p));

    snprintf(mtl_p.port[MTL_PORT_P], MTL_PORT_MAX_LEN, "%s", port_name);
    mtl_p.num_ports = 1;
    mtl_p.flags |= MTL_FLAG_TASKLET_THREAD;
    mtl_p.flags |= MTL_FLAG_TASKLET_SLEEP;
    mtl_p.flags |= MTL_FLAG_TX_VIDEO_MIGRATE;
    mtl_p.flags |= MTL_FLAG_DEV_AUTO_START_STOP;

    // 關鍵：啟用 kernel socket 模式
    mtl_p.flags |= MTL_FLAG_BIND_NUMA;
    mtl_p.pmd[MTL_PORT_P] = MTL_PMD_KERNEL_SOCKET;

    // ===== 套用 log level 設定 =====
    mtl_p.log_level = g_st2110_mtl_log_level;
    // ================================

    mtl_handle mtl = mtl_init(&mtl_p);
    if (!mtl) return WT_ERR_INIT;

    wt_rx_t* rx = calloc(1, sizeof(*rx));
    if (!rx) {
        mtl_uninit(mtl);
        return WT_ERR_NOMEM;
    }
    rx->mtl = mtl;
    rx->proto = proto;

    // 儲存 IP（確保 '\0' 結尾）
    snprintf(rx->dest_ip, sizeof(rx->dest_ip), "%s", dest_ip);
    snprintf(rx->local_ip, sizeof(rx->local_ip), "%s", local_ip);

    rx->udp_port = port;

    // audio 30p
    rx->audio_frame_addrs = calloc(rx->framebuff_cnt, sizeof(void*));

    /* ==== 新增的 buffer 初始化 ==== */
    rx->framebuff_cnt = 8;
    rx->framebuffs = malloc(sizeof(struct st_tx_frame) * rx->framebuff_cnt);
    if (!rx->framebuffs) {
        fprintf(stderr, "framebuff malloc fail\n");
        mtl_uninit(mtl);
        free(rx);
        return WT_ERR_NOMEM;
    }
    for (uint16_t j = 0; j < rx->framebuff_cnt; j++) {
        rx->framebuffs[j].stat = ST_TX_FRAME_FREE;
    }
    st_pthread_mutex_init(&rx->wake_mutex, NULL);
    st_pthread_cond_init(&rx->wake_cond, NULL);
    rx->framebuff_consumer_idx = 0;

    // 儲存 port name
    strncpy(rx->port_name, port_name, sizeof(rx->port_name) - 1);
    rx->port_name[sizeof(rx->port_name) - 1] = '\0';

    *ppThis = rx;
    return WT_OK;
}




wt_status_t wt_tx_destroy(wt_tx_t* pThis) {
    if (!pThis) return WT_ERR_PARAM;

    /* 關閉 ST22 session */
    if (pThis->session)
        st22_tx_free(pThis->session);

    /* 銷毀同步物件 */
    pthread_mutex_destroy(&pThis->wake_mutex);
    pthread_cond_destroy(&pThis->wake_cond);

    /* 釋放 frame buffer */
    if (pThis->framebuffs)
        free(pThis->framebuffs);

    /* 關閉 MTL */
    if (pThis->mtl)
        mtl_uninit(pThis->mtl);

    free(pThis);
    return WT_OK;
}



wt_status_t wt_tx_set_video_format(wt_tx_t* pThis, wt_codec_t codec, int width, int height, int fps_num, int fps_den) {
    if (!pThis) return WT_ERR_PARAM;
    pThis->codec = codec;
    pThis->width = width;
    pThis->height = height;
    pThis->fps_num = fps_num;
    pThis->fps_den = fps_den;
    /* 直接轉成 MTL 枚舉存起來 */
    pThis->mtl_fps = wt_map_fps(fps_num, fps_den);
    if (pThis->mtl_fps == ST_FPS_MAX) {
        fprintf(stderr, "wt_tx_set_video_format: unsupported fps %d/%d\n", fps_num, fps_den);
        return WT_ERR_PARAM;
    }
    return WT_OK;
}
wt_status_t wt_rx_set_video_format(wt_rx_t* pThis, wt_codec_t codec, int width, int height, int fps_num, int fps_den) {
    if (!pThis) return WT_ERR_PARAM;
    pThis->codec = codec;
    pThis->width = width;
    pThis->height = height;
    pThis->fps_num = fps_num;
    pThis->fps_den = fps_den;
    /* 直接轉成 MTL 枚舉存起來 */
    pThis->mtl_fps = wt_map_fps(fps_num, fps_den);
    if (pThis->mtl_fps == ST_FPS_MAX) {
        fprintf(stderr, "wt_tx_set_video_format: unsupported fps %d/%d\n", fps_num, fps_den);
        return WT_ERR_PARAM;
    }
    return WT_OK;
}


wt_status_t wt_tx_set_audio_format(wt_tx_t* pThis,
                                   wt_codec_t codec,
                                   int bitcount,
                                   int channels,
                                   int frequency) {
    if (!pThis) return WT_ERR_PARAM;

    pThis->codec = codec;
    pThis->bitcount = bitcount;
    pThis->channels = channels;
    pThis->frequency = frequency;

    /* ==== ST30 對應用的 mapping ==== */
    /* bitcount -> ST30_FMT */
    if (bitcount == 16) {
        pThis->st30_fmt = ST30_FMT_PCM16;
    } else if (bitcount == 24) {
        pThis->st30_fmt = ST30_FMT_PCM24;
    } else {
        fprintf(stderr, "Unsupported bitcount: %d\n", bitcount);
        return WT_ERR_PARAM;
    }

    /* frequency -> ST30_SAMPLING */
    switch (frequency) {
        case 48000:
            pThis->st30_sampling = ST30_SAMPLING_48K;
            break;
        case 96000:
            pThis->st30_sampling = ST30_SAMPLING_96K;
            break;
        default:
            fprintf(stderr, "Unsupported frequency: %d\n", frequency);
            return WT_ERR_PARAM;
    }

    /* 預設不指定 ptime，由 MTL 自己決定 */
    pThis->st30_ptime = ST30_PTIME_1MS;

    return WT_OK;
}
wt_status_t wt_rx_set_audio_format(wt_rx_t* pThis,
                                   wt_codec_t codec,
                                   int bitcount,
                                   int channels,
                                   int frequency) {
    if (!pThis) return WT_ERR_PARAM;

    pThis->codec = codec;
    pThis->bitcount = bitcount;
    pThis->channels = channels;
    pThis->frequency = frequency;

    /* ==== ST30 對應用的 mapping ==== */
    /* bitcount -> ST30_FMT */
    if (bitcount == 16) {
        pThis->st30_fmt = ST30_FMT_PCM16;
    } else if (bitcount == 24) {
        pThis->st30_fmt = ST30_FMT_PCM24;
    } else {
        fprintf(stderr, "Unsupported bitcount: %d\n", bitcount);
        return WT_ERR_PARAM;
    }

    /* frequency -> ST30_SAMPLING */
    switch (frequency) {
        case 48000:
            pThis->st30_sampling = ST30_SAMPLING_48K;
            break;
        case 96000:
            pThis->st30_sampling = ST30_SAMPLING_96K;
            break;
        default:
            fprintf(stderr, "Unsupported frequency: %d\n", frequency);
            return WT_ERR_PARAM;
    }

    /* 預設不指定 ptime，由 MTL 自己決定 */
    pThis->st30_ptime = ST30_PTIME_1MS;

    return WT_OK;
}


static int wt_tx22_next_frame(void* priv, uint16_t* next_frame_idx,
                            struct st22_tx_frame_meta* meta) {
    wt_tx_t* tx = (wt_tx_t*)priv;
    uint16_t idx = tx->framebuff_consumer_idx;
    struct st_tx_frame* fb = &tx->framebuffs[idx];
    int ret;

    st_pthread_mutex_lock(&tx->wake_mutex);
    if (ST_TX_FRAME_READY == fb->stat) {
        fb->stat = ST_TX_FRAME_IN_TRANSMITTING;
        *next_frame_idx = idx;
        meta->codestream_size = fb->size;  // 已填好的資料大小
        ret = 0;
        idx++;
        if (idx >= tx->framebuff_cnt) idx = 0;
        tx->framebuff_consumer_idx = idx;
    } else {
        ret = -EIO;
    }
    st_pthread_cond_signal(&tx->wake_cond);
    st_pthread_mutex_unlock(&tx->wake_mutex);

    return ret;
}

/* Frame 傳送完成的回調 */
static int wt_tx22_frame_done(void* priv, uint16_t frame_idx,
                            struct st22_tx_frame_meta* meta) {
    wt_tx_t* pThis = (wt_tx_t*)priv;
    MTL_MAY_UNUSED(meta);

    pthread_mutex_lock(&pThis->wake_mutex);

    if (pThis->framebuffs[frame_idx].stat == ST_TX_FRAME_IN_TRANSMITTING) {
        pThis->framebuffs[frame_idx].stat = ST_TX_FRAME_FREE;
        //printf("[WT_TX] frame %u done, buffer released\n", frame_idx);
    } else {
        fprintf(stderr, "[WT_TX] frame_done error: frame %u status=%d\n",
                frame_idx, pThis->framebuffs[frame_idx].stat);
    }

    pthread_cond_signal(&pThis->wake_cond);
    pthread_mutex_unlock(&pThis->wake_mutex);

    return 0;
}

#if 0
/* ===== ST30p Callbacks (Audio) ===== */
/* ST30p: 送完一個 frame 的通知 */
static int wt_tx30p_frame_done(void* priv, struct st30_frame* frame) {
    wt_tx_t* pThis = (wt_tx_t*)priv;

    pthread_mutex_lock(&pThis->wake_mutex);

    for (int i = 0; i < pThis->framebuff_cnt; i++) {
        if (pThis->audio_frame_addrs[i] == frame->addr) {
            pThis->framebuffs[i].stat = ST_TX_FRAME_FREE;
            break;
        }
    }

    pthread_cond_signal(&pThis->wake_cond);
    pthread_mutex_unlock(&pThis->wake_mutex);

    return 0;
}
#endif 
static inline uint32_t ptime_to_ms(enum st30_ptime ptime) {
    switch (ptime) {
    case ST30_PTIME_1MS: return 1;
    case ST30_PTIME_125US: return 0; /* 或者 0.125ms，看你要怎麼處理 */
    default: return 1;
    }
}


wt_status_t wt_tx_start(wt_tx_t* pThis) {
    if (!pThis) return WT_ERR_PARAM;

    //printf("[DEBUG wt] wt_tx_start: proto=%d (%s)\n", 
    //    pThis->proto,
    //    (pThis->proto == WT_PROTOCOL_ST22) ? "ST22" :
    //    (pThis->proto == WT_PROTOCOL_ST30) ? "ST30" :
    //    "UNKNOWN"
    //);

    if (pThis->proto == WT_PROTOCOL_ST22) {
        /* -------- ST22 Video -------- */
        struct st22_tx_ops ops_tx;
        memset(&ops_tx, 0, sizeof(ops_tx));

        ops_tx.name = "wt_st22_tx";
        ops_tx.priv = pThis;
        ops_tx.num_port = 1;

        //memcpy(ops_tx.dip_addr[MTL_SESSION_PORT_P], pThis->dest_ip, MTL_IP_ADDR_LEN);
        // ✅ 修改這行：使用 inet_pton 轉換 IP 字串
        if (inet_pton(AF_INET, pThis->dest_ip, ops_tx.dip_addr[MTL_SESSION_PORT_P]) != 1) {
            fprintf(stderr, "❌ Invalid destination IP: %s\n", pThis->dest_ip);
            return WT_ERR_PARAM;
        }
	
	snprintf(ops_tx.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s", pThis->port_name);
        ops_tx.udp_port[MTL_SESSION_PORT_P] = pThis->udp_port;

        ops_tx.pacing = ST21_PACING_NARROW;
        ops_tx.width = pThis->width;
        ops_tx.height = pThis->height;
        ops_tx.fps = pThis->mtl_fps;

        ops_tx.payload_type = 112;
        ops_tx.type = ST22_TYPE_FRAME_LEVEL;
        ops_tx.pack_type = ST22_PACK_CODESTREAM;

   	pThis->bitcount = 24; // 24-bit
    	pThis->framebuff_cnt = 8;
    	ops_tx.framebuff_cnt = pThis->framebuff_cnt;

        ops_tx.framebuff_max_size = pThis->width * pThis->height * (pThis->bitcount / 8);

        ops_tx.get_next_frame = wt_tx22_next_frame;
        ops_tx.notify_frame_done = wt_tx22_frame_done;


        pThis->session = st22_tx_create(pThis->mtl, &ops_tx);
    }
    else if (pThis->proto == WT_PROTOCOL_ST30) {
    	struct st30p_tx_ops ops_tx;
    	memset(&ops_tx, 0, sizeof(ops_tx));

    	ops_tx.name = "wt_st30p_tx";
    	ops_tx.priv = pThis;
    	ops_tx.fmt = pThis->st30_fmt;
    	ops_tx.sampling = pThis->st30_sampling;
    	ops_tx.ptime = pThis->st30_ptime;
    	ops_tx.channel = pThis->channels;
	ops_tx.port.num_port = 1;  // 這行是關鍵
	/* 填 ST30P port 資訊 */
	st_txp_para_port_set(&ops_tx.port, MTL_SESSION_PORT_P, pThis->port_name);
	st_txp_para_dip_set(&ops_tx.port, MTL_PORT_P, pThis->dest_ip);
	st_txp_para_udp_port_set(&ops_tx.port, MTL_PORT_P, pThis->udp_port);

    	pThis->framebuff_cnt = 8;
    	ops_tx.framebuff_cnt = pThis->framebuff_cnt;


	// 計算 packet size
	int pkt_size = st30_get_packet_size(ops_tx.fmt,
				    ops_tx.ptime,
                                    ops_tx.sampling,
                                    ops_tx.channel
                                    );

	if (pkt_size <= 0) {
    		fprintf(stderr, "❌ Invalid packet size, fmt=%d sampling=%d ch=%d ptime=%d\n",
            		ops_tx.fmt, ops_tx.sampling, ops_tx.channel, ops_tx.ptime);
    	return WT_ERR_PARAM;
	}

	// 一幀就放一個 packet（最小配置），你也可以改成多個 packet
	ops_tx.framebuff_cnt  = 8;           // 幀緩衝數量
	ops_tx.framebuff_size = pkt_size * 1; // 必須是 pkt_size 的整數倍
    	// ops_tx.notify_frame_done = wt_tx30p_frame_done; // 回收 buffer audio tx crash fixed 20251205

    	pThis->session = st30p_tx_create(pThis->mtl, &ops_tx);
    }else {
        fprintf(stderr, "❌ Unsupported proto %d\n", pThis->proto);
        return WT_ERR_PARAM;
    }
    return pThis->session ? WT_OK : WT_ERR_INIT;
}


wt_status_t wt_tx_stop(wt_tx_t* pThis) {
    if (!pThis) return WT_ERR_PARAM;
    if (pThis->session) {
        switch (pThis->proto) {
            case WT_PROTOCOL_ST20: st20_tx_free(pThis->session); break;
            case WT_PROTOCOL_ST22: st22_tx_free(pThis->session); break;
            case WT_PROTOCOL_ST30: st30p_tx_free(pThis->session); break;
            case WT_PROTOCOL_ST31: st30_tx_free(pThis->session); break;
        }
        pThis->session = NULL;
    }
    return WT_OK;
}

wt_status_t wt_tx_send_packet(wt_tx_t* pThis, wt_packet_t* pPacket) {
    if (!pThis || !pPacket) return WT_ERR_PARAM;

    pthread_mutex_lock(&pThis->wake_mutex);

    if (pThis->proto == WT_PROTOCOL_ST30) {
        /* Push mode: 拿一個可用 frame */
        struct st30_frame* frame = st30p_tx_get_frame(pThis->session);
        if (!frame) {
            pthread_mutex_unlock(&pThis->wake_mutex);
            return WT_ERR_INTERNAL;
        }

        /* 拷貝資料到 frame buffer */
        memcpy(frame->addr, pPacket->data, pPacket->size);
        frame->data_size = pPacket->size;

        /* 提交 frame */
        st30p_tx_put_frame(pThis->session, frame);

        pthread_mutex_unlock(&pThis->wake_mutex);
        return WT_OK;
    }
    else if (pThis->proto == WT_PROTOCOL_ST22) {
        /* 等待有空 buffer */
        uint16_t producer_idx = pThis->framebuff_producer_idx;
        struct st_tx_frame* framebuff = &pThis->framebuffs[producer_idx];

        while (ST_TX_FRAME_FREE != framebuff->stat) {
            pthread_cond_wait(&pThis->wake_cond, &pThis->wake_mutex);
            producer_idx = pThis->framebuff_producer_idx;
            framebuff = &pThis->framebuffs[producer_idx];
        }

        void* dst = st22_tx_get_fb_addr(pThis->session, producer_idx);
        if (!dst) {
            pthread_mutex_unlock(&pThis->wake_mutex);
            return WT_ERR_INTERNAL;
        }

        memcpy(dst, pPacket->data, pPacket->size);
        framebuff->size = pPacket->size;
        framebuff->stat = ST_TX_FRAME_READY;

        pThis->framebuff_producer_idx = (producer_idx + 1) % pThis->framebuff_cnt;
        pthread_cond_signal(&pThis->wake_cond);

        pthread_mutex_unlock(&pThis->wake_mutex);
        return WT_OK;
    }

    pthread_mutex_unlock(&pThis->wake_mutex);
    return WT_ERR_INTERNAL;
}


/* ====================== RX ====================== */
//static int mtl_rx_callback(void* priv, void* frame, struct st22_rx_frame_meta* meta) {
#if 0    
    wt_rx_t* rx = (wt_rx_t*)priv;
    if (rx->packet_cb) {
        wt_packet_t pkt;
        pkt.codec = WT_CODEC_H264; /* TODO: 根據 meta 填正確 codec */
        pkt.data = frame;
        pkt.size = meta->frame_total_size;
        pkt.timestamp_ns = meta->timestamp;
        rx->packet_cb(&pkt, rx->user_data);
    }
    st22_rx_put_framebuff(rx->session, frame);
#endif
//    return 0;
//}

//wt_status_t wt_rx_create(wt_protocol_t proto, const char* port_name, const char* local_ip, const char* source_ip, int port, wt_rx_t** ppThis) {




//}
/* ---- Callbacks setter ---- */
wt_status_t wt_rx_set_connection_status_callback(
    wt_rx_t* rx, wt_rx_connection_status_callback_t cb, void* user_data) {
  if (!rx) return WT_ERR_PARAM;
  rx->conn_cb = cb;
  rx->conn_user = user_data;
  return WT_OK;
}

wt_status_t wt_rx_set_packet_callback(
    wt_rx_t* rx, wt_rx_packet_callback_t cb, void* user_data) {
  if (!rx) return WT_ERR_PARAM;
  rx->pkt_cb = cb;
  rx->pkt_user = user_data;
  return WT_OK;
}

/* ---- ST22: frame-ready callback → 轉成 wt_packet_t 丟給上層 ---- */
static int wt_rx22_frame_ready(void* priv, void* frame,
                               struct st22_rx_frame_meta* meta) {
  wt_rx_t* rx = (wt_rx_t*)priv;

  /* 第一次收包 → Connected */
  if (!rx->connected) {
    rx->connected = 1;
    if (rx->conn_cb) rx->conn_cb(WT_CONNECTION_STATUS_CONNECTED, rx->conn_user);
  }

  if (rx->pkt_cb && meta && frame) {
    wt_packet_t pkt;
    pkt.codec        = rx->codec;                /* 先前 set 的 codec */
    pkt.data         = frame;                      /* 注意：回傳後會立刻 put 回去 */
    pkt.size         = meta->frame_total_size;     /* 當幀 bytes */
    pkt.timestamp_ns = meta->timestamp;            /* ns */

    rx->pkt_cb(&pkt, rx->pkt_user);
  }

  /* 一定要歸還 buffer，否則 RX 會塞滿 */
  st22_rx_put_framebuff(rx->session, frame);
  return 0;
}

/* ★ st30p：只有 void* 參數的回呼 */
static int rx_st30p_packet_ready(void* priv) {
    wt_rx_t* rx = (wt_rx_t*)priv;
        // ✅ 關鍵修正：檢查 session 是否已經建立
    if (!rx->session) {
        return 0;  // ← 提早返回，等 session 建立好再處理
    }
    
    if (!rx->connected) {
        rx->connected = 1;
        if (rx->conn_cb) rx->conn_cb(WT_CONNECTION_STATUS_CONNECTED, rx->conn_user);
    }
    while (1) {
        /* 1) 取一個可讀 frame（注意 handle 要 cast 成 st30p_rx_handle） */
        struct st30_frame* f = st30p_rx_get_frame((st30p_rx_handle)rx->session);
        if (!f) break;  /* 沒有可用的 frame 就跳出 */

        /* 2) 從 frame 取位址與大小 —— 把這兩行欄位名對齊你的 header */
        void*  data = NULL;
        size_t size = 0;
        /* 常見命名：addr / data_size 或 buf / len */
        /* 如果不確定，先試試這個組合；不對就打開 st30_pipeline_api.h 查 struct st30_frame */
        data = f->addr;      size = f->data_size;
        /* 3) 轉成你自家的 wt_packet_t 往上拋 */
        if (rx->pkt_cb && data && size) {
            wt_packet_t pkt;
            pkt.codec        = rx->codec;   /* 先前用 wt_rx_set_audio_format 設的 */
            pkt.data         = data;          /* 回呼結束後會 put 回去，要延後處理請 memcpy */
            pkt.size         = size;
            pkt.timestamp_ns = f->timestamp;  // ✅ 修正 3：可以用 frame 的 timestamp;/* 若要時間戳，之後再用 API 取 RTP/PTS 做換算 */
            rx->pkt_cb(&pkt, rx->pkt_user);
        }
        /* 4) 還回 frame */
        st30p_rx_put_frame((st30p_rx_handle)rx->session, f);
    }
    return 0;
}



wt_status_t wt_rx_start(wt_rx_t* pThis) {
    if (!pThis) return WT_ERR_PARAM;

    printf("[DEBUG wt] wt_rx_start: proto=%d (%s)\n",
        pThis->proto,
        (pThis->proto == WT_PROTOCOL_ST22) ? "ST22" :
        (pThis->proto == WT_PROTOCOL_ST30) ? "ST30" :
        "UNKNOWN"
    );

    if (pThis->proto == WT_PROTOCOL_ST22) {
        /* -------- ST22 Video RX -------- */
        struct st22_rx_ops ops_rx;
        memset(&ops_rx, 0, sizeof(ops_rx));

        ops_rx.name     = "wt_st22_rx";
        ops_rx.priv     = pThis;
        ops_rx.num_port = 1;


        //memcpy(ops_rx.ip_addr[MTL_SESSION_PORT_P], pThis->dest_ip, MTL_IP_ADDR_LEN);
        
	// ✅ 使用 inet_pton 正確轉換
	if (inet_pton(AF_INET, pThis->dest_ip, ops_rx.ip_addr[MTL_SESSION_PORT_P]) != 1) {
    		fprintf(stderr, "❌ Invalid source/multicast IP: %s\n", pThis->dest_ip);
    		return WT_ERR_PARAM;
	}
	
	snprintf(ops_rx.port[MTL_SESSION_PORT_P], MTL_PORT_MAX_LEN, "%s", pThis->port_name);
        ops_rx.udp_port[MTL_SESSION_PORT_P] = pThis->udp_port;

        ops_rx.pacing = ST21_PACING_NARROW;
        ops_rx.width = pThis->width;
        ops_rx.height = pThis->height;
        ops_rx.fps = pThis->mtl_fps;


        /* 對齊 TX：Frame-level + CodeStream */
        ops_rx.type       = ST22_TYPE_FRAME_LEVEL;
        ops_rx.pack_type  = ST22_PACK_CODESTREAM;
        /* Payload type 與對端一致（你 TX 用 112） */
        ops_rx.payload_type = 112;

        /* buffer 設定 */
        if (pThis->framebuff_cnt <= 0) pThis->framebuff_cnt = 8;
        ops_rx.framebuff_cnt = pThis->framebuff_cnt;
        /* RX 端通常不需要設 max_size；必要時可估一個上限*/
           ops_rx.framebuff_max_size = pThis->width * pThis->height * 3;

        /* RX 專屬 callback：有一幀 codestream 可取用時呼叫 */
        ops_rx.notify_frame_ready = wt_rx22_frame_ready; /* 你要實作 */

        pThis->session = st22_rx_create(pThis->mtl, &ops_rx);
        if (!pThis->session) return WT_ERR_INIT;
    }else if (pThis->proto == WT_PROTOCOL_ST30) {
        struct st30p_rx_ops ops_rx;
        memset(&ops_rx, 0, sizeof(ops_rx));

        ops_rx.name = "wt_st30p_rx";
        ops_rx.priv = pThis;
        ops_rx.fmt = pThis->st30_fmt;
        ops_rx.sampling = pThis->st30_sampling;
        ops_rx.ptime = pThis->st30_ptime;
        ops_rx.channel = pThis->channels;
        ops_rx.port.num_port = 1;  // 這行是關鍵
        /* 填 ST30P port 資訊 */
        st_rxp_para_port_set(&ops_rx.port, MTL_SESSION_PORT_P, pThis->port_name);
        st_rxp_para_ip_set(&ops_rx.port, MTL_PORT_P, pThis->dest_ip);
        st_rxp_para_udp_port_set(&ops_rx.port, MTL_PORT_P, pThis->udp_port);

        pThis->framebuff_cnt = 8;
        ops_rx.framebuff_cnt = pThis->framebuff_cnt;


        // 計算 packet size
        int pkt_size = st30_get_packet_size(ops_rx.fmt,
                                    ops_rx.ptime,
                                    ops_rx.sampling,
                                    ops_rx.channel
                                    );

        if (pkt_size <= 0) {
                fprintf(stderr, "❌ Invalid packet size, fmt=%d sampling=%d ch=%d ptime=%d\n",
                        ops_rx.fmt, ops_rx.sampling, ops_rx.channel, ops_rx.ptime);
        return WT_ERR_PARAM;
        }

        // 一幀就放一個 packet（最小配置），你也可以改成多個 packet
        ops_rx.framebuff_cnt  = 8;           // 幀緩衝數量
        ops_rx.framebuff_size = pkt_size * 1; // 必須是 pkt_size 的整數倍

    	ops_rx.notify_frame_available = rx_st30p_packet_ready;

        pThis->session = st30p_rx_create(pThis->mtl, &ops_rx);
    }else {
        fprintf(stderr, "❌ Unsupported proto %d\n", pThis->proto);
        return WT_ERR_PARAM;
    }

    return WT_OK;
}

wt_status_t wt_rx_destroy(wt_rx_t* pThis) {
    if (!pThis) return WT_ERR_PARAM;

    /* ✅ 根據協議類型關閉 session */
    if (pThis->session) {
        if (pThis->proto == WT_PROTOCOL_ST22) {
            st22_rx_free(pThis->session);
        } else if (pThis->proto == WT_PROTOCOL_ST30) {
            st30p_rx_free((st30p_rx_handle)pThis->session);  // ← 加這個判斷！
        }
    }


    /* 銷毀同步物件 */
    pthread_mutex_destroy(&pThis->wake_mutex);
    pthread_cond_destroy(&pThis->wake_cond);

    /* 釋放 frame buffer */
    if (pThis->framebuffs)
        free(pThis->framebuffs);

    /* 關閉 MTL */
    if (pThis->mtl)
        mtl_uninit(pThis->mtl);

    free(pThis);
    return WT_OK;
}


//wt_status_t wt_rx_set_connection_status_callback(wt_rx_t* pThis, wt_rx_connection_status_callback_t cb, void* user_data) {
//    if (!pThis) return WT_ERR_PARAM;
//    pThis->status_cb = cb;
//    pThis->user_data = user_data;
//    return WT_OK;
//}

//wt_status_t wt_rx_set_packet_callback(wt_rx_t* pThis, wt_rx_packet_callback_t cb, void* user_data) {
//    if (!pThis) return WT_ERR_PARAM;
//    pThis->packet_cb = cb;
//    pThis->user_data = user_data;
//    return WT_OK;
//}

wt_status_t wt_rx_stop(wt_rx_t* pThis) {
    if (!pThis) return WT_ERR_PARAM;
    if (pThis->session) {
        switch (pThis->proto) {
            case WT_PROTOCOL_ST20: st20_rx_free(pThis->session); break;
            case WT_PROTOCOL_ST22: st22_rx_free(pThis->session); break;
            case WT_PROTOCOL_ST30: st30p_rx_free(pThis->session); break;
            case WT_PROTOCOL_ST31: st30_rx_free(pThis->session); break;
        }
        pThis->session = NULL;
    }
    return WT_OK;
}

#if 0
wt_status_t wt_rx_get_video_format(wt_rx_t* pThis, wt_codec_t* pCodec, int* pWidth, int* pHeight, int* pFpsNum, int* pFpsDen) {
    /* TODO: 從 MTL 查詢真正的格式 */
    return WT_ERR_INTERNAL;
}

wt_status_t wt_rx_get_audio_format(wt_rx_t* pThis, wt_codec_t* pCodec, int* pBitCount, int* pChannels, int* pFrequency) {
    /* TODO: 從 MTL 查詢真正的格式 */
    return WT_ERR_INTERNAL;
}
#endif
