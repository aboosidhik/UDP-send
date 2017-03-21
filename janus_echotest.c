
/*! \file   janus_echotest.c
 * \author Lorenzo Miniero <lorenzo@meetecho.com>
 * \copyright GNU General Public License v3
 * \brief  Janus EchoTest plugin
 * \details  This is a trivial EchoTest plugin for Janus, just used to
 * showcase the plugin interface. A peer attaching to this plugin will
 * receive back the same RTP packets and RTCP messages he sends: the
 * RTCP messages, of course, would be modified on the way by the gateway
 * to make sure they are coherent with the involved SSRCs. In order to
 * demonstrate how peer-provided messages can change the behaviour of a
 * plugin, this plugin implements a simple API based on three messages:
 * 
 * 1. a message to enable/disable audio (that is, to tell the plugin
 * whether incoming audio RTP packets need to be sent back or discarded);
 * 2. a message to enable/disable video (that is, to tell the plugin
 * whether incoming video RTP packets need to be sent back or discarded);
 * 3. a message to cap the bitrate (which would modify incoming RTCP
 * REMB messages before sending them back, in order to trick the peer into
 * thinking the available bandwidth is different).
 * 
 * \section echoapi Echo Test
 * There's a single unnamed request you can send and it's asynchronous,
 * which means all responses (successes and errors) will be delivered
 * as events with the same transaction. 
 * 
 * The request has to be formatted as follows. All the attributes are
 * optional, so any request can contain a subset of them:
 *
\verbatim
{
	"audio" : true|false,
	"video" : true|false,
	"bitrate" : <numeric bitrate value>,
	"record" : true|false,
	"filename" : <base path/filename to use for the recording>
}
\endverbatim
 *
 * \c audio instructs the plugin to do or do not bounce back audio
 * frames; \c video does the same for video; \c bitrate caps the
 * bandwidth to force on the browser encoding side (e.g., 128000 for
 * 128kbps).
 * 
 * The first request must be sent together with a JSEP offer to
 * negotiate a PeerConnection: a JSEP answer will be provided with
 * the asynchronous response notification. Subsequent requests (e.g., to
 * dynamically manipulate the bitrate while testing) have to be sent
 * without any JSEP payload attached.
 * 
 * A successful request will result in an \c ok event:
 * 
\verbatim
{
	"echotest" : "event",
	"result": "ok"
}
\endverbatim
 * 
 * An error instead will provide both an error code and a more verbose
 * description of the cause of the issue:
 * 
\verbatim
{
	"echotest" : "event",
	"error_code" : <numeric ID, check Macros below>,
	"error" : "<error description as a string>"
}
\endverbatim
 *
 * If the plugin detects a loss of the associated PeerConnection, a
 * "done" notification is triggered to inform the application the Echo
 * Test session is over:
 * 
\verbatim
{
	"echotest" : "event",
	"result": "done"
}
\endverbatim
 *
 * \ingroup plugins
 * \ref plugins
 */

#include "plugin.h"

#include <jansson.h>

#include "../debug.h"
#include "../apierror.h"
#include "../config.h"
#include "../mutex.h"
#include "../record.h"
#include "../rtcp.h"
#include "../utils.h"
#include <stdio.h>
#include "libavutil/opt.h"
#include "libavutil/time.h"
#include "libavutil/mathematics.h"
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavfilter/avfiltergraph.h"
#include "libavfilter/buffersink.h"
#include "libavfilter/buffersrc.h"
#include <vpx/vpx_encoder.h>
#include <vpx/vpx_decoder.h>
#include <vpx/vp8cx.h>
#include <vpx/vp8dx.h>
#define dec_interface (&vpx_codec_vp8_dx_algo)
#define interface (&vpx_codec_vp8_cx_algo)

#define fourcc    0x30385056

#define IVF_FILE_HDR_SZ  (32)
#define IVF_FRAME_HDR_SZ (12)


static void mem_put_le16(char *mem, unsigned int val) {
    mem[0] = val;
    mem[1] = val>>8;
}

static void mem_put_le32(char *mem, unsigned int val) {
    mem[0] = val;
    mem[1] = val>>8;
    mem[2] = val>>16;
    mem[3] = val>>24;
}

static void write_ivf_file_header(FILE *outfile,
                                  const vpx_codec_enc_cfg_t *cfg,
                                  int frame_cnt) {
    char header[32];

    if(cfg->g_pass != VPX_RC_ONE_PASS && cfg->g_pass != VPX_RC_LAST_PASS)
        return;
    header[0] = 'D';
    header[1] = 'K';
    header[2] = 'I';
    header[3] = 'F';
    mem_put_le16(header+4,  0);                   /* version */
    mem_put_le16(header+6,  32);                  /* headersize */
    mem_put_le32(header+8,  fourcc);              /* headersize */
    mem_put_le16(header+12, cfg->g_w);            /* width */
    mem_put_le16(header+14, cfg->g_h);            /* height */
    mem_put_le32(header+16, cfg->g_timebase.den); /* rate */
    mem_put_le32(header+20, cfg->g_timebase.num); /* scale */
    mem_put_le32(header+24, frame_cnt);           /* length */
    mem_put_le32(header+28, 0);                   /* unused */

    fwrite(header, 1, 32, outfile);
}

static void write_ivf_frame_header(FILE *outfile,
                                   const vpx_codec_cx_pkt_t *pkt)
{
    char             header[12];
    vpx_codec_pts_t  pts;

    if(pkt->kind != VPX_CODEC_CX_FRAME_PKT)
        return;

    pts = pkt->data.frame.pts;
    mem_put_le32(header, pkt->data.frame.sz);
    mem_put_le32(header+4, pts&0xFFFFFFFF);
    mem_put_le32(header+8, pts >> 32);

    fwrite(header, 1, 12, outfile);
}
/* Plugin information */
#define JANUS_ECHOTEST_VERSION			6
#define JANUS_ECHOTEST_VERSION_STRING	"0.0.6"
#define JANUS_ECHOTEST_DESCRIPTION		"This is a trivial EchoTest plugin for Janus, just used to showcase the plugin interface."
#define JANUS_ECHOTEST_NAME				"JANUS EchoTest plugin"
#define JANUS_ECHOTEST_AUTHOR			"Meetecho s.r.l."
#define JANUS_ECHOTEST_PACKAGE			"janus.plugin.echotest"

/* Plugin methods */
janus_plugin *create(void);
int janus_echotest_init(janus_callbacks *callback, const char *config_path);
void janus_echotest_destroy(void);
int janus_echotest_get_api_compatibility(void);
int janus_echotest_get_version(void);
const char *janus_echotest_get_version_string(void);
const char *janus_echotest_get_description(void);
const char *janus_echotest_get_name(void);
const char *janus_echotest_get_author(void);
const char *janus_echotest_get_package(void);
void janus_echotest_create_session(janus_plugin_session *handle, int *error);
struct janus_plugin_result *janus_echotest_handle_message(janus_plugin_session *handle, char *transaction, json_t *message, json_t *jsep);
void janus_echotest_setup_media(janus_plugin_session *handle);
void janus_echotest_incoming_rtp(janus_plugin_session *handle, int video, char *buf, int len);
void janus_echotest_incoming_rtcp(janus_plugin_session *handle, int video, char *buf, int len);
void janus_echotest_incoming_data(janus_plugin_session *handle, char *buf, int len);
void janus_echotest_slow_link(janus_plugin_session *handle, int uplink, int video);
void janus_echotest_hangup_media(janus_plugin_session *handle);
void janus_echotest_destroy_session(janus_plugin_session *handle, int *error);
json_t *janus_echotest_query_session(janus_plugin_session *handle);

/*
FIX: H.264 in some container format (FLV, MP4, MKV etc.) need
"h264_mp4toannexb" bitstream filter (BSF)
*Add SPS,PPS in front of IDR frame
*Add start code ("0,0,0,1") in front of NALU
H.264 in some container (MPEG2TS) don't need this BSF.
*/
//'1': Use H.264 Bitstream Filter   
#define USE_H264BSF 0  

/*
FIX:AAC in some container format (FLV, MP4, MKV etc.) need
"aac_adtstoasc" bitstream filter (BSF)
*/
//'1': Use AAC Bitstream Filter   
#define USE_AACBSF 0  

typedef enum{
	VFX_NULL = 0,
	VFX_EDGE = 1,
	VFX_NEGATE = 2
}VFX;

typedef enum{
	AFX_NULL = 0,
}AFX;

#if defined(__ppc__) || defined(__ppc64__)
	# define swap2(d)  \
	((d&0x000000ff)<<8) |  \
	((d&0x0000ff00)>>8)
#else
	# define swap2(d) d
#endif
vpx_image_t raw;
AVBitStreamFilterContext *aacbsfc = NULL;
AVBitStreamFilterContext* h264bsfc = NULL;
//multiple input
static AVFormatContext **ifmt_ctx;
static vpx_codec_ctx_t de_codec;
AVFrame **frame = NULL;
//single output
static AVFormatContext *ofmt_ctx;
static AVCodecContext *codec_v;
static AVCodecContext *codec_ctx_1;
typedef struct FilteringContext {
	AVFilterContext *buffersink_ctx;
	AVFilterContext **buffersrc_ctx;
  	AVFilterGraph *filter_graph;
} FilteringContext;
static FilteringContext *filter_ctx;

typedef struct FilteringContext_a {
	AVFilterContext *buffersink_ctx_a;
	AVFilterContext **buffersrc_ctx_a;
     	AVFilterGraph *filter_graph_a;
} FilteringContext_a;
static FilteringContext_a *filter_ctx_a;


static AVFilterContext *buffersink_ctx;
static AVFilterContext *buffersrc_ctx;
AVFrame *frame_in;
AVFrame *frame_out;


int vpx_img_plane_width(const vpx_image_t *img, int plane) {
  if (plane > 0 && img->x_chroma_shift > 0)
    return (img->d_w + 1) >> img->x_chroma_shift;
  else
    return img->d_w;
}

int vpx_img_plane_height(const vpx_image_t *img, int plane) {
  if (plane > 0 && img->y_chroma_shift > 0)
    return (img->d_h + 1) >> img->y_chroma_shift;
  else
    return img->d_h;
}



typedef struct InputFile{
	const char* filenames;
	/*
	* position index
	* 0 - 1 - 2
	* 3 - 4 - 5
	* 6 - 7 - 8
	* ……
	*/
	uint32_t video_idx;
	//scale level, 0 means keep the same
	//uint32_t video_expand;
	uint32_t video_effect;
	uint32_t audio_effect;
} InputFile;
InputFile* inputfiles;

typedef struct GlobalContext{
	//always be a square,such as 2x2, 3x3
	uint32_t grid_num;
	uint32_t video_num;
	uint32_t enc_width;
	uint32_t enc_height;
	uint32_t enc_bit_rate;
	InputFile* input_file;
	const char* outfilename;
} GlobalContext;
GlobalContext* global_ctx;
typedef struct janus_pp_rtp_header
{
#if __BYTE_ORDER == __BIG_ENDIAN
	uint16_t version:2;
	uint16_t padding:1;
	uint16_t extension:1;
	uint16_t csrccount:4;
	uint16_t markerbit:1;
	uint16_t type:7;
#elif __BYTE_ORDER == __LITTLE_ENDIAN
	uint16_t csrccount:4;
	uint16_t extension:1;
	uint16_t padding:1;
	uint16_t version:2;
	uint16_t type:7;
	uint16_t markerbit:1;
#endif
	uint16_t seq_number;
	uint32_t timestamp;
	uint32_t ssrc;
	uint32_t csrc[16];
} janus_pp_rtp_header;

