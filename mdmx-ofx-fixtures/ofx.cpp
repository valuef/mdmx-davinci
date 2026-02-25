// Copyright (C) 2026 ValueFactory https://value.gay

#include "OpenFX/include/ofxCore.h"
#include "OpenFX/include/ofxProperty.h"
#include "OpenFX/include/ofxImageEffect.h"
#include "OpenFX/include/ofxParam.h"
#include "OpenFX/include/ofxGPURender.h"
#include "OpenFX/resolve/ofxImageEffectExt.h"
#include "OpenFX/resolve/ofxParamExt.h"

#include "defer.h"
#include <windows.h>
#include <memory>
#include <assert.h>

#include <wrl/client.h>

#include <stdio.h>
#include <cuda_runtime.h>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <iostream>
#include <mutex>
#include <Shlobj.h>

using namespace Microsoft::WRL;

#pragma comment(lib, "cudart.lib")

#include "json-read.h"
#include "util.cpp"
#include "sfd.h"

#define MAX_FIXTURE_CHANNELS 32

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
);

#define STR_HELPER(x) #x
#define TO_STR(x) STR_HELPER(x)

#define WIDEN2(x) L##x
#define WIDEN(x) WIDEN2(x)

#if !defined(SHIPPING)
  #define PLUGIN_MAJOR 1
  #define PLUGIN_MINOR 0
  #define PLUGIN_PATCH 0

  #define PLUGIN_BASE_NAME "MDMX Fixture dev"
  #define PLUGIN_ID_BASE "gay.value.mdmx_fixture_dev"

  #define PLUGIN_ID PLUGIN_ID_BASE
  #define PLUGIN_NAME PLUGIN_BASE_NAME
#else
  #define PLUGIN_BASE_NAME "MDMX Fixture"
  #define PLUGIN_ID_BASE "gay.value.mdmx_fixture"

  #define PLUGIN_ID PLUGIN_ID_BASE TO_STR(PLUGIN_MAJOR)
  #define PLUGIN_NAME PLUGIN_BASE_NAME " v" TO_STR(PLUGIN_MAJOR) "." TO_STR(PLUGIN_MINOR) "." TO_STR(PLUGIN_PATCH)
#endif

#define LPLUGIN_NAME WIDEN(PLUGIN_NAME)

#if defined(_DEBUG)
  #define DEBUG_BREAK __debugbreak()

  #define debug_logf(fmt, ...) { \
    printf("[%s][T %d][L %d] " ## fmt, __FUNCTION__, GetCurrentThreadId(), __LINE__, __VA_ARGS__); \
  }
#else
  #define DEBUG_BREAK
  #define debug_logf(...)
#endif

static OfxHost *ofx_host;
static OfxPropertySuiteV1 *suite_props;
static OfxParameterSuiteV1 *suite_param;
static OfxImageEffectSuiteV1 *suite_effect;

#include "ofx_help.h"

static
void
show_critical_error(
  std::string err
) {
  auto wstr = str_to_wstr(err.c_str());
  MessageBoxW(0, wstr.c_str(), LPLUGIN_NAME, MB_OK | MB_ICONERROR);
}

static
void
show_info_error(
  std::string err
) {
  auto wstr = str_to_wstr(err.c_str());
  MessageBoxW(0, wstr.c_str(), LPLUGIN_NAME, MB_OK | MB_ICONEXCLAMATION);
}

void
ofx_set_host(
  OfxHost *host
) {
  ofx_host = host;
}

#define PARAM_FIXTURE_NAME "LabelFixtureName"
#define PARAM_BACKING_FIXTURE_CACHE "FixtureCache"
#define PARAM_BACKING_FIXTURE_FILEPATH "FixtureFilepath"
#define PARAM_BUTTON_SELECT_FIXTURE_FILE "ButtonSelectFixtureFile"
#define PARAM_BUTTON_RELOAD_FIXTURE_FILE "ButtonReloadFixture"

#define PARAM_DMX_UNIVERSE "DMXUniverse"
#define PARAM_DMX_CHANNEL "DMXChannel"

#define GROUP_FIXTURE "GroupFixtureDefinition"
#define GROUP_FIXTURE_FILE "GroupFixtureFile"
#define GROUP_CHANNELS "GroupChannels"
#define GROUP_BACKING "GroupBacking"

#define PARAM_ID_RGB(i) "CHANNEL_RGB_" TO_STR(i)
#define PARAM_ID_FLOAT1(i) "CHANNEL_FLOAT_" TO_STR(i)
#define PARAM_ID_FLOAT2(i) "CHANNEL_FLOAT2_" TO_STR(i)
#define PARAM_ID_FLOAT3(i) "CHANNEL_FLOAT3_" TO_STR(i)

struct Fixture {

  struct Control {
    enum Type {
      float_fader,
      rgb,
      pan_tilt_16,
      pos_xyz_16,
      euler_xyz_16,
    };

    // The name of the control.
    char name[128];

    // The name of the fixture.
    Type type;

    // For float_fader, pan_tilt_16, pos_xyz_16, euler_xyz_16, determines the min and max values of those sliders.
    bool has_display_min_max;
    double display_min;
    double display_max;

    // For float_fader, determines the relative DMX channel to output to.
    int channel;

    // For rgb, determines the relative DMX channels of RGB to output to.
    int red_channel;
    int green_channel;
    int blue_channel;

    // pan_tilt_16:
    //  the pan and pan fine channels
    // pos_xyz_16
    //  the x and x fine channels
    // euler_xyz_16
    //  the x euler rotation and x euler rotation fine channels
    int x_channel;
    int x_fine_channel;

    // pan_tilt_16:
    //  the tilt and tile fine channels
    // pos_xyz_16
    //  the y and y fine channels
    // euler_xyz_16
    //  the y euler rotation and y euler rotation fine channels
    int y_channel;
    int y_fine_channel;

    // pos_xyz_16
    //  the z and z fine channels
    // euler_xyz_16
    //  the z euler rotation and z euler rotation fine channels
    int z_channel;
    int z_fine_channel;

    // If this is set, the encoded DMX value will not be smaller than this number.
    // Useful if for example you have a DMX control that considers 0 and 1 to be "off" while 2 is an extreme in one direction.
    // E.g SF 3 particle fwd/back movement considers 0 and 1 to be off, but 2 to be maximum speed in the forward direction.
    bool has_min_dmx_value;
    int min_dmx_value;

    // The default value that this control will take (if it's a double)
    bool has_default_value;
    double default_value;

    // The default color this control will take.
    bool has_default_color;
    double default_r;
    double default_g;
    double default_b;
  };

  char id[128];
  Control controls[32];
  int num_controls;
  int bytes_for_dmx_buffer;
};

// TODO would love a way to communicate the various notes from the patch and present them to the user

struct Param_Group {
  OFX_Param_Instance rgb;
  OFX_Param_Instance f32_1;
  OFX_Param_Instance f32_2;
  OFX_Param_Instance f32_3;
};

