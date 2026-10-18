#ifndef FALCO_COLOR_SPACE_SH
#define FALCO_COLOR_SPACE_SH
// Imported albedo and editor color swatches are sRGB; render targets and data
// maps are linear. Decode at the material input, never on G-buffer samples.
float falcoSRGBChannelToLinear(float x)
{
    x = max(x, 0.0);
    return x <= 0.04045 ? x / 12.92 : pow((x + 0.055) / 1.055, 2.4);
}
vec3 falcoSRGBToLinear(vec3 color)
{
    return vec3(falcoSRGBChannelToLinear(color.r), falcoSRGBChannelToLinear(color.g), falcoSRGBChannelToLinear(color.b));
}
vec4 falcoSRGBToLinear(vec4 color)
{
    return vec4(falcoSRGBToLinear(color.rgb), color.a);
}
#endif