typedef struct janus_pp_rtp_header_extension {
	uint16_t type;
	uint16_t length;
} janus_pp_rtp_header_extension;

typedef struct janus_pp_frame_packet {
	uint16_t seq;	/* RTP Sequence number */
	uint64_t ts;	/* RTP Timestamp */
	uint16_t len;	/* Length of the data */
	int pt;			/* Payload type of the data */
	long offset;	/* Offset of the data in the file */
	int skip;		/* Bytes to skip, besides the RTP header */
	uint8_t drop;	/* Whether this packet can be dropped (e.g., padding)*/
	struct janus_pp_frame_packet *next;
	struct janus_pp_frame_packet *prev;
} janus_pp_frame_packet;
#define RTP_HEADER_MIN 12
typedef struct {
    int version;
    int type;
    int pad, ext, cc, mark;
    int seq, time;
    int ssrc;
    int *csrc;
    int header_size;
    int custom;
    int payload_size;
    char hh;
} rtp_header;

static int rbe16(const unsigned char *p) {
    int v = p[0] << 8 | p[1];
    return v;
}

/* helper, read a big-endian 32 bit int from memory */
static int rbe32(const unsigned char *p) {
    int v = p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
    return v;
}

static int open_output_file(const char *filename)
{
	AVStream *out_stream;
	AVStream *in_stream;
	AVCodecContext *dec_ctx, *enc_ctx;
        AVCodecContext *enc_ctx_a = av_malloc(sizeof(AVCodecContext));
	AVCodec *encoder;
        AVCodec *encoder_a;
	int ret;
	unsigned int i;
	ofmt_ctx = NULL;
	avformat_alloc_output_context2(&ofmt_ctx, NULL, "flv", filename);
	if (!ofmt_ctx) {
		JANUS_LOG(LOG_WARN, "Could not create output context\n");
		return AVERROR_UNKNOWN;
	}

	out_stream = avformat_new_stream(ofmt_ctx, NULL);
	if (!out_stream) {
            JANUS_LOG(LOG_WARN, "Failed allocating output stream\n");
            return AVERROR_UNKNOWN;
	}
	//in_stream = ifmt_ctx[0]->streams[i];
	//out_stream->time_base = in_stream->time_base;
	//dec_ctx = in_stream->codec;
	enc_ctx = out_stream->codec;
	
        /* in this example, we choose transcoding to same codec */
        encoder = avcodec_find_encoder(AV_CODEC_ID_H264);
        enc_ctx->height = global_ctx->enc_height;
        enc_ctx->width = global_ctx->enc_width;
        //enc_ctx->sample_aspect_ratio = dec_ctx->sample_aspect_ratio;
        /* take first format from list of supported formats */
        enc_ctx->pix_fmt = encoder->pix_fmts[0];
        enc_ctx->me_range = 16;
        enc_ctx->max_qdiff = 4;
        enc_ctx->bit_rate = global_ctx->enc_bit_rate;
        enc_ctx->qcompress = 0.6;
        /* video time_base can be set to whatever is handy and supported by encoder */
        enc_ctx->time_base.num = 1;
        enc_ctx->time_base.den = 25;
        enc_ctx->gop_size = 250;
        enc_ctx->max_b_frames = 3;
        AVDictionary * d = NULL;
        char *k = av_strdup("preset");       // if your strings are already allocated,
        char *v = av_strdup("ultrafast");    // you can avoid copying them like this
        av_dict_set(&d, k, v, AV_DICT_DONT_STRDUP_KEY | AV_DICT_DONT_STRDUP_VAL);
        ret = avcodec_open2(enc_ctx, encoder, &d);
        if (ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
            enc_ctx->flags |= CODEC_FLAG_GLOBAL_HEADER;
	encoder_a = avcodec_find_encoder(AV_CODEC_ID_AAC);
	av_dump_format(ofmt_ctx, 0, filename, 1);
	if (!(ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
		ret = avio_open(&ofmt_ctx->pb, filename, AVIO_FLAG_WRITE);
		if (ret < 0) {
			JANUS_LOG(LOG_WARN, "Could not open output file '%s'", filename);
			return ret;
		}
	}
	/* init muxer, write output file header */
	ret = avformat_write_header(ofmt_ctx, NULL);
	if (ret < 0) {
		JANUS_LOG(LOG_WARN, "Error occurred when opening output file\n");
		return ret;
	}

	return 0;
}

static int max_width = 0, max_height = 0, fps = 0;
/* WebM output */
static AVFormatContext *fctx;
static AVStream *vStream;
static int init_filter(FilteringContext* fctx, AVCodecContext **dec_ctx,
	AVCodecContext *enc_ctx, const char *filter_spec)
{
	char args[512];
	char pad_name[10];
        char pad_fifo_name[10];
	int ret = 0;
	int i;
	AVFilter **buffersrc = (AVFilter**)av_malloc(global_ctx->video_num*sizeof(AVFilter*));
	AVFilter *buffersink = NULL;
	AVFilterContext **buffersrc_ctx = (AVFilterContext**)av_malloc(global_ctx->video_num*sizeof(AVFilterContext*));
	AVFilterContext *buffersink_ctx = NULL;
	AVFilterInOut **outputs = (AVFilterInOut**)av_malloc(global_ctx->video_num*sizeof(AVFilterInOut*));
	AVFilterInOut *inputs = avfilter_inout_alloc();
	AVFilterGraph *filter_graph = avfilter_graph_alloc();
	for (i = 0; i < global_ctx->video_num; i++)
	{
		buffersrc[i] = NULL;
		buffersrc_ctx[i] = NULL;
		outputs[i] = avfilter_inout_alloc();
	}
	if (!outputs || !inputs || !filter_graph) {
		ret = AVERROR(ENOMEM);
		goto end;
	}
	for (i = 0; i < global_ctx->video_num; i++)
	{
            buffersrc[i] = avfilter_get_by_name("buffer");
	}
	buffersink = avfilter_get_by_name("buffersink");
	if (!buffersrc || !buffersink) {
            JANUS_LOG(LOG_WARN, "filtering source or sink element not found\n");
            ret = AVERROR_UNKNOWN;
            goto end;
        }
	for (i = 0; i < global_ctx->video_num; i++)
	{
            snprintf(args, sizeof(args),"video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
                dec_ctx[i]->width, dec_ctx[i]->height, dec_ctx[i]->pix_fmt,
		dec_ctx[i]->time_base.num, dec_ctx[i]->time_base.den,
		dec_ctx[i]->sample_aspect_ratio.num,
		dec_ctx[i]->sample_aspect_ratio.den);
            JANUS_LOG(LOG_WARN, "write frame %s\n", args);
            snprintf(pad_name, sizeof(pad_name), "in%d", i);
            ret = avfilter_graph_create_filter(&(buffersrc_ctx[i]), buffersrc[i], pad_name,
                    args, NULL, filter_graph);
            if (ret < 0) {
                JANUS_LOG(LOG_WARN, "Cannot create buffer source\n");
		goto end;
            }
        }
        ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out",
			NULL, NULL, filter_graph);
	if (ret < 0) {
            JANUS_LOG(LOG_WARN, "Cannot create buffer sink\n");
            goto end;
	}
	ret = av_opt_set_bin(buffersink_ctx, "pix_fmts",
		(uint8_t*)&enc_ctx->pix_fmt, sizeof(enc_ctx->pix_fmt),
		AV_OPT_SEARCH_CHILDREN);
	if (ret < 0) {
            JANUS_LOG(LOG_WARN, "Cannot set output pixel format\n");
            goto end;
	}
        /* Endpoints for the filter graph. */
	for (i = 0; i < global_ctx->video_num; i++)
        {
            snprintf(pad_name, sizeof(pad_name), "in%d", i);
            outputs[i]->name = av_strdup(pad_name);
            outputs[i]->filter_ctx = buffersrc_ctx[i];
            outputs[i]->pad_idx = 0;
            if (i == global_ctx->video_num - 1)
            	outputs[i]->next = NULL;
            else
                outputs[i]->next = outputs[i + 1];
	}
	inputs->name = av_strdup("out");
	inputs->filter_ctx = buffersink_ctx;
	inputs->pad_idx = 0;
	inputs->next = NULL;
	if (!outputs[0]->name || !inputs->name) {
		ret = AVERROR(ENOMEM);
		goto end;
	}
	if ((ret = avfilter_graph_parse_ptr(filter_graph, filter_spec,
		&inputs, outputs, NULL)) < 0)
		goto end;
        if ((ret = avfilter_graph_config(filter_graph, NULL)) < 0)
		goto end;
	/* Fill FilteringContext */
	fctx->buffersrc_ctx = buffersrc_ctx;
	fctx->buffersink_ctx = buffersink_ctx;
	fctx->filter_graph = filter_graph;
end:
	avfilter_inout_free(&inputs);
	av_free(buffersrc);
	//	av_free(buffersrc_ctx);
	avfilter_inout_free(outputs);
	av_free(outputs);

	return ret;
}


static guint64 time_1 = 0;
static int init_spec_filter(void)
{
	char filter_spec[1000];
	char spec_temp[1000];
	unsigned int i;
	unsigned int j;
	unsigned int k;
	unsigned int x_coor;
	unsigned int y_coor;
	AVCodecContext** dec_ctx_array;
	int	stream_num = 1;
	int stride = (int)sqrt((long double)global_ctx->grid_num);
	int ret;
	filter_ctx = (FilteringContext *)av_malloc_array(1, sizeof(*filter_ctx));
	dec_ctx_array = (AVCodecContext**)av_malloc(global_ctx->video_num*sizeof(AVCodecContext));
        codec_v = av_malloc(sizeof(AVCodecContext));
        codec_v->codec_id = AV_CODEC_ID_VP8;
        codec_v->codec_type = AVMEDIA_TYPE_VIDEO;
        codec_v->height = 480;
        codec_v->width = 640;
        codec_v->pix_fmt = AV_PIX_FMT_YUV420P;
        codec_v->time_base.num = 1;
        codec_v->time_base.den = 32;
        codec_v->sample_aspect_ratio.num = 3;
        codec_v->sample_aspect_ratio.den = 4;
        if (!filter_ctx || !dec_ctx_array || !filter_ctx_a)
		return AVERROR(ENOMEM);
        
        for (j = 0; j < global_ctx->video_num; j++) {
                        dec_ctx_array[j] = codec_v;        
	}
        filter_ctx[0].buffersrc_ctx = NULL;
        filter_ctx[0].buffersink_ctx = NULL;
        filter_ctx[0].filter_graph = NULL;
    	if (global_ctx->grid_num == 1)
            snprintf(filter_spec, sizeof(filter_spec), "null");
	else {
            //snprintf(filter_spec, sizeof(filter_spec), "color=c=black@1:s=%dx%d[x0];", global_ctx->enc_width, global_ctx->enc_height);
            snprintf(filter_spec, sizeof(filter_spec), "nullsrc=size=%dx%d[x0];", global_ctx->enc_width, global_ctx->enc_height);
            k = 1;
            for (j = 0; j < global_ctx->video_num; j++)
            {
                x_coor = j % stride;
                y_coor = j / stride;
		snprintf(spec_temp, sizeof(spec_temp), "[in%d]setpts=PTS-STARTPTS,scale=w=%d:h=%d[innf%d];[innf%d]fifo[inn%d];", j, global_ctx->enc_width / stride, 
								global_ctx->enc_height / stride, j,j,j);
		strcat(filter_spec, spec_temp);
	    }
            for (j = 0; j < global_ctx->video_num; j++)
            {
		x_coor = j % stride;
                y_coor = j / stride;
		if(j == 3) {
                    snprintf(spec_temp, sizeof(spec_temp),"[x%d][inn%d]overlay=%d*%d/%d:%d*%d/%d[xx%d];[xx%d]fifo[x%d];",k - 1, j,
                                global_ctx->enc_width, x_coor, stride, global_ctx->enc_height, y_coor, stride, k,k,k);
		} else {	
                    snprintf(spec_temp, sizeof(spec_temp),"[x%d][inn%d]overlay=%d*%d/%d:%d*%d/%d[xx%d];[xx%d]fifo[x%d];",k - 1, j, 
                    		global_ctx->enc_width, x_coor, stride, global_ctx->enc_height, y_coor, stride, k,k,k);
		}
		k++;                                      //[ine%d]//[x%d][inn%d]overlay=%d*%d/%d:%d*%d/%d[xx%d];[xx%d]fifo[x%d];
		strcat(filter_spec, spec_temp);
		JANUS_LOG(LOG_WARN, "write frame %s\n", filter_spec);
            }
            snprintf(spec_temp, sizeof(spec_temp), "[x%d]fifo[out]", k - 1);
            		//, global_ctx->enc_width, global_ctx->enc_height);
            strcat(filter_spec, spec_temp);
        }
	JANUS_LOG(LOG_WARN, "write frame %s\n", filter_spec);
        //printf("%s",spec_temp);
        ret = init_filter(&filter_ctx[0], dec_ctx_array,ofmt_ctx->streams[0]->codec, filter_spec);
        av_free(dec_ctx_array);
	return 0;
}
static int  pts = 0;
static int  pts1 = 0;
static int frame_num = 0;
static int frame_num_audio = 0;
static int frame_num_v_enc = 0;
static int frame_num_a_enc = 0;

/* Plugin setup */
static janus_plugin janus_echotest_plugin =
	JANUS_PLUGIN_INIT (
		.init = janus_echotest_init,
		.destroy = janus_echotest_destroy,

		.get_api_compatibility = janus_echotest_get_api_compatibility,
		.get_version = janus_echotest_get_version,
		.get_version_string = janus_echotest_get_version_string,
		.get_description = janus_echotest_get_description,
		.get_name = janus_echotest_get_name,
		.get_author = janus_echotest_get_author,
		.get_package = janus_echotest_get_package,
		
		.create_session = janus_echotest_create_session,
		.handle_message = janus_echotest_handle_message,
		.setup_media = janus_echotest_setup_media,
		.incoming_rtp = janus_echotest_incoming_rtp,
		.incoming_rtcp = janus_echotest_incoming_rtcp,
		.incoming_data = janus_echotest_incoming_data,
		.slow_link = janus_echotest_slow_link,
		.hangup_media = janus_echotest_hangup_media,
		.destroy_session = janus_echotest_destroy_session,
		.query_session = janus_echotest_query_session,
	);

/* Plugin creator */
janus_plugin *create(void) {
	JANUS_LOG(LOG_VERB, "%s created!\n", JANUS_ECHOTEST_NAME);
	return &janus_echotest_plugin;
}


/* Useful stuff */
static volatile gint initialized = 0, stopping = 0;
static gboolean notify_events = TRUE;
static janus_callbacks *gateway = NULL;
static GThread *handler_thread;
static GThread *watchdog;
static void *janus_echotest_handler(void *data);

typedef struct janus_echotest_message {
	janus_plugin_session *handle;
	char *transaction;
	json_t *message;
	json_t *jsep;
} janus_echotest_message;
static GAsyncQueue *messages = NULL;
static janus_echotest_message exit_message;

typedef struct janus_echotest_session {
	janus_plugin_session *handle;
	gboolean has_audio;
	gboolean has_video;
	gboolean has_data;
	gboolean audio_active;
	gboolean video_active;
	uint64_t bitrate;
	janus_recorder *arc;	/* The Janus recorder instance for this user's audio, if enabled */
	janus_recorder *vrc;	/* The Janus recorder instance for this user's video, if enabled */
	janus_recorder *drc;	/* The Janus recorder instance for this user's data, if enabled */
	janus_mutex rec_mutex;	/* Mutex to protect the recorders from race conditions */
	guint16 slowlink_count;
	volatile gint hangingup;
	gint64 destroyed;	/* Time at which this session was marked as destroyed */
        guint64 keyframe_ts;
        guint64 current_ts;
        guint64 previous_ts;
        guint64 current_audio_ts;
        guint64 previous_audio_ts;
        guint64  first_audio_ts;
        int previous_keyframe;
        int keyframe_number;
        int numBytes;
        uint8_t *received_frame;
        int frameLen;
        uint8_t *received_audio_frame;
        int audio_frameLen;
        int keyFrame;
        FILE *outfile;
        vpx_codec_enc_cfg_t cfg;
        vpx_codec_ctx_t codec;
        vpx_image_t raw;
        int frame_avail;
        int flags;
        int frame_cnt;
        FILE *fp_out;
} janus_echotest_session;
static GHashTable *sessions;
static GList *old_sessions;
static janus_mutex sessions_mutex;

static void janus_echotest_message_free(janus_echotest_message *msg) {
	if(!msg || msg == &exit_message)
		return;

	msg->handle = NULL;

	g_free(msg->transaction);
	msg->transaction = NULL;
	if(msg->message)
		json_decref(msg->message);
	msg->message = NULL;
	if(msg->jsep)
		json_decref(msg->jsep);
	msg->jsep = NULL;

	g_free(msg);
}


/* Error codes */
#define JANUS_ECHOTEST_ERROR_NO_MESSAGE			411
#define JANUS_ECHOTEST_ERROR_INVALID_JSON		412
#define JANUS_ECHOTEST_ERROR_INVALID_ELEMENT	413


/* EchoTest watchdog/garbage collector (sort of) */
void *janus_echotest_watchdog(void *data);
void *janus_echotest_watchdog(void *data) {
	JANUS_LOG(LOG_INFO, "EchoTest watchdog started\n");
	gint64 now = 0;
	while(g_atomic_int_get(&initialized) && !g_atomic_int_get(&stopping)) {
		janus_mutex_lock(&sessions_mutex);
		/* Iterate on all the sessions */
		now = janus_get_monotonic_time();
		if(old_sessions != NULL) {
			GList *sl = old_sessions;
			JANUS_LOG(LOG_HUGE, "Checking %d old EchoTest sessions...\n", g_list_length(old_sessions));
			while(sl) {
				janus_echotest_session *session = (janus_echotest_session *)sl->data;
				if(!session) {
					sl = sl->next;
					continue;
				}
				if(now-session->destroyed >= 5*G_USEC_PER_SEC) {
					/* We're lazy and actually get rid of the stuff only after a few seconds */
					JANUS_LOG(LOG_VERB, "Freeing old EchoTest session\n");
					GList *rm = sl->next;
					old_sessions = g_list_delete_link(old_sessions, sl);
					sl = rm;
					session->handle = NULL;
					g_free(session);
					session = NULL;
					continue;
				}
				sl = sl->next;
			}
		}
		janus_mutex_unlock(&sessions_mutex);
		g_usleep(500000);
	}
	JANUS_LOG(LOG_INFO, "EchoTest watchdog stopped\n");
	return NULL;
}


/* Plugin implementation */
int janus_echotest_init(janus_callbacks *callback, const char *config_path) {
	if(g_atomic_int_get(&stopping)) {
		/* Still stopping from before */
		return -1;
	}
	if(callback == NULL || config_path == NULL) {
		/* Invalid arguments */
		return -1;
	}

	/* Read configuration */
	char filename[255];
	g_snprintf(filename, 255, "%s/%s.cfg", config_path, JANUS_ECHOTEST_PACKAGE);
	JANUS_LOG(LOG_VERB, "Configuration file: %s\n", filename);
	janus_config *config = janus_config_parse(filename);
	if(config != NULL) {
		janus_config_print(config);
		janus_config_item *events = janus_config_get_item_drilldown(config, "general", "events");
		if(events != NULL && events->value != NULL)
			notify_events = janus_is_true(events->value);
		if(!notify_events && callback->events_is_enabled()) {
			JANUS_LOG(LOG_WARN, "Notification of events to handlers disabled for %s\n", JANUS_ECHOTEST_NAME);
		}
	}
	janus_config_destroy(config);
	config = NULL;
	
	sessions = g_hash_table_new(NULL, NULL);
	janus_mutex_init(&sessions_mutex);
	messages = g_async_queue_new_full((GDestroyNotify) janus_echotest_message_free);
	/* This is the callback we'll need to invoke to contact the gateway */
	gateway = callback;
	g_atomic_int_set(&initialized, 1);

	GError *error = NULL;
	/* Start the sessions watchdog */
	watchdog = g_thread_try_new("echotest watchdog", &janus_echotest_watchdog, NULL, &error);
	if(error != NULL) {
		g_atomic_int_set(&initialized, 0);
		JANUS_LOG(LOG_ERR, "Got error %d (%s) trying to launch the EchoTest watchdog thread...\n", error->code, error->message ? error->message : "??");
		return -1;
	}
	/* Launch the thread that will handle incoming messages */
	handler_thread = g_thread_try_new("echotest handler", janus_echotest_handler, NULL, &error);
	if(error != NULL) {
		g_atomic_int_set(&initialized, 0);
		JANUS_LOG(LOG_ERR, "Got error %d (%s) trying to launch the EchoTest handler thread...\n", error->code, error->message ? error->message : "??");
		return -1;
	}
	JANUS_LOG(LOG_INFO, "%s initialized!\n", JANUS_ECHOTEST_NAME);
	return 0;
}

void janus_echotest_destroy(void) {
	if(!g_atomic_int_get(&initialized))
		return;
	g_atomic_int_set(&stopping, 1);

	g_async_queue_push(messages, &exit_message);
	if(handler_thread != NULL) {
		g_thread_join(handler_thread);
		handler_thread = NULL;
	}
	if(watchdog != NULL) {
		g_thread_join(watchdog);
		watchdog = NULL;
	}

	/* FIXME We should destroy the sessions cleanly */
	janus_mutex_lock(&sessions_mutex);
	g_hash_table_destroy(sessions);
	janus_mutex_unlock(&sessions_mutex);
	g_async_queue_unref(messages);
	messages = NULL;
	sessions = NULL;

	g_atomic_int_set(&initialized, 0);
	g_atomic_int_set(&stopping, 0);
	JANUS_LOG(LOG_INFO, "%s destroyed!\n", JANUS_ECHOTEST_NAME);
}

int janus_echotest_get_api_compatibility(void) {
	/* Important! This is what your plugin MUST always return: don't lie here or bad things will happen */
	return JANUS_PLUGIN_API_VERSION;
}

int janus_echotest_get_version(void) {
	return JANUS_ECHOTEST_VERSION;
}

const char *janus_echotest_get_version_string(void) {
	return JANUS_ECHOTEST_VERSION_STRING;
}

const char *janus_echotest_get_description(void) {
	return JANUS_ECHOTEST_DESCRIPTION;
}

const char *janus_echotest_get_name(void) {
	return JANUS_ECHOTEST_NAME;
}

const char *janus_echotest_get_author(void) {
	return JANUS_ECHOTEST_AUTHOR;
}

const char *janus_echotest_get_package(void) {
	return JANUS_ECHOTEST_PACKAGE;
}

void janus_echotest_create_session(janus_plugin_session *handle, int *error) {
	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized)) {
		*error = -1;
		return;
	}	
        //added video combine
        global_ctx = (GlobalContext*)av_malloc(sizeof(GlobalContext));
	global_ctx->video_num = 4;
	global_ctx->grid_num = 4;
	global_ctx->enc_bit_rate = 500000;
	global_ctx->enc_height = 360;
	global_ctx->enc_width = 640;
	global_ctx->outfilename = "combined.flv";
	int ret;
	int tmp = 0;
	int got_frame_num = 0;
	unsigned int stream_index;
	AVPacket packet;
	AVPacket enc_pkt;
	AVFrame* picref;
	enum AVMediaType mediatype;
	int read_frame_done = 0;
	int flush_now = 0;
	int framecnt = 0;
	int i, j;
	int got_frame;
	int enc_got_frame = 0;
	int(*dec_func)(AVCodecContext *, AVFrame *, int *, const AVPacket *);
	int(*enc_func)(AVCodecContext *, AVPacket *, const AVFrame *, int *);
	frame = (AVFrame**)av_malloc(global_ctx->video_num*sizeof(AVFrame*));
	picref = av_frame_alloc();
	av_register_all();
	avfilter_register_all();
        codec_ctx_1 = av_malloc(sizeof(AVCodecContext));
        AVCodecContext *codec_ctx_a = av_malloc(sizeof(AVCodecContext));
	codec_ctx_1->codec_id =  AV_CODEC_ID_VP8;
        codec_ctx_1->codec_type = AVMEDIA_TYPE_VIDEO;
        codec_ctx_1->width = 640;
        codec_ctx_1->height = 480;
	codec_ctx_a->codec_id =  AV_CODEC_ID_OPUS;
        codec_ctx_a->codec_type = AVMEDIA_TYPE_AUDIO;        
        /* Open decoder */
        ret = avcodec_open2(codec_ctx_1,
                    avcodec_find_decoder(codec_ctx_1->codec_id), NULL);
        codec_ctx_1->width = 640;
        codec_ctx_1->height = 480;
	codec_ctx_1->time_base = (AVRational){1, 30};
        codec_ctx_1->pix_fmt = AV_PIX_FMT_YUV420P;
        if (ret < 0) {
            JANUS_LOG(LOG_ERR, "Failed to open decoder for stream \n");
            //return ret;
        }
        ret = avcodec_open2(codec_ctx_a,
                    avcodec_find_decoder(codec_ctx_a->codec_id), NULL);
        if (ret < 0) {
            JANUS_LOG(LOG_ERR, "Failed to open decoder for stream \n");
            //return ret;
        }	
        codec_ctx_1->width = 640;
        codec_ctx_1->height = 480;
	codec_ctx_1->time_base = (AVRational){1, 30};
        codec_ctx_1->pix_fmt = AV_PIX_FMT_YUV420P;
        open_output_file(global_ctx->outfilename);
	init_spec_filter();

        char filename[255];
        char filename1[255];
	gint64 now = janus_get_real_time();
         //end of video combine
	janus_echotest_session *session = (janus_echotest_session *)g_malloc0(sizeof(janus_echotest_session));
	session->handle = handle;
	session->has_audio = FALSE;
	session->has_video = FALSE;
	session->has_data = FALSE;
	session->audio_active = TRUE;
	session->video_active = TRUE;
        janus_mutex_init(&session->rec_mutex);
	session->bitrate = 0;	/* No limit */
	session->destroyed = 0;
	g_atomic_int_set(&session->hangingup, 0);
	handle->plugin_handle = session;
	janus_mutex_lock(&sessions_mutex);
	g_hash_table_insert(sessions, handle, session);
        janus_mutex_unlock(&sessions_mutex);
        memset(filename, 0, 255);
        /* Build a filename */
	g_snprintf(filename, 255, "echotest-%p-%"SCNi64"-video", session, now);
	session->vrc = janus_recorder_create(NULL, "vp8", filename);
	if(session->vrc == NULL) {
            /* FIXME We should notify the fact the recorder could not be created */
            JANUS_LOG(LOG_ERR, "Couldn't open an video recording file for this EchoTest user!\n");
	}
        /* Send a PLI */
	JANUS_LOG(LOG_VERB, "Recording video, sending a PLI to kickstart it\n");
	char buf[12];
	memset(buf, 0, 12);
	janus_rtcp_pli((char *)&buf, 12);
	gateway->relay_rtcp(session->handle, 1, buf, 12);
        	/* Setup FFmpeg */
	av_register_all();
	/* Adjust logging to match the postprocessor's */
	av_log_set_level(janus_log_level <= LOG_NONE ? AV_LOG_QUIET :
		(janus_log_level == LOG_FATAL ? AV_LOG_FATAL :
			(janus_log_level == LOG_ERR ? AV_LOG_ERROR :
				(janus_log_level == LOG_WARN ? AV_LOG_WARNING :
					(janus_log_level == LOG_INFO ? AV_LOG_INFO :
						(janus_log_level == LOG_VERB ? AV_LOG_VERBOSE : AV_LOG_DEBUG))))));
	/* WebM output */
	fctx = avformat_alloc_context();
	if(fctx == NULL) {
		JANUS_LOG(LOG_ERR, "Error allocating context\n");
		return -1;
	}
	//~ fctx->oformat = guess_format("webm", NULL, NULL);
	fctx->oformat = av_guess_format("webm", NULL, NULL);
	if(fctx->oformat == NULL) {
		JANUS_LOG(LOG_ERR, "Error guessing format\n");
		return -1;
	}
        memset(filename1, 0, 255);
        /* Build a filename */
	g_snprintf(filename1, 255, "echotest-%p-%"SCNi64"-video.webm", session, now);
	snprintf(fctx->filename, sizeof(fctx->filename), "%s", filename1);
	//~ vStream = av_new_stream(fctx, 0);
	vStream = avformat_new_stream(fctx, 0);
	if(vStream == NULL) {
		JANUS_LOG(LOG_ERR, "Error adding stream\n");
		return -1;
	}

	avcodec_get_context_defaults3(vStream->codec, AVMEDIA_TYPE_VIDEO);


        session->keyframe_ts = 0;
        session->frameLen = 0;
        session->keyFrame = 0;
        session->previous_audio_ts = 0;
        session->first_audio_ts = 0;
        session->previous_ts = 0;
        session->keyframe_number = 0;
        

	vStream->codec->codec_id = AV_CODEC_ID_VP8;


	//~ vStream->codec->codec_type = CODEC_TYPE_VIDEO;
	vStream->codec->codec_type = AVMEDIA_TYPE_VIDEO;
	vStream->codec->time_base = (AVRational){1, 30};
	vStream->codec->width = 640;
	vStream->codec->height = 480;
	vStream->codec->pix_fmt = AV_PIX_FMT_YUV420P;
	if (fctx->flags & AVFMT_GLOBALHEADER)
		vStream->codec->flags |= CODEC_FLAG_GLOBAL_HEADER;
	//~ fctx->timestamp = 0;
	//~ if(url_fopen(&fctx->pb, fctx->filename, URL_WRONLY) < 0) {
	if(avio_open(&fctx->pb, fctx->filename, AVIO_FLAG_WRITE) < 0) {
		JANUS_LOG(LOG_ERR, "Error opening file for output\n");
		return -1;
	}
	//~ memset(&parameters, 0, sizeof(AVFormatParameters));
	//~ av_set_parameters(fctx, &parameters);
	//~ fctx->preload = (int)(0.5 * AV_TIME_BASE);
	//~ fctx->max_delay = (int)(0.7 * AV_TIME_BASE);
	//~ if(av_write_header(fctx) < 0) {
	if(avformat_write_header(fctx, NULL) < 0) {
		JANUS_LOG(LOG_ERR, "Error writing header\n");
		return -1;
	}
        
         if (vpx_codec_dec_init(&de_codec, dec_interface, NULL, 0)) {
                printf("Failed to initialize decoder\n");
                return -1;
        }

              
	//const char *filter_descr = "lutyuv='u=128:v=128'";
	//const char *filter_descr = "boxblur";
	//const char *filter_descr = "vflip";
	//const char *filter_descr = "hue='h=60:s=-3'";
	//const char *filter_descr = "crop=2/3*in_w:2/3*in_h";
	//const char *filter_descr = "drawbox=x=100:y=100:w=100:h=100:color=pink@0.5";
	const char *filter_descr = "drawtext=fontfile=arial.ttf:fontcolor=green:fontsize=30:x=100:y=50:text='Powered by Dialoga'";
       // const char *filter_descr ="movie=/home/admin/logo.jpg[wm];[in][wm]overlay=5:5[out]";
	char args[512];
        AVFilterGraph *filter_graph;
	AVFilter *buffersrc  = avfilter_get_by_name("buffer");
	AVFilter *buffersink = avfilter_get_by_name("buffersink");
	AVFilterInOut *outputs = avfilter_inout_alloc();
	AVFilterInOut *inputs  = avfilter_inout_alloc();
	AVBufferSinkParams *buffersink_params;
        filter_graph = avfilter_graph_alloc();
        int in_width=640;
	int in_height=480;
        /* buffer video source: the decoded frames from the decoder will be inserted here. */
	snprintf(args, sizeof(args),
		"video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
		in_width,in_height,AV_PIX_FMT_YUV420P,
		1, 25,1,1);
        ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in",
		args, NULL, filter_graph);
	if (ret < 0) {
		printf("Cannot create buffer source\n");
		return ret;
	}
        buffersink_params = av_buffersink_params_alloc();
	buffersink_params->pixel_fmts = AV_PIX_FMT_YUV420P;
	ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out",
		NULL, buffersink_params, filter_graph);
	av_free(buffersink_params);
	if (ret < 0) {
		printf("Cannot create buffer sink\n");
		return ret;
	}      
        /* Endpoints for the filter graph. */
	outputs->name       = av_strdup("in");
	outputs->filter_ctx = buffersrc_ctx;
	outputs->pad_idx    = 0;
	outputs->next       = NULL;

	inputs->name       = av_strdup("out");
	inputs->filter_ctx = buffersink_ctx;
	inputs->pad_idx    = 0;
	inputs->next       = NULL;
        
       if ((ret = avfilter_graph_parse_ptr(filter_graph, filter_descr,
		&inputs, &outputs, NULL)) < 0)
		return;

	if ((ret = avfilter_graph_config(filter_graph, NULL)) < 0)
		return;
        unsigned char *frame_buffer_in;
	unsigned char *frame_buffer_out;
        frame_in=av_frame_alloc();
	frame_buffer_in=(unsigned char *)av_malloc(av_image_get_buffer_size(AV_PIX_FMT_YUV420P, in_width,in_height,1));
	av_image_fill_arrays(frame_in->data, frame_in->linesize,frame_buffer_in,
		AV_PIX_FMT_YUV420P,in_width, in_height,1);
        
        frame_out=av_frame_alloc();
	frame_buffer_out=(unsigned char *)av_malloc(av_image_get_buffer_size(AV_PIX_FMT_YUV420P, in_width,in_height,1));
	av_image_fill_arrays(frame_out->data, frame_out->linesize,frame_buffer_out,
		AV_PIX_FMT_YUV420P,in_width, in_height,1);

	frame_in->width=in_width;
	frame_in->height=in_height;
	frame_in->format=AV_PIX_FMT_YUV420P;
        session->outfile = fopen("cuc_ieschool.ivf", "wb");
        if(session->outfile==NULL){
            printf("Error open files.\n");
            return ;
        }
        if(!vpx_img_alloc(&session->raw, VPX_IMG_FMT_I420, in_width, in_height, 1)){
            printf("Fail to allocate image\n");
            return;
        }
        vpx_codec_err_t ret_vp;
        ret_vp = vpx_codec_enc_config_default(interface, &session->cfg, 0);
        if(ret_vp) {
            printf("Failed to get config: %s\n", vpx_codec_err_to_string(ret_vp));
            return;
        }
        session->cfg.rc_target_bitrate =200000;
        session->cfg.g_w = in_width;
        session->cfg.g_h = in_height;
        write_ivf_file_header(session->outfile, &session->cfg, 0);
        if(vpx_codec_enc_init(&session->codec, interface, &session->cfg, 0)){
            printf("Failed to initialize encoder\n");
            return;
        }
        session->frame_avail = 1;
        session->flags = 0;
        session->frame_cnt = 0;
        session->fp_out = fopen("output.yuv","wb+");
	if(session->fp_out==NULL){
		printf("Error open output file.\n");
		return;
	}
	return;
}

