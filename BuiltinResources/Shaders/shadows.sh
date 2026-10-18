#ifndef __SHADOWS_SH__
#define __SHADOWS_SH__

#include "common.sh"
#include "shaderlib.sh"

float texcoordInRange(vec2 _texcoord)
{
	bool inRange = all(greaterThan(_texcoord, vec2_splat(0.0)))
				&& all(lessThan   (_texcoord, vec2_splat(1.0)));

	return float(inRange);
}

float hardShadow(sampler2DShadow _sampler, vec4 _shadowCoord, float _bias)
{
	vec3 coord = _shadowCoord.xyz / _shadowCoord.w;
	float visibility = shadow2D(_sampler, vec3(coord.xy, coord.z - _bias));

	return visibility;
}

float PCF(sampler2DShadow _sampler, vec4 _shadowCoord, float _bias, vec4 _pcfParams, vec2 _texelSize)
{
	vec2 texCoord = _shadowCoord.xy/_shadowCoord.w;
	bool outside = any(greaterThan(texCoord, vec2_splat(1.0))) || any(lessThan(texCoord, vec2_splat(0.0)));
	if (outside)
		return 1.0;
	
	float result = 0.0;
	vec2 offset = _pcfParams.zw * _texelSize * _shadowCoord.w;
	float samples = _pcfParams.x;
	
	if (samples > 1)
	{
		for(float x = -offset.x; x < offset.x; x += offset.x / (samples * 0.5))
		{
			for(float y = -offset.y; y < offset.y; y += offset.y / (samples * 0.5))
			{
				result += hardShadow(_sampler, _shadowCoord + vec4(vec2(x * samples, y * samples), 0.0, 0.0), _bias);
			}
		}
		result /= (samples * samples);
	}
	else
		result = hardShadow(_sampler, _shadowCoord, _bias);
	
	return result;
}

float computeVisibility(sampler2DShadow _sampler, vec4 _shadowCoord, float _bias, vec4 _samplingParams, vec2 _texelSize)
{
	float visibility = 1.0;
	vec4 shadowcoord = _shadowCoord;
	
	visibility = PCF(_sampler, shadowcoord, _bias, _samplingParams, _texelSize);

	return visibility;
}
#endif