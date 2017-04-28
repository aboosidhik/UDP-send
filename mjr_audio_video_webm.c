/*! \file    janus-pp-rec.c
 * \author   Lorenzo Miniero <lorenzo@meetecho.com>
 * \copyright GNU General Public License v3
 * \brief    Simple utility to post-process .mjr files saved by Janus
 * \details  Our Janus WebRTC gateway provides a simple helper (janus_recorder)
 * to allow plugins to record audio, video and text frames sent by users. At the time
 * of writing, this helper has been integrated in several plugins in Janus.
 * To keep things simple on the Janus side, though, no processing
 * at all is done in the recording step: this means that the recorder
 * actually only dumps the RTP frames it receives to a file in a structured way,
 * so that they can be post-processed later on to extract playable media
 * files. This utility allows you to process those files, in order to
 * get a working media file you can playout with an external player.
 * The tool will generate a .webm if the recording includes VP8 frames,
 * an .opus if the recording includes Opus frames, an .mp4 if the recording
 * includes H.264 frames, and a .wav file if the recording includes
 * G.711 (mu-law or a-law) frames. In case the recording contains text
 * frames as received via data channels, instead, a .srt file will be
 * generated with the text content and the related timing information.
 * 
 * Using the utility is quite simple. Just pass, as arguments to the tool,
 * the path to the .mjr source file you want to post-process, and the
 * path to the destination file, e.g.:
 * 
\verbatimwhere
./janus-pp-rec /path/to/source.mjr /path/to/destination.[opus|wav|webm|h264|srt]
\endverbatim 
 * 
 * An attempt to specify an output format that is not compliant with the
 * recording content (e.g., a .webm for H.264 frames) will result in an
 * error since, again, no transcoding is involved.
 *
 * You can also just print the internal header of the recording, or parse
 * it without processing it (e.g., for debugging), by invoking the tool
 * in a different way:
 *
\verbatim
./janus-pp-rec --header /path/to/source.mjr
./janus-pp-rec --parse /path/to/source.mjr
\endverbatim
 *
 * \note This utility does not do any form of transcoding. It just
 * depacketizes the RTP frames in order to get the payload, and saves
 * the frames in a valid container. Any further post-processing (e.g.,
 * muxing audio and video belonging to the same media session in a single
 * .webm file) is up to third-party applications.
 * 
 * \ingroup postprocessing
 * \ref postprocessing
 */

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
#include <glib.h>
#include <jansson.h>


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
	struct janus_pp_frame_packet *next;
	struct janus_pp_frame_packet *prev;
} janus_pp_frame_packet;

int janus_log_level = 4;
gboolean janus_log_timestamps = FALSE;
gboolean janus_log_colors = TRUE;