void janus_echotest_destroy_session(janus_plugin_session *handle, int *error) {
	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized)) {
		*error = -1;
		return;
	}	
	janus_echotest_session *session = (janus_echotest_session *)handle->plugin_handle;
	if(!session) {
		JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
		*error = -2;
		return;
	}
        
        	if(fctx != NULL)
		av_write_trailer(fctx);
	if(vStream != NULL && vStream->codec != NULL)
		avcodec_close(vStream->codec);
	if(fctx != NULL && fctx->streams[0] != NULL) {
		av_free(fctx->streams[0]->codec);
		av_free(fctx->streams[0]);
	}
	if(fctx != NULL) {
		//~ url_fclose(fctx->pb);
		avio_close(fctx->pb);
		av_free(fctx);
	}
        fclose(session->outfile);
        vpx_codec_destroy(&session->codec);
        if(!fseek(session->outfile, 0, SEEK_SET))
            write_ivf_file_header(session->outfile, &session->cfg, session->frame_cnt-1);
	JANUS_LOG(LOG_VERB, "Removing Echo Test session...\n");
	janus_mutex_lock(&sessions_mutex);
	if(!session->destroyed) {
		session->destroyed = janus_get_monotonic_time();
		g_hash_table_remove(sessions, handle);
		/* Cleaning up and removing the session is done in a lazy way */
		old_sessions = g_list_append(old_sessions, session);
	}
	janus_mutex_unlock(&sessions_mutex);
	return;
}

