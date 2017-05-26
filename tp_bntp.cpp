//g++ tp_bntp.cpp -I/usr/local/include -L/usr/local/lib -lavcodec -lavformat -lswscale -lavfilter -lavutil -lx264 -lz -lm -lopencv_imgproc -lopencv_highgui -lopencv_core `pkg-config --cflags --libs opencv` `pkg-config --cflags --libs glib-2.0` -lm -lvpx -ljansson -std=gnu++11
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
#include <inttypes.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <glib.h>
#include <jansson.h>
#include <vpx/vpx_decoder.h>
#include <vpx/vp8dx.h>
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

typedef struct janus_pp_frame_packet {
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
	struct janus_pp_frame_packet *next;
	struct janus_pp_frame_packet *prev;
} janus_pp_frame_packet;


typedef struct rtcp_frame_packet {
        guint32 ntp_sec;  /* NTP timestamp */
        guint32 ntp_frac;
	uint64_t rtp_ts;	/* RTP Timestamp */
	uint16_t len;	/* Length of the data */
	uint8_t drop;	/* Whether this packet can be dropped (e.g., padding)*/
	uint64_t ToMs;
	struct rtcp_frame_packet *next;
	struct rtcp_frame_packet *prev;
} rtcp_frame_packet;

typedef struct file_av {
    char *source;
    char *source_rtcp;
    FILE *file;
    FILE *file_rtcp;
    long fsize;
    long fsize_rtcp;
    long offset;
    long offset_rtcp;
    int opus;
    int opus_rtcp;
    int vp9;
    int vp9_rtcp;
    int count;
    int count_rtcp;
    gboolean parsed_header;
    janus_pp_frame_packet *list;
    janus_pp_frame_packet *last;
    rtcp_frame_packet *list_rtcp;
    rtcp_frame_packet *last_rtcp;
    gint64 c_time;
    gint64 c_time_rtcp;
    gint64 w_time;
    gint64 w_time_rtcp;
    uint32_t last_ts;
    uint32_t last_ts_rtcp;
    uint32_t reset;
    uint32_t reset_rtcp;
    AVCodecContext *codec_ctx;
    AVCodec *codec_dec; 
    guint32 ntp_sec;  /* NTP timestamp */
    guint32 ntp_frac;
    //OpusEncoder *encoder;		/* Opus encoder instance */
    //OpusDecoder *decoder;		/* Opus decoder instance */
    int times_resetted;
    int times_resetted_rtcp;
    int numBytes;
    uint64_t ToMs;
    uint8_t *received_frame;
    uint8_t *buffer;
    uint8_t *start;
    int max_width, max_height, fps;
    int min_ts_diff, max_ts_diff;
    uint32_t post_reset_pkts;
    uint32_t post_reset_pkts_rtcp;
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
	struct file_combine *next;
	struct file_combine *prev;
} file_combine;

typedef struct file_combine_list {
    size_t size;
    struct file_combine *head;
    struct file_combine *tail;
}file_combine_list;



int janus_log_level = 4;
gboolean janus_log_timestamps = FALSE;
gboolean janus_log_colors = TRUE;

int working = 0;


/* Signal handler */
void janus_pp_handle_signal(int signum);
void janus_pp_handle_signal(int signum) {
	working = 0;
}
/*! \file    pp-webm.c
 * \author   Lorenzo Miniero <lorenzo@meetecho.com>
 * \copyright GNU General Public License v3
 * \brief    Post-processing to generate .webm files
 * \details  Implementation of the post-processing code (based on FFmpeg)
 * needed to generate .webm files out of VP8/VP9 RTP frames.
 *
 * \ingroup postprocessing
 * \ref postprocessing
 */


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
static int max_width = 0, max_height = 0, fps = 0;
static AVRational audio_timebase;
static AVRational video_timebase;
static AVOutputFormat *fmt;
static AVCodec *audio_codec;
static AVCodec *video_codec;
static AVDictionary *opt_arg;
static AVCodecContext *context;
static AVCodecContext *video_context;
int janus_pp_webm_create(char *destination) {
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
            avformat_alloc_output_context2(&fctx, fmt, "webm", destination);
        }
        if (!fctx) {
            return -1;
        }    
        fmt = fctx->oformat;
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
        context->sample_fmt = AV_SAMPLE_FMT_S16;
        context->bit_rate = 64000;
        context->sample_rate = 48000;
        context->channel_layout = AV_CH_LAYOUT_STEREO;
        context->channels = 2;
        aStream->time_base = (AVRational){ 1, context->sample_rate};
        audio_timebase = (AVRational){ 1, context->sample_rate};
       /* Some formats want stream headers to be separate. */
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

