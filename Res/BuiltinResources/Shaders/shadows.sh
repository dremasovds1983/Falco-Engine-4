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

// dz/du,dz/dv of the receiving surface in shadow texture space. Compute
// before choosing a cascade so derivatives do not cross divergent branches.
vec2 shadowReceiverGradient(vec4 shadowCoord)
{
	vec3 projected = shadowCoord.xyz / max(abs(shadowCoord.w), 0.000001);
	vec3 dx = dFdx(projected);
	vec3 dy = dFdy(projected);
	float jacobianDet = dx.x * dy.y - dx.y * dy.x;
	float scale = max(length(dx.xy) * length(dy.xy), 0.000000000001);
	if (abs(jacobianDet) <= scale * 0.0001)
		return vec2_splat(0.0);
	return vec2(dy.y * dx.z - dx.y * dy.z, dx.x * dy.z - dy.x * dx.z) / jacobianDet;
}

float PCF(sampler2DShadow _sampler, vec4 _shadowCoord, float _bias, vec4 _pcfParams, vec2 _texelSize, vec2 receiverGradient)
{
	vec2 texCoord = _shadowCoord.xy/_shadowCoord.w;
	float receiverDepth = _shadowCoord.z / _shadowCoord.w;
	bool outside = _shadowCoord.w <= 0.0 || receiverDepth < 0.0 || receiverDepth > 1.0
		|| any(greaterThan(texCoord, vec2_splat(1.0))) || any(lessThan(texCoord, vec2_splat(0.0)));
	if (outside)
		return 1.0;
	// The hardware comparison filter covers neighboring texel centers even
	// for one tap. Account for their depth change on a tilted receiver.
	float receiverBias = min(dot(abs(receiverGradient), _texelSize), 0.01);
	float compareBias = max(_bias, 0.0) + receiverBias;
	
	float result = 0.0;
	float samples = _pcfParams.x;
	
	if (samples > 1)
	{
		// Sample a symmetric grid in actual shadow-map texels. The old kernel
		// multiplied its already sample-dependent offsets by samples again,
		// producing an asymmetric, sparse pattern that preserved jagged edges.
		float first = -0.5 * (samples - 1.0);
		for (float ix = 0.0; ix < samples; ix += 1.0)
		{
			for (float iy = 0.0; iy < samples; iy += 1.0)
			{
				vec2 uvOffset = vec2(first + ix, first + iy) * _pcfParams.zw * _texelSize;
				float depthOffset = clamp(dot(receiverGradient, uvOffset), -0.01, 0.01);
				vec4 sampleOffset = vec4(uvOffset, depthOffset, 0.0) * _shadowCoord.w;
				result += hardShadow(_sampler, _shadowCoord + sampleOffset, compareBias);
			}
		}
		result /= (samples * samples);
	}
	else
		result = hardShadow(_sampler, _shadowCoord, compareBias);
	
	return result;
}

float computeVisibility(sampler2DShadow _sampler, vec4 _shadowCoord, float _bias, vec4 _samplingParams, vec2 _texelSize, vec2 receiverGradient)
{
	float visibility = 1.0;
	vec4 shadowcoord = _shadowCoord;
	
	visibility = PCF(_sampler, shadowcoord, _bias, _samplingParams, _texelSize, receiverGradient);

	return visibility;
}