json_t *janus_echotest_query_session(janus_plugin_session *handle) {
	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized)) {
		return NULL;
	}	
	janus_echotest_session *session = (janus_echotest_session *)handle->plugin_handle;
	if(!session) {
		JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
		return NULL;
	}
	/* In the echo test, every session is the same: we just provide some configure info */
	json_t *info = json_object();
	json_object_set_new(info, "audio_active", session->audio_active ? json_true() : json_false());
	json_object_set_new(info, "video_active", session->video_active ? json_true() : json_false());
	json_object_set_new(info, "bitrate", json_integer(session->bitrate));
	if(session->arc || session->vrc || session->drc) {
		json_t *recording = json_object();
		if(session->arc && session->arc->filename)
			json_object_set_new(recording, "audio", json_string(session->arc->filename));
		if(session->vrc && session->vrc->filename)
			json_object_set_new(recording, "video", json_string(session->vrc->filename));
		if(session->drc && session->drc->filename)
			json_object_set_new(recording, "data", json_string(session->drc->filename));
		json_object_set_new(info, "recording", recording);
	}
	json_object_set_new(info, "slowlink_count", json_integer(session->slowlink_count));
	json_object_set_new(info, "destroyed", json_integer(session->destroyed));
	return info;
}

struct janus_plugin_result *janus_echotest_handle_message(janus_plugin_session *handle, char *transaction, json_t *message, json_t *jsep) {
	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		return janus_plugin_result_new(JANUS_PLUGIN_ERROR, g_atomic_int_get(&stopping) ? "Shutting down" : "Plugin not initialized", NULL);