static janus_pp_frame_packet *list = NULL, *last = NULL;
static janus_pp_frame_packet *list_video = NULL, *last_video = NULL;
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
int janus_pp_webm_create(char *destination, int vp8) {
	if(destination == NULL)
		return -1;
#if LIBAVCODEC_VERSION_MAJOR < 55
	if(!vp8) {
		printf("Your FFmpeg version does not support VP9\n");
		return -1;
	}
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
        video_codec = avcodec_find_encoder(AV_CODEC_ID_VP9);
        vStream = avformat_new_stream(fctx, NULL);
        aStream = avformat_new_stream(fctx, NULL);
        if (!aStream) {
            printf("Could not allocate audio stream\n");
            return -1;
        } 
        if (!vStream) {
            printf("Could not allocate video stream\n");
            return -1;
        }
        vStream->id = fctx->nb_streams-1;
        aStream->id = fctx->nb_streams-1;
        video_context = avcodec_alloc_context3(video_codec);
        context = avcodec_alloc_context3(audio_codec);
        if (!context) {
            printf("Could not alloc an encoding context\n");
            return -1;
        } 
        if (!video_context) {
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
        video_context->codec_type = AVMEDIA_TYPE_VIDEO;
        video_context->codec_id = AV_CODEC_ID_VP9;
        video_context->width = max_width;
	video_context->height = max_height;
        vStream->time_base = (AVRational){1, fps};
        video_timebase = (AVRational){1, fps};
        video_context->time_base =vStream->time_base;
        video_context->pix_fmt = AV_PIX_FMT_YUV420P;
       /* Some formats want stream headers to be separate. */
        if (fctx->oformat->flags & AVFMT_GLOBALHEADER) {
            context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            video_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }    
        int ret;
        /* open it */
       
        opt_arg = NULL;
        ret = avcodec_open2(context, audio_codec, &opt_arg);
        if (ret < 0) {
            printf("Could not open audio codec\n");
            return -1;
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
        /*
	fctx = avformat_alloc_context();
	if(fctx == NULL) {
		printf( "Error allocating context\n");
		return -1;
	}
	fctx->oformat = av_guess_format("webm", NULL, NULL);
	if(fctx->oformat == NULL) {
		printf( "Error guessing format\n");
		return -1;
	}
	snprintf(fctx->filename, sizeof(fctx->filename), "%s", destination);
	//~ vStream = av_new_stream(fctx, 0);
	vStream = avformat_new_stream(fctx, 0);
	if(vStream == NULL) {
		printf("Error adding stream\n");
		return -1;
	}
	//~ avcodec_get_context_defaults2(vStream->codec, CODEC_TYPE_VIDEO);
#if LIBAVCODEC_VER_AT_LEAST(53, 21)
	avcodec_get_context_defaults3(vStream->codec, AVMEDIA_TYPE_VIDEO);
#else
	avcodec_get_context_defaults2(vStream->codec, AVMEDIA_TYPE_VIDEO);
#endif
#if LIBAVCODEC_VER_AT_LEAST(54, 25)
	#if LIBAVCODEC_VERSION_MAJOR >= 55
	vStream->codec->codec_id = vp8 ? AV_CODEC_ID_VP8 : AV_CODEC_ID_VP9;
	#else
	vStream->codec->codec_id = AV_CODEC_ID_VP8;
	#endif
#else
	vStream->codec->codec_id = CODEC_ID_VP8;
#endif
	//~ vStream->codec->codec_type = CODEC_TYPE_VIDEO;
	vStream->codec->codec_type = AVMEDIA_TYPE_VIDEO;
	vStream->codec->time_base = (AVRational){1, fps};
	vStream->codec->width = max_width;
	vStream->codec->height = max_height;
	vStream->codec->pix_fmt = PIX_FMT_YUV420P;
	if (fctx->flags & AVFMT_GLOBALHEADER)
		vStream->codec->flags |= CODEC_FLAG_GLOBAL_HEADER;
	//~ fctx->timestamp = 0;
	//~ if(url_fopen(&fctx->pb, fctx->filename, URL_WRONLY) < 0) {
	if(avio_open(&fctx->pb, fctx->filename, AVIO_FLAG_WRITE) < 0) {
		printf( "Error opening file for output\n");
		return -1;
	}
	//~ memset(&parameters, 0, sizeof(AVFormatParameters));
	//~ av_set_parameters(fctx, &parameters);
	//~ fctx->preload = (int)(0.5 * AV_TIME_BASE);
	//~ fctx->max_delay = (int)(0.7 * AV_TIME_BASE);
	//~ if(av_write_header(fctx) < 0) {
	if(avformat_write_header(fctx, NULL) < 0) {
		printf( "Error writing header\n");
		return -1;
	}
        
        */
	return 0;
}

int janus_pp_webm_preprocess(FILE *file, janus_pp_frame_packet *list, int vp8) {
	if(!file || !list)
		return -1;
	janus_pp_frame_packet *tmp = list;
	int bytes = 0, min_ts_diff = 0, max_ts_diff = 0;
	char prebuffer[1500];
	memset(prebuffer, 0, 1500);
	while(tmp) {
		if(tmp == list || tmp->ts > tmp->prev->ts) {
			if(tmp->prev != NULL && tmp->ts > tmp->prev->ts) {
				int diff = tmp->ts - tmp->prev->ts;
				if(min_ts_diff == 0 || min_ts_diff > diff)
					min_ts_diff = diff;
				if(max_ts_diff == 0 || max_ts_diff < diff)
					max_ts_diff = diff;
			}
			if(tmp->prev != NULL && (tmp->seq - tmp->prev->seq > 1)) {
				printf("Lost a packet here? (got seq %"SCNu16" after %"SCNu16", time ~%"SCNu64"s)\n",
					tmp->seq, tmp->prev->seq, (tmp->ts-list->ts)/90000);
			}
		}
		if(tmp->drop) {
			/* We marked this packet as one to drop, before */
			printf("Dropping previously marked video packet (time ~%"SCNu64"s)\n", (tmp->ts-list->ts)/90000);
			tmp = tmp->next;
			continue;
		}
		if(vp8) {
			/* https://tools.ietf.org/html/draft-ietf-payload-vp8 */
			/* Read the first bytes of the payload, and get the first octet (VP8 Payload Descriptor) */
			fseek(file, tmp->offset+12+tmp->skip, SEEK_SET);
			bytes = fread(prebuffer, sizeof(char), 16, file);
			if(bytes != 16)
				printf("Didn't manage to read all the bytes we needed (%d < 16)...\n", bytes);
			char *buffer = (char *)&prebuffer;
			uint8_t vp8pd = *buffer;
			uint8_t xbit = (vp8pd & 0x80);
			uint8_t sbit = (vp8pd & 0x10);
			/* Read the Extended control bits octet */
			if (xbit) {
				buffer++;
				vp8pd = *buffer;
				uint8_t ibit = (vp8pd & 0x80);
				uint8_t lbit = (vp8pd & 0x40);
				uint8_t tbit = (vp8pd & 0x20);
				uint8_t kbit = (vp8pd & 0x10);
				if(ibit) {
					/* Read the PictureID octet */
					buffer++;
					vp8pd = *buffer;
					uint16_t picid = vp8pd, wholepicid = picid;
					uint8_t mbit = (vp8pd & 0x80);
					if(mbit) {
						memcpy(&picid, buffer, sizeof(uint16_t));
						wholepicid = ntohs(picid);
						picid = (wholepicid & 0x7FFF);
						buffer++;
					}
				}
				if(lbit) {
					/* Read the TL0PICIDX octet */
					buffer++;
					vp8pd = *buffer;
				}
				if(tbit || kbit) {
					/* Read the TID/KEYIDX octet */
					buffer++;
					vp8pd = *buffer;
				}
			}
			buffer++;	/* Now we're in the payload */
			if(sbit) {
				unsigned long int vp8ph = 0;
				memcpy(&vp8ph, buffer, 4);
				vp8ph = ntohl(vp8ph);
				uint8_t pbit = ((vp8ph & 0x01000000) >> 24);
				if(!pbit) {
					/* Get resolution */
					unsigned char *c = (unsigned char *)buffer+3;
					/* vet via sync code */
					if(c[0]!=0x9d||c[1]!=0x01||c[2]!=0x2a) {
						printf("First 3-bytes after header not what they're supposed to be?\n");
					} else {
						int vp8w = swap2(*(unsigned short*)(c+3))&0x3fff;
						int vp8ws = swap2(*(unsigned short*)(c+3))>>14;
						int vp8h = swap2(*(unsigned short*)(c+5))&0x3fff;
						int vp8hs = swap2(*(unsigned short*)(c+5))>>14;
						printf("(seq=%"SCNu16", ts=%"SCNu64") Key frame: %dx%d (scale=%dx%d)\n", tmp->seq, tmp->ts, vp8w, vp8h, vp8ws, vp8hs);
						if(vp8w > max_width)
							max_width = vp8w;
						if(vp8h > max_height)
							max_height = vp8h;
					}
				}
			}
		} else {
			/* https://tools.ietf.org/html/draft-ietf-payload-vp9 */
			/* Read the first bytes of the payload, and get the first octet (VP9 Payload Descriptor) */
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
				/* Read the PictureID octet */
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
					/* Non-flexible mode, skip TL0PICIDX */
					buffer++;
				}
			}
			if(fbit && pbit) {
				/* Skip reference indices */
				uint8_t nbit = 1;
				while(nbit) {
					vp9pd = *buffer;
					nbit = (vp9pd & 0x01);
					buffer++;
				}
			}
			if(vbit) {
				/* Parse and skip SS */
				vp9pd = *buffer;
				uint n_s = (vp9pd & 0xE0) >> 5;
				n_s++;
				uint8_t ybit = (vp9pd & 0x10);
				if(ybit) {
					/* Iterate on all spatial layers and get resolution */
					buffer++;
					uint i=0;
					for(i=0; i<n_s; i++) {
						/* Width */
						uint16_t *w = (uint16_t *)buffer;
						int width = ntohs(*w);
						buffer += 2;
						/* Height */
						uint16_t *h = (uint16_t *)buffer;
						int height = ntohs(*h);
						buffer += 2;
						if(width > max_width)
							max_width = width;
						if(height > max_height)
							max_height = height;
					}
				}
			}
		}
		tmp = tmp->next;
	}
	int mean_ts = min_ts_diff;	/* FIXME: was an actual mean, (max_ts_diff+min_ts_diff)/2; */
	fps = (90000/(mean_ts > 0 ? mean_ts : 30));
	printf( "  -- %dx%d (fps [%d,%d] ~ %d)\n", max_width, max_height, min_ts_diff, max_ts_diff, fps);
	if(max_width == 0 && max_height == 0) {
		printf("No key frame?? assuming 640x480...\n");
		max_width = 640;
		max_height = 480;
	}
	if(fps == 0) {
		printf("No fps?? assuming 1...\n");
		fps = 1;	/* Prevent divide by zero error */
	}
	return 0;
}

