// Copyright (C) 2026 ValueFactory https://value.gay

#define CONTEXT_FUSION kOfxImageEffectContextGeneral
#define CONTEXT_TIMELINE kOfxImageEffectContextFilter

#if defined(_DEBUG)
  #define OFX_CHECK(_call) { \
    auto OFX_RESULT = _call; \
    if(OFX_RESULT != kOfxStatOK) { \
      auto *err_str = ofx_status_to_string(OFX_RESULT); \
      debug_logf("OFX_CHECK: %s (%d) (%s)\n", err_str, OFX_RESULT, #_call); \
    } \
  }
#else
  #define OFX_CHECK(_call) _call
#endif

static
const char*
ofx_status_to_string(
  OfxStatus status
) {
  switch (status) {
  case kOfxStatOK: return "kOfxStatOK";
  case kOfxStatFailed: return "kOfxStatFailed";
  case kOfxStatErrFatal: return "kOfxStatErrFatal";
  case kOfxStatErrUnknown: return "kOfxStatErrUnknown";
  case kOfxStatErrMissingHostFeature: return "kOfxStatErrMissingHostFeature";
  case kOfxStatErrUnsupported: return "kOfxStatErrUnsupported";
  case kOfxStatErrExists: return "kOfxStatErrExists";
  case kOfxStatErrFormat: return "kOfxStatErrFormat";
  case kOfxStatErrMemory: return "kOfxStatErrMemory";
  case kOfxStatErrBadHandle: return "kOfxStatErrBadHandle";
  case kOfxStatErrBadIndex: return "kOfxStatErrBadIndex";
  case kOfxStatErrValue: return "kOfxStatErrValue";
  case kOfxStatReplyYes: return "kOfxStatReplyYes";
  case kOfxStatReplyNo: return "kOfxStatReplyNo";
  case kOfxStatReplyDefault: return "kOfxStatReplyDefault";
  case kOfxStatErrImageFormat: return "kOfxStatErrImageFormat";
  }
  return "<Unknown OfxStatus>";
}

enum class Pixel_Component {
  rgba,
  rgb,
  alpha,
  none,
  unknown
};

enum class Pixel_Depth {
  u8,
  u16,
  f16,
  f32,
  none,
  unknown
};

static
Pixel_Component
str_to_pixel_component(
  const char *str
) {
  if(!str) return Pixel_Component::unknown;

       if(str_equal(str, kOfxImageComponentRGBA)) return Pixel_Component::rgba;
  else if(str_equal(str, kOfxImageComponentRGB)) return Pixel_Component::rgb;
  else if(str_equal(str, kOfxImageComponentAlpha)) return Pixel_Component::alpha;
  else if(str_equal(str, kOfxImageComponentNone)) return Pixel_Component::none;

  return Pixel_Component::unknown;
}

static
Pixel_Depth
str_to_pixel_depth(
  const char *str
) {
  if(!str) return Pixel_Depth::unknown;

       if(str_equal(str, kOfxBitDepthByte)) return Pixel_Depth::u8;
  else if(str_equal(str, kOfxBitDepthShort)) return Pixel_Depth::u16;
  else if(str_equal(str, kOfxBitDepthHalf)) return Pixel_Depth::f16;
  else if(str_equal(str, kOfxBitDepthFloat)) return Pixel_Depth::f32;
  else if(str_equal(str, kOfxBitDepthNone)) return Pixel_Depth::none;

  return Pixel_Depth::none;
}

static
void
ofx_append_to_string_array(
  OfxPropertySetHandle props,
  const char *prop,
  const char *value
) {
  int count = 0;

  OFX_CHECK(suite_props->propGetDimension(props, prop, &count));
  OFX_CHECK(suite_props->propSetString(props, prop, count, value));
}

