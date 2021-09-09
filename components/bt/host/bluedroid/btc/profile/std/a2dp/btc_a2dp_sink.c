// Copyright 2015-2016 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at

//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/******************************************************************************
 **
 **  Name:          btc_a2dp_sink.c
 **
 ******************************************************************************/
#include "common/bt_target.h"
#include "common/bt_trace.h"
#include <string.h>
#include <stdint.h>
#include "common/bt_defs.h"
#include "osi/allocator.h"
#include "osi/mutex.h"
#include "osi/semaphore.h"
#include "osi/thread.h"
#include "osi/fixed_queue.h"
#include "stack/a2d_api.h"
#include "stack/a2d_sbc.h"
#include "bta/bta_av_api.h"
#include "bta/bta_av_ci.h"
#include "btc_av_co.h"
#include "btc_a2dp.h"
#include "btc_a2dp_control.h"
#include "btc_a2dp_sink.h"
#include "btc/btc_manage.h"
#include "btc_av.h"
#include "btc/btc_util.h"
#include "esp_a2dp_api.h"
#include "oi_codec_sbc.h"
#include "oi_status.h"
#include "osi/future.h"
#include <assert.h>

// add by hishi for support APTX 2019.1.25
#include "openaptx.h"

//add by hailin for ldac
#include "ldac_codec_api.h"

// for watch dog diable by nishi
#include "esp_int_wdt.h"
#include "esp_task_wdt.h"


#if (BTC_AV_SINK_INCLUDED == TRUE)

extern osi_thread_t *btc_thread;

/*****************************************************************************
 **  Constants
 *****************************************************************************/

/* BTC media cmd event definition : BTC_MEDIA_TASK_CMD */
enum {
    BTC_MEDIA_TASK_SINK_INIT,
    BTC_MEDIA_TASK_SINK_CLEAN_UP,
    BTC_MEDIA_FLUSH_AA_RX,
    BTC_MEDIA_AUDIO_SINK_CFG_UPDATE,
    BTC_MEDIA_AUDIO_SINK_CLEAR_TRACK,
};

enum {
    BTC_A2DP_SINK_STATE_OFF = 0,
    BTC_A2DP_SINK_STATE_ON = 1,
    BTC_A2DP_SINK_STATE_SHUTTING_DOWN = 2
};

/*
 * CONGESTION COMPENSATION CTRL ::
 *
 * Thus setting controls how many buffers we will hold in media task
 * during temp link congestion. Together with the stack buffer queues
 * it controls much temporary a2dp link congestion we can
 * compensate for. It however also depends on the default run level of sinks
 * jitterbuffers. Depending on type of sink this would vary.
 * Ideally the (SRC) max tx buffer capacity should equal the sinks
 * jitterbuffer runlevel including any intermediate buffers on the way
 * towards the sinks codec.
 */

/* fixme -- define this in pcm time instead of buffer count */

/* The typical runlevel of the tx queue size is ~1 buffer
   but due to link flow control or thread preemption in lower
   layers we might need to temporarily buffer up data */

/* 18 frames is equivalent to 6.89*18*2.9 ~= 360 ms @ 44.1 khz, 20 ms mediatick */
#define MAX_OUTPUT_A2DP_SNK_FRAME_QUEUE_SZ     (25)
#define JITTER_BUFFER_WATER_LEVEL (5)

typedef struct {
    uint32_t sig;
    void *param;
} a2dp_sink_task_evt_t;

typedef struct {
    UINT16 num_frames_to_be_processed;
    UINT16 len;
    UINT16 offset;
    UINT16 layer_specific;
} tBT_SBC_HDR;

typedef struct {
    BOOLEAN rx_flush; /* discards any incoming data when true */
    UINT8   channel_count;
    osi_sem_t post_sem;
    fixed_queue_t *RxSbcQ;
    UINT32  sample_rate;
    UINT8	codec_type;		// add by nishi 2019.1.25 for APTX support
} tBTC_A2DP_SINK_CB;

typedef struct {
    tBTC_A2DP_SINK_CB   btc_aa_snk_cb;
    osi_thread_t        *btc_aa_snk_task_hdl;
    OI_CODEC_SBC_DECODER_CONTEXT    context;
    OI_UINT32           contextData[CODEC_DATA_WORDS(2, SBC_CODEC_FAST_FILTER_BUFFERS)];
    OI_INT16            pcmData[15 * SBC_MAX_SAMPLES_PER_FRAME * SBC_MAX_CHANNELS];
} a2dp_sink_local_param_t;

static void btc_a2dp_sink_thread_init(UNUSED_ATTR void *context);
static void btc_a2dp_sink_thread_cleanup(UNUSED_ATTR void *context);
static void btc_a2dp_sink_flush_q(fixed_queue_t *p_q);
static void btc_a2dp_sink_rx_flush(void);
//static int btc_a2dp_sink_get_track_frequency(UINT8 frequency);
// changed by nishi 2019.1.22
static int btc_a2dp_sink_get_track_frequency(UINT8 codec_id,UINT8 frequency);
//add by hailin
static int btc_a2dp_sink_get_track_frequency_ldac(UINT8 frequency);
//static int btc_a2dp_sink_get_track_channel_count(UINT8 channeltype);
// changed by nishi
static int btc_a2dp_sink_get_track_channel_count(UINT8 codec_id,UINT8 channeltype);
//add by hailin
static int btc_a2dp_sink_get_track_channel_count_ldac(UINT8 channeltype);
/* Handle incoming media packets A2DP SINK streaming*/
static void btc_a2dp_sink_handle_inc_media(tBT_SBC_HDR *p_msg);
// add b nishi */
static void btc_a2dp_sink_handle_inc_media_Aptx(tBT_SBC_HDR *p_msg);
//add by hailin
static void btc_a2dp_sink_handle_inc_media_ldac(tBT_SBC_HDR *p_msg);
static void btc_a2dp_sink_handle_decoder_reset(tBTC_MEDIA_SINK_CFG_UPDATE *p_msg);
// add by nishi 2019.1.22 start
static void btc_a2dp_sink_handle_decoder_reset_Sbc(tBTC_MEDIA_SINK_CFG_UPDATE *p_msg);
static void btc_a2dp_sink_handle_decoder_reset_Aptx(tBTC_MEDIA_SINK_CFG_UPDATE *p_msg);
// add by nishi 2019.1.22 end
//add by hailin
static void btc_a2dp_sink_handle_decoder_reset_ldac(tBTC_MEDIA_SINK_CFG_UPDATE *p_msg);

static void btc_a2dp_sink_handle_clear_track(void);
static BOOLEAN btc_a2dp_sink_clear_track(void);

//static void btc_a2dp_sink_data_ready(void *context);
static void btc_a2dp_sink_data_ready_task(void *arg);
static void btc_a2dp_sink_data_ready(void *arg);

#define BTC_A2DP_SINK_TASK_STACK_SIZE         (2048 + BT_TASK_EXTRA_STACK_SIZE) // by menuconfig
#define BTC_A2DP_SINK_TASK_PRIO               (configMAX_PRIORITIES - 3)

static int btc_a2dp_sink_state = BTC_A2DP_SINK_STATE_OFF;
static esp_a2d_sink_data_cb_t bt_aa_snk_data_cb = NULL;
static QueueHandle_t btc_aa_snk_data_queue = NULL;
static xTaskHandle btc_aa_snk_task_dr_hdl = NULL;

static struct aptx_context *ctx=NULL;
static ldacdec_t * ldacctx=NULL;

#if A2D_DYNAMIC_MEMORY == FALSE
static a2dp_sink_local_param_t a2dp_sink_local_param;
#else
static a2dp_sink_local_param_t *a2dp_sink_local_param_ptr;
#define a2dp_sink_local_param (*a2dp_sink_local_param_ptr)
#endif ///A2D_DYNAMIC_MEMORY == FALSE

void btc_a2dp_sink_reg_data_cb(esp_a2d_sink_data_cb_t callback)
{
    // todo: critical section protection
    bt_aa_snk_data_cb = callback;
}