struct Plugin_Data {
  OfxImageEffectHandle handle;
  OfxParamSetHandle param_set;

  OfxImageClipHandle output_clip;

  Fixture fixture;
  Param_Group params[MAX_FIXTURE_CHANNELS];
  int bytes_for_dmx_buffer;

  OFX_Param_Instance p_fixture_name;
  OFX_Param_Instance p_backing_fixture_cache;
  OFX_Param_Instance p_backing_fixture_filepath;

  OFX_Param_Instance p_dmx_universe;
  OFX_Param_Instance p_dmx_channel;

};


static
void
enable_double_param(
  OfxPropertySetHandle props,
  const char *name,
  Fixture::Control *control
) {
  ofx_set_is_secret(props, false);
  ofx_set_label(props, name);

  if(control->has_display_min_max) {
    ofx_set_min_value_double(props, control->display_min);
    ofx_set_min_display_double(props, control->display_min);

    ofx_set_max_value_double(props, control->display_max);
    ofx_set_max_display_double(props, control->display_max);
  }

  if(control->has_default_value) {
    ofx_set_default_double(props, control->default_value);
  }
}

static
void
reset_double_param(
  OfxPropertySetHandle props
) {
  ofx_set_is_secret(props, true);

  ofx_set_min_value_double(props, 0);
  ofx_set_min_display_double(props, 0);

  ofx_set_max_value_double(props, 1);
  ofx_set_max_display_double(props, 1);

  ofx_set_default_double(props, 0);
}

static
void
load_fixture(
  Fixture& fixture,
  Plugin_Data* plugin
) {
  plugin->fixture = fixture;
  plugin->bytes_for_dmx_buffer = 0;

  for (int i = 0; i < MAX_FIXTURE_CHANNELS; i++) {
    {
      ofx_set_is_secret(plugin->params[i].rgb.props, true);
      ofx_set_default_rgb(plugin->params[i].rgb.props, 1, 1, 1);

    }

    reset_double_param(plugin->params[i].f32_1.props);
    reset_double_param(plugin->params[i].f32_2.props);
    reset_double_param(plugin->params[i].f32_3.props);
  }

  auto channel_watermark = 0;
  for (int i = 0; i < fixture.num_controls; i++) {
    auto* control = &fixture.controls[i];

    if(control->type == Fixture::Control::Type::float_fader) {
      channel_watermark = max(channel_watermark , control->channel);

      enable_double_param(plugin->params[i].f32_1.props, control->name, control);
    }
    else if(control->type == Fixture::Control::Type::rgb) {
      channel_watermark = max(channel_watermark, control->red_channel);
      channel_watermark = max(channel_watermark, control->green_channel);
      channel_watermark = max(channel_watermark, control->blue_channel);

      auto props = plugin->params[i].rgb.props;
      ofx_set_is_secret(props, false);
      ofx_set_label(props, control->name);

      if (control->has_default_color) {
        ofx_set_default_rgb(props, control->default_r, control->default_g, control->default_b);
      }
    }
    else if(control->type == Fixture::Control::Type::pan_tilt_16) {
      channel_watermark = max(channel_watermark, control->x_channel);
      channel_watermark = max(channel_watermark, control->x_fine_channel);
      channel_watermark = max(channel_watermark, control->y_channel);
      channel_watermark = max(channel_watermark, control->y_fine_channel);

      enable_double_param(plugin->params[i].f32_1.props, "Pan", control);
      enable_double_param(plugin->params[i].f32_2.props, "Tilt", control);
    }
    else if(control->type == Fixture::Control::Type::pos_xyz_16) {
      channel_watermark = max(channel_watermark, control->x_channel);
      channel_watermark = max(channel_watermark, control->x_fine_channel);
      channel_watermark = max(channel_watermark, control->y_channel);
      channel_watermark = max(channel_watermark, control->y_fine_channel);
      channel_watermark = max(channel_watermark, control->z_channel);
      channel_watermark = max(channel_watermark, control->z_fine_channel);

      enable_double_param(plugin->params[i].f32_1.props, "Pos X", control);
      enable_double_param(plugin->params[i].f32_2.props, "Pos Y", control);
      enable_double_param(plugin->params[i].f32_3.props, "Pos Z", control);
    }
    else if(control->type == Fixture::Control::Type::euler_xyz_16) {
      channel_watermark = max(channel_watermark, control->x_channel);
      channel_watermark = max(channel_watermark, control->x_fine_channel);
      channel_watermark = max(channel_watermark, control->y_channel);
      channel_watermark = max(channel_watermark, control->y_fine_channel);
      channel_watermark = max(channel_watermark, control->z_channel);
      channel_watermark = max(channel_watermark, control->z_fine_channel);

      enable_double_param(plugin->params[i].f32_1.props, "Euler X", control);
      enable_double_param(plugin->params[i].f32_2.props, "Euler Y", control);
      enable_double_param(plugin->params[i].f32_3.props, "Euler Z", control);
    }
    else {
      assert(false);
    }
  }

  plugin->bytes_for_dmx_buffer = min(255, channel_watermark);
}

static
const char* 
json_peek_key(
  JSON_Read_Data *j
) {
  JSON_Read_Peek peek = jsonr_peek_begin(j);

  char* key;
  unsigned long key_len;
  jsonr_k(j, &key, &key_len);
  if (j->error) return "";

  jsonr_peek_end(j, peek);

  return key;
}

static
void
json_strcpy(
  char *into, 
  int buf_size, 
  const char *from, 
  int from_size
) {
  if (buf_size <= 0) return;
  if (from_size <= 0) return;

  auto bytes_to_copy = min(buf_size, from_size);
  memcpy(into, from, bytes_to_copy);

  auto zero_term_pos = min(buf_size - 1, bytes_to_copy);
  into[zero_term_pos] = '\0';
}

static
void
json_read_string_strcpy(
  JSON_Read_Data *j,
  char *into,
  int into_size
) {
  char* data; 
  unsigned long len;
  jsonr_v_string(j, &data, &len);
  json_strcpy(into, into_size, data, len);
}

static
void
json_unknown_key_error(
  JSON_Read_Data *j
) {
  auto* key = json_peek_key(j);
  jsonr_error(j, "Unknown key '%s'", key);
}

static
void
json_read_rgb(
  JSON_Read_Data *j,
  double *out_r,
  double *out_g,
  double *out_b
) {
  jsonr_v_table(j) {
    if(jsonr_k_case(j, "r")) {
      *out_r = jsonr_v_number(j);
    }
    else if(jsonr_k_case(j, "g")) {
      *out_g = jsonr_v_number(j);
    }
    else if(jsonr_k_case(j, "b")) {
      *out_b = jsonr_v_number(j);
    }
    else {
      json_unknown_key_error(j);
      return;
    }
  }
}