typedef struct frame_packet {
        int plen;
        char *payload;
        int64_t ts;
        int64_t seq;
} frame_packet;
int janus_pp_webm_process(file_combine_list *file_combine_list_1, int *working) {
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
            janus_pp_frame_packet *tmp = file_av_1->list;
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

static int parse_control(char *buf, int len, file_av *file_av_1)
{
    rtcp_t *r;         /* RTCP header */
    int i;

    r = (rtcp_t *)buf;
    if (r->common.version == RTP_VERSION) {
        printf("\n");
        while (len > 0) {
            len -= (ntohs(r->common.length) + 1) << 2;
            if (len < 0) {
              /* something wrong with packet format */
              printf("Illegal RTCP packet length %d words.\n",
                     ntohs(r->common.length));
              return -1;
            }
            if (r->common.pt == RTCP_SR) {
                //unsigned long long val = (unsigned long long) (unsigned long)ntohl(r->r.sr.ntp_sec) << 32 | (unsigned long)ntohl(r->r.sr.ntp_frac);
                //printf( "%lli \n", val );
                uint64_t max32 = UINT32_MAX;
                rtcp_frame_packet *p = (rtcp_frame_packet *)g_malloc0(sizeof(rtcp_frame_packet));
                if(p == NULL) {
                  printf("Memory error!\n");
                  return -1;
                }

                if(file_av_1->last_ts_rtcp == 0) {
                       /* Simple enough... */
                      p->rtp_ts = (unsigned long)ntohl(r->r.sr.rtp_ts);
                } else {
                    /* Is the new timestamp smaller than the next one, and if so, is it a timestamp reset or simply out of order? */
                    gboolean late_pkt = FALSE;

                    if((unsigned long)ntohl(r->r.sr.rtp_ts) < file_av_1->last_ts_rtcp && (file_av_1->last_ts_rtcp-(unsigned long)ntohl(r->r.sr.rtp_ts) > 2*1000*1000*1000)) {
                        if(file_av_1->post_reset_pkts_rtcp > 1000) {
                            file_av_1->reset_rtcp = (unsigned long)ntohl(r->r.sr.rtp_ts);
                            printf("Timestamp reset: %"SCNu32"\n", file_av_1->reset_rtcp);
                            file_av_1->times_resetted_rtcp++;
                            file_av_1->post_reset_pkts_rtcp = 0;
                        }
                    } else if(((unsigned long)ntohl(r->r.sr.rtp_ts) > file_av_1->reset_rtcp) && ((unsigned long)ntohl(r->r.sr.rtp_ts) > file_av_1->last_ts_rtcp) &&
                                ((unsigned long)ntohl(r->r.sr.rtp_ts)-file_av_1->last_ts_rtcp > 2*1000*1000*1000)) {
                        if(file_av_1->post_reset_pkts_rtcp < 1000) {
                            printf("Late pre-reset packet after a timestamp reset: %"SCNu32"\n", (unsigned long)ntohl(r->r.sr.rtp_ts));
                            late_pkt = TRUE;
                            file_av_1->times_resetted_rtcp--;
                        }
                    } else if((unsigned long)ntohl(r->r.sr.rtp_ts) < file_av_1->reset_rtcp) {
                        if(file_av_1->post_reset_pkts_rtcp < 1000) {
                            printf("Updating latest timestamp reset: %"SCNu32" (was %"SCNu32")\n", (unsigned long)ntohl(r->r.sr.rtp_ts), file_av_1->reset_rtcp);
                            file_av_1->reset_rtcp = (unsigned long)ntohl(r->r.sr.rtp_ts);
                        } else {
                            file_av_1->reset_rtcp = (unsigned long)ntohl(r->r.sr.rtp_ts);
                            printf("Timestamp reset: %"SCNu32"\n", file_av_1->reset_rtcp);
                            file_av_1->times_resetted_rtcp++;
                            file_av_1->post_reset_pkts_rtcp = 0;
                        }
                    }
                    /* Take into account the number of resets when setting the internal, 64-bit, timestamp */
                    p->rtp_ts = (file_av_1->times_resetted_rtcp*max32)+(unsigned long)ntohl(r->r.sr.rtp_ts);
                    if(late_pkt)
                        file_av_1->times_resetted_rtcp++;
                }
                file_av_1->last_ts_rtcp = (unsigned long)ntohl(r->r.sr.rtp_ts);
                file_av_1->post_reset_pkts_rtcp++;
                /* Fill in the rest of the details */
                p->ntp_frac = (unsigned long)ntohl(r->r.sr.ntp_frac);
                p->ntp_sec =  (unsigned long)ntohl(r->r.sr.ntp_sec);     
                //printf("ntp=%lu.%lu ts=%lu\n",p->ntp_sec,p->ntp_frac, p->rtp_ts);
                uint64_t kFractionsPerSecond = 0x100000000;
		uint64_t value_;
		
		value_ = p->ntp_sec * kFractionsPerSecond + p->ntp_frac;
		uint32_t seconds = value_ / kFractionsPerSecond; 
		uint32_t fractions = value_ % kFractionsPerSecond; 
		//printf("%" PRIu64 "\n", value_);
		double kNtpFracPerMs = 4.294967296E6;  // 2^32 / 1000.
		double frac_ms = p->ntp_frac / kNtpFracPerMs;
		uint64_t ToMs;
		p->ToMs = 1000 *p->ntp_sec +frac_ms + 0.5;
		//printf("heyy %" PRIu64 "\n", ToMs);
		
		p->next = NULL;
                p->prev = NULL;
                if(file_av_1->list_rtcp == NULL) {
                    /* First element becomes the list itself (and the last item), at least for now */
                    file_av_1->list_rtcp = p;
                    file_av_1->last_rtcp = p;
                } else {
                    /* Check where we should insert this, starting from the end */
                    int added = 0;
                    rtcp_frame_packet *tmp = file_av_1->last_rtcp;
                    double frequency_khz;
                    frequency_khz = (p->rtp_ts - file_av_1->last_rtcp->rtp_ts)/(p->ToMs-file_av_1->last_rtcp->ToMs);
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
                                file_av_1->last_rtcp = p;
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
                        p->next = file_av_1->list_rtcp;
                        file_av_1->list_rtcp->prev = p;
                        file_av_1->list_rtcp = p;
                    }
                }
		
            }
	
            switch (r->common.pt) {
            case RTCP_SR:
                /*printf(" (SR ssrc=0x%lx p=%d count=%d len=%d\n", 
                  (unsigned long)ntohl(r->r.rr.ssrc),
                  r->common.p, r->common.count,
                      ntohs(r->common.length));
                printf("ntp=%lu.%lu ts=%lu psent=%lu osent=%lu\n",
                  (unsigned long)ntohl(r->r.sr.ntp_sec),
                  (unsigned long)ntohl(r->r.sr.ntp_frac),
                  (unsigned long)ntohl(r->r.sr.rtp_ts),
                  (unsigned long)ntohl(r->r.sr.psent),
                  (unsigned long)ntohl(r->r.sr.osent));
               */ for (i = 0; i < r->common.count; i++) {
                /*  printf("  (ssrc=%0lx fraction=%g lost=%lu last_seq=%lu jit=%lu lsr=%lu dlsr=%lu)\n",
                   (unsigned long)ntohl(r->r.sr.rr[i].ssrc),
                   r->r.sr.rr[i].fraction / 256.,
                   (unsigned long)ntohl(r->r.sr.rr[i].lost), // XXX I'm pretty sure this is wrong 
                   (unsigned long)ntohl(r->r.sr.rr[i].last_seq),
                   (unsigned long)ntohl(r->r.sr.rr[i].jitter),
                   (unsigned long)ntohl(r->r.sr.rr[i].lsr),
                   (unsigned long)ntohl(r->r.sr.rr[i].dlsr));
		*/
                }
               // printf(" )\n"); 
                break;

                case RTCP_RR:
                 /*   printf(" (RR ssrc=0x%lx p=%d count=%d len=%d\n", 
                        (unsigned long)ntohl(r->r.rr.ssrc), r->common.p, r->common.count,
                          ntohs(r->common.length));
                   */ for (i = 0; i < r->common.count; i++) {
                    /*    printf("(ssrc=%0lx fraction=%g lost=%lu last_seq=%lu jit=%lu lsr=%lu dlsr=%lu)\n",
                            (unsigned long)ntohl(r->r.rr.rr[i].ssrc),
                            r->r.rr.rr[i].fraction / 256.,
                            (unsigned long)ntohl(r->r.rr.rr[i].lost),
                            (unsigned long)ntohl(r->r.rr.rr[i].last_seq),
                            (unsigned long)ntohl(r->r.rr.rr[i].jitter),
                            (unsigned long)ntohl(r->r.rr.rr[i].lsr),
                            (unsigned long)ntohl(r->r.rr.rr[i].dlsr));
                   */ }
                    //printf(" )\n"); 
                    break;
                case RTCP_SDES:
                   /* printf(" (SDES p=%d count=%d len=%d\n", 
                      r->common.p, r->common.count, ntohs(r->common.length));
                    *//* buf = (char *)&r->r.sdes;
                    for (i = 0; i < r->common.count; i++) {
                        int remaining = (ntohs(r->common.length) << 2) -
                          (buf - (char *)&r->r.sdes);

                        printf("  (src=0x%lx ", 
                          (unsigned long)ntohl(((struct rtcp_sdes *)buf)->src));
                        if (remaining > 0) {
                          buf = rtp_read_sdes(buf, 
                            (ntohs(r->common.length) << 2) - (buf - (char *)&r->r.sdes));
                          if (!buf) return -1;
                        }
                        else {
                          fprintf(stderr, "Missing at least %d bytes.\n", -remaining);
                          return -1;
                        }
                        printf(")\n");  
                    } */
                    printf(" )\n"); 
                    break;

                case RTCP_BYE:
              /*      printf(" (BYE p=%d count=%d len=%d\n", 
                      r->common.p, r->common.count, ntohs(r->common.length));
                    for (i = 0; i < r->common.count; i++) {
                        printf("ssrc[%d]=%0lx ", i, 
                          (unsigned long)ntohl(r->r.bye.src[i]));
                    }
                    if (ntohs(r->common.length) > r->common.count) {
                        buf = (char *)&r->r.bye.src[r->common.count];
                        printf("reason=\"%*.*s\"", *buf, *buf, buf+1); 
                    }
                */    printf(")\n");
                    break;

                /* invalid type */
                default:
                    printf("(? pt=%d src=0x%lx)\n", r->common.pt, 
                      (unsigned long)ntohl(r->r.sdes.src));
                    break;
            }

            r = (rtcp_t *)((guint32 *)r + ntohs(r->common.length) + 1);
        }
    } else {
        printf("invalid version %d\n", r->common.version);
    }
    return len;
} /* parse_control */