static inline void btc_a2d_data_cb_to_app(const uint8_t *data, uint32_t len)
{
    // todo: critical section protection
    if (bt_aa_snk_data_cb) {
        bt_aa_snk_data_cb(data, len);
    }
}

/*****************************************************************************
 **  Misc helper functions
 *****************************************************************************/
static inline void btc_a2d_cb_to_app(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    esp_a2d_cb_t btc_aa_cb = (esp_a2d_cb_t)btc_profile_cb_get(BTC_PID_A2DP);
    if (btc_aa_cb) {
        btc_aa_cb(event, param);
    }
}

/*****************************************************************************
 **  BTC ADAPTATION
 *****************************************************************************/

static bool btc_a2dp_sink_ctrl(uint32_t sig, void *param)
{
    switch (sig) {
    case BTC_MEDIA_TASK_SINK_INIT:
        btc_a2dp_sink_thread_init(NULL);
        break;
    case BTC_MEDIA_TASK_SINK_CLEAN_UP:
        btc_a2dp_sink_thread_cleanup(NULL);
        break;
    case BTC_MEDIA_AUDIO_SINK_CFG_UPDATE:
        btc_a2dp_sink_handle_decoder_reset(param);
        break;
    case BTC_MEDIA_AUDIO_SINK_CLEAR_TRACK:
        btc_a2dp_sink_handle_clear_track();
        break;
    case BTC_MEDIA_FLUSH_AA_RX:
        btc_a2dp_sink_rx_flush();
        break;
    default:
        APPL_TRACE_WARNING("media task unhandled evt: 0x%x\n", sig);
    }

    if (param != NULL) {
        osi_free(param);
    }

    return true;
}

bool btc_a2dp_sink_startup(void)
{
    if (btc_a2dp_sink_state != BTC_A2DP_SINK_STATE_OFF) {
        APPL_TRACE_ERROR("warning : media task already running");
        return false;
    }

#if A2D_DYNAMIC_MEMORY == TRUE
    if ((a2dp_sink_local_param_ptr = (a2dp_sink_local_param_t *)osi_malloc(sizeof(a2dp_sink_local_param_t))) == NULL) {
        APPL_TRACE_ERROR("%s malloc failed!", __func__);
        return false;
    }
    memset((void *)a2dp_sink_local_param_ptr, 0, sizeof(a2dp_sink_local_param_t));
#endif

    APPL_TRACE_EVENT("## A2DP SINK START MEDIA THREAD ##");

    a2dp_sink_local_param.btc_aa_snk_task_hdl = btc_thread;

    if (btc_a2dp_sink_ctrl(BTC_MEDIA_TASK_SINK_INIT, NULL) == false) {
        goto error_exit;
    }
    APPL_TRACE_WARNING("%s(): BTC_A2DP_SINK_TASK_STACK_SIZE=%d",__func__,BTC_A2DP_SINK_TASK_STACK_SIZE);
    xTaskCreatePinnedToCore(btc_a2dp_sink_data_ready_task, "BtA2dSinkDataRT", BTC_A2DP_SINK_TASK_STACK_SIZE, NULL, BTC_A2DP_SINK_TASK_PRIO, &btc_aa_snk_task_dr_hdl, 1);
    if (btc_aa_snk_task_dr_hdl == NULL) {
        goto error_exit;
    }
    APPL_TRACE_EVENT("## A2DP SINK MEDIA THREAD STARTED ##\n");

    return true;

error_exit:;
    APPL_TRACE_ERROR("%s unable to start up media thread\n", __func__);
    a2dp_sink_local_param.btc_aa_snk_task_hdl = NULL;

#if A2D_DYNAMIC_MEMORY == TRUE
    osi_free(a2dp_sink_local_param_ptr);
    a2dp_sink_local_param_ptr = NULL;
#endif

    return false;
}

void btc_a2dp_sink_shutdown(void)
{
    APPL_TRACE_EVENT("## A2DP SINK STOP MEDIA THREAD ##\n");

    // Exit thread
    btc_a2dp_sink_state = BTC_A2DP_SINK_STATE_SHUTTING_DOWN;

    btc_a2dp_sink_ctrl(BTC_MEDIA_TASK_SINK_CLEAN_UP, NULL);

    a2dp_sink_local_param.btc_aa_snk_task_hdl = NULL;

    vQueueDelete(btc_aa_snk_data_queue);
    btc_aa_snk_data_queue = NULL;

#if A2D_DYNAMIC_MEMORY == TRUE
    osi_free(a2dp_sink_local_param_ptr);
    a2dp_sink_local_param_ptr = NULL;
#endif
}

/*****************************************************************************
**
** Function        btc_a2dp_sink_on_idle
**
*******************************************************************************/

void btc_a2dp_sink_on_idle(void)
{
    a2dp_sink_local_param.btc_aa_snk_cb.rx_flush = TRUE;
    btc_a2dp_sink_rx_flush_req();
    btc_a2dp_sink_clear_track();

    APPL_TRACE_DEBUG("Stopped BT track");
}

/*****************************************************************************
**
** Function        btc_a2dp_sink_on_stopped
**
*******************************************************************************/

void btc_a2dp_sink_on_stopped(tBTA_AV_SUSPEND *p_av)
{
    a2dp_sink_local_param.btc_aa_snk_cb.rx_flush = TRUE;
    btc_a2dp_sink_rx_flush_req();
    btc_a2dp_control_set_datachnl_stat(FALSE);
}

/*****************************************************************************
**
** Function        btc_a2dp_on_suspended
**
*******************************************************************************/

void btc_a2dp_sink_on_suspended(tBTA_AV_SUSPEND *p_av)
{
    a2dp_sink_local_param.btc_aa_snk_cb.rx_flush = TRUE;
    btc_a2dp_sink_rx_flush_req();
    return;
}

static void btc_a2dp_sink_data_post(void)
{
    osi_thread_post(a2dp_sink_local_param.btc_aa_snk_task_hdl, btc_a2dp_sink_data_ready, NULL, 1, OSI_THREAD_MAX_TIMEOUT);
}

/*******************************************************************************
 **
 ** Function         btc_a2dp_sink_clear_track
 **
 ** Description
 **
 ** Returns          TRUE is success
 **
 *******************************************************************************/
static BOOLEAN btc_a2dp_sink_clear_track(void)
{
    return btc_a2dp_sink_ctrl(BTC_MEDIA_AUDIO_SINK_CLEAR_TRACK, NULL);
}

/* when true media task discards any rx frames */
void btc_a2dp_sink_set_rx_flush(BOOLEAN enable)
{
    APPL_TRACE_EVENT("## DROP RX %d ##\n", enable);
    a2dp_sink_local_param.btc_aa_snk_cb.rx_flush = enable;
}

/*****************************************************************************
**
** Function        btc_a2dp_sink_reset_decoder
**
** Description
**
** Returns
**
*******************************************************************************/

void btc_a2dp_sink_reset_decoder(UINT8 *p_av)
{
    APPL_TRACE_EVENT("btc reset decoder");
    APPL_TRACE_DEBUG("btc reset decoder p_codec_info[%x:%x:%x:%x:%x:%x]\n",
                     p_av[1], p_av[2], p_av[3],
                     p_av[4], p_av[5], p_av[6]);

    tBTC_MEDIA_SINK_CFG_UPDATE *p_buf;
    if (NULL == (p_buf = osi_malloc(sizeof(tBTC_MEDIA_SINK_CFG_UPDATE)))) {
        APPL_TRACE_ERROR("btc reset decoder No Buffer ");
        return;
    }

    memcpy(p_buf->codec_info, p_av, AVDT_CODEC_SIZE);
    btc_a2dp_sink_ctrl(BTC_MEDIA_AUDIO_SINK_CFG_UPDATE, p_buf);
}

