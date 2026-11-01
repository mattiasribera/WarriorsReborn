/*
 * Warriors.fx - a self-contained ReShade look for the PC port of The Warriors (xbrun)
 *
 * What it is
 *   One technique ("Warriors", one pass) that chains five optional stages, each with its own
 *   toggle in the ReShade overlay:
 *     1. Contrast-adaptive sharpening (AMD FidelityFX CAS) for the upscaled 1440x1080 /
 *        1920x1080 image.
 *     2. Color grade inspired by the 1979 film's night look: lift/gamma/gain, a gentle S-curve
 *        for slightly deeper blacks, warm sodium-vapor highlights and cool shadows (split
 *        toning), and vibrance that spares already-saturated pixels and skin tones.
 *     3. Animated, luminance-weighted film grain.
 *     4. Soft, aspect-correct vignette.
 *     5. Triangular output dither against banding after grading.
 *   Defaults are deliberately subtle so HUD and subtitle text stay readable.
 *   Only "ReShade.fxh" and "ReShadeUI.fxh" are included (both ship with every ReShade install);
 *   no depth buffer is used.
 *
 * Install
 *   1. Install ReShade for TheWarriors.exe and choose the Vulkan API.
 *   2. Copy this file into the game folder's reshade-shaders\Shaders folder.
 *   3. Copy Warriors.ini next to TheWarriors.exe, start the game, open the overlay (Home key) and
 *      select the Warriors.ini preset.
 *
 * Licence
 *   Original code in this file is distributed under the same terms as the rest of the
 *   warriors-port repository. The sharpening stage (CAS_Sharpen) is a port of the AMD
 *   FidelityFX Contrast Adaptive Sharpening algorithm and is covered by the MIT licence below.
 *
 *   ---- AMD FidelityFX CAS ----
 *   Copyright (c) 2020 Advanced Micro Devices, Inc. All rights reserved.
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included in
 *   all copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 *   THE SOFTWARE.
 *   ----------------------------
 */

#include "ReShadeUI.fxh"
#include "ReShade.fxh"

#ifndef BUFFER_COLOR_BIT_DEPTH
	#define BUFFER_COLOR_BIT_DEPTH 8
#endif

// ---------------------------------------------------------------------------------------------
// Uniforms
// ---------------------------------------------------------------------------------------------

uniform bool CAS_Enable <
	ui_category = "1. Sharpening (CAS)";
	ui_label = "Enable sharpening";
	ui_tooltip = "AMD FidelityFX Contrast Adaptive Sharpening. Restores detail lost to texture upscaling and resolution scaling while avoiding halos on strong edges.";
> = true;

uniform float CAS_Sharpness <
	ui_type = "slider";
	ui_category = "1. Sharpening (CAS)";
	ui_label = "Sharpness";
	ui_tooltip = "0 = mild, 1 = strongest CAS setting.";
	ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
> = 0.4;

uniform bool Grade_Enable <
	ui_category = "2. Color grade";
	ui_label = "Enable color grade";
	ui_tooltip = "Night look inspired by the 1979 film: warm streetlight highlights, cool shadows, slightly deeper blacks.";
> = true;

uniform float Grade_Lift <
	ui_type = "slider";
	ui_category = "2. Color grade";
	ui_label = "Lift (blacks)";
	ui_tooltip = "Raises (+) or lowers (-) the black level. Negative values crush shadow detail quickly.";
	ui_min = -0.10; ui_max = 0.10; ui_step = 0.005;
> = 0.0;

uniform float Grade_Gamma <
	ui_type = "slider";
	ui_category = "2. Color grade";
	ui_label = "Gamma (midtones)";
	ui_tooltip = "Above 1 brightens midtones, below 1 darkens them.";
	ui_min = 0.50; ui_max = 2.00; ui_step = 0.01;
> = 1.0;

uniform float Grade_Gain <
	ui_type = "slider";
	ui_category = "2. Color grade";
	ui_label = "Gain (highlights)";
	ui_tooltip = "Scales the whole signal; above 1 clips the brightest highlights.";
	ui_min = 0.50; ui_max = 1.50; ui_step = 0.01;