/* Main Code */

int main(int argc, char *argv[])
{
        fm_count = 0;
        int i, j, m;
        char *destination = (char *)malloc(sizeof(char)*128);
        char *extension = (char *)malloc(sizeof(char)*128);
        printf("Enter the number of files for audio mixing: \n");
        scanf("%d",&i);
        printf("Enter the destination file: \n");
        scanf("%s",destination);
        extension = strrchr(destination, '.');
	if(extension == NULL) {
            /* No extension? */
            printf( "No extension? Unsupported target file\n");
            exit(1);
        }
	if(strcasecmp(extension, ".webm")) {
            /* Unsupported extension? */
            printf( "Unsupported extension '%s'\n", extension);
            exit(1);
        }
        file_combine_list *file_combine_list_1 = (file_combine_list *)malloc(sizeof(file_combine_list_1));
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
            file_av *file_av_1 = (file_av*) malloc(sizeof(file_av));
            file_av_1->source =  (char*)malloc(sizeof(char)*128);
            file_av_1->source_rtcp =  (char*)malloc(sizeof(char)*128);
            printf("Enter audio file \n");
            scanf("%s",file_av_1->source);
            printf("Enter audio rtcp file \n");
            scanf("%s",file_av_1->source_rtcp);
            file_av_1->opus = 1;
            file_av_1->vp9 = 0;
            file_av_1->file = fopen(file_av_1->source, "rb");
            if(file_av_1->file == NULL) {
                printf("Could not open one of the file \n");
                return -1;
            }
            file_av_1->file_rtcp = fopen(file_av_1->source_rtcp, "rb");
            if(file_av_1->file_rtcp == NULL) {
                printf("Could not open one of the file \n");
                return -1;
            }
            fseek(file_av_1->file, 0L, SEEK_END);
            fseek(file_av_1->file_rtcp, 0L, SEEK_END);
            file_av_1->fsize = ftell(file_av_1->file);
            file_av_1->fsize_rtcp = ftell(file_av_1->file_rtcp);
            fseek(file_av_1->file, 0L, SEEK_SET);
            fseek(file_av_1->file_rtcp, 0L, SEEK_SET);
            printf("File is %zu bytes\n", file_av_1->fsize);
            printf("File is %zu bytes\n", file_av_1->fsize_rtcp);
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
        file_combine *file_combine_1 = file_combine_list_1->head;
        for (j = 0; j < i; j++) {
            file_av *file_av_1 = file_combine_1->file_av_list_1->head;
            working = 1;
            file_av_1->offset = 0;
            file_av_1->offset_rtcp = 0;
            int bytes = 0, skip = 0;
            uint16_t len = 0;
            char prebuffer[1500];
            memset(prebuffer, 0, 1500);
            file_av_1->parsed_header = FALSE;
            while(working && file_av_1->offset < file_av_1->fsize) {
                /* Read frame header */
                skip = 0;
                fseek(file_av_1->file, file_av_1->offset, SEEK_SET);
                bytes = fread(prebuffer, sizeof(char), 8, file_av_1->file);
                if(bytes != 8 || prebuffer[0] != 'M') {
                    printf("Invalid header at offset %ld (%s), the processing will stop here...\n",
                        file_av_1->offset, bytes != 8 ? "not enough bytes" : "wrong prefix");
                    break;
                }
                if(prebuffer[1] == 'E') {
                    /* Either the old .mjr format header ('MEETECHO' header followed by 'audio' or 'video'), or a frame */
                    //printf("eee %i %i\n",file_av_1->offset, file_av_1->fsize);
                    file_av_1->offset += 8;
                    bytes = fread(&len, sizeof(uint16_t), 1, file_av_1->file);
                    len = ntohs(len);
                    file_av_1->offset += 2;
                    if(len < 12) {
                        /* Not RTP, skip */
                        printf("Skipping packet (not RTP?)\n");
                        file_av_1->offset += len;
                        continue;
                    }
                } else if(prebuffer[1] == 'J') {
                    /* New .mjr format, the header may contain useful info */
                    file_av_1->offset += 8;
                    bytes = fread(&len, sizeof(uint16_t), 1, file_av_1->file);
                    len = ntohs(len);
                    file_av_1->offset += 2;
                    if(len > 0  && !file_av_1->parsed_header) {
                        /* This is the info header */
                        printf("New .mjr header format\n");
                        bytes = fread(prebuffer, sizeof(char), len, file_av_1->file);
                        file_av_1->parsed_header = TRUE;
                        prebuffer[len] = '\0';
                        json_error_t error;
                        json_t *info = json_loads(prebuffer, 0, &error);
                        if(!info) {
                            printf("JSON error: on line %d: %s\n", error.line, error.text);
                            printf("Error parsing info header...\n");
                            exit(1);
                        }
                        /* Is it audio or video? */
                        json_t *type = json_object_get(info, "t");
                        if(!type || !json_is_string(type)) {
                            printf("Missing/invalid recording type in info header...\n");
                            exit(1);
                        }
                        const char *t = json_string_value(type);
                        if(!strcasecmp(t, "a")) {
                            file_av_1->opus = 1;
                            file_av_1->vp9 = 0;
                        } else if(!strcasecmp(t, "v")) {
                            file_av_1->opus = 0;
                            file_av_1->vp9 = 1;
                        } else {
                            printf("Unsupported recording type '%s' in info header...\n", t);
                            exit(1);
                        }
                        /* What codec was used? */
                        json_t *codec = json_object_get(info, "c");
                        if(!codec || !json_is_string(codec)) {
                            printf("Missing recording codec in info header...\n");
                            exit(1);
                        }
                        const char *c = json_string_value(codec);
                        if(!strcasecmp(c, "opus")) {
                            file_av_1->opus = 1;
                            file_av_1->vp9 = 0;
                            if(extension && strcasecmp(extension, ".webm")) {
                                printf("Opus RTP packets can only be converted to a .opus file\n");
                                exit(1);
                            }
                        } else if(!strcasecmp(c, "vp9")) {
                            file_av_1->opus = 0;
                            file_av_1->vp9 = 1;
                            if(extension && strcasecmp(extension, ".webm")) {
				printf("VP9 RTP packets can only be converted to a .webm file\n");
				exit(1);
                            }
			} else {
                            printf("The post-processor only supports Opus and G.711 audio for now (was '%s')...\n", c);
                            exit(1);
                        }
                        /* When was the file created? */
                        json_t *created = json_object_get(info, "s");
                        if(!created || !json_is_integer(created)) {
                            printf("Missing recording created time in info header...\n");
                            exit(1);
                        }
                        file_av_1->c_time = json_integer_value(created);
                        /* When was the first frame written? */
                        json_t *written = json_object_get(info, "u");
                        if(!written || !json_is_integer(written)) {
                            printf("Missing recording written time in info header...\n");
                            exit(1);
                        }
                        file_av_1->w_time = json_integer_value(written);
                        /* Summary */
                        printf("This is %s recording:\n", file_av_1->vp9 ? "a video" : "an audio");
                        printf("  -- Codec:   %s\n", c);
                        printf("  -- Created: %"SCNi64"\n", file_av_1->c_time);
                        printf("  -- Written: %"SCNi64"\n", file_av_1->w_time);
                    }
                } else {
                    printf("Invalid header...\n");
                    exit(1);
                }        
                /* Skip data for now */
                file_av_1->offset += len;
            }
            len = 0;
            bytes = 0, skip = 0;
            file_av_1->parsed_header = FALSE;
            int bytes_rtcp = 0;
            while(working && file_av_1->offset_rtcp < file_av_1->fsize_rtcp) {
                /* Read frame header */
                skip = 0;
                fseek(file_av_1->file_rtcp, file_av_1->offset_rtcp, SEEK_SET);
                bytes_rtcp = fread(prebuffer, sizeof(char), 8, file_av_1->file_rtcp);
                if(bytes_rtcp != 8 || prebuffer[0] != 'M') {
                    printf("Invalid header at offset %ld (%s), the processing will stop here...\n",
                        file_av_1->offset_rtcp, bytes_rtcp != 8 ? "not enough bytes" : "wrong prefix");
                    break;
                }
                if(prebuffer[1] == 'E') {
                    /* Either the old .mjr format header ('MEETECHO' header followed by 'audio' or 'video'), or a frame */
                    //printf("eee %i %i\n",file_av_1->offset, file_av_1->fsize);
                    file_av_1->offset_rtcp += 8;
                    bytes_rtcp = fread(&len, sizeof(uint16_t), 1, file_av_1->file_rtcp);
                    len = ntohs(len);
                    file_av_1->offset_rtcp += 2;
                    if(len < 12) {
                         /* Not RTP, skip */
                        printf("Skipping packet (not RTP?)\n");
                        file_av_1->offset_rtcp += len;
                        continue;
                    }
                } else if(prebuffer[1] == 'J') {
                    /* New .mjr format, the header may contain useful info */
                    file_av_1->offset_rtcp += 8;
                    bytes_rtcp = fread(&len, sizeof(uint16_t), 1, file_av_1->file_rtcp);
                    len = ntohs(len);
                    file_av_1->offset_rtcp += 2;
                    if(len > 0  && !file_av_1->parsed_header) {
                        /* This is the info header */
                        printf("New .mjr header format\n");
                        bytes_rtcp = fread(prebuffer, sizeof(char), len, file_av_1->file_rtcp);
                        file_av_1->parsed_header = TRUE;
                        prebuffer[len] = '\0';
                        json_error_t error;
                        json_t *info = json_loads(prebuffer, 0, &error);
                        if(!info) {
                            printf("JSON error: on line %d: %s\n", error.line, error.text);
                            printf("Error parsing info header...\n");
                            exit(1);
                        }
                        /* Is it audio or video? */
                        json_t *type = json_object_get(info, "t");
                        if(!type || !json_is_string(type)) {
                            printf("Missing/invalid recording type in info header...\n");
                            exit(1);
                        }
                        const char *t = json_string_value(type);
                        if(!strcasecmp(t, "a")) {
                            file_av_1->opus_rtcp = 1;
                            file_av_1->vp9_rtcp = 0;
                        } else if(!strcasecmp(t, "v")) {
                            file_av_1->opus_rtcp = 0;
                            file_av_1->vp9_rtcp = 1;
                        } else {
                            printf("Unsupported recording type '%s' in info header...\n", t);
                            exit(1);
                        }
                        /* What codec was used? */
                        json_t *codec = json_object_get(info, "c");
                        if(!codec || !json_is_string(codec)) {
                            printf("Missing recording codec in info header...\n");
                            exit(1);
                        }
                        const char *c = json_string_value(codec);
                        if(!strcasecmp(c, "opus")) {
                            file_av_1->opus_rtcp = 1;
                            file_av_1->vp9_rtcp = 0;
                            if(extension && strcasecmp(extension, ".webm")) {
                                printf("Opus RTP packets can only be converted to a .opus file\n");
                                exit(1);
                            }
                        } else if(!strcasecmp(c, "vp9")) {
                            file_av_1->opus_rtcp = 0;
                            file_av_1->vp9_rtcp = 1;
                            if(extension && strcasecmp(extension, ".webm")) {
				printf("VP9 RTP packets can only be converted to a .webm file\n");
				exit(1);
                            }
			} else {
                            printf("The post-processor only supports Opus and G.711 audio for now (was '%s')...\n", c);
                            exit(1);
                        }
                        /* When was the file created? */
                        json_t *created = json_object_get(info, "s");
                        if(!created || !json_is_integer(created)) {
                            printf("Missing recording created time in info header...\n");
                            exit(1);
                        }
                        file_av_1->c_time_rtcp = json_integer_value(created);
                        /* When was the first frame written? */
                        json_t *written = json_object_get(info, "u");
                        if(!written || !json_is_integer(written)) {
                            printf("Missing recording written time in info header...\n");
                            exit(1);
                        }
                        file_av_1->w_time_rtcp = json_integer_value(written);
                        /* Summary */
                        printf("This is %s recording:\n", file_av_1->vp9_rtcp ? "a video" : "an audio");
                        printf("  -- Codec:   %s\n", c);
                        printf("  -- Created: %"SCNi64"\n", file_av_1->c_time_rtcp);
                        printf("  -- Written: %"SCNi64"\n", file_av_1->w_time_rtcp);
                    }
                } else {
                    printf("Invalid header...\n");
                    exit(1);
                }        
                /* Skip data for now */
                file_av_1->offset_rtcp += len;
            }
            file_combine_1 = file_combine_1->next;
        }
	if(!working)
            exit(0);

	uint64_t max32 = UINT32_MAX;
	/* Start loop */
        file_combine_1 = file_combine_list_1->head;
        for(j = 0; j < i; j++) {
            file_av *file_av_1 = file_combine_1->file_av_list_1->head;
            working = 1;
            file_av_1->offset = 0;
            file_av_1->last_ts = 0;
            file_av_1->reset = 0;
            file_av_1->times_resetted = 0;
            file_av_1->post_reset_pkts = 0;
            int bytes = 0, skip = 0;
            uint16_t len = 0;
            char prebuffer[1500];
            memset(prebuffer, 0, 1500);
            while(working && file_av_1->offset < file_av_1->fsize) {
                /* Read frame header */
                skip = 0;
                fseek(file_av_1->file, file_av_1->offset, SEEK_SET);
                bytes = fread(prebuffer, sizeof(char), 8, file_av_1->file);
                if(bytes != 8 || prebuffer[0] != 'M') {
                    /* Broken packet? Stop here */
                    break;
                }
                prebuffer[8] = '\0';
                //printf("Header: %s\n", prebuffer);
                file_av_1->offset += 8;
                bytes = fread(&len, sizeof(uint16_t), 1, file_av_1->file);
                len = ntohs(len);
                //printf("  -- Length: %"SCNu16"\n", len);
                file_av_1->offset += 2;
                if(prebuffer[1] == 'J' || ( len < 12)) {
                    /* Not RTP, skip */
                    printf("  -- Not RTP, skipping\n");
                    file_av_1->offset += len;
                    continue;
                }
                if(len > 2000) {
                    /* Way too large, very likely not RTP, skip */
                    printf("  -- Too large packet (%d bytes), skipping\n", len);
                    file_av_1->offset += len;
                    continue;
                }
                /* Only read RTP header */
                bytes = fread(prebuffer, sizeof(char), 16, file_av_1->file);
                janus_pp_rtp_header *rtp = (janus_pp_rtp_header *)prebuffer;
                if(file_av_1->count <2)
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
                janus_pp_frame_packet *p = (janus_pp_frame_packet *)g_malloc0(sizeof(janus_pp_frame_packet));
                if(p == NULL) {
                    printf("Memory error!\n");
                    return -1;
                }
                p->seq = ntohs(rtp->seq_number);
                p->pt = rtp->type;
                /* Due to resets, we need to mess a bit with the original timestamps */
                if(file_av_1->last_ts == 0) {
                    /* Simple enough... */
                    p->ts = ntohl(rtp->timestamp);
                } else {
                    /* Is the new timestamp smaller than the next one, and if so, is it a timestamp reset or simply out of order? */
                    gboolean late_pkt = FALSE;
                    if(ntohl(rtp->timestamp) < file_av_1->last_ts && (file_av_1->last_ts-ntohl(rtp->timestamp) > 2*1000*1000*1000)) {
                        if(file_av_1->post_reset_pkts > 1000) {
                            file_av_1->reset = ntohl(rtp->timestamp);
                            printf("Timestamp reset: %"SCNu32"\n", file_av_1->reset);
                            file_av_1->times_resetted++;
                            file_av_1->post_reset_pkts = 0;
                        }
                    } else if(ntohl(rtp->timestamp) > file_av_1->reset && ntohl(rtp->timestamp) > file_av_1->last_ts &&
                            (ntohl(rtp->timestamp)-file_av_1->last_ts > 2*1000*1000*1000)) {
                        if(file_av_1->post_reset_pkts < 1000) {
                            printf("Late pre-reset packet after a timestamp reset: %"SCNu32"\n", ntohl(rtp->timestamp));
                            late_pkt = TRUE;
                            file_av_1->times_resetted--;
                        }
                    } else if(ntohl(rtp->timestamp) < file_av_1->reset) {
                        if(file_av_1->post_reset_pkts < 1000) {
                            printf("Updating latest timestamp reset: %"SCNu32" (was %"SCNu32")\n", ntohl(rtp->timestamp), file_av_1->reset);
                            file_av_1->reset = ntohl(rtp->timestamp);
                        } else {
                            file_av_1->reset = ntohl(rtp->timestamp);
                            printf("Timestamp reset: %"SCNu32"\n", file_av_1->reset);
                            file_av_1->times_resetted++;
                            file_av_1->post_reset_pkts = 0;
                        }
                    }
                    /* Take into account the number of resets when setting the internal, 64-bit, timestamp */
                    p->ts = (file_av_1->times_resetted*max32)+ntohl(rtp->timestamp);
                    if(late_pkt)
                        file_av_1->times_resetted++;
                }
                p->len = len;
                p->drop = 0;
                if(rtp->padding) {
                    /* There's padding data, let's check the last byte to see how much data we should skip */
                    fseek(file_av_1->file, file_av_1->offset + len - 1, SEEK_SET);
                    bytes = fread(prebuffer, sizeof(char), 1, file_av_1->file);
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
                file_av_1->last_ts = ntohl(rtp->timestamp);
                file_av_1->post_reset_pkts++;
                /* Fill in the rest of the details */
                p->offset = file_av_1->offset;
                p->skip = skip;
                p->next = NULL;
                p->prev = NULL;
                if(file_av_1->list == NULL) {
                    /* First element becomes the list itself (and the last item), at least for now */
                    file_av_1->list = p;
                    file_av_1->last = p;
                } else {
                    /* Check where we should insert this, starting from the end */
                    int added = 0;
                    janus_pp_frame_packet *tmp = file_av_1->last;
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
                                file_av_1->last = p;
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
                                    file_av_1->last = p;
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
                                    file_av_1->last = p;
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
                        p->next = file_av_1->list;
                        file_av_1->list->prev = p;
                        file_av_1->list = p;
                    }
                }
                /* Skip data for now */
                file_av_1->offset += len;
                file_av_1->count++;
            }
            working = 1;
            file_av_1->offset_rtcp = 0;
            file_av_1->last_ts_rtcp = 0;
            file_av_1->reset_rtcp = 0;
            file_av_1->times_resetted_rtcp = 0;
            file_av_1->post_reset_pkts_rtcp = 0;
            int bytes_rtcp = 0, skip_rtcp = 0;
            uint16_t len_rtcp = 0;
            char *prebuffer_rtcp = (char *) malloc(1500);
           // memset(prebuffer_rtcp, 0, 1500);
            while(working && file_av_1->offset_rtcp < file_av_1->fsize_rtcp) {
                /* Read frame header */
                skip_rtcp = 0;
                fseek(file_av_1->file_rtcp, file_av_1->offset_rtcp, SEEK_SET);
                bytes_rtcp = fread(prebuffer_rtcp, sizeof(char), 8, file_av_1->file_rtcp);
                if(bytes_rtcp != 8 || prebuffer_rtcp[0] != 'M') {
                    /* Broken packet? Stop here */
                    
                    break;
                }
                prebuffer_rtcp[8] = '\0';
                //printf("Header: %s\n", prebuffer);
                file_av_1->offset_rtcp += 8;
                bytes_rtcp = fread(&len_rtcp, sizeof(uint16_t), 1, file_av_1->file_rtcp);
                len_rtcp = ntohs(len_rtcp);
                printf("  -- Length: %"SCNu16"\n", len_rtcp);
                file_av_1->offset_rtcp += 2;
                if(prebuffer_rtcp[1] == 'J' || ( len_rtcp < 12)) {
                    /* Not RTP, skip */
                    printf("  -- Not RTCP, skipping\n");
                    file_av_1->offset_rtcp += len_rtcp;
                    continue;
                }
                bytes_rtcp = fread(prebuffer_rtcp, sizeof(char), len_rtcp, file_av_1->file_rtcp);
                parse_control(prebuffer_rtcp,len_rtcp,file_av_1);
                file_av_1->offset_rtcp += len_rtcp;
                file_av_1->count_rtcp++;
            }
            printf("Counted file %"SCNu32" RTP packets\n", file_av_1->count);
            printf("Counted file %"SCNu32" RTCP packets\n", file_av_1->count_rtcp);
            janus_pp_frame_packet *tmp = file_av_1->list;
            rtcp_frame_packet *rtcp_tmp = file_av_1->list_rtcp;
            working = 1;
            while(working && tmp != NULL) {
                if(rtcp_tmp == NULL)
                    break;
                //while(!(tmp->ts > rtcp_tmp->rtp_ts && tmp->ts < rtcp_tmp->next->rtp_ts) && rtcp_tmp->next != NULL) {
                if(rtcp_tmp->next != NULL){
                    while(!(tmp->ts > rtcp_tmp->rtp_ts && tmp->ts < rtcp_tmp->next->rtp_ts) && rtcp_tmp->next != NULL) {
                        rtcp_tmp = rtcp_tmp->next;
                        if(rtcp_tmp->next == NULL)
                            break;
                    }
                }
                /*if(rtcp_tmp->next == NULL && tmp->ts > rtcp_tmp->rtp_ts) {
                    tmp->ntp_frac = rtcp_tmp->ntp_frac + (tmp->ts-rtcp_tmp->rtp_ts);
                } else if(rtcp_tmp->next == NULL && tmp->ts < rtcp_tmp->rtp_ts) {
                     tmp->ntp_frac = rtcp_tmp->ntp_frac + (tmp->ts-rtcp_tmp->rtp_ts);
                } 
                if(tmp->ts > rtcp_tmp->rtp_ts && tmp->ts < rtcp_tmp->next->rtp_ts) {
                */    tmp->ntp_frac = rtcp_tmp->ntp_frac + (tmp->ts-rtcp_tmp->rtp_ts);
                      tmp->ntp_sec = rtcp_tmp->ntp_sec;
                //}
                //printf("RTCP here ntp=%lu.%lu ts=%lu\n",rtcp_tmp->ntp_sec,rtcp_tmp->ntp_frac, rtcp_tmp->rtp_ts);
                //printf("RTP here ntp=%lu.%lu ts=%lu \n",tmp->ntp_sec,tmp->ntp_frac, tmp->ts);
                tmp = tmp->next;
            }
            file_combine_1 = file_combine_1->next;
        }
        if(!working)
            exit(0);
        file_combine_1 = file_combine_list_1->head;
	for (j = 0; j<i; j++) {
            file_av *file_av_1 = file_combine_1->file_av_list_1->head;
            int error = 0;
            //file_av_1->decoder = opus_decoder_create(48000, 1, &error);
            janus_pp_frame_packet *tmp = file_av_1->list;
            file_av_1->count = 0;
            while(tmp) {
                file_av_1->count++;
                if(tmp->ts > file_av_1->last_rtcp->rtp_ts) {
                  tmp->ntp_ms = ((tmp->ts-file_av_1->last_rtcp->rtp_ts)/90) + file_av_1->last_rtcp->ToMs;
                  printf("%" PRIu64 " %lu %" PRIu64 " %lu \n",tmp->ntp_ms,tmp->ts, file_av_1->last_rtcp->ToMs, file_av_1->last_rtcp->rtp_ts);
                }else if(tmp->ts < file_av_1->last_rtcp->rtp_ts) {
                   // tmp->ntp_ms = file_av_1->last_rtcp->ToMs - (((tmp->ts-file_av_1->last_rtcp->rtp_ts)/90));
			                    rtcp_frame_packet *rtcp_tmp = file_av_1->list_rtcp;
                    if(rtcp_tmp->next != NULL){
                        while(!(tmp->ts > rtcp_tmp->rtp_ts && tmp->ts < rtcp_tmp->next->rtp_ts) && rtcp_tmp->next != NULL) {
                            rtcp_tmp = rtcp_tmp->next;
                            if(rtcp_tmp->next == NULL)
                                break;
                        }
                    }
		if(tmp->ts > rtcp_tmp->rtp_ts) {

                    tmp->ntp_ms = ((tmp->ts-rtcp_tmp->rtp_ts)/90) + rtcp_tmp->ToMs;
			printf("ssss %" PRIu64 " %lu %" PRIu64 " %lu \n",tmp->ntp_ms,tmp->ts, rtcp_tmp->ToMs, rtcp_tmp->rtp_ts);
		} else {
		  //tmp->ntp_ms =  rtcp_tmp->ToMs - ((tmp->ts-rtcp_tmp->rtp_ts)/90);
		uint32_t smp = (rtcp_tmp->rtp_ts-tmp->ts); 
		tmp->ntp_ms =  rtcp_tmp->ToMs -((rtcp_tmp->rtp_ts-tmp->ts)/90);
			printf("ssss %" PRIu64 " %lu %lu %" PRIu64 "\n",tmp->ntp_ms,tmp->ts,rtcp_tmp->rtp_ts,rtcp_tmp->ToMs);
		}
		 }

		//    printf("[%10lu][%4d] seq=%"SCNu16", ts=%"SCNu64", time=%"SCNu64"s\n", tmp->offset, tmp->len, tmp->seq, tmp->ts, (tmp->ts-file_av_1->list->ts)/90000);
                tmp = tmp->next;
            }
            printf("Counted %"SCNu32" frame packets in file\n", file_av_1->count);
            file_combine_1 = file_combine_1->next;
        }
     
	/*if(janus_pp_webm_create(destination) < 0) {
            printf("Error creating .webm file...\n");
            exit(1);
	}
        
        if(janus_pp_webm_process(file_combine_list_1, &working) < 0) {
            printf("Error processing %s RTP frames...\n");
	}
        /*
	janus_pp_webm_close();
	fclose(file);
	
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
	janus_pp_frame_packet *temp = list, *next = NULL;
	while(temp) {
		next = temp->next;
		g_free(temp);
		temp = next;
	}
        */
	printf("Bye!\n");
	return 0;
}







