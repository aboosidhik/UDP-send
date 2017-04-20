
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
    // Mat image( img2.rows*2,img2.cols*2, CV_8UC1, Scalar(0,0,0));
    Mat image( rows,cols, CV_8UC3);
    string ty =  type2str( img2.type() ); 
    printf("Matrix: %s %dx%d \n", ty.c_str(), img2.cols, img2.rows );
    printf("Matrix: %s %dx%d \n", ty.c_str(), img1.cols, img1.rows );
    printf("Matrix: %s %dx%d \n", ty.c_str(), image.cols, image.rows ); 
    // cout << "The value you entered is " << img1.type();
    // Create a black image
    // Mat3b res(rows, cols, Vec3b(0,0,0));
    //Mat res(rows, cols, Vec3b(0,0,0));
    // Copy images in correct position
    cuda::GpuMat d_src(img1);
    cuda::GpuMat d_dst;
    //cuda::bilateralFilter(d_src, d_dst, -1, 50, 7);
    //Mat dst;
    //d_dst.download(dst);
    //int i;
    //for (i = 0; i < 10; i++) 
    cuda::resize(d_src, d_dst, Size(img2.cols*2, rows), 0, 0, INTER_CUBIC);
    Mat dst;
    d_dst.download(dst);

    cuda::GpuMat d_src_image(image);
    cuda::GpuMat d_dst_image;
    cuda::GpuMat small_image_1(dst);
    small_image_1.copyTo(d_src_image(cv::Rect(0,0,small_image_1.cols, small_image_1.rows)));
    Mat dd;
    d_src_image.download(dd);

    cuda::GpuMat d_src_image_1(dd);
    cuda::GpuMat d_dst_image_1;
    cuda::GpuMat small_image_2(img2);
    small_image_2.copyTo(d_src_image_1(cv::Rect(dst.cols,0,small_image_2.cols, small_image_2.rows)));
    Mat dd1;
    d_src_image_1.download(dd1); 

    cuda::GpuMat d_src_image_2(dd1);
    cuda::GpuMat d_dst_image_2;
    cuda::GpuMat small_image_3(img2);
    small_image_3.copyTo(d_src_image_2(cv::Rect(dst.cols+img2.cols,0,small_image_3.cols, small_image_3.rows)));
    Mat dd2;
    d_src_image_2.download(dd2);

    cuda::GpuMat d_src_image_3(dd2);
    cuda::GpuMat d_dst_image_3;
    cuda::GpuMat small_image_4(img2);
    small_image_4.copyTo(d_src_image_3(cv::Rect(dst.cols,img2.rows,small_image_4.cols, small_image_4.rows)));
    Mat dd3;
    d_src_image_3.download(dd3);
   
    cuda::GpuMat d_src_image_4(dd3);
    cuda::GpuMat d_dst_image_4;
    cuda::GpuMat small_image_5(img2);
    small_image_5.copyTo(d_src_image_4(cv::Rect(dst.cols,img2.rows,small_image_5.cols, small_image_5.rows)));
    Mat dd4;
    d_src_image_4.download(dd4);

    //imwrite("result2.jpg", dd);
    // img2.copyTo(dd(cv::Rect(dst.cols,0,img2.cols, img2.rows)));
    //img2.copyTo(dd1(cv::Rect(dst.cols+img2.cols,0,img2.cols, img2.rows)));
    //img2.copyTo(dd2(cv::Rect(dst.cols,img2.rows,img2.cols, img2.rows)));
    //img2.copyTo(dd3(cv::Rect(dst.cols+img2.cols,img2.rows,img2.cols, img2.rows)));

/*
    dst.copyTo(image(cv::Rect(0,0,dst.cols, dst.rows)));
    img2.copyTo(image(cv::Rect(dst.cols,0,img2.cols, img2.rows)));
    img2.copyTo(image(cv::Rect(dst.cols+img2.cols,0,img2.cols, img2.rows))); 
    img2.copyTo(image(cv::Rect(dst.cols,img2.rows,img2.cols, img2.rows)));
    img2.copyTo(image(cv::Rect(dst.cols+img2.cols,img2.rows,img2.cols, img2.rows)));
*/ 
   //img1.copyTo(image(cv::Rect(img2.cols*2,0,img1.cols, img1.rows)));
    // img2.copyTo(res(Rect(img1.cols, 0, img2.cols, img2.rows)));
    //cvtColor(image, image, CV_GRAY2RGB);
    //applyColorMap(image, image,COLORMAP_RAINBOW);
    // Show result
    //imshow("Img 1", img1);
    //imshow("Img 2", img2);
    //Mat3b imageF_8UC3;
    //imageF.convertTo(imageF_8UC3, CV_8UC3, 255);
    //imwrite("test.png", imageF_8UC3);

 imwrite("result1.jpg", dd4);
 //   imwrite("result1.jpg", image);
    // imshow("Result", res);
    waitKey();
    return 0;
}