static
void
parse_fixture_json(
  JSON_Read_Data *j,
  Fixture *fixture
) {
  jsonr_v_table(j) {
    if(jsonr_k_case(j, "id")) {
      json_read_string_strcpy(j, fixture->id, ARRAYSIZE(fixture->id));
    }
    else if (jsonr_k_case(j, "controls")) {
      int count = 0;

      jsonr_v_array(j) {
        if(count >= MAX_FIXTURE_CHANNELS) {
          jsonr_error(j, "More than 32 controls in fixture!");
          return;
        }

        auto *control = &fixture->controls[count];

        jsonr_v_table(j) {
          if(jsonr_k_case(j, "name")) {
            json_read_string_strcpy(j, control->name, ARRAYSIZE(control->name));
          }
          else if(jsonr_k_case(j, "type")) {
            char* data; unsigned long len;
            jsonr_v_string(j, &data, &len);

            if(str_equal(data, "float")) {
              control->type = Fixture::Control::Type::float_fader;
            }
            else if(str_equal(data, "rgb")) {
              control->type = Fixture::Control::Type::rgb;
            }
            else if(str_equal(data, "pan_tilt_16")) {
              control->type = Fixture::Control::Type::pan_tilt_16;
            }
            else if(str_equal(data, "pos_xyz_16")) {
              control->type = Fixture::Control::Type::pos_xyz_16;
            }
            else if(str_equal(data, "euler_xyz_16")) {
              control->type = Fixture::Control::Type::euler_xyz_16;
            }
            else {
              jsonr_error(j, "Unknown fixture type %.*s", len, data);
              return;
            }
          }
          else if(jsonr_k_case(j, "display_min")) {
            control->has_display_min_max = true;
            control->display_min = jsonr_v_number(j);
          }
          else if(jsonr_k_case(j, "display_max")) {
            control->has_display_min_max = true;
            control->display_max = jsonr_v_number(j);
          }
          else if(jsonr_k_case(j, "channel")) {
            control->channel = jsonr_v_number(j);
          }
          else if(jsonr_k_case(j, "x_channel")) {
            control->x_channel = jsonr_v_number(j);
          }
          else if(jsonr_k_case(j, "x_fine_channel")) {
            control->x_fine_channel = jsonr_v_number(j);
          }
          else if(jsonr_k_case(j, "y_channel")) {
            control->y_channel = jsonr_v_number(j);
          }
          else if(jsonr_k_case(j, "y_fine_channel")) {
            control->y_fine_channel = jsonr_v_number(j);
          }
          else if(jsonr_k_case(j, "z_channel")) {
            control->z_channel = jsonr_v_number(j);
          }
          else if(jsonr_k_case(j, "default_value")) {
            control->has_default_value = true;
            control->default_value = jsonr_v_number(j);
          }
          else if(jsonr_k_case(j, "min_dmx_value")) {
            control->has_min_dmx_value = true;
            control->min_dmx_value = jsonr_v_number(j);
          }
          else if(jsonr_k_case(j, "z_fine_channel")) {
            control->z_fine_channel = jsonr_v_number(j);
          }
          else if(jsonr_k_case(j, "red_channel")) {
            control->red_channel = jsonr_v_number(j);
          }
          else if(jsonr_k_case(j, "green_channel")) {
            control->green_channel = jsonr_v_number(j);
          }
          else if(jsonr_k_case(j, "blue_channel")) {
            control->blue_channel = jsonr_v_number(j);
          }
          else if(jsonr_k_case(j, "default_color")) {
            control->has_default_color = true;
            json_read_rgb(j, &control->default_r, &control->default_g, &control->default_b);
            control->default_r /= 255.0f;
            control->default_g /= 255.0f;
            control->default_b /= 255.0f;
          }
          else {
            json_unknown_key_error(j);
            return;
          }
        }
        count += 1;
      }

      fixture->num_controls = count;
    }
    else {
      json_unknown_key_error(j);
      return;
    }
  }
}

static
bool
load_fixture_from_json(
  Plugin_Data *plugin,
  const char *json,
  std::string *maybe_err
) {
  JSON_Read_Data read_json;
  JSON_Read_Data *j = &read_json;
  jsonr_init(j, json, strlen(json));

  Fixture fixture = {};

  parse_fixture_json(j, &fixture);

  if(j->error) {
    if (maybe_err) {
      *maybe_err = "Encountered an error during parsing.\n" + std::string(j->error_msg, j->error_msg_length);
    }
    return false;
  }

  load_fixture(fixture, plugin);

  return true;
}

static
bool
check_cuda_error(
  cudaError_t result,
  Plugin_Data *plugin
) {
  if(result == cudaSuccess) {
    return true;
  }

  DEBUG_BREAK;

  auto* err = cudaGetErrorString(result);

  char buf[128] = { 0 };
  snprintf(buf, ARRAYSIZE(buf), "Cuda Err: %s", err);
  MessageBoxA(0, buf, PLUGIN_NAME, MB_OK | MB_ICONEXCLAMATION);

  return false;
}


static
Plugin_Data *
try_get_plugin(
  OfxImageEffectHandle handle
) {
  OfxPropertySetHandle props;
  OFX_CHECK(suite_effect->getPropertySet(handle, &props));

  void *instance_void = 0;
  OFX_CHECK(suite_props->propGetPointer(props, kOfxPropInstanceData, 0, (void**)&instance_void));

  if(instance_void) {
    auto *instance = (Plugin_Data*)instance_void;
    return instance;
  }

  return 0;
}

#define OUTPUT_CLIP_NAME "Output"