static void btc_a2dp_sink_data_ready_task(void *arg)
{
    int32_t i=0;
    UINT8 j=0;
    TickType_t xLastWakeTime0;
#define BTC_A2DP_SINK_DATA_TIMER (30UL)
    for (;;) {
        int32_t data_evt;
        BaseType_t xStatus;

        xStatus = xQueueReceive(btc_aa_snk_data_queue, &data_evt, portMAX_DELAY);
        if(j==0){
            xLastWakeTime0 = xTaskGetTickCount() + BTC_A2DP_SINK_DATA_TIMER;
            j=1;
        }

        if(xStatus == pdPASS)
        {
            tBT_SBC_HDR *p_msg;
            size_t cnt;

            if (data_evt == 0)
            {
                while(1){
                    p_msg = (tBT_SBC_HDR *)fixed_queue_dequeue(a2dp_sink_local_param.btc_aa_snk_cb.RxSbcQ,0);

                    if ( p_msg == NULL ) {
                        APPL_TRACE_DEBUG("Insufficient data in que ");
                        break;
                    }
                    if (btc_a2dp_sink_state == BTC_A2DP_SINK_STATE_ON){
                        switch(a2dp_sink_local_param.btc_aa_snk_cb.codec_type){
                            case BTA_AV_CODEC_SBC:
                                btc_a2dp_sink_handle_inc_media(p_msg);
                                break;
                            case BTA_AV_CODEC_VEND:
                                //btc_a2dp_sink_handle_inc_media_Aptx(p_msg);
                                btc_a2dp_sink_handle_inc_media_ldac(p_msg);
                                break;
                        }
                    }
                    osi_free(p_msg);
                    if(xLastWakeTime0 < xTaskGetTickCount()){
                        j=0;
                        break;
                    }
                }
            }
        }
    }
}

/*
 * void btc_a2dp_sink_data_ready(void *arg)
 * ������́A�����łł��B
 *
 */
static void btc_a2dp_sink_data_ready(void *arg) {
    APPL_TRACE_DEBUG("%s():#1 start!!", __func__);

    tBT_SBC_HDR *p_msg;
    int nb_of_msgs_to_process = 0;

    osi_sem_give(&a2dp_sink_local_param.btc_aa_snk_cb.post_sem);
    if (fixed_queue_is_empty(a2dp_sink_local_param.btc_aa_snk_cb.RxSbcQ)) {
        APPL_TRACE_DEBUG("  QUE  EMPTY ");
    } else {
        if (a2dp_sink_local_param.btc_aa_snk_cb.rx_flush == TRUE) {
            btc_a2dp_sink_flush_q(a2dp_sink_local_param.btc_aa_snk_cb.RxSbcQ);
            return;
        }
        nb_of_msgs_to_process = fixed_queue_length(a2dp_sink_local_param.btc_aa_snk_cb.RxSbcQ);
        APPL_TRACE_DEBUG("nb:%d", nb_of_msgs_to_process);
        while (nb_of_msgs_to_process > 0) {
            if (btc_a2dp_sink_state != BTC_A2DP_SINK_STATE_ON) {
                return;
            }
            p_msg = (tBT_SBC_HDR *) fixed_queue_dequeue(a2dp_sink_local_param.btc_aa_snk_cb.RxSbcQ, 0);
            if (p_msg == NULL) {
                APPL_TRACE_ERROR("Insufficient data in que ");
                break;
            }
            // DEBUG by nishi
            APPL_TRACE_DEBUG("%s():#2 call btc_a2dp_sink_handle_inc_media() SBC,APTX", __func__);

            switch (a2dp_sink_local_param.btc_aa_snk_cb.codec_type) {
                // SBC codec
                case BTA_AV_CODEC_SBC:
                    btc_a2dp_sink_handle_inc_media(p_msg);
                    break;
                    // APTX codec   add by nishi for APTX support 2019.1.25
                case BTA_AV_CODEC_VEND:
                    btc_a2dp_sink_handle_inc_media_Aptx(p_msg);
                    break;
            }
            // deque �̎��s���`�F�b�N by nishi
            APPL_TRACE_DEBUG("%s():#3 dequeue", __func__);
            osi_free(p_msg);
            nb_of_msgs_to_process--;
        }
        APPL_TRACE_DEBUG(" Process Frames - ");
    }

}

#ifdef TEST_NISHI_USE_OLD_VERSION
static void btc_a2dp_sink_data_ready(UNUSED_ATTR void *context)
{
    tBT_SBC_HDR *p_msg;
    int nb_of_msgs_to_process = 0;

    osi_sem_give(&a2dp_sink_local_param.btc_aa_snk_cb.post_sem);
    if (fixed_queue_is_empty(a2dp_sink_local_param.btc_aa_snk_cb.RxSbcQ)) {
        APPL_TRACE_DEBUG("  QUE  EMPTY ");
    } else {
        if (a2dp_sink_local_param.btc_aa_snk_cb.rx_flush == TRUE) {
            btc_a2dp_sink_flush_q(a2dp_sink_local_param.btc_aa_snk_cb.RxSbcQ);
            return;
        }
        nb_of_msgs_to_process = fixed_queue_length(a2dp_sink_local_param.btc_aa_snk_cb.RxSbcQ);
        APPL_TRACE_DEBUG("nb:%d", nb_of_msgs_to_process);
        while (nb_of_msgs_to_process > 0) {
            if (btc_a2dp_sink_state != BTC_A2DP_SINK_STATE_ON){
                return;
            }
            p_msg = (tBT_SBC_HDR *)fixed_queue_dequeue(a2dp_sink_local_param.btc_aa_snk_cb.RxSbcQ, 0);
            if ( p_msg == NULL ) {
                APPL_TRACE_ERROR("Insufficient data in que ");
                break;
            }
             // DEBUG by nishi
            APPL_TRACE_DEBUG("%s():#2 call btc_a2dp_sink_handle_inc_media() SBC,APTX",__func__);

            switch(btc_aa_snk_cb.codec_type){
            	// SBC codec
            	case BTA_AV_CODEC_SBC:
                btc_a2dp_sink_handle_inc_media(p_msg);
            	break;
            	// APTX codec   add by nishi for APTX support 2019.1.25
            	case BTA_AV_CODEC_VEND:
                btc_a2dp_sink_handle_inc_media_Aptx(p_msg);
            	break;
            }
            // deque �̎��s���`�F�b�N by nishi
            APPL_TRACE_DEBUG("%s():#3 dequeue",__func__);
            osi_free(p_msg);
            nb_of_msgs_to_process--;
        }
        APPL_TRACE_DEBUG(" Process Frames - ");
    }
}
#endif

/*******************************************************************************
 **
 ** Function         btc_a2dp_sink_handle_decoder_reset
 **
 ** Description
 **
 ** Returns          void
 **
 *******************************************************************************/
static void btc_a2dp_sink_handle_decoder_reset(tBTC_MEDIA_SINK_CFG_UPDATE *p_msg)
{
    tBTC_MEDIA_SINK_CFG_UPDATE *p_buf = p_msg;
    APPL_TRACE_EVENT("%s():#1，mode: 0x%02x", __FUNCTION__,p_buf->codec_info[2]);
    switch(p_buf->codec_info[2]){
    case BTA_AV_CODEC_SBC:
    	btc_a2dp_sink_handle_decoder_reset_Sbc(p_msg);
    	break;
    case BTA_AV_CODEC_VEND:
    	//btc_a2dp_sink_handle_decoder_reset_Aptx(p_msg);
    	btc_a2dp_sink_handle_decoder_reset_ldac(p_msg);
    	break;
    }
}

/*******************************************************************************
 **
 ** Function         btc_a2dp_sink_handle_decoder_reset for SBC
 **
 ** Description		rename btc_a2dp_sink_handle_decoder_reset_Sbc from btc_a2dp_sink_handle_decoder_reset
 **                 by nishi for APTX support 2019.1.22
 **
 ** Returns          void
 **
 *******************************************************************************/