> = 1.0;

uniform float Grade_Contrast <
	ui_type = "slider";
	ui_category = "2. Color grade";
	ui_label = "S-curve contrast";
	ui_tooltip = "Gentle luminance S-curve: deeper blacks and brighter highlights while pure black and white stay fixed.";
	ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
> = 0.15;

uniform float3 Grade_HighlightTint <
	ui_type = "color";
	ui_category = "2. Color grade";
	ui_label = "Highlight tint";
	ui_tooltip = "Color pushed into the highlights (default: sodium-vapor streetlight orange).";
> = float3(1.0, 0.78, 0.45);

uniform float Grade_HighlightAmount <
	ui_type = "slider";
	ui_category = "2. Color grade";
	ui_label = "Highlight tint amount";
	ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
> = 0.12;

uniform float3 Grade_ShadowTint <
	ui_type = "color";
	ui_category = "2. Color grade";
	ui_label = "Shadow tint";
	ui_tooltip = "Color pushed into the shadows (default: cool night blue).";
> = float3(0.45, 0.62, 1.0);

uniform float Grade_ShadowAmount <
	ui_type = "slider";
	ui_category = "2. Color grade";
	ui_label = "Shadow tint amount";
	ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
> = 0.10;

uniform float Grade_Vibrance <
	ui_type = "slider";
	ui_category = "2. Color grade";
	ui_label = "Vibrance";
	ui_tooltip = "Saturation boost that mostly affects dull colors; negative values desaturate.";
	ui_min = -1.0; ui_max = 1.0; ui_step = 0.01;
> = 0.15;

uniform float Grade_SkinProtect <
	ui_type = "slider";
	ui_category = "2. Color grade";
	ui_label = "Skin tone protection";
	ui_tooltip = "How much vibrance spares orange-red skin hues.";
	ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
> = 0.7;

uniform bool Grain_Enable <
	ui_category = "3. Film grain";
	ui_label = "Enable film grain";
> = true;

uniform float Grain_Amount <
	ui_type = "slider";
	ui_category = "3. Film grain";
	ui_label = "Grain amount";
	ui_min = 0.0; ui_max = 0.20; ui_step = 0.001;
> = 0.025;

uniform float Grain_HighlightProtect <
	ui_type = "slider";
	ui_category = "3. Film grain";
	ui_label = "Highlight protection";
	ui_tooltip = "Reduces grain in bright areas (and on bright HUD text). 1 = no grain on pure white.";
	ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
> = 0.6;

uniform bool Vignette_Enable <
	ui_category = "4. Vignette";
	ui_label = "Enable vignette";
> = true;

uniform float Vignette_Amount <
	ui_type = "slider";
	ui_category = "4. Vignette";
	ui_label = "Vignette amount";
	ui_tooltip = "Darkening in the far corners.";
	ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
> = 0.15;

uniform float Vignette_Start <
	ui_type = "slider";
	ui_category = "4. Vignette";
	ui_label = "Vignette start";
	ui_tooltip = "Distance from the center (0) to the corner (1) where darkening begins.";
	ui_min = 0.0; ui_max = 0.95; ui_step = 0.01;
> = 0.55;

uniform bool Dither_Enable <
	ui_category = "5. Output";
	ui_label = "Enable output dither";
	ui_tooltip = "Tiny triangular noise that hides banding introduced by grading.";
> = true;

uniform float Dither_Strength <
	ui_type = "slider";
	ui_category = "5. Output";
	ui_label = "Dither strength";
	ui_tooltip = "In output quantization steps (1 = one 8-bit code value).";
	ui_min = 0.0; ui_max = 2.0; ui_step = 0.05;
> = 1.0;

uniform bool Debug_SplitView <
	ui_category = "5. Output";
	ui_label = "Split view (left: original)";
	ui_tooltip = "Shows the unprocessed image on the left half for comparison.";
> = false;

uniform float Timer < source = "timer"; >;

