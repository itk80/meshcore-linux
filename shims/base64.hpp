#pragma once
// Minimal base64 shim — covers the densaugeo/base64 API surface used by
// MeshCore (encode_base64 / decode_base64 with raw byte buffers).

#include <cstdint>
#include <cstring>

static inline unsigned int encode_base64_length(unsigned int input_length) {
  return ((input_length + 2) / 3) * 4;
}

static inline unsigned int decode_base64_length(const unsigned char* input,
                                                 unsigned int input_length) {
  unsigned int padding = 0;
  if (input_length >= 2) {
    if (input[input_length - 1] == '=') padding++;
    if (input[input_length - 2] == '=') padding++;
  }
  return (input_length / 4) * 3 - padding;
}

static inline unsigned int encode_base64(const unsigned char* input,
                                          unsigned int input_length,
                                          unsigned char* output) {
  static const char enc[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  unsigned int i, j = 0;
  for (i = 0; i + 2 < input_length; i += 3) {
    output[j++] = enc[ input[i] >> 2 ];
    output[j++] = enc[ ((input[i] & 0x03) << 4) | (input[i+1] >> 4) ];
    output[j++] = enc[ ((input[i+1] & 0x0F) << 2) | (input[i+2] >> 6) ];
    output[j++] = enc[ input[i+2] & 0x3F ];
  }
  if (i < input_length) {
    output[j++] = enc[ input[i] >> 2 ];
    if (i + 1 < input_length) {
      output[j++] = enc[ ((input[i] & 0x03) << 4) | (input[i+1] >> 4) ];
      output[j++] = enc[ (input[i+1] & 0x0F) << 2 ];
    } else {
      output[j++] = enc[ (input[i] & 0x03) << 4 ];
      output[j++] = '=';
    }
    output[j++] = '=';
  }
  return j;
}

static inline unsigned int decode_base64(const unsigned char* input,
                                          unsigned int input_length,
                                          unsigned char* output) {
  static const int8_t dec[256] = {
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63, 52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
    -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14, 15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
    -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40, 41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
  };
  unsigned int j = 0;
  int val = 0, bits = -8;
  for (unsigned int i = 0; i < input_length; i++) {
    int8_t d = dec[input[i]];
    if (d < 0) continue;
    val = (val << 6) | d;
    bits += 6;
    if (bits >= 0) {
      output[j++] = (unsigned char)((val >> bits) & 0xFF);
      bits -= 8;
    }
  }
  return j;
}