int janus_pp_webm_process(FILE *file, janus_pp_frame_packet *list, int vp8, int *working, FILE *file_audio, janus_pp_frame_packet *list_audio) {
	if(!file || !list || !working || !file_audio || !list_video)
		return -1;
	janus_pp_frame_packet *tmp = list;
        janus_pp_frame_packet *tmp_audio = list_audio;
	int bytes = 0, numBytes = max_width*max_height*3;	/* FIXME */
	uint8_t *received_frame = g_malloc0(numBytes);
	uint8_t *buffer = g_malloc0(10000), *start = buffer;
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
	while(*working && tmp != NULL && tmp_audio != NULL) {
		        
                
                if((audio == 0 && video ==0) || (audio == 1 && video == 1)) {
                    keyFrame = 0;
                    frameLen = 0;
                    len = 0;
                    audio_len = 0;
                    buf = g_malloc0(1000);
                    while(1) {
                            if(tmp->drop) {
                                    // Check if timestamp changes: marker bit is not mandatory, and may be lost as well 
                                    if(tmp->next == NULL || tmp->next->ts > tmp->ts)
                                            break;
                                    tmp = tmp->next;
                                    continue;
                            }
                            // RTP payload 
                            buffer = start;
                            fseek(file, tmp->offset+12+tmp->skip, SEEK_SET);
                            len = tmp->len-12-tmp->skip;
                            bytes = fread(buffer, sizeof(char), len, file);
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
                                                            keyframe_ts = tmp->ts;
                                                            printf("First keyframe: %"SCNu64"\n", tmp->ts-list->ts);
                                                    }
                                                    keyframe_ts = tmp->ts;
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
                            // Check if timestamp changes: marker bit is not mandatory, and may be lost as well 
                            if(tmp->next == NULL || tmp->next->ts > tmp->ts)
                                    break;
                            tmp = tmp->next;
                    }                    
                    len = 0;
                    if(tmp_audio->drop) {
                        // Check if timestamp changes: marker bit is not mandatory, and may be lost as well 
                        if(tmp_audio->next == NULL || tmp_audio->next->ts > tmp_audio->ts)
                            break;
                        tmp_audio = tmp_audio->next;
                        continue;
                    }
                    fseek(file_audio, tmp_audio->offset+12+tmp_audio->skip, SEEK_SET);
                    audio_len = tmp_audio->len-12-tmp_audio->skip;
                    bytes = fread(buf, sizeof(char), audio_len, file_audio);
                } else if(audio == 0 && video == 1) {
                        buf = g_malloc0(1000);
                        audio_len = 0;
                    /*    if(tmp_audio->drop) {
                            // Check if timestamp changes: marker bit is not mandatory, and may be lost as well 
                            if(tmp_audio->next == NULL || tmp_audio->next->ts > tmp_audio->ts)
                                break;
                            tmp_audio = tmp_audio->next;
                            continue;
                        }
                    */    fseek(file_audio, tmp_audio->offset+12+tmp_audio->skip, SEEK_SET);
                        audio_len = tmp_audio->len-12-tmp_audio->skip;
                        bytes = fread(buf, sizeof(char), audio_len, file_audio);
                } else if (audio == 1 && video == 0) {
                        keyFrame = 0;
                        frameLen = 0;
                        len = 0;
         
                        while(1) {
                            if(tmp->drop) {
                                    // Check if timestamp changes: marker bit is not mandatory, and may be lost as well 
                                    if(tmp->next == NULL || tmp->next->ts > tmp->ts)
                                            break;
                                    tmp = tmp->next;
                                    continue;
                            }
                            // RTP payload 
                            buffer = start;
                            fseek(file, tmp->offset+12+tmp->skip, SEEK_SET);
                            len = tmp->len-12-tmp->skip;
                            bytes = fread(buffer, sizeof(char), len, file);
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
                                                            keyframe_ts = tmp->ts;
                                                            printf("First keyframe: %"SCNu64"\n", tmp->ts-list->ts);
                                                    }
                                                    keyframe_ts = tmp->ts;
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
                            // Check if timestamp changes: marker bit is not mandatory, and may be lost as well 
                            if(tmp->next == NULL || tmp->next->ts > tmp->ts)
                                    break;
                            tmp = tmp->next;
                    }
                }
                
               // if(frameLen > 0) {
			video_pts = (tmp->ts-keyframe_ts)/90;
                        if(video_pts == 0 && audio_pts == 0) {
                            memset(received_frame + frameLen, 0, FF_INPUT_BUFFER_PADDING_SIZE);
                            
                            AVPacket packet;
                            av_init_packet(&packet); 
                            packet.stream_index = 0;
                            packet.data = received_frame;
                            packet.size = frameLen;
                            if(keyFrame)
				//~ packet.flags |= PKT_FLAG_KEY;
				packet.flags |= AV_PKT_FLAG_KEY;

                                // First we save to the file... 
                                //~ packet.dts = AV_NOPTS_VALUE;
                                //~ packet.pts = AV_NOPTS_VALUE;
				// printf("Error writing  to audio file 1 .. of user %" PRId64 "...\n", audio_ts);
                                packet.dts = (tmp->ts-keyframe_ts)/90;
                                packet.pts = (tmp->ts-keyframe_ts)/90;
				
                                if(fctx) {
                                        if(av_write_frame(fctx, &packet) < 0) {
                                                printf("Error writing video frame to file...\n");
                                        }
                                }
                                AVPacket packet1;
                                av_init_packet(&packet1); 
                                packet1.dts = audio_pts;
                                packet1.pts = audio_pts;
                                packet1.data = buf;
                                packet1.size = audio_len;
                                packet1.stream_index = 1;
                                if(fctx) {
                                    if(av_write_frame(fctx, &packet1) < 0) {
                                        //printf("Error writing  to audio file 1 .. of user %lu...\n", node->id);
                                        tmp_audio = tmp_audio->next;
                                        audio_ts = audio_ts + 20;
                                        g_free(buf);
                                        //continue;

                                    }    
                                }
                                
                                audio_pts = audio_pts + 20;
                                printf("Audio and video %" PRId64 " %" PRId64 "...\n", audio_pts, video_pts);
                                audio = 1;
                                video = 1;
                                tmp = tmp->next;
                                tmp_audio = tmp_audio->next;
                        } else {
                            AVPacket packet;
                            av_init_packet(&packet); 
                            if(video_pts < audio_pts) {
                                memset(received_frame + frameLen, 0, FF_INPUT_BUFFER_PADDING_SIZE);
                                
                                packet.stream_index = 0;
                                packet.data = received_frame;
                                packet.size = frameLen;
                                if(keyFrame)
                                    //~ packet.flags |= PKT_FLAG_KEY;
                                    packet.flags |= AV_PKT_FLAG_KEY;

                                // First we save to the file... 
                                //~ packet.dts = AV_NOPTS_VALUE;
                                //~ packet.pts = AV_NOPTS_VALUE;
				// printf("Error writing  to audio file 1 .. of user %" PRId64 "...\n", audio_ts);
                                packet.dts = (tmp->ts-keyframe_ts)/90;
                                packet.pts = (tmp->ts-keyframe_ts)/90;
				printf("video only %" PRId64 "  %"PRId64 "...\n", video_pts, audio_pts);
                                if(fctx) {
                                        if(av_write_frame(fctx, &packet) < 0) {
                                                printf("Error writing video frame to file...\n");
                                        }
                                }
                                tmp = tmp->next;
                                audio = 1;
                                video = 0;
                            } else if (video_pts > audio_pts){
                                
                                packet.dts = audio_pts;
                                packet.pts = audio_pts;
                                packet.data = buf;
                                packet.size = audio_len;
                                printf("Audio only %" PRId64 " %" PRId64 "...\n", audio_pts, video_pts);
                                packet.stream_index = 1;
                                if(fctx) {
                                    if(av_write_frame(fctx, &packet) < 0) {
                                        //printf("Error writing  to audio file 1 .. of user %lu...\n", node->id);

                                    }    
                                }
                                audio_pts = audio_pts + 20;
                                tmp_audio= tmp_audio->next;
                                video = 1;
                                audio = 0;
                            } else if (video_pts == audio_pts) {
                                 memset(received_frame + frameLen, 0, FF_INPUT_BUFFER_PADDING_SIZE);
                            
                            AVPacket packet;
                            av_init_packet(&packet); 
                            packet.stream_index = 0;
                            packet.data = received_frame;
                            packet.size = frameLen;
                            if(keyFrame)
				//~ packet.flags |= PKT_FLAG_KEY;
				packet.flags |= AV_PKT_FLAG_KEY;

                                // First we save to the file... 
                                //~ packet.dts = AV_NOPTS_VALUE;
                                //~ packet.pts = AV_NOPTS_VALUE;
				// printf("Error writing  to audio file 1 .. of user %" PRId64 "...\n", audio_ts);
                                packet.dts = (tmp->ts-keyframe_ts)/90;
                                packet.pts = (tmp->ts-keyframe_ts)/90;
				
                                if(fctx) {
                                        if(av_write_frame(fctx, &packet) < 0) {
                                                printf("Error writing video frame to file...\n");
                                        }
                                }
                                AVPacket packet1;
                                av_init_packet(&packet1); 
                                packet1.dts = audio_pts;
                                packet1.pts = audio_pts;
                                packet1.data = buf;
                                packet1.size = audio_len;
                                packet1.stream_index = 1;
                                if(fctx) {
                                    if(av_write_frame(fctx, &packet1) < 0) {
                                        //printf("Error writing  to audio file 1 .. of user %lu...\n", node->id);
                                        tmp_audio = tmp_audio->next;
                                        audio_ts = audio_ts + 20;
                                        g_free(buf);
                                        //continue;

                                    }    
                                }
                                
                                audio_pts = audio_pts + 20;
                                printf("Audio and video %" PRId64 " %" PRId64 "...\n", audio_pts, video_pts);
                                audio = 1;
                                video = 1;
                                tmp = tmp->next;
                                tmp_audio = tmp_audio->next;
                            }
                        }
		//}
		
	}

        /*
        while(*working && tmp_audio != NULL) {
            gchar *buf = g_malloc0(1000);
            len = 0;
            if(tmp_audio->drop) {
		// Check if timestamp changes: marker bit is not mandatory, and may be lost as well 
		if(tmp_audio->next == NULL || tmp_audio->next->ts > tmp_audio->ts)
                    break;
		tmp_audio = tmp_audio->next;
		continue;
            }
            fseek(file_audio, tmp_audio->offset+12+tmp_audio->skip, SEEK_SET);
            len = tmp_audio->len-12-tmp_audio->skip;
            bytes = fread(buf, sizeof(char), len, file_audio);
	
            AVPacket packet;
            av_init_packet(&packet); 
            packet.dts = audio_ts;
            packet.pts = audio_ts;
            packet.data = buf;
            packet.size = len;
	    printf("Error writing  to audio file 1 .. of user %" PRId64 "...\n", audio_ts);
            packet.stream_index = 1;
            if(fctx) {
                if(av_write_frame(fctx, &packet) < 0) {
                    //printf("Error writing  to audio file 1 .. of user %lu...\n", node->id);
                    tmp = tmp->next;
                    audio_ts = audio_ts + 20;
                    g_free(buf);
                    continue;
                   
                }    
            }
            audio_ts = audio_ts + 20;
            tmp_audio= tmp_audio->next;
        }
        */
	g_free(received_frame);
	g_free(start);
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

