#include <iostream>
#include <vector>
// FFmpeg
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}
// OpenCV
#include <opencv2/opencv.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/core/cuda.hpp>
#include <opencv2/cudaimgproc.hpp>
using namespace std;
using namespace cv;
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

int main(int argc, char* argv[])
{
    if (argc < 2) {
        std::cout << "Usage: cv2ff <outfile>" << std::endl;
        return 1;
    }
    const char* outfile = argv[1];

    // initialize FFmpeg library
    av_register_all();
//  av_log_set_level(AV_LOG_DEBUG);
    int ret;

    const int dst_width = 1920;
    const int dst_height = 1080;
    const AVRational dst_fps = {30, 1};

    // initialize OpenCV capture as input frame generator
  /*cv::VideoCapture cvcap("in2.flv");
    if (!cvcap.isOpened()) {
        std::cerr << "fail to open cv::VideoCapture";
        return 2;
    }
    cvcap.set(cv::CAP_PROP_FRAME_WIDTH, dst_width);
    cvcap.set(cv::CAP_PROP_FRAME_HEIGHT, dst_height);
  */const std::string fname("new1.mkv");
    cv::cuda::GpuMat d_frame;
    cv::Ptr<cv::cudacodec::VideoReader> d_reader = cv::cudacodec::createVideoReader(fname);
    
     // allocate cv::Mat with extra bytes (required by AVFrame::data)
    std::vector<uint8_t> imgbuf(dst_height * dst_width * 3 + 16);
    cv::Mat image(dst_height, dst_width, CV_8UC3, imgbuf.data(), dst_width * 3);

    // open output format context
    AVFormatContext* outctx = 0;
    ret = avformat_alloc_output_context2(&outctx, 0, 0, outfile);
    if (ret < 0) {
        std::cerr << "fail to avformat_alloc_output_context2(" << outfile << "): ret=" << ret;
        return 2;
    }

    // open output IO context
    ret = avio_open2(&outctx->pb, outfile, AVIO_FLAG_WRITE, 0, 0);
    if (ret < 0) {
        std::cerr << "fail to avio_open2: ret=" << ret;
        return 2;
    }

    // create new video stream
    AVCodec* vcodec = avcodec_find_encoder(outctx->oformat->video_codec);
    //AVCodec* vcodec = avcodec_find_decoder_by_name("h264_nvenc");
    AVStream* vstrm = avformat_new_stream(outctx, vcodec);
  //  std::cout<< "name: " << vcodec->name << "\n";
    if (!vstrm) {
        std::cerr << "fail to avformat_new_stream";
        return 2;
    }
    avcodec_get_context_defaults3(vstrm->codec, vcodec);
    //avcodec_get_context_defaults3(vstrm->codec, avcodec_find_decoder_by_name("h264_nvenc"));
    vstrm->codec->width = dst_width;
    vstrm->codec->height = dst_height;
    vstrm->codec->pix_fmt = vcodec->pix_fmts[0];
    vstrm->codec->time_base = vstrm->time_base = av_inv_q(dst_fps);
    vstrm->r_frame_rate = vstrm->avg_frame_rate = dst_fps;
    if (outctx->oformat->flags & AVFMT_GLOBALHEADER)
        vstrm->codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    // open video encoder
    ret = avcodec_open2(vstrm->codec, vcodec, 0);
    //ret = avcodec_open2(vstrm->codec,avcodec_find_decoder_by_name("h264_nvenc"), 0);
    if (ret < 0) {
        std::cerr << "fail to avcodec_open2: ret=" << ret;
        return 2;
    }

    std::cout
        << "outfile: " << outfile << "\n"
        << "format:  " << outctx->oformat->name << "\n"
        << "vcodec:  " << vcodec->name << "\n"
        << "size:    " << dst_width << 'x' << dst_height << "\n"
        << "fps:     " << av_q2d(dst_fps) << "\n"
        << "pixfmt:  " << av_get_pix_fmt_name(vstrm->codec->pix_fmt) << "\n"
        << std::flush;

    // initialize sample scaler
    SwsContext* swsctx = sws_getCachedContext(
        0, dst_width, dst_height, AV_PIX_FMT_BGR24,
        dst_width, dst_height, vstrm->codec->pix_fmt, SWS_BICUBIC, 0, 0, 0);
    if (!swsctx) {
        std::cerr << "fail to sws_getCachedContext";
        return 2;
    }

    // allocate frame buffer for encoding
    AVFrame* frame = av_frame_alloc();
    std::vector<uint8_t> framebuf(avpicture_get_size(vstrm->codec->pix_fmt, dst_width, dst_height));
    avpicture_fill(reinterpret_cast<AVPicture*>(frame), framebuf.data(), vstrm->codec->pix_fmt, dst_width, dst_height);
    frame->width = dst_width;
    frame->height = dst_height;
    frame->format = static_cast<int>(vstrm->codec->pix_fmt);

    // encoding loop
    avformat_write_header(outctx, 0);
    int64_t frame_pts = 0;
    unsigned nb_frames = 0;
    bool end_of_stream = false;
    int got_pkt = 0;
    do {
	Mat image_re(1080,1920, CV_8UC3);  
      if (!end_of_stream) {
//            Mat image_re(1080,1920, CV_8UC3);
            // retrieve source image
            if (!d_reader->nextFrame(d_frame)) {
                printf("hhh %dx%d \n", image_re.cols, image_re.rows );
                break;
            }          
            if (cv::waitKey(33) == 0x1b)
                end_of_stream = true;
            cuda::GpuMat d_dst_big;
            cuda::GpuMat d_dst1;
            cuda::GpuMat d_src_image(image_re);
            cuda::GpuMat d_src_image1;
            cuda::GpuMat d_1;
            printf(" %dx%d \n", image_re.cols, image_re.rows );
            //cuda::resize(d_frame1, d_dst1, Size(960,540), 0, 0, INTER_CUBIC);
            cuda::resize(d_frame, d_dst1, Size(1920/2,1080/2), 0, 0, INTER_CUBIC);
            cuda::resize(d_frame, d_dst_big, Size(960,1080), 0, 0, INTER_CUBIC);
            string ty =  type2str( d_dst_big.type() );
            printf("Matrix: %s %dx%d \n", ty.c_str(), d_dst_big.cols, d_dst_big.rows );
            cuda::cvtColor(d_dst1, d_1, CV_BGRA2BGR);
            d_1.copyTo(d_src_image(cv::Rect(960,0,d_1.cols, d_1.rows)));
            cuda::cvtColor(d_dst_big, d_src_image1, CV_BGRA2BGR);
            d_src_image1.copyTo(d_src_image(cv::Rect(0,0,d_src_image1.cols, d_src_image1.rows)));
            d_1.copyTo(d_src_image(cv::Rect(960,540,d_1.cols, d_1.rows)));
           // cuda::flip(d_src_image,d_src_image,1);
            d_src_image.download(image_re);
        }
 
       if (!end_of_stream) {
            // convert cv::Mat(OpenCV) to AVFrame(FFmpeg)
            const int stride[] = { static_cast<int>(image_re.step[0]) };
            sws_scale(swsctx, &image_re.data, stride, 0, image_re.rows, frame->data, frame->linesize);
            frame->pts = frame_pts++;
        }
        // encode video frame
        AVPacket pkt;
        pkt.data = 0;
        pkt.size = 0;
        av_init_packet(&pkt);
        ret = avcodec_encode_video2(vstrm->codec, &pkt, end_of_stream ? 0 : frame, &got_pkt);
        if (ret < 0) {
            std::cerr << "fail to avcodec_encode_video2: ret=" << ret << "\n";
            break;
        }
        if (got_pkt) {
            // rescale packet timestamp
            pkt.duration = 1;
            av_packet_rescale_ts(&pkt, vstrm->codec->time_base, vstrm->time_base);
            // write packet
            av_write_frame(outctx, &pkt);
            std::cout << nb_frames << '\r' << std::flush;  // dump progress
            ++nb_frames;
        }
        av_free_packet(&pkt);
    } while (!end_of_stream || got_pkt);
    av_write_trailer(outctx);
    std::cout << nb_frames << " frames encoded" << std::endl;

    av_frame_free(&frame);
    avcodec_close(vstrm->codec);
    avio_close(outctx->pb);
    avformat_free_context(outctx);
    return 0;
}

