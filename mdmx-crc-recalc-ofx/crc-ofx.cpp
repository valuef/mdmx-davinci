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
#include <stdio.h>
#include <cuda_runtime.h>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <iostream>
#include <Shlobj.h>

#pragma comment(lib, "cudart.lib")

extern "C" void recalc_crc(
  float *input,
  float *output,
  int width,
  int height,
  int px_x_start,
  int px_y_start,
  int mdmx_blade_width,
  cudaStream_t stream
);

#define STR_HELPER(x) #x
#define TO_STR(x) STR_HELPER(x)

#define WIDEN2(x) L##x
#define WIDEN(x) WIDEN2(x)

#if !defined(SHIPPING)
  #define PLUGIN_MAJOR 1
  #define PLUGIN_MINOR 0

  #define PLUGIN_BASE_NAME "MDMX CRC dev"
  #define PLUGIN_ID_BASE "gay.value.mdmx_crc_dev"

  #define PLUGIN_ID PLUGIN_ID_BASE
  #define PLUGIN_NAME PLUGIN_BASE_NAME
#else
  #define PLUGIN_MAJOR 1
  #define PLUGIN_MINOR 0

  #define PLUGIN_BASE_NAME "MDMX CRC"
  #define PLUGIN_ID_BASE "gay.value.mdmx_crc"

  #define PLUGIN_ID PLUGIN_ID_BASE TO_STR(PLUGIN_MAJOR)
  #define PLUGIN_NAME PLUGIN_BASE_NAME " v" TO_STR(PLUGIN_MAJOR) "." TO_STR(PLUGIN_MINOR)
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

static OfxHost* ofx_host;
static OfxPropertySuiteV1* suite_props;
static OfxParameterSuiteV1* suite_param;
static OfxImageEffectSuiteV1* suite_effect;

#include "util.cpp"
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
  OfxHost* host
) {
  ofx_host = host;
}

#define PARAM_MDMX_WIDTH  "MDMXWidth"
#define PARAM_MDMX_X_POS "MDMX_XPos"
#define PARAM_MDMX_Y_POS "MDMX_YPos"

struct Plugin_Data {
  OfxImageEffectHandle handle;
  OfxParamSetHandle param_set;

  OfxImageClipHandle input_clip;
  OfxImageClipHandle output_clip;

  OFX_Param_Instance p_mdmx_width;
  //OFX_Param_Instance p_mdmx_x;
  //OFX_Param_Instance p_mdmx_y;
};