static void btc_a2dp_sink_handle_decoder_reset_Sbc(tBTC_MEDIA_SINK_CFG_UPDATE *p_msg)
{
    tBTC_MEDIA_SINK_CFG_UPDATE *p_buf = p_msg;
    tA2D_STATUS a2d_status;
    tA2D_SBC_CIE sbc_cie;
    OI_STATUS       status;
    UINT32          freq_multiple = 48 * 20; /* frequency multiple for 20ms of data , initialize with 48K*/
    UINT32          num_blocks = 16;
    UINT32          num_subbands = 8;

    APPL_TRACE_EVENT("%s():#1", __FUNCTION__);
    APPL_TRACE_EVENT("\tp_codec_info[%x:%x:%x:%x:%x:%x:%x]",
    			p_buf->codec_info[0],p_buf->codec_info[1], p_buf->codec_info[2],
				p_buf->codec_info[3],p_buf->codec_info[4], p_buf->codec_info[5], p_buf->codec_info[6]);

    a2d_status = A2D_ParsSbcInfo(&sbc_cie, p_buf->codec_info, FALSE,"btc_a2dp_sink_handle_decoder_reset_Sbc():#1");
    if (a2d_status != A2D_SUCCESS) {
        APPL_TRACE_ERROR("ERROR dump_codec_info A2D_ParsSbcInfo fail:%d\n", a2d_status);
        return;
    }

    a2dp_sink_local_param.btc_aa_snk_cb.sample_rate = btc_a2dp_sink_get_track_frequency(p_buf->codec_info[2],sbc_cie.samp_freq);
    a2dp_sink_local_param.btc_aa_snk_cb.channel_count = btc_a2dp_sink_get_track_channel_count(p_buf->codec_info[2],sbc_cie.ch_mode);
    a2dp_sink_local_param.btc_aa_snk_cb.codec_type = p_buf->codec_info[2];

    a2dp_sink_local_param.btc_aa_snk_cb.rx_flush = FALSE;
    APPL_TRACE_EVENT("Reset to sink role");
    status = OI_CODEC_SBC_DecoderReset(&a2dp_sink_local_param.context, a2dp_sink_local_param.contextData,
                                        sizeof(a2dp_sink_local_param.contextData), 2, 2, FALSE, FALSE);
    if (!OI_SUCCESS(status)) {
        APPL_TRACE_ERROR("OI_CODEC_SBC_DecoderReset failed with error code %d\n", status);
    }

    btc_a2dp_control_set_datachnl_stat(TRUE);

    switch (sbc_cie.samp_freq) {
    case A2D_SBC_IE_SAMP_FREQ_16:
        APPL_TRACE_DEBUG("\tsamp_freq:%d (16000)\n", sbc_cie.samp_freq);
        freq_multiple = 16 * 20;
        break;
    case A2D_SBC_IE_SAMP_FREQ_32:
        APPL_TRACE_DEBUG("\tsamp_freq:%d (32000)\n", sbc_cie.samp_freq);
        freq_multiple = 32 * 20;
        break;
    case A2D_SBC_IE_SAMP_FREQ_44:
        APPL_TRACE_DEBUG("\tsamp_freq:%d (44100)\n", sbc_cie.samp_freq);
        freq_multiple = 441 * 2;
        break;
    case A2D_SBC_IE_SAMP_FREQ_48:
        APPL_TRACE_DEBUG("\tsamp_freq:%d (48000)\n", sbc_cie.samp_freq);
        freq_multiple = 48 * 20;
        break;
    default:
        APPL_TRACE_DEBUG(" Unknown Frequency ");
        break;
    }

    switch (sbc_cie.ch_mode) {
    case A2D_SBC_IE_CH_MD_MONO:
        APPL_TRACE_DEBUG("\tch_mode:%d (Mono)\n", sbc_cie.ch_mode);
        break;
    case A2D_SBC_IE_CH_MD_DUAL:
        APPL_TRACE_DEBUG("\tch_mode:%d (DUAL)\n", sbc_cie.ch_mode);
        break;
    case A2D_SBC_IE_CH_MD_STEREO:
        APPL_TRACE_DEBUG("\tch_mode:%d (STEREO)\n", sbc_cie.ch_mode);
        break;
    case A2D_SBC_IE_CH_MD_JOINT:
        APPL_TRACE_DEBUG("\tch_mode:%d (JOINT)\n", sbc_cie.ch_mode);
        break;
    default:
        APPL_TRACE_DEBUG(" Unknown Mode ");
        break;
    }

    switch (sbc_cie.block_len) {
    case A2D_SBC_IE_BLOCKS_4:
        APPL_TRACE_DEBUG("\tblock_len:%d (4)\n", sbc_cie.block_len);
        num_blocks = 4;
        break;
    case A2D_SBC_IE_BLOCKS_8:
        APPL_TRACE_DEBUG("\tblock_len:%d (8)\n", sbc_cie.block_len);
        num_blocks = 8;
        break;
    case A2D_SBC_IE_BLOCKS_12:
        APPL_TRACE_DEBUG("\tblock_len:%d (12)\n", sbc_cie.block_len);
        num_blocks = 12;
        break;
    case A2D_SBC_IE_BLOCKS_16:
        APPL_TRACE_DEBUG("\tblock_len:%d (16)\n", sbc_cie.block_len);
        num_blocks = 16;
        break;
    default:
        APPL_TRACE_DEBUG(" Unknown BlockLen ");
        break;
    }

    switch (sbc_cie.num_subbands) {
    case A2D_SBC_IE_SUBBAND_4:
        APPL_TRACE_DEBUG("\tnum_subbands:%d (4)\n", sbc_cie.num_subbands);
        num_subbands = 4;
        break;
    case A2D_SBC_IE_SUBBAND_8:
        APPL_TRACE_DEBUG("\tnum_subbands:%d (8)\n", sbc_cie.num_subbands);
        num_subbands = 8;
        break;
    default:
        APPL_TRACE_DEBUG(" Unknown SubBands ");
        break;
    }

    switch (sbc_cie.alloc_mthd) {
    case A2D_SBC_IE_ALLOC_MD_S:
        APPL_TRACE_DEBUG("\talloc_mthd:%d (SNR)\n", sbc_cie.alloc_mthd);
        break;
    case A2D_SBC_IE_ALLOC_MD_L:
        APPL_TRACE_DEBUG("\talloc_mthd:%d (Loudness)\n", sbc_cie.alloc_mthd);
        break;
    default:
        APPL_TRACE_DEBUG(" Unknown Allocation Method");
        break;
    }

    APPL_TRACE_EVENT("\tBit pool Min:%d Max:%d\n", sbc_cie.min_bitpool, sbc_cie.max_bitpool);

    int frames_to_process = ((freq_multiple) / (num_blocks * num_subbands)) + 1;
    APPL_TRACE_EVENT(" Frames to be processed in 20 ms %d\n", frames_to_process);
    UNUSED(frames_to_process);
}


/*******************************************************************************
 **
 ** Function         btc_a2dp_sink_handle_decoder_reset for APTX
 **
 ** Description		add by nishi for APTX support 2019.1.22
 **
 ** Returns          void
 **
 *******************************************************************************/