// ---------------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------------

static const float3 LUMA_709 = float3(0.2126, 0.7152, 0.0722);

float Luma(float3 c)
{
	return dot(c, LUMA_709);
}

// Sin-free hash (Dave Hoskins, "Hash without Sine"), stable across GPU vendors.
float Hash12(float2 p)
{
	float3 p3 = frac(float3(p.xyx) * 0.1031);
	p3 += dot(p3, p3.yzx + 33.33);
	return frac((p3.x + p3.y) * p3.z);
}

float Min3s(float a, float b, float c) { return min(a, min(b, c)); }
float Max3s(float a, float b, float c) { return max(a, max(b, c)); }
float3 Min3(float3 a, float3 b, float3 c) { return min(a, min(b, c)); }
float3 Max3(float3 a, float3 b, float3 c) { return max(a, max(b, c)); }

// ---------------------------------------------------------------------------------------------
// 1. AMD FidelityFX CAS (no-scaling path), per channel. MIT licence, see header.
// ---------------------------------------------------------------------------------------------

float3 CAS_Sharpen(float2 uv)
{
	// a b c
	// d e f
	// g h i
	float3 a = tex2Doffset(ReShade::BackBuffer, uv, int2(-1, -1)).rgb;
	float3 b = tex2Doffset(ReShade::BackBuffer, uv, int2( 0, -1)).rgb;
	float3 c = tex2Doffset(ReShade::BackBuffer, uv, int2( 1, -1)).rgb;
	float3 d = tex2Doffset(ReShade::BackBuffer, uv, int2(-1,  0)).rgb;
	float3 e = tex2D(ReShade::BackBuffer, uv).rgb;
	float3 f = tex2Doffset(ReShade::BackBuffer, uv, int2( 1,  0)).rgb;
	float3 g = tex2Doffset(ReShade::BackBuffer, uv, int2(-1,  1)).rgb;
	float3 h = tex2Doffset(ReShade::BackBuffer, uv, int2( 0,  1)).rgb;
	float3 i = tex2Doffset(ReShade::BackBuffer, uv, int2( 1,  1)).rgb;

	// Soft min and max: cross plus full 3x3 window, range 0..2.
	float3 mn = Min3(Min3(d, e, f), b, h);
	float3 mn2 = Min3(Min3(mn, a, c), g, i);
	mn += mn2;
	float3 mx = Max3(Max3(d, e, f), b, h);
	float3 mx2 = Max3(Max3(mx, a, c), g, i);
	mx += mx2;

	// Smooth minimum distance to signal limit divided by smooth maximum.
	float3 amp = saturate(min(mn, 2.0 - mx) / max(mx, 1e-5));
	amp = sqrt(amp);

	// Filter shape:  0 w 0 / w 1 w / 0 w 0, w = amp * peak, peak = -1 / lerp(8, 5, sharpness).
	float peak = -1.0 / lerp(8.0, 5.0, saturate(CAS_Sharpness));
	float3 w = amp * peak;
	float3 rcpWeight = 1.0 / (1.0 + 4.0 * w);
	return saturate((b * w + d * w + f * w + h * w + e) * rcpWeight);
}

// ---------------------------------------------------------------------------------------------
// 2. Color grade
// ---------------------------------------------------------------------------------------------

float3 ApplyLiftGammaGain(float3 c)
{
	c = Grade_Gain * (c + Grade_Lift * (1.0 - c));
	c = saturate(c);
	return pow(c, 1.0 / max(Grade_Gamma, 1e-3));
}

float3 ApplySCurve(float3 c)
{
	float l = Luma(c);
	float curved = l * l * (3.0 - 2.0 * l); // smoothstep: 0 and 1 stay fixed
	float target = lerp(l, curved, Grade_Contrast);
	return saturate(c * (target / max(l, 1e-4)));
}