static
bool
check_cuda_error(
  cudaError_t result,
  Plugin_Data* plugin
) {
  if (result == cudaSuccess) {
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
Plugin_Data*
try_get_plugin(
  OfxImageEffectHandle handle
) {
  OfxPropertySetHandle props;
  OFX_CHECK(suite_effect->getPropertySet(handle, &props));

  void* instance_void = 0;
  OFX_CHECK(suite_props->propGetPointer(props, kOfxPropInstanceData, 0, (void**)&instance_void));

  if (instance_void) {
    auto* instance = (Plugin_Data*)instance_void;
    return instance;
  }

  return 0;
}

#define INPUT_CLIP_NAME "Source"
#define OUTPUT_CLIP_NAME "Output"

static
bool
init_plugin(
  OfxImageEffectHandle handle,
  OfxPropertySetHandle props,
  Plugin_Data* plugin
) {
  plugin->handle = handle;

  OfxParamSetHandle param_set;
  OFX_CHECK(suite_effect->getParamSet(handle, &param_set));
  plugin->param_set = param_set;

  OFX_CHECK(suite_effect->clipGetHandle(handle, OUTPUT_CLIP_NAME, &plugin->output_clip, NULL));
  OFX_CHECK(suite_effect->clipGetHandle(handle, INPUT_CLIP_NAME, &plugin->input_clip, NULL));

  plugin->p_mdmx_width = ofx_get_param(PARAM_MDMX_WIDTH, param_set);
  //plugin->p_mdmx_x = ofx_get_param(PARAM_MDMX_X_POS, param_set);
  //plugin->p_mdmx_y = ofx_get_param(PARAM_MDMX_Y_POS, param_set);

  return true;
}

static
void
cleanup_plugin(
  Plugin_Data* plugin
) {
}

OfxStatus
ofx_main(
  const char* action, /* ASCII c string indicating which action to take */
  const void* raw_handle, /* object to which action should be applied, this will need to be cast to the appropriate blind data type depending on the \e action */
  OfxPropertySetHandle in_args, /* handle that contains action specific properties */
  OfxPropertySetHandle out_args /* handle where the plug-in should set various action specific properties*/
) {
  auto handle = (OfxImageEffectHandle)raw_handle;

  debug_logf("%s (handle 0x%X)\n", action, handle);

  if(str_equal(action, kOfxImageEffectActionRender)) {
    auto* plugin = try_get_plugin(handle);

    if (!plugin) {
      return kOfxStatFailed;
    }

    int cuda_enabled = 0;
    OFX_CHECK(suite_props->propGetInt(in_args, kOfxImageEffectPropCudaEnabled, 0, &cuda_enabled));
    if (!(bool)cuda_enabled) {
      return kOfxStatFailed;
    }

    //OfxRectI render_window;
    //suite_props->propGetIntN(in-args, kOfxImageEffectPropRenderWindow, 4, &render_window.x1);

    double time = 0;
    OFX_CHECK(suite_props->propGetDouble(in_args, kOfxPropTime, 0, &time));

    OfxPropertySetHandle output_img = 0;
    OFX_CHECK(suite_effect->clipGetImage(plugin->output_clip, time, NULL /* region, */, &output_img));
    defer{ suite_effect->clipReleaseImage(output_img); };

    OfxPropertySetHandle input_img = 0;
    OFX_CHECK(suite_effect->clipGetImage(plugin->input_clip, time, NULL /* region, */, &input_img));
    defer{ suite_effect->clipReleaseImage(input_img); };


    auto out_components = Pixel_Component::unknown;
    {
      char* components = 0;
      OFX_CHECK(suite_props->propGetString(output_img, kOfxImageEffectPropComponents, 0, &components));
      out_components = str_to_pixel_component(components);
    }

    auto out_depth = Pixel_Depth::unknown;
    {
      char* pixel_depth = 0;
      OFX_CHECK(suite_props->propGetString(output_img, kOfxImageEffectPropPixelDepth, 0, &pixel_depth));
      out_depth = str_to_pixel_depth(pixel_depth);
    }

    auto in_components = Pixel_Component::unknown;
    {
      char* components = 0;
      OFX_CHECK(suite_props->propGetString(input_img, kOfxImageEffectPropComponents, 0, &components));
      in_components = str_to_pixel_component(components);
    }

    auto in_depth = Pixel_Depth::unknown;
    {
      char* pixel_depth = 0;
      OFX_CHECK(suite_props->propGetString(input_img, kOfxImageEffectPropPixelDepth, 0, &pixel_depth));
      in_depth = str_to_pixel_depth(pixel_depth);
    }

    if(out_components != in_components) {
      show_critical_error("out_components != in_components");
      return kOfxStatErrFormat;
    }

    if(out_depth != in_depth) {
      show_critical_error("out_depth != in_depth");
      return kOfxStatErrFormat;
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

    OfxRectI in_bounds = {};
    OFX_CHECK(suite_props->propGetIntN(input_img, kOfxImagePropBounds, 4, &in_bounds.x1));

    auto in_width = in_bounds.x2 - in_bounds.x1;
    auto in_height = in_bounds.y2 - in_bounds.y1;

    if(out_width != in_width) {
      show_critical_error("out_width != in_width");
      return kOfxStatErrFormat;
    }

    if(out_height != in_height) {
      show_critical_error("out_height != in_height");
      return kOfxStatErrFormat;
    }

    void* out_px = 0;
    OFX_CHECK(suite_props->propGetPointer(output_img, kOfxImagePropData, 0, (void**)&out_px));

    void* in_px = 0;
    OFX_CHECK(suite_props->propGetPointer(input_img, kOfxImagePropData, 0, (void**)&in_px));

    if(!out_px || !in_px) {
      if(suite_effect->abort(plugin->handle)) {
        return kOfxStatOK;
      }
      else if (!out_px) {
        show_critical_error("Failed to get output image data");
      }
      else if (!in_px) {
        show_critical_error("Failed to get input image data");
      }

      return kOfxStatFailed;
    }


    void* cuda_stream_void = 0;
    OFX_CHECK(suite_props->propGetPointer(in_args, kOfxImageEffectPropCudaStream, 0, &cuda_stream_void));

    if(!cuda_stream_void) {
      show_critical_error("CUDA unavailable. CUDA & GPU must be enabled.");
      return kOfxStatFailed;
    } 

    auto stream = (cudaStream_t)cuda_stream_void;
    cudaError_t err = cudaSuccess;

    err = cudaMemcpy2DAsync(
      out_px, 
      out_width * sizeof(float) * 4, 
      in_px, 
      out_width * sizeof(float) * 4, 
      out_width * sizeof(float) * 4,
      out_height, 
      cudaMemcpyDefault, 
      stream
    );
    check_cuda_error(err, plugin);

    int mdmx_blade_width = 0;
    OFX_CHECK(suite_param->paramGetValueAtTime(plugin->p_mdmx_width.handle, time, &mdmx_blade_width));

    int x_start = 0;
    //OFX_CHECK(suite_param->paramGetValueAtTime(plugin->p_mdmx_x.handle, time, &x_start));

    int y_start = 0;
    //OFX_CHECK(suite_param->paramGetValueAtTime(plugin->p_mdmx_y.handle, time, &y_start));

    recalc_crc((float*)in_px, (float*)out_px, out_width, out_height, x_start, y_start, mdmx_blade_width, stream);

    err = cudaGetLastError();
    check_cuda_error(err, plugin);

    return kOfxStatOK;
  }
  else if (str_equal(action, kOfxActionLoad)) {
    suite_props = (OfxPropertySuiteV1*)ofx_host->fetchSuite(ofx_host->host, kOfxPropertySuite, 1);
    if (!suite_props) {
      debug_logf("OpenFX: Failed to get the OfxPropertySuiteV1.\n");
      return kOfxStatErrMissingHostFeature;
    }

    suite_effect = (OfxImageEffectSuiteV1*)ofx_host->fetchSuite(ofx_host->host, kOfxImageEffectSuite, 1);
    if (!suite_effect) {
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

      char* host_name = 0;
      OFX_CHECK(suite_props->propGetString(ofx_host->host, kOfxPropName, 0, &host_name));
      if (host_name) debug_logf("host_name: %s\n", host_name);

      char* host_label = 0;
      OFX_CHECK(suite_props->propGetString(ofx_host->host, kOfxPropLabel, 0, &host_label));
      if (!str_equal(host_name, "DaVinciResolve")) {
        debug_logf("Host is not DaVinciResolve and is '%s'. Not loading.\n", host_label);
        return kOfxStatErrMissingHostFeature;
      }
      if (host_label) debug_logf("host_label: %s\n", host_label);

      int host_version_major = -1;
      OFX_CHECK(suite_props->propGetInt(ofx_host->host, kOfxPropVersion, 0, &host_version_major));
      debug_logf("host_version_major: %d\n", host_version_major);

      int host_version_minor = -1;
      OFX_CHECK(suite_props->propGetInt(ofx_host->host, kOfxPropVersion, 1, &host_version_minor));
      debug_logf("host_version_minor: %d\n", host_version_minor);

      int host_version_patch = -1;
      OFX_CHECK(suite_props->propGetInt(ofx_host->host, kOfxPropVersion, 2, &host_version_patch));
      debug_logf("host_version_patch: %d\n", host_version_patch);

      char* host_version_label = 0;
      OFX_CHECK(suite_props->propGetString(ofx_host->host, kOfxPropVersionLabel, 0, &host_version_label));
      if (host_version_label) debug_logf("host_version_label: %s\n", host_version_label);

      char* supports_opencl = 0;
      OFX_CHECK(suite_props->propGetString(ofx_host->host, kOfxImageEffectPropOpenCLRenderSupported, 0, &supports_opencl));
      if (supports_opencl) debug_logf("supports_opencl: %s\n", supports_opencl);

      char* supports_cuda_renderer = 0;
      OFX_CHECK(suite_props->propGetString(ofx_host->host, kOfxImageEffectPropCudaRenderSupported, 0, &supports_cuda_renderer));
      if (supports_cuda_renderer) debug_logf("supports_cuda_renderer: %s\n", supports_cuda_renderer);

      char* supports_cuda_stream = 0;
      OFX_CHECK(suite_props->propGetString(ofx_host->host, kOfxImageEffectPropCudaStream, 0, &supports_cuda_stream));
      if (supports_cuda_stream) debug_logf("supports_cuda_stream: %s\n", supports_cuda_stream);

      char* supports_metal = 0;
      OFX_CHECK(suite_props->propGetString(ofx_host->host, kOfxImageEffectPropMetalRenderSupported, 0, &supports_metal));
      if (supports_metal) debug_logf("supports_metal: %s\n", supports_metal);

      {
        int count = 0;
        OFX_CHECK(suite_props->propGetDimension(ofx_host->host, kOfxImageEffectPropSupportedComponents, &count));

        debug_logf("kOfxImageEffectPropSupportedComponents (%d):\n", count);

        for (int i = 0; i < count; i++) {
          char* value = 0;
          OFX_CHECK(suite_props->propGetString(ofx_host->host, kOfxImageEffectPropSupportedComponents, i, &value));
          if (value) debug_logf("  %s\n", value);
        }
      }

      {
        int count = 0;
        OFX_CHECK(suite_props->propGetDimension(ofx_host->host, kOfxImageEffectPropSupportedPixelDepths, &count));

        debug_logf("kOfxImageEffectPropSupportedPixelDepths (%d):\n", count);

        for (int i = 0; i < count; i++) {
          char* value;
          OFX_CHECK(suite_props->propGetString(ofx_host->host, kOfxImageEffectPropSupportedPixelDepths, i, &value));
          if (value) debug_logf("  %s\n", value);
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
  else if (str_equal(action, kOfxActionDescribe)) {
    OfxPropertySetHandle fx_props;
    OFX_CHECK(suite_effect->getPropertySet(handle, &fx_props));

    ofx_set_all_labels(fx_props, PLUGIN_NAME);

    OFX_CHECK(suite_props->propSetString(fx_props, kOfxImageEffectPluginPropGrouping, 0, "Filter"));
    OFX_CHECK(suite_props->propSetString(fx_props, kOfxPropPluginDescription, 0, PLUGIN_BASE_NAME));

    ofx_append_to_string_array(fx_props, kOfxImageEffectPropSupportedContexts, CONTEXT_FUSION);
    ofx_append_to_string_array(fx_props, kOfxImageEffectPropSupportedContexts, CONTEXT_TIMELINE);

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
  else if (str_equal(action, kOfxImageEffectActionDescribeInContext)) {
    {
      OfxPropertySetHandle in_clip_props;
      OFX_CHECK(suite_effect->clipDefine(handle, INPUT_CLIP_NAME, &in_clip_props));

      ofx_append_to_string_array(in_clip_props, kOfxImageEffectPropSupportedComponents, kOfxImageComponentRGBA);

      OFX_CHECK(suite_props->propSetInt(in_clip_props, kOfxImageEffectPropTemporalClipAccess, 0, 0 /* false */));
      OFX_CHECK(suite_props->propSetInt(in_clip_props, kOfxImageEffectPropSupportsTiles, 0, 0 /* false */));
      OFX_CHECK(suite_props->propSetInt(in_clip_props, kOfxImageClipPropIsMask, 0, 0 /* false */));
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

    {
      OFX_CHECK(suite_param->paramDefine(params_set, kOfxParamTypeInteger, PARAM_MDMX_WIDTH, &param_props));
      ofx_set_label(param_props, "MDMX Blade Width (px)");
      ofx_set_animates(param_props, true);
      ofx_set_evaluate_on_change(param_props, true);
      ofx_set_default_int(param_props, 1920);
      ofx_set_min_value_int(param_props, 1);
      ofx_set_min_display_int(param_props, 1);
      ofx_set_max_value_int(param_props, 1024 * 8);
      ofx_set_max_display_int(param_props, 1024 * 8);
    }

    /*
     * NOTE(valuef): We don't need to specify the location of the mdmx blade for now so this is unimplemented.
    {
      OFX_CHECK(suite_param->paramDefine(params_set, kOfxParamTypeInteger, PARAM_MDMX_X_POS, &param_props));
      ofx_set_label(param_props, "X Start");
      ofx_set_animates(param_props, true);
      ofx_set_evaluate_on_change(param_props, true);
      ofx_set_min_value_int(param_props, 0);
      ofx_set_min_display_int(param_props, 0);
      ofx_set_max_value_int(param_props, 1024 * 16);
      ofx_set_max_display_int(param_props, 1024 * 16);
    }

    {
      OFX_CHECK(suite_param->paramDefine(params_set, kOfxParamTypeInteger, PARAM_MDMX_Y_POS, &param_props));
      ofx_set_label(param_props, "Y Start");
      ofx_set_animates(param_props, true);
      ofx_set_evaluate_on_change(param_props, true);
      ofx_set_min_value_int(param_props, 0);
      ofx_set_min_display_int(param_props, 0);
      ofx_set_max_value_int(param_props, 1024 * 16);
      ofx_set_max_display_int(param_props, 1024 * 16);
    }
    */
    
    return kOfxStatOK;
  }
  else if (str_equal(action, kOfxActionCreateInstance)) {
    auto* plugin = new Plugin_Data();

    OfxPropertySetHandle props;
    OFX_CHECK(suite_effect->getPropertySet(handle, &props));
    OFX_CHECK(suite_props->propSetPointer(props, kOfxPropInstanceData, 0, (void*)plugin));

    if (!init_plugin(handle, props, plugin)) {
      cleanup_plugin(plugin);
      delete plugin;
      return kOfxStatFailed;
    }

    return kOfxStatOK;
  }
  else if (str_equal(action, kOfxActionDestroyInstance)) {
    auto* plugin = try_get_plugin(handle);

    if (plugin) {
      cleanup_plugin(plugin);
      delete plugin;
    }

    return kOfxStatOK;
  }
  else if (str_equal(action, kOfxImageEffectActionIsIdentity)) {
    return kOfxStatReplyDefault; /* do render */
  }
  else if (str_equal(action, kOfxActionUnload)) {
    ofx_host = 0;
    suite_props = 0;
    suite_param = 0;
    suite_effect = 0;

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

    if (!DID_OPEN_CONSOLE) {
      AllocConsole();
      DID_OPEN_CONSOLE = true;
    }
  }
#endif

  if (nth == 0) {
    return &PLUGIN_DEFN;
  }

  return 0;
}

__declspec(dllexport)
int
OfxGetNumberOfPlugins() {
  return 1;
}
