#include <cuda_runtime.h>

#define PIXELS_PER_BIT 4
#define BITS_IN_CRC 4
#define BYTES_IN_COLUMN 6

#define DMX_BYTE_HEIGHT_IN_PIXELS (8 * PIXELS_PER_BIT)
#define DMX_COLUMN_HEIGHT_IN_PIXELS (DMX_BYTE_HEIGHT_IN_PIXELS * BYTES_IN_COLUMN)
#define CRC_COLUMN_HEIGHT_IN_PIXELS (BITS_IN_CRC * PIXELS_PER_BIT)

__device__
unsigned char
crc_4_bits_from_6_bytes(
  unsigned char data[6]
) {
  unsigned int crc = 0;
  unsigned int poly = 0x03;

  for (int i = 0; i < 6; i++) {
    unsigned char v = data[i];

    for (int bit = 7; bit >= 0; --bit) {
      unsigned int inBit = (v >> bit) & 1;
      unsigned int top = (crc >> 3) & 1;
      crc = ((crc << 1) | inBit) & 0xF;
      if (top == 1) crc ^= poly;
    }
  }

  //crc = crc << 4; // @Endianness

  return (unsigned char)crc;
}

__device__
int
make_index(
  int pixel_x,
  int pixel_y,
  int px_stride
) {
  return (pixel_y * px_stride + pixel_x) * 4;
}

__global__ 
void 
cuda_recalc_crc(
  float *input,
  float *output,
  int width,
  int height,
  int px_x_start,
  int px_y_start
) {
  int crc_y_start = height - DMX_COLUMN_HEIGHT_IN_PIXELS - CRC_COLUMN_HEIGHT_IN_PIXELS - px_y_start;

  int dmx_column_y_ceiling = height - 1;

  int pixel_x = px_x_start + threadIdx.x + blockIdx.x * blockDim.x;
  int pixel_y = crc_y_start + threadIdx.y + blockIdx.y * blockDim.y;

  unsigned char dmx[6];
  for(int dmx_it = 0; dmx_it < 6; dmx_it++) {

    unsigned char data = 0;
    for(int bit_it = 0; bit_it < 8; bit_it++) {
      int y = dmx_column_y_ceiling - (dmx_it * DMX_BYTE_HEIGHT_IN_PIXELS) - (bit_it * PIXELS_PER_BIT) - px_y_start;

      int idx = make_index(pixel_x, y, width);
      int is_on = input[idx] > .2f ? 1 : 0;

      if(is_on) {
        int nth_bit = 7 - bit_it; // @Endianness
        int mask = 1 << nth_bit;
        data |= mask;
      }
    }

    dmx[dmx_it] = data;
  }

  unsigned char crc = crc_4_bits_from_6_bytes(dmx);

  int which_crc_bit = (threadIdx.y / PIXELS_PER_BIT);
  unsigned int mask = (1 << which_crc_bit);
  unsigned int anded = crc & mask;

  float bit = anded != 0 ? 1.0f : 0.0f;

  int idx = make_index(pixel_x, pixel_y, width);

  output[idx + 0] = bit;
  output[idx + 1] = bit;
  output[idx + 2] = bit;
  output[idx + 3] = 1.0f;

  /*
  output[idx + 0] = threadIdx.x;
  output[idx + 1] = threadIdx.y;
  output[idx + 2] = 0;
  output[idx + 3] = 1.0f;
  */
}

extern "C" 
void 
recalc_crc(
  float *input,
  float *output,
  int width,
  int height,
  int px_x_start,
  int px_y_start,
  int mdmx_blade_width,
  cudaStream_t stream
) {
  dim3 block_size(PIXELS_PER_BIT, PIXELS_PER_BIT * BITS_IN_CRC);
  mdmx_blade_width = max(block_size.x, mdmx_blade_width);
  mdmx_blade_width = min(mdmx_blade_width, width);

  dim3 grid_size(mdmx_blade_width / block_size.x, 1);

  cuda_recalc_crc<<<grid_size, block_size, 0, stream>>>(
    input,
    output,
    width,
    height,
    px_x_start,
    px_y_start
  );
}
