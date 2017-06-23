
/*! \file    audio-mixing.c
 * \author   Aboobeker Sidhik <aboosidhik@gmail.com>
 * \copyright GNU General Public License v3
 */

#include <iostream>
#include <opencv2/opencv.hpp>
#include <opencv2/core/cuda.hpp>
#include <opencv2/cudaimgproc.hpp>  

extern "C"
{
#include <arpa/inet.h>
#ifdef __MACH__
#include <machine/endian.h>
#else
#include <endian.h>
#endif
#include <stdio.h>    
#include <inttypes.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include "types.h"
#include "wave.h"
#include "noise_remover.h"
//#include "test_fract.h"
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <glib.h>
#include <opus/opus.h>    
#include <jansson.h>
#include <vpx/vpx_decoder.h>
#include <vpx/vp8dx.h>
#include "libavutil/channel_layout.h"
#include "libavutil/md5.h"
#include "libavutil/opt.h"
#include "libavutil/samplefmt.h"

#include "libavfilter/avfilter.h"
#include "libavfilter/buffersink.h"
#include "libavfilter/buffersrc.h"

#include <libavformat/avformat.h>
#include <libavfilter/avfiltergraph.h>

#include "libavformat/avio.h"

#include "libavutil/audio_fifo.h"
#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/frame.h"
#include "libavutil/opt.h"    
}


using namespace std;
using namespace cv;
#define RTP_VERSION    2
string type2str(int type) { 
  string r;

  uchar depth = type & CV_MAT_DEPTH_MASK;
  uchar chans = 1 + (type >> CV_CN_SHIFT);

  switch ( depth ) {
    case CV_8U:  r = "8U"; break;
    case CV_8S:  r = "8S"; break;
    case CV_16U: r = "16U"; break;
    case CV_16S: r = "16S"; break;
    case CV_32S: r = "32S"; break;
    case CV_32F: r = "32F"; break;
    case CV_64F: r = "64F"; break;
    default:     r = "User"; break;
  }

  r += "C";
  r += (chans+'0');

  return r;
}

#define INPUT_SAMPLERATE     48000
#define INPUT_FORMAT         AV_SAMPLE_FMT_S16
#define INPUT_CHANNEL_LAYOUT AV_CH_LAYOUT_MONO

/** The output bit rate in kbit/s */
#define OUTPUT_BIT_RATE 48000
/** The number of output channels */
#define OUTPUT_CHANNELS 1
/** The audio sample output format */
#define OUTPUT_SAMPLE_FORMAT AV_SAMPLE_FMT_S16

#define VOLUME_VAL 0.90

AVFormatContext *output_format_context = NULL;
AVCodecContext *output_codec_context = NULL;

AVFormatContext *input_format_context_0 = NULL;
AVCodecContext *input_codec_context_0 = NULL;
AVFormatContext *input_format_context_1 = NULL;
AVCodecContext *input_codec_context_1 = NULL;

AVFilterGraph *graph;
AVFilterContext *src0,*src1, *sink;
static char *const get_error_text(const int error)
{
    static char error_buffer[255];
    av_strerror(error, error_buffer, sizeof(error_buffer));
    return error_buffer;
}

static int init_filter_graph(AVFilterGraph **graph, AVFilterContext **src0, AVFilterContext **src1,
                             AVFilterContext **sink)
{
    AVFilterGraph *filter_graph;
    AVFilterContext *abuffer1_ctx;
    AVFilter        *abuffer1;
    AVFilterContext *abuffer0_ctx;
    AVFilter        *abuffer0;
    AVFilterContext *volume_0;
    AVFilter        *volume_filter_0;
    AVFilterContext *volume_1;
    AVFilter        *volume_filter_1;
    AVFilterContext *mix_ctx_0;
    AVFilter        *mix_filter_0;
    AVFilterContext *mix_ctx_1;
    AVFilter        *mix_filter_1;
    AVFilterContext *mix_ctx_u;
    AVFilter        *mix_filter;
    AVFilterContext *aformat_u;
    AVFilter        *aformat_filt;
    AVFilterContext *abuffersink_ctx;
    AVFilter        *abuffersink;
    char args[512];
    int err;
    av_register_all();
    avfilter_register_all();
    /* Create a new filtergraph, which will contain all the filters. */
    filter_graph = avfilter_graph_alloc();
    if (!filter_graph) {
        av_log(NULL, AV_LOG_ERROR, "Unable to create filter graph.\n");
        return AVERROR(ENOMEM);
    }
    /****** abuffer 0 ********/
    /* Create the abuffer filter;
     * it will be used for feeding the data into the graph. */
    abuffer0 = avfilter_get_by_name("abuffer");
    if (!abuffer0) {
        av_log(NULL, AV_LOG_ERROR, "Could not find the abuffer filter.\n");
        return AVERROR_FILTER_NOT_FOUND;
    }
    /* buffer audio source: the decoded frames from the decoder will be inserted here. */
    snprintf(args, sizeof(args),"sample_rate=48000:sample_fmt=fltp:channel_layout=mono");
    err = avfilter_graph_create_filter(&abuffer0_ctx, abuffer0, "src0",
                                       args, NULL, filter_graph);
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot create audio buffer source\n");
        return err;
    }
    /****** abuffer 1 ******* */
    /* Create the abuffer filter;
     * it will be used for feeding the data into the graph. */
   abuffer1 = avfilter_get_by_name("abuffer");
    if (!abuffer1) {
        av_log(NULL, AV_LOG_ERROR, "Could not find the abuffer filter.\n");
        return AVERROR_FILTER_NOT_FOUND;
    }
    snprintf(args, sizeof(args),"sample_rate=48000:sample_fmt=fltp:channel_layout=mono");
    err = avfilter_graph_create_filter(&abuffer1_ctx, abuffer1, "src1",
                                       args, NULL, filter_graph);
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot create audio buffer source\n");
        return err;
    }
	
    /****** amix ******* */
    /* Create mix filter. */
  /*  mix_filter_0 = avfilter_get_by_name("amix");
    if (!mix_filter_0) {
        av_log(NULL, AV_LOG_ERROR, "Could not find the mix filter.\n");
        return AVERROR_FILTER_NOT_FOUND;
    }
    snprintf(args, sizeof(args), "inputs=1");
    err = avfilter_graph_create_filter(&mix_ctx_0, mix_filter_0, "amix0",
                                       args, NULL, filter_graph);
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot create audio amix filter\n");
        return err;
    }
  */  /****** amix ******* */
    /* Create mix filter. */
  /*  mix_filter_1 = avfilter_get_by_name("amix");
    if (!mix_filter_1) {
        av_log(NULL, AV_LOG_ERROR, "Could not find the mix filter.\n");
        return AVERROR_FILTER_NOT_FOUND;
    }
    snprintf(args, sizeof(args), "inputs=1");
    err = avfilter_graph_create_filter(&mix_ctx_1, mix_filter_1, "amix2",
                                       args, NULL, filter_graph);
  */  
    /****** volume 0 ******* */
    /* Create the abuffer filter;
     * it will be used for feeding the data into the graph. */
  /*  volume_filter_0 = avfilter_get_by_name("volume");
    if (!volume_filter_0) {
        av_log(NULL, AV_LOG_ERROR, "Could not find the abuffer filter.\n");
        return AVERROR_FILTER_NOT_FOUND;
    }
    snprintf(args, sizeof(args),"volume=0.5");
    err = avfilter_graph_create_filter(&volume_0, volume_filter_0, "vol0",
                                       args, NULL, filter_graph);
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot create audio buffer source\n");
        return err;
    }
    */
    /****** volume 1 ******* */
    /* Create the abuffer filter;
     * it will be used for feeding the data into the graph. */
 /*   volume_filter_1 = avfilter_get_by_name("volume");
    if (!volume_filter_1) {
        av_log(NULL, AV_LOG_ERROR, "Could not find the abuffer filter.\n");
        return AVERROR_FILTER_NOT_FOUND;
    }
    snprintf(args, sizeof(args),"volume=0.5");
    err = avfilter_graph_create_filter(&volume_1, volume_filter_1, "vol1",
                                       args, NULL, filter_graph);
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot create audio buffer source\n");
        return err;
    }
   */ 
    /* Create mix filter. */
    mix_filter = avfilter_get_by_name("amix");
    if (!mix_filter) {
        av_log(NULL, AV_LOG_ERROR, "Could not find the mix filter.\n");
        return AVERROR_FILTER_NOT_FOUND;
    }
    snprintf(args, sizeof(args), "inputs=2");
    err = avfilter_graph_create_filter(&mix_ctx_u, mix_filter, "amix3",
                                       args, NULL, filter_graph);
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot create audio amix filter\n");
        return err;
    }
    
        /* Create aformat filter. */
 /*   aformat_filt = avfilter_get_by_name("aformat");
    if (!aformat_filt) {
        av_log(NULL, AV_LOG_ERROR, "Could not find the mix filter.\n");
        return AVERROR_FILTER_NOT_FOUND;
    }
    snprintf(args, sizeof(args), "sample_fmts=fltp|flt");
    err = avfilter_graph_create_filter(&aformat_u, aformat_filt, "aformat",
                                       args, NULL, filter_graph);
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot create audio amix filter\n");
        return err;
    }
   */ /* Finally create the abuffersink filter;
     * it will be used to get the filtered data out of the graph. */
    abuffersink = avfilter_get_by_name("abuffersink");
    if (!abuffersink) {
        av_log(NULL, AV_LOG_ERROR, "Could not find the abuffersink filter.\n");
        return AVERROR_FILTER_NOT_FOUND;
    }
	
    abuffersink_ctx = avfilter_graph_alloc_filter(filter_graph, abuffersink, "sink");
    if (!abuffersink_ctx) {
        av_log(NULL, AV_LOG_ERROR, "Could not allocate the abuffersink instance.\n");
        return AVERROR(ENOMEM);
    }
	
    /* Same sample fmts as the output file. */
    //err = av_opt_set_int_list(abuffersink_ctx, "sample_fmts", ((int[]){ AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_NONE }), AV_SAMPLE_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
   /* av_opt_set(abuffersink_ctx, "sample_fmts", "AV_SAMPLE_FMT_S16",AV_OPT_SEARCH_CHILDREN);
    uint8_t ch_layout[64];
    av_get_channel_layout_string((char *)ch_layout, sizeof(ch_layout), 0, OUTPUT_CHANNELS);
    av_opt_set    (abuffersink_ctx, "channel_layout",  (const char *)ch_layout, 0);
    
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR, "Could set options to the abuffersink instance.\n");
        return err;
    }
    */
    err = avfilter_init_str(abuffersink_ctx, NULL);
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR, "Could not initialize the abuffersink instance.\n");
        return err;
    }
    
    
    /* Connect the filters; */
   
         err = avfilter_link(abuffer0_ctx, 0, mix_ctx_u, 0);
       if (err >= 0)
            err = avfilter_link(abuffer1_ctx, 0, mix_ctx_u, 1);
    /*	if (err >= 0)
            err = avfilter_link(mix_ctx_0, 0, volume_0, 0);
        if (err >= 0)
            err = avfilter_link(mix_ctx_1, 0, volume_1, 0);
    */  /* if (err >= 0)
            err = avfilter_link(mix_ctx_0, 0, mix_ctx_u, 0);
        if (err >= 0)
           err = avfilter_link(mix_ctx_1, 0, mix_ctx_u, 1);
    /*	if (err >= 0) 
            err = avfilter_link(mix_ctx_u, 0, aformat_u, 0);
    	if (err >= 0)        
            err = avfilter_link(aformat_u, 0, abuffersink_ctx, 0);
    */ 
     	if (err >= 0)        
            err = avfilter_link(mix_ctx_u, 0, abuffersink_ctx, 0);
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR, "Error connecting filters\n");
        return err;
    }
	
    /* Configure the graph. */
    err = avfilter_graph_config(filter_graph, NULL);
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR, "Error while configuring graph : %s\n", get_error_text(err));
        return err;
    }
    
    char* dump =avfilter_graph_dump(filter_graph, NULL);
    av_log(NULL, AV_LOG_ERROR, "Graph :\n%s\n", dump);
	
    *graph = filter_graph;
    *src0   = abuffer0_ctx;
    *src1   = abuffer1_ctx;
    *sink  = abuffersink_ctx;
    
    return 0;
}

/* Helper struct to generate and parse WAVE headers */
typedef struct wav_header {
	char riff[4];
	uint32_t len;
	char wave[4];
	char fmt[4];
	uint32_t formatsize;
	uint16_t format;
	uint16_t channels;
	uint32_t samplerate;
	uint32_t avgbyterate;
	uint16_t samplebytes;
	uint16_t channelbits;
	char data[4];
	uint32_t blocksize;
} wav_header;

#define fourcc 0x30395056
#define interface (&vpx_codec_vp9_dx_algo)


static int kVp9FrameMarker = 2;
static int kMinTileWidthB64 = 4;
static int kMaxTileWidthB64 = 64;
static int kRefFrames = 8;
static int kRefsPerFrame = 3;
static int kRefFrames_LOG2 = 3;
static int kVpxCsBt601 = 1;
static int kVpxCsSrgb = 7;
static int kVpxCrStudioRange = 0;
static int kVpxCrFullRange = 1;
static int kMiSizeLog2 = 3;
static int bit_depth_ = 0;
static  int profile_ = -1;
static  int show_existing_frame_ = 0;
static  int key_ = 0;
static  int altref_ = 0;
static  int error_resilient_mode_ = 0;
static  int intra_only_ = 0;
static  int reset_frame_context_ = 0;
  
static  int color_space_ = 0;
static  int color_range_ = 0;
static  int subsampling_x_ = 0;
static  int subsampling_y_ = 0;
static  int refresh_frame_flags_ = 0;
static  int width_;
static  int height_;
static  int row_tiles_;
static  int column_tiles_;
static  int frame_parallel_mode_;
static int fm_count;
#define htonll(x) ((1==htonl(1)) ? (x) : ((gint64)htonl((x) & 0xFFFFFFFF) << 32) | htonl((x) >> 32))
#define ntohll(x) ((1==ntohl(1)) ? (x) : ((gint64)ntohl((x) & 0xFFFFFFFF) << 32) | ntohl((x) >> 32))

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

typedef struct frame_packet {
        uint16_t seq;	/* RTP Sequence number */
	uint64_t ts;	/* RTP Timestamp */
	uint16_t len;	/* Length of the data */
	int pt;			/* Payload type of the data */
	long offset;	/* Offset of the data in the file */
	int skip;		/* Bytes to skip, besides the RTP header */
	uint8_t drop;	/* Whether this packet can be dropped (e.g., padding)*/
        guint32 ntp_sec;  /* NTP timestamp */
        guint32 ntp_frac;
        uint64_t ntp_ms;
        uint64_t rtp_new;
        int dif;
        int mod;
        int counter;
        uint64_t last_tmp;
        int dummy;
        uint8_t *received_frame;
        FILE *file1;
        uint64_t rtp_ts;	/* RTP Timestamp */
	uint64_t ToMs;
        AVFrame *frame;
        AVPacket input_packet;
        int video_key_pro;
        int diff_ntps;
        uint64_t ntp_changed;
        struct frame_packet *last_pp;
	struct frame_packet *next;
	struct frame_packet *prev;
} frame_packet;

typedef struct frame_packet_list {
    size_t size;
    struct frame_packet *head;
    struct frame_packet *tail;
} frame_packet_list;