static
bool
init_plugin(
  OfxImageEffectHandle handle,
  OfxPropertySetHandle props,
  Plugin_Data *plugin
) {
  plugin->handle = handle;

  OfxParamSetHandle param_set;
  OFX_CHECK(suite_effect->getParamSet(handle, &param_set));
  plugin->param_set = param_set;

  OFX_CHECK(suite_effect->clipGetHandle(handle, OUTPUT_CLIP_NAME, &plugin->output_clip, NULL));

  #define LOAD_FIXTURE_GROUP(i) { \
    plugin->params[i].rgb = ofx_get_param(PARAM_ID_RGB(i), param_set); \
    plugin->params[i].f32_1 = ofx_get_param(PARAM_ID_FLOAT1(i), param_set); \
    plugin->params[i].f32_2 = ofx_get_param(PARAM_ID_FLOAT2(i), param_set); \
    plugin->params[i].f32_3 = ofx_get_param(PARAM_ID_FLOAT3(i), param_set); \
  }

  LOAD_FIXTURE_GROUP(0)
  LOAD_FIXTURE_GROUP(1)
  LOAD_FIXTURE_GROUP(2)
  LOAD_FIXTURE_GROUP(3)
  LOAD_FIXTURE_GROUP(4)
  LOAD_FIXTURE_GROUP(5)
  LOAD_FIXTURE_GROUP(6)
  LOAD_FIXTURE_GROUP(7)
  LOAD_FIXTURE_GROUP(8)
  LOAD_FIXTURE_GROUP(9)
  LOAD_FIXTURE_GROUP(10)
  LOAD_FIXTURE_GROUP(11)
  LOAD_FIXTURE_GROUP(12)
  LOAD_FIXTURE_GROUP(13)
  LOAD_FIXTURE_GROUP(14)
  LOAD_FIXTURE_GROUP(15)
  LOAD_FIXTURE_GROUP(16)
  LOAD_FIXTURE_GROUP(17)
  LOAD_FIXTURE_GROUP(18)
  LOAD_FIXTURE_GROUP(19)
  LOAD_FIXTURE_GROUP(20)
  LOAD_FIXTURE_GROUP(21)
  LOAD_FIXTURE_GROUP(22)
  LOAD_FIXTURE_GROUP(23)
  LOAD_FIXTURE_GROUP(24)
  LOAD_FIXTURE_GROUP(25)
  LOAD_FIXTURE_GROUP(26)
  LOAD_FIXTURE_GROUP(27)
  LOAD_FIXTURE_GROUP(28)
  LOAD_FIXTURE_GROUP(29)
  LOAD_FIXTURE_GROUP(30)
  LOAD_FIXTURE_GROUP(31)

  plugin->p_fixture_name = ofx_get_param(PARAM_FIXTURE_NAME, param_set);
  plugin->p_backing_fixture_cache = ofx_get_param(PARAM_BACKING_FIXTURE_CACHE, param_set);
  plugin->p_backing_fixture_filepath = ofx_get_param(PARAM_BACKING_FIXTURE_FILEPATH, param_set);
  plugin->p_dmx_universe = ofx_get_param(PARAM_DMX_UNIVERSE, param_set);
  plugin->p_dmx_channel = ofx_get_param(PARAM_DMX_CHANNEL, param_set);

  {
    auto cache = ofx_get_cstr(plugin->p_backing_fixture_cache);

    if(strlen(cache) > 0 && cache[0] != '\0') {
      load_fixture_from_json(plugin, cache, 0);
    }
  }

  return true;
}

static
void
cleanup_plugin(
  Plugin_Data *plugin
) {
}

static inline
double clamp_d(
  double x,
  double min_value,
  double max_value
) {
  if(x > max_value) return max_value;
  if(min_value > x) return min_value;
  return x;
}

#define saturate_d(x) clamp_d(x, 0, 1)

static
double
remap_double_value_if_needed(
  double value, 
  Fixture::Control *control
) {
  if(!control->has_display_min_max) {
    return value;
  }

  auto t = (value - control->display_min) / max(0.0001, (control->display_max - control->display_min));
  t = saturate_d(t);

  return t;
}

static
bool
is_valid_array_index(
  int index,
  int max
) {
  if(0 > index || index >= max) {
    return false;
  }

  return true;
}


static
void
write_double_as_8(
  int cursor,
  double value,
  unsigned char *buf,
  int *min_dmx_value
) {
  auto as_byte = (unsigned char)(value * 255.0);

  if(min_dmx_value) {
    as_byte = max(*min_dmx_value, as_byte);
  }

  buf[cursor] = as_byte;
}

static
void
write_double_as_16(
  int cursor,
  int fine_cursor,
  double value,
  unsigned char *buf,
  int *min_dmx_value
) {
  auto as_16 = (uint16_t)(value * 65535.0);

  if(min_dmx_value) {
    as_16 = max(*min_dmx_value, as_16);
  }

  auto high_u16 = as_16 >> 8;
  auto low_u16 = as_16 & 0xff;

  auto high_u8 = (unsigned char)high_u16;
  auto low_u8 = (unsigned char)low_u16;

  buf[cursor] = high_u16;
  buf[fine_cursor] = low_u16;
}

static
void
encode_double_as_8(
  int relative_dmx_channel,
  double value,
  Fixture::Control *control,
  unsigned char *buf,
  int buf_size
) {
  auto idx = relative_dmx_channel - 1;

  if(!is_valid_array_index(idx, buf_size)) return;

  value = remap_double_value_if_needed(value, control);

  write_double_as_8(
    idx, 
    value, 
    buf, 
    control->has_min_dmx_value ? &control->min_dmx_value : 0
  );
}

static
void
encode_double_as_16(
  int relative_dmx_channel,
  int relative_dmx_channel_fine,
  double value,
  Fixture::Control *control,
  unsigned char *buf,
  int buf_size
) {
  auto idx = relative_dmx_channel - 1;
  auto idx_fine = relative_dmx_channel_fine - 1;

  if(!is_valid_array_index(idx, buf_size)) return;
  if(!is_valid_array_index(idx_fine, buf_size)) return;

  value = remap_double_value_if_needed(value, control);

  write_double_as_16(
    idx, 
    idx_fine,
    value, 
    buf,
    control->has_min_dmx_value ? &control->min_dmx_value : 0
  );
}

static
void
get_double_and_encode_as_16(
  int relative_dmx_channel,
  int relative_dmx_channel_fine,
  OfxParamHandle param_handle,
  double time,
  Fixture::Control *control,
  unsigned char *buf,
  int buf_size
) {
  double value = 0;
  suite_param->paramGetValueAtTime(param_handle, time, &value);

  encode_double_as_16(
    relative_dmx_channel,
    relative_dmx_channel_fine,
    value,
    control,
    buf,
    buf_size
  );
}