/* Main Code */
int main(int argc, char *argv[])
{

        /* Evaluate arguments */
	if(argc != 4) {
		printf( "Usage: %s source-audio.mjr source-video.mjr destination.[webm]\n", argv[0]);
		printf( "       %s --header source.mjr (only parse header)\n", argv[0]);
		printf( "       %s --parse source.mjr (only parse and re-order packets)\n", argv[0]);
		return -1;
	}
	char *source = NULL, *destination = NULL, *extension = NULL;
        char *source_video = NULL;
	gboolean header_only = !strcmp(argv[1], "--header");
	gboolean parse_only = !strcmp(argv[1], "--parse");
	if(header_only || parse_only) {
		/* Only parse the .mjr header and/or re-order the packets, no processing */
		source = argv[2];
	} else {
		/* Post-process the .mjr recording */
		source = argv[1];
                source_video = argv[2];
		destination = argv[3];
		printf("%s --> %s\n", source, destination);
		/* Check the extension */
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
	}
	FILE *file = fopen(source, "rb");
        FILE *file_video = fopen(source_video, "rb");
	if(file == NULL && file_video == NULL) {
		printf("Could not open file %s\n", source);
		return -1;
	}
	fseek(file, 0L, SEEK_END);
        fseek(file_video, 0L, SEEK_END);
	long fsize = ftell(file);
        long fsize_video = ftell(file_video);
	fseek(file, 0L, SEEK_SET);
        fseek(file_video, 0L, SEEK_SET);
	printf("File is %zu bytes\n", fsize_video);

	/* Handle SIGINT */
	working = 1;
	signal(SIGINT, janus_pp_handle_signal);

	/* Pre-parse */
	printf("Pre-parsing file to generate ordered index...\n");
	gboolean parsed_header = FALSE;
	int video = 0, data = 0;
	int opus = 0, g711 = 0, vp8 = 0, vp9 = 0, h264 = 0;
	gint64 c_time = 0, w_time = 0;
	int bytes = 0, skip = 0, skip_video = 0;
	long offset = 0;
        long offset_video = 0;
	uint16_t len = 0;
	uint32_t count = 0;
        uint32_t count_video = 0;
	char prebuffer[1500];
	memset(prebuffer, 0, 1500);
	/* Let's look for timestamp resets first */
	while(working && offset < fsize) {
		if(header_only && parsed_header) {
			/* We only needed to parse the header */
			exit(0);
		}
		/* Read frame header */
		skip = 0;
		fseek(file, offset, SEEK_SET);
		bytes = fread(prebuffer, sizeof(char), 8, file);
		if(bytes != 8 || prebuffer[0] != 'M') {
			printf("Invalid header at offset %ld (%s), the processing will stop here...\n",
				offset, bytes != 8 ? "not enough bytes" : "wrong prefix");
			break;
		}
		if(prebuffer[1] == 'E') {
			/* Either the old .mjr format header ('MEETECHO' header followed by 'audio' or 'video'), or a frame */
			offset += 8;
			bytes = fread(&len, sizeof(uint16_t), 1, file);
			len = ntohs(len);
			offset += 2;
			if(len == 5 && !parsed_header) {
				/* This is the main header */
				parsed_header = TRUE;
				printf("Old .mjr header format\n");
				bytes = fread(prebuffer, sizeof(char), 5, file);
				if(prebuffer[0] == 'v') {
					printf("This is a video recording, assuming VP8\n");
					video = 1;
					data = 0;
					vp8 = 1;
					if(extension && strcasecmp(extension, ".webm")) {
						printf("VP8 RTP packets can only be converted to a .webm file\n");
						exit(1);
					}
				} else if(prebuffer[0] == 'a') {
					printf("This is an audio recording, assuming Opus\n");
					video = 0;
					data = 0;
					opus = 1;
					if(extension && strcasecmp(extension, ".opus")) {
						printf("Opus RTP packets can only be converted to an .opus file\n");
						exit(1);
					}
				} else if(prebuffer[0] == 'd') {
					printf("This is a text data recording, assuming SRT\n");
					video = 0;
					data = 1;
					if(extension && strcasecmp(extension, ".srt")) {
						printf("Data channel packets can only be converted to a .srt file\n");
						exit(1);
					}
				} else {
					printf("Unsupported recording media type...\n");
					exit(1);
				}
				offset += len;
				continue;
			} else if(!data && len < 12) {
				/* Not RTP, skip */
				printf("Skipping packet (not RTP?)\n");
				offset += len;
				continue;
			}
		} else if(prebuffer[1] == 'J') {
			/* New .mjr format, the header may contain useful info */
			offset += 8;
			bytes = fread(&len, sizeof(uint16_t), 1, file);
			len = ntohs(len);
			offset += 2;
			if(len > 0 && !parsed_header) {
				/* This is the info header */
				printf("New .mjr header format\n");
				bytes = fread(prebuffer, sizeof(char), len, file);
				parsed_header = TRUE;
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
				if(!strcasecmp(t, "v")) {
					video = 1;
					data = 0;
				} else if(!strcasecmp(t, "a")) {
					video = 0;
					data = 0;
				} else if(!strcasecmp(t, "d")) {
					video = 0;
					data = 1;
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
			/*	if(video) {
					if(!strcasecmp(c, "vp8")) {
						vp8 = 1;
						if(extension && strcasecmp(extension, ".webm")) {
							printf("VP8 RTP packets can only be converted to a .webm file\n");
							exit(1);
						}
					} else if(!strcasecmp(c, "vp9")) {
						vp9 = 1;
						if(extension && strcasecmp(extension, ".webm")) {
							printf("VP9 RTP packets can only be converted to a .webm file\n");
							exit(1);
						}
					} else if(!strcasecmp(c, "h264")) {
						h264 = 1;
						if(extension && strcasecmp(extension, ".mp4")) {
							printf("H.264 RTP packets can only be converted to a .mp4 file\n");
							exit(1);
						}
					} else {
						printf("The post-processor only supports VP8, VP9 and H.264 video for now (was '%s')...\n", c);
						exit(1);
					}
				} else if(!video && !data) {
					if(!strcasecmp(c, "opus")) {
						opus = 1;
						if(extension && strcasecmp(extension, ".opus")) {
							printf("Opus RTP packets can only be converted to a .opus file\n");
							exit(1);
						}
					} else if(!strcasecmp(c, "g711")) {
						g711 = 1;
						if(extension && strcasecmp(extension, ".wav")) {
							printf("G.711 RTP packets can only be converted to a .wav file\n");
							exit(1);
						}
					} else {
						printf("The post-processor only supports Opus and G.711 audio for now (was '%s')...\n", c);
						exit(1);
					}
				} else if(data) {
					if(strcasecmp(c, "text")) {
						printf("The post-processor only supports text data for now (was '%s')...\n", c);
						exit(1);
					}
					if(extension && strcasecmp(extension, ".srt")) {
						printf("Data channel packets can only be converted to a .srt file\n");
						exit(1);
					}
				}
				/* When was the file created? */
				json_t *created = json_object_get(info, "s");
				if(!created || !json_is_integer(created)) {
					printf("Missing recording created time in info header...\n");
					exit(1);
				}
				c_time = json_integer_value(created);
				/* When was the first frame written? */
				json_t *written = json_object_get(info, "u");
				if(!written || !json_is_integer(written)) {
					printf("Missing recording written time in info header...\n");
					exit(1);
				}
				w_time = json_integer_value(written);
				/* Summary */
				printf("This is %s recording:\n", video ? "a video" : (data ? "a text data" : "an audio"));
				printf("  -- Codec:   %s\n", c);
				printf("  -- Created: %"SCNi64"\n", c_time);
				printf("  -- Written: %"SCNi64"\n", w_time);
			}
		} else {
			printf("Invalid header...\n");
			exit(1);
		}
		/* Skip data for now */
		offset += len;
	}
        while(working && offset_video < fsize_video) {
		if(header_only && parsed_header) {
			/* We only needed to parse the header */
			exit(0);
		}
		/* Read frame header */
		skip_video = 0;
		fseek(file_video, offset_video, SEEK_SET);
		bytes = fread(prebuffer, sizeof(char), 8, file_video);
		if(bytes != 8 || prebuffer[0] != 'M') {
			printf("Invalid header at offset %ld (%s), the processing will stop here...\n",
				offset_video, bytes != 8 ? "not enough bytes" : "wrong prefix");
			break;
		}
		if(prebuffer[1] == 'E') {
			/* Either the old .mjr format header ('MEETECHO' header followed by 'audio' or 'video'), or a frame */
			offset_video += 8;
			bytes = fread(&len, sizeof(uint16_t), 1, file_video);
			len = ntohs(len);
			offset_video += 2;
			if(len == 5 && !parsed_header) {
				/* This is the main header */
				parsed_header = TRUE;
				printf("Old .mjr header format\n");
				bytes = fread(prebuffer, sizeof(char), 5, file_video);
				if(prebuffer[0] == 'v') {
					printf("This is a video recording, assuming VP8\n");
					video = 1;
					data = 0;
					vp8 = 1;
					if(extension && strcasecmp(extension, ".webm")) {
						printf("VP8 RTP packets can only be converted to a .webm file\n");
						exit(1);
					}
				} else if(prebuffer[0] == 'a') {
					printf("This is an audio recording, assuming Opus\n");
					video = 0;
					data = 0;
					opus = 1;
					if(extension && strcasecmp(extension, ".opus")) {
						printf("Opus RTP packets can only be converted to an .opus file\n");
						exit(1);
					}
				} else if(prebuffer[0] == 'd') {
					printf("This is a text data recording, assuming SRT\n");
					video = 0;
					data = 1;
					if(extension && strcasecmp(extension, ".srt")) {
						printf("Data channel packets can only be converted to a .srt file\n");
						exit(1);
					}
				} else {
					printf("Unsupported recording media type...\n");
					exit(1);
				}
				offset_video += len;
				continue;
			} else if(!data && len < 12) {
				/* Not RTP, skip */
				printf("Skipping packet (not RTP?)\n");
				offset_video += len;
				continue;
			}
		} else if(prebuffer[1] == 'J') {
			/* New .mjr format, the header may contain useful info */
			offset_video += 8;
			bytes = fread(&len, sizeof(uint16_t), 1, file_video);
			len = ntohs(len);
			offset_video += 2;
			if(len > 0 && !parsed_header) {
				/* This is the info header */
				printf("New .mjr header format\n");
				bytes = fread(prebuffer, sizeof(char), len, file_video);
				parsed_header = TRUE;
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
				if(!strcasecmp(t, "v")) {
					video = 1;
					data = 0;
				} else if(!strcasecmp(t, "a")) {
					video = 0;
					data = 0;
				} else if(!strcasecmp(t, "d")) {
					video = 0;
					data = 1;
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
				if(video) {
					if(!strcasecmp(c, "vp8")) {
						vp8 = 1;
						if(extension && strcasecmp(extension, ".webm")) {
							printf("VP8 RTP packets can only be converted to a .webm file\n");
							exit(1);
						}
					} else if(!strcasecmp(c, "vp9")) {
						vp9 = 1;
						if(extension && strcasecmp(extension, ".webm")) {
							printf("VP9 RTP packets can only be converted to a .webm file\n");
							exit(1);
						}
					} else if(!strcasecmp(c, "h264")) {
						h264 = 1;
						if(extension && strcasecmp(extension, ".mp4")) {
							printf("H.264 RTP packets can only be converted to a .mp4 file\n");
							exit(1);
						}
					} else {
						printf("The post-processor only supports VP8, VP9 and H.264 video for now (was '%s')...\n", c);
						exit(1);
					}
				} else if(!video && !data) {
					if(!strcasecmp(c, "opus")) {
						opus = 1;
						if(extension && strcasecmp(extension, ".opus")) {
							printf("Opus RTP packets can only be converted to a .opus file\n");
							exit(1);
						}
					} else if(!strcasecmp(c, "g711")) {
						g711 = 1;
						if(extension && strcasecmp(extension, ".wav")) {
							printf("G.711 RTP packets can only be converted to a .wav file\n");
							exit(1);
						}
					} else {
						printf("The post-processor only supports Opus and G.711 audio for now (was '%s')...\n", c);
						exit(1);
					}
				} else if(data) {
					if(strcasecmp(c, "text")) {
						printf("The post-processor only supports text data for now (was '%s')...\n", c);
						exit(1);
					}
					if(extension && strcasecmp(extension, ".srt")) {
						printf("Data channel packets can only be converted to a .srt file\n");
						exit(1);
					}
				}
				/* When was the file created? */
				json_t *created = json_object_get(info, "s");
				if(!created || !json_is_integer(created)) {
					printf("Missing recording created time in info header...\n");
					exit(1);
				}
				c_time = json_integer_value(created);
				/* When was the first frame written? */
				json_t *written = json_object_get(info, "u");
				if(!written || !json_is_integer(written)) {
					printf("Missing recording written time in info header...\n");
					exit(1);
				}
				w_time = json_integer_value(written);
				/* Summary */
				printf("This is %s recording:\n", video ? "a video" : (data ? "a text data" : "an audio"));
				printf( "  -- Codec:   %s\n", c);
				printf("  -- Created: %"SCNi64"\n", c_time);
				printf( "  -- Written: %"SCNi64"\n", w_time);
			}
		} else {
			printf("Invalid header...\n");
			exit(1);
		}
		/* Skip data for now */
		offset_video += len;
	}
	if(!working)
		exit(0);
	/* Now let's parse the frames and order them */
	uint32_t last_ts = 0, reset = 0;
	int times_resetted = 0;
	uint32_t post_reset_pkts = 0;
	offset = 0;
        offset_video = 0;
	/* Timestamp reset related stuff */
	last_ts = 0;
	reset = 0;
	times_resetted = 0;
	post_reset_pkts = 0;
	uint64_t max32 = UINT32_MAX;
	/* Start loop */
	while(working && offset < fsize) {
		/* Read frame header */
		skip = 0;
		fseek(file, offset, SEEK_SET);
		bytes = fread(prebuffer, sizeof(char), 8, file);
		if(bytes != 8 || prebuffer[0] != 'M') {
			/* Broken packet? Stop here */
			break;
		}
		prebuffer[8] = '\0';
		printf("Header: %s\n", prebuffer);
		offset += 8;
		bytes = fread(&len, sizeof(uint16_t), 1, file);
		len = ntohs(len);
		printf("  -- Length: %"SCNu16"\n", len);
		offset += 2;
		if(prebuffer[1] == 'J' || (!data && len < 12)) {
			/* Not RTP, skip */
			printf("  -- Not RTP, skipping\n");
			offset += len;
			continue;
		}
		if(!data && len > 2000) {
			/* Way too large, very likely not RTP, skip */
			printf("  -- Too large packet (%d bytes), skipping\n", len);
			offset += len;
			continue;
		}
		if(data) {
			/* Things are simpler for data, no reordering is needed: start by the data time */
			gint64 when = 0;
			bytes = fread(&when, sizeof(gint64), 1, file);
			when = ntohll(when);
			offset += sizeof(gint64);
			len -= sizeof(gint64);
			/* Generate frame packet and insert in the ordered list */
			janus_pp_frame_packet *p = g_malloc0(sizeof(janus_pp_frame_packet));
			if(p == NULL) {
				printf("Memory error!\n");
				return -1;
			}
			/* We "abuse" the timestamp field for the timing info */
			p->ts = when-c_time;
			p->len = len;
			p->drop = 0;
			p->offset = offset;
			p->skip = 0;
			p->next = NULL;
			p->prev = NULL;
			if(list == NULL) {
				list = p;
			} else {
				last->next = p;
			}
			last = p;
			/* Done */
			offset += len;
			continue;
		}
		/* Only read RTP header */
		bytes = fread(prebuffer, sizeof(char), 16, file);
		janus_pp_rtp_header *rtp = (janus_pp_rtp_header *)prebuffer;
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
		janus_pp_frame_packet *p = g_malloc0(sizeof(janus_pp_frame_packet));
		if(p == NULL) {
			printf("Memory error!\n");
			return -1;
		}
		p->seq = ntohs(rtp->seq_number);
		p->pt = rtp->type;
		/* Due to resets, we need to mess a bit with the original timestamps */
		if(last_ts == 0) {
			/* Simple enough... */
			p->ts = ntohl(rtp->timestamp);
		} else {
			/* Is the new timestamp smaller than the next one, and if so, is it a timestamp reset or simply out of order? */
			gboolean late_pkt = FALSE;
			if(ntohl(rtp->timestamp) < last_ts && (last_ts-ntohl(rtp->timestamp) > 2*1000*1000*1000)) {
				if(post_reset_pkts > 1000) {
					reset = ntohl(rtp->timestamp);
					printf("Timestamp reset: %"SCNu32"\n", reset);
					times_resetted++;
					post_reset_pkts = 0;
				}
			} else if(ntohl(rtp->timestamp) > reset && ntohl(rtp->timestamp) > last_ts &&
					(ntohl(rtp->timestamp)-last_ts > 2*1000*1000*1000)) {
				if(post_reset_pkts < 1000) {
					printf("Late pre-reset packet after a timestamp reset: %"SCNu32"\n", ntohl(rtp->timestamp));
					late_pkt = TRUE;
					times_resetted--;
				}
			} else if(ntohl(rtp->timestamp) < reset) {
				if(post_reset_pkts < 1000) {
					printf("Updating latest timestamp reset: %"SCNu32" (was %"SCNu32")\n", ntohl(rtp->timestamp), reset);
					reset = ntohl(rtp->timestamp);
				} else {
					reset = ntohl(rtp->timestamp);
					printf("Timestamp reset: %"SCNu32"\n", reset);
					times_resetted++;
					post_reset_pkts = 0;
				}
			}
			/* Take into account the number of resets when setting the internal, 64-bit, timestamp */
			p->ts = (times_resetted*max32)+ntohl(rtp->timestamp);
			if(late_pkt)
				times_resetted++;
		}
		p->len = len;
		p->drop = 0;
		if(rtp->padding) {
			/* There's padding data, let's check the last byte to see how much data we should skip */
			fseek(file, offset + len - 1, SEEK_SET);
			bytes = fread(prebuffer, sizeof(char), 1, file);
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
		last_ts = ntohl(rtp->timestamp);
		post_reset_pkts++;
		/* Fill in the rest of the details */
		p->offset = offset;
		p->skip = skip;
		p->next = NULL;
		p->prev = NULL;
		if(list == NULL) {
			/* First element becomes the list itself (and the last item), at least for now */
			list = p;
			last = p;
		} else {
			/* Check where we should insert this, starting from the end */
			int added = 0;
			janus_pp_frame_packet *tmp = last;
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
						last = p;
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
							last = p;
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
							last = p;
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
				p->next = list;
				list->prev = p;
				list = p;
			}
		}
		/* Skip data for now */
		offset += len;
		count++;
	}
        if(!working)
            exit(0);
        
        last_ts = 0;
	reset = 0;
	times_resetted = 0;
	post_reset_pkts = 0;
        while(working && offset_video < fsize_video) {
		/* Read frame header */
		skip_video = 0;
		fseek(file_video, offset_video, SEEK_SET);
		bytes = fread(prebuffer, sizeof(char), 8, file_video);
		if(bytes != 8 || prebuffer[0] != 'M') {
			/* Broken packet? Stop here */
			break;
		}
		prebuffer[8] = '\0';
		printf("Header: %s\n", prebuffer);
		offset_video += 8;
		bytes = fread(&len, sizeof(uint16_t), 1, file_video);
		len = ntohs(len);
		printf("  -- Length: %"SCNu16"\n", len);
		offset_video += 2;
		if(prebuffer[1] == 'J' || (!data && len < 12)) {
			/* Not RTP, skip */
			printf("  -- Not RTP, skipping\n");
			offset_video += len;
			continue;
		}
		if(!data && len > 2000) {
			/* Way too large, very likely not RTP, skip */
			printf("  -- Too large packet (%d bytes), skipping\n", len);
			offset_video += len;
			continue;
		}
		if(data) {
			/* Things are simpler for data, no reordering is needed: start by the data time */
			gint64 when = 0;
			bytes = fread(&when, sizeof(gint64), 1, file_video);
			when = ntohll(when);
			offset_video += sizeof(gint64);
			len -= sizeof(gint64);
			/* Generate frame packet and insert in the ordered list */
			janus_pp_frame_packet *p = g_malloc0(sizeof(janus_pp_frame_packet));
			if(p == NULL) {
				printf("Memory error!\n");
				return -1;
			}
			/* We "abuse" the timestamp field for the timing info */
			p->ts = when-c_time;
			p->len = len;
			p->drop = 0;
			p->offset = offset_video;
			p->skip = 0;
			p->next = NULL;
			p->prev = NULL;
			if(list_video == NULL) {
				list_video = p;
			} else {
				last_video->next = p;
			}
			last_video = p;
			/* Done */
			offset_video += len;
			continue;
		}
		/* Only read RTP header */
		bytes = fread(prebuffer, sizeof(char), 16, file_video);
		janus_pp_rtp_header *rtp = (janus_pp_rtp_header *)prebuffer;
		printf("  -- RTP packet (ssrc=%"SCNu32", pt=%"SCNu16", ext=%"SCNu16", seq=%"SCNu16", ts=%"SCNu32")\n",
				ntohl(rtp->ssrc), rtp->type, rtp->extension, ntohs(rtp->seq_number), ntohl(rtp->timestamp));
		if(rtp->csrccount) {
			printf("  -- -- Skipping CSRC list\n");
			skip_video += rtp->csrccount*4;
		}
		if(rtp->extension) {
			janus_pp_rtp_header_extension *ext = (janus_pp_rtp_header_extension *)(prebuffer+12);
			printf("  -- -- RTP extension (type=%"SCNu16", length=%"SCNu16")\n",
				ntohs(ext->type), ntohs(ext->length));
			skip_video += 4 + ntohs(ext->length)*4;
		}
		/* Generate frame packet and insert in the ordered list */
		janus_pp_frame_packet *p = g_malloc0(sizeof(janus_pp_frame_packet));
		if(p == NULL) {
			printf("Memory error!\n");
			return -1;
		}
		p->seq = ntohs(rtp->seq_number);
		p->pt = rtp->type;
		/* Due to resets, we need to mess a bit with the original timestamps */
		if(last_ts == 0) {
			/* Simple enough... */
			p->ts = ntohl(rtp->timestamp);
		} else {
			/* Is the new timestamp smaller than the next one, and if so, is it a timestamp reset or simply out of order? */
			gboolean late_pkt = FALSE;
			if(ntohl(rtp->timestamp) < last_ts && (last_ts-ntohl(rtp->timestamp) > 2*1000*1000*1000)) {
				if(post_reset_pkts > 1000) {
					reset = ntohl(rtp->timestamp);
					printf("Timestamp reset: %"SCNu32"\n", reset);
					times_resetted++;
					post_reset_pkts = 0;
				}
			} else if(ntohl(rtp->timestamp) > reset && ntohl(rtp->timestamp) > last_ts &&
					(ntohl(rtp->timestamp)-last_ts > 2*1000*1000*1000)) {
				if(post_reset_pkts < 1000) {
					printf("Late pre-reset packet after a timestamp reset: %"SCNu32"\n", ntohl(rtp->timestamp));
					late_pkt = TRUE;
					times_resetted--;
				}
			} else if(ntohl(rtp->timestamp) < reset) {
				if(post_reset_pkts < 1000) {
					printf("Updating latest timestamp reset: %"SCNu32" (was %"SCNu32")\n", ntohl(rtp->timestamp), reset);
					reset = ntohl(rtp->timestamp);
				} else {
					reset = ntohl(rtp->timestamp);
					printf("Timestamp reset: %"SCNu32"\n", reset);
					times_resetted++;
					post_reset_pkts = 0;
				}
			}
			/* Take into account the number of resets when setting the internal, 64-bit, timestamp */
			p->ts = (times_resetted*max32)+ntohl(rtp->timestamp);
			if(late_pkt)
				times_resetted++;
		}
		p->len = len;
		p->drop = 0;
		if(rtp->padding) {
			/* There's padding data, let's check the last byte to see how much data we should skip */
			fseek(file_video, offset_video + len - 1, SEEK_SET);
			bytes = fread(prebuffer, sizeof(char), 1, file_video);
			uint8_t padlen = (uint8_t)prebuffer[0];
			printf("Padding at sequence number %hu: %d/%d\n",
				ntohs(rtp->seq_number), padlen, p->len);
			p->len -= padlen;
			if((p->len - skip_video - 12) <= 0) {
				/* Only padding, take note that we should drop the packet later */
				p->drop = 1;
				printf("  -- All padding, marking packet as dropped\n");
			}
		}
		last_ts = ntohl(rtp->timestamp);
		post_reset_pkts++;
		/* Fill in the rest of the details */
		p->offset = offset_video;
		p->skip = skip_video;
		p->next = NULL;
		p->prev = NULL;
		if(list_video == NULL) {
			/* First element becomes the list itself (and the last item), at least for now */
			list_video = p;
			last_video = p;
		} else {
			/* Check where we should insert this, starting from the end */
			int added = 0;
			janus_pp_frame_packet *tmp = last_video;
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
						last_video = p;
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
							last_video = p;
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
							last_video = p;
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
				p->next = list_video;
				list_video->prev = p;
				list_video = p;
			}
		}
		/* Skip data for now */
		offset_video += len;
		count_video++;
	}
        
	if(!working)
		exit(0);
	printf("Counted video file %"SCNu32" RTP packets\n", count_video);
	printf("Counted audio file %"SCNu32" RTP packets\n", count);
	janus_pp_frame_packet *tmp = list;
        janus_pp_frame_packet *tmp_video = list_video;
	count = 0;
        count_video = 0;
	while(tmp) {
		count++;
		if(!data)
			printf("[%10lu][%4d] seq=%"SCNu16", ts=%"SCNu64", time=%"SCNu64"s\n", tmp->offset, tmp->len, tmp->seq, tmp->ts, (tmp->ts-list->ts)/90000);
		else
			printf("[%10lu][%4d] time=%"SCNu64"s\n", tmp->offset, tmp->len, tmp->ts);
		tmp = tmp->next;
	}
	printf("Counted %"SCNu32" frame packets in audio file\n", count);
        
        while(tmp_video) {
		count_video++;
		if(!data)
			printf("[%10lu][%4d] seq=%"SCNu16", ts=%"SCNu64", time=%"SCNu64"s\n", tmp_video->offset, tmp_video->len, tmp_video->seq, tmp_video->ts, (tmp_video->ts-list_video->ts)/90000);
		else
			printf("[%10lu][%4d] time=%"SCNu64"s\n", tmp_video->offset, tmp_video->len, tmp_video->ts);
		tmp_video = tmp_video->next;
	}
	printf("Counted %"SCNu32" frame packets in video\n", count_video);

        if(janus_pp_webm_preprocess(file_video, list_video, vp8) < 0) {
		printf("Error pre-processing %s RTP frames...\n", vp8 ? "VP8" : "VP9");
				exit(1);
        }
	if(janus_pp_webm_create(destination, vp8) < 0) {
            printf("Error creating .webm file...\n");
            exit(1);
	}
        if(janus_pp_webm_process(file_video, list_video, vp8, &working, file, list) < 0) {
            printf("Error processing %s RTP frames...\n", vp8 ? "VP8" : "VP9");
	}
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

	printf("Bye!\n");
	return 0;
}


