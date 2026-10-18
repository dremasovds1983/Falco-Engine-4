#include <string>
#include "FXAACommon.h"

namespace shaders
{
	static std::string outlineFinalVarying =
		"vec2 v_texcoord0 : TEXCOORD0 = vec2(0.0, 0.0);\n"
		"vec3 v_position  :	TEXCOORD6 = vec3(0.0, 0.0, 0.0);\n"
		"vec3 v_cam       :	TEXCOORD7 = vec3(0.0, 0.0, 0.0);\n"
		"\n"
		"vec3 a_position : POSITION;\n"
		"vec2 a_texcoord0 : TEXCOORD0;\n"
		"\n"
		"vec2 v_rgbNW : TEXCOORD1 = vec2(0.0, 0.0);\n"
		"vec2 v_rgbNE : TEXCOORD2 = vec2(0.0, 0.0);\n"
		"vec2 v_rgbSW : TEXCOORD3 = vec2(0.0, 0.0);\n"
		"vec2 v_rgbSE : TEXCOORD4 = vec2(0.0, 0.0);\n"
		"vec2 v_rgbM  : TEXCOORD5 = vec2(0.0, 0.0);";

	static std::string outlineFinalVertex =
		"$input a_position, a_texcoord0\n"
		"$output v_texcoord0, v_position, v_cam, v_rgbNW, v_rgbNE, v_rgbSW, v_rgbSE, v_rgbM\n"
		"\n"
		"uniform mat4 u_modelViewProj;\n"
		"uniform mat4 u_invProj;\n"
		"uniform vec4 u_screenParams;\n"
		"\n"
		"void texcoords(vec2 fragCoord, vec2 resolution,\n"
		"	out vec2 resultNW, out vec2 resultNE,\n"
		"	out vec2 resultSW, out vec2 resultSE,\n"
		"	out vec2 resultM)\n"
		"{\n"
		"	vec2 inverseVP = 1.0 / resolution.xy;\n"
		"	resultNW = (fragCoord + vec2(-1.0, -1.0)) * inverseVP;\n"
		"	resultNE = (fragCoord + vec2(1.0, -1.0)) * inverseVP;\n"
		"	resultSW = (fragCoord + vec2(-1.0, 1.0)) * inverseVP;\n"
		"	resultSE = (fragCoord + vec2(1.0, 1.0)) * inverseVP;\n"
		"	resultM = vec2(fragCoord * inverseVP);\n"
		"}\n"
		"void main()\n"
		"{\n"
		"	gl_Position = mul(u_modelViewProj, vec4(a_position, 1.0));\n"
		"	v_texcoord0 = a_texcoord0;\n"
		"	v_position = gl_Position.xyz;\n"
		"	v_cam = mul(u_invProj, vec4(a_position, 1.0)).xyz;\n"
		"	\n"
		"	vec2 fragCoord = v_texcoord0 * u_screenParams.xy;\n"
		"	texcoords(fragCoord, u_screenParams.xy, v_rgbNW, v_rgbNE, v_rgbSW, v_rgbSE, v_rgbM);\n"
		"}\n";