static void ofx_set_label(OfxPropertySetHandle props, const char *label) { 
  OFX_CHECK(suite_props->propSetString(props, kOfxPropLabel, 0, label));
}
static void ofx_set_all_labels(OfxPropertySetHandle props, const char *label) { 
  ofx_set_label(props, label);
  OFX_CHECK(suite_props->propSetString(props, kOfxPropShortLabel, 0, label));
  OFX_CHECK(suite_props->propSetString(props, kOfxPropLongLabel, 0, label));
}
static void ofx_set_animates(OfxPropertySetHandle props, bool animates) { 
  OFX_CHECK(suite_props->propSetInt(props, kOfxParamPropAnimates, 0, animates ? 1 : 0));
}
static void ofx_set_enabled(OfxPropertySetHandle props, bool enabled) { 
  OFX_CHECK(suite_props->propSetInt(props, kOfxParamPropEnabled, 0, enabled ? 1 : 0));
}
static void ofx_set_persistant(OfxPropertySetHandle props, bool persist) { 
  OFX_CHECK(suite_props->propSetInt(props, kOfxParamPropPersistant, 0, persist ? 1 : 0));
}
static void ofx_set_evaluate_on_change(OfxPropertySetHandle props, bool value) { 
  OFX_CHECK(suite_props->propSetInt(props, kOfxParamPropEvaluateOnChange, 0, value ? 1 : 0));
}
static void ofx_set_group(OfxPropertySetHandle props, const char* group_id) {
  OFX_CHECK(suite_props->propSetString(props, kOfxParamPropParent, 0, group_id));
}
static void ofx_set_default_str(OfxPropertySetHandle props, const char *value) {
  OFX_CHECK(suite_props->propSetString(props, kOfxParamPropDefault, 0, value));
}
static void ofx_set_default_bool(OfxPropertySetHandle props, bool value) {
  OFX_CHECK(suite_props->propSetInt(props, kOfxParamPropDefault, 0, value ? 1 : 0));
}
static void ofx_set_default_int(OfxPropertySetHandle props, int x) {
  OFX_CHECK(suite_props->propSetInt(props, kOfxParamPropDefault, 0, x));
}

static void ofx_set_default_int2(OfxPropertySetHandle props, int x, int y) {
  OFX_CHECK(suite_props->propSetInt(props, kOfxParamPropDefault, 0, x));
  OFX_CHECK(suite_props->propSetInt(props, kOfxParamPropDefault, 1, y));
}
static void ofx_set_default_double(OfxPropertySetHandle props, double v) {
  OFX_CHECK(suite_props->propSetDouble(props, kOfxParamPropDefault, 0, v));
}
static void ofx_set_default_rgb(OfxPropertySetHandle props, double r, double g, double b) {
  OFX_CHECK(suite_props->propSetDouble(props, kOfxParamPropDefault, 0, r));
  OFX_CHECK(suite_props->propSetDouble(props, kOfxParamPropDefault, 1, g));
  OFX_CHECK(suite_props->propSetDouble(props, kOfxParamPropDefault, 2, b));
}
static void ofx_set_min_value_double(OfxPropertySetHandle props, double v) {
  OFX_CHECK(suite_props->propSetDouble(props, kOfxParamPropMin, 0, v));
}
static void ofx_set_min_display_double(OfxPropertySetHandle props, double v) {
  OFX_CHECK(suite_props->propSetDouble(props, kOfxParamPropDisplayMin, 0, v));
}
static void ofx_set_max_value_double(OfxPropertySetHandle props, double v) {
  OFX_CHECK(suite_props->propSetDouble(props, kOfxParamPropMax, 0, v));
}
static void ofx_set_max_display_double(OfxPropertySetHandle props, double v) {
  OFX_CHECK(suite_props->propSetDouble(props, kOfxParamPropDisplayMax, 0, v));
}
static void ofx_set_min_value_int(OfxPropertySetHandle props, int v) {
  OFX_CHECK(suite_props->propSetInt(props, kOfxParamPropMin, 0, v));
}
static void ofx_set_min_display_int(OfxPropertySetHandle props, int v) {
  OFX_CHECK(suite_props->propSetInt(props, kOfxParamPropDisplayMin, 0, v));
}
static void ofx_set_max_value_int(OfxPropertySetHandle props, int v) {
  OFX_CHECK(suite_props->propSetInt(props, kOfxParamPropMax, 0, v));
}
static void ofx_set_max_display_int(OfxPropertySetHandle props, int v) {
  OFX_CHECK(suite_props->propSetInt(props, kOfxParamPropDisplayMax, 0, v));
}