static void btc_a2dp_sink_handle_decoder_reset_Aptx(tBTC_MEDIA_SINK_CFG_UPDATE *p_msg)
{
    tBTC_MEDIA_SINK_CFG_UPDATE *p_buf = p_msg;
    tA2D_STATUS a2d_status;
    tA2DP_APTX_CIE aptx_cie;
    OI_STATUS       status;
    UINT32          freq_multiple = 48 * 20; /* frequency multiple for 20ms of data , initialize with 48K*/
    //UINT32          num_blocks = 16;
    //UINT32          num_subbands = 8;

    //int freq = 48000;

    APPL_TRACE_EVENT("%s():#1", __FUNCTION__);
    APPL_TRACE_EVENT("\tp_codec_info[%x:%x:%x:%x:%x:%x:%x:%x:%x:%x]",
    				p_buf->codec_info[0],p_buf->codec_info[1], p_buf->codec_info[2], p_buf->codec_info[3],
                    p_buf->codec_info[4], p_buf->codec_info[5], p_buf->codec_info[6],
					p_buf->codec_info[7], p_buf->codec_info[8], p_buf->codec_info[9]);


    a2d_status = A2DP_ParseInfoAptx(&aptx_cie,p_buf->codec_info, FALSE,"btc_a2dp_sink_handle_decoder_reset_Aptx");
    if (a2d_status != A2D_SUCCESS) {
        APPL_TRACE_ERROR("ERROR dump_codec_info A2DP_ParseInfoAptx:%d\n", a2d_status);
        return;
    }

    // set sampleRate
    a2dp_sink_local_param.btc_aa_snk_cb.sample_rate = btc_a2dp_sink_get_track_frequency(p_buf->codec_info[2],aptx_cie.sampleRate);
    // set channelMode  stero/mono
    a2dp_sink_local_param.btc_aa_snk_cb.channel_count = btc_a2dp_sink_get_track_channel_count(p_buf->codec_info[2],aptx_cie.channelMode);
    // set codec_type
    a2dp_sink_local_param.btc_aa_snk_cb.codec_type = p_buf->codec_info[2];

    a2dp_sink_local_param.btc_aa_snk_cb.rx_flush = FALSE;

    APPL_TRACE_EVENT("Reset to sink role");
    status = OI_CODEC_SBC_DecoderReset(&a2dp_sink_local_param.context, a2dp_sink_local_param.contextData,
                                       sizeof(a2dp_sink_local_param.contextData), 2, 2, FALSE, FALSE);

    if (!OI_SUCCESS(status)) {
        APPL_TRACE_ERROR("OI_CODEC_SBC_DecoderReset failed with error code %d", status);
    }

    if(ctx!=NULL){
    	aptx_finish(ctx);
    	ctx=NULL;
    }
    // aptx decoder ������
    ctx=aptx_init(0);
    if (!ctx) {
        APPL_TRACE_ERROR("%s():#2 APTX DecoderReset failed", __func__);
        return;
    }

    btc_a2dp_control_set_datachnl_stat(TRUE);

    // sampleRate
    switch (aptx_cie.sampleRate) {
    case A2DP_APTX_SAMPLERATE_44100:
    	APPL_TRACE_EVENT("\tsamp_freq:%d (44100)", aptx_cie.sampleRate);
        freq_multiple = 441 * 2;
        break;
    case A2DP_APTX_SAMPLERATE_48000:
    	APPL_TRACE_EVENT("\tsamp_freq:%d (48000)", aptx_cie.sampleRate);
        freq_multiple = 48 * 20;
        break;
    default:
    	APPL_TRACE_EVENT("\tUnknown Frequency ");
        break;
    }

    // channelMode
    switch (aptx_cie.channelMode) {
    case A2DP_APTX_CHANNELS_MONO:
    	APPL_TRACE_EVENT("\tch_mode:%d (Mono)", aptx_cie.channelMode);
        break;
    case A2DP_APTX_CHANNELS_STEREO:
    	APPL_TRACE_EVENT("\tch_mode:%d (STEREO)", aptx_cie.channelMode);
        break;
    default:
    	APPL_TRACE_EVENT("\tUnknown Mode ");
        break;
    }


    //APPL_TRACE_EVENT("\tBit pool Min:%d Max:%d\n", sbc_cie.min_bitpool, sbc_cie.max_bitpool);

    //int frames_to_process = ((freq_multiple) / (num_blocks * num_subbands)) + 1;
    //APPL_TRACE_EVENT(" Frames to be processed in 20 ms %d\n", frames_to_process);
}

/*******************************************************************************
 **
 ** Function         btc_a2dp_sink_handle_decoder_reset for APTX
 **
 ** Description		add by nishi for APTX support 2019.1.22
 **
 ** Returns          void
 **
 *******************************************************************************/
static void btc_a2dp_sink_handle_decoder_reset_ldac(tBTC_MEDIA_SINK_CFG_UPDATE *p_msg)
{
    tBTC_MEDIA_SINK_CFG_UPDATE *p_buf = p_msg;
    tA2D_STATUS a2d_status;
    tA2DP_LDAC_CIE ldac_cie;
    OI_STATUS       status;
    //UINT32          num_blocks = 16;
    //UINT32          num_subbands = 8;

    //int freq = 48000;

    APPL_TRACE_EVENT("%s():#1", __FUNCTION__);
    APPL_TRACE_EVENT("\tp_codec_info[%x:%x:%x:%x:%x:%x:%x:%x:%x:%x]",
                     p_buf->codec_info[0],p_buf->codec_info[1], p_buf->codec_info[2], p_buf->codec_info[3],
                     p_buf->codec_info[4], p_buf->codec_info[5], p_buf->codec_info[6],
                     p_buf->codec_info[7], p_buf->codec_info[8], p_buf->codec_info[9]);


    a2d_status = A2DP_ParseInfoLDAC(&ldac_cie,p_buf->codec_info, FALSE,"btc_a2dp_sink_handle_decoder_reset_LDAC");
    if (a2d_status != A2D_SUCCESS) {
        APPL_TRACE_ERROR("ERROR dump_codec_info A2DP_ParseInfoLDAC:%d\n", a2d_status);
        return;
    }

    // set sampleRate
    a2dp_sink_local_param.btc_aa_snk_cb.sample_rate = btc_a2dp_sink_get_track_frequency_ldac(ldac_cie.sampleRate);
    // set channelMode  stero/mono
    a2dp_sink_local_param.btc_aa_snk_cb.channel_count = btc_a2dp_sink_get_track_channel_count_ldac(ldac_cie.channelMode);
    // set codec_type
    a2dp_sink_local_param.btc_aa_snk_cb.codec_type = p_buf->codec_info[2];

    a2dp_sink_local_param.btc_aa_snk_cb.rx_flush = FALSE;

    APPL_TRACE_EVENT("Reset to sink role");
    status = OI_CODEC_SBC_DecoderReset(&a2dp_sink_local_param.context, a2dp_sink_local_param.contextData,
                                       sizeof(a2dp_sink_local_param.contextData), 2, 2, FALSE, FALSE);

    if (!OI_SUCCESS(status)) {
        APPL_TRACE_ERROR("OI_CODEC_SBC_DecoderReset failed with error code %d", status);
    }

    if(ldacctx!=NULL){
        free(ldacctx);
        ldacctx=NULL;
    }
    ldacctx=malloc(sizeof(ldacdec_t));
    if(!ldacctx)
    {
        APPL_TRACE_ERROR("%s():#2 LDAC DecoderAlloc failed", __func__);
        return;
    }
    ldacdecInit(ldacctx);

    btc_a2dp_control_set_datachnl_stat(TRUE);

    // sampleRate
    switch (ldac_cie.sampleRate) {
        case A2DP_LDAC_SAMPLERATE_44100:
        case A2DP_LDAC_SAMPLERATE_48000:
        case A2DP_LDAC_SAMPLERATE_88200:
        case A2DP_LDAC_SAMPLERATE_96000:
        case A2DP_LDAC_SAMPLERATE_176400:
        case A2DP_LDAC_SAMPLERATE_192000:
        APPL_TRACE_EVENT("\tsamp_freq:%d ()", ldac_cie.sampleRate);
            break;
        default:
        APPL_TRACE_EVENT("\tUnknown Frequency ");
            break;
    }

    // channelMode
    switch (ldac_cie.channelMode) {
        case A2DP_LDAC_CHANNELS_MONO:
        APPL_TRACE_EVENT("\tch_mode:%d (Mono)", ldac_cie.channelMode);
            break;
        case A2DP_LDAC_CHANNELS_STEREO:
        APPL_TRACE_EVENT("\tch_mode:%d (STEREO)", ldac_cie.channelMode);
            break;
        case A2DP_LDAC_CHANNELS_DUAL_CHANNEL:
        APPL_TRACE_EVENT("\tch_mode:%d (dual_channel)", ldac_cie.channelMode);
            break;
        default:
        APPL_TRACE_EVENT("\tUnknown Mode ");
            break;
    }


    //APPL_TRACE_EVENT("\tBit pool Min:%d Max:%d\n", sbc_cie.min_bitpool, sbc_cie.max_bitpool);

    //int frames_to_process = ((freq_multiple) / (num_blocks * num_subbands)) + 1;
    //APPL_TRACE_EVENT(" Frames to be processed in 20 ms %d\n", frames_to_process);
}


