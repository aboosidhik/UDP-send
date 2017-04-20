//g++ -ggdb `pkg-config --cflags --libs opencv` video_big_small_three.cpp -o video_big_small_three
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
    cv::VideoCapture cap1("in1.flv");
    cv::VideoCapture cap2("in3.flv");
    if(!cap.isOpened() || !cap1.isOpened() || !cap2.isOpened()) {
        std::cout << "Unable to open one of the files\n";
        std::exit(-1);
    }
    // Get the width/height and the FPS of the vide
    int width = static_cast<int>(cap.get(CV_CAP_PROP_FRAME_WIDTH));
    int height = static_cast<int>(cap.get(CV_CAP_PROP_FRAME_HEIGHT));
    double FPS = cap.get(CV_CAP_PROP_FPS);
    
    int width1 = static_cast<int>(cap1.get(CV_CAP_PROP_FRAME_WIDTH));
    int height1 = static_cast<int>(cap1.get(CV_CAP_PROP_FRAME_HEIGHT));
    double FPS1 = cap1.get(CV_CAP_PROP_FPS);

    int width2 = static_cast<int>(cap2.get(CV_CAP_PROP_FRAME_WIDTH));
    int height2 = static_cast<int>(cap2.get(CV_CAP_PROP_FRAME_HEIGHT));
    double FPS2 = cap2.get(CV_CAP_PROP_FPS);
    // Open a video file for writing (the MP4V codec works on OS X and Windows)
    cv::VideoWriter out("output1.mov", CV_FOURCC('m','p', '4', 'v'), FPS, cv::Size(width, height));
    if(!out.isOpened()) {
         std::cout <<"Error! Unable to open video file for output." << std::endl;
        std::exit(-1);
    }
    cv::Mat image;
    cv::Mat image1;
    cv::Mat image2;
    cv::Mat panel_middle, panel_right;
    int delta = width/3;
    while(true) {
        cap >> image;
        cap1 >> image1;
        cap2 >> image2;
        if(image.empty() || image1.empty() || image2.empty()) {
            std::cout << "Can't read frames from one of your files\n";
            break;
        }
        Mat image_re(image.rows,image.cols, CV_8UC3);
        cuda::GpuMat d_src(image);
        cuda::GpuMat d_src1(image1);
        cuda::GpuMat d_src2(image2);
        cuda::GpuMat d_dst1;
        cuda::GpuMat d_dst2;
        cuda::GpuMat d_dst_big;
        cuda::GpuMat d_src_image(image_re);
        cuda::resize(d_src1, d_dst1, Size(image1.cols/2,image1.rows/2), 0, 0, INTER_CUBIC);  
        cuda::resize(d_src2, d_dst2, Size(image2.cols/2,image2.rows/2), 0, 0, INTER_CUBIC);  
        cuda::resize(d_src, d_dst_big, Size(image.cols/2,image.rows), 0, 0, INTER_CUBIC);  
        d_dst1.copyTo(d_src_image(cv::Rect(320,0,d_dst1.cols, d_dst1.rows)));
        d_dst_big.copyTo(d_src_image(cv::Rect(0,0,d_dst_big.cols, d_dst_big.rows)));
        d_dst2.copyTo(d_src_image(cv::Rect(320,180,d_dst2.cols, d_dst2.rows)));
        d_src_image.download(image_re);
        // Save frame to video
        out << image_re;
        // Stop the camera if the user presses the "ESC" key
        if(cv::waitKey(1000.0/FPS) == 27) break;
    }
    return 0;
 }