OfxStatus
ofx_main(
  const char *action, /* ASCII c string indicating which action to take */
  const void *raw_handle, /* object to which action should be applied, this will need to be cast to the appropriate blind data type depending on the \e action */
  OfxPropertySetHandle in_args, /* handle that contains action specific properties */
  OfxPropertySetHandle out_args /* handle where the plug-in should set various action specific properties*/
) {
  auto handle = (OfxImageEffectHandle)raw_handle;

  debug_logf("%s (handle 0x%X)\n", action, handle);

  if(str_equal(action, kOfxImageEffectActionRender)) {
    auto *plugin = try_get_plugin(handle);

    if(!plugin) {
      return kOfxStatFailed;
    }

    int cuda_enabled = 0;
    OFX_CHECK(suite_props->propGetInt(in_args, kOfxImageEffectPropCudaEnabled, 0, &cuda_enabled));
    if(!(bool)cuda_enabled) {
      return kOfxStatFailed;
    }

    //OfxRectI render_window;
    //suite_props->propGetIntN(in-args, kOfxImageEffectPropRenderWindow, 4, &render_window.x1);

    double time = 0;
    OFX_CHECK(suite_props->propGetDouble(in_args, kOfxPropTime, 0, &time));

    OfxPropertySetHandle output_img = 0;
    OFX_CHECK(suite_effect->clipGetImage(plugin->output_clip, time, NULL /* region, */, &output_img));
    defer { suite_effect->clipReleaseImage(output_img); };

    auto out_components = Pixel_Component::unknown;
    {
      char *components = 0;
      OFX_CHECK(suite_props->propGetString(output_img, kOfxImageEffectPropComponents, 0, &components));
      out_components = str_to_pixel_component(components);
    }

    auto out_depth = Pixel_Depth::unknown;
    {
      char *pixel_depth = 0;
      OFX_CHECK(suite_props->propGetString(output_img, kOfxImageEffectPropPixelDepth, 0, &pixel_depth));
      out_depth = str_to_pixel_depth(pixel_depth);
    }

    if(out_depth != Pixel_Depth::f32) {
      show_critical_error("Pixel depth isn't f32.");
      return kOfxStatErrFormat;
    }

    if(out_components != Pixel_Component::rgba) {
      show_critical_error("Components aren't RGBA.");
      return kOfxStatErrFormat;
    }

    OfxRectI out_bounds = {};
    OFX_CHECK(suite_props->propGetIntN(output_img, kOfxImagePropBounds, 4, &out_bounds.x1));

    auto out_width = out_bounds.x2 - out_bounds.x1;
    auto out_height = out_bounds.y2 - out_bounds.y1;

    void *out_px = 0;
    OFX_CHECK(suite_props->propGetPointer(output_img, kOfxImagePropData, 0, (void**)&out_px));

    if(!out_px) {
      if(suite_effect->abort(plugin->handle)) {
        return kOfxStatOK;
      }

      else if(!out_px) {
        show_critical_error("Failed to get output image data");
      }

      return kOfxStatFailed;
    }


    void *cuda_stream_void = 0;
    OFX_CHECK(suite_props->propGetPointer(in_args, kOfxImageEffectPropCudaStream, 0, &cuda_stream_void));

    if(!cuda_stream_void) {
      show_critical_error("CUDA unavailable. CUDA & GPU must be enabled.");
      return kOfxStatFailed;
    }
    
    auto stream = (cudaStream_t)cuda_stream_void;
    cudaError_t err = cudaSuccess;

    err = cudaMemset2DAsync(out_px, out_width * sizeof(float) * 4, 0, out_width * sizeof(float) * 4, out_height, stream);
    check_cuda_error(err, plugin);

    auto buf_size = plugin->bytes_for_dmx_buffer;
    auto *buf = (unsigned char*)_alloca(buf_size);
    memset(buf, 0, buf_size);

    {
      auto write_cursor = 0;

      for(int i = 0; i < plugin->fixture.num_controls; i++) {
        auto *control = &plugin->fixture.controls[i];

        if(control->type == Fixture::Control::Type::float_fader) do {
          double value = 0;
          suite_param->paramGetValueAtTime(plugin->params[i].f32_1.handle, time, &value);

          encode_double_as_8(control->channel, value, control, buf, buf_size);

        } while(0);
        else if(control->type == Fixture::Control::Type::rgb) do {
          if(!is_valid_array_index(control->red_channel, buf_size)) break;
          if(!is_valid_array_index(control->green_channel, buf_size)) break;
          if(!is_valid_array_index(control->blue_channel, buf_size)) break;

          double r = 0;
          double g = 0;
          double b = 0;
          suite_param->paramGetValueAtTime(plugin->params[i].rgb.handle, time, &r, &g, &b);

          write_double_as_8(control->red_channel - 1, r, buf, nullptr);
          write_double_as_8(control->green_channel - 1, g, buf, nullptr);
          write_double_as_8(control->blue_channel - 1, b, buf, nullptr);
        } while(0);
        else if(control->type == Fixture::Control::Type::pan_tilt_16) {

          get_double_and_encode_as_16( // pan
            control->x_channel,
            control->x_fine_channel,
            plugin->params[i].f32_1.handle, 
            time, control, buf, buf_size
          );

          get_double_and_encode_as_16( // tilt
            control->y_channel,
            control->y_fine_channel,
            plugin->params[i].f32_2.handle, 
            time, control, buf, buf_size
          );
        }
        else if(control->type == Fixture::Control::Type::pos_xyz_16) {
          get_double_and_encode_as_16( // x
            control->x_channel,
            control->x_fine_channel,
            plugin->params[i].f32_1.handle, 
            time, control, buf, buf_size
          );

          get_double_and_encode_as_16( // y
            control->y_channel,
            control->y_fine_channel,
            plugin->params[i].f32_2.handle, 
            time, control, buf, buf_size
          ); 

          get_double_and_encode_as_16( // z
            control->z_channel,
            control->z_fine_channel,
            plugin->params[i].f32_3.handle, 
            time, control, buf, buf_size
          );
        }
        else if(control->type == Fixture::Control::Type::euler_xyz_16) {
          get_double_and_encode_as_16( // x
            control->x_channel,
            control->x_fine_channel,
            plugin->params[i].f32_1.handle, 
            time, control, buf, buf_size
          );

          get_double_and_encode_as_16( // y
            control->y_channel,
            control->y_fine_channel,
            plugin->params[i].f32_2.handle, 
            time, control, buf, buf_size
          ); 

          get_double_and_encode_as_16( // z
            control->z_channel,
            control->z_fine_channel,
            plugin->params[i].f32_3.handle, 
            time, control, buf, buf_size
          );
        }
        else {
          assert(false);
        }
      }
    }

    int dmx_universe = 1;
    OFX_CHECK(suite_param->paramGetValueAtTime(plugin->p_dmx_universe.handle, time, &dmx_universe));
    int dmx_channel = 1;
    OFX_CHECK(suite_param->paramGetValueAtTime(plugin->p_dmx_channel.handle, time, &dmx_channel));

    auto grid_channel = ((dmx_universe - 1) * 512) + (dmx_channel - 1);

    auto grid_line = grid_channel / 6;
    auto y_start_offset = grid_channel % 6;
    auto remaining_bytes = buf_size;
    auto *read_cursor = buf;

    while(true) {
      if(remaining_bytes <= 0) break;

      auto px_x_start = grid_line * 4;
      auto px_y_start = y_start_offset * 4 * 8; /* 4 pixels per 8 bytes */

      auto num_bytes_to_blit = min(6 - y_start_offset, remaining_bytes);

      unsigned char crc = 0;
      blit_dmx_line(
        (float*)out_px, 
        out_width, 
        out_height, 
        px_x_start, 
        px_y_start, 
        num_bytes_to_blit,
        read_cursor,
        crc, 
        stream
      );

      err = cudaGetLastError();
      check_cuda_error(err, plugin);

      grid_line += 1;
      y_start_offset = 0;
      remaining_bytes -= num_bytes_to_blit;
      read_cursor += num_bytes_to_blit;
    }

    return kOfxStatOK;
  }
  else if(str_equal(action, kOfxActionLoad)) {
    suite_props = (OfxPropertySuiteV1*)ofx_host->fetchSuite(ofx_host->host, kOfxPropertySuite, 1);
    if(!suite_props) {
      debug_logf("OpenFX: Failed to get the OfxPropertySuiteV1.\n");
      return kOfxStatErrMissingHostFeature;
    }

    suite_effect = (OfxImageEffectSuiteV1*)ofx_host->fetchSuite(ofx_host->host, kOfxImageEffectSuite, 1);
    if(!suite_effect) {
      debug_logf("OpenFX: Failed to get the OfxImageEffectSuiteV1.\n");
      return kOfxStatErrMissingHostFeature;
    }

    suite_param = (OfxParameterSuiteV1*)ofx_host->fetchSuite(ofx_host->host, kOfxParameterSuite, 1);
    if (!suite_param) {
      debug_logf("OpenFX: Failed to get the OfxParameterSuiteV1.\n");
      return kOfxStatErrMissingHostFeature;
    }

    #if defined(_DEBUG)
    {
      int api_version_major = -1;
      OFX_CHECK(suite_props->propGetInt(ofx_host->host, kOfxPropAPIVersion, 0, &api_version_major));
      debug_logf("api_version_major: %d\n", api_version_major);

      int api_version_minor = -1;
      OFX_CHECK(suite_props->propGetInt(ofx_host->host, kOfxPropAPIVersion, 1, &api_version_minor));
      debug_logf("api_version_minor: %d\n", api_version_minor);

      char *host_name = 0;
      OFX_CHECK(suite_props->propGetString(ofx_host->host, kOfxPropName, 0, &host_name));
      if(host_name) debug_logf("host_name: %s\n", host_name);

      char *host_label = 0;
      OFX_CHECK(suite_props->propGetString(ofx_host->host, kOfxPropLabel, 0, &host_label));
      if(!str_equal(host_name, "DaVinciResolve")) {
        debug_logf("Host is not DaVinciResolve and is '%s'. Not loading.\n", host_label);
        return kOfxStatErrMissingHostFeature;
      }
      if(host_label) debug_logf("host_label: %s\n", host_label);

      int host_version_major = -1;
      OFX_CHECK(suite_props->propGetInt(ofx_host->host, kOfxPropVersion, 0, &host_version_major));
      debug_logf("host_version_major: %d\n", host_version_major);

      int host_version_minor = -1;
      OFX_CHECK(suite_props->propGetInt(ofx_host->host, kOfxPropVersion, 1, &host_version_minor));
      debug_logf("host_version_minor: %d\n", host_version_minor);

      int host_version_patch = -1;
      OFX_CHECK(suite_props->propGetInt(ofx_host->host, kOfxPropVersion, 2, &host_version_patch));
      debug_logf("host_version_patch: %d\n", host_version_patch);

      char *host_version_label = 0;
      OFX_CHECK(suite_props->propGetString(ofx_host->host, kOfxPropVersionLabel, 0, &host_version_label));
      if(host_version_label) debug_logf("host_version_label: %s\n", host_version_label);

      char *supports_opencl = 0;
      OFX_CHECK(suite_props->propGetString(ofx_host->host, kOfxImageEffectPropOpenCLRenderSupported, 0, &supports_opencl));
      if(supports_opencl) debug_logf("supports_opencl: %s\n", supports_opencl);

      char *supports_cuda_renderer = 0;
      OFX_CHECK(suite_props->propGetString(ofx_host->host, kOfxImageEffectPropCudaRenderSupported, 0, &supports_cuda_renderer));
      if(supports_cuda_renderer) debug_logf("supports_cuda_renderer: %s\n", supports_cuda_renderer);

      char *supports_cuda_stream = 0;
      OFX_CHECK(suite_props->propGetString(ofx_host->host, kOfxImageEffectPropCudaStream, 0, &supports_cuda_stream));
      if(supports_cuda_stream) debug_logf("supports_cuda_stream: %s\n", supports_cuda_stream);

      char *supports_metal = 0;
      OFX_CHECK(suite_props->propGetString(ofx_host->host, kOfxImageEffectPropMetalRenderSupported, 0, &supports_metal));
      if(supports_metal) debug_logf("supports_metal: %s\n", supports_metal);

      {
        int count = 0;
        OFX_CHECK(suite_props->propGetDimension(ofx_host->host, kOfxImageEffectPropSupportedComponents, &count));

        debug_logf("kOfxImageEffectPropSupportedComponents (%d):\n", count);

        for(int i = 0; i < count; i++) {
          char *value = 0;
          OFX_CHECK(suite_props->propGetString(ofx_host->host, kOfxImageEffectPropSupportedComponents, i, &value));
          if(value) debug_logf("  %s\n", value);
        }
      }

      {
        int count = 0;
        OFX_CHECK(suite_props->propGetDimension(ofx_host->host, kOfxImageEffectPropSupportedPixelDepths, &count));

        debug_logf("kOfxImageEffectPropSupportedPixelDepths (%d):\n", count);

        for(int i = 0; i < count; i++) {
          char *value;
          OFX_CHECK(suite_props->propGetString(ofx_host->host, kOfxImageEffectPropSupportedPixelDepths, i, &value));
          if(value) debug_logf("  %s\n", value);
        }
      }
    }
    #endif

    /*
    * this doesn't actually test properly on davinci either with kOfxImageEffectPropCudaRenderSupported or kOfxImageEffectPropCudaStreamSupported
    {
      char *cuda_state = 0;
      OFX_CHECK(suite_props->propGetString(ofx_host->host, kOfxImageEffectPropCudaStreamSupported, 0, &cuda_state));

      if(!str_equal(cuda_state, "true")) {
        show_critical_error("CUDA: Host indicated that CUDA rendering is not supported.\nCUDA may be disabled in the settings or not supported in this machine.\nCUDA requires an NVIDIA GPU.");
        return kOfxStatErrMissingHostFeature;
      }
    }
    */

    return kOfxStatOK;
  }
  else if(str_equal(action, kOfxActionDescribe)) {
    OfxPropertySetHandle fx_props;
    OFX_CHECK(suite_effect->getPropertySet(handle, &fx_props));

    ofx_set_all_labels(fx_props, PLUGIN_NAME);

    OFX_CHECK(suite_props->propSetString(fx_props, kOfxImageEffectPluginPropGrouping, 0, "Generator"));
    OFX_CHECK(suite_props->propSetString(fx_props, kOfxPropPluginDescription, 0, PLUGIN_BASE_NAME));

    ofx_append_to_string_array(fx_props, kOfxImageEffectPropSupportedContexts, kOfxImageEffectContextGenerator);

    //ofx_append_to_string_array(fx_props, kOfxImageEffectPropSupportedPixelDepths, kOfxBitDepthByte);
    //ofx_append_to_string_array(fx_props, kOfxImageEffectPropSupportedPixelDepths, kOfxBitDepthShort);
    //ofx_append_to_string_array(fx_props, kOfxImageEffectPropSupportedPixelDepths, kOfxBitDepthHalf);
    ofx_append_to_string_array(fx_props, kOfxImageEffectPropSupportedPixelDepths, kOfxBitDepthFloat);

    OFX_CHECK(suite_props->propSetInt(fx_props, kOfxImageEffectPluginPropSingleInstance, 0, 0 /* false */));
    OFX_CHECK(suite_props->propSetString(fx_props, kOfxImageEffectPluginRenderThreadSafety, 0, kOfxImageEffectRenderInstanceSafe));
    OFX_CHECK(suite_props->propSetInt(fx_props, kOfxImageEffectPluginPropHostFrameThreading, 0, 0 /* false */));

    OFX_CHECK(suite_props->propSetInt(fx_props, kOfxImageEffectPropSupportsMultiResolution, 0, 1 /* true */));

    OFX_CHECK(suite_props->propSetInt(fx_props, kOfxImageEffectPropSupportsTiles, 0, 0 /* false*/));
    OFX_CHECK(suite_props->propSetInt(fx_props, kOfxImageEffectPropTemporalClipAccess, 0, 0 /* false */));

    OFX_CHECK(suite_props->propSetInt(fx_props, kOfxImageEffectPluginPropFieldRenderTwiceAlways, 0, 0 /* false */));
    OFX_CHECK(suite_props->propSetInt(fx_props, kOfxImageEffectPropSupportsMultipleClipPARs, 0, 0 /* false*/));
    OFX_CHECK(suite_props->propSetString(fx_props, kOfxImageEffectPropNoSpatialAwareness, 0, "true"));

    OFX_CHECK(suite_props->propSetString(fx_props, kOfxImageEffectPropCudaRenderSupported, 0, "true"));
    OFX_CHECK(suite_props->propSetString(fx_props, kOfxImageEffectPropCudaStreamSupported, 0, "true"));
    OFX_CHECK(suite_props->propSetString(fx_props, kOfxImageEffectPropOpenCLRenderSupported, 0, "false"));
    OFX_CHECK(suite_props->propSetString(fx_props, kOfxImageEffectPropMetalRenderSupported, 0, "false"));

    OFX_CHECK(suite_props->propSetString(fx_props, kOfxImageEffectPropCPURenderSupported, 0, "false"));

    return kOfxStatOK;
  }
  else if(str_equal(action, kOfxImageEffectActionDescribeInContext)) {
    {
      // NOTE(valuef): We still need an input clip even if we're not gonna use it otherwise davinci doesn't give us an input component type.
      OfxPropertySetHandle in_clip_props;
      OFX_CHECK(suite_effect->clipDefine(handle, "Source", &in_clip_props));

      ofx_append_to_string_array(in_clip_props, kOfxImageEffectPropSupportedComponents, kOfxImageComponentRGBA);

      OFX_CHECK(suite_props->propSetInt(in_clip_props, kOfxImageEffectPropTemporalClipAccess, 0, 0 /* false */));
      OFX_CHECK(suite_props->propSetInt(in_clip_props, kOfxImageEffectPropSupportsTiles, 0, 0 /* false */));
      OFX_CHECK(suite_props->propSetInt(in_clip_props, kOfxImageClipPropIsMask, 0, 0 /* false */));
      OFX_CHECK(suite_props->propSetInt(in_clip_props, kOfxImageClipPropOptional, 0, 1 /* true */));
    }

    {
      OfxPropertySetHandle out_clip_props;
      OFX_CHECK(suite_effect->clipDefine(handle, OUTPUT_CLIP_NAME, &out_clip_props));

      ofx_append_to_string_array(out_clip_props, kOfxImageEffectPropSupportedComponents, kOfxImageComponentRGBA);

      OFX_CHECK(suite_props->propSetInt(out_clip_props, kOfxImageEffectPropTemporalClipAccess, 0, 0 /* false */));
      OFX_CHECK(suite_props->propSetInt(out_clip_props, kOfxImageEffectPropSupportsTiles, 0, 0 /* false */));
      OFX_CHECK(suite_props->propSetInt(out_clip_props, kOfxImageClipPropIsMask, 0, 0 /* false */));
    }


    OfxParamSetHandle params_set;
    OFX_CHECK(suite_effect->getParamSet(handle, &params_set));

    OfxPropertySetHandle param_props;

    ofx_add_group(params_set, GROUP_FIXTURE, "Fixture");

    {
      OFX_CHECK(suite_param->paramDefine(params_set, kOfxParamTypeInteger, PARAM_DMX_UNIVERSE, &param_props));
      ofx_set_label(param_props, "DMX Universe");
      ofx_set_animates(param_props, true);
      ofx_set_evaluate_on_change(param_props, true);
      ofx_set_group(param_props, GROUP_FIXTURE);
      ofx_set_min_value_int(param_props, 1);
      ofx_set_min_display_int(param_props, 1);
      ofx_set_max_value_int(param_props, 8);
      ofx_set_max_display_int(param_props, 8);
    }

    {
      OFX_CHECK(suite_param->paramDefine(params_set, kOfxParamTypeInteger, PARAM_DMX_CHANNEL, &param_props));
      ofx_set_label(param_props, "DMX Channel");
      ofx_set_animates(param_props, true);
      ofx_set_evaluate_on_change(param_props, true);
      ofx_set_group(param_props, GROUP_FIXTURE);
      ofx_set_min_value_int(param_props, 1);
      ofx_set_min_display_int(param_props, 1);
      ofx_set_max_value_int(param_props, 512);
      ofx_set_max_display_int(param_props, 512);
    }

    ofx_add_readonly_str(params_set, PARAM_FIXTURE_NAME, "Fixture", false, GROUP_FIXTURE);

    {
      ofx_add_group(params_set, GROUP_CHANNELS, "Channels");

      #define RGB_FIXTURE(i) { \
        OFX_CHECK(suite_param->paramDefine(params_set, kOfxParamTypeRGB, PARAM_ID_RGB(i), &param_props)); \
        ofx_set_label(param_props, "Color " TO_STR(i)); \
        ofx_set_animates(param_props, true); \
        ofx_set_evaluate_on_change(param_props, true); \
        ofx_set_is_secret(param_props, true); \
        ofx_set_group(param_props, GROUP_FIXTURE); \
      }

      #define FLOAT_FIXTURE(i, id) { \
        OFX_CHECK(suite_param->paramDefine(params_set, kOfxParamTypeDouble, id, &param_props)); \
        ofx_set_label(param_props, "Float " id); \
        ofx_set_min_value_double(param_props, 0); \
        ofx_set_min_display_double(param_props, 0); \
        ofx_set_max_value_double(param_props, 1); \
        ofx_set_max_display_double(param_props, 1); \
        ofx_set_animates(param_props, true); \
        ofx_set_evaluate_on_change(param_props, true); \
        ofx_set_is_secret(param_props, true); \
        ofx_set_group(param_props, GROUP_FIXTURE); \
      }

      #define FIXTURE_GROUP(i) \
        RGB_FIXTURE(i) \
        FLOAT_FIXTURE(i, PARAM_ID_FLOAT1(i)) \
        FLOAT_FIXTURE(i, PARAM_ID_FLOAT2(i)) \
        FLOAT_FIXTURE(i, PARAM_ID_FLOAT3(i))

      FIXTURE_GROUP(0)
      FIXTURE_GROUP(1)
      FIXTURE_GROUP(2)
      FIXTURE_GROUP(3)
      FIXTURE_GROUP(4)
      FIXTURE_GROUP(5)
      FIXTURE_GROUP(6)
      FIXTURE_GROUP(7)
      FIXTURE_GROUP(8)
      FIXTURE_GROUP(9)
      FIXTURE_GROUP(10)
      FIXTURE_GROUP(11)
      FIXTURE_GROUP(12)
      FIXTURE_GROUP(13)
      FIXTURE_GROUP(14)
      FIXTURE_GROUP(15)
      FIXTURE_GROUP(16)
      FIXTURE_GROUP(17)
      FIXTURE_GROUP(18)
      FIXTURE_GROUP(19)
      FIXTURE_GROUP(20)
      FIXTURE_GROUP(21)
      FIXTURE_GROUP(22)
      FIXTURE_GROUP(23)
      FIXTURE_GROUP(24)
      FIXTURE_GROUP(25)
      FIXTURE_GROUP(26)
      FIXTURE_GROUP(27)
      FIXTURE_GROUP(28)
      FIXTURE_GROUP(29)
      FIXTURE_GROUP(30)
      FIXTURE_GROUP(31)
    }

    {
      param_props = ofx_add_group(params_set, GROUP_FIXTURE_FILE, "File");

      {
        param_props = ofx_add_button(params_set, PARAM_BUTTON_SELECT_FIXTURE_FILE, "Select File");
        ofx_set_group(param_props, GROUP_FIXTURE_FILE);
      }

      {
        param_props = ofx_add_button(params_set, PARAM_BUTTON_RELOAD_FIXTURE_FILE, "Reload File");
        ofx_set_group(param_props, GROUP_FIXTURE_FILE);
      }
    }

    {
      param_props = ofx_add_group(params_set, GROUP_BACKING, "Backing");
      #if !defined(_DEBUG)
        ofx_set_is_secret(param_props, true);
      #endif

      ofx_add_backing_string(params_set, PARAM_BACKING_FIXTURE_CACHE, GROUP_BACKING);
      ofx_add_backing_string(params_set, PARAM_BACKING_FIXTURE_FILEPATH, GROUP_BACKING);
    }

    return kOfxStatOK;
  }
  else if(str_equal(action, kOfxActionCreateInstance)) {
    auto *plugin = new Plugin_Data();

    OfxPropertySetHandle props;
    OFX_CHECK(suite_effect->getPropertySet(handle, &props));
    OFX_CHECK(suite_props->propSetPointer(props, kOfxPropInstanceData, 0, (void*)plugin));

    if(!init_plugin(handle, props, plugin)) {
      cleanup_plugin(plugin);
      delete plugin;
      return kOfxStatFailed;
    }

    return kOfxStatOK;
  }
  else if(str_equal(action, kOfxActionDestroyInstance)) {
    auto *plugin = try_get_plugin(handle);

    if(plugin) {
      cleanup_plugin(plugin);
      delete plugin;
    }

    return kOfxStatOK;
  }
  else if(str_equal(action, kOfxImageEffectActionIsIdentity)) {
    return kOfxStatReplyDefault; /* do render */
  }
  else if(str_equal(action, kOfxActionUnload)) {
    ofx_host = 0;
    suite_props = 0;
    suite_param = 0;
    suite_effect = 0;

    return kOfxStatOK;
  }
  else if(str_equal(action, kOfxActionInstanceChanged)) {
    auto *plugin = try_get_plugin(handle);
    if(!plugin) {
      return kOfxStatFailed;
    }

    char *prop_type = 0;
    OFX_CHECK(suite_props->propGetString(in_args, kOfxPropType, 0, &prop_type));

    if(str_equal(prop_type, kOfxTypeParameter)) {
      char *param_name = 0;
      OFX_CHECK(suite_props->propGetString(in_args, kOfxPropName, 0, &param_name));
      
      if(str_equal(param_name, PARAM_BUTTON_SELECT_FIXTURE_FILE)) {
        static sfd_Options opt = {};
        opt.filter = L"*.json";
        opt.filter_name = L"Fixture JSON(.json)";
        opt.title = L"Choose Fixture JSON";

        auto *cur_path = ofx_get_cstr(plugin->p_backing_fixture_filepath);

        std::wstring cur_path_w;
        if(cur_path && strlen(cur_path) > 0) {
          cur_path_w = str_to_wstr(cur_path);
          opt.path = cur_path_w.c_str();
        }

        auto *path = sfd_open_dialog(&opt);

        if(path) do {
          auto new_path = wstr_to_str(path);

          std::string json_data;
          std::string err;

          if(!read_entire_file_string(new_path, json_data, err)) {
            show_info_error(err);
            break;
          }

          if(!load_fixture_from_json(plugin, json_data.c_str(), &err)) {
            show_info_error(err);
            break;
          }

          ofx_set_str(plugin->p_fixture_name, plugin->fixture.id);
          ofx_set_str(plugin->p_backing_fixture_cache, json_data.c_str());
          ofx_set_str(plugin->p_backing_fixture_filepath, new_path.c_str());
        } while(0);
      }
      else if(str_equal(param_name, PARAM_BUTTON_RELOAD_FIXTURE_FILE)) {
        do {
          auto path = ofx_get_stdstr(plugin->p_backing_fixture_filepath);

          std::string json_data;
          std::string err;
          if(!read_entire_file_string(path, json_data, err)) {
            show_info_error(err);
            break;
          }

          if(!load_fixture_from_json(plugin, json_data.c_str(), &err)) {
            show_info_error(err);
            break;
          }

          ofx_set_str(plugin->p_fixture_name, plugin->fixture.id);
          ofx_set_str(plugin->p_backing_fixture_cache, json_data.c_str());
        } while (0);
      }
    }
    return kOfxStatOK;
  }

  return kOfxStatReplyDefault;
}

static OfxPlugin PLUGIN_DEFN = {
  "OfxImageEffectPluginAPI",
  1,
  PLUGIN_ID,
  PLUGIN_MAJOR,
  PLUGIN_MINOR,
  ofx_set_host,
  ofx_main
};

__declspec(dllexport) 
OfxPlugin* 
OfxGetPlugin(int nth) {
  #if defined(_DEBUG)
  {
    static bool DID_OPEN_CONSOLE = false;

    if(!DID_OPEN_CONSOLE) {
      AllocConsole();
      DID_OPEN_CONSOLE = true;
    }
  }
  #endif

  if(nth == 0) {
    return &PLUGIN_DEFN;
  }

  return 0;
}

__declspec(dllexport)
int
OfxGetNumberOfPlugins() {
  return 1;
}