/*******************************************************************************
 **
 ** Function         btc_a2dp_sink_handle_inc_media
 **
 ** Description
 **
 ** Returns          void
 **
 *******************************************************************************/
static void btc_a2dp_sink_handle_inc_media(tBT_SBC_HDR *p_msg)
{
    UINT8 *sbc_start_frame = ((UINT8 *)(p_msg + 1) + p_msg->offset + 1);
    int count;
    UINT32 pcmBytes, availPcmBytes;
    OI_INT16 *pcmDataPointer = a2dp_sink_local_param.pcmData; /*Will be overwritten on next packet receipt*/
    OI_STATUS status;
    int num_sbc_frames = p_msg->num_frames_to_be_processed;
    UINT32 sbc_frame_len = p_msg->len - 1;
    availPcmBytes = sizeof(a2dp_sink_local_param.pcmData);

    /* XXX: Check if the below check is correct, we are checking for peer to be sink when we are sink */
    if (btc_av_get_peer_sep() == AVDT_TSEP_SNK || (a2dp_sink_local_param.btc_aa_snk_cb.rx_flush)) {
        APPL_TRACE_DEBUG(" State Changed happened in this tick ");
        return;
    }

    // ignore data if no one is listening
    if (!btc_a2dp_control_get_datachnl_stat()) {
        return;
    }

    APPL_TRACE_DEBUG("Number of sbc frames %d, frame_len %d\n", num_sbc_frames, sbc_frame_len);

    for (count = 0; count < num_sbc_frames && sbc_frame_len != 0; count ++) {
        pcmBytes = availPcmBytes;
        status = OI_CODEC_SBC_DecodeFrame(&a2dp_sink_local_param.context, (const OI_BYTE **)&sbc_start_frame,
                                          (OI_UINT32 *)&sbc_frame_len,
                                          (OI_INT16 *)pcmDataPointer,
                                          (OI_UINT32 *)&pcmBytes);
        if (!OI_SUCCESS(status)) {
            APPL_TRACE_ERROR("Decoding failure: %d\n", status);
            break;
        }
        availPcmBytes -= pcmBytes;
        pcmDataPointer += pcmBytes / 2;
        p_msg->offset += (p_msg->len - 1) - sbc_frame_len;
        p_msg->len = sbc_frame_len + 1;
    }

    btc_a2d_data_cb_to_app((uint8_t *)a2dp_sink_local_param.pcmData, (sizeof(a2dp_sink_local_param.pcmData) - availPcmBytes));
}
/*******************************************************************************
 **
 ** Function         btc_a2dp_sink_handle_inc_media_APTX
 **
 ** Description
 **           add by nishi for support APTX  2019.1.25
 **
 ** Returns          void
 **
 *******************************************************************************/
static void btc_a2dp_sink_handle_inc_media_Aptx(tBT_SBC_HDR *p_msg)
{
	//p_msg->len--;
	//p_msg->offset++;

    int count;
    UINT32 pcmBytes, availPcmBytes;
    OI_INT16 *pcmDataPointer = a2dp_sink_local_param.pcmData; /*Will be overwritten on next packet receipt*/
    OI_STATUS status;
    int num_sbc_frames = (int)(*(UINT8 *)(p_msg+1) & 0x0f);

    //UINT8 *sbc_start_frame = ((UINT8 *)(p_msg + 1) + p_msg->offset + 1);
    UINT8 *sbc_start_frame = ((UINT8 *)(p_msg + 1) + p_msg->offset);  // tBT_SBC_HDR ���΂����ʒu + p_msg->offset

    //UINT32 sbc_frame_len = p_msg->len - 1;
    UINT32 sbc_frame_len = p_msg->len;

    availPcmBytes = sizeof(a2dp_sink_local_param.pcmData);

    size_t processed;
    size_t offset;
    size_t sample_size;

    uint32_t len0,len;

    //UINT8 *c_p;

    // DEBUG by nishi
//    APPL_TRACE_DEBUG("%s(): #1 start!",__func__);
//    APPL_TRACE_DEBUG("\tp_msg->num_frames.=x%x, len=x%x, offset=x%x, layer_specific=x%x",
//    		p_msg->num_frames_to_be_processed,p_msg->len,p_msg->offset,p_msg->layer_specific);
//    APPL_TRACE_DEBUG("\tsbc_start_frame[0-5]=%x:%x:%x:%x:%x:%x",
//    		sbc_start_frame[0],sbc_start_frame[1],sbc_start_frame[2],sbc_start_frame[3],sbc_start_frame[4],sbc_start_frame[5]);

    /* XXX: Check if the below check is correct, we are checking for peer to be sink when we are sink */
    if (btc_av_get_peer_sep() == AVDT_TSEP_SNK || (a2dp_sink_local_param.btc_aa_snk_cb.rx_flush)) {
    	APPL_TRACE_WARNING(" State Changed happened in this tick");
        return;
    }

    // ignore data if no one is listening
    if (!btc_a2dp_control_get_datachnl_stat()) {
    	APPL_TRACE_WARNING(" ignore data if no one is listening");
        return;
    }

    //c_p=(UINT8 *)p_msg;
    //APPL_TRACE_EVENT("p_msg[0-7] [8] data=[0-3]=%x:%x:%x:%x:%x:%x:%x:%x %x %x:%x:%x:%x",
    //		c_p[0],c_p[1],c_p[2],c_p[3],c_p[4],c_p[5],c_p[6],c_p[7],c_p[8],
	//		sbc_start_frame[0],sbc_start_frame[1],sbc_start_frame[2],sbc_start_frame[3]);

//    APPL_TRACE_EVENT("Number of aptx frames %d, p_msg->len %d ,p_msg->offset %d", num_sbc_frames, p_msg->len,p_msg->offset);


    sample_size = aptx_get_sample_size(ctx);

   	// APTX codec   add by nishi for APTX support 2019.1.25
    offset=0;
   	//for (count = 0; count < num_sbc_frames && sbc_frame_len > 0; count ++) {
   	//for (count = 0; sbc_frame_len > 0; count ++) {
   	for (count = 0; sbc_frame_len >= sample_size; count ++) {
   		pcmBytes = availPcmBytes;

//   		APPL_TRACE_DEBUG("%s(): #3 sbc_frame_len=%d, availPcmBytes=%d",__func__,sbc_frame_len,availPcmBytes);
		processed = aptx_decode_16bit(
				ctx,								// struct aptx_context *ctx
				(unsigned char *)(sbc_start_frame + offset),	// const unsigned char *input ���̓o�b�t�@�|�C���^
				(size_t)sbc_frame_len,				// size_t input_size  ���̓f�[�^��
				(int16_t *)pcmDataPointer,	// unsigned char *output  �o�̓o�b�t�@
				(size_t)availPcmBytes,				// size_t output_size  �o�̓o�b�t�@��
				(size_t *)&pcmBytes);				// size_t *written   �o�͂��ꂽ�f�[�^��

//		APPL_TRACE_DEBUG("%s(): #4 processed=%d, pcmBytes=%d",__func__,processed,pcmBytes);

		offset += processed;
   		sbc_frame_len -= processed;

   		p_msg->offset = offset;				// BT_SBC_HDR->offset �I�t�Z�b�g�̍X�V
   		p_msg->len = sbc_frame_len;			// BT_SBC_HDR->len �f�[�^���̍X�V

		// ��x������ break�����܂���B
		//break;

		availPcmBytes -= pcmBytes;			// �c��o�̓o�b�t�@�T�C�Y�@�̍X�V
   		pcmDataPointer += pcmBytes / 2;		// �o�̓o�b�t�@�|�C���^�̍X�V

   		if(pcmBytes == 0)
   			break;
   	}

    //btc_a2d_data_cb_to_app((uint8_t *)btc_sbc_pcm_data, (BTC_SBC_DEC_PCM_DATA_LEN * sizeof(OI_INT16) - availPcmBytes));
   	//len0=BTC_SBC_DEC_PCM_DATA_LEN * sizeof(OI_INT16);
   	len=sizeof(a2dp_sink_local_param.pcmData) - availPcmBytes;
    //len=pcmDataPointer-btc_sbc_pcm_data;
    btc_a2d_data_cb_to_app((uint8_t *)a2dp_sink_local_param.pcmData, len);

    // DEBUG by nishi
    //APPL_TRACE_EVENT("%s(): #5 len0=%d, len=%d, availPcmBytes=%d",__func__,len0,len,availPcmBytes);

}
/*******************************************************************************
 **
 ** Function         btc_a2dp_sink_handle_inc_media_LDAC
 **
 ** Description
 **           add by hailin for support LDAC  2021.9.9
 **
 ** Returns          void
 **
 *******************************************************************************/