float getShadow(int lightType,
				vec4 v_texcoord1,
				vec4 v_texcoord2,
				vec4 v_texcoord3,
				vec4 v_texcoord4,
				vec4 lightShadowBias,
				vec4 shadowSamplingParams,
				vec4 shadowCascadeSplits,
				float cameraDepth,
				vec3 lightPosition,
				float shadowMapTexelSize,
				sampler2DShadow shadowMap0,
				sampler2DShadow shadowMap1,
				sampler2DShadow shadowMap2,
				sampler2DShadow shadowMap3
				)
{
	//vec3 colorCoverage = vec3(1, 1, 1);
	float visibility = 1.0;
	
	if (lightType == 2) // Directional light shadow
	{
		vec2 texelSize = vec2_splat(shadowMapTexelSize);
		vec2 gradient0 = shadowReceiverGradient(v_texcoord1);
		vec2 gradient1 = shadowReceiverGradient(v_texcoord2);
		vec2 gradient2 = shadowReceiverGradient(v_texcoord3);
		vec2 gradient3 = shadowReceiverGradient(v_texcoord4);
		int cascadeCount = int(shadowSamplingParams.y + 0.5);

		// Each directional shadow map contains a distinct camera-depth slice.
		// Selecting by projected XY coverage can jump between unrelated slices
		// when the camera rotates, producing bright, view-dependent leak bands.
		if (cascadeCount >= 1 && cameraDepth <= shadowCascadeSplits.x)
		{
			vec4 shadowcoord = v_texcoord1;

			//float coverage = texcoordInRange(shadowcoord.xy/shadowcoord.w) * 0.4;
			//colorCoverage = vec3(-coverage, coverage, -coverage);
			visibility = computeVisibility(shadowMap0
							, shadowcoord
							, lightShadowBias.x
							, shadowSamplingParams
							, texelSize, gradient0
							);

			if (cascadeCount >= 2)
			{
				float transitionWidth = max((shadowCascadeSplits.y - shadowCascadeSplits.x) * 0.1, 0.25);
				float transition = clamp((cameraDepth - (shadowCascadeSplits.x - transitionWidth)) / transitionWidth, 0.0, 1.0);
				if (transition > 0.0)
				{
					float nextVisibility = computeVisibility(shadowMap1
								, v_texcoord2
								, lightShadowBias.y
								, shadowSamplingParams
								, texelSize, gradient1
								);
					visibility = mix(visibility, nextVisibility, transition);
				}
			}
		}
		else if (cascadeCount >= 2 && cameraDepth <= shadowCascadeSplits.y)
		{
			vec4 shadowcoord = v_texcoord2;

			//float coverage = texcoordInRange(shadowcoord.xy/shadowcoord.w) * 0.4;
			//colorCoverage = vec3(coverage, coverage, -coverage);
			visibility = computeVisibility(shadowMap1
							, shadowcoord
							, lightShadowBias.y
							, shadowSamplingParams
							, texelSize, gradient1
							);

			if (cascadeCount >= 3)
			{
				float transitionWidth = max((shadowCascadeSplits.z - shadowCascadeSplits.y) * 0.1, 0.25);
				float transition = clamp((cameraDepth - (shadowCascadeSplits.y - transitionWidth)) / transitionWidth, 0.0, 1.0);
				if (transition > 0.0)
				{
					float nextVisibility = computeVisibility(shadowMap2
								, v_texcoord3
								, lightShadowBias.z
								, shadowSamplingParams
								, texelSize, gradient2
								);
					visibility = mix(visibility, nextVisibility, transition);
				}
			}
		}
		else if (cascadeCount >= 3 && cameraDepth <= shadowCascadeSplits.z)
		{
			vec4 shadowcoord = v_texcoord3;

			//float coverage = texcoordInRange(shadowcoord.xy/shadowcoord.w) * 0.4;
			//colorCoverage = vec3(-coverage, -coverage, coverage);
			visibility = computeVisibility(shadowMap2
							, shadowcoord
							, lightShadowBias.z
							, shadowSamplingParams
							, texelSize, gradient2
							);

			if (cascadeCount >= 4)
			{
				float transitionWidth = max((shadowCascadeSplits.w - shadowCascadeSplits.z) * 0.1, 0.25);
				float transition = clamp((cameraDepth - (shadowCascadeSplits.z - transitionWidth)) / transitionWidth, 0.0, 1.0);
				if (transition > 0.0)
				{
					float nextVisibility = computeVisibility(shadowMap3
								, v_texcoord4
								, lightShadowBias.w
								, shadowSamplingParams
								, texelSize, gradient3
								);
					visibility = mix(visibility, nextVisibility, transition);
				}
			}
		}
		else if (cascadeCount >= 4 && cameraDepth <= shadowCascadeSplits.w)
		{
			vec4 shadowcoord = v_texcoord4;

			//float coverage = texcoordInRange(shadowcoord.xy/shadowcoord.w) * 0.4;
			//colorCoverage = vec3(coverage, -coverage, -coverage);
			visibility = computeVisibility(shadowMap3
							, shadowcoord
							, lightShadowBias.w
							, shadowSamplingParams
							, texelSize, gradient3
							);
		}
		else
		{
			visibility = 1.0;
		}
	}
	else if (lightType == 1) // Spot light shadow
	{
		vec2 texelSize = vec2_splat(shadowMapTexelSize);

		//float coverage = texcoordInRange(v_texcoord1.xy/v_texcoord1.w) * 0.3;
		//colorCoverage = vec3(coverage, -coverage, -coverage);

		visibility = computeVisibility(shadowMap0
						, v_texcoord1
						, lightShadowBias.x
						, shadowSamplingParams
						, texelSize, vec2_splat(0.0)
						);
	}
	else // Point light shadow
	{
		vec3 m_tetraNormalGreen = vec3(0.0, -0.57735026, 0.81649661);
		vec3 m_tetraNormalYellow = vec3(0.0, -0.57735026, -0.81649661);
		vec3 m_tetraNormalBlue = vec3(-0.81649661, 0.57735026, 0.0);
		vec3 m_tetraNormalRed = vec3(0.81649661, 0.57735026, 0.0);

		// Each atlas quadrant occupies half the UV extent and half the pixels,
		// therefore one physical atlas texel is still 1/fullMapSize in UV space.
		vec2 texelSize = vec2_splat(shadowMapTexelSize);

		vec4 faceSelection;
		vec3 pos = lightPosition;
		faceSelection.x = dot(m_tetraNormalGreen,  pos);
		faceSelection.y = dot(m_tetraNormalYellow, pos);
		faceSelection.z = dot(m_tetraNormalBlue,   pos);
		faceSelection.w = dot(m_tetraNormalRed,    pos);

		vec4 shadowcoord;
		float faceMax = max(max(faceSelection.x, faceSelection.y), max(faceSelection.z, faceSelection.w));
		if (faceSelection.x == faceMax)
		{
			shadowcoord = v_texcoord1;

			//float coverage = texcoordInRange(shadowcoord.xy/shadowcoord.w) * 0.3;
			//colorCoverage = vec3(-coverage, coverage, -coverage);
		}
		else if (faceSelection.y == faceMax)
		{
			shadowcoord = v_texcoord2;

			//float coverage = texcoordInRange(shadowcoord.xy/shadowcoord.w) * 0.3;
			//colorCoverage = vec3(coverage, coverage, -coverage);
		}
		else if (faceSelection.z == faceMax)
		{
			shadowcoord = v_texcoord3;

			//float coverage = texcoordInRange(shadowcoord.xy/shadowcoord.w) * 0.3;
			//colorCoverage = vec3(-coverage, -coverage, coverage);
		}
		else // (faceSelection.w == faceMax)
		{
			shadowcoord = v_texcoord4;

			//float coverage = texcoordInRange(shadowcoord.xy/shadowcoord.w) * 0.3;
			//colorCoverage = vec3(coverage, -coverage, -coverage);
		}

		visibility = computeVisibility(shadowMap0
						, shadowcoord
						, lightShadowBias.x
						, shadowSamplingParams
						, texelSize, vec2_splat(0.0)
						);
	}
	
	return visibility;
}
#endif