float3 ApplySplitTone(float3 c)
{
	float l = Luma(c);
	float3 warm = Grade_HighlightTint / max(Luma(Grade_HighlightTint), 1e-3);
	float3 cool = Grade_ShadowTint / max(Luma(Grade_ShadowTint), 1e-3);
	float wHigh = smoothstep(0.35, 1.0, l) * Grade_HighlightAmount;
	float wLow = (1.0 - smoothstep(0.0, 0.55, l)) * Grade_ShadowAmount;
	c = lerp(c, c * warm, wHigh);
	c = lerp(c, c * cool, wLow);
	return saturate(c);
}

float3 ApplyVibrance(float3 c)
{
	float mxc = Max3s(c.r, c.g, c.b);
	float mnc = Min3s(c.r, c.g, c.b);
	float sat = mxc - mnc;

	// Skin: red >= green >= blue with a hue of roughly 10..40 degrees.
	float hue = (c.g - c.b) / max(c.r - c.b, 1e-4) / 6.0;
	float isWarmOrder = step(c.g, c.r) * step(c.b, c.g) * step(0.02, sat);
	float skin = isWarmOrder * saturate(1.0 - abs(hue - 0.07) / 0.05);

	float amount = Grade_Vibrance * (1.0 - sat) * (1.0 - skin * Grade_SkinProtect);
	float l = Luma(c);
	return saturate(lerp(float3(l, l, l), c, 1.0 + amount));
}

float3 ColorGrade(float3 c)
{
	c = ApplyLiftGammaGain(c);
	c = ApplySCurve(c);
	c = ApplySplitTone(c);
	c = ApplyVibrance(c);
	return c;
}

// ---------------------------------------------------------------------------------------------
// 3. Film grain, 4. vignette, 5. dither
// ---------------------------------------------------------------------------------------------

float3 ApplyGrain(float3 c, float2 pos)
{
	float t = frac(Timer * 0.001) * 1000.0; // changes every frame, bounded for precision
	float2 p = pos + t * float2(1.618, 2.718);
	float n = (Hash12(p) + Hash12(p + 57.0)) - 1.0; // -1..1, triangular-ish
	float weight = 1.0 - Grain_HighlightProtect * Luma(c);
	return saturate(c + n * Grain_Amount * weight);
}

float3 ApplyVignette(float3 c, float2 uv)
{
	float2 d = (uv - 0.5) * float2(BUFFER_ASPECT_RATIO, 1.0);
	float r = length(d) / length(float2(BUFFER_ASPECT_RATIO, 1.0) * 0.5); // 0 center, 1 corner
	float v = smoothstep(Vignette_Start, 1.0, r);
	return c * (1.0 - Vignette_Amount * v);
}

float3 ApplyDither(float3 c, float2 pos)
{
	float t = frac(Timer * 0.0007) * 1000.0;
	float n = Hash12(pos + t) + Hash12(pos.yx + t + 113.0) - 1.0; // triangular, -1..1
	float lsb = 1.0 / (exp2(BUFFER_COLOR_BIT_DEPTH * 1.0) - 1.0);
	return c + n * lsb * Dither_Strength;
}

// ---------------------------------------------------------------------------------------------
// Pass
// ---------------------------------------------------------------------------------------------

float4 PS_Warriors(float4 vpos : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
	float4 src = tex2D(ReShade::BackBuffer, uv);
	if (Debug_SplitView && uv.x < 0.5)
		return src;

	float3 c = src.rgb;
	if (CAS_Enable)
		c = CAS_Sharpen(uv);
	if (Grade_Enable)
		c = ColorGrade(c);
	if (Vignette_Enable)
		c = ApplyVignette(c, uv);
	if (Grain_Enable)
		c = ApplyGrain(c, vpos.xy);
	if (Dither_Enable)
		c = ApplyDither(c, vpos.xy);

	return float4(saturate(c), src.a);
}

technique Warriors <
	ui_label = "Warriors look";
	ui_tooltip = "Sharpening, 1979-film night grade, film grain, vignette and dither for The Warriors PC port.\nEvery stage can be toggled in the effect settings.";
>
{
	pass
	{
		VertexShader = PostProcessVS;
		PixelShader = PS_Warriors;
	}
}