    static std::string outlineFinalFragment = R"SHADER($input v_texcoord0, v_position, v_cam, v_rgbNW, v_rgbNE, v_rgbSW, v_rgbSE, v_rgbM

uniform sampler2D u_albedoMap;
uniform sampler2D u_depthMap;
uniform sampler2D u_forwardColor;
uniform sampler2D u_forwardDepth;
uniform sampler2D u_normalMap;
uniform sampler2D u_mraMap;
uniform vec4 u_outlineParams;
uniform vec4 u_outlineColor;
uniform vec4 u_screenParams;
uniform vec4 u_useFXAA;
uniform vec4 u_rtOrigin;
float _DepthThreshold = 6.0;
float _DepthNormalThreshold = 0.5;
float _DepthNormalThresholdScale = 7.0;
float _NormalThreshold = 0.4;

)SHADER" + fxaaFunctions + R"SHADER(float outlineMask(vec4 outlineSample)
{
	return outlineSample.a > 0.5 ? 1.0 : 0.0;
}

void main()
{
	float outlineRadius = u_outlineParams.z;
	float halfScaleFloor = floor(outlineRadius * 0.5);
	float halfScaleCeil = ceil(outlineRadius * 0.5);
	vec2 outlineUV = vec2(v_texcoord0.x, mix(v_texcoord0.y, 1.0-v_texcoord0.y, u_rtOrigin.x));
	if (u_outlineParams.w > 0.5)
	{
		float depth = texture2D(u_forwardDepth, outlineUV).r;
		vec3 diagnostic = vec3_splat(0.0);
		if (depth < 1.0)
		{
			vec4 mra = texture2D(u_mraMap, outlineUV);
			float mode = u_outlineParams.w;
			if (mode < 1.5) diagnostic = texture2D(u_forwardColor, outlineUV).rgb;
			else if (mode < 2.5) diagnostic = texture2D(u_normalMap, outlineUV).rgb;
			else if (mode < 3.5) diagnostic = vec3_splat(mra.x);
			else if (mode < 4.5) diagnostic = vec3_splat(mra.y);
			else if (mode < 5.5) diagnostic = vec3_splat(mra.z);
			else if (mode < 6.5) diagnostic = vec3_splat(depth);
			else
			{
				diagnostic = max(texture2D(u_forwardColor, outlineUV).rgb, vec3_splat(0.0));
				diagnostic /= vec3_splat(1.0) + diagnostic;
			}
			if (mode < 1.5 || mode > 6.5)
			{
				diagnostic = max(diagnostic, vec3_splat(0.0));
				diagnostic = mix(diagnostic * 12.92,
					1.055 * pow(diagnostic, vec3_splat(1.0 / 2.4)) - vec3_splat(0.055),
					step(vec3_splat(0.0031308), diagnostic));
			}
		}
		gl_FragColor = vec4(diagnostic, 1.0);
		return;
	}

	vec2 texelSize = u_outlineParams.xy;
	vec2 bottomLeftUV = outlineUV - texelSize * halfScaleFloor;
	vec2 topRightUV = outlineUV + texelSize * halfScaleCeil;
	vec2 bottomRightUV = outlineUV + vec2(texelSize.x * halfScaleCeil, -texelSize.y * halfScaleFloor);
	vec2 topLeftUV = outlineUV + vec2(-texelSize.x * halfScaleFloor, texelSize.y * halfScaleCeil);

	float depth0 = texture2D(u_forwardDepth, bottomLeftUV).r;
	float depth1 = texture2D(u_forwardDepth, topRightUV).r;
	float depth2 = texture2D(u_forwardDepth, bottomRightUV).r;
	float depth3 = texture2D(u_forwardDepth, topLeftUV).r;
	vec4 outlineColor0 = texture2D(u_forwardColor, bottomLeftUV);
	vec4 outlineColor1 = texture2D(u_forwardColor, topRightUV);
	vec4 outlineColor2 = texture2D(u_forwardColor, bottomRightUV);
	vec4 outlineColor3 = texture2D(u_forwardColor, topLeftUV);
	float selectedMask = outlineMask(texture2D(u_forwardColor, outlineUV));
	vec2 outlineStep = texelSize * outlineRadius;
	float maskLeft = outlineMask(texture2D(u_forwardColor, outlineUV - vec2(outlineStep.x, 0.0)));
	float maskRight = outlineMask(texture2D(u_forwardColor, outlineUV + vec2(outlineStep.x, 0.0)));
	float maskDown = outlineMask(texture2D(u_forwardColor, outlineUV - vec2(0.0, outlineStep.y)));
	float maskUp = outlineMask(texture2D(u_forwardColor, outlineUV + vec2(0.0, outlineStep.y)));
	float maskBottomLeft = outlineMask(texture2D(u_forwardColor, outlineUV - outlineStep));
	float maskTopRight = outlineMask(texture2D(u_forwardColor, outlineUV + outlineStep));
	float maskBottomRight = outlineMask(texture2D(u_forwardColor, outlineUV + vec2(outlineStep.x, -outlineStep.y)));
	float maskTopLeft = outlineMask(texture2D(u_forwardColor, outlineUV + vec2(-outlineStep.x, outlineStep.y)));
	float axialMask = max(max(maskLeft, maskRight), max(maskDown, maskUp));
	float diagonalMask = max(max(maskBottomLeft, maskTopRight), max(maskBottomRight, maskTopLeft));
	float silhouette = (1.0 - selectedMask) * max(axialMask, diagonalMask);

	vec3 normal0 = outlineColor0.rgb;
	vec3 normal1 = outlineColor1.rgb;
	vec3 normal2 = outlineColor2.rgb;
	vec3 normal3 = outlineColor3.rgb;

	vec3 viewNormal = normal0 * 2.0 - 1.0;
	vec3 viewSpaceDir = v_cam;
	float NdotV = 1 - dot(viewNormal, -viewSpaceDir);
	float normalThreshold01 = clamp((NdotV - _DepthNormalThreshold) / (1 - _DepthNormalThreshold), 0.0, 1.0);
	float normalThreshold = normalThreshold01 * _DepthNormalThresholdScale + 1;

	float depthFiniteDifference0 = depth1 - depth0;
	float depthFiniteDifference1 = depth3 - depth2;
	float outline = sqrt(pow(depthFiniteDifference0, 2) + pow(depthFiniteDifference1, 2)) * 100.0;
	float depthThreshold = _DepthThreshold * depth0 * normalThreshold;
	outline = outline > depthThreshold ? 1 : 0;

	vec3 normalFiniteDifference0 = normal1 - normal0;
	vec3 normalFiniteDifference1 = normal3 - normal2;
	float edgeNormal = sqrt(dot(normalFiniteDifference0, normalFiniteDifference0) + dot(normalFiniteDifference1, normalFiniteDifference1));
	edgeNormal = edgeNormal > _NormalThreshold ? 1 : 0;

	float edge = silhouette;

	vec2 fragCoord = v_texcoord0 * u_screenParams.xy;
	vec4 color = vec4_splat(0.0);
	if (u_useFXAA.x == 1.0)
		color = falcoFXAA(u_albedoMap, outlineUV, u_screenParams.xy);
	else
		color = texture2D(u_albedoMap, outlineUV);
	vec3 highlight = vec3_splat(0.0);

	float alpha = texture2D(u_albedoMap, outlineUV).a;
	float finalAlpha = max(alpha, edge);
	float outlineBlend = edge * u_outlineColor.a;
	vec3 finalColor = mix(color.rgb + highlight, u_outlineColor.rgb, outlineBlend);
	gl_FragColor = vec4(finalColor, finalAlpha);
}
)SHADER";
}