	janus_echotest_message *msg = g_malloc0(sizeof(janus_echotest_message));
	msg->handle = handle;
	msg->transaction = transaction;
	msg->message = message;
	msg->jsep = jsep;
	g_async_queue_push(messages, msg);

	/* All the requests to this plugin are handled asynchronously: we add a comment
	 * (a JSON object with a "hint" string in it, that's what the core expects),
	 * but we don't have to: other plugins don't put anything in there */
	return janus_plugin_result_new(JANUS_PLUGIN_OK_WAIT, "I'm taking my time!", NULL);
}

void janus_echotest_setup_media(janus_plugin_session *handle) {
	JANUS_LOG(LOG_INFO, "WebRTC media is now available\n");
	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		return;
	janus_echotest_session *session = (janus_echotest_session *)handle->plugin_handle;	
	if(!session) {
		JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
		return;
	}
	if(session->destroyed)
		return;
	g_atomic_int_set(&session->hangingup, 0);
	/* We really don't care, as we only send RTP/RTCP we get in the first place back anyway */
}

int parse_rtp_header(const unsigned char *packet, int size, rtp_header *rtp) {
    if (!packet || !rtp) {
        return -2;
    }
    if (size < RTP_HEADER_MIN) {
        fprintf(stderr, "Packet too short for rtp\n");
        return -1;
    }
    rtp->version = (packet[0] >> 6) & 3;
    rtp->pad = (packet[0] >> 5) & 1;
    rtp->ext = (packet[0] >> 4) & 1;
    rtp->cc = packet[0] & 7;
    rtp->header_size = 12 + 4 * rtp->cc;
    rtp->payload_size = size - rtp->header_size;
    rtp->mark = (packet[1] >> 7) & 1;
    rtp->type = (packet[1]) & 127;
    rtp->seq  = rbe16(packet + 2);
    rtp->time = rbe32(packet + 4);
    rtp->ssrc = rbe32(packet + 8);
    rtp->csrc = NULL;
    if (size < rtp->header_size) {
        fprintf(stderr, "Packet too short for RTP header\n");
        return -1;
    }
    return 0;
}
static int count = 0;
void janus_echotest_incoming_rtp(janus_plugin_session *handle, int video, char *buf, int len) {
	if(handle == NULL || handle->stopped || g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		return;
        char * buffy = malloc (2000);
        memcpy(buffy, buf, len);
        int len1 = len;
        int(*dec_func)(AVCodecContext *, AVFrame *, int *, const AVPacket *);
	int(*enc_func)(AVCodecContext *, AVPacket *, const AVFrame *, int *);
        int got_frame;
        AVFrame* picref;
        picref = av_frame_alloc();
        int ret;
       // ret = avcodec_open2(codec_v,
       // avcodec_find_decoder(codec_v->codec_id), NULL);
	//ret = avcodec_open2(codec_v,
       // avcodec_find_decoder_by_name("libvpx"), NULL);
        int bytes = 0, skip = 0;
	/* Simple echo test */
	if(gateway) {
		/* Honour the audio/video active flags */
		janus_echotest_session *session = (janus_echotest_session *)handle->plugin_handle;	
		if(!session) {
			JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
			return;
		}
		if(session->destroyed)
			return;
		if((!video && session->audio_active)) {
			/* Save the frame if we're recording */
			janus_recorder_save_frame(video ? session->vrc : session->arc, buf, len);
			/* Send the frame back */
			gateway->relay_rtp(handle, video, buf, len);
		} else if (video && session->video_active) {
                    
                        session->keyFrame = 0;
                        AVPacket packet;
			janus_recorder_save_frame(session->vrc, buf, len);
		    	char *prebuffer =  malloc(20);
			memcpy(prebuffer, buf, 16);
			janus_pp_rtp_header *rtp = (janus_pp_rtp_header *)prebuffer;
			 rtp_header *rtp_s = (rtp_header *)malloc(sizeof(rtp_header));
			parse_rtp_header(buf, len, rtp_s);
		    	if(rtp->csrccount) {
                                skip += rtp->csrccount*4;
                        }
                        if(rtp->extension) {
                                janus_pp_rtp_header_extension *ext = (janus_pp_rtp_header_extension *)(prebuffer+12);
                                skip += 4 + ntohs(ext->length)*4;
                        }
			if(!rtp->padding) {
                                buf = buf + 12 +skip;
                                len1 = len1 - 12 - skip;
                                int skipped = 1;
                                len1--;
                                uint8_t vp8pd = *buf;
                                uint8_t xbit = (vp8pd & 0x80);
                                uint8_t sbit = (vp8pd & 0x10);
                                if (xbit) {
                                    buf++;
                                    skipped++;
                                    len1--;
                                    vp8pd = *buf;
                                    uint8_t ibit = (vp8pd & 0x80);
                                    uint8_t lbit = (vp8pd & 0x40);
                                    uint8_t tbit = (vp8pd & 0x20);
                                    uint8_t kbit = (vp8pd & 0x10);
                                    if(ibit) {
                                        /* Read the PictureID octet */
                                        buf++;
                                        len1--;
                                        skipped++;
                                        vp8pd = *buf;
                                        uint16_t picid = vp8pd, wholepicid = picid;
                                        uint8_t mbit = (vp8pd & 0x80);
                                        if(mbit) {
                                            memcpy(&picid, buf, sizeof(uint16_t));
                                            wholepicid = ntohs(picid);
                                            picid = (wholepicid & 0x7FFF);
                                            buf++;
                                            len1--;
                                            skipped++;
                                        }
                                    }
                                    if(lbit) {
                                        /* Read the TL0PICIDX octet */
                                        buf++;
                                        len1--;
                                        skipped++;
                                        vp8pd = *buf;
                                    }
                                    if(tbit || kbit) {
                                        /* Read the TID/KEYIDX octet */
                                        buf++;
                                        len1--;
                                        skipped++;
                                        vp8pd = *buf;
                                    }
                                }
                                buf++;	/* Now we're in the payload */
                                if(sbit) {
                                    unsigned long int vp8ph = 0;
                                    memcpy(&vp8ph, buf, 4);
                                    vp8ph = ntohl(vp8ph);
                                    uint8_t pbit = ((vp8ph & 0x01000000) >> 24);
                                    if(!pbit) {
                                        //keyFrame = 1;
                                        /* Get resolution */
                                        unsigned char *c = buf+3;
                                        /* vet via sync code */
                                        if(c[0]!=0x9d||c[1]!=0x01||c[2]!=0x2a) {
                                            JANUS_LOG(LOG_WARN, "First 3-bytes after header not what they're supposed to be?\n");
                                        } else {
                                            session->keyFrame = 1;
                                            session->keyframe_number++;
                                            int vp8w = swap2(*(unsigned short*)(c+3))&0x3fff;
                                            int vp8ws = swap2(*(unsigned short*)(c+3))>>14;
                                            int vp8h = swap2(*(unsigned short*)(c+5))&0x3fff;
                                            int vp8hs = swap2(*(unsigned short*)(c+5))>>14;
                                            JANUS_LOG(LOG_INFO, "(seq=%"SCNu16", ts=%"SCNu64") Key frame: %dx%d (scale=%dx%d)\n", ntohs(rtp->seq_number), ntohl(rtp->timestamp), vp8w, vp8h, vp8ws, vp8hs);
                                            //~ packet.flags |= PKT_FLAG_KEY;
                                            packet.flags |= AV_PKT_FLAG_KEY;
                                            if(session->keyframe_ts == 0) {
                                                  session->keyframe_ts = ntohl(rtp->timestamp);
                                                  printf("First keyframe: %"SCNu64"\n", session->keyframe_ts);
                                                  printf("Value: sbit %i mark %i pbit %i\n", sbit, rtp->markerbit, pbit);
                                            }
                                        }
                                    }
                                }
                                
                                session->current_ts = ntohl(rtp->timestamp);
                                if(session->current_ts == 0 ) {
                                    printf("Time is zero ts=%"SCNu32"\n", ntohl(rtp->timestamp));
                                } else {
                                    if(session->current_ts > session->previous_ts) {   
                                        AVPacket packet;
                                        av_init_packet(&packet); 
                                        if(session->keyFrame) {
                                            if(session->keyframe_number == 1){
                                                printf("Keyframe number :%i\n", session->keyframe_number);
                                                session->received_frame = g_malloc0(640*480*3);
                                                packet.flags |= AV_PKT_FLAG_KEY;
                                                memcpy(session->received_frame, buf, len1);
                                                session->previous_ts = session->current_ts;
                                                session->frameLen += len1;
                                            } else {
                                                printf("Keyframe number :%i\n", session->keyframe_number);
                                                packet.dts = (session->previous_ts-session->keyframe_ts)/90;
                                                packet.pts = (session->previous_ts-session->keyframe_ts)/90;
                                                if(fctx) {
                                                    packet.stream_index = 0;
                                                    //packet.data = session->received_frame;
                                                    //packet.size = session->frameLen;
                                                    vpx_codec_decode(&de_codec, session->received_frame, (unsigned int)session->frameLen, NULL, 0);
                                                    vpx_codec_iter_t iter = NULL;
                                                    vpx_image_t *img = NULL;
                                                    while ((img = vpx_codec_get_frame(&de_codec, &iter)) != NULL) {
                                                        printf("Succeed decoding\n");
                                                        frame_in->width= 640;
                                                        frame_in->height= 480;
                                                        frame_in->format=AV_PIX_FMT_YUV420P;
                                                        int plane;
                                                        for (plane = 0; plane < 3; ++plane) {
                                                            frame_in->data[plane]=img->planes[plane];
                                                            frame_in->linesize[plane] = img->stride[plane];
                                                        }
                                                        if (av_buffersrc_add_frame(buffersrc_ctx, frame_in) < 0) {
                                                            printf( "Error while add frame.\n");

                                                        }
                                                        /* pull filtered pictures from the filtergraph */
                                                        ret = av_buffersink_get_frame(buffersink_ctx, frame_out);
                                                        if (ret < 0) {
                                                            printf( "Error while pulling filtered frame.\n");
                                                        }
                                                        printf("Process 1 frame!\n");
                                                        vpx_codec_iter_t iter = NULL;
                                                        const vpx_codec_cx_pkt_t *pkt;
                                                        session->raw.planes[0] = frame_out->data[0];
                                                        session->raw.planes[1] = frame_out->data[1];
                                                        session->raw.planes[2] = frame_out->data[2];
                                                        session->raw.stride[0] = frame_out->linesize[0];
                                                        session->raw.stride[1] = frame_out->linesize[1];
                                                        session->raw.stride[2] = frame_out->linesize[2];
                                                        if(session->frame_avail){
                                                            //Encode
                                                            ret=vpx_codec_encode(&session->codec,&session->raw,session->frame_cnt,1,session->flags,VPX_DL_REALTIME);
                                                        }else{
                                                            //Flush Encoder
                                                            ret=vpx_codec_encode(&session->codec,NULL,session->frame_cnt,1,session->flags,VPX_DL_REALTIME);
                                                        }
                                                        if(ret){
                                                            printf("Failed to encode frame\n");
                                                            return;
                                                        }
                                                       
                                                        pkt = vpx_codec_get_cx_data(&session->codec, &iter);
                                                        packet.data = pkt->data.frame.buf;
                                                        packet.size = pkt->data.frame.sz;
                                                        av_frame_unref(frame_out);
                                                    }
                                                    session->frame_cnt++;
                                                    if(av_write_frame(fctx, &packet) < 0) {
                                                        printf("Error writing video frame to file...\n");
                                                    }
                                                    
                                                }
                                                g_free(session->received_frame);
                                                session->received_frame = g_malloc0(640*480*3);
                                                memcpy(session->received_frame, buf, len1);
                                                session->frameLen = len1;
                                                session->previous_ts = session->current_ts;
                                                session->previous_keyframe = 1;
                                            }
                                        } else {
                                            if(session->previous_ts == 0) {
                                                session->received_frame = g_malloc0(640*480*3);
                                                memcpy(session->received_frame, buf, len1);
                                                session->frameLen = len;
                                                session->previous_ts = session->current_ts;
                                                session->previous_keyframe = 0;
                                                session->keyframe_ts = session->current_ts;
                                            }
                                            if (session->previous_keyframe){
                                                printf("Previous frame was keyframe number :%i with  time %"SCNu64" and current time %"SCNu64" \n",session->keyframe_number, session->previous_ts, session->current_ts);
                                                packet.dts = (session->previous_ts-session->keyframe_ts)/90;
                                                packet.pts = (session->previous_ts-session->keyframe_ts)/90;
                                                printf("PTS time %lu and DTS time %lu \n", packet.pts, packet.dts);
                                                packet.flags |= AV_PKT_FLAG_KEY;
                                                packet.stream_index = 0;
                                                //packet.data = session->received_frame;
                                                //packet.size = session->frameLen;
                                                vpx_codec_decode(&de_codec, session->received_frame, (unsigned int)session->frameLen, NULL, 0);
                                                vpx_codec_iter_t iter = NULL;
                                                vpx_image_t *img = NULL;
                                                while ((img = vpx_codec_get_frame(&de_codec, &iter)) != NULL) {
                                                    printf("Succeed decoding\n");
                                                    frame_in->width= 640;
                                                    frame_in->height= 480;
                                                    frame_in->format=AV_PIX_FMT_YUV420P;    
                                                    int plane;
                                                    for (plane = 0; plane < 3; ++plane) {
                                                        frame_in->data[plane]=img->planes[plane];
                                                        frame_in->linesize[plane] = img->stride[plane];
                                                    }
                                                    if (av_buffersrc_add_frame(buffersrc_ctx, frame_in) < 0) {
                                                        printf( "Error while add frame.\n");
                                                    } 
                                                    /* pull filtered pictures from the filtergraph */
                                                    ret = av_buffersink_get_frame(buffersink_ctx, frame_out);
                                                    if (ret < 0) {
                                                        printf( "Error while pulling filtered frame.\n");
                                                    }
                                                    printf("Process 1 frame!\n");
                                                    vpx_codec_iter_t iter = NULL;
                                                    const vpx_codec_cx_pkt_t *pkt;
                                                    session->raw.planes[0] = frame_out->data[0];
                                                    session->raw.planes[1] = frame_out->data[1];
                                                    session->raw.planes[2] = frame_out->data[2];
                                                    session->raw.stride[0] = frame_out->linesize[0];
                                                    session->raw.stride[1] = frame_out->linesize[1];
                                                    session->raw.stride[2] = frame_out->linesize[2];
                                                    if(session->frame_avail){
                                                        //Encode
                                                        ret=vpx_codec_encode(&session->codec,&session->raw,session->frame_cnt,1,session->flags,VPX_DL_REALTIME);
                                                    }else{
                                                        //Flush Encoder
                                                        ret=vpx_codec_encode(&session->codec,NULL,session->frame_cnt,1,session->flags,VPX_DL_REALTIME);
                                                    }
                                                    if(ret){
                                                        printf("Failed to encode frame\n");
                                                        return;
                                                    }
                                                    pkt = vpx_codec_get_cx_data(&session->codec, &iter);
                                                    packet.data = pkt->data.frame.buf;
                                                    packet.size = pkt->data.frame.sz;
                                                    av_frame_unref(frame_out);
                                                }
                                                if(fctx) {
                                                    if(av_write_frame(fctx, &packet) < 0) {
                                                        printf("Error writing video frame to file...\n");
                                                    } 
                                                } 
                                                g_free(session->received_frame);
                                                session->frameLen = 0;
                                                session->received_frame = g_malloc0(640*480*3);
                                                memcpy(session->received_frame, buf, len1);
                                                session->frameLen += len1;
                                                session->previous_ts = session->current_ts;
                                                session->previous_keyframe = 0;    
                                            } else {
                                                packet.dts = (session->previous_ts-session->keyframe_ts)/90;
                                                packet.pts = (session->previous_ts-session->keyframe_ts)/90;
                                                packet.stream_index = 0;
                                                //packet.data = session->received_frame;
                                                //packet.size = session->frameLen;
                                                vpx_codec_decode(&de_codec, session->received_frame, (unsigned int)session->frameLen, NULL, 0);
                                                vpx_codec_iter_t iter = NULL;
                                                vpx_image_t *img = NULL;
                                                while ((img = vpx_codec_get_frame(&de_codec, &iter)) != NULL) {
                                                    printf("Succeed decoding\n");
                                                    frame_in->width= 640;
                                                    frame_in->height= 480;
                                                    frame_in->format=AV_PIX_FMT_YUV420P;    
                                                    int plane;
                                                    for (plane = 0; plane < 3; ++plane) {
                                                        frame_in->data[plane]=img->planes[plane];
                                                        frame_in->linesize[plane] = img->stride[plane];
                                                    }
                                                    if (av_buffersrc_add_frame(buffersrc_ctx, frame_in) < 0) {
                                                        printf( "Error while add frame.\n");

                                                    }
                                                    /* pull filtered pictures from the filtergraph */
                                                    ret = av_buffersink_get_frame(buffersink_ctx, frame_out);
                                                    if (ret < 0) {
                                                        printf( "Error while pulling filtered frame.\n");
                                                    }
                                                    printf("Process 1 frame!\n");
                                                    vpx_codec_iter_t iter = NULL;
                                                    const vpx_codec_cx_pkt_t *pkt;
                                                    session->raw.planes[0] = frame_out->data[0];
                                                    session->raw.planes[1] = frame_out->data[1];
                                                    session->raw.planes[2] = frame_out->data[2];
                                                    session->raw.stride[0] = frame_out->linesize[0];
                                                    session->raw.stride[1] = frame_out->linesize[1];
                                                    session->raw.stride[2] = frame_out->linesize[2];
                                                    if(session->frame_avail){
                                                        //Encode
                                                        ret=vpx_codec_encode(&session->codec,&session->raw,session->frame_cnt,1,session->flags,VPX_DL_REALTIME);
                                                    }else{
                                                        //Flush Encoder
                                                        ret=vpx_codec_encode(&session->codec,NULL,session->frame_cnt,1,session->flags,VPX_DL_REALTIME);
                                                    }
                                                    if(ret){
                                                        printf("Failed to encode frame\n");
                                                        return;
                                                    }
                                                    pkt = vpx_codec_get_cx_data(&session->codec, &iter);
                                                    packet.data = pkt->data.frame.buf;
                                                    packet.size = pkt->data.frame.sz;
                                                    av_frame_unref(frame_out);
                                                }
                                                if(fctx) {
                                                    if(av_write_frame(fctx, &packet) < 0) {
                                                        printf("Error writing video frame to file...\n");
                                                    } 
                                                }
                                                g_free(session->received_frame);
                                                session->frameLen = 0;
                                                session->received_frame = g_malloc0(640*480*3);
                                                memcpy(session->received_frame, buf, len1);
                                                session->frameLen += len1;
                                                session->previous_ts = session->current_ts;
                                                session->previous_keyframe = 0;    
                                            }
                                        }
                                    } else {
                                        memcpy(session->received_frame + session->frameLen, buf, len1);
                                        session->frameLen += len1;
                                        session->previous_ts = session->current_ts;
                                    }
                                }
                        }
                    gateway->relay_rtp(handle, video, buffy, len);
                }
	}
}

void janus_echotest_incoming_rtcp(janus_plugin_session *handle, int video, char *buf, int len) {
	if(handle == NULL || handle->stopped || g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		return;
	/* Simple echo test */
	if(gateway) {
		janus_echotest_session *session = (janus_echotest_session *)handle->plugin_handle;	
		if(!session) {
			JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
			return;
		}
		if(session->destroyed)
			return;
		if(session->bitrate > 0)
			janus_rtcp_cap_remb(buf, len, session->bitrate);
		gateway->relay_rtcp(handle, video, buf, len);
	}
}

void janus_echotest_incoming_data(janus_plugin_session *handle, char *buf, int len) {
	if(handle == NULL || handle->stopped || g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		return;
	/* Simple echo test */
	if(gateway) {
		janus_echotest_session *session = (janus_echotest_session *)handle->plugin_handle;	
		if(!session) {
			JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
			return;
		}
		if(session->destroyed)
			return;
		if(buf == NULL || len <= 0)
			return;
		char *text = g_malloc0(len+1);
		memcpy(text, buf, len);
		*(text+len) = '\0';
		JANUS_LOG(LOG_VERB, "Got a DataChannel message (%zu bytes) to bounce back: %s\n", strlen(text), text);
		/* Save the frame if we're recording */
		janus_recorder_save_frame(session->drc, text, strlen(text));
		/* We send back the same text with a custom prefix */
		const char *prefix = "Janus EchoTest here! You wrote: ";
		char *reply = g_malloc0(strlen(prefix)+len+1);
		g_snprintf(reply, strlen(prefix)+len+1, "%s%s", prefix, text);
		g_free(text);
		gateway->relay_data(handle, reply, strlen(reply));
		g_free(reply);
	}
}

void janus_echotest_slow_link(janus_plugin_session *handle, int uplink, int video) {
	/* The core is informing us that our peer got or sent too many NACKs, are we pushing media too hard? */
	if(handle == NULL || handle->stopped || g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		return;
	janus_echotest_session *session = (janus_echotest_session *)handle->plugin_handle;	
	if(!session) {
		JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
		return;
	}
	if(session->destroyed)
		return;
	session->slowlink_count++;
	if(uplink && !video && !session->audio_active) {
		/* We're not relaying audio and the peer is expecting it, so NACKs are normal */
		JANUS_LOG(LOG_VERB, "Getting a lot of NACKs (slow uplink) for audio, but that's expected, a configure disabled the audio forwarding\n");
	} else if(uplink && video && !session->video_active) {
		/* We're not relaying video and the peer is expecting it, so NACKs are normal */
		JANUS_LOG(LOG_VERB, "Getting a lot of NACKs (slow uplink) for video, but that's expected, a configure disabled the video forwarding\n");
	} else {
		/* Slow uplink or downlink, maybe we set the bitrate cap too high? */
		if(video) {
			/* Halve the bitrate, but don't go too low... */
			session->bitrate = session->bitrate > 0 ? session->bitrate : 512*1024;
			session->bitrate = session->bitrate/2;
			if(session->bitrate < 64*1024)
				session->bitrate = 64*1024;
			JANUS_LOG(LOG_WARN, "Getting a lot of NACKs (slow %s) for %s, forcing a lower REMB: %"SCNu64"\n",
				uplink ? "uplink" : "downlink", video ? "video" : "audio", session->bitrate);
			/* ... and send a new REMB back */
			char rtcpbuf[24];
			janus_rtcp_remb((char *)(&rtcpbuf), 24, session->bitrate);
			gateway->relay_rtcp(handle, 1, rtcpbuf, 24);
			/* As a last thing, notify the user about this */
			json_t *event = json_object();
			json_object_set_new(event, "echotest", json_string("event"));
			json_t *result = json_object();
			json_object_set_new(result, "status", json_string("slow_link"));
			json_object_set_new(result, "bitrate", json_integer(session->bitrate));
			json_object_set_new(event, "result", result);
			gateway->push_event(session->handle, &janus_echotest_plugin, NULL, event, NULL);
			/* We don't need the event anymore */
			json_decref(event);
		}
	}
}

void janus_echotest_hangup_media(janus_plugin_session *handle) {
	JANUS_LOG(LOG_INFO, "No WebRTC media anymore\n");
	if(g_atomic_int_get(&stopping) || !g_atomic_int_get(&initialized))
		return;
	janus_echotest_session *session = (janus_echotest_session *)handle->plugin_handle;
	if(!session) {
		JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
		return;
	}
	if(session->destroyed)
		return;
	if(g_atomic_int_add(&session->hangingup, 1))
		return;
	/* Send an event to the browser and tell it's over */
	json_t *event = json_object();
	json_object_set_new(event, "echotest", json_string("event"));
	json_object_set_new(event, "result", json_string("done"));
	int ret = gateway->push_event(handle, &janus_echotest_plugin, NULL, event, NULL);
	JANUS_LOG(LOG_VERB, "  >> Pushing event: %d (%s)\n", ret, janus_get_api_error(ret));
	json_decref(event);
	/* Get rid of the recorders, if available */
	janus_mutex_lock(&session->rec_mutex);
	if(session->arc) {
		janus_recorder_close(session->arc);
		JANUS_LOG(LOG_INFO, "Closed audio recording %s\n", session->arc->filename ? session->arc->filename : "??");
		janus_recorder_free(session->arc);
	}
	session->arc = NULL;
	if(session->vrc) {
		janus_recorder_close(session->vrc);
		JANUS_LOG(LOG_INFO, "Closed video recording %s\n", session->vrc->filename ? session->vrc->filename : "??");
		janus_recorder_free(session->vrc);
	}
	session->vrc = NULL;
	if(session->drc) {
		janus_recorder_close(session->drc);
		JANUS_LOG(LOG_INFO, "Closed data recording %s\n", session->drc->filename ? session->drc->filename : "??");
		janus_recorder_free(session->drc);
	}
	session->drc = NULL;
	janus_mutex_unlock(&session->rec_mutex);
	/* Reset controls */
	session->has_audio = FALSE;
	session->has_video = FALSE;
	session->has_data = FALSE;
	session->audio_active = TRUE;
	session->video_active = TRUE;
	session->bitrate = 0;
}

/* Thread to handle incoming messages */
static void *janus_echotest_handler(void *data) {
	JANUS_LOG(LOG_VERB, "Joining EchoTest handler thread\n");
	janus_echotest_message *msg = NULL;
	int error_code = 0;
	char *error_cause = g_malloc0(512);
	json_t *root = NULL;
	while(g_atomic_int_get(&initialized) && !g_atomic_int_get(&stopping)) {
		msg = g_async_queue_pop(messages);
		if(msg == NULL)
			continue;
		if(msg == &exit_message)
			break;
		if(msg->handle == NULL) {
			janus_echotest_message_free(msg);
			continue;
		}
		janus_echotest_session *session = NULL;
		janus_mutex_lock(&sessions_mutex);
		if(g_hash_table_lookup(sessions, msg->handle) != NULL ) {
			session = (janus_echotest_session *)msg->handle->plugin_handle;
		}
		janus_mutex_unlock(&sessions_mutex);
		if(!session) {
			JANUS_LOG(LOG_ERR, "No session associated with this handle...\n");
			janus_echotest_message_free(msg);
			continue;
		}
		if(session->destroyed) {
			janus_echotest_message_free(msg);
			continue;
		}
		/* Handle request */
		error_code = 0;
		root = msg->message;
		if(msg->message == NULL) {
			JANUS_LOG(LOG_ERR, "No message??\n");
			error_code = JANUS_ECHOTEST_ERROR_NO_MESSAGE;
			g_snprintf(error_cause, 512, "%s", "No message??");
			goto error;
		}
		if(!json_is_object(root)) {
			JANUS_LOG(LOG_ERR, "JSON error: not an object\n");
			error_code = JANUS_ECHOTEST_ERROR_INVALID_JSON;
			g_snprintf(error_cause, 512, "JSON error: not an object");
			goto error;
		}
		/* Parse request */
		const char *msg_sdp_type = json_string_value(json_object_get(msg->jsep, "type"));
		const char *msg_sdp = json_string_value(json_object_get(msg->jsep, "sdp"));
		json_t *audio = json_object_get(root, "audio");
		if(audio && !json_is_boolean(audio)) {
			JANUS_LOG(LOG_ERR, "Invalid element (audio should be a boolean)\n");
			error_code = JANUS_ECHOTEST_ERROR_INVALID_ELEMENT;
			g_snprintf(error_cause, 512, "Invalid value (audio should be a boolean)");
			goto error;
		}
		json_t *video = json_object_get(root, "video");
		if(video && !json_is_boolean(video)) {
			JANUS_LOG(LOG_ERR, "Invalid element (video should be a boolean)\n");
			error_code = JANUS_ECHOTEST_ERROR_INVALID_ELEMENT;
			g_snprintf(error_cause, 512, "Invalid value (video should be a boolean)");
			goto error;
		}
		json_t *bitrate = json_object_get(root, "bitrate");
		if(bitrate && (!json_is_integer(bitrate) || json_integer_value(bitrate) < 0)) {
			JANUS_LOG(LOG_ERR, "Invalid element (bitrate should be a positive integer)\n");
			error_code = JANUS_ECHOTEST_ERROR_INVALID_ELEMENT;
			g_snprintf(error_cause, 512, "Invalid value (bitrate should be a positive integer)");
			goto error;
		}
		json_t *record = json_object_get(root, "record");
		if(record && !json_is_boolean(record)) {
			JANUS_LOG(LOG_ERR, "Invalid element (record should be a boolean)\n");
			error_code = JANUS_ECHOTEST_ERROR_INVALID_ELEMENT;
			g_snprintf(error_cause, 512, "Invalid value (record should be a boolean)");
			goto error;
		}
		json_t *recfile = json_object_get(root, "filename");
		if(recfile && !json_is_string(recfile)) {
			JANUS_LOG(LOG_ERR, "Invalid element (filename should be a string)\n");
			error_code = JANUS_ECHOTEST_ERROR_INVALID_ELEMENT;
			g_snprintf(error_cause, 512, "Invalid value (filename should be a string)");
			goto error;
		}
		/* Enforce request */
		if(audio) {
			session->audio_active = json_is_true(audio);
			JANUS_LOG(LOG_VERB, "Setting audio property: %s\n", session->audio_active ? "true" : "false");
		}
		if(video) {
			if(!session->video_active && json_is_true(video)) {
				/* Send a PLI */
				JANUS_LOG(LOG_VERB, "Just (re-)enabled video, sending a PLI to recover it\n");
				char buf[12];
				memset(buf, 0, 12);
				janus_rtcp_pli((char *)&buf, 12);
				gateway->relay_rtcp(session->handle, 1, buf, 12);
			}
			session->video_active = json_is_true(video);
			JANUS_LOG(LOG_VERB, "Setting video property: %s\n", session->video_active ? "true" : "false");
		}
		if(bitrate) {
			session->bitrate = json_integer_value(bitrate);
			JANUS_LOG(LOG_VERB, "Setting video bitrate: %"SCNu64"\n", session->bitrate);
			if(session->bitrate > 0) {
				/* FIXME Generate a new REMB (especially useful for Firefox, which doesn't send any we can cap later) */
				char buf[24];
				memset(buf, 0, 24);
				janus_rtcp_remb((char *)&buf, 24, session->bitrate);
				JANUS_LOG(LOG_VERB, "Sending REMB\n");
				gateway->relay_rtcp(session->handle, 1, buf, 24);
				/* FIXME How should we handle a subsequent "no limit" bitrate? */
			}
		}
		if(record) {
			if(msg_sdp) {
				session->has_audio = (strstr(msg_sdp, "m=audio") != NULL);
				session->has_video = (strstr(msg_sdp, "m=video") != NULL);
				session->has_data = (strstr(msg_sdp, "DTLS/SCTP") != NULL);
			}
			gboolean recording = json_is_true(record);
			const char *recording_base = json_string_value(recfile);
			JANUS_LOG(LOG_VERB, "Recording %s (base filename: %s)\n", recording ? "enabled" : "disabled", recording_base ? recording_base : "not provided");
			janus_mutex_lock(&session->rec_mutex);
			if(!recording) {
				/* Not recording (anymore?) */
				if(session->arc) {
					janus_recorder_close(session->arc);
					JANUS_LOG(LOG_INFO, "Closed audio recording %s\n", session->arc->filename ? session->arc->filename : "??");
					janus_recorder_free(session->arc);
				}
				session->arc = NULL;
				if(session->vrc) {
					janus_recorder_close(session->vrc);
					JANUS_LOG(LOG_INFO, "Closed video recording %s\n", session->vrc->filename ? session->vrc->filename : "??");
					janus_recorder_free(session->vrc);
				}
				session->vrc = NULL;
				if(session->drc) {
					janus_recorder_close(session->drc);
					JANUS_LOG(LOG_INFO, "Closed data recording %s\n", session->drc->filename ? session->drc->filename : "??");
					janus_recorder_free(session->drc);
				}
				session->drc = NULL;
			} else {
				/* We've started recording, send a PLI and go on */
				char filename[255];
				gint64 now = janus_get_real_time();
				if(session->has_audio) {
					/* FIXME We assume we're recording Opus, here */
					memset(filename, 0, 255);
					if(recording_base) {
						/* Use the filename and path we have been provided */
						g_snprintf(filename, 255, "%s-audio", recording_base);
						session->arc = janus_recorder_create(NULL, "opus", filename);
						if(session->arc == NULL) {
							/* FIXME We should notify the fact the recorder could not be created */
							JANUS_LOG(LOG_ERR, "Couldn't open an audio recording file for this EchoTest user!\n");
						}
					} else {
						/* Build a filename */
						g_snprintf(filename, 255, "echotest-%p-%"SCNi64"-audio", session, now);
						session->arc = janus_recorder_create(NULL, "opus", filename);
						if(session->arc == NULL) {
							/* FIXME We should notify the fact the recorder could not be created */
							JANUS_LOG(LOG_ERR, "Couldn't open an audio recording file for this EchoTest user!\n");
						}
					}
				}
				if(session->has_video) {
					/* FIXME We assume we're recording VP8, here */
					memset(filename, 0, 255);
					if(recording_base) {
						/* Use the filename and path we have been provided */
						g_snprintf(filename, 255, "%s-video", recording_base);
						session->vrc = janus_recorder_create(NULL, "vp8", filename);
						if(session->vrc == NULL) {
							/* FIXME We should notify the fact the recorder could not be created */
							JANUS_LOG(LOG_ERR, "Couldn't open an video recording file for this EchoTest user!\n");
						}
					} else {
						/* Build a filename */
						g_snprintf(filename, 255, "echotest-%p-%"SCNi64"-video", session, now);
						session->vrc = janus_recorder_create(NULL, "vp8", filename);
						if(session->vrc == NULL) {
							/* FIXME We should notify the fact the recorder could not be created */
							JANUS_LOG(LOG_ERR, "Couldn't open an video recording file for this EchoTest user!\n");
						}
					}
					/* Send a PLI */
					JANUS_LOG(LOG_VERB, "Recording video, sending a PLI to kickstart it\n");
					char buf[12];
					memset(buf, 0, 12);
					janus_rtcp_pli((char *)&buf, 12);
					gateway->relay_rtcp(session->handle, 1, buf, 12);
				}
				if(session->has_data) {
					memset(filename, 0, 255);
					if(recording_base) {
						/* Use the filename and path we have been provided */
						g_snprintf(filename, 255, "%s-data", recording_base);
						session->drc = janus_recorder_create(NULL, "text", filename);
						if(session->drc == NULL) {
							/* FIXME We should notify the fact the recorder could not be created */
							JANUS_LOG(LOG_ERR, "Couldn't open a text data recording file for this EchoTest user!\n");
						}
					} else {
						/* Build a filename */
						g_snprintf(filename, 255, "echotest-%p-%"SCNi64"-data", session, now);
						session->drc = janus_recorder_create(NULL, "text", filename);
						if(session->drc == NULL) {
							/* FIXME We should notify the fact the recorder could not be created */
							JANUS_LOG(LOG_ERR, "Couldn't open a text data recording file for this EchoTest user!\n");
						}
					}
				}
			}
			janus_mutex_unlock(&session->rec_mutex);
		}
		/* Any SDP to handle? */
		if(msg_sdp) {
			JANUS_LOG(LOG_VERB, "This is involving a negotiation (%s) as well:\n%s\n", msg_sdp_type, msg_sdp);
			session->has_audio = (strstr(msg_sdp, "m=audio") != NULL);
			session->has_video = (strstr(msg_sdp, "m=video") != NULL);
			session->has_data = (strstr(msg_sdp, "DTLS/SCTP") != NULL);
		}

		if(!audio && !video && !bitrate && !record && !msg_sdp) {
			JANUS_LOG(LOG_ERR, "No supported attributes (audio, video, bitrate, record, jsep) found\n");
			error_code = JANUS_ECHOTEST_ERROR_INVALID_ELEMENT;
			g_snprintf(error_cause, 512, "Message error: no supported attributes (audio, video, bitrate, record, jsep) found");
			goto error;
		}

		/* Prepare JSON event */
		json_t *event = json_object();
		json_object_set_new(event, "echotest", json_string("event"));
		json_object_set_new(event, "result", json_string("ok"));
		if(!msg_sdp) {
			int ret = gateway->push_event(msg->handle, &janus_echotest_plugin, msg->transaction, event, NULL);
			JANUS_LOG(LOG_VERB, "  >> %d (%s)\n", ret, janus_get_api_error(ret));
			json_decref(event);
		} else {
			/* Forward the same offer to the gateway, to start the echo test */
			const char *type = NULL;
			if(!strcasecmp(msg_sdp_type, "offer"))
				type = "answer";
			if(!strcasecmp(msg_sdp_type, "answer"))
				type = "offer";
			/* Any media direction that needs to be fixed? */
			char *sdp = g_strdup(msg_sdp);
			if(strstr(sdp, "a=recvonly")) {
				/* Turn recvonly to inactive, as we simply bounce media back */
				sdp = janus_string_replace(sdp, "a=recvonly", "a=inactive");
			} else if(strstr(sdp, "a=sendonly")) {
				/* Turn sendonly to recvonly */
				sdp = janus_string_replace(sdp, "a=sendonly", "a=recvonly");
				/* FIXME We should also actually not echo this media back, though... */
			}
			/* Make also sure we get rid of ULPfec, red, etc. */
			if(strstr(sdp, "ulpfec")) {
				/* FIXME This really needs some better code */
				sdp = janus_string_replace(sdp, "a=rtpmap:116 red/90000\r\n", "");
				sdp = janus_string_replace(sdp, "a=rtpmap:117 ulpfec/90000\r\n", "");
				sdp = janus_string_replace(sdp, "a=rtpmap:96 rtx/90000\r\n", "");
				sdp = janus_string_replace(sdp, "a=fmtp:96 apt=100\r\n", "");
				sdp = janus_string_replace(sdp, "a=rtpmap:97 rtx/90000\r\n", "");
				sdp = janus_string_replace(sdp, "a=fmtp:97 apt=101\r\n", "");
				sdp = janus_string_replace(sdp, "a=rtpmap:98 rtx/90000\r\n", "");
				sdp = janus_string_replace(sdp, "a=fmtp:98 apt=116\r\n", "");
				sdp = janus_string_replace(sdp, " 116", "");
				sdp = janus_string_replace(sdp, " 117", "");
				sdp = janus_string_replace(sdp, " 96", "");
				sdp = janus_string_replace(sdp, " 97", "");
				sdp = janus_string_replace(sdp, " 98", "");
			}
			json_t *jsep = json_pack("{ssss}", "type", type, "sdp", sdp);
			/* How long will the gateway take to push the event? */
			g_atomic_int_set(&session->hangingup, 0);
			gint64 start = janus_get_monotonic_time();
			int res = gateway->push_event(msg->handle, &janus_echotest_plugin, msg->transaction, event, jsep);
			JANUS_LOG(LOG_VERB, "  >> Pushing event: %d (took %"SCNu64" us)\n",
				res, janus_get_monotonic_time()-start);
			g_free(sdp);
			/* We don't need the event and jsep anymore */
			json_decref(event);
			json_decref(jsep);
		}
		janus_echotest_message_free(msg);

		if(notify_events && gateway->events_is_enabled()) {
			/* Just to showcase how you can notify handlers, let's update them on our configuration */
			json_t *info = json_object();
			json_object_set_new(info, "audio_active", session->audio_active ? json_true() : json_false());
			json_object_set_new(info, "video_active", session->video_active ? json_true() : json_false());
			json_object_set_new(info, "bitrate", json_integer(session->bitrate));
			if(session->arc || session->vrc) {
				json_t *recording = json_object();
				if(session->arc && session->arc->filename)
					json_object_set_new(recording, "audio", json_string(session->arc->filename));
				if(session->vrc && session->vrc->filename)
					json_object_set_new(recording, "video", json_string(session->vrc->filename));
				json_object_set_new(info, "recording", recording);
			}
			gateway->notify_event(&janus_echotest_plugin, session->handle, info);
		}

		/* Done, on to the next request */
		continue;
		
error:
		{
			/* Prepare JSON error event */
			json_t *event = json_object();
			json_object_set_new(event, "echotest", json_string("event"));
			json_object_set_new(event, "error_code", json_integer(error_code));
			json_object_set_new(event, "error", json_string(error_cause));
			int ret = gateway->push_event(msg->handle, &janus_echotest_plugin, msg->transaction, event, NULL);
			JANUS_LOG(LOG_VERB, "  >> %d (%s)\n", ret, janus_get_api_error(ret));
			janus_echotest_message_free(msg);
			/* We don't need the event anymore */
			json_decref(event);
		}
	}
	g_free(error_cause);
	JANUS_LOG(LOG_VERB, "Leaving EchoTest handler thread\n");
	return NULL;
}

