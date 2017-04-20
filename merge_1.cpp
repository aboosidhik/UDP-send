
#include <opencv2/opencv.hpp>
#include <iostream>
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

int main()
{
    // Load images
    Mat img2 = imread("img11.png");
    Mat img1 = imread("img22.jpg");
    // Get dimension of final image
    //  int rows = max(img1.rows,  img2.rows);
    int rows = img2.rows * 2;
    int cols = img2.cols*4;
    Mat image( rows,cols, CV_8UC3);
    string ty =  type2str( img2.type() ); 
    printf("Matrix: %s %dx%d \n", ty.c_str(), img2.cols, img2.rows );
    printf("Matrix: %s %dx%d \n", ty.c_str(), img1.cols, img1.rows );
    printf("Matrix: %s %dx%d \n", ty.c_str(), image.cols, image.rows ); 
    cuda::GpuMat d_src(img1);
    cuda::GpuMat d_dst;
    cuda::resize(d_src, d_dst, Size(img2.cols*2, rows), 0, 0, INTER_CUBIC);
    cuda::GpuMat d_src_image(image);
    d_dst.copyTo(d_src_image(cv::Rect(0,0,d_dst.cols, d_dst.rows)));
    cuda::GpuMat small_image_2(img2);
    small_image_2.copyTo(d_src_image(cv::Rect(d_dst.cols,0,small_image_2.cols, small_image_2.rows)));
    small_image_2.copyTo(d_src_image(cv::Rect(d_dst.cols+img2.cols,0,small_image_2.cols, small_image_2.rows)));
    small_image_2.copyTo(d_src_image(cv::Rect(d_dst.cols,img2.rows,small_image_2.cols, small_image_2.rows)));
    small_image_2.copyTo(d_src_image(cv::Rect(d_dst.cols+img2.cols,img2.rows,small_image_2.cols, small_image_2.rows)));
    Mat dd4;
    d_src_image.download(dd4);
    imwrite("result1.jpg", dd4);
    waitKey();
    return 0;
}
