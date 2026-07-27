#include <obs-module.h>
#include <math.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("sr_filter", "en-US")

// -----------------------------------------------------------------
// シェーダーコード
// -----------------------------------------------------------------
static const char *sr_shader_code =
	"uniform float4x4 ViewProj;"
	"uniform texture2d image;"
	"uniform float noise_strength;"
	"uniform float bit_steps;"
	"uniform float elapsed_time;"
	"uniform bool  monochrome;" // C言語側から「白黒にするか」を受け取る
	"sampler_state textureSampler { Filter = Linear; AddressU = Clamp; AddressV = Clamp; };"

	"struct VertData { float4 pos : POSITION; float2 uv : TEXCOORD0; };"

	"VertData VSDefault(VertData v_in) {"
	"  VertData v_out;"
	"  v_out.pos = mul(float4(v_in.pos.xyz, 1.0), ViewProj);"
	"  v_out.uv = v_in.uv;"
	"  return v_out;"
	"}"

	"float rand(float2 co) { return frac(sin(dot(co.xy, float2(12.9898, 78.233))) * 43758.5453); }"

	"float gaussian_noise(float2 uv) {"
	"  float2 seed = uv + float2(elapsed_time, elapsed_time * 1.2);"
	"  float u1 = rand(seed + float2(0.1, 0.3));"
	"  float u2 = rand(seed + float2(0.7, 0.5));"
	"  if (u1 < 0.00001) u1 = 0.00001;"
	"  float r = sqrt(-2.0 * log(u1));"
	"  float theta = 2.0 * 3.14159265 * u2;"
	"  return r * cos(theta);"
	"}"

	"float4 PSFunction(VertData v_in) : TARGET {"
	"  float4 rgba = image.Sample(textureSampler, v_in.uv);"
	"  float3 color = rgba.rgb;"
	"  "
	"  /* 1. 白黒化（チェックが入っている場合のみ） */"
	"  if (monochrome) {"
	"    float lum = dot(color, float3(0.299, 0.587, 0.114));"
	"    color = float3(lum, lum, lum);"
	"  }"
	"  "
	"  /* 2. ガウスノイズ追加 */"
	"  float g_noise = gaussian_noise(v_in.uv) * noise_strength * 0.5;"
	"  color += g_noise;"
	"  "
	"  /* 3. ビット階調制限 */"
	"  if (bit_steps <= 1.5) {"
	"    color.r = step(0.5, color.r);"
	"    color.g = step(0.5, color.g);"
	"    color.b = step(0.5, color.b);"
	"  } else {"
	"    color = floor(color * (bit_steps - 1.0) + 0.5) / (bit_steps - 1.0);"
	"  }"
	"  "
	"  return float4(color, rgba.a);"
	"}"

	"technique Draw {"
	"  pass { vertex_shader = VSDefault(v_in); pixel_shader = PSFunction(v_in); }"
	"}";

// -----------------------------------------------------------------
// データ構造
// -----------------------------------------------------------------
struct sr_filter_data {
	obs_source_t *context;
	gs_effect_t *effect;
	gs_eparam_t *param_noise;
	gs_eparam_t *param_bits;
	gs_eparam_t *param_time;
	gs_eparam_t *param_mono;
	float noise_val;
	float bit_val;
	float cur_time;
	bool is_mono;
};

static const char *sr_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "確率共鳴フィルタ";
}

static void sr_destroy(void *data)
{
	struct sr_filter_data *filter = data;
	if (filter) {
		obs_enter_graphics();
		gs_effect_destroy(filter->effect);
		obs_leave_graphics();
		bfree(filter);
	}
}

static void *sr_create(obs_data_t *settings, obs_source_t *context)
{
	struct sr_filter_data *filter = bzalloc(sizeof(struct sr_filter_data));
	filter->context = context;
	obs_enter_graphics();
	filter->effect = gs_effect_create(sr_shader_code, "sr_shader", NULL);
	if (filter->effect) {
		filter->param_noise = gs_effect_get_param_by_name(filter->effect, "noise_strength");
		filter->param_bits = gs_effect_get_param_by_name(filter->effect, "bit_steps");
		filter->param_time = gs_effect_get_param_by_name(filter->effect, "elapsed_time");
		filter->param_mono = gs_effect_get_param_by_name(filter->effect, "monochrome");
	}
	obs_leave_graphics();
	obs_source_update(context, settings);
	return filter;
}

static void sr_update(void *data, obs_data_t *settings)
{
	struct sr_filter_data *filter = data;
	filter->noise_val = (float)obs_data_get_int(settings, "noise_amount") / 100.0f;
	int exponent = (int)obs_data_get_int(settings, "bit_exponent");
	filter->bit_val = powf(2.0f, (float)exponent);
	filter->is_mono = obs_data_get_bool(settings, "use_monochrome");
}

static void sr_video_tick(void *data, float seconds)
{
	struct sr_filter_data *filter = data;
	filter->cur_time += seconds;
	if (filter->cur_time > 1000.0f)
		filter->cur_time = 0.0f;
}

static void sr_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct sr_filter_data *filter = data;
	if (!filter->effect) {
		obs_source_skip_video_filter(filter->context);
		return;
	}

	gs_effect_set_float(filter->param_noise, filter->noise_val);
	gs_effect_set_float(filter->param_bits, filter->bit_val);
	gs_effect_set_float(filter->param_time, filter->cur_time);
	gs_effect_set_bool(filter->param_mono, filter->is_mono);

	obs_source_process_filter_begin(filter->context, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING);
	obs_source_process_filter_tech_end(filter->context, filter->effect, 0, 0, "Draw");
}

static obs_properties_t *sr_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_bool(props, "use_monochrome", "白黒モード");
	obs_properties_add_int_slider(props, "noise_amount", "ノイズ強度", 0, 100, 1);
	obs_properties_add_int_slider(props, "bit_exponent", "ビット数 (2^N)", 1, 8, 1);
	return props;
}

static struct obs_source_info sr_filter_info = {
	.id = "sr_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = sr_get_name,
	.create = sr_create,
	.destroy = sr_destroy,
	.update = sr_update,
	.get_properties = sr_properties,
	.video_tick = sr_video_tick,
	.video_render = sr_render,
};

bool obs_module_load(void)
{
	obs_register_source(&sr_filter_info);
	return true;
}
