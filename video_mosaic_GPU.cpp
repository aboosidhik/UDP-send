#include <iostream>
#include <opencv2/opencv.hpp>
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

int main() {
    // Open a video file:
    cv::VideoCapture cap("in2.flv");
    if(!cap.isOpened()) {
        std::cout << "Unable to open the camera\n";
        std::exit(-1);
    }
    // Get the width/height and the FPS of the vide
    int width = static_cast<int>(cap.get(CV_CAP_PROP_FRAME_WIDTH));
    int height = static_cast<int>(cap.get(CV_CAP_PROP_FRAME_HEIGHT));
    double FPS = cap.get(CV_CAP_PROP_FPS);
    // Open a video file for writing (the MP4V codec works on OS X and Windows)
    cv::VideoWriter out("output1.mov", CV_FOURCC('m','p', '4', 'v'), FPS, cv::Size(width, height));
    if(!out.isOpened()) {
         std::cout <<"Error! Unable to open video file for output." << std::endl;
        std::exit(-1);
    }
    cv::Mat image;
    cv::Mat panel_middle, panel_right;
    int delta = width/3;
    while(true) {
        cap >> image;
        if(image.empty()) {
            std::cout << "Can't read frames from your camera\n";
            break;
        }
        Mat image_re(image.rows,image.cols, CV_8UC3);
        cuda::GpuMat d_src(image);
        cuda::GpuMat d_dst;
        cuda::GpuMat d_src_image(image_re);
        cuda::resize(d_src, d_dst, Size(image.cols/2,image.rows/2), 0, 0, INTER_CUBIC);  
        d_dst.copyTo(d_src_image(cv::Rect(320,0,d_dst.cols, d_dst.rows)));
        d_dst.copyTo(d_src_image(cv::Rect(0,0,d_dst.cols, d_dst.rows)));
        d_dst.copyTo(d_src_image(cv::Rect(0,180,d_dst.cols, d_dst.rows)));
        d_dst.copyTo(d_src_image(cv::Rect(320,180,d_dst.cols, d_dst.rows)));
        d_src_image.download(image_re);
        // Save frame to video
        out << image_re;
        // Stop the camera if the user presses the "ESC" key
        if(cv::waitKey(1000.0/FPS) == 27) break;
    }
    return 0;
 }

