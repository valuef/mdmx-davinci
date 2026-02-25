// Copyright (C) 2026 ValueFactory https://value.gay

#include <cuda_runtime.h>

#define DMX_BIT_SIZE 4
#define BITS_IN_BYTE 8
#define BITS_IN_CRC 4

/*
  One bit is 4x4 pixels
  One byte is BITS_IN_BYTE 4x4 pixels (so 8 * (4x4))

  And we're drawing a line of num_byte bytes

  So a block size of 4 by 4 threads
  And a grid size of 1 by (num_byte * BITS_IN_BYTE) blocks
*/

// NOTE(valuef): Wrap the array in a struct so that CUDA passes this by value instead of by pointer
// to host memory.
// 2026-02-13
struct Payload {
  unsigned char bytes[6];
};

__global__ void bit_blits(
  float* output,
  int px_stride,
  int out_height,
  int px_x_start,
  int px_y_start,
  Payload payload
) {
  int bit_index = 7 - (blockIdx.y % BITS_IN_BYTE);
  int byte_index = blockIdx.y / BITS_IN_BYTE;

  unsigned char data = payload.bytes[byte_index];
  unsigned int mask = (1 << bit_index);
  unsigned int anded = data & mask;

  float bit = anded != 0 ? 1.0f : 0.0f;

  int x = px_x_start + threadIdx.x;
  int y = px_y_start + threadIdx.y + blockIdx.y * DMX_BIT_SIZE;
  y = out_height - y - 1;

  int idx = (y * px_stride + x) * 4; /* x4 as we're working with interleaved RGBA */
  output[idx + 0] = bit;
  output[idx + 1] = bit;
  output[idx + 2] = bit;
  output[idx + 3] = 1.0f;

  /*
  output[idx + 0] = (float)bit_index / 7.0f;
  output[idx + 1] = (float)byte_index / 6.0f;
  output[idx + 2] = (float)(threadIdx.y * 4 + threadIdx.x) / 16.0f;
  output[idx + 3] = 1.0f;
  */
}


extern "C" void blit_dmx_line(
  float* output,
  int out_width,
  int out_height,
  int px_x_start,
  int px_y_start,
  int num_bytes,
  unsigned char line[6],
  unsigned char crc,
  cudaStream_t stream
) {

  Payload payload = {};
  payload.bytes[0] = line[0];
  payload.bytes[1] = line[1];
  payload.bytes[2] = line[2];
  payload.bytes[3] = line[3];
  payload.bytes[4] = line[4];
  payload.bytes[5] = line[5];

  dim3 grid_size(1, num_bytes * BITS_IN_BYTE);
  dim3 block_size(DMX_BIT_SIZE, DMX_BIT_SIZE);

  bit_blits << <grid_size, block_size, 0, stream>> > (output, out_width, out_height, px_x_start, px_y_start, payload);
}