static void btc_a2dp_sink_handle_inc_media_ldac(tBT_SBC_HDR *p_msg)
{
    //p_msg->len--;
    //p_msg->offset++;

    int count;
    UINT32 pcmBytes, availPcmBytes;
    OI_INT16 *pcmDataPointer = a2dp_sink_local_param.pcmData; /*Will be overwritten on next packet receipt*/
    OI_STATUS status;
    int num_sbc_frames = (int)(*(UINT8 *)(p_msg+1) & 0x0f);

    //UINT8 *sbc_start_frame = ((UINT8 *)(p_msg + 1) + p_msg->offset + 1);
    UINT8 *sbc_start_frame = ((UINT8 *)(p_msg + 1) + p_msg->offset);  // tBT_SBC_HDR ���΂����ʒu + p_msg->offset

    //UINT32 sbc_frame_len = p_msg->len - 1;
    UINT32 sbc_frame_len = p_msg->len;

    availPcmBytes = sizeof(a2dp_sink_local_param.pcmData);

    size_t processed;
    size_t offset;
    size_t sample_size;

    uint32_t len0,len;

    //UINT8 *c_p;

    // DEBUG by nishi
//    APPL_TRACE_DEBUG("%s(): #1 start!",__func__);
//    APPL_TRACE_DEBUG("\tp_msg->num_frames.=x%x, len=x%x, offset=x%x, layer_specific=x%x",
//    		p_msg->num_frames_to_be_processed,p_msg->len,p_msg->offset,p_msg->layer_specific);
//    APPL_TRACE_DEBUG("\tsbc_start_frame[0-5]=%x:%x:%x:%x:%x:%x",
//    		sbc_start_frame[0],sbc_start_frame[1],sbc_start_frame[2],sbc_start_frame[3],sbc_start_frame[4],sbc_start_frame[5]);

    /* XXX: Check if the below check is correct, we are checking for peer to be sink when we are sink */
    if (btc_av_get_peer_sep() == AVDT_TSEP_SNK || (a2dp_sink_local_param.btc_aa_snk_cb.rx_flush)) {
        APPL_TRACE_WARNING(" State Changed happened in this tick");
        return;
    }

    // ignore data if no one is listening
    if (!btc_a2dp_control_get_datachnl_stat()) {
        APPL_TRACE_WARNING(" ignore data if no one is listening");
        return;
    }

    //c_p=(UINT8 *)p_msg;
    //APPL_TRACE_EVENT("p_msg[0-7] [8] data=[0-3]=%x:%x:%x:%x:%x:%x:%x:%x %x %x:%x:%x:%x",
    //		c_p[0],c_p[1],c_p[2],c_p[3],c_p[4],c_p[5],c_p[6],c_p[7],c_p[8],
    //		sbc_start_frame[0],sbc_start_frame[1],sbc_start_frame[2],sbc_start_frame[3]);

    APPL_TRACE_EVENT("Number of LDAC frames %d, p_msg->len %d ,p_msg->offset %d", num_sbc_frames, p_msg->len,p_msg->offset);





    offset=0;
    while(sbc_frame_len>0)
    {
        int bytesUsed = 0;
        int ret = ldacDecode( ldacctx, (unsigned char *)(sbc_start_frame + offset), (int16_t *)pcmDataPointer, &bytesUsed );
        if( ret < 0 ){
            APPL_TRACE_ERROR("%s(): LDAC decode error!",__func__);
            processed=sbc_frame_len;
        }
        else
            processed=bytesUsed;
        pcmBytes=ldacctx->frame.frameSamples*ldacdecGetChannelCount( ldacctx )*2;


        offset += processed;
        sbc_frame_len -= processed;

        p_msg->offset = offset;				// BT_SBC_HDR->offset �I�t�Z�b�g�̍X�V
        p_msg->len = sbc_frame_len;			// BT_SBC_HDR->len �f�[�^���̍X�V

        // ��x������ break�����܂���B
        //break;

        availPcmBytes -= pcmBytes;			// �c��o�̓o�b�t�@�T�C�Y�@�̍X�V
        pcmDataPointer += pcmBytes / 2;		// �o�̓o�b�t�@�|�C���^�̍X�V

        if(pcmBytes == 0)
            break;
    }

    //btc_a2d_data_cb_to_app((uint8_t *)btc_sbc_pcm_data, (BTC_SBC_DEC_PCM_DATA_LEN * sizeof(OI_INT16) - availPcmBytes));
    //len0=BTC_SBC_DEC_PCM_DATA_LEN * sizeof(OI_INT16);
    len=sizeof(a2dp_sink_local_param.pcmData) - availPcmBytes;
    //len=pcmDataPointer-btc_sbc_pcm_data;
    btc_a2d_data_cb_to_app((uint8_t *)a2dp_sink_local_param.pcmData, len);

    // DEBUG by nishi
    //APPL_TRACE_EVENT("%s(): #5 len0=%d, len=%d, availPcmBytes=%d",__func__,len0,len,availPcmBytes);

}

/*******************************************************************************
 **
 ** Function         btc_a2dp_sink_rx_flush_req
 **
 ** Description
 **
 ** Returns          TRUE is success
 **
 *******************************************************************************/
BOOLEAN btc_a2dp_sink_rx_flush_req(void)
{
    if (fixed_queue_is_empty(a2dp_sink_local_param.btc_aa_snk_cb.RxSbcQ) == TRUE) { /*  Que is already empty */
        return TRUE;
    }

    return btc_a2dp_sink_ctrl(BTC_MEDIA_FLUSH_AA_RX, NULL);
}

/*******************************************************************************
 **
 ** Function         btc_a2dp_sink_rx_flush
 **
 ** Description
 **
 ** Returns          void
 **
 *******************************************************************************/
static void btc_a2dp_sink_rx_flush(void)
{
    /* Flush all enqueued SBC  buffers (encoded) */
    APPL_TRACE_DEBUG("btc_a2dp_sink_rx_flush");

    btc_a2dp_sink_flush_q(a2dp_sink_local_param.btc_aa_snk_cb.RxSbcQ);
}