static void ofx_set_is_secret(OfxPropertySetHandle props, bool secret) {
  OFX_CHECK(suite_props->propSetInt(props, kOfxParamPropSecret, 0, secret ? 1 : 0));
}

static 
OfxPropertySetHandle
ofx_add_group(
  OfxParamSetHandle params_set,
  const char *id, 
  const char *label
) {
  OfxPropertySetHandle param_props;
  OFX_CHECK(suite_param->paramDefine(params_set, kOfxParamTypeGroup, id, &param_props));

  ofx_set_label(param_props, label);

  return param_props;
}

static 
OfxPropertySetHandle
ofx_add_button(
  OfxParamSetHandle params_set,
  const char *id, 
  const char *label
) {
  OfxPropertySetHandle param_props;
  OFX_CHECK(suite_param->paramDefine(params_set, kOfxParamTypePushButton, id, &param_props));

  ofx_set_label(param_props, label);

  return param_props;
}

static 
OfxPropertySetHandle
ofx_add_label(
  OfxParamSetHandle params_set,
  const char* id, 
  const char *label,
  bool is_secret,
  const char *group = 0
) {
  OfxPropertySetHandle param_props;
  OFX_CHECK(suite_param->paramDefine(params_set, kOfxParamTypeBoolean, id, &param_props));

  ofx_set_all_labels(param_props, label);
  ofx_set_animates(param_props, false);
  ofx_set_is_secret(param_props, is_secret);
  ofx_set_enabled(param_props, false);
  ofx_set_evaluate_on_change(param_props, false);

  if(group) {
    ofx_set_group(param_props, group);
  }

  return param_props;
}

static 
OfxPropertySetHandle
ofx_add_readonly_str(
  OfxParamSetHandle params_set,
  const char *id, 
  const char *label,
  bool is_static = false,
  const char *group = 0
) {

  OfxPropertySetHandle param_props;
  OFX_CHECK(suite_param->paramDefine(params_set, kOfxParamTypeString, id, &param_props));

  ofx_set_all_labels(param_props, label);
  ofx_set_animates(param_props, false);
  ofx_set_enabled(param_props, false);
  ofx_set_evaluate_on_change(param_props, false);

  if(is_static) {
    ofx_append_to_string_array(param_props, kOfxParamPropStringMode, kOfxParamStringIsLabel);
  }

  if(group) {
    ofx_set_group(param_props, group);
  }


  return param_props;
}

static 
void 
ofx_append_str_choice(
  OfxPropertySetHandle props, 
  const char *enum_str, 
  const char *option_str
) {
  int option_count = 0;
  OFX_CHECK(suite_props->propGetDimension(props, kOfxParamPropChoiceOption, &option_count));

  OFX_CHECK(suite_props->propSetString(props, kOfxParamPropChoiceEnum, option_count, enum_str));
  OFX_CHECK(suite_props->propSetString(props, kOfxParamPropChoiceOption, option_count, option_str));
}

enum class Param_Type {
  string_param,
  int_param,
  int2_param,
  int3_param,
  double_param,
  double2_param,
  double3_param,
  rgb_param,
  rgba_param,
  bool_param,
  choice_param,
  str_choice_param,
  custom_param,
  group_param,
  page_param,
  button_param,
  unknown,
};