static uint8_t opus_silence[] = {
	0xf8, 0xff, 0xfe, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
typedef struct rt_type {
    char *source;
    FILE *file;
    long fsize;
    long offset;
    int count;
    frame_packet *list;
    frame_packet *last;
    gint64 c_time;
    gint64 w_time;
    uint32_t last_ts;
    uint32_t reset;    
    int times_resetted;
    uint32_t post_reset_pkts;
    gboolean parsed_header;
    int opus;
    int vp9;
    int max_width, max_height, fps;
    int min_ts_diff, max_ts_diff;
    frame_packet *list_video_pro, *last_video_pro;
    int size_video_sorted;
    frame_packet_list *list_video_sorted;
    frame_packet_list *list_video_new;
    int video_count_pro;
    
            
}rt_type;
typedef struct file_type {
    struct rt_type *rtp_pkt;
    struct rt_type *rtcp_pkt;
}file_type;
typedef struct file_av {
    struct file_type *audio_file;
    struct file_type *video_file;
    int opus;
    int vp9;
    AVCodecContext *codec_ctx;
    AVCodec *codec_dec; 
    guint32 ntp_sec;  /* NTP timestamp */
    guint32 ntp_frac;
    int numBytes;
    uint64_t ToMs;
    uint8_t *received_frame;
    uint8_t *buffer;
    uint8_t *start;
    int len, frameLen;
    int audio_len;
    int keyFrame;
    uint32_t keyframe_ts;
    int64_t audio_ts;
    int audio_pts;
    int video_pts;
    int audio;
    int video;
    gchar *buf;
    struct file_av *next;
    struct file_av *prev;
}file_av;

typedef struct file_av_list {
    size_t size;
    struct file_av *head;
    struct file_av *tail;
}file_av_list;

typedef struct file_combine {
        int num;
        char *audio_source;
        char *video_source;
        file_av_list *file_av_list_1;
        AVFrame *frame;
        AVPacket input_packet;
        AVCodecContext *avctx;
        AVCodec *input_codec;
        int data_present;
        AVFrame *frame_video;
        AVPacket input_packet_video;
        AVCodecContext *avctx_video;
        AVCodec *input_codec_video;
        int data_present_video;
        struct SwsContext *img_convert_ctx;
	struct file_combine *next;
	struct file_combine *prev;
} file_combine;

typedef struct file_combine_list {
    size_t size;
    int mix_type;
    int audio_number;
    int audio_video;
    AVCodecContext *oavctx;
    AVCodec *o_codec;
    struct SwsContext * convert_ctx;
    struct file_combine *head;
    struct file_combine *tail;
}file_combine_list;


typedef struct packet_audio {
    frame_packet *audio_pkt;
    struct packet_audio *next;
    struct packet_audio *prev;
} packet_audio;

typedef struct packet_audio_list {
    size_t size;
    //OpusEncoder *encoder;
    //GList *inbuf;			/* Incoming audio from this participant, as an ordered list of packets */
    struct packet_audio *head;
    struct packet_audio *tail;
}packet_audio_list;



int janus_log_level = 4;
gboolean janus_log_timestamps = FALSE;
gboolean janus_log_colors = TRUE;

int working = 0;


/* Signal handler */
void janus_pp_handle_signal(int signum);
void janus_pp_handle_signal(int signum) {
	working = 0;
}

/* WebRTC stuff (VP8/VP9) */
#if defined(__ppc__) || defined(__ppc64__)
	# define swap2(d)  \
	((d&0x000000ff)<<8) |  \
	((d&0x0000ff00)>>8)
#else
	# define swap2(d) d
#endif

#define LIBAVCODEC_VER_AT_LEAST(major, minor) \
	(LIBAVCODEC_VERSION_MAJOR > major || \
	 (LIBAVCODEC_VERSION_MAJOR == major && \
	  LIBAVCODEC_VERSION_MINOR >= minor))

#if LIBAVCODEC_VER_AT_LEAST(51, 42)
#define PIX_FMT_YUV420P AV_PIX_FMT_YUV420P
#endif
/* WebM output */
static AVFormatContext *fctx;
static AVStream *vStream;
static AVStream *aStream;
static AVRational audio_timebase;
static AVRational video_timebase;
static AVOutputFormat *fmt;
static AVCodec *audio_codec;
static AVCodec *video_codec;
static AVDictionary *opt_arg;
static AVCodecContext *context;
static AVCodecContext *video_context;

int file_create(char *destination, int type, char *extension) {
    if(destination == NULL)
	return -1;
    #if LIBAVCODEC_VERSION_MAJOR < 55
	printf("Your FFmpeg version does not support VP9\n");
	return -1;
    #endif
    /* Setup FFmpeg */
    av_register_all();
    avformat_alloc_output_context2(&fctx, NULL, NULL, destination);
    if (!fctx) {
        printf("Could not deduce output format from file extension: using WEBM.\n");
        if(!strcasecmp(extension, ".webm")) {
            avformat_alloc_output_context2(&fctx, fmt, "webm", destination);
        } else if (!strcasecmp(extension, ".mkv")) {
            avformat_alloc_output_context2(&fctx, fmt, "mkv", destination);
        } else if (!strcasecmp(extension, ".mp4")) {
            avformat_alloc_output_context2(&fctx, fmt, "mp4", destination);
        }

    }
    if (!fctx) {
        return -1;
    }    
    int ret;
    fmt = fctx->oformat;
    if(type == 1 || type == 3) {
        if (!strcasecmp(extension, ".webm")) {
            audio_codec = avcodec_find_encoder(AV_CODEC_ID_OPUS);
            aStream = avformat_new_stream(fctx, NULL);
            if (!aStream) {
                printf("Could not allocate audio stream\n");
                return -1;
            } 
            aStream->id = fctx->nb_streams-1;
            context = avcodec_alloc_context3(audio_codec);
            if (!context) {
                printf("Could not alloc an encoding context\n");
                return -1;
            } 
            context->codec_type = AVMEDIA_TYPE_AUDIO;
            context->codec_id = AV_CODEC_ID_OPUS;
            //context->sample_fmt = AV_SAMPLE_FMT_S16;
            context->sample_fmt = AV_SAMPLE_FMT_FLT;
            context->bit_rate = 64000;
            context->sample_rate = 48000;
            context->channel_layout = AV_CH_LAYOUT_MONO;
            context->channels = 1;
            aStream->time_base = (AVRational){ 1, context->sample_rate};
            audio_timebase = (AVRational){ 1, context->sample_rate};
            /* Some formats want stream headers to be separate. */
        /*    audio_codec = avcodec_find_encoder(AV_CODEC_ID_PCM_S16LE); // AV_CODEC_ID_PCM_S16BE
            aStream = avformat_new_stream(fctx, NULL);
            if (!aStream) {
                printf("Could not allocate audio stream\n");
                return -1;
            } 
            aStream->id = fctx->nb_streams-1;
            context = avcodec_alloc_context3(audio_codec);
            if (!context) {
                printf("Could not alloc an encoding context\n");
                return -1;
            } 
            context->codec_type = AVMEDIA_TYPE_AUDIO;
            context->codec_id = AV_CODEC_ID_PCM_S16LE;
            context->sample_fmt = AV_SAMPLE_FMT_S16;
            context->bit_rate = 64000;
            context->sample_rate = 48000;
            context->channel_layout = AV_CH_LAYOUT_MONO;
            context->channels = 1;
            aStream->time_base = (AVRational){ 1, context->sample_rate};
            audio_timebase = (AVRational){ 1, context->sample_rate};
            /* Some formats want stream headers to be separate. */
        }else if (!strcasecmp(extension, ".mp4")) {
            audio_codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
            aStream = avformat_new_stream(fctx, NULL);
            if (!aStream) {
                printf("Could not allocate audio stream\n");
                return -1;
            }
            aStream->id = fctx->nb_streams-1;
            context = avcodec_alloc_context3(audio_codec);
            if (!context) {
                printf("Could not alloc an encoding context\n");
                return -1;
            }
            context->codec_type = AVMEDIA_TYPE_AUDIO;
            context->codec_id = AV_CODEC_ID_AAC;
            context->sample_fmt = AV_SAMPLE_FMT_S16;
            //context->sample_fmt = AV_SAMPLE_FMT_FLT;
            context->bit_rate = 64000;
            context->sample_rate = 48000;
            context->channel_layout = AV_CH_LAYOUT_MONO;
            context->channels = 1;
            aStream->time_base = (AVRational){ 1, context->sample_rate};
            audio_timebase = (AVRational){ 1, context->sample_rate};
            /* Some formats want stream headers to be separate. */
        }
        if (fctx->oformat->flags & AVFMT_GLOBALHEADER) {
            context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }    
        int ret;
        /* open it */
        opt_arg = NULL;
        ret = avcodec_open2(context, audio_codec, &opt_arg);
        if (ret < 0) {
            printf("Could not open audio codec\n");
            return -1;
        }
        /* copy the stream parameters to the muxer */
        ret = avcodec_parameters_from_context(aStream->codecpar, context);
        if (ret < 0) {
            printf("Could not copy the stream parameters\n");
            return -1;
        }    
    }
    if(type == 2 || type == 3) {
         if (!strcasecmp(extension, ".webm")) {
            video_codec = avcodec_find_encoder(AV_CODEC_ID_VP9);
            vStream = avformat_new_stream(fctx, NULL);
            if (!vStream) {
                printf("Could not allocate video stream\n");
                return -1;
            }
            vStream->id = fctx->nb_streams-1;
            video_context = avcodec_alloc_context3(video_codec);
            if (!video_context) {
                printf("Could not alloc an encoding context\n");
                return -1;
            }
            video_context->codec_type = AVMEDIA_TYPE_VIDEO;
            video_context->codec_id = AV_CODEC_ID_VP9;
            video_context->width = 400;//max_width;
            video_context->height = 300; //max_height;
            vStream->time_base = (AVRational){1, 30};//(AVRational){1, fps};
            video_timebase = (AVRational){1, 30};
            video_context->time_base = vStream->time_base;
            video_context->pix_fmt = AV_PIX_FMT_YUV420P;
        } else if (!strcasecmp(extension, ".mkv")) {
            video_codec = avcodec_find_encoder(AV_CODEC_ID_H264);
            vStream = avformat_new_stream(fctx, NULL);
            if (!vStream) {
                printf("Could not allocate video stream\n");
                return -1;
            }
            vStream->id = fctx->nb_streams-1;
            video_context = avcodec_alloc_context3(video_codec);
            if (!video_context) {
                printf("Could not alloc an encoding context\n");
                return -1;
            }
            video_context->codec_type = AVMEDIA_TYPE_VIDEO;
            video_context->codec_id = AV_CODEC_ID_H264;
            video_context->width = 400;//max_width;
            video_context->height = 300; //max_height;
            vStream->time_base = (AVRational){1, 30};//(AVRational){1, fps};
            video_timebase = (AVRational){1, 30};
            video_context->time_base = vStream->time_base;
            video_context->pix_fmt = AV_PIX_FMT_YUV420P;
        }
        /* Some formats want stream headers to be separate. */
        if (fctx->oformat->flags & AVFMT_GLOBALHEADER) {
            video_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }    
        /* 
        ret = avcodec_open2(node->video_context, node->video_codec, &node->opt_arg);
        if (ret < 0) {
            fprintf(stderr, "Could not open video codec: %s\n", av_err2str(ret));
            return NULL;
        }
        */      
        /* copy the stream parameters to the muxer */
        ret = avcodec_parameters_from_context(vStream->codecpar, video_context);
        if (ret < 0) {
            printf("Could not copy the stream parameters\n");
            return -1;
        }    
    }
    av_dump_format(fctx, 0, destination, 1);
    /* open the output file, if needed */
    if (!(fmt->flags & AVFMT_NOFILE)) {
        ret = avio_open(&fctx->pb, destination, AVIO_FLAG_WRITE);
        if (ret < 0) {
            printf("Could not open file\n");
            return -1;
        }
    }
    /* Write the stream header, if any. */
    ret = avformat_write_header(fctx, &opt_arg);
    if (ret < 0) {
        printf("Error occurred when opening output file\n");
        return -1;
    }
    return 0;
}
/*
typedef struct frame_packet {
        int plen;
        char *payload;
        int64_t ts;
        int64_t seq;
} frame_packet;

*/

/* Close WebM file */
void janus_pp_webm_close(void) {
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
}



typedef enum {
  RTCP_SR   = 200,
  RTCP_RR   = 201,
  RTCP_SDES = 202,
  RTCP_BYE  = 203,
  RTCP_APP  = 204
} rtcp_type_t;
/*
 * SDES item
 */
typedef struct {
  guint8 type;              /* type of item (rtcp_sdes_type_t) */
  guint8 length;            /* length of item (in octets) */
  char data[1];             /* text, not null-terminated */
} rtcp_sdes_item_t;

/*
 * One RTCP packet
 */

typedef struct {
#if __BYTE_ORDER == __BIG_ENDIAN
  unsigned int version:2;   /* protocol version */
  unsigned int p:1;         /* padding flag */
  unsigned int count:5;     /* varies by packet type */
  unsigned int pt:8;        /* RTCP packet type */
#elif __BYTE_ORDER == __LITTLE_ENDIAN
  unsigned int count:5;  
  unsigned int p:1;   
  unsigned int version:2; 
  unsigned int pt:8;   
#endif
  guint16 length;           /* pkt len in words, w/o this word */
} rtcp_common_t;
typedef struct {
 guint32 ssrc;             /* data source being reported */
  unsigned int fraction:8;  /* fraction lost since last SR/RR */
  int lost:24;              /* cumul. no. pkts lost (signed!) */
  guint32 last_seq;         /* extended last seq. no. received */
  guint32 jitter;           /* interarrival jitter */
  guint32 lsr;              /* last SR packet from this source */
  guint32 dlsr;             /* delay since last SR packet */
} rtcp_rr_t;
                                                               
typedef struct {
  rtcp_common_t common;     /* common header */
  union {
    /* sender report (SR) */
    struct {
      guint32 ssrc;     /* sender generating this report */
      guint32 ntp_sec;  /* NTP timestamp */
      guint32 ntp_frac;
      guint32 rtp_ts;   /* RTP timestamp */
      guint32 psent;    /* packets sent */
      guint32 osent;    /* octets sent */
      rtcp_rr_t rr[1];  /* variable-length list */
    } sr;

    /* reception report (RR) */
    struct {
      guint32 ssrc;     /* receiver generating this report */
      rtcp_rr_t rr[1];  /* variable-length list */
    } rr;

    /* source description (SDES) */
    struct rtcp_sdes {
      guint32 src;      /* first SSRC/CSRC */
      rtcp_sdes_item_t item[1]; /* list of SDES items */
    } sdes;

    /* BYE */
    struct {
      guint32 src[1];   /* list of sources */
      /* can't express trailing text for reason */
    } bye;
  } r;
} rtcp_t;

int video_preprocess(rt_type *rtp_pkt) {
    if(!rtp_pkt)
	return -1;
    
    frame_packet *list = rtp_pkt->list;
    frame_packet *tmp = rtp_pkt->list;
    FILE *file = rtp_pkt->file;
    int bytes = 0;
    rtp_pkt->min_ts_diff = 0, rtp_pkt->max_ts_diff = 0, rtp_pkt->max_height = 0, rtp_pkt->max_width = 0, rtp_pkt->fps = 0;
    char prebuffer[1500];
    memset(prebuffer, 0, 1500);
    while(tmp) {
    	if(tmp == list || tmp->ts > tmp->prev->ts) {
            if(tmp->prev != NULL && tmp->ts > tmp->prev->ts) {
		int diff = tmp->ts - tmp->prev->ts;
                if(rtp_pkt->min_ts_diff == 0 || rtp_pkt->min_ts_diff > diff)
                    rtp_pkt->min_ts_diff = diff;
                if(rtp_pkt->max_ts_diff == 0 || rtp_pkt->max_ts_diff < diff)
                    rtp_pkt->max_ts_diff = diff;
            }
            if(tmp->prev != NULL && (tmp->seq - tmp->prev->seq > 1)) {
            	printf("Lost a packet here? (got seq %"SCNu16" after %"SCNu16", time ~%"SCNu64"s)\n",
                    tmp->seq, tmp->prev->seq, (tmp->ts-list->ts)/90000);
            }
        }
	if(tmp->drop) {
            // We marked this packet as one to drop, before 
            printf("Dropping previously marked video packet (time ~%"SCNu64"s)\n", (tmp->ts-list->ts)/90000);
            tmp = tmp->next;
            continue;
	}
	// https://tools.ietf.org/html/draft-ietf-payload-vp9 
	// Read the first bytes of the payload, and get the first octet (VP9 Payload Descriptor) 
	fseek(file, tmp->offset+12+tmp->skip, SEEK_SET);
	bytes = fread(prebuffer, sizeof(char), 16, file);
	if(bytes != 16)
            printf("Didn't manage to read all the bytes we needed (%d < 16)...\n", bytes);
	char *buffer = (char *)&prebuffer;
	uint8_t vp9pd = *buffer;
	uint8_t ibit = (vp9pd & 0x80);
	uint8_t pbit = (vp9pd & 0x40);
	uint8_t lbit = (vp9pd & 0x20);
	uint8_t fbit = (vp9pd & 0x10);
	uint8_t vbit = (vp9pd & 0x02);
	buffer++;
	if(ibit) {
            // Read the PictureID octet 
            vp9pd = *buffer;
            uint16_t picid = vp9pd, wholepicid = picid;
            uint8_t mbit = (vp9pd & 0x80);
            if(!mbit) {
		buffer++;
            } else {
		memcpy(&picid, buffer, sizeof(uint16_t));
		wholepicid = ntohs(picid);
		picid = (wholepicid & 0x7FFF);
		buffer += 2;
            }
        }
	if(lbit) {
            buffer++;
            if(!fbit) {
            	// Non-flexible mode, skip TL0PICIDX 
            	buffer++;
            }
	}
	if(fbit && pbit) {
            // Skip reference indices 
            uint8_t nbit = 1;
            while(nbit) {
		vp9pd = *buffer;
		nbit = (vp9pd & 0x01);
		buffer++;
            }
	}
	if(vbit) {
            // Parse and skip SS 
            vp9pd = *buffer;
            uint n_s = (vp9pd & 0xE0) >> 5;
            n_s++;
            uint8_t ybit = (vp9pd & 0x10);
            if(ybit) {
            	// Iterate on all spatial layers and get resolution 
            	buffer++;
            	uint i=0;
            	for(i=0; i<n_s; i++) {
                    // Width 
                    uint16_t *w = (uint16_t *)buffer;
                    int width = ntohs(*w);
                    buffer += 2;
                    // Height 
                    uint16_t *h = (uint16_t *)buffer;
                    int height = ntohs(*h);
                    buffer += 2;
                    if(width > rtp_pkt->max_width)
                    	rtp_pkt->max_width = width;
                    if(height > rtp_pkt->max_height)
                    	rtp_pkt->max_height = height;
                }
            }
	}
	tmp = tmp->next;
    }
    int mean_ts = rtp_pkt->min_ts_diff;	// FIXME: was an actual mean, (max_ts_diff+min_ts_diff)/2; 
    rtp_pkt->fps = (90000/(mean_ts > 0 ? mean_ts : 30));
    printf( "  -- %dx%d (fps [%d,%d] ~ %d)\n", rtp_pkt->max_width, rtp_pkt->max_height, rtp_pkt->min_ts_diff, rtp_pkt->max_ts_diff, rtp_pkt->fps);
    if(rtp_pkt->max_width == 0 && rtp_pkt->max_height == 0) {
    	printf("No key frame?? assuming 640x480...\n");
    	rtp_pkt->max_width = 640;
    	rtp_pkt->max_height = 480;
    }
    if(rtp_pkt->fps == 0) {
    	printf("No fps?? assuming 1...\n");
    	rtp_pkt->fps = 1;	// Prevent divide by zero error 
    }
    return 0;
    
}

int webm_process_1(file_combine_list *file_combine_list_1) {
	if(!file_combine_list_1)
		return -1;
        /* 1) audio decoding
         * 2) check timestamp and mix it 
         * 3) audio encode
         * 4) save to webm file
         */
        /*file_combine *file_combine_1 = file_combine_list_1->head;
        int j,m;
         int frame_cnt = 0;
        GList *first;
        for(j = 0; j <file_combine_list_1->size; j++) {
            file_av *file_av_1 = file_combine_1->file_av_list_1->head;
            frame_packet *tmp = file_av_1->list;
            fseek(file_av_1->file, tmp->offset+12+tmp->skip, SEEK_SET);
            file_av_1->len = tmp->len-12-tmp->skip;
            bytes = fread(buffer, sizeof(char), file_av_1->len, file_av_1->file);
            if(bytes != file_av_1->len)
                printf("Didn't manage to read all the bytes we needed (%d < %d)...\n", bytes, file_av_1->len);
            frame_packet *frame_packet_1 = malloc(sizeof(frame_packet));
            frame_packet_1->payload = buffer;
            frame_packet_1->plen = file_av_1->len;
            frame_packet_1->ts = tmp->ts;
            frame_packet_1->seq = tmp->seq;
            g_list_append(first, frame_packet_1);
            file_combine_1 = file_combine_1->next;
        }
        */
    return 0;
}
/* Helper to sort incoming RTP packets by timestamp */
static gint rtp_sort(gconstpointer a, gconstpointer b, gpointer user_data) {
	frame_packet *pkt1 = (frame_packet *)a;
	frame_packet *pkt2 = (frame_packet *)b;
        printf("pk1 ts % "SCNu64" pk2 ts % "SCNu64"\n", pkt1->ntp_ms, pkt2->ntp_ms);
	if(pkt1->ntp_ms < pkt2->ntp_ms) {
		return -1;
	} else if(pkt1->ntp_ms > pkt2->ntp_ms) {
		return 1;
	}
        return 0;
}
static gint rtp_sort_dif(gconstpointer a, gconstpointer b, gpointer user_data) {
	frame_packet *pkt1 = (frame_packet *)a;
	frame_packet *pkt2 = (frame_packet *)b;
        printf("pk1 ts % "SCNu64" pk2 ts % "SCNu64"\n", pkt1->dif, pkt2->dif);
	if(pkt1->dif < pkt2->dif) {
		return -1;
	} else if(pkt1->dif > pkt2->dif) {
		return 1;
	}
        return 0;
}
static gint rtp_sort_video(gconstpointer a, gconstpointer b, gpointer user_data) {
	frame_packet *pkt1 = (frame_packet *)a;
	frame_packet *pkt2 = (frame_packet *)b;
        printf("pk1 ts % "SCNu64" pk2 ts % "SCNu64"\n", pkt1->ntp_changed, pkt2->ntp_changed);
	if(pkt1->ntp_changed < pkt2->ntp_changed) {
		return -1;
	} else if(pkt1->ntp_changed > pkt2->ntp_changed) {
		return 1;
	}
        return 0;
}
/*
int audio_mix_process_1(file_combine_list *file_combine_list_1) {
    if(!file_combine_list_1)
    	return -1;
    file_combine *file_combine_1 = file_combine_list_1->head;
    // after decoding check the timestamp
    GQueue *inbuf = g_queue_new ();
    g_queue_init (inbuf);
    int count = 0;
    int i;
    for (i = 0; i < file_combine_list_1->size; i++) {
        file_av *file_av_1 = file_combine_1->file_av_list_1->head;
        frame_packet *tmp1 = file_av_1->list;
        while(tmp1 !=NULL) {
            if (count == 0) {
                g_queue_push_tail(inbuf, tmp1);
            } else {
                g_queue_insert_sorted(inbuf,tmp1,rtp_sort, NULL);
            }
            count++;
            tmp1 = tmp1->next;
        }
        file_combine_1 = file_combine_1->next;
    }    
    guint length = inbuf->length;
    printf("length % "SCNu64"\n", length);
    GList *first = inbuf->head;
    for (i = 0; i < length; i++) {
        frame_packet *pkt = (frame_packet *)first->data;
        //printf("ts % "SCNu64"\n", pkt->ntp_ms);
        first = first->next;
    }
    return 0;
}
*/
int audio_mix_process(file_combine_list *file_combine_list_1, char *extension) {
    if(!file_combine_list_1)
    	return -1;
    file_combine *file_combine_1 = file_combine_list_1->head;
    // after decoding check the timestamp
    frame_packet *lowest_tmp;
    frame_packet *highest;
    uint64_t highest_tmp;
    frame_packet packet_sort[file_combine_list_1->size];
    int i;
    int dif;
    for( i = 0; i < file_combine_list_1->size; i++) {
        file_av *file_av_1 = file_combine_1->file_av_list_1->head;
        rt_type *rtp_pkt = file_av_1->audio_file->rtp_pkt;
        printf("Counter % "SCNu64"\n", rtp_pkt->count);
        if(i == 0) {
            lowest_tmp = rtp_pkt->list;
            lowest_tmp->dif = 0;
            printf("lowest first ts % "SCNu64"\n", lowest_tmp->ntp_ms);
        } else {
            if(lowest_tmp->ntp_ms > rtp_pkt->list->ntp_ms) { 
                lowest_tmp = rtp_pkt->list;
                printf("lowest second ts % "SCNu64"\n", lowest_tmp->ntp_ms);
            } 
        }
        packet_sort[i] = *rtp_pkt->list;
        packet_sort[i].counter = rtp_pkt->count;
        packet_sort[i].last_tmp = rtp_pkt->last->ntp_ms;
        packet_sort[i].last_pp = rtp_pkt->last;
        packet_sort[i].file1 = rtp_pkt->file;
        file_combine_1 = file_combine_1->next;
    }
    GQueue *inbuf = g_queue_new ();
    g_queue_init (inbuf);
    for (i = 0; i < file_combine_list_1->size; i++) {
        printf("maria ts % "SCNu64"\n", packet_sort[i].ntp_ms); 
        packet_sort[i].mod = (packet_sort[i].ntp_ms-lowest_tmp->ntp_ms)% 20;
        if(packet_sort[i].mod > 10) {
            packet_sort[i].dif = ((packet_sort[i].ntp_ms-lowest_tmp->ntp_ms)/ 20) + 1;
        } else {
            packet_sort[i].dif = (packet_sort[i].ntp_ms-lowest_tmp->ntp_ms)/ 20;
        }
        if(i == 0) {
            g_queue_push_tail(inbuf, &packet_sort[i]);
        } else {
             g_queue_insert_sorted(inbuf,&packet_sort[i],rtp_sort_dif, NULL);
        }
        printf("dif ts % "SCNu64"\n", packet_sort[i].dif); 
        printf("mod ts % "SCNu64"\n", packet_sort[i].mod); 
    }
    guint length = inbuf->length;
    printf("length % "SCNu64"\n", length);
    GList *first = inbuf->head;
    for (i = 0; i < length; i++) {
        frame_packet *pkt = &packet_sort[i];
        printf("ts % "SCNu64"\n", pkt->dif);
        printf("mod % "SCNu64"\n", pkt->mod);
        if(pkt->dif != 0 || pkt->mod != 0) {
            int mod =  pkt->mod;
            while(pkt != NULL){
                if (mod <= 10) {
                    pkt->ntp_ms =  pkt->ntp_ms - mod;
                } else {
                    pkt->ntp_ms =  pkt->ntp_ms - mod + 20;
                }
                if(pkt->next == NULL) {
                    packet_sort[i].last_tmp = pkt->ntp_ms;
                    packet_sort[i].last_pp = pkt;
                }
                pkt = pkt->next;
            }
        }
    }
    for (i = 0; i < length; i++) {
        frame_packet *pkt = &packet_sort[i];
        if(i == 0) {
            highest_tmp = pkt->last_tmp;
        } else {
            if(highest_tmp < pkt->last_tmp) { 
                highest_tmp = pkt->last_tmp;
            }
        }
    }
    int cc;
    g_queue_sort(inbuf,rtp_sort,NULL);
    first = inbuf->head;
    for (i = 0; i < length; i++) {
        frame_packet *pkt = (frame_packet *)first->data;
        printf("ts % "SCNu64"\n", pkt->ntp_ms);
        int dif_num = 0;
        if((highest_tmp - pkt->last_tmp) != 0){
            frame_packet *pkt1 = pkt->last_pp;
            dif_num = (highest_tmp - pkt->last_tmp)/20;
            uint64_t last_tmp = pkt->last_tmp;
            int cum;
            for(cum = 0; cum < dif_num; cum++){
                frame_packet *pkt_dummy = (frame_packet *)g_malloc0(sizeof(frame_packet));
                pkt_dummy->ntp_ms = last_tmp + ((cum+1)*20);
                pkt_dummy->dummy = 1;
                pkt_dummy->prev = pkt1;
                pkt_dummy->next = NULL;
                if(pkt1->next != NULL) {
                    pkt_dummy->next = pkt1->next;
                    pkt->next->prev = pkt_dummy;
                }
                pkt1->next = pkt_dummy;
                pkt1 = pkt_dummy;
            }
        }
        cc = pkt->dif + pkt->counter + dif_num;
        printf("Total counter % "SCNu64"\n", cc);
        first = first->next;
    }
    int xm;
    int sk;
    int dify;
    uint64_t first_time;
    first = inbuf->head;
    GQueue *inbuf_added = g_queue_new ();
    g_queue_init (inbuf_added);
    for( i = 0; i< file_combine_list_1->size; i++) {
        frame_packet *pkt = (frame_packet *)first->data;
        int counter = 0;
        printf("counter dif % "SCNu64"\n", pkt->dif);
        sk = pkt->dif;
        dify = pkt->dif;
        first_time = pkt->ntp_ms;
        for (xm = 0; xm < sk; xm++) {
            frame_packet *pkt_dummy = (frame_packet *)g_malloc0(sizeof(frame_packet));
            counter = counter + 1;
            if(counter == 433 || counter == 434)
            printf("my dad counter % "SCNu64"\n", counter);
            pkt_dummy->counter = counter;
            pkt_dummy->dummy = 1;
            pkt_dummy->ntp_ms = (first_time - ((xm+1)*20));
            pkt_dummy->prev = NULL;
            pkt_dummy->next = pkt;
            if(pkt->prev != NULL) {
                pkt_dummy->prev = pkt->prev;
                pkt->prev->next = pkt_dummy;
            } 
            pkt->prev = pkt_dummy;
            pkt = pkt_dummy; 
        }
        printf("counter xxx dif % "SCNu64"\n", pkt->counter);
        pkt->dif = dify;
        first->data = pkt;
        first = first->next;
    }
    first = inbuf->head;
    int count_t = 0;
    gchar *buf = (gchar *)g_malloc0((sizeof(gchar))*1000);
    opus_int16 *data = (opus_int16 *)g_malloc0(8000*sizeof(opus_int16));
    //float *data = (float *)g_malloc0(8000*sizeof(float));
    int audio_len = 0;
    int bytes = 0;
    int error = 0;
    opus_int32 buffer[960];
    opus_int16 outBuffer[960], sumBuffer[960], *curBuffer = NULL;
    //float outBuffer[960],buffer[960], sumBuffer[960], *curBuffer = NULL;
    memset(buffer, 0, 960*4);
    memset(sumBuffer, 0, 960*2);
    memset(outBuffer, 0, 960*2);
    //memset(buffer, 0, 960*sizeof(float));
    //memset(sumBuffer, 0, 960*sizeof(float));
    //memset(outBuffer, 0, 960*sizeof(float));
    OpusDecoder *decoder = opus_decoder_create(48000, 1, &error);
    for(i=0; i<960; i++) {
	outBuffer[i] = 0;
    }
    OpusEncoder *rtp_encoder;
    //rtp_encoder = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &error);
    rtp_encoder = opus_encoder_create(48000, 1, OPUS_APPLICATION_AUDIO, &error);        
    opus_encoder_ctl(rtp_encoder, OPUS_SET_MAX_BANDWIDTH(OPUS_BANDWIDTH_FULLBAND));
    opus_encoder_ctl(rtp_encoder, OPUS_SET_COMPLEXITY(10));
    opus_encoder_ctl(rtp_encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    unsigned char *rtpbuffer = (unsigned char *)g_malloc0(1500*sizeof(unsigned char));
    int pts_audio_video = 0;
    uint64_t first_tmp = 0;
    frame_packet *pkt = (frame_packet *)first->data;
    first_tmp = pkt->ntp_ms;
    unsigned char *fbytes = (unsigned char*)malloc(960*2);
    FILE *recording;
    recording = fopen("ss.wav", "wb");
    /* Write WAV header */
    wav_header header = {
        {'R', 'I', 'F', 'F'},
        0,
        {'W', 'A', 'V', 'E'},
        {'f', 'm', 't', ' '},
        16,
        1,
        1,
        48000,
        48000 * 2,
        2,
        16,
        {'d', 'a', 't', 'a'},
        0
    };
    if(fwrite(&header, 1, sizeof(header), recording) != sizeof(header)) {
	printf("Error writing WAV header...\n");
    }
    fflush(recording);
    opus_int16 *array_out;
    array_out = curBuffer;
    struct noise_remover_s nrm;
    int          err;
    int16_t      x;
    int16_t      y;
    uint32_t     samples;
    uint32_t     processed;
    noise_remover_init( &nrm );
    init_filter_graph(&graph, &src0, &src1, &sink);
    AVFilterContext* buffer_contexts[2];
    buffer_contexts[0] = src0;
    buffer_contexts[1] = src1;
    file_combine *file_combine_20 = file_combine_list_1->head;
    for( i = 0; i< file_combine_list_1->size; i++) {
        file_combine_20->input_codec = avcodec_find_decoder_by_name("opus");
        file_combine_20->avctx = avcodec_alloc_context3(file_combine_20->input_codec);
        if (!file_combine_20->avctx) {
            printf("Could not alloc an encoding context\n");
            return -1;
        }
        file_combine_20->avctx->codec_type = AVMEDIA_TYPE_AUDIO;
        file_combine_20->avctx->codec_id = AV_CODEC_ID_OPUS;
        file_combine_20->avctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
        //avctx->request_sample_fmt = AV_SAMPLE_FMT_FLT;
        file_combine_20->avctx->bit_rate = 64000;
        file_combine_20->avctx->sample_rate = 48000;
        file_combine_20->avctx->channel_layout = AV_CH_LAYOUT_MONO;
        file_combine_20->avctx->channels = 1;
        avcodec_open2(file_combine_20->avctx, file_combine_20->input_codec, NULL);
        file_combine_20 = file_combine_20->next;
    }
    file_combine_list_1->o_codec = avcodec_find_encoder_by_name("libopus");
    file_combine_list_1->oavctx = avcodec_alloc_context3(file_combine_list_1->o_codec);
    if (!file_combine_list_1->oavctx) {
        printf("Could not alloc an encoding context\n");
        return -1;
    }
    file_combine_list_1->oavctx->codec_type = AVMEDIA_TYPE_AUDIO;
    file_combine_list_1->oavctx->codec_id = AV_CODEC_ID_OPUS;
    file_combine_list_1->oavctx->sample_fmt = AV_SAMPLE_FMT_FLT;
    //oavctx->request_sample_fmt = AV_SAMPLE_FMT_FLT;
    file_combine_list_1->oavctx->bit_rate = 64000;
    file_combine_list_1->oavctx->sample_rate = 48000;
    file_combine_list_1->oavctx->channel_layout = AV_CH_LAYOUT_MONO;
    file_combine_list_1->oavctx->channels = 1;
    avcodec_open2(file_combine_list_1->oavctx, file_combine_list_1->o_codec, NULL);
    AVCodecContext *mavctx;
    AVCodec *m_codec;    
    m_codec =  avcodec_find_encoder(AV_CODEC_ID_AAC);
    mavctx = avcodec_alloc_context3(m_codec);
    if (!mavctx) {
        printf("Could not alloc an encoding context\n");
        return -1;
    }
    mavctx->codec_type = AVMEDIA_TYPE_AUDIO;
    mavctx->codec_id = AV_CODEC_ID_AAC;
    mavctx->sample_fmt = AV_SAMPLE_FMT_FLT;
    mavctx->bit_rate = 64000;
    mavctx->sample_rate = 48000;
    mavctx->channel_layout = AV_CH_LAYOUT_MONO;
    mavctx->channels = 1;
    avcodec_open2(mavctx, m_codec, NULL);
   
    for(i = 0; i< cc; i++) {
        int m;
        for(m=0; m<960; m++) {
            sumBuffer[m] = 0;
        }
        first = inbuf->head;
        int j;
        uint64_t current_tmp;
        int xy;
        file_combine *file_combine_22 = file_combine_list_1->head;
        for (j = 0; j< file_combine_list_1->size; j++) {
            frame_packet *pkt = (frame_packet *)first->data;
            int y = 0;
	    current_tmp = pkt->ntp_ms;
            file_combine_22->frame = NULL;
            if (!(file_combine_22->frame = av_frame_alloc())) {
                av_log(NULL, AV_LOG_ERROR, "Could not allocate input frame\n");
                return AVERROR(ENOMEM);
            }
            if(!pkt->dummy) {
                fseek(pkt->file1, pkt->offset+12+pkt->skip, SEEK_SET);
                audio_len = pkt->len-12-pkt->skip;
                bytes = fread(buf, sizeof(char), audio_len, pkt->file1);
                if(pkt->next != NULL){
                    pkt->next->file1 = pkt->file1; 
                }
                //AVPacket input_packet;
                int error;
                av_init_packet(&file_combine_22->input_packet);
                file_combine_22->input_packet.data = (uint8_t *)buf;
                file_combine_22->input_packet.size = audio_len;
		file_combine_22->input_packet.pts = pts_audio_video;
                file_combine_22->input_packet.dts = pts_audio_video;
                file_combine_22->data_present = 0;
                avcodec_decode_audio4(file_combine_22->avctx, file_combine_22->frame,&file_combine_22->data_present, &file_combine_22->input_packet);
            } else {
                /** Packet used for temporary storage. */
                //AVPacket input_packet;
                int error;
                av_init_packet(&file_combine_22->input_packet);
                file_combine_22->input_packet.data = (uint8_t *)&opus_silence;
                file_combine_22->input_packet.size = sizeof(opus_silence);
		file_combine_22->input_packet.pts = pts_audio_video;
                file_combine_22->input_packet.dts = pts_audio_video;
                file_combine_22->data_present = 0;
                avcodec_decode_audio4(file_combine_22->avctx, file_combine_22->frame,&file_combine_22->data_present, &file_combine_22->input_packet);
            }
            int ret;
            
                if(file_combine_22->data_present) {
                    ret = av_buffersrc_write_frame(buffer_contexts[j], file_combine_22->frame);
                    if (ret < 0) {
                        av_log(NULL, AV_LOG_ERROR, "Error while feeding the audio filtergraph\n");

                    }
                }
            
            av_frame_free(&file_combine_22->frame);
            first->data = pkt->next;
            first = first->next;
            file_combine_22 = file_combine_22->next;
        }
        //int ret;
        //AVFrame *filt_frame = av_frame_alloc();
      //  ret = av_buffersink_get_frame(sink, filt_frame);
        /*
        int qu;
        if(i == 2000) {
            for(qu = 0; qu < 960; qu++) {
                printf("% "SCNu64" ", buffer[qu]);
            }
        }
        int dd;
        for (dd = 0; dd < 960; dd++) {
             sumBuffer[dd] = buffer[dd];
        }*/
        if (!strcasecmp(extension, ".mkv")) {
            opus_int32 length_opus = opus_encode(rtp_encoder, sumBuffer, 960, rtpbuffer, 1500);
            //opus_int32 length_opus = opus_encode_float(rtp_encoder, sumBuffer, 960, rtpbuffer, 1500);
            // encode and
            //write here
            AVPacket packet1;
            av_init_packet(&packet1); 
            packet1.dts = pts_audio_video;
            packet1.pts = pts_audio_video;
            packet1.data = (uint8_t*)rtpbuffer;
            packet1.size = length_opus;
            packet1.stream_index = 0;
            if(fctx) {
                if(av_write_frame(fctx, &packet1) < 0) {
                    //printf("Error writing  to audio file 1 .. of user %lu...\n", node->id);
                    //continue;
                }    
            }
        } else if(!strcasecmp(extension, ".webm")) {
            AVFrame *filt_frame = av_frame_alloc();
            int ret;
 //          while (1) {
                ret = av_buffersink_get_frame(sink, filt_frame);
 /*               if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF){
                    for(int pk = 0 ; pk < 2 ; pk++){
                        if(av_buffersrc_get_nb_failed_requests(buffer_contexts[pk]) > 0){
                            //input_to_read[i] = 1;
                            av_log(NULL, AV_LOG_INFO, "Need to read input %d\n", pk);
                        }
                    }
                    
                    break;
                }
*/
//printf("Error %i \n",filt_frame->pkt_size);
/*
if(i == 2000) {

int m;

for(i=0;)
}*/
AVPacket packet1;
av_init_packet(&packet1);
ret = avcodec_send_frame(file_combine_list_1->oavctx,filt_frame);
if(ret < 0) {
   printf("ufff \n");
}
ret = avcodec_receive_packet(file_combine_list_1->oavctx, &packet1);
if(ret == 0) {
 // printf("sucesss %i \n",packet1.size);
}
//avcodec_encode_audio2(avctx, &packet1, filt_frame, &data_present);
            /*   AVPacket packet1;
            av_init_packet(&packet1);*/
            packet1.dts = pts_audio_video;
            packet1.pts = pts_audio_video;
            
            packet1.stream_index = 0;
            //av_write_uncoded_frame(fctx,0,filt_frame);	
	    if(fctx) {
                if(av_write_frame(fctx, &packet1) < 0) {
                    printf("Error writing  to audio file 1 .. of user \n");
                    //continue;
                }
            }
              
           //     av_frame_unref(filt_frame);
        } else if((!strcasecmp(extension, ".mp4"))) {
 AVFrame *filt_frame = av_frame_alloc();
            int ret;
            while (1) {
                ret = av_buffersink_get_frame(sink, filt_frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF){
                    for(int pk = 0 ; pk < 2 ; pk++){
                        if(av_buffersrc_get_nb_failed_requests(buffer_contexts[pk]) > 0){
                            //input_to_read[i] = 1;
                            av_log(NULL, AV_LOG_INFO, "Need to read input %d\n", pk);
                        }
                    }

                    break;
                }
AVPacket packet1;
av_init_packet(&packet1);
ret = avcodec_send_frame(mavctx,filt_frame);
if(ret < 0) {
   printf("ufff \n");
}
ret = avcodec_receive_packet(mavctx, &packet1);
if(ret == 0) {
  printf("sucesss \n");
}
//avcodec_encode_audio2(avctx, &packet1, filt_frame, &data_present);
            /*   AVPacket packet1;
            av_init_packet(&packet1);*/
            packet1.dts = pts_audio_video;
            packet1.pts = pts_audio_video;

            packet1.stream_index = 0;
            //av_write_uncoded_frame(fctx,0,filt_frame);
            if(fctx) {
                if(av_write_frame(fctx, &packet1) < 0) {
                    printf("Error writing  to audio file 1 .. of user \n");
                    //continue;
                }
            }
               // fwrite(filt_frame->data, 1, 960, recording);
           
            }
             //   av_frame_unref(filt_frame);
           // }


}

        pts_audio_video = pts_audio_video + 20; 
    }
    return 0;
}
int video_mix_process (file_combine_list *file_combine_list_1) {
    if(!file_combine_list_1)
    	return -1;
    
    file_combine *file_combine_1 = file_combine_list_1->head;
    // after decoding check the timestamp
    frame_packet *lowest_tmp;
    frame_packet *highest;
    uint64_t highest_tmp;
    int i;
    int dif;
    for( i = 0; i < file_combine_list_1->size; i++) {
        file_av *file_av_1 = file_combine_1->file_av_list_1->head;
        rt_type *rtp_pkt = file_av_1->video_file->rtp_pkt;
        frame_packet *tmp = rtp_pkt->list;
        rtp_pkt->list_video_pro = NULL;
        rtp_pkt->last_video_pro = NULL;
        while(!tmp == NULL) {
            int bytes = 0, numBytes = 600*400*3;	/* FIXME */
            uint8_t *received_frame = (uint8_t *)g_malloc0(sizeof(uint8_t)*numBytes);
            uint8_t *buffer = (uint8_t *)g_malloc0(sizeof(uint8_t)*10000), *start = buffer;
            int len = 0, frameLen = 0;
            int keyFrame = 0;
            keyFrame = 0;
            frameLen = 0;
            len = 0;
            while(1) {
            	if(tmp->drop) {
                    /* Check if timestamp changes: marker bit is not mandatory, and may be lost as well */
                    if(tmp->next == NULL || tmp->next->ts > tmp->ts)
                    	break;
                    tmp = tmp->next;
                    continue;
                }
		/* RTP payload */
		buffer = start;
		fseek(rtp_pkt->file, tmp->offset+12+tmp->skip, SEEK_SET);
		len = tmp->len-12-tmp->skip;
		bytes = fread(buffer, sizeof(char), len, rtp_pkt->file);
		if(bytes != len)
                    printf("Didn't manage to read all the bytes we needed (%d < %d)...\n", bytes, len);
                /* VP9 depay */
                /*https://tools.ietf.org/html/draft-ietf-payload-vp9-02*/
                /* Read the first octet (VP9 Payload Descriptor) */
                int skipped = 0;
                uint8_t vp9pd = *buffer;
                uint8_t ibit = (vp9pd & 0x80);
		uint8_t pbit = (vp9pd & 0x40);
		uint8_t lbit = (vp9pd & 0x20);
		uint8_t fbit = (vp9pd & 0x10);
		uint8_t vbit = (vp9pd & 0x02);
		/* Move to the next octet and see what's there */
		buffer++;
		len--;
		skipped++;
		if(ibit) {
                    /* Read the PictureID octet */
                    vp9pd = *buffer;
                    uint16_t picid = vp9pd, wholepicid = picid;
                    uint8_t mbit = (vp9pd & 0x80);
                    if(!mbit) {
                    	buffer++;
                    	len--;
                    	skipped++;
                    } else {
                    	memcpy(&picid, buffer, sizeof(uint16_t));
                    	wholepicid = ntohs(picid);
                    	picid = (wholepicid & 0x7FFF);
                    	buffer += 2;
                    	len -= 2;
                    	skipped += 2;
                    }
                }
                if(lbit) {
                    buffer++;
                    len--;
                    skipped++;
                    if(!fbit) {
                    	/* Non-flexible mode, skip TL0PICIDX */
                    	buffer++;
                    	len--;
                    	skipped++;
                    }
		}
		if(fbit && pbit) {
                    /* Skip reference indices */
                    uint8_t nbit = 1;
                    while(nbit) {
			vp9pd = *buffer;
			nbit = (vp9pd & 0x01);
			buffer++;
			len--;
			skipped++;
                    }
		}
		if(vbit) {
                    /* Parse and skip SS */
                    vp9pd = *buffer;
                    int n_s = (vp9pd & 0xE0) >> 5;
                    n_s++;
                    uint8_t ybit = (vp9pd & 0x10);
                    uint8_t gbit = (vp9pd & 0x08);
                    if(ybit) {
			/* Iterate on all spatial layers and get resolution */
			buffer++;
			len--;
			skipped++;
			int i=0;
			for(i=0; i<n_s; i++) {
                            /* Been there, done that: skip skip skip */
                            buffer += 4;
                            len -= 4;
                            skipped += 4;
			}
                        /* Is this the first keyframe we find?
			* (FIXME assuming this really means "keyframe...) */
			printf("keyframe how many \n");
						
                    }
                    if(gbit) {
			if(!ybit) {
                            buffer++;
                            len--;
                            skipped++;
			}
			uint8_t n_g = *buffer;
			buffer++;
			len--;
			skipped++;
			if(n_g > 0) {
                            int i=0;
                            for(i=0; i<n_g; i++) {
                                /* Read the R bits */
				vp9pd = *buffer;
				int r = (vp9pd & 0x0C) >> 2;
				if(r > 0) {
                                    /* Skip reference indices */
                                    buffer += r;
                                    len -= r;
                                    skipped += r;
                                }
				buffer++;
                                len--;
				skipped++;
                            }
			}
                    }
		}
		/* Frame manipulation */
		memcpy(received_frame + frameLen, buffer, len);
		frameLen += len;
		if(len == 0)
                    break;
		/* Check if timestamp changes: marker bit is not mandatory, and may be lost as well */
		if(tmp->next == NULL || tmp->next->ts > tmp->ts)
                    break;
		tmp = tmp->next;
            }
            if(frameLen > 0) {
            	memset(received_frame + frameLen, 0, FF_INPUT_BUFFER_PADDING_SIZE);
                /* Generate frame packet and insert in the ordered list */
		frame_packet *p = (frame_packet *) g_malloc0(sizeof(frame_packet));
		if(p == NULL) {
                    printf("Memory error!\n");
                    return -1;
		}
                p->diff_ntps = tmp->diff_ntps;
                p->ntp_ms = tmp->ntp_ms;
                p->len = frameLen;
                p->seq = tmp->seq;
                p->received_frame = received_frame;
		//last_ts = tmp->rtp_ts;
		p->next = NULL;
		p->prev = NULL;
		if(rtp_pkt->list_video_pro == NULL) {
                    /* First element becomes the list itself (and the last item), at least for now */
                    rtp_pkt->list_video_pro = p;
                    rtp_pkt->last_video_pro = p;
		} else {
                    /* Check where we should insert this, starting from the end */
                    int added = 0;
                    frame_packet *tmp1 = rtp_pkt->last_video_pro;
                    while(tmp1) {
                    	if(tmp1->ntp_ms < p->ntp_ms) {
                            /* The new timestamp is greater than the last one we have, append */
                            added = 1;
                            if(tmp1->next != NULL) {
                            	/* We're inserting */
                            	tmp1->next->prev = p;
                            	p->next = tmp1->next;
                            } else {
                            	/* Update the last packet */
                            	rtp_pkt->last_video_pro = p;
                            }
                            tmp1->next = p;
                            p->prev = tmp1;
                            break;
			} else if(tmp1->ntp_ms == p->ntp_ms) {
                            /* Same timestamp, check the sequence number */
                            if(tmp1->seq < p->seq && (abs(tmp1->seq - p->seq) < 10000)) {
                            	/* The new sequence number is greater than the last one we have, append */
                            	added = 1;
                            	if(tmp1->next != NULL) {
                                    /* We're inserting */
                                    tmp1->next->prev = p;
                                    p->next = tmp1->next;
				} else {
                                    /* Update the last packet */
                                    rtp_pkt->last_video_pro = p;
				}
				tmp1->next = p;
				p->prev = tmp1;
				break;
                            } else if(tmp1->seq > p->seq && (abs(tmp1->seq - p->seq) > 10000)) {
                            	/* The new sequence number (resetted) is greater than the last one we have, append */
                            	added = 1;
                            	if(tmp1->next != NULL) {
                                    /* We're inserting */
                                    tmp1->next->prev = p;
                                    p->next = tmp1->next;
				} else {
                                    /* Update the last packet */
                                    rtp_pkt->last_video_pro = p;
				}
                                tmp1->next = p;
				p->prev = tmp1;
				break;
                            }
			}
			/* If either the timestamp ot the sequence number we just got is smaller, keep going back */
			tmp1 = tmp1->prev;
                    }
                    if(!added) {
                    	/* We reached the start */
                    	p->next = rtp_pkt->list_video_pro;
                    	rtp_pkt->list_video_pro->prev = p;
                    	rtp_pkt->list_video_pro = p;
                    }
		}
		/* Skip data for now */
		
		rtp_pkt->video_count_pro++;
  		if(keyFrame)
                    p->video_key_pro = 1;
            }
            rtp_pkt->size_video_sorted++;
            tmp = tmp->next;
	}
        uint64_t diff = (rtp_pkt->last_video_pro->ntp_ms - rtp_pkt->list_video_pro->ntp_ms) / rtp_pkt->video_count_pro;
        int mod = (rtp_pkt->last_video_pro->ntp_ms - rtp_pkt->list_video_pro->ntp_ms) % rtp_pkt->video_count_pro;
        //printf("difference % "SCNu64"  mod %i \n", diff, mod);
        //printf("processed count %i \n",rtp_pkt->video_count_pro);
        frame_packet *tmp8 = rtp_pkt->list_video_pro;
        //printf("changed ntp_ms % "SCNu64"\n", tmp8->ntp_ms);
        int xw;
        int ppw = 0;
        rtp_pkt->list_video_sorted = (frame_packet_list *) g_malloc0(sizeof(frame_packet_list));
        rtp_pkt->list_video_sorted->size = 0;
        rtp_pkt->list_video_sorted->head = NULL;
        rtp_pkt->list_video_sorted->tail = NULL;
        int er;
        for(er = 0; er < rtp_pkt->size_video_sorted; er++){
            if(tmp8->diff_ntps == 0) {
                tmp8->ntp_changed = rtp_pkt->list_video_pro->ntp_ms;
            } else {
                tmp8->ntp_changed = rtp_pkt->list_video_pro->ntp_ms +tmp8->diff_ntps;
                xw = tmp8->ntp_changed-tmp8->prev->ntp_changed;
                /*if(xw == 33)
                    printf("changed 33 \n");
                else if(xw == 64)
                    printf("changed 64 \n");
                else if(xw == 80)
                    printf("changed 80 \n");
                else if(xw == 48)
                    printf("changed 48 \n");
                 */
                //printf("changed %i \n",xw);
                ppw += xw;
            }
            //if(tmp8->ntp_changed!=tmp8->ntp_ms)
            //    printf("changed ntp_ms % "SCNu64" unchanged % "SCNu64"\n", tmp8->ntp_changed, tmp8->ntp_ms);
            frame_packet *p1 = (frame_packet *) g_malloc0(sizeof(frame_packet));
            p1->ntp_changed = tmp8->ntp_changed;
            int numBytes1 = 600*400*3;	
            p1->received_frame = (uint8_t *)g_malloc0(sizeof(uint8_t)*numBytes1);
            memcpy(p1->received_frame, tmp8->received_frame, tmp8->len);
            p1->len = tmp8->len; 
            if (rtp_pkt->list_video_sorted->tail) {
                p1->prev = rtp_pkt->list_video_sorted->tail;
                p1->next = rtp_pkt->list_video_sorted->tail->next;
                rtp_pkt->list_video_sorted->tail->next->prev = p1;
                rtp_pkt->list_video_sorted->tail->next = p1;
            } else {
                p1->next = p1;
                p1->prev = p1;
                rtp_pkt->list_video_sorted->head = p1;
            }
            rtp_pkt->list_video_sorted->tail = p1;
            rtp_pkt->list_video_sorted->size++;
            tmp8 = tmp8->next;
        }
        //printf("mmmm %i \n",ppw);
        file_combine_1 = file_combine_1->next;
    }
    
    frame_packet packet_sort[file_combine_list_1->size];

    for( i = 0; i < file_combine_list_1->size; i++) {
        file_av *file_av_1 = file_combine_1->file_av_list_1->head;
        rt_type *rtp_pkt = file_av_1->video_file->rtp_pkt;
        //printf("Counter % "SCNu64"\n", rtp_pkt->count);
        if(i == 0) {
            lowest_tmp = rtp_pkt->list_video_pro;
            lowest_tmp->dif = 0;
            printf("lowest first ts % "SCNu64"\n", lowest_tmp->ntp_changed);
        } else {
            if(lowest_tmp->ntp_changed > rtp_pkt->list_video_pro->ntp_changed) { 
                lowest_tmp = rtp_pkt->list_video_pro;
                printf("lowest second ts % "SCNu64"\n", lowest_tmp->ntp_changed);
            } 
        }
        uint64_t diff = (rtp_pkt->last_video_pro->ntp_changed - rtp_pkt->list_video_pro->ntp_changed) / rtp_pkt->video_count_pro;
        int mod = (rtp_pkt->last_video_pro->ntp_changed - rtp_pkt->list_video_pro->ntp_changed) % rtp_pkt->video_count_pro;
        //printf("difference % "SCNu64"  mod %i \n", diff, mod);
        packet_sort[i] = *rtp_pkt->list_video_pro;
        packet_sort[i].counter = rtp_pkt->video_count_pro;
        packet_sort[i].last_tmp = rtp_pkt->last_video_pro->ntp_ms;
        packet_sort[i].last_pp = rtp_pkt->last_video_pro;
        packet_sort[i].file1 = rtp_pkt->file;
        frame_packet *sq = rtp_pkt->list_video_pro;
    /*    while (sq != NULL) {
            printf("ts % "SCNu64"  \n", sq->ntp_ms);
            sq = sq->next;
        }
    */    file_combine_1 = file_combine_1->next;
    }

        
    GQueue *inbuf = g_queue_new ();
    g_queue_init (inbuf);
    for (i = 0; i < file_combine_list_1->size; i++) {
        if(i == 0) {
            g_queue_push_tail(inbuf, &packet_sort[i]);
        } else {
             g_queue_insert_sorted(inbuf,&packet_sort[i],rtp_sort_video, NULL);
        }
    }
    guint length = inbuf->length;
    printf("length % "SCNu64"\n", length);
    GList *first1 = inbuf->head;
    int j;
    inbuf->head->prev = inbuf->tail;
    inbuf->tail->next = inbuf->head;
    for (j = 0; j < (length*2); j++) {
        frame_packet *pkt_tail = (frame_packet *)first1->data;
        printf("ntp % "SCNu64"\n", pkt_tail->ntp_changed);
        first1 = first1->next;
    }    

    GList *last;
    GList *first = inbuf->head;
    for (i = 0; i < 1; i++) {
        frame_packet *pkt_head = (frame_packet *)first->data;
        frame_packet *pkt_v = (frame_packet *)first->data;
        frame_packet_list *frame_packet_list_4;
        last = first->next;
        int uy;
        frame_packet *first_hk = (frame_packet *)first->data;
        for (uy = 0; uy < file_combine_list_1->size; uy++) {
            file_av *file_av_1 = file_combine_1->file_av_list_1->head;
            rt_type *rtp_pkt = file_av_1->video_file->rtp_pkt;
            if(first_hk->ntp_changed == rtp_pkt->list_video_sorted->head->ntp_changed){
                frame_packet_list_4 = rtp_pkt->list_video_sorted;
                break;
            }
            file_combine_1 = file_combine_1->next;
        }
        int k;
        for (k = 0; k < (length-1); k++) { 
            frame_packet_list *frame_packet_list_2 = (frame_packet_list *) g_malloc0(sizeof(frame_packet_list));
            frame_packet_list_2->head = NULL;
            frame_packet_list_2->size = 0;
            frame_packet_list_2->tail =NULL;
            frame_packet *pkt_tail = (frame_packet *)last->data;
            frame_packet *pkt_il = (frame_packet *)last->data;
            frame_packet *hjk = (frame_packet *)last->data;
            frame_packet_list *frame_packet_list_3;
            for (uy = 0; uy < file_combine_list_1->size; uy++) {
                file_av *file_av_1 = file_combine_1->file_av_list_1->head;
                rt_type *rtp_pkt = file_av_1->video_file->rtp_pkt;
                if(hjk->ntp_changed == rtp_pkt->list_video_sorted->head->ntp_changed){
                    frame_packet_list_3 = rtp_pkt->list_video_sorted;
                    break;
                }
                file_combine_1 = file_combine_1->next;
            }
            uint64_t x1 = pkt_tail->ntp_changed;
            uint8_t *bufy =  pkt_tail->received_frame;
            int len1 = pkt_tail->len;
            while(x1 > pkt_head->ntp_changed) {
                frame_packet *pk1 = (frame_packet *) g_malloc0(sizeof(frame_packet));
                pk1->ntp_changed = pkt_head->ntp_changed;
                int bytes = 0, numBytes = 600*400*3;	
                pk1->received_frame = (uint8_t *)g_malloc0(sizeof(uint8_t)*numBytes);
                memcpy(pk1->received_frame, bufy, len1);
                pk1->len = len1; 
                if (frame_packet_list_2->tail) {
                    pk1->prev = frame_packet_list_2->tail;
                    pk1->next = frame_packet_list_2->tail->next;
                    frame_packet_list_2->tail->next->prev = pk1;
                    frame_packet_list_2->tail->next = pk1;
                } else {
                    pk1->next = pk1;
                    pk1->prev = pk1;
                    frame_packet_list_2->head = pk1;
                }
                frame_packet_list_2->tail = pk1;
                frame_packet_list_2->size++;
                pkt_head = pkt_head->next;
                if(x1 < pkt_head->ntp_changed) {
                    int yuo = frame_packet_list_2->size;
                    int hl;
                    for(hl = 0; hl < yuo; hl++) {
                        frame_packet *py1 = (frame_packet *) g_malloc0(sizeof(frame_packet));
                        py1->ntp_changed = frame_packet_list_2->tail->ntp_changed;
                        py1->received_frame = (uint8_t *)g_malloc0(sizeof(uint8_t)*numBytes);
                        memcpy(py1->received_frame, frame_packet_list_2->tail->received_frame, frame_packet_list_2->tail->len);
                        py1->len = frame_packet_list_2->tail->len; 
                        if (frame_packet_list_3->head) {
                            py1->next = frame_packet_list_3->head;
                            py1->prev = frame_packet_list_3->head->prev;
                            frame_packet_list_3->head->prev->next = py1;
                            frame_packet_list_3->head->prev = py1;
                        } else {
                            py1->next = py1;
                            py1->prev = py1;
                            frame_packet_list_3->tail = py1;
                        }
                        frame_packet_list_3->head = py1;
                        frame_packet_list_3->size++;
                        frame_packet_list_2->tail = frame_packet_list_2->tail->prev;
                    }
                }
            }
            frame_packet *pkt_tail_mod = frame_packet_list_3->head;
            while(pkt_tail->ntp_changed != pkt_tail_mod->ntp_changed){
                pkt_tail_mod  = pkt_tail_mod->next;
            }
            frame_packet *pkt_head_mod = frame_packet_list_4->head;
            while(pkt_head->ntp_changed != pkt_head_mod->ntp_changed){
                pkt_head_mod  = pkt_head_mod->next;
            }
            frame_packet *pkt_tail_last, *pkt_tail_last_mod, *pkt_head_last, *pkt_head_last_mod;             
            while(pkt_tail != NULL && pkt_head != NULL) {
                if(pkt_tail->ntp_changed == pkt_head->ntp_changed) {
                    
                    if(pkt_tail->next == NULL && pkt_head->next == NULL)
                        continue;
                    if(pkt_tail->next == NULL){
                        pkt_tail_last = pkt_tail;
                        pkt_tail_last_mod = pkt_tail_mod;
                    }
                    if(pkt_head->next == NULL){
                        pkt_head_last = pkt_head;
                        pkt_head_last_mod = pkt_head_mod;
                    }
                    pkt_tail_mod = pkt_tail_mod->next;
                    pkt_head_mod = pkt_head_mod->next;        
                    pkt_tail = pkt_tail->next;
                    pkt_head = pkt_head->next;
                    continue;
                } else if (pkt_tail->ntp_changed < pkt_head->ntp_changed) {
                    frame_packet *pk1 = (frame_packet *) g_malloc0(sizeof(frame_packet));
                    pk1->ntp_changed = pkt_tail->ntp_changed;
                    int bytes = 0, numBytes = 600*400*3;	
                    uint8_t *bufy =  pkt_head->received_frame;
                    pk1->len = pkt_head->len;
                    pk1->received_frame = (uint8_t *)g_malloc0(sizeof(uint8_t)*numBytes);
                    memcpy(pk1->received_frame, bufy, pk1->len);
                    pk1->next = pkt_head;
                    pkt_head->prev->next = pk1;
                    pk1->prev =  pkt_head->prev;
                    pkt_head->prev = pk1;
                    frame_packet *pt1 = (frame_packet *) g_malloc0(sizeof(frame_packet));
                    pt1->ntp_changed = pkt_tail->ntp_changed;
                    pt1->len = pkt_head->len;
                    pt1->received_frame = (uint8_t *)g_malloc0(sizeof(uint8_t)*numBytes);
                    memcpy(pt1->received_frame, pkt_head->received_frame, pt1->len);
                    pt1->next = pkt_head_mod;
                    pkt_head_mod->prev->next = pt1;
                    pt1->prev =  pkt_head_mod->prev;
                    pkt_head_mod->prev = pt1;
                    frame_packet_list_4->size++;
                    if(pkt_tail->next == NULL){
                        pkt_tail_last = pkt_tail;
                        pkt_tail_last_mod = pkt_tail_mod;
                    }
                    pkt_tail_mod = pkt_tail_mod->next;
                    pkt_tail = pkt_tail->next;
                    continue;
                } else if (pkt_head->ntp_changed < pkt_tail->ntp_changed) {
                    frame_packet *pk1 = (frame_packet *) g_malloc0(sizeof(frame_packet));
                    pk1->ntp_changed = pkt_head->ntp_changed;
                    int bytes = 0, numBytes = 600*400*3;	
                    uint8_t *bufy =  pkt_tail->received_frame;
                    pk1->len = pkt_tail->len;
                    pk1->received_frame = (uint8_t *)g_malloc0(sizeof(uint8_t)*numBytes);
                    memcpy(pk1->received_frame, bufy, pk1->len);
                    pk1->next = pkt_tail;
                    pk1->prev =  pkt_tail->prev;
                    pkt_tail->prev->next = pk1;
                    pkt_tail->prev = pk1;
                    frame_packet *pt1 = (frame_packet *) g_malloc0(sizeof(frame_packet));
                    pt1->ntp_changed = pkt_head_mod->ntp_changed;
                    pt1->len = pkt_tail_mod->len;
                    pt1->received_frame = (uint8_t *)g_malloc0(sizeof(uint8_t)*numBytes);
                    memcpy(pt1->received_frame, pkt_tail_mod->received_frame, pt1->len);
                    pt1->next = pkt_tail_mod;
                    pt1->prev =  pkt_tail_mod->prev;
                    pkt_tail_mod->prev->next = pt1;
                    pkt_tail_mod->prev = pt1;
                    if(pkt_head->next == NULL){
                        pkt_head_last = pkt_head;
                        pkt_head_last_mod = pkt_head_mod;
                    }
                    frame_packet_list_3->size++;
                    pkt_head_mod = pkt_head_mod->next;
                    pkt_head = pkt_head->next;
                    continue;
                }
            }
            if(pkt_tail != NULL && pkt_head == NULL) {
                 while(pkt_tail != NULL){
                    frame_packet *pk1 = (frame_packet *) g_malloc0(sizeof(frame_packet));
                    pk1->ntp_changed = pkt_tail->ntp_changed;
                    int bytes = 0, numBytes = 600*400*3;	
                    uint8_t *bufy =  pkt_head_last->received_frame;
                    pk1->len = pkt_head_last->len;
                    pk1->received_frame = (uint8_t *)g_malloc0(sizeof(uint8_t)*numBytes);
                    memcpy(pk1->received_frame, bufy, pk1->len);
                    pk1->next = NULL;
                    pk1->prev =  pkt_head_last;
                    pkt_head_last->next = pk1;
                    pkt_head_last = pkt_head_last->next;
                    frame_packet *pb1 = (frame_packet *) g_malloc0(sizeof(frame_packet));
                    pb1->ntp_changed = pkt_tail->ntp_changed;
                    pb1->len = pkt_head_last->len;
                    pb1->received_frame = (uint8_t *)g_malloc0(sizeof(uint8_t)*numBytes);
                    memcpy(pb1->received_frame,  pkt_head_last->received_frame, pb1->len);
                    pb1->next = pkt_head_last_mod->next;
                    pb1->prev =  pkt_head_last_mod;
                    pkt_head_last_mod->next = pb1;
                    pkt_head_last_mod = pkt_head_last_mod->next;
                    if(pkt_tail->next == NULL) {
                        //tail = pkt_tail;
                        pkt_v->last_pp = pkt_head_last;
                        first->data = pkt_v;
                    }    
                    frame_packet_list_4->size++;
                    pkt_tail_mod = pkt_tail_mod->next;
                    pkt_tail = pkt_tail->next;
                }
            } else if (pkt_head != NULL && pkt_tail == NULL) {
                while(pkt_head != NULL){
                    frame_packet *pk1 = (frame_packet *) g_malloc0(sizeof(frame_packet));
                    pk1->ntp_changed = pkt_head->ntp_changed;
                    int bytes = 0, numBytes = 600*400*3;	
                    uint8_t *bufy =  pkt_head_last->received_frame;
                    pk1->len = pkt_head_last->len;
                    pk1->received_frame = (uint8_t *)g_malloc0(sizeof(uint8_t)*numBytes);
                    memcpy(pk1->received_frame, bufy, pk1->len);
                    pk1->next = NULL;
                    pk1->prev =  pkt_tail_last;
                    pkt_tail_last->next = pk1;
                    pkt_tail_last = pkt_tail_last->next;
                    if(pkt_head->next == NULL){
                        //tail = pkt_tail;
                        frame_packet *la = (frame_packet *)last->data; 
                        la->last_pp = pkt_tail_last;
                        last->data = la;
                        
                    }
                    frame_packet *pu1 = (frame_packet *) g_malloc0(sizeof(frame_packet));
                    pu1->ntp_changed = pkt_head->ntp_changed;
                    pu1->len = pkt_head_last->len;
                    pu1->received_frame = (uint8_t *)g_malloc0(sizeof(uint8_t)*numBytes);
                    memcpy(pu1->received_frame, pkt_head_last->received_frame, pu1->len);
                    pu1->next = pkt_tail_last_mod->next;
                    pu1->prev =  pkt_tail_last_mod;
                    pkt_tail_last_mod->next = pu1;
                    pkt_tail_last_mod = pkt_tail_last_mod->next;
                    frame_packet_list_3->size++;
                    pkt_head_mod = pkt_head_mod->next;
                    pkt_head = pkt_head->next;
                }
            }
          /*  int mb;
            frame_packet *py2 = frame_packet_list_3->head;
            for(mb = 0; mb < frame_packet_list_3->size; mb++ ) {
                printf("ffftut e %"SCNu64"| ", py2->ntp_changed);
                py2 = py2->next;
            }*/
            last = last->next;
        }
        first = first->next;
    }
    file_combine_list_1->o_codec = avcodec_find_encoder_by_name("nvenc_h264");
    file_combine_list_1->oavctx = avcodec_alloc_context3(file_combine_list_1->o_codec);
    if (!file_combine_list_1->oavctx) {
        printf("Could not alloc an encoding context\n");
        return -1;
    }
    file_combine_list_1->oavctx->codec_type = AVMEDIA_TYPE_VIDEO;
    file_combine_list_1->oavctx->codec_id = AV_CODEC_ID_H264;
    file_combine_list_1->oavctx->pix_fmt = AV_PIX_FMT_YUV420P;
    file_combine_list_1->oavctx->width = 400;//max_width;
    file_combine_list_1->oavctx->height = 300; //max_height;
    file_combine_list_1->oavctx->time_base = (AVRational){1, 30};
    avcodec_open2(file_combine_list_1->oavctx, file_combine_list_1->o_codec, NULL);
    file_av *file_av_13 = file_combine_1->file_av_list_1->head;
    rt_type *rtp_pkt3 = file_av_13->video_file->rtp_pkt;
    frame_packet *hhw = rtp_pkt3->list_video_sorted->head;
    file_combine_1 = file_combine_1->next;
    file_av *file_av_14 = file_combine_1->file_av_list_1->head;
    rt_type *rtp_pkt34 = file_av_14->video_file->rtp_pkt;
    frame_packet *hu = rtp_pkt34->list_video_sorted->head;
    for (i = 0; i <  rtp_pkt3->list_video_sorted->size; i++) {
        if(hhw->ntp_changed != hu->ntp_changed)
            printf("ggggffftut e %"SCNu64"| ", hhw->ntp_changed);
        if(rtp_pkt3->list_video_sorted->size == i+1)
            printf(" kkk %"SCNu64" kkkk %"SCNu64" \n", hu->ntp_changed, hhw->ntp_changed);
        hu = hu->next;
        hhw = hhw->next;
    }
    printf("frames %i -- frames %i \n",rtp_pkt3->list_video_sorted->size,rtp_pkt34->list_video_sorted->size);
    printf(" tail %"SCNu64" tail1 %"SCNu64" \n", rtp_pkt3->list_video_sorted->tail->ntp_changed, rtp_pkt34->list_video_sorted->tail->ntp_changed);
    printf(" tail %"SCNu64" tail1 %"SCNu64" \n", rtp_pkt3->list_video_sorted->head->ntp_changed, rtp_pkt34->list_video_sorted->head->ntp_changed);
    for( i = 0; i < file_combine_list_1->size; i++) {
        file_av *file_av_1 = file_combine_1->file_av_list_1->head;
        rt_type *rtp_pkt = file_av_1->video_file->rtp_pkt;
        file_combine_1->input_codec_video = avcodec_find_decoder_by_name("libvpx-vp9");
        file_combine_1->avctx_video = avcodec_alloc_context3(file_combine_1->input_codec_video);
        if (!file_combine_1->avctx_video) {
            printf("Could not alloc an encoding context\n");
            return -1;
        }
        file_combine_1->avctx_video->codec_type = AVMEDIA_TYPE_VIDEO;
        file_combine_1->avctx_video->codec_id = AV_CODEC_ID_VP9;
        file_combine_1->avctx_video->pix_fmt = AV_PIX_FMT_YUV420P;
        file_combine_1->avctx_video->width = rtp_pkt->max_width;
        file_combine_1->avctx_video->height = rtp_pkt->max_height;
        avcodec_open2(file_combine_1->avctx_video, file_combine_1->input_codec_video, NULL);
        file_combine_1->img_convert_ctx = NULL;
        if(file_combine_1->img_convert_ctx == NULL){
                    file_combine_1->img_convert_ctx = sws_getContext(file_combine_1->avctx_video->width, file_combine_1->avctx_video->height,
                        file_combine_1->avctx_video->pix_fmt, file_combine_1->avctx_video->width, file_combine_1->avctx_video->height,
                        AV_PIX_FMT_BGR24, SWS_BICUBIC, NULL, NULL, NULL);
        }
        file_combine_1 = file_combine_1->next;
    }
    int ret;
    for (i = 0; i <  902; i++) {
        int yo;
        Mat imageFrame[file_combine_list_1->size];
        cuda::GpuMat d_src[file_combine_list_1->size];
        cuda::GpuMat d_dst[file_combine_list_1->size];
        Mat image_re(300,400, CV_8UC3);
        AVFrame *pFrameRGB_one = NULL;
        pFrameRGB_one = av_frame_alloc();
        cuda::GpuMat d_src_image(image_re);
        int got_frame;
        uint64_t pts = 0;
        uint64_t sdm;
        AVFrame *frame = NULL;
        for( yo = 0; yo < file_combine_list_1->size; yo++) {
            file_av *file_av_1 = file_combine_1->file_av_list_1->head;
            rt_type *rtp_pkt = file_av_1->video_file->rtp_pkt;
            frame_packet *test1 = rtp_pkt->list_video_sorted->head;
            if(yo == 0) {
                if(i == 0) {
                    pts = rtp_pkt->list_video_sorted->head->ntp_changed;
                }
                sdm = rtp_pkt->list_video_sorted->head->ntp_changed - pts;
            }
            AVPacket enc_pkt;
            enc_pkt.data = NULL;
            enc_pkt.size = 0;
            av_init_packet(&enc_pkt);
            enc_pkt.data = test1->received_frame;
            enc_pkt.size = test1->len;
            frame = av_frame_alloc();
            if (!frame) {
                ret = AVERROR(ENOMEM);
                printf("Could not alloc an encoding context\n");
                break;
            }
            ret = avcodec_decode_video2(file_combine_1->avctx_video, frame, &got_frame, &enc_pkt);
            if(ret < 0)
                printf("decoding\n");
            AVFrame *pFrameRGB = NULL;
            pFrameRGB = av_frame_alloc();
            int size = avpicture_get_size(AV_PIX_FMT_BGR24, file_combine_1->avctx_video->width, file_combine_1->avctx_video->height);
            uint8_t  *out_bufferRGB = new uint8_t[size];
            avpicture_fill((AVPicture *)pFrameRGB, out_bufferRGB, AV_PIX_FMT_BGR24, file_combine_1->avctx_video->width, file_combine_1->avctx_video->height);
            sws_scale(file_combine_1->img_convert_ctx, frame->data, frame->linesize, 0, file_combine_1->avctx_video->height, pFrameRGB->data, pFrameRGB->linesize);
            imageFrame[yo] = Mat(file_combine_1->avctx_video->height, file_combine_1->avctx_video->width, CV_8UC3);
            memcpy(imageFrame[yo].data, out_bufferRGB, size);
            delete[] out_bufferRGB;
            d_src[yo] = cuda::GpuMat(imageFrame[yo]);
            cuda::resize(d_src[yo], d_dst[yo], Size(200,300), 0, 0, INTER_CUBIC);
            d_dst[yo].copyTo(d_src_image(cv::Rect((yo*400)/2,0,d_dst[yo].cols, d_dst[yo].rows)));
            rtp_pkt->list_video_sorted->head = rtp_pkt->list_video_sorted->head->next;
            file_combine_1 = file_combine_1->next;
        }
        d_src_image.download(image_re);
        avpicture_fill((AVPicture *)pFrameRGB_one, image_re.data,AV_PIX_FMT_BGR24, file_combine_list_1->oavctx->width, file_combine_list_1->oavctx->height);
        file_combine_list_1->convert_ctx = NULL;
        if(file_combine_list_1->convert_ctx == NULL){
            file_combine_list_1->convert_ctx = sws_getContext(file_combine_list_1->oavctx->width, file_combine_list_1->oavctx->height,
                                                     AV_PIX_FMT_BGR24, file_combine_list_1->oavctx->width, file_combine_list_1->oavctx->height,
                                                     file_combine_list_1->oavctx->pix_fmt, SWS_BICUBIC, NULL, NULL, NULL);
        }
        AVFrame *srcFrame = av_frame_alloc();
        int sizee = avpicture_get_size(file_combine_list_1->oavctx->pix_fmt, file_combine_list_1->oavctx->width,file_combine_list_1->oavctx->height);
        uint8_t  *out_buffer = new uint8_t[sizee];
        avpicture_fill((AVPicture *)srcFrame, out_buffer, file_combine_list_1->oavctx->pix_fmt, file_combine_list_1->oavctx->width, file_combine_list_1->oavctx->height);
        sws_scale(file_combine_list_1->convert_ctx, pFrameRGB_one->data, pFrameRGB_one->linesize, 0, file_combine_list_1->oavctx->height, srcFrame->data, srcFrame->linesize);
        delete[] out_buffer;
        av_free(pFrameRGB_one);
        srcFrame->width = file_combine_list_1->oavctx->width;
        srcFrame->height = file_combine_list_1->oavctx->height;
        srcFrame->format = frame->format;
        //av_frame_copy_props(srcFrame, frame);
        //AVFrame *convertFrame = useOpenCVProcessFrame(frame,stream_index);
        srcFrame->pts = sdm;
        srcFrame->pict_type =  AV_PICTURE_TYPE_NONE;
        AVPacket encoding_pkt;
        encoding_pkt.data = NULL;
        encoding_pkt.size = 0;
        av_init_packet(&encoding_pkt);
        ret = avcodec_encode_video2(file_combine_list_1->oavctx, &encoding_pkt, srcFrame, &got_frame);
        if(ret < 0) {
            printf("encoding error \n");
        }
        encoding_pkt.pts = sdm;
        encoding_pkt.dts = sdm;
        encoding_pkt.stream_index = 0;
        if (got_frame) {
            //av_write_uncoded_frame(fctx,0,filt_frame);	
            if(fctx) {
               if(av_write_frame(fctx, &encoding_pkt) < 0) {
                    printf("Error writing  to audio file 1 .. of user \n");
                    //continue;
                }
            }
        }
    }
    
}
/*

int webm_process(file_combine_list *file_combine_list_1) {
    if(!file_combine_list_1)
    	return -1;
    file_combine *file_combine_1 = file_combine_list_1->head;
    int aud = 1; 
    int vid = 1;
    frame_packet *tmp1;
    frame_packet *list1;
    frame_packet *tmp2;
    frame_packet *list2;
    FILE *file1;
    FILE *file2;
    int we;
    //frame_packet *tmp1 = file_av_1->list;
    for (we = 0;  we < file_combine_list_1->size; we++ ) {
        file_av *file_av_1 = file_combine_1->file_av_list_1->head;
        if(file_av_1->opus) {
            tmp1 = file_av_1->list;
            file1 = file_av_1->file;
            list1 = file_av_1->list;
            printf("sqqq% "SCNu64"\n",tmp1->ts);
        } else if(file_av_1->vp9) {
            tmp2 = file_av_1->list;
            file2 = file_av_1->file;
            list2 = file_av_1->list;
            printf("vp9% "SCNu64"\n",tmp2->ts);
        }
        file_combine_1 = file_combine_1->next;
    }
    printf("1hh23 sqqq% "SCNu64"\n",tmp1->ts);
    uint64_t first_ntp;
    uint64_t pts_audio_video = 0; 
    if(tmp1->ntp_ms > tmp2->ntp_ms) {
        first_ntp = tmp2->ntp_ms;
    } else if (tmp1->ntp_ms < tmp2->ntp_ms) {
        first_ntp = tmp1->ntp_ms;
    } else {
        first_ntp = tmp1->ntp_ms;
    }
    int bytes = 0, numBytes = max_width*max_height*3;	
    uint8_t *received_frame = (uint8_t *) g_malloc0((sizeof(uint8_t))*numBytes);
    uint8_t *buffer = (uint8_t * ) g_malloc0((sizeof(uint8_t))*10000), *start = buffer;
    int len = 0, frameLen = 0;
    int audio_len = 0;
    int keyFrame = 0;
    uint32_t keyframe_ts = 0;
    int64_t audio_ts = 0;
    int audio_pts = 0;
    int video_pts = 0;
    int audio = 0;
    int video = 0;
    gchar *buf;
    while(tmp1 != NULL || tmp2 != NULL) {
        if (aud == 1 || vid == 1) {
            if(tmp2 != NULL && vid == 1) {
                len = 0;
                keyFrame = 0;
                frameLen = 0;
                while(1) {
                    if(tmp2->drop) {
                        // Check if timestamp changes: marker bit is not mandatory, and may be lost as well 
                        if(tmp2->next == NULL || tmp2->next->ntp_ms > tmp2->ntp_ms)
                            break;
                        tmp2 = tmp2->next;
                        continue;
                    }
                    // RTP payload 
                    buffer = start;
                    fseek(file2, tmp2->offset+12+tmp2->skip, SEEK_SET);
                    len = tmp2->len-12-tmp2->skip;
                    bytes = fread(buffer, sizeof(char), len, file2);
                    if(bytes != len)
                        printf("Didn't manage to read all the bytes we needed (%d < %d)...\n", bytes, len);
                    // VP9 depay 
                    // https://tools.ietf.org/html/draft-ietf-payload-vp9-02 
                    // Read the first octet (VP9 Payload Descriptor) 
                    int skipped = 0;
                    uint8_t vp9pd = *buffer;
                    uint8_t ibit = (vp9pd & 0x80);
                    uint8_t pbit = (vp9pd & 0x40);
                    uint8_t lbit = (vp9pd & 0x20);
                    uint8_t fbit = (vp9pd & 0x10);
                    uint8_t vbit = (vp9pd & 0x02);
                    // Move to the next octet and see what's there 
                    buffer++;
                    len--;
                    skipped++;
                    if(ibit) {
                        // Read the PictureID octet 
                        vp9pd = *buffer;
                        uint16_t picid = vp9pd, wholepicid = picid;
                        uint8_t mbit = (vp9pd & 0x80);
                        if(!mbit) {
                            buffer++;
                            len--;
                            skipped++;
                        } else {
                            memcpy(&picid, buffer, sizeof(uint16_t));
                            wholepicid = ntohs(picid);
                            picid = (wholepicid & 0x7FFF);
                            buffer += 2;
                            len -= 2;
                            skipped += 2;
                        }
                    }
                    if(lbit) {
                        buffer++;
                        len--;
                        skipped++;
                        if(!fbit) {
                            // Non-flexible mode, skip TL0PICIDX 
                            buffer++;
                            len--;
                            skipped++;
                        }
                    }
                    if(fbit && pbit) {
                        // Skip reference indices 
                        uint8_t nbit = 1;
                        while(nbit) {
                            vp9pd = *buffer;
                            nbit = (vp9pd & 0x01);
                            buffer++;
                            len--;
                            skipped++;
                        }
                    }
                    if(vbit) {
                        // Parse and skip SS 
                        vp9pd = *buffer;
                        int n_s = (vp9pd & 0xE0) >> 5;
                        n_s++;
                        uint8_t ybit = (vp9pd & 0x10);
                        uint8_t gbit = (vp9pd & 0x08);
                        if(ybit) {
                            // Iterate on all spatial layers and get resolution 
                            buffer++;
                            len--;
                            skipped++;
                            int i=0;
                            for(i=0; i<n_s; i++) {
                                // Been there, done that: skip skip skip 
                                buffer += 4;
                                len -= 4;
                                skipped += 4;
                            }
                            // Is this the first keyframe we find?
                            // (FIXME assuming this really means "keyframe...) 
                            if(keyframe_ts == 0) {
                                keyframe_ts = tmp2->ntp_ms;
                                printf("First keyframe: %"SCNu64"\n", tmp2->ntp_ms-list2->ntp_ms);
                            }
                            keyframe_ts = tmp2->ntp_ms;
                        }
                        if(gbit) {
                            if(!ybit) {
                                buffer++;
                                len--;
                                skipped++;
                            }
                            uint8_t n_g = *buffer;
                            buffer++;
                            len--;
                            skipped++;
                            if(n_g > 0) {
                                int i=0;
                                for(i=0; i<n_g; i++) {
                                    // Read the R bits
                                    vp9pd = *buffer;
                                    int r = (vp9pd & 0x0C) >> 2;
                                    if(r > 0) {
                                        // Skip reference indices 
                                        buffer += r;
                                        len -= r;
                                        skipped += r;
                                    }
                                    buffer++;
                                    len--;
                                    skipped++;
                                }
                            }
                        }
                    }
                    // Frame manipulation 
                    memcpy(received_frame + frameLen, buffer, len);
                    frameLen += len;
                    if(len == 0)
                        break;
                    //printf("seq: %"SCNu32"\n",tmp2->seq);
                    // Check if timestamp changes: marker bit is not mandatory, and may be lost as well 
                    if(tmp2->next == NULL || tmp2->next->ntp_ms > tmp2->ntp_ms)
                        break;
                    tmp2 = tmp2->next;
                } 
            }
            if(tmp1 != NULL && aud == 1) {                      
                audio_len = 0;
                buf = (gchar *)g_malloc0((sizeof(gchar))*1000);
                if(tmp1->drop) {
                    // Check if timestamp changes: marker bit is not mandatory, and may be lost as well 
                    if(tmp1->next == NULL || tmp1->next->ntp_ms > tmp1->ntp_ms)
                        break;
                    tmp1 = tmp1->next;
                    continue;
                }
                fseek(file1, tmp1->offset+12+tmp1->skip, SEEK_SET);
                audio_len = tmp1->len-12-tmp1->skip;
                bytes = fread(buf, sizeof(char), audio_len, file1);
            }
        }
        if(tmp1 != NULL && tmp2 != NULL) {
            if(tmp1->ntp_ms > tmp2->ntp_ms) {
                pts_audio_video = pts_audio_video + (tmp2->ntp_ms - first_ntp);
                memset(received_frame + frameLen, 0, FF_INPUT_BUFFER_PADDING_SIZE);
                AVPacket packet;
                av_init_packet(&packet); 
                packet.stream_index = 0;
                packet.data = received_frame;
                packet.size = frameLen;
                if(keyFrame)
                    packet.flags |= AV_PKT_FLAG_KEY;
                packet.dts = pts_audio_video;
                packet.pts = pts_audio_video;
                if(fctx) {
                    if(av_write_frame(fctx, &packet) < 0) {
                        printf("Error writing video frame to file...\n");
                    }
                }
                first_ntp = tmp2->ntp_ms;
                tmp2 = tmp2->next;
                vid = 1;
                aud = 0;
            } else if (tmp1->ntp_ms < tmp2->ntp_ms) {
                pts_audio_video = pts_audio_video + (tmp1->ntp_ms - first_ntp);
                AVPacket packet1;
                av_init_packet(&packet1); 
                packet1.dts = pts_audio_video;
                packet1.pts = pts_audio_video;
                packet1.data = (uint8_t*)buf;
                packet1.size = audio_len;
                packet1.stream_index = 1;
                if(fctx) {
                    if(av_write_frame(fctx, &packet1) < 0) {
                        //printf("Error writing  to audio file 1 .. of user %lu...\n", node->id);
                         g_free(buf);
                        //continue;
                    }    
                }
                first_ntp = tmp1->ntp_ms;
                tmp1 = tmp1->next;
                vid = 0;
                aud = 1;
            } else {
                pts_audio_video = pts_audio_video + (tmp1->ntp_ms - first_ntp);
                memset(received_frame + frameLen, 0, FF_INPUT_BUFFER_PADDING_SIZE);
                AVPacket packet;
                av_init_packet(&packet); 
                packet.stream_index = 0;
                packet.data = received_frame;
                packet.size = frameLen;
                if(keyFrame)
                    packet.flags |= AV_PKT_FLAG_KEY;
                packet.dts = pts_audio_video;
                packet.pts = pts_audio_video;
                if(fctx) {
                    if(av_write_frame(fctx, &packet) < 0) {
                        printf("Error writing video frame to file...\n");
                    }
                }
                AVPacket packet1;
                av_init_packet(&packet1); 
                packet1.dts = pts_audio_video;
                packet1.pts = pts_audio_video;
                packet1.data = (uint8_t*)buf;
                packet1.size = audio_len;
                packet1.stream_index = 1;
                if(fctx) {
                    if(av_write_frame(fctx, &packet1) < 0) {
                        //printf("Error writing  to audio file 1 .. of user %lu...\n", node->id);
                        g_free(buf);
                        //continue;
                    }    
                }
                first_ntp = tmp1->ntp_ms;
                tmp1 = tmp1->next;
                tmp2 = tmp2->next;
                vid = 1;
                aud = 1;
            }
        } else if(tmp1 != NULL && tmp2 == NULL) {
            if(tmp1->ntp_ms > first_ntp || tmp1->ntp_ms == first_ntp) {
                pts_audio_video = pts_audio_video + (tmp1->ntp_ms - first_ntp);
                AVPacket packet1;
                av_init_packet(&packet1); 
                packet1.dts = pts_audio_video;
                packet1.pts = pts_audio_video;
                packet1.data = (uint8_t*)buf;
                packet1.size = audio_len;
                packet1.stream_index = 1;
                if(fctx) {
                    if(av_write_frame(fctx, &packet1) < 0) {
                        //printf("Error writing  to audio file 1 .. of user %lu...\n", node->id);
                         g_free(buf);
                        //continue;
                    }    
                }
                first_ntp = tmp1->ntp_ms;
            }  
            tmp1 = tmp1->next;
            vid = 0;
            aud = 1;
        } else if (tmp1 == NULL && tmp2 != NULL) {
            if(tmp2->ntp_ms > first_ntp || tmp2->ntp_ms == first_ntp) {
                pts_audio_video = pts_audio_video + (tmp2->ntp_ms - first_ntp);
                memset(received_frame + frameLen, 0, FF_INPUT_BUFFER_PADDING_SIZE);
                AVPacket packet;
                av_init_packet(&packet); 
                packet.stream_index = 0;
                packet.data = received_frame;
                packet.size = frameLen;
                if(keyFrame)
                    packet.flags |= AV_PKT_FLAG_KEY;
                packet.dts = pts_audio_video;
                packet.pts = pts_audio_video;
                if(fctx) {
                    if(av_write_frame(fctx, &packet) < 0) {
                        printf("Error writing video frame to file...\n");
                    }
                }
                first_ntp = tmp2->ntp_ms;
            }
            tmp2 = tmp2->next;
            vid = 1;
            aud = 0;
        }
    }
    g_free(received_frame);
    g_free(start);
    return 0;
}

*/

static int parse_control(char *buf, int len, rt_type *rtcp_pkt)
{
    rtcp_t *r;         /* RTCP header */
    int i;
    r = (rtcp_t *)buf;
    if (r->common.version == RTP_VERSION) {
        while (len > 0) {
            len -= (ntohs(r->common.length) + 1) << 2;
            if (len < 0) {
              /* something wrong with packet format */
              printf("Illegal RTCP packet length %d words.\n",
                     ntohs(r->common.length));
              return -1;
            }
            if (r->common.pt == RTCP_SR) {
                uint64_t max32 = UINT32_MAX;
                frame_packet *p = (frame_packet *)g_malloc0(sizeof(frame_packet));
                if(p == NULL) {
                  printf("Memory error!\n");
                  return -1;
                }
                if(rtcp_pkt->last_ts == 0) {
                       /* Simple enough... */
                      p->rtp_ts = (unsigned long)ntohl(r->r.sr.rtp_ts);
                } else {
                    /* Is the new timestamp smaller than the next one, and if so, is it a timestamp reset or simply out of order? */
                    gboolean late_pkt = FALSE;
                    if((unsigned long)ntohl(r->r.sr.rtp_ts) < rtcp_pkt->last_ts && (rtcp_pkt->last_ts-(unsigned long)ntohl(r->r.sr.rtp_ts) > 2*1000*1000*1000)) {
                        if(rtcp_pkt->post_reset_pkts > 1000) {
                            rtcp_pkt->reset = (unsigned long)ntohl(r->r.sr.rtp_ts);
                            printf("Timestamp reset: %"SCNu32"\n", rtcp_pkt->reset);
                            rtcp_pkt->times_resetted++;
                            rtcp_pkt->post_reset_pkts = 0;
                        }
                    } else if(((unsigned long)ntohl(r->r.sr.rtp_ts) > rtcp_pkt->reset) && ((unsigned long)ntohl(r->r.sr.rtp_ts) > rtcp_pkt->last_ts) &&
                                ((unsigned long)ntohl(r->r.sr.rtp_ts)-rtcp_pkt->last_ts > 2*1000*1000*1000)) {
                        if(rtcp_pkt->post_reset_pkts < 1000) {
                            printf("Late pre-reset packet after a timestamp reset: %"SCNu32"\n", (unsigned long)ntohl(r->r.sr.rtp_ts));
                            late_pkt = TRUE;
                            rtcp_pkt->times_resetted--;
                        }
                    } else if((unsigned long)ntohl(r->r.sr.rtp_ts) < rtcp_pkt->reset) {
                        if(rtcp_pkt->post_reset_pkts < 1000) {
                            printf("Updating latest timestamp reset: %"SCNu32" (was %"SCNu32")\n", (unsigned long)ntohl(r->r.sr.rtp_ts), rtcp_pkt->reset);
                            rtcp_pkt->reset = (unsigned long)ntohl(r->r.sr.rtp_ts);
                        } else {
                            rtcp_pkt->reset = (unsigned long)ntohl(r->r.sr.rtp_ts);
                            printf("Timestamp reset: %"SCNu32"\n", rtcp_pkt->reset);
                            rtcp_pkt->times_resetted++;
                            rtcp_pkt->post_reset_pkts = 0;
                        }
                    }
                    /* Take into account the number of resets when setting the internal, 64-bit, timestamp */
                    p->rtp_ts = (rtcp_pkt->times_resetted*max32)+(unsigned long)ntohl(r->r.sr.rtp_ts);
                    if(late_pkt)
                        rtcp_pkt->times_resetted++;
                }
                rtcp_pkt->last_ts = (unsigned long)ntohl(r->r.sr.rtp_ts);
                rtcp_pkt->post_reset_pkts++;
                /* Fill in the rest of the details */
                p->ntp_frac = (unsigned long)ntohl(r->r.sr.ntp_frac);
                p->ntp_sec =  (unsigned long)ntohl(r->r.sr.ntp_sec);     
                uint64_t kFractionsPerSecond = 0x100000000;
		uint64_t value_;
		value_ = p->ntp_sec * kFractionsPerSecond + p->ntp_frac;
		uint32_t seconds = value_ / kFractionsPerSecond; 
		uint32_t fractions = value_ % kFractionsPerSecond; 
		double kNtpFracPerMs = 4.294967296E6;  // 2^32 / 1000.
		double frac_ms = p->ntp_frac / kNtpFracPerMs;
		uint64_t ToMs;
		p->ToMs = 1000 *p->ntp_sec +frac_ms + 0.5;
		p->next = NULL;
                p->prev = NULL;
                if(rtcp_pkt->list == NULL) {
                    /* First element becomes the list itself (and the last item), at least for now */
                    rtcp_pkt->list = p;
                    rtcp_pkt->last = p;
                } else {
                    /* Check where we should insert this, starting from the end */
                    int added = 0;
                    frame_packet *tmp = rtcp_pkt->last;
                    double frequency_khz;
                    frequency_khz = (p->rtp_ts - rtcp_pkt->last->rtp_ts)/(p->ToMs-rtcp_pkt->last->ToMs);
                    while(tmp) {
                        if(tmp->rtp_ts < p->rtp_ts) {
                            /* The new timestamp is greater than the last one we have, append */
                            added = 1;
                            if(tmp->next != NULL) {
                                /* We're inserting */
                                tmp->next->prev = p;
                                p->next = tmp->next;
                            } else {
                                /* Update the last packet */
                                rtcp_pkt->last = p;
                            }
                            tmp->next = p;
                            p->prev = tmp;
                            break;
                        } 
                        /* If either the timestamp ot the sequence number we just got is smaller, keep going back */
                        tmp = tmp->prev;
                    }
                    if(!added) {
                        /* We reached the start */
                        p->next = rtcp_pkt->list;
                        rtcp_pkt->list->prev = p;
                        rtcp_pkt->list = p;
                    }
                }
            }
            r = (rtcp_t *)((guint32 *)r + ntohs(r->common.length) + 1);
        }
    } else {
        printf("invalid version %d\n", r->common.version);
    }
    return len;
} 
/* parse_control */

/* Main Code */
int parse_files (rt_type *rt_type_1, int vp9, int opus) {
    working = 1;
    rt_type_1->offset = 0;
    int bytes = 0, skip = 0;
    uint16_t len = 0;
    char prebuffer[1500];
    memset(prebuffer, 0, 1500);
    rt_type_1->parsed_header = FALSE;
    while(working && rt_type_1->offset < rt_type_1->fsize) {
        /* Read frame header */
        skip = 0;
        fseek(rt_type_1->file, rt_type_1->offset, SEEK_SET);
        bytes = fread(prebuffer, sizeof(char), 8, rt_type_1->file);
        if(bytes != 8 || prebuffer[0] != 'M') {
            printf("Invalid header at offset %ld (%s), the processing will stop here...\n",
            rt_type_1->offset, bytes != 8 ? "not enough bytes" : "wrong prefix");
            break;
        }
        if(prebuffer[1] == 'E') {
            rt_type_1->offset += 8;
            bytes = fread(&len, sizeof(uint16_t), 1, rt_type_1->file);
            len = ntohs(len);
            rt_type_1->offset += 2;
            if(len < 12) {
                /* Not RTP, skip */
                printf("Skipping packet (not RTP?)\n");
                rt_type_1->offset += len;
                continue;
            }
        } else if(prebuffer[1] == 'J') {
            /* New .mjr format, the header may contain useful info */
            rt_type_1->offset += 8;
            bytes = fread(&len, sizeof(uint16_t), 1, rt_type_1->file);
            len = ntohs(len);
            rt_type_1->offset += 2;
            if(len > 0  && !rt_type_1->parsed_header) {
                /* This is the info header */
                printf("New .mjr header format\n");
                bytes = fread(prebuffer, sizeof(char), len, rt_type_1->file);
                rt_type_1->parsed_header = TRUE;
                prebuffer[len] = '\0';
                json_error_t error;
                json_t *info = json_loads(prebuffer, 0, &error);
                if(!info) {
                    printf("JSON error: on line %d: %s\n", error.line, error.text);
                    printf("Error parsing info header...\n");
                    return -1;
                }
                /* Is it audio or video? */
                json_t *type = json_object_get(info, "t");
                if(!type || !json_is_string(type)) {
                    printf("Missing/invalid recording type in info header...\n");
                    return -1;
                }
                const char *t = json_string_value(type);
                if(!strcasecmp(t, "a")) {
                    rt_type_1->opus = 1;
                    rt_type_1->vp9 = 0;
                } else if(!strcasecmp(t, "v")) {
                    rt_type_1->opus = 0;
                    rt_type_1->vp9 = 1;
                } else {
                    printf("Unsupported recording type '%s' in info header...\n", t);
                    return -1;
                }
                /* What codec was used? */
                json_t *codec = json_object_get(info, "c");
                if(!codec || !json_is_string(codec)) {
                    printf("Missing recording codec in info header...\n");
                    return -1;
                }
                const char *c = json_string_value(codec);
                if(!strcasecmp(c, "opus")) {
                    rt_type_1->opus = 1;
                    rt_type_1->vp9 = 0;
                } else if(!strcasecmp(c, "vp9")) {
                    rt_type_1->opus = 0;
                    rt_type_1->vp9 = 1;
		} else {
                    printf("The post-processor only supports Opus and VP9 for now (was '%s')...\n", c);
                    return -1;
                }
                /* When was the file created? */
                json_t *created = json_object_get(info, "s");
                if(!created || !json_is_integer(created)) {
                    printf("Missing recording created time in info header...\n");
                    return -1;
                }
                rt_type_1->c_time = json_integer_value(created);
                /* When was the first frame written? */
                json_t *written = json_object_get(info, "u");
                if(!written || !json_is_integer(written)) {
                    printf("Missing recording written time in info header...\n");
                    return -1;
                }
                rt_type_1->w_time = json_integer_value(written);
                /* Summary */
                printf("This is %s recording:\n", rt_type_1->vp9 ? "a video" : "an audio");
                printf("  -- Codec:   %s\n", c);
                printf("  -- Created: %"SCNi64"\n", rt_type_1->c_time);
                printf("  -- Written: %"SCNi64"\n", rt_type_1->w_time);
            }
        } else {
            printf("Invalid header...\n");
            return -1;
        }        
        /* Skip data for now */
        rt_type_1->offset += len;
    }
    return 0;
}

int split_packets (rt_type *rt_type_1, int vp9, int opus, int type) {
    working = 1;
    rt_type_1->offset = 0;
    rt_type_1->last_ts = 0;
    rt_type_1->reset = 0;
    rt_type_1->times_resetted = 0;
    rt_type_1->post_reset_pkts = 0;
    int bytes = 0, skip = 0;
    uint16_t len = 0;
    char prebuffer[1500];
    memset(prebuffer, 0, 1500);
    uint64_t max32 = UINT32_MAX;
    while(working && rt_type_1->offset < rt_type_1->fsize) {
        /* Read frame header */
        skip = 0;
        fseek(rt_type_1->file, rt_type_1->offset, SEEK_SET);
        bytes = fread(prebuffer, sizeof(char), 8, rt_type_1->file);
        if(bytes != 8 || prebuffer[0] != 'M') {
            /* Broken packet? Stop here */
            break;
        }
        prebuffer[8] = '\0';
        rt_type_1->offset += 8;
        bytes = fread(&len, sizeof(uint16_t), 1, rt_type_1->file);
        len = ntohs(len);
        rt_type_1->offset += 2;
        if(prebuffer[1] == 'J' || ( len < 12)) {
            /* Not RTP, skip */
            printf("  -- Not RTP, skipping\n");
            rt_type_1->offset += len;
            continue;
        }
        if(type == 1) { 
            if(len > 2000) {
                /* Way too large, very likely not RTP, skip */
                printf("  -- Too large packet (%d bytes), skipping\n", len);
                rt_type_1->offset += len;
                continue;
            }
            /* Only read RTP header */
            bytes = fread(prebuffer, sizeof(char), 16, rt_type_1->file);
            janus_pp_rtp_header *rtp = (janus_pp_rtp_header *)prebuffer;
            if(rt_type_1->count <2)
                printf("  -- RTP packet (ssrc=%"SCNu32", pt=%"SCNu16", ext=%"SCNu16", seq=%"SCNu16", ts=%"SCNu32")\n",
                    ntohl(rtp->ssrc), rtp->type, rtp->extension, ntohs(rtp->seq_number), ntohl(rtp->timestamp));
            if(rtp->csrccount) {
                printf("  -- -- Skipping CSRC list\n");
                skip += rtp->csrccount*4;
            }
            if(rtp->extension) {
                janus_pp_rtp_header_extension *ext = (janus_pp_rtp_header_extension *)(prebuffer+12);
                printf("  -- -- RTP extension (type=%"SCNu16", length=%"SCNu16")\n",
                    ntohs(ext->type), ntohs(ext->length));
                skip += 4 + ntohs(ext->length)*4;
            }
            /* Generate frame packet and insert in the ordered list */
            frame_packet *p = (frame_packet *)g_malloc0(sizeof(frame_packet));
            if(p == NULL) {
                printf("Memory error!\n");
                return -1;
            }
            p->seq = ntohs(rtp->seq_number);
            p->pt = rtp->type;
            /* Due to resets, we need to mess a bit with the original timestamps */
            if(rt_type_1->last_ts == 0) {
                /* Simple enough... */
                p->ts = ntohl(rtp->timestamp);
            } else {
                /* Is the new timestamp smaller than the next one, and if so, is it a timestamp reset or simply out of order? */
                gboolean late_pkt = FALSE;
                if(ntohl(rtp->timestamp) < rt_type_1->last_ts && (rt_type_1->last_ts-ntohl(rtp->timestamp) > 2*1000*1000*1000)) {
                    if(rt_type_1->post_reset_pkts > 1000) {
                        rt_type_1->reset = ntohl(rtp->timestamp);
                        printf("Timestamp reset: %"SCNu32"\n", rt_type_1->reset);
                        rt_type_1->times_resetted++;
                        rt_type_1->post_reset_pkts = 0;
                    }
                } else if(ntohl(rtp->timestamp) > rt_type_1->reset && ntohl(rtp->timestamp) > rt_type_1->last_ts &&
                                (ntohl(rtp->timestamp)-rt_type_1->last_ts > 2*1000*1000*1000)) {
                    if(rt_type_1->post_reset_pkts < 1000) {
                        printf("Late pre-reset packet after a timestamp reset: %"SCNu32"\n", ntohl(rtp->timestamp));
                        late_pkt = TRUE;
                        rt_type_1->times_resetted--;
                    }
                } else if(ntohl(rtp->timestamp) < rt_type_1->reset) {
                    if(rt_type_1->post_reset_pkts < 1000) {
                        printf("Updating latest timestamp reset: %"SCNu32" (was %"SCNu32")\n", ntohl(rtp->timestamp), rt_type_1->reset);
                        rt_type_1->reset = ntohl(rtp->timestamp);
                    } else {
                        rt_type_1->reset = ntohl(rtp->timestamp);
                        printf("Timestamp reset: %"SCNu32"\n", rt_type_1->reset);
                        rt_type_1->times_resetted++;
                        rt_type_1->post_reset_pkts = 0;
                    }
                }
                /* Take into account the number of resets when setting the internal, 64-bit, timestamp */
                p->ts = (rt_type_1->times_resetted*max32)+ntohl(rtp->timestamp);
                if(late_pkt)
                    rt_type_1->times_resetted++;
            }
            p->dummy = 0;
            p->len = len;
            p->drop = 0;
            if(rtp->padding) {
                /* There's padding data, let's check the last byte to see how much data we should skip */
                fseek(rt_type_1->file, rt_type_1->offset + len - 1, SEEK_SET);
                bytes = fread(prebuffer, sizeof(char), 1, rt_type_1->file);
                uint8_t padlen = (uint8_t)prebuffer[0];
                printf("Padding at sequence number %hu: %d/%d\n",
                    ntohs(rtp->seq_number), padlen, p->len);
                p->len -= padlen;
                if((p->len - skip - 12) <= 0) {
                    /* Only padding, take note that we should drop the packet later */
                    p->drop = 1;
                    printf("  -- All padding, marking packet as dropped\n");
                }
            }
            rt_type_1->last_ts = ntohl(rtp->timestamp);
            rt_type_1->post_reset_pkts++;
            /* Fill in the rest of the details */
            p->offset = rt_type_1->offset;
            p->skip = skip;
            p->next = NULL;
            p->prev = NULL;
            if(rt_type_1->list == NULL) {
                /* First element becomes the list itself (and the last item), at least for now */
                rt_type_1->list = p;
                rt_type_1->last = p;
            } else {
                /* Check where we should insert this, starting from the end */
                int added = 0;
                frame_packet *tmp = rt_type_1->last;
                while(tmp) {
                    if(tmp->ts < p->ts) {
                        /* The new timestamp is greater than the last one we have, append */
                        added = 1;
                        if(tmp->next != NULL) {
                            /* We're inserting */
                            tmp->next->prev = p;
                            p->next = tmp->next;
                        } else {
                            /* Update the last packet */
                            rt_type_1->last = p;
                        }
                        tmp->next = p;
                        p->prev = tmp;
                        break;
                    } else if(tmp->ts == p->ts) {
                        /* Same timestamp, check the sequence number */
                        if(tmp->seq < p->seq && (abs(tmp->seq - p->seq) < 10000)) {
                            /* The new sequence number is greater than the last one we have, append */
                            added = 1;
                            if(tmp->next != NULL) {
                                /* We're inserting */
                                tmp->next->prev = p;
                                p->next = tmp->next;
                            } else {
                                /* Update the last packet */
                                rt_type_1->last = p;
                            }
                            tmp->next = p;
                            p->prev = tmp;
                            break;
                        } else if(tmp->seq > p->seq && (abs(tmp->seq - p->seq) > 10000)) {
                            /* The new sequence number (resetted) is greater than the last one we have, append */
                            added = 1;
                            if(tmp->next != NULL) {
                                /* We're inserting */
                                tmp->next->prev = p;
                                p->next = tmp->next;
                            } else {
                                /* Update the last packet */
                                rt_type_1->last = p;
                            }
                            tmp->next = p;
                            p->prev = tmp;
                            break;
                        }
                    }
                    /* If either the timestamp ot the sequence number we just got is smaller, keep going back */
                    tmp = tmp->prev;
                }
                if(!added) {
                    /* We reached the start */
                    p->next = rt_type_1->list;
                    rt_type_1->list->prev = p;
                    rt_type_1->list = p;
                }
            }
        } else if(type == 0){
            bytes = fread(prebuffer, sizeof(char), len, rt_type_1->file);
            parse_control(prebuffer,len,rt_type_1);
        }    
        /* Skip data for now */
        rt_type_1->offset += len;
        rt_type_1->count++;
    }
    frame_packet *tt = rt_type_1->list;
    int sq = 0;
    int q = 0;
    int pp = 0;
    uint32_t tim;
    while(tt != NULL) {
        if(q == 0) {
            sq = tt->seq;
            tim = tt->ts;
            tt->diff_ntps = 0;
        } else  {
            if(tt->seq > (sq+1)) {
          /*      sq = tt->seq;
            } else {
                while (sq!=tt->seq) {
                    pp++;
                    sq += pp;
                }*/
                //printf("missing packets current seq %i earlier seq %i ts %"SCNu32" prev %"SCNu32" ntp ts %"SCNu32" ntp prev %"SCNu32"\n",tt->seq,sq,tt->ts,tt->prev->ts,tt->ntp_ms, tt->prev->ntp_ms);
            }
            if(tt->next != NULL){
                if(tt->ts != tt->prev->ts) {
                    if(tt->ts > (tim+2880)){
                        //printf("time variation higher %"SCNu32" prev %"SCNu32" next %"SCNu32" difference %"SCNu32" pts %i -- pts mod %i\n",tt->ts, tt->prev->ts, tt->next->ts, (tt->ts-tt->prev->ts), ((tt->ts-rt_type_1->list->ts)/90),((tt->ts-rt_type_1->list->ts)%90));
                    } else if(tt->ts < (tim+2880)) {
                        //printf("time variation lower %"SCNu32" prev %"SCNu32" next %"SCNu32" difference %"SCNu32" pts %i -- pts mod %i\n",tt->ts, tt->prev->ts, tt->next->ts,(tt->ts-tt->prev->ts),((tt->ts-rt_type_1->list->ts)/90),((tt->ts-rt_type_1->list->ts)%90) );
                    }
                }
            }
            if(tt->ts != tt->prev->ts) {
                tt->diff_ntps = (tt->ts - rt_type_1->list->ts)/90;
            } else {
                tt->diff_ntps = tt->prev->diff_ntps;
            }
            //printf("chang %i \n",(tt->diff_ntps - tt->prev->diff_ntps) );
        }
        tim = tt->ts;
        sq = tt->seq;
        tt = tt->next;
        q++;
    }
    printf("Counted file %"SCNu32" RTP packets\n", rt_type_1->count);
    return 0;
}
int change_timestamp (file_type *file_type_1, int vp9, int opus) {
    int error = 0;
    rt_type *rtp_pkt = file_type_1->rtp_pkt;
    rt_type *rtcp_pkt = file_type_1->rtcp_pkt;
    frame_packet *tmp = rtp_pkt->list;
    rtp_pkt->count = 0;
    while(tmp) {
        rtp_pkt->count++;
        if(tmp->ts > rtcp_pkt->last->rtp_ts) {
            if(opus == 1) {
                tmp->ntp_ms = ((tmp->ts-rtcp_pkt->last->rtp_ts)/48) + rtcp_pkt->last->ToMs;
            } else if (vp9 == 1) {
                tmp->ntp_ms = ((tmp->ts-rtcp_pkt->last->rtp_ts)/90) + rtcp_pkt->last->ToMs;
            }
        } else if(tmp->ts < rtcp_pkt->last->rtp_ts) {
            frame_packet *rtcp_tmp = rtcp_pkt->list;
            if(rtcp_tmp->next != NULL){
                while(!(tmp->ts > rtcp_tmp->rtp_ts && tmp->ts < rtcp_tmp->next->rtp_ts) && rtcp_tmp->next != NULL) {
                    rtcp_tmp = rtcp_tmp->next;
                    if(rtcp_tmp->next == NULL)
                        break;
                }
            }
            if(tmp->ts > rtcp_tmp->rtp_ts) {
                if(opus == 1) {
                    tmp->ntp_ms = ((tmp->ts-rtcp_tmp->rtp_ts)/48) + rtcp_tmp->ToMs;
                } else if (vp9 == 1) {
                    tmp->ntp_ms = ((tmp->ts-rtcp_tmp->rtp_ts)/90) + rtcp_tmp->ToMs;
                }
            } else {
                uint32_t smp = (rtcp_tmp->rtp_ts-tmp->ts); 
                if(opus == 1) {
                    tmp->ntp_ms =  rtcp_tmp->ToMs -((rtcp_tmp->rtp_ts-tmp->ts)/48);
                } else if (vp9 == 1) {
                    tmp->ntp_ms =  rtcp_tmp->ToMs -((rtcp_tmp->rtp_ts-tmp->ts)/90);
                }
            }
	}
        //printf("ts %"SCNu32" \n", tmp->ntp_ms);
        tmp = tmp->next;
    }
    printf("Counted %"SCNu32" frame packets in file\n", rtp_pkt->count);
    return 0;
}

int main(int argc, char *argv[])
{
        fm_count = 0;
        int i, j, m;
        char *destination = (char *)malloc(sizeof(char)*128);
        char *extension = (char *)malloc(sizeof(char)*128);
        printf("Enter the number of streams (audio+video considered as 1) for mixing: \n");
        scanf("%d",&i);
        if(!(i>=2)) {
            printf("Error Minimum two files required for mixing\n");
            exit(1);
        }    
        printf("Enter the destination file: \n");
        scanf("%s",destination);
        extension = strrchr(destination, '.');
	if(extension == NULL) {
            /* No extension? */
            printf( "No extension? Unsupported target file\n");
            exit(1);
        }
	if(strcasecmp(extension, ".webm") && strcasecmp(extension, ".mkv")&& strcasecmp(extension, ".mp4")) {
            /* Unsupported extension? */
            printf( "Unsupported extension '%s'\n", extension);
            exit(1);
        }
        int pp;
        int dd;
        printf("Press Following Number \n 1: AUDIO MIXING \n 2: VIDEO MIXING \n 3: AUDIO AND VIDEO MIXING\n");
        scanf("%d",&dd);
        file_combine_list *file_combine_list_1 = (file_combine_list *)g_malloc0(sizeof(file_combine_list));
        file_combine_list_1->size = 0;
        file_combine_list_1->head = NULL;
        file_combine_list_1->tail = NULL;   
        for (j = 0; j<i; j++) {
            file_combine *number_source = (file_combine*) malloc(sizeof(file_combine));
            number_source->file_av_list_1 =  (file_av_list*) malloc(sizeof(file_av_list));
            number_source->file_av_list_1->size = 0;
            number_source->file_av_list_1->head = NULL;
            number_source->file_av_list_1->tail = NULL;   
            int p;
            file_combine_list_1->mix_type = dd;
            file_av *file_av_1 = (file_av*) malloc(sizeof(file_av));
            if(file_combine_list_1->mix_type== 1 || file_combine_list_1->mix_type == 3) {
                file_av_1->audio_file = (file_type*) malloc(sizeof(file_type));
                file_av_1->audio_file->rtp_pkt = (rt_type*) malloc(sizeof(rt_type));
                file_av_1->audio_file->rtcp_pkt = (rt_type*) malloc(sizeof(rt_type));
                file_av_1->audio_file->rtp_pkt->source = (char*)malloc(sizeof(char)*128);
                file_av_1->audio_file->rtcp_pkt->source = (char*)malloc(sizeof(char)*128);
                printf("Enter audio file \n");
                scanf("%s",file_av_1->audio_file->rtp_pkt->source);
                printf("Enter audio rtcp file \n");
                scanf("%s",file_av_1->audio_file->rtcp_pkt->source);
                file_av_1->audio_file->rtp_pkt->file = fopen(file_av_1->audio_file->rtp_pkt->source, "rb");
                file_av_1->opus = 1; 
                if(file_av_1->audio_file->rtp_pkt->file == NULL) {
                    printf("Could not open one of the file \n");
                    return -1;
                }
                file_av_1->audio_file->rtcp_pkt->file = fopen(file_av_1->audio_file->rtcp_pkt->source, "rb");
                if(file_av_1->audio_file->rtcp_pkt->file == NULL) {
                    printf("Could not open one of the file \n");
                    return -1;
                }
            }
            if(file_combine_list_1->mix_type == 2 || file_combine_list_1->mix_type == 3) {
                file_av_1->video_file = (file_type*) malloc(sizeof(file_type));
                file_av_1->video_file->rtp_pkt = (rt_type*) malloc(sizeof(rt_type));
                file_av_1->video_file->rtcp_pkt = (rt_type*) malloc(sizeof(rt_type));
                file_av_1->video_file->rtp_pkt->source =  (char*)malloc(sizeof(char)*128);
                file_av_1->video_file->rtcp_pkt->source =  (char*)malloc(sizeof(char)*128);
                printf("Enter video file \n");
                scanf("%s",file_av_1->video_file->rtp_pkt->source);
                printf("Enter video rtcp file \n");
                scanf("%s",file_av_1->video_file->rtcp_pkt->source);
                file_av_1->vp9 = 1;
                file_av_1->video_file->rtp_pkt->file = fopen(file_av_1->video_file->rtp_pkt->source, "rb");
                if(file_av_1->video_file->rtp_pkt->file == NULL) {
                    printf("Could not open one of the file \n");
                    return -1;
                }
                file_av_1->video_file->rtcp_pkt->file = fopen(file_av_1->video_file->rtcp_pkt->source, "rb");
                if(file_av_1->video_file->rtcp_pkt->file == NULL) {
                    printf("Could not open one of the file \n");
                    return -1;
                }
            }
            if(file_combine_list_1->mix_type == 1) {
                file_av_1->vp9 = 0;
                fseek(file_av_1->audio_file->rtp_pkt->file, 0L, SEEK_END);
                fseek(file_av_1->audio_file->rtcp_pkt->file, 0L, SEEK_END);
                file_av_1->audio_file->rtp_pkt->fsize = ftell(file_av_1->audio_file->rtp_pkt->file);
                file_av_1->audio_file->rtcp_pkt->fsize = ftell(file_av_1->audio_file->rtcp_pkt->file);
                fseek(file_av_1->audio_file->rtp_pkt->file, 0L, SEEK_SET);
                fseek(file_av_1->audio_file->rtcp_pkt->file, 0L, SEEK_SET);
            } else if (file_combine_list_1->mix_type == 2){
                file_av_1->opus = 0;    
                fseek(file_av_1->video_file->rtp_pkt->file, 0L, SEEK_END);
                fseek(file_av_1->video_file->rtcp_pkt->file, 0L, SEEK_END);
                file_av_1->video_file->rtp_pkt->fsize = ftell(file_av_1->video_file->rtp_pkt->file);
                file_av_1->video_file->rtcp_pkt->fsize = ftell(file_av_1->video_file->rtcp_pkt->file);
                fseek(file_av_1->video_file->rtp_pkt->file, 0L, SEEK_SET);
                fseek(file_av_1->video_file->rtcp_pkt->file, 0L, SEEK_SET);
            }
            if (number_source->file_av_list_1->head) {
                // Binding the node to the list elements.
                file_av_1->next = number_source->file_av_list_1->head;
                file_av_1->prev = number_source->file_av_list_1->head->prev;
                // Binding the list elements to the node.
                number_source->file_av_list_1->head->prev->next = file_av_1;
                number_source->file_av_list_1->head->prev = file_av_1;
            } else {
                file_av_1->next = file_av_1;
                file_av_1->prev = file_av_1;
                number_source->file_av_list_1->tail = file_av_1;
            }
            number_source->file_av_list_1->head = file_av_1;
            number_source->file_av_list_1->size++;
            if (file_combine_list_1->head) {
                // Binding the node to the list elements.
                number_source->next = file_combine_list_1->head;
                number_source->prev = file_combine_list_1->head->prev;
                // Binding the list elements to the node.
                file_combine_list_1->head->prev->next = number_source;
                file_combine_list_1->head->prev = number_source;
            } else {
                number_source->next = number_source;
                number_source->prev = number_source;
                file_combine_list_1->tail = number_source;
            }
            file_combine_list_1->head = number_source;
            file_combine_list_1->size++;
        }
	/* Handle SIGINT */
	working = 1;
	signal(SIGINT, janus_pp_handle_signal);
        /* Let's look for timestamp resets first */
        int value;
        uint64_t max32 = UINT32_MAX;
        file_combine *file_combine_1 = file_combine_list_1->head;
        for (j = 0; j < i; j++) {
            file_av *file_av_1 = file_combine_1->file_av_list_1->head;
            if(file_combine_list_1->mix_type == 1 || file_combine_list_1->mix_type == 3) {
                if (file_av_1->audio_file->rtp_pkt) {
                    value = parse_files(file_av_1->audio_file->rtp_pkt,0,1); 
                    value = split_packets(file_av_1->audio_file->rtp_pkt,0,1,1);
                }
                if (file_av_1->audio_file->rtcp_pkt) {
                    value = parse_files(file_av_1->audio_file->rtcp_pkt,0,1);
                    value = split_packets(file_av_1->audio_file->rtcp_pkt,0,1,0);
                }
                value = change_timestamp(file_av_1->audio_file,0,1);
            } if(file_combine_list_1->mix_type == 2 || file_combine_list_1->mix_type == 3) {
                if (file_av_1->video_file->rtp_pkt) {
                    value = parse_files(file_av_1->video_file->rtp_pkt,1,0);
                    value = split_packets(file_av_1->video_file->rtp_pkt,1,0,1);
                    if(video_preprocess(file_av_1->video_file->rtp_pkt) < 0) {
                        printf("Error pre-processing %s RTP frames...\n");
                        exit(1);
                    }
                }    
                if (file_av_1->video_file->rtcp_pkt) {    
                    value = parse_files(file_av_1->video_file->rtcp_pkt,1,0);
                    value = split_packets(file_av_1->video_file->rtcp_pkt,1,0,0);
                }
                value = change_timestamp(file_av_1->video_file,1,0);
            }
            if(value != 0)
                exit(0);
            file_combine_1 = file_combine_1->next;
        }
	if(!working)
            exit(0);
	if(file_create(destination, file_combine_list_1->mix_type , extension) < 0) {
            printf("Error creating .webm file...\n");
            exit(1);
	}
        if(file_combine_list_1->mix_type == 1) {
            if(audio_mix_process(file_combine_list_1,extension) < 0) {
                printf("Error processing %s RTP frames...\n");
            }
        }
       
        if(file_combine_list_1->mix_type == 2) {
            if(video_mix_process(file_combine_list_1) < 0) {
                printf("Error processing %s RTP frames...\n");
            }
        }
        
        /*
        if(file_combine_list_1->mix_type == 3) {
            if(audio_video_mix_process(file_combine_list_1) < 0) {
                printf("Error processing %s RTP frames...\n");
            }
        }
        */
    /*    if(webm_process(file_combine_list_1) < 0) {
            printf("Error processing %s RTP frames...\n");
	} */
        janus_pp_webm_close();
	//fclose(file);
	/*
	file = fopen(destination, "rb");
	if(file == NULL) {
		printf("No destination file %s??\n", destination);
	} else {
		fseek(file, 0L, SEEK_END);
		fsize = ftell(file);
		fseek(file, 0L, SEEK_SET);
		printf("%s is %zu bytes\n", destination, fsize);
		fclose(file);
	}
	frame_packet *temp = list, *next = NULL;
	while(temp) {
		next = temp->next;
		g_free(temp);
		temp = next;
	}
        */
	printf("Bye!\n");
	return 0;
}