static int btc_a2dp_sink_get_track_frequency(UINT8 codec_id,UINT8 frequency)
{
    int freq = 48000;
    switch(codec_id){
    // SBC
    case BTA_AV_CODEC_SBC:
    	switch (frequency) {
    	case A2D_SBC_IE_SAMP_FREQ_16:
    		freq = 16000;
    		break;
    	case A2D_SBC_IE_SAMP_FREQ_32:
    		freq = 32000;
    		break;
    	case A2D_SBC_IE_SAMP_FREQ_44:
    		freq = 44100;
    		break;
    	case A2D_SBC_IE_SAMP_FREQ_48:
    		freq = 48000;
    		break;
    	}
    	break;
    // APTX
   	case BTA_AV_CODEC_VEND:
       	switch (frequency) {
        case A2DP_APTX_SAMPLERATE_44100:
       		freq = 44100;
            break;
        case A2DP_APTX_SAMPLERATE_48000:
       		freq = 48000;
            break;
       	}
    }
    return freq;
}
static int btc_a2dp_sink_get_track_frequency_ldac(UINT8 frequency)
{
    int freq = 48000;
    switch (frequency) {
        case A2DP_LDAC_SAMPLERATE_44100:
            freq = 44100;
            break;
        case A2DP_LDAC_SAMPLERATE_48000:
            freq = 48000;
            break;
        case A2DP_LDAC_SAMPLERATE_88200:
            freq = 88200;
            break;
        case A2DP_LDAC_SAMPLERATE_96000:
            freq = 9600;
            break;
        case A2DP_LDAC_SAMPLERATE_176400:
            freq = 176400;
            break;
        case A2DP_LDAC_SAMPLERATE_192000:
            freq = 192000;
            break;
        default:
            break;
    }
    return freq;
}

static int btc_a2dp_sink_get_track_channel_count(UINT8 codec_id,UINT8 channeltype)
{
    int count = 1;
    switch(codec_id){
    // SBC
    case BTA_AV_CODEC_SBC:
    	switch (channeltype) {
    	case A2D_SBC_IE_CH_MD_MONO:
    		count = 1;
    		break;
    	case A2D_SBC_IE_CH_MD_DUAL:
    	case A2D_SBC_IE_CH_MD_STEREO:
    	case A2D_SBC_IE_CH_MD_JOINT:
    		count = 2;
        break;
    	}
    break;
    // APTX
   	case BTA_AV_CODEC_VEND:
    	switch (channeltype) {
    	case A2DP_APTX_CHANNELS_MONO:
    		count = 1;
    		break;
    	case A2DP_APTX_CHANNELS_STEREO:
    		count = 2;
        	break;
    	}
    	break;
    }
    return count;
}
static int btc_a2dp_sink_get_track_channel_count_ldac(UINT8 channeltype)
{
    int count = 1;

    switch (channeltype) {
        case A2DP_LDAC_CHANNELS_MONO:
            count = 1;
            break;
        case A2DP_LDAC_CHANNELS_STEREO:
        case A2DP_LDAC_CHANNELS_DUAL_CHANNEL:
            count = 2;
            break;
        default:
            break;
    }
    return count;
}

/*******************************************************************************
 **
 ** Function         btc_a2dp_sink_enque_buf
 **
 ** Description      This function is called by the av_co to fill A2DP Sink Queue
 **
 **
 ** Returns          size of the queue
 *******************************************************************************/
UINT8 btc_a2dp_sink_enque_buf(BT_HDR *p_pkt)
{
    tBT_SBC_HDR *p_msg;
    int32_t e_id;
    if (btc_a2dp_sink_state != BTC_A2DP_SINK_STATE_ON){
        return 0;
    }

    if (a2dp_sink_local_param.btc_aa_snk_cb.rx_flush == TRUE) { /* Flush enabled, do not enque*/
        return fixed_queue_length(a2dp_sink_local_param.btc_aa_snk_cb.RxSbcQ);
    }

    if (fixed_queue_length(a2dp_sink_local_param.btc_aa_snk_cb.RxSbcQ) >= MAX_OUTPUT_A2DP_SNK_FRAME_QUEUE_SZ) {
        APPL_TRACE_WARNING("Pkt dropped\n");
        return fixed_queue_length(a2dp_sink_local_param.btc_aa_snk_cb.RxSbcQ);
    }

    APPL_TRACE_DEBUG("btc_a2dp_sink_enque_buf + ");

    /* allocate and Queue this buffer */
    if ((p_msg = (tBT_SBC_HDR *) osi_malloc(sizeof(tBT_SBC_HDR) +
                                            p_pkt->offset + p_pkt->len)) != NULL) {
        memcpy(p_msg, p_pkt, (sizeof(BT_HDR) + p_pkt->offset + p_pkt->len));
        p_msg->num_frames_to_be_processed = (*((UINT8 *)(p_msg + 1) + p_msg->offset)) & 0x0f;
        APPL_TRACE_VERBOSE("btc_a2dp_sink_enque_buf %d + \n", p_msg->num_frames_to_be_processed);
        fixed_queue_enqueue(a2dp_sink_local_param.btc_aa_snk_cb.RxSbcQ, p_msg, FIXED_QUEUE_MAX_TIMEOUT);

        if (fixed_queue_length(a2dp_sink_local_param.btc_aa_snk_cb.RxSbcQ) >= JITTER_BUFFER_WATER_LEVEL) {
            e_id = 0;
            if(xQueueSend(btc_aa_snk_data_queue,&e_id,0)!= pdTRUE){
                APPL_TRACE_DEBUG("%s():#5 xQueueSend error!",__func__);
            }
//            if (osi_sem_take(&a2dp_sink_local_param.btc_aa_snk_cb.post_sem, 0) == 0) {
//                btc_a2dp_sink_data_post();
//            }
        }
    } else {
        /* let caller deal with a failed allocation */
        APPL_TRACE_WARNING("btc_a2dp_sink_enque_buf No Buffer left - ");
    }
    return fixed_queue_length(a2dp_sink_local_param.btc_aa_snk_cb.RxSbcQ);
}

static void btc_a2dp_sink_handle_clear_track (void)
{
    APPL_TRACE_DEBUG("%s", __FUNCTION__);
}

/*******************************************************************************
 **
 ** Function         btc_a2dp_sink_flush_q
 **
 ** Description
 **
 ** Returns          void
 **
 *******************************************************************************/
static void btc_a2dp_sink_flush_q(fixed_queue_t *p_q)
{
    while (! fixed_queue_is_empty(p_q)) {
        osi_free(fixed_queue_dequeue(p_q, 0));
    }
}

static void btc_a2dp_sink_thread_init(UNUSED_ATTR void *context)
{
    APPL_TRACE_EVENT("%s\n", __func__);
    memset(&a2dp_sink_local_param.btc_aa_snk_cb, 0, sizeof(a2dp_sink_local_param.btc_aa_snk_cb));

    btc_a2dp_sink_state = BTC_A2DP_SINK_STATE_ON;
    if (!a2dp_sink_local_param.btc_aa_snk_cb.post_sem) {
        osi_sem_new(&a2dp_sink_local_param.btc_aa_snk_cb.post_sem, 1, 1);
    }
    a2dp_sink_local_param.btc_aa_snk_cb.RxSbcQ = fixed_queue_new(QUEUE_SIZE_MAX);

    btc_aa_snk_data_queue = xQueueCreate(MAX_OUTPUT_A2DP_SNK_FRAME_QUEUE_SZ + 2, sizeof(int32_t));
    configASSERT(btc_aa_snk_data_queue);

    btc_a2dp_control_init();
}

static void btc_a2dp_sink_thread_cleanup(UNUSED_ATTR void *context)
{
    btc_a2dp_control_set_datachnl_stat(FALSE);
    /* Clear task flag */
    btc_a2dp_sink_state = BTC_A2DP_SINK_STATE_OFF;

    btc_a2dp_control_cleanup();

    fixed_queue_free(a2dp_sink_local_param.btc_aa_snk_cb.RxSbcQ, osi_free_func);

    a2dp_sink_local_param.btc_aa_snk_cb.RxSbcQ = NULL;

    if (a2dp_sink_local_param.btc_aa_snk_cb.post_sem) {
        osi_sem_free(&a2dp_sink_local_param.btc_aa_snk_cb.post_sem);
        a2dp_sink_local_param.btc_aa_snk_cb.post_sem = NULL;
    }
}

#endif /* BTC_AV_SINK_INCLUDED */