static
Param_Type
str_to_param_type(
  const char *str
) {
       if(str_equal(str, kOfxParamTypeString)) return Param_Type::string_param;
  else if(str_equal(str, kOfxParamTypeInteger)) return Param_Type::int_param;
  else if(str_equal(str, kOfxParamTypeInteger2D)) return Param_Type::int2_param;
  else if(str_equal(str, kOfxParamTypeInteger3D)) return Param_Type::int3_param;
  else if(str_equal(str, kOfxParamTypeDouble)) return Param_Type::double_param;
  else if(str_equal(str, kOfxParamTypeDouble2D)) return Param_Type::double2_param;
  else if(str_equal(str, kOfxParamTypeDouble3D)) return Param_Type::double3_param;
  else if(str_equal(str, kOfxParamTypeRGB)) return Param_Type::rgb_param;
  else if(str_equal(str, kOfxParamTypeRGBA)) return Param_Type::rgba_param;
  else if(str_equal(str, kOfxParamTypeBoolean)) return Param_Type::bool_param;
  else if(str_equal(str, kOfxParamTypeChoice)) return Param_Type::choice_param;
  else if(str_equal(str, kOfxParamTypeStrChoice)) return Param_Type::str_choice_param;
  else if(str_equal(str, kOfxParamTypeCustom)) return Param_Type::custom_param;
  else if(str_equal(str, kOfxParamTypeGroup)) return Param_Type::group_param;
  else if(str_equal(str, kOfxParamTypePage)) return Param_Type::page_param;
  else if(str_equal(str, kOfxParamTypePushButton)) return Param_Type::button_param;

  return Param_Type::unknown;
};


struct OFX_Param_Instance {
  OfxParamHandle handle;
  OfxPropertySetHandle props;
  Param_Type type;
  const char *id;
};

static
OFX_Param_Instance
ofx_get_param(
  const char *id,
  OfxParamSetHandle param_set
) {
  OFX_Param_Instance inst = {};
  OFX_CHECK(suite_param->paramGetHandle(param_set, id, &inst.handle, &inst.props));
  // OFX_CHECK(suite_param->paramGetPropertySet(inst.handle, &inst.props));

  char *type_str = 0;
  OFX_CHECK(suite_props->propGetString(inst.props, kOfxParamPropType, 0, &type_str));

  inst.type = str_to_param_type(type_str);
  inst.id = id;

  return inst;
}

static
int
ofx_get_int(
  OFX_Param_Instance &param
) {
  int number = 0;
  OFX_CHECK(suite_param->paramGetValue(param.handle, &number));
  return number;
}

static
int
ofx_get_bool(
  OFX_Param_Instance &param
) {
  auto number = ofx_get_int(param);
  return number != 0;
}

static
OfxPointI
ofx_get_int2(
  OFX_Param_Instance &param
) {
  OfxPointI point = {};
  OFX_CHECK(suite_param->paramGetValue(param.handle, &point.x, &point.y));
  return point;
}

static
const char *
ofx_get_cstr(
  OFX_Param_Instance &param
) {
  char *str_data;
  OFX_CHECK(suite_param->paramGetValue(param.handle, &str_data));
  return str_data;
}

static
std::string
ofx_get_stdstr(
  OFX_Param_Instance &param
) {
  auto *cstr = ofx_get_cstr(param);
  return std::string(cstr);
}

static
void
ofx_set_str(
  OFX_Param_Instance &param,
  const char *str
) {
  OFX_CHECK(suite_param->paramSetValue(param.handle, str));
}

static
void
ofx_reset_choice_param(
  OFX_Param_Instance &param
) {
  OFX_CHECK(suite_props->propReset(param.props, kOfxParamPropChoiceEnum));
  OFX_CHECK(suite_props->propReset(param.props, kOfxParamPropChoiceOption));
}

static 
OfxPropertySetHandle
ofx_add_backing_string(
  OfxParamSetHandle params_set,
  const char *id, 
  const char *group = 0
) {
  OfxPropertySetHandle param_props;

  OFX_CHECK(suite_param->paramDefine(params_set, kOfxParamTypeString, id, &param_props));
  ofx_set_label(param_props, id);
  ofx_set_animates(param_props, false);
  ofx_set_evaluate_on_change(param_props, false);

  #if defined(_DEBUG)
    ofx_set_group(param_props, group);
  #else
    ofx_set_is_secret(param_props, true);
  #endif

  return param_props;
}

