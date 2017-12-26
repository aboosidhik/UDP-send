#include <opencv2/gpu/gpu.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/highgui/highgui.hpp>

using namespace cv;

int main(int argc, char* argv[])
{

  Mat src_img, dst_img;
  gpu::GpuMat src_gpu, dst_gpu;

  src_img = imread("my_image.jpg");

  if (src_img.empty()) {
    printf("error: can not load image...\n");
    return -1;
  }

  // Create a destination to paint the source into.
  dst_img.create(src_img.size(), src_img.type());

  // Push the images into the GPU
  src_gpu.upload(src_img);
  dst_gpu.upload(dst_img);

  // Rotate in the GPU!
  gpu::rotate(src_gpu, dst_gpu, src_gpu.size(), 180, src_gpu.size().width, src_gpu.size().height);

  // Download the rendered GPU data back to CPU
  dst_gpu.download(dst_img);

  // ... so that it can be written with imwrite.
  imwrite("./COOL.png", dst_img);

  return 0;

}
