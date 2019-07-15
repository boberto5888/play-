#include "GSH_OpenGL.h"
#include <assert.h>
#include <sstream>

#ifdef GLES_COMPATIBILITY
#define GLSL_VERSION "#version 300 es"
#else
#define GLSL_VERSION "#version 430"
#endif

enum FRAGMENT_SHADER_ORDERING_MODE
{
	FRAGMENT_SHADER_ORDERING_NONE = 0,
	FRAGMENT_SHADER_ORDERING_INTEL = 1,
	FRAGMENT_SHADER_ORDERING_NV = 2,
	FRAGMENT_SHADER_ORDERING_ARB = 3,
};

static const char* s_andFunction =
    "float and(int a, int b)\r\n"
    "{\r\n"
    "	int r = 0;\r\n"
    "	int ha, hb;\r\n"
    "	\r\n"
    "	int m = int(min(float(a), float(b)));\r\n"
    "	\r\n"
    "	for(int k = 1; k <= m; k *= 2)\r\n"
    "	{\r\n"
    "		ha = a / 2;\r\n"
    "		hb = b / 2;\r\n"
    "		if(((a - ha * 2) != 0) && ((b - hb * 2) != 0))\r\n"
    "		{\r\n"
    "			r += k;\r\n"
    "		}\r\n"
    "		a = ha;\r\n"
    "		b = hb;\r\n"
    "	}\r\n"
    "	\r\n"
    "	return float(r);\r\n"
    "}\r\n";

static const char* s_orFunction =
    "float or(int a, int b)\r\n"
    "{\r\n"
    "	int r = 0;\r\n"
    "	int ha, hb;\r\n"
    "	\r\n"
    "	int m = int(max(float(a), float(b)));\r\n"
    "	\r\n"
    "	for(int k = 1; k <= m; k *= 2)\r\n"
    "	{\r\n"
    "		ha = a / 2;\r\n"
    "		hb = b / 2;\r\n"
    "		if(((a - ha * 2) != 0) || ((b - hb * 2) != 0))\r\n"
    "		{\r\n"
    "			r += k;\r\n"
    "		}\r\n"
    "		a = ha;\r\n"
    "		b = hb;\r\n"
    "	}\r\n"
    "	\r\n"
    "	return float(r);\r\n"
    "}\r\n";

Framework::OpenGl::ProgramPtr CGSH_OpenGL::GenerateShader(const SHADERCAPS& caps)
{
	auto vertexShader = GenerateVertexShader(caps);
	auto fragmentShader = GenerateFragmentShader(caps);

	auto result = std::make_shared<Framework::OpenGl::CProgram>();

	result->AttachShader(vertexShader);
	result->AttachShader(fragmentShader);

	glBindAttribLocation(*result, static_cast<GLuint>(PRIM_VERTEX_ATTRIB::POSITION), "a_position");
	glBindAttribLocation(*result, static_cast<GLuint>(PRIM_VERTEX_ATTRIB::DEPTH), "a_depth");
	glBindAttribLocation(*result, static_cast<GLuint>(PRIM_VERTEX_ATTRIB::COLOR), "a_color");
	glBindAttribLocation(*result, static_cast<GLuint>(PRIM_VERTEX_ATTRIB::TEXCOORD), "a_texCoord");
	glBindAttribLocation(*result, static_cast<GLuint>(PRIM_VERTEX_ATTRIB::FOG), "a_fog");

	FRAMEWORK_MAYBE_UNUSED bool linkResult = result->Link();
	assert(linkResult);

	CHECKGLERROR();

	return result;
}

Framework::OpenGl::CShader CGSH_OpenGL::GenerateVertexShader(const SHADERCAPS& caps)
{
	std::stringstream shaderBuilder;
	shaderBuilder << GLSL_VERSION << std::endl;

	shaderBuilder << "layout(std140) uniform VertexParams" << std::endl;
	shaderBuilder << "{" << std::endl;
	shaderBuilder << "	mat4 g_projMatrix;" << std::endl;
	shaderBuilder << "	mat4 g_texMatrix;" << std::endl;
	shaderBuilder << "};" << std::endl;

	shaderBuilder << "in vec2 a_position;" << std::endl;
	shaderBuilder << "in uint a_depth;" << std::endl;
	shaderBuilder << "in vec4 a_color;" << std::endl;
	shaderBuilder << "in vec3 a_texCoord;" << std::endl;

	shaderBuilder << "out float v_depth;" << std::endl;
	shaderBuilder << "out vec4 v_color;" << std::endl;
	shaderBuilder << "out vec3 v_texCoord;" << std::endl;
	if(caps.hasFog)
	{
		shaderBuilder << "in float a_fog;" << std::endl;
		shaderBuilder << "out float v_fog;" << std::endl;
	}

	shaderBuilder << "void main()" << std::endl;
	shaderBuilder << "{" << std::endl;
	shaderBuilder << "	vec4 texCoord = g_texMatrix * vec4(a_texCoord, 1);" << std::endl;
	shaderBuilder << "	v_depth = float(a_depth) / 4294967296.0;" << std::endl;
	shaderBuilder << "	v_color = a_color;" << std::endl;
	shaderBuilder << "	v_texCoord = texCoord.xyz;" << std::endl;
	if(caps.hasFog)
	{
		shaderBuilder << "	v_fog = a_fog;" << std::endl;
	}
	shaderBuilder << "	gl_Position = g_projMatrix * vec4(a_position, 0, 1);" << std::endl;
	shaderBuilder << "}" << std::endl;

	auto shaderSource = shaderBuilder.str();

	Framework::OpenGl::CShader result(GL_VERTEX_SHADER);
	result.SetSource(shaderSource.c_str(), shaderSource.size());
	FRAMEWORK_MAYBE_UNUSED bool compilationResult = result.Compile();
	assert(compilationResult);

	CHECKGLERROR();

	return result;
}

Framework::OpenGl::CShader CGSH_OpenGL::GenerateFragmentShader(const SHADERCAPS& caps)
{
	std::stringstream shaderBuilder;

	auto orderingMode = FRAGMENT_SHADER_ORDERING_NONE;

	if(GLEW_ARB_fragment_shader_interlock)
	{
		orderingMode = FRAGMENT_SHADER_ORDERING_ARB;
	}
	//else if(GLEW_NV_fragment_shader_interlock)
	//{
	//	orderingMode = FRAGMENT_SHADER_ORDERING_NV;
	//}
	else if(GLEW_INTEL_fragment_shader_ordering)
	{
		orderingMode = FRAGMENT_SHADER_ORDERING_INTEL;
	}

	uint32 depthBits = [&]()
		{
			switch(caps.depthPsm)
			{
			case PSMZ32:
				return 32;
			case PSMZ24:
				return 24;
			case PSMZ16:
			case PSMZ16S:
				return 16;
			default:
				assert(false);
				return 0;
			}
		}();

	shaderBuilder << GLSL_VERSION << std::endl;

	switch(orderingMode)
	{
	case FRAGMENT_SHADER_ORDERING_ARB:
		shaderBuilder << "#extension GL_ARB_fragment_shader_interlock : enable" << std::endl;
		shaderBuilder << "layout(pixel_interlock_ordered) in;" << std::endl;
		break;
	case FRAGMENT_SHADER_ORDERING_INTEL:
		shaderBuilder << "#extension GL_INTEL_fragment_shader_ordering : enable" << std::endl;
		break;
	}

	shaderBuilder << "precision mediump float;" << std::endl;

	shaderBuilder << "in highp float v_depth;" << std::endl;
	shaderBuilder << "in vec4 v_color;" << std::endl;
	shaderBuilder << "in highp vec3 v_texCoord;" << std::endl;
	if(caps.hasFog)
	{
		shaderBuilder << "in float v_fog;" << std::endl;
	}

	shaderBuilder << "out vec4 fragColor;" << std::endl;

	shaderBuilder << GenerateMemoryAccessSection() << std::endl;

	shaderBuilder << "layout(binding = " << SHADER_IMAGE_CLUT << ", r32ui) readonly uniform uimage2D g_clut;" << std::endl;
	shaderBuilder << "layout(binding = " << SHADER_IMAGE_TEXTURE_SWIZZLE << ", r32ui) readonly uniform uimage2D g_textureSwizzleTable;" << std::endl;
	shaderBuilder << "layout(binding = " << SHADER_IMAGE_FRAME_SWIZZLE << ", r32ui) readonly uniform uimage2D g_frameSwizzleTable;" << std::endl;
	shaderBuilder << "layout(binding = " << SHADER_IMAGE_DEPTH_SWIZZLE << ", r32ui) readonly uniform uimage2D g_depthSwizzleTable;" << std::endl;
	//shaderBuilder << "uniform sampler2D g_texture;" << std::endl;
	//shaderBuilder << "uniform sampler2D g_palette;" << std::endl;

	shaderBuilder << "layout(std140) uniform FragmentParams" << std::endl;
	shaderBuilder << "{" << std::endl;
	shaderBuilder << "	vec2 g_textureSize;" << std::endl;
	shaderBuilder << "	vec2 g_texelSize;" << std::endl;
	shaderBuilder << "	vec2 g_clampMin;" << std::endl;
	shaderBuilder << "	vec2 g_clampMax;" << std::endl;
	shaderBuilder << "	float g_texA0;" << std::endl;
	shaderBuilder << "	float g_texA1;" << std::endl;
	shaderBuilder << "	uint g_alphaRef;" << std::endl;
	shaderBuilder << "	uint g_depthMask;" << std::endl;
	shaderBuilder << "	vec3 g_fogColor;" << std::endl;
	shaderBuilder << "	uint g_alphaFix;" << std::endl;
	shaderBuilder << "	uint g_colorMask;" << std::endl;
	shaderBuilder << "	uint g_textureBufPtr;" << std::endl;
	shaderBuilder << "	uint g_textureBufWidth;" << std::endl;
	shaderBuilder << "	uint g_textureCsa;" << std::endl;
	shaderBuilder << "	uint g_frameBufPtr;" << std::endl;
	shaderBuilder << "	uint g_frameBufWidth;" << std::endl;
	shaderBuilder << "	uint g_depthBufPtr;" << std::endl;
	shaderBuilder << "	uint g_depthBufWidth;" << std::endl;
	shaderBuilder << "};" << std::endl;

	if(caps.texClampS == TEXTURE_CLAMP_MODE_REGION_REPEAT || caps.texClampT == TEXTURE_CLAMP_MODE_REGION_REPEAT)
	{
		shaderBuilder << s_andFunction << std::endl;
		shaderBuilder << s_orFunction << std::endl;
	}

	shaderBuilder << "float combineColors(float a, float b)" << std::endl;
	shaderBuilder << "{" << std::endl;
	shaderBuilder << "	uint aInt = uint(a * 255.0);" << std::endl;
	shaderBuilder << "	uint bInt = uint(b * 255.0);" << std::endl;
	shaderBuilder << "	uint result = min((aInt * bInt) >> 7, 255u);" << std::endl;
	shaderBuilder << "	return float(result) / 255.0;" << std::endl;
	shaderBuilder << "}" << std::endl;

	shaderBuilder << "vec4 expandAlpha(vec4 inputColor)" << std::endl;
	shaderBuilder << "{" << std::endl;
	if(caps.texUseAlphaExpansion)
	{
		shaderBuilder << "	float alpha = mix(g_texA0, g_texA1, inputColor.a);" << std::endl;
		if(caps.texBlackIsTransparent)
		{
			shaderBuilder << "	float black = inputColor.r + inputColor.g + inputColor.b;" << std::endl;
			shaderBuilder << "	if(black == 0.0) alpha = 0.0;" << std::endl;
		}
		shaderBuilder << "	return vec4(inputColor.rgb, alpha);" << std::endl;
	}
	else
	{
		shaderBuilder << "	return inputColor;" << std::endl;
	}
	shaderBuilder << "}" << std::endl;

	shaderBuilder << "void main()" << std::endl;
	shaderBuilder << "{" << std::endl;

	shaderBuilder << "	uint depth = uint(v_depth * 4294967296.0);" << std::endl;

	shaderBuilder << "	bool depthTestFail = false;" << std::endl;
	shaderBuilder << "	bool alphaTestFail = false;" << std::endl;

	shaderBuilder << "	highp vec3 texCoord = v_texCoord;" << std::endl;
	shaderBuilder << "	texCoord.st /= texCoord.p;" << std::endl;

	if((caps.texClampS != TEXTURE_CLAMP_MODE_STD) || (caps.texClampT != TEXTURE_CLAMP_MODE_STD))
	{
		shaderBuilder << "	texCoord.st *= g_textureSize.st;" << std::endl;
		shaderBuilder << GenerateTexCoordClampingSection(static_cast<TEXTURE_CLAMP_MODE>(caps.texClampS), "s");
		shaderBuilder << GenerateTexCoordClampingSection(static_cast<TEXTURE_CLAMP_MODE>(caps.texClampT), "t");
		shaderBuilder << "	texCoord.st /= g_textureSize.st;" << std::endl;
	}

	shaderBuilder << "	vec4 textureColor = vec4(1, 1, 1, 1);" << std::endl;
#if 0
	if(caps.isIndexedTextureSource())
	{
		if(!caps.texBilinearFilter)
		{
			shaderBuilder << "	float colorIndex = texture(g_texture, texCoord.st).r * 255.0;" << std::endl;
			if(caps.texSourceMode == TEXTURE_SOURCE_MODE_IDX4)
			{
				shaderBuilder << "	float paletteTexelBias = 0.5 / 16.0;" << std::endl;
				shaderBuilder << "	textureColor = expandAlpha(texture(g_palette, vec2(colorIndex / 16.0 + paletteTexelBias, 0)));" << std::endl;
			}
			else if(caps.texSourceMode == TEXTURE_SOURCE_MODE_IDX8)
			{
				shaderBuilder << "	float paletteTexelBias = 0.5 / 256.0;" << std::endl;
				shaderBuilder << "	textureColor = expandAlpha(texture(g_palette, vec2(colorIndex / 256.0 + paletteTexelBias, 0)));" << std::endl;
			}
		}
		else
		{
			shaderBuilder << "	float tlIdx = texture(g_texture, texCoord.st                                     ).r * 255.0;" << std::endl;
			shaderBuilder << "	float trIdx = texture(g_texture, texCoord.st + vec2(g_texelSize.x, 0)            ).r * 255.0;" << std::endl;
			shaderBuilder << "	float blIdx = texture(g_texture, texCoord.st + vec2(0, g_texelSize.y)            ).r * 255.0;" << std::endl;
			shaderBuilder << "	float brIdx = texture(g_texture, texCoord.st + vec2(g_texelSize.x, g_texelSize.y)).r * 255.0;" << std::endl;

			if(caps.texSourceMode == TEXTURE_SOURCE_MODE_IDX4)
			{
				shaderBuilder << "	float paletteTexelBias = 0.5 / 16.0;" << std::endl;
				shaderBuilder << "	vec4 tl = expandAlpha(texture(g_palette, vec2(tlIdx / 16.0 + paletteTexelBias, 0)));" << std::endl;
				shaderBuilder << "	vec4 tr = expandAlpha(texture(g_palette, vec2(trIdx / 16.0 + paletteTexelBias, 0)));" << std::endl;
				shaderBuilder << "	vec4 bl = expandAlpha(texture(g_palette, vec2(blIdx / 16.0 + paletteTexelBias, 0)));" << std::endl;
				shaderBuilder << "	vec4 br = expandAlpha(texture(g_palette, vec2(brIdx / 16.0 + paletteTexelBias, 0)));" << std::endl;
			}
			else if(caps.texSourceMode == TEXTURE_SOURCE_MODE_IDX8)
			{
				shaderBuilder << "	float paletteTexelBias = 0.5 / 256.0;" << std::endl;
				shaderBuilder << "	vec4 tl = expandAlpha(texture(g_palette, vec2(tlIdx / 256.0 + paletteTexelBias, 0)));" << std::endl;
				shaderBuilder << "	vec4 tr = expandAlpha(texture(g_palette, vec2(trIdx / 256.0 + paletteTexelBias, 0)));" << std::endl;
				shaderBuilder << "	vec4 bl = expandAlpha(texture(g_palette, vec2(blIdx / 256.0 + paletteTexelBias, 0)));" << std::endl;
				shaderBuilder << "	vec4 br = expandAlpha(texture(g_palette, vec2(brIdx / 256.0 + paletteTexelBias, 0)));" << std::endl;
			}

			shaderBuilder << "	highp vec2 f = fract(texCoord.st * g_textureSize);" << std::endl;
			shaderBuilder << "	vec4 tA = mix(tl, tr, f.x);" << std::endl;
			shaderBuilder << "	vec4 tB = mix(bl, br, f.x);" << std::endl;
			shaderBuilder << "	textureColor = mix(tA, tB, f.y);" << std::endl;
		}
	}
	else if(caps.texSourceMode == TEXTURE_SOURCE_MODE_STD)
#endif
	//if(caps.texSourceMode != TEXTURE_SOURCE_MODE_NONE)
	if(caps.texSourceMode != TEXTURE_SOURCE_MODE_NONE)
	{
		shaderBuilder << "	uvec2 imageCoord = uvec2(texCoord.st * g_textureSize.st);" << std::endl;
		if(caps.texPsm == PSMCT32 || caps.texPsm == PSMCT24)
		{
			shaderBuilder << "	uint textureAddress = GetPixelAddress_PSMCT32(g_textureBufPtr, g_textureBufWidth, g_textureSwizzleTable, imageCoord);" << std::endl;
			shaderBuilder << "	uint pixel = Memory_Read32(textureAddress);" << std::endl;
			shaderBuilder << "	textureColor = PSM32ToVec4(pixel);" << std::endl;
		}
		else if(caps.texPsm == PSMCT16 || caps.texPsm == PSMCT16S)
		{
			shaderBuilder << "	uint textureAddress = GetPixelAddress_PSMCT16(g_textureBufPtr, g_textureBufWidth, g_textureSwizzleTable, imageCoord);" << std::endl;
			shaderBuilder << "	uint pixel = Memory_Read16(textureAddress);" << std::endl;
			shaderBuilder << "	textureColor = PSM16ToVec4(pixel);" << std::endl;
		}
		else if(caps.texPsm == PSMT8)
		{
			shaderBuilder << "	uint textureAddress = GetPixelAddress_PSMT8(g_textureBufPtr, g_textureBufWidth, g_textureSwizzleTable, imageCoord);" << std::endl;
			shaderBuilder << "	uint colorIndex = Memory_Read8(textureAddress);" << std::endl;
			if(caps.texCpsm == PSMCT32)
			{
				shaderBuilder << "	uint colorLo = imageLoad(g_clut, ivec2(colorIndex + 0x000, 0)).r;" << std::endl;
				shaderBuilder << "	uint colorHi = imageLoad(g_clut, ivec2(colorIndex + 0x100, 0)).r;" << std::endl;
				shaderBuilder << "	textureColor = PSM32ToVec4(colorLo | (colorHi << 16));" << std::endl;
			}
			else
			{
				assert(false);
			}
		}
		else if(caps.texPsm == PSMT4)
		{
			shaderBuilder << "	uint textureAddress = GetPixelAddress_PSMT4(g_textureBufPtr, g_textureBufWidth, g_textureSwizzleTable, imageCoord);" << std::endl;
			shaderBuilder << "	uint pixel = Memory_Read4(textureAddress, 0);" << std::endl;
			if(caps.texCpsm == PSMCT32)
			{
				shaderBuilder << "	uint colorIndex = (g_textureCsa * 16) + pixel;" << std::endl;
				shaderBuilder << "	uint colorLo = imageLoad(g_clut, ivec2(colorIndex + 0x000, 0)).r;" << std::endl;
				shaderBuilder << "	uint colorHi = imageLoad(g_clut, ivec2(colorIndex + 0x100, 0)).r;" << std::endl;
				shaderBuilder << "	textureColor = PSM32ToVec4(colorLo | (colorHi << 16));" << std::endl;
			}
			else
			{
				assert(false);
			}
		}
		else
		{
			assert(false);
		}
	}

	if(caps.texSourceMode != TEXTURE_SOURCE_MODE_NONE)
	{
		if(!caps.texHasAlpha)
		{
			shaderBuilder << "	textureColor.a = 1.0;" << std::endl;
		}

		switch(caps.texFunction)
		{
		case TEX0_FUNCTION_MODULATE:
			shaderBuilder << "	textureColor.rgb = clamp(textureColor.rgb * v_color.rgb * 2.0, 0.0, 1.0);" << std::endl;
			if(!caps.texHasAlpha)
			{
				shaderBuilder << "	textureColor.a = v_color.a;" << std::endl;
			}
			else
			{
				shaderBuilder << "	textureColor.a = combineColors(textureColor.a, v_color.a);" << std::endl;
			}
			break;
		case TEX0_FUNCTION_DECAL:
			break;
		case TEX0_FUNCTION_HIGHLIGHT:
			shaderBuilder << "	textureColor.rgb = clamp(textureColor.rgb * v_color.rgb * 2.0, 0.0, 1.0) + v_color.aaa;" << std::endl;
			if(!caps.texHasAlpha)
			{
				shaderBuilder << "	textureColor.a = v_color.a;" << std::endl;
			}
			else
			{
				shaderBuilder << "	textureColor.a += v_color.a;" << std::endl;
			}
			break;
		case TEX0_FUNCTION_HIGHLIGHT2:
			shaderBuilder << "	textureColor.rgb = clamp(textureColor.rgb * v_color.rgb * 2.0, 0.0, 1.0) + v_color.aaa;" << std::endl;
			if(!caps.texHasAlpha)
			{
				shaderBuilder << "	textureColor.a = v_color.a;" << std::endl;
			}
			break;
		default:
			assert(0);
			break;
		}
	}
	else
	{
		shaderBuilder << "	textureColor = v_color;" << std::endl;
	}

	if(caps.hasAlphaTest)
	{
		shaderBuilder << GenerateAlphaTestSection(static_cast<ALPHA_TEST_METHOD>(caps.alphaTestMethod));
		//Check for early rejection
		if(caps.alphaFailResult == ALPHA_TEST_FAIL_KEEP)
		{
			shaderBuilder << "	if(alphaTestFail) discard;" << std::endl;
		}
	}

	if(caps.hasFog)
	{
		shaderBuilder << "	fragColor.xyz = mix(textureColor.rgb, g_fogColor, v_fog);" << std::endl;
	}
	else
	{
		shaderBuilder << "	fragColor.xyz = textureColor.xyz;" << std::endl;
	}

	//For proper alpha blending, alpha has to be multiplied by 2 (0x80 -> 1.0)
#ifdef GLES_COMPATIBILITY
	//This has the side effect of not writing a proper value in the framebuffer (should write alpha "as is")
	shaderBuilder << "	fragColor.a = clamp(textureColor.a * 2.0, 0.0, 1.0);" << std::endl;
#else
	shaderBuilder << "	fragColor.a = textureColor.a;" << std::endl;
#endif

	shaderBuilder << "	ivec2 pixelPosition = ivec2(gl_FragCoord.xy);" << std::endl;
	if(caps.framePsm == PSMCT32 || caps.framePsm == PSMCT24)
	{
		shaderBuilder << "	uint frameAddress = GetPixelAddress_PSMCT32(g_frameBufPtr, g_frameBufWidth, g_frameSwizzleTable, pixelPosition);" << std::endl;
	}
	else if(caps.framePsm == PSMCT16 || caps.framePsm == PSMCT16S)
	{
		shaderBuilder << "	uint frameAddress = GetPixelAddress_PSMCT16(g_frameBufPtr, g_frameBufWidth, g_frameSwizzleTable, pixelPosition);" << std::endl;
	}
	else
	{
		assert(false);
	}

	switch(depthBits)
	{
	case 32:
	case 24:
		shaderBuilder << "	uint depthAddress = GetPixelAddress_PSMCT32(g_depthBufPtr, g_depthBufWidth, g_depthSwizzleTable, pixelPosition);" << std::endl;
		break;
	case 16:
		shaderBuilder << "	uint depthAddress = GetPixelAddress_PSMCT16(g_depthBufPtr, g_depthBufWidth, g_depthSwizzleTable, pixelPosition);" << std::endl;
		break;
	default:
		assert(false);
		break;
	}

	switch(orderingMode)
	{
	case FRAGMENT_SHADER_ORDERING_ARB:
		shaderBuilder << "	beginInvocationInterlockARB();" << std::endl;
		break;
	case FRAGMENT_SHADER_ORDERING_INTEL:
		shaderBuilder << "	beginFragmentShaderOrderingINTEL();" << std::endl;
		break;
	}

	if((caps.depthTestMethod == DEPTH_TEST_GEQUAL) || (caps.depthTestMethod == DEPTH_TEST_GREATER))
	{
		switch(depthBits)
		{
		case 32:
			shaderBuilder << "	uint dstDepth = Memory_Read32(depthAddress);" << std::endl;
			break;
		case 16:
			shaderBuilder << "	uint dstDepth = Memory_Read16(depthAddress);" << std::endl;
			break;
		default:
			assert(false);
			break;
		}
	}

	//Depth test
	switch(caps.depthTestMethod)
	{
	case DEPTH_TEST_ALWAYS:
		break;
	case DEPTH_TEST_NEVER:
		shaderBuilder << "	depthTestFail = true;" << std::endl;
		break;
	case DEPTH_TEST_GEQUAL:
		shaderBuilder << "	depthTestFail = (depth < dstDepth);" << std::endl;
		break;
	case DEPTH_TEST_GREATER:
		shaderBuilder << "	depthTestFail = (depth <= dstDepth);" << std::endl;
		break;
	}

	//Update depth buffer
	if(caps.depthWriteEnabled)
	{
		const char* depthWriteCondition = "	if(!depthTestFail)";
		if(caps.hasAlphaTest && (caps.alphaFailResult == ALPHA_TEST_FAIL_FBONLY))
		{
			depthWriteCondition = "	if(!depthTestFail && !alphaTestFail)";
		}
		shaderBuilder << depthWriteCondition << std::endl;
		shaderBuilder << "	{" << std::endl;
		switch(depthBits)
		{
		case 32:
			shaderBuilder << "		Memory_Write32(depthAddress);" << std::endl;
			break;
		case 16:
			shaderBuilder << "		Memory_Write16(depthAddress, depth & 0xFFFF);" << std::endl;
			break;
		default:
			assert(false);
			break;
		}
		shaderBuilder << "	}" << std::endl;
	}

	const char* colorWriteCondition = "	if(!depthTestFail)";
	if(caps.hasAlphaTest && (caps.alphaFailResult == ALPHA_TEST_FAIL_ZBONLY))
	{
		colorWriteCondition = "	if(!depthTestFail && !alphaTestFail)";
	}

	shaderBuilder << colorWriteCondition << std::endl;
	shaderBuilder << "	{" << std::endl;

	if((caps.framePsm == PSMCT32) || (caps.framePsm == PSMCT24))
	{
		shaderBuilder << "		uint dstPixel = Memory_Read32(frameAddress);" << std::endl;
		shaderBuilder << "		vec4 dstColor = PSM32ToVec4(dstPixel);" << std::endl;
	}
	else if((caps.framePsm == PSMCT16) || (caps.framePsm == PSMCT16S))
	{
		shaderBuilder << "		uint dstPixel = Memory_Read16(frameAddress);" << std::endl;
		shaderBuilder << "		vec4 dstColor = PSM16ToVec4(dstPixel);" << std::endl;
	}
	else
	{
		assert(false);
	}

	if(caps.hasAlphaBlend)
	{
		shaderBuilder << "		vec3 colorA = " << GenerateAlphaBlendABDValue(static_cast<ALPHABLEND_ABD>(caps.blendFactorA)) << ";" << std::endl;
		shaderBuilder << "		vec3 colorB = " << GenerateAlphaBlendABDValue(static_cast<ALPHABLEND_ABD>(caps.blendFactorB)) << ";" << std::endl;
		shaderBuilder << "		vec3 colorD = " << GenerateAlphaBlendABDValue(static_cast<ALPHABLEND_ABD>(caps.blendFactorD)) << ";" << std::endl;
		shaderBuilder << "		float alphaC = " << GenerateAlphaBlendCValue(static_cast<ALPHABLEND_C>(caps.blendFactorC)) << ";" << std::endl;
		shaderBuilder << "		fragColor.xyz = ((colorA - colorB) * alphaC * 2) + colorD;" << std::endl;
	}

	if(caps.hasAlphaTest && (caps.alphaFailResult == ALPHA_TEST_FAIL_RGBONLY))
	{
		shaderBuilder << "		if(alphaTestFail) fragColor.a = dstColor.a;" << std::endl;
	}

	shaderBuilder << "		if((g_colorMask & 0xFF000000) == 0) fragColor.a = dstColor.a;" << std::endl;
	shaderBuilder << "		if((g_colorMask & 0x00FF0000) == 0) fragColor.b = dstColor.b;" << std::endl;
	shaderBuilder << "		if((g_colorMask & 0x0000FF00) == 0) fragColor.g = dstColor.g;" << std::endl;
	shaderBuilder << "		if((g_colorMask & 0x000000FF) == 0) fragColor.r = dstColor.r;" << std::endl;

	if(caps.framePsm == PSMCT24)
	{
		shaderBuilder << "		fragColor.a = dstColor.a;" << std::endl;
	}

	shaderBuilder << "		fragColor = clamp(fragColor, 0, 1);" << std::endl;

	if((caps.framePsm == PSMCT32) || (caps.framePsm == PSMCT24))
	{
		shaderBuilder << "		uint pixel = Vec4ToPSM32(fragColor);" << std::endl;
		shaderBuilder << "		Memory_Write32(frameAddress, pixel);" << std::endl;
	}
	else if((caps.framePsm == PSMCT16) || (caps.framePsm == PSMCT16S))
	{
		shaderBuilder << "		uint pixel = Vec4ToPSM16(fragColor);" << std::endl;
		shaderBuilder << "		Memory_Write16(frameAddress, pixel);" << std::endl;
	}

	shaderBuilder << "	}" << std::endl;

	switch(orderingMode)
	{
	case FRAGMENT_SHADER_ORDERING_ARB:
		shaderBuilder << "	endInvocationInterlockARB();" << std::endl;
		break;
	}

	shaderBuilder << "	discard;" << std::endl;

	shaderBuilder << "}" << std::endl;

	auto shaderSource = shaderBuilder.str();

	Framework::OpenGl::CShader result(GL_FRAGMENT_SHADER);
	result.SetSource(shaderSource.c_str(), shaderSource.size());
	FRAMEWORK_MAYBE_UNUSED bool compilationResult = result.Compile();
	assert(compilationResult);

	CHECKGLERROR();

	return result;
}

std::string CGSH_OpenGL::GenerateTexCoordClampingSection(TEXTURE_CLAMP_MODE clampMode, const char* coordinate)
{
	std::stringstream shaderBuilder;

	switch(clampMode)
	{
	case TEXTURE_CLAMP_MODE_REGION_CLAMP:
		shaderBuilder << "	texCoord." << coordinate << " = min(g_clampMax." << coordinate << ", "
		              << "max(g_clampMin." << coordinate << ", texCoord." << coordinate << "));" << std::endl;
		break;
	case TEXTURE_CLAMP_MODE_REGION_REPEAT:
		shaderBuilder << "	texCoord." << coordinate << " = or(int(and(int(texCoord." << coordinate << "), "
		              << "int(g_clampMin." << coordinate << "))), int(g_clampMax." << coordinate << "));";
		break;
	case TEXTURE_CLAMP_MODE_REGION_REPEAT_SIMPLE:
		shaderBuilder << "	texCoord." << coordinate << " = mod(texCoord." << coordinate << ", "
		              << "g_clampMin." << coordinate << ") + g_clampMax." << coordinate << ";" << std::endl;
		break;
	}

	std::string shaderSource = shaderBuilder.str();
	return shaderSource;
}

std::string CGSH_OpenGL::GenerateAlphaBlendABDValue(ALPHABLEND_ABD factor)
{
	switch(factor)
	{
	case ALPHABLEND_ABD_CS:
		return "fragColor.xyz";
		break;
	case ALPHABLEND_ABD_CD:
		return "dstColor.xyz";
		break;
	case ALPHABLEND_ABD_ZERO:
		return "vec3(0, 0, 0)";
		break;
	case ALPHABLEND_ABD_INVALID:
		assert(false);
		return "vec3(0, 0, 0)";
		break;
	}
}

std::string CGSH_OpenGL::GenerateAlphaBlendCValue(ALPHABLEND_C factor)
{
	switch(factor)
	{
	case ALPHABLEND_C_AS:
		return "fragColor.a";
		break;
	case ALPHABLEND_C_AD:
		return "dstColor.a";
		break;
	case ALPHABLEND_C_FIX:
		return "float(g_alphaFix) / 255.0";
		break;
	case ALPHABLEND_C_INVALID:
		assert(false);
		return "0";
		break;
	}
}

std::string CGSH_OpenGL::GenerateAlphaTestSection(ALPHA_TEST_METHOD testMethod)
{
	std::stringstream shaderBuilder;

	const char* test = "	alphaTestFail = false;";

	//testMethod is the condition to pass the test
	switch(testMethod)
	{
	case ALPHA_TEST_NEVER:
		test = "	alphaTestFail = true;";
		break;
	case ALPHA_TEST_ALWAYS:
		test = "	alphaTestFail = false;";
		break;
	case ALPHA_TEST_LESS:
		test = "	alphaTestFail = (textureColorAlphaInt >= g_alphaRef);";
		break;
	case ALPHA_TEST_LEQUAL:
		test = "	alphaTestFail = (textureColorAlphaInt > g_alphaRef);";
		break;
	case ALPHA_TEST_EQUAL:
		test = "	alphaTestFail = (textureColorAlphaInt != g_alphaRef);";
		break;
	case ALPHA_TEST_GEQUAL:
		test = "	alphaTestFail = (textureColorAlphaInt < g_alphaRef);";
		break;
	case ALPHA_TEST_GREATER:
		test = "	alphaTestFail = (textureColorAlphaInt <= g_alphaRef);";
		break;
	case ALPHA_TEST_NOTEQUAL:
		test = "	alphaTestFail = (textureColorAlphaInt == g_alphaRef);";
		break;
	default:
		assert(false);
		break;
	}

	shaderBuilder << "	uint textureColorAlphaInt = uint(textureColor.a * 255.0);" << std::endl;
	shaderBuilder << test << std::endl;

	std::string shaderSource = shaderBuilder.str();
	return shaderSource;
}

std::string CGSH_OpenGL::GenerateMemoryAccessSection()
{
	std::stringstream shaderBuilder;

	shaderBuilder << "layout(binding = " << SHADER_IMAGE_MEMORY << ", r32ui) uniform uimage2D g_memory;" << std::endl;
	shaderBuilder << "const uint c_memorySize = 1024;" << std::endl;

	shaderBuilder << "void Memory_Write32(uint address, uint value)" << std::endl;
	shaderBuilder << "{" << std::endl;
	shaderBuilder << "	uint wordAddress = address / 4;" << std::endl;
	shaderBuilder << "	ivec2 coords = ivec2(wordAddress % c_memorySize, wordAddress / c_memorySize);" << std::endl;
	shaderBuilder << "	imageStore(g_memory, coords, uvec4(value));" << std::endl;
	shaderBuilder << "}" << std::endl;

	shaderBuilder << "void Memory_Write16(uint address, uint value)" << std::endl;
	shaderBuilder << "{" << std::endl;
	shaderBuilder << "	uint wordAddress = address / 4;" << std::endl;
	shaderBuilder << "	uint shiftAmount = (address & 2) * 8;" << std::endl;
	shaderBuilder << "	uint mask = 0xFFFFFFFF ^ (0xFFFF << shiftAmount);" << std::endl;
	shaderBuilder << "	uint valueWord = value << shiftAmount;" << std::endl;
	shaderBuilder << "	ivec2 coords = ivec2(wordAddress % c_memorySize, wordAddress / c_memorySize);" << std::endl;
	shaderBuilder << "	imageAtomicAnd(g_memory, coords, mask);" << std::endl;
	shaderBuilder << "	imageAtomicOr(g_memory, coords, valueWord);" << std::endl;
	shaderBuilder << "}" << std::endl;

	shaderBuilder << "void Memory_Write8(uint address, uint value)" << std::endl;
	shaderBuilder << "{" << std::endl;
	shaderBuilder << "	uint wordAddress = address / 4;" << std::endl;
	shaderBuilder << "	uint shiftAmount = (address & 3) * 8;" << std::endl;
	shaderBuilder << "	uint mask = 0xFFFFFFFF ^ (0xFF << shiftAmount);" << std::endl;
	shaderBuilder << "	uint valueWord = value << shiftAmount;" << std::endl;
	shaderBuilder << "	ivec2 coords = ivec2(wordAddress % c_memorySize, wordAddress / c_memorySize);" << std::endl;
	shaderBuilder << "	imageAtomicAnd(g_memory, coords, mask);" << std::endl;
	shaderBuilder << "	imageAtomicOr(g_memory, coords, valueWord);" << std::endl;
	shaderBuilder << "}" << std::endl;

	shaderBuilder << "void Memory_Write4(uint address, uint nibIndex, uint value)" << std::endl;
	shaderBuilder << "{" << std::endl;
	shaderBuilder << "	uint wordAddress = address / 4;";
	shaderBuilder << "	uint shiftAmount = ((address & 3) * 2 + nibIndex) * 4;" << std::endl;
	shaderBuilder << "	uint mask = 0xFFFFFFFF ^ (0xF << shiftAmount);" << std::endl;
	shaderBuilder << "	uint valueWord = value << shiftAmount;" << std::endl;
	shaderBuilder << "	ivec2 coords = ivec2(wordAddress % c_memorySize, wordAddress / c_memorySize);" << std::endl;
	shaderBuilder << "	imageAtomicAnd(g_memory, coords, mask);" << std::endl;
	shaderBuilder << "	imageAtomicOr(g_memory, coords, valueWord);" << std::endl;
	shaderBuilder << "}" << std::endl;

	shaderBuilder << "uint Memory_Read32(uint address)" << std::endl;
	shaderBuilder << "{" << std::endl;
	shaderBuilder << "	uint wordAddress = address / 4;" << std::endl;
	shaderBuilder << "	ivec2 coords = ivec2(wordAddress % c_memorySize, wordAddress / c_memorySize);" << std::endl;
	shaderBuilder << "	return imageLoad(g_memory, coords).r;" << std::endl;
	shaderBuilder << "}" << std::endl;

	shaderBuilder << "uint Memory_Read16(uint address)" << std::endl;
	shaderBuilder << "{" << std::endl;
	shaderBuilder << "	uint wordAddress = address / 4;" << std::endl;
	shaderBuilder << "	uint shiftAmount = (address & 2) * 8;" << std::endl;
	shaderBuilder << "	ivec2 coords = ivec2(wordAddress % c_memorySize, wordAddress / c_memorySize);" << std::endl;
	shaderBuilder << "	uint pixel = imageLoad(g_memory, coords).r;" << std::endl;
	shaderBuilder << "	return (pixel >> shiftAmount) & 0xFFFF;" << std::endl;
	shaderBuilder << "}" << std::endl;

	shaderBuilder << "uint Memory_Read8(uint address)" << std::endl;
	shaderBuilder << "{" << std::endl;
	shaderBuilder << "	uint wordAddress = address / 4;" << std::endl;
	shaderBuilder << "	uint shiftAmount = (address & 3) * 8;" << std::endl;
	shaderBuilder << "	ivec2 coords = ivec2(wordAddress % c_memorySize, wordAddress / c_memorySize);" << std::endl;
	shaderBuilder << "	uint pixel = imageLoad(g_memory, coords).r;" << std::endl;
	shaderBuilder << "	return (pixel >> shiftAmount) & 0xFF;" << std::endl;
	shaderBuilder << "}" << std::endl;

	shaderBuilder << "uint Memory_Read4(uint address, uint nibIndex)" << std::endl;
	shaderBuilder << "{" << std::endl;
	shaderBuilder << "	uint wordAddress = address / 4;" << std::endl;
	shaderBuilder << "	uint shiftAmount = ((address & 3) * 2 + nibIndex) * 4;" << std::endl;
	shaderBuilder << "	ivec2 coords = ivec2(wordAddress % c_memorySize, wordAddress / c_memorySize);" << std::endl;
	shaderBuilder << "	uint pixel = imageLoad(g_memory, coords).r;" << std::endl;
	shaderBuilder << "	return (pixel >> shiftAmount) & 0xF;" << std::endl;
	shaderBuilder << "}" << std::endl;

	shaderBuilder << "uint Vec4ToPSM32(vec4 color)" << std::endl;
	shaderBuilder << "{" << std::endl;
	shaderBuilder << "	uint pixel = 0;" << std::endl;
	shaderBuilder << "	pixel |= uint(color.r * 255.0) << 0;" << std::endl;
	shaderBuilder << "	pixel |= uint(color.g * 255.0) << 8;" << std::endl;
	shaderBuilder << "	pixel |= uint(color.b * 255.0) << 16;" << std::endl;
	shaderBuilder << "	pixel |= uint(color.a * 255.0) << 24;" << std::endl;
	shaderBuilder << "	return pixel;" << std::endl;
	shaderBuilder << "}" << std::endl;

	shaderBuilder << "uint Vec4ToPSM16(vec4 color)" << std::endl;
	shaderBuilder << "{" << std::endl;
	shaderBuilder << "	uint pixel = 0;" << std::endl;
	shaderBuilder << "	pixel |= uint(color.r * 31.0) << 0;" << std::endl;
	shaderBuilder << "	pixel |= uint(color.g * 31.0) << 5;" << std::endl;
	shaderBuilder << "	pixel |= uint(color.b * 31.0) << 10;" << std::endl;
	shaderBuilder << "	pixel |= uint(color.a) << 15;" << std::endl;
	shaderBuilder << "	return pixel;" << std::endl;
	shaderBuilder << "}" << std::endl;

	shaderBuilder << "vec4 PSM32ToVec4(uint pixel)" << std::endl;
	shaderBuilder << "{" << std::endl;
	shaderBuilder << "	vec4 result;" << std::endl;
	shaderBuilder << "	result.r = float((pixel & 0x000000FF) >> 0) / 255.0;" << std::endl;
	shaderBuilder << "	result.g = float((pixel & 0x0000FF00) >> 8) / 255.0;" << std::endl;
	shaderBuilder << "	result.b = float((pixel & 0x00FF0000) >> 16) / 255.0;" << std::endl;
	shaderBuilder << "	result.a = float((pixel & 0xFF000000) >> 24) / 255.0;" << std::endl;
	shaderBuilder << "	return result;" << std::endl;
	shaderBuilder << "}" << std::endl;

	shaderBuilder << "vec4 PSM16ToVec4(uint pixel)" << std::endl;
	shaderBuilder << "{" << std::endl;
	shaderBuilder << "	vec4 result;" << std::endl;
	shaderBuilder << "	result.r = float((pixel & 0x001F) >> 0) / 31.0;" << std::endl;
	shaderBuilder << "	result.g = float((pixel & 0x03E0) >> 5) / 31.0;" << std::endl;
	shaderBuilder << "	result.b = float((pixel & 0x7C00) >> 10) / 31.0;" << std::endl;
	shaderBuilder << "	result.a = float((pixel & 0x8000) >> 15) / 1.0;" << std::endl;
	shaderBuilder << "	return result;" << std::endl;
	shaderBuilder << "}" << std::endl;

	shaderBuilder << "const uint c_pageSize = 8192;" << std::endl;

	shaderBuilder << "uint GetPixelAddress_PSMCT32(uint bufAddress, uint bufWidth, readonly layout(r32ui) uimage2D swizzleTable, uvec2 pixelPos)" << std::endl;
	shaderBuilder << "{" << std::endl;
	shaderBuilder << "	const uint c_pageWidth = 64;" << std::endl;
	shaderBuilder << "	const uint c_pageHeight = 32;" << std::endl;
	shaderBuilder << "	uint pageNum = (pixelPos.x / c_pageWidth) + (pixelPos.y / c_pageHeight) * bufWidth / c_pageWidth;" << std::endl;
	shaderBuilder << "	pixelPos.x %= c_pageWidth;" << std::endl;
	shaderBuilder << "	pixelPos.y %= c_pageHeight;" << std::endl;
	shaderBuilder << "	uint pageOffset = imageLoad(swizzleTable, ivec2(pixelPos)).r;" << std::endl;
	shaderBuilder << "	return bufAddress + (pageNum * c_pageSize) + pageOffset;" << std::endl;
	shaderBuilder << "}" << std::endl;

	shaderBuilder << "uint GetPixelAddress_PSMCT16(uint bufAddress, uint bufWidth, readonly layout(r32ui) uimage2D swizzleTable, uvec2 pixelPos)" << std::endl;
	shaderBuilder << "{" << std::endl;
	shaderBuilder << "	const uint c_pageWidth = 64;" << std::endl;
	shaderBuilder << "	const uint c_pageHeight = 64;" << std::endl;
	shaderBuilder << "	uint pageNum = (pixelPos.x / c_pageWidth) + (pixelPos.y / c_pageHeight) * bufWidth / c_pageWidth;" << std::endl;
	shaderBuilder << "	pixelPos.x %= c_pageWidth;" << std::endl;
	shaderBuilder << "	pixelPos.y %= c_pageHeight;" << std::endl;
	shaderBuilder << "	uint pageOffset = imageLoad(swizzleTable, ivec2(pixelPos)).r;" << std::endl;
	shaderBuilder << "	return bufAddress + (pageNum * c_pageSize) + pageOffset;" << std::endl;
	shaderBuilder << "}" << std::endl;

	shaderBuilder << "uint GetPixelAddress_PSMT8(uint bufAddress, uint bufWidth, readonly layout(r32ui) uimage2D swizzleTable, uvec2 pixelPos)" << std::endl;
	shaderBuilder << "{" << std::endl;
	shaderBuilder << "	const uint c_pageWidth = 128;" << std::endl;
	shaderBuilder << "	const uint c_pageHeight = 64;" << std::endl;
	shaderBuilder << "	uint pageNum = (pixelPos.x / c_pageWidth) + (pixelPos.y / c_pageHeight) * bufWidth / c_pageWidth;" << std::endl;
	shaderBuilder << "	pixelPos.x %= c_pageWidth;" << std::endl;
	shaderBuilder << "	pixelPos.y %= c_pageHeight;" << std::endl;
	shaderBuilder << "	uint pageOffset = imageLoad(swizzleTable, ivec2(pixelPos)).r;" << std::endl;
	shaderBuilder << "	return bufAddress + (pageNum * c_pageSize) + pageOffset;" << std::endl;
	shaderBuilder << "}" << std::endl;

	shaderBuilder << "uint GetPixelAddress_PSMT4(uint bufAddress, uint bufWidth, readonly layout(r32ui) uimage2D swizzleTable, uvec2 pixelPos)" << std::endl;
	shaderBuilder << "{" << std::endl;
	shaderBuilder << "	const uint c_pageWidth = 128;" << std::endl;
	shaderBuilder << "	const uint c_pageHeight = 128;" << std::endl;
	shaderBuilder << "	uint pageNum = (pixelPos.x / c_pageWidth) + (pixelPos.y / c_pageHeight) * bufWidth / c_pageWidth;" << std::endl;
	shaderBuilder << "	pixelPos.x %= c_pageWidth;" << std::endl;
	shaderBuilder << "	pixelPos.y %= c_pageHeight;" << std::endl;
	shaderBuilder << "	uint pageOffset = imageLoad(swizzleTable, ivec2(pixelPos)).r;" << std::endl;
	shaderBuilder << "	return bufAddress + (pageNum * c_pageSize) + pageOffset;" << std::endl;
	shaderBuilder << "}" << std::endl;

	auto shaderSource = shaderBuilder.str();
	return shaderSource;
}

Framework::OpenGl::ProgramPtr CGSH_OpenGL::GeneratePresentProgram()
{
	Framework::OpenGl::CShader vertexShader(GL_VERTEX_SHADER);
	Framework::OpenGl::CShader pixelShader(GL_FRAGMENT_SHADER);

	{
		std::stringstream shaderBuilder;
		shaderBuilder << GLSL_VERSION << std::endl;
		shaderBuilder << "in vec2 a_position;" << std::endl;
		shaderBuilder << "in vec2 a_texCoord;" << std::endl;
		shaderBuilder << "out vec2 v_texCoord;" << std::endl;
		shaderBuilder << "uniform vec2 g_texCoordScale;" << std::endl;
		shaderBuilder << "void main()" << std::endl;
		shaderBuilder << "{" << std::endl;
		shaderBuilder << "	v_texCoord = a_texCoord * g_texCoordScale;" << std::endl;
		shaderBuilder << "	gl_Position = vec4(a_position, 0, 1);" << std::endl;
		shaderBuilder << "}" << std::endl;

		vertexShader.SetSource(shaderBuilder.str().c_str());
		FRAMEWORK_MAYBE_UNUSED bool result = vertexShader.Compile();
		assert(result);
	}

	{
		std::stringstream shaderBuilder;
		shaderBuilder << GLSL_VERSION << std::endl;
		shaderBuilder << "precision mediump float;" << std::endl;
		shaderBuilder << "in vec2 v_texCoord;" << std::endl;
		shaderBuilder << "out vec4 fragColor;" << std::endl;
		shaderBuilder << GenerateMemoryAccessSection() << std::endl;
//		shaderBuilder << "uniform sampler2D g_texture;" << std::endl;
		shaderBuilder << "uniform uint g_frameBufPtr;" << std::endl;
		shaderBuilder << "uniform uint g_frameBufWidth;" << std::endl;
		shaderBuilder << "layout(binding = " << SHADER_IMAGE_FRAME_SWIZZLE << ", r32ui) readonly uniform uimage2D g_frameSwizzleTable;" << std::endl;
		shaderBuilder << "void main()" << std::endl;
		shaderBuilder << "{" << std::endl;
		shaderBuilder << "	ivec2 pixelPosition = ivec2(v_texCoord.x * 512.0, v_texCoord.y * 448.0);" << std::endl;
//		shaderBuilder << "	ivec2 pixelPosition = ivec2(v_texCoord.x * 640.0, v_texCoord.y * 256.0);" << std::endl;
//		shaderBuilder << "	ivec2 pixelPosition = ivec2(v_texCoord.x * 640.0, v_texCoord.y * 448.0);" << std::endl;
#if 0
		shaderBuilder << "	uint frameAddress = GetPixelAddress_PSMCT32(g_frameBufPtr, g_frameBufWidth, g_frameSwizzleTable, pixelPosition);" << std::endl;
		shaderBuilder << "	uint pixel = Memory_Read32(frameAddress);" << std::endl;
		shaderBuilder << "	fragColor = PSM32ToVec4(pixel);" << std::endl;
#endif
#if 1
		shaderBuilder << "	uint frameAddress = GetPixelAddress_PSMCT16(g_frameBufPtr, g_frameBufWidth, g_frameSwizzleTable, pixelPosition);" << std::endl;
		shaderBuilder << "	uint pixel = Memory_Read16(frameAddress);" << std::endl;
		shaderBuilder << "	fragColor = PSM16ToVec4(pixel);" << std::endl;
#endif
		shaderBuilder << "}" << std::endl;

		pixelShader.SetSource(shaderBuilder.str().c_str());
		FRAMEWORK_MAYBE_UNUSED bool result = pixelShader.Compile();
		assert(result);
	}

	auto program = std::make_shared<Framework::OpenGl::CProgram>();

	{
		program->AttachShader(vertexShader);
		program->AttachShader(pixelShader);

		glBindAttribLocation(*program, static_cast<GLuint>(PRIM_VERTEX_ATTRIB::POSITION), "a_position");
		glBindAttribLocation(*program, static_cast<GLuint>(PRIM_VERTEX_ATTRIB::TEXCOORD), "a_texCoord");

		FRAMEWORK_MAYBE_UNUSED bool result = program->Link();
		assert(result);
	}

	return program;
}

Framework::OpenGl::ProgramPtr CGSH_OpenGL::GenerateCopyToFbProgram()
{
	Framework::OpenGl::CShader vertexShader(GL_VERTEX_SHADER);
	Framework::OpenGl::CShader pixelShader(GL_FRAGMENT_SHADER);

	{
		std::stringstream shaderBuilder;
		shaderBuilder << GLSL_VERSION << std::endl;
		shaderBuilder << "in vec2 a_position;" << std::endl;
		shaderBuilder << "in vec2 a_texCoord;" << std::endl;
		shaderBuilder << "out vec2 v_texCoord;" << std::endl;
		shaderBuilder << "uniform vec2 g_srcPosition;" << std::endl;
		shaderBuilder << "uniform vec2 g_srcSize;" << std::endl;
		shaderBuilder << "void main()" << std::endl;
		shaderBuilder << "{" << std::endl;
		shaderBuilder << "	v_texCoord = (a_texCoord * g_srcSize) + g_srcPosition;" << std::endl;
		shaderBuilder << "	gl_Position = vec4(a_position, 0, 1);" << std::endl;
		shaderBuilder << "}" << std::endl;

		vertexShader.SetSource(shaderBuilder.str().c_str());
		FRAMEWORK_MAYBE_UNUSED bool result = vertexShader.Compile();
		assert(result);
	}

	{
		std::stringstream shaderBuilder;
		shaderBuilder << GLSL_VERSION << std::endl;
		shaderBuilder << "precision mediump float;" << std::endl;
		shaderBuilder << "in vec2 v_texCoord;" << std::endl;
		shaderBuilder << "out vec4 fragColor;" << std::endl;
		shaderBuilder << "uniform sampler2D g_texture;" << std::endl;
		shaderBuilder << "void main()" << std::endl;
		shaderBuilder << "{" << std::endl;
		shaderBuilder << "	fragColor = texture(g_texture, v_texCoord);" << std::endl;
		shaderBuilder << "}" << std::endl;

		pixelShader.SetSource(shaderBuilder.str().c_str());
		FRAMEWORK_MAYBE_UNUSED bool result = pixelShader.Compile();
		assert(result);
	}

	auto program = std::make_shared<Framework::OpenGl::CProgram>();

	{
		program->AttachShader(vertexShader);
		program->AttachShader(pixelShader);

		glBindAttribLocation(*program, static_cast<GLuint>(PRIM_VERTEX_ATTRIB::POSITION), "a_position");
		glBindAttribLocation(*program, static_cast<GLuint>(PRIM_VERTEX_ATTRIB::TEXCOORD), "a_texCoord");

		FRAMEWORK_MAYBE_UNUSED bool result = program->Link();
		assert(result);
	}

	return program;
}

std::string CGSH_OpenGL::GenerateXferProgramBase()
{
	std::stringstream shaderBuilder;
	shaderBuilder << "#version 430" << std::endl;

	shaderBuilder << "layout(local_size_x = " << g_xferWorkGroupSize << ") in;" << std::endl;

	shaderBuilder << "layout(binding = " << SHADER_IMAGE_XFER_SWIZZLE << ", r32ui) readonly uniform uimage2D g_xferSwizzleTable;" << std::endl;

	shaderBuilder << "layout(binding = 0, std140) uniform xferParams" << std::endl;
	shaderBuilder << "{" << std::endl;
	shaderBuilder << "	uint g_bufAddress;" << std::endl;
	shaderBuilder << "	uint g_bufWidth;" << std::endl;
	shaderBuilder << "	uint g_rrw;" << std::endl;
	shaderBuilder << "	uint g_dsax;" << std::endl;
	shaderBuilder << "	uint g_dsay;" << std::endl;
	shaderBuilder << "};" << std::endl;

	shaderBuilder << "layout(binding = 0, std430) buffer xferData" << std::endl;
	shaderBuilder << "{" << std::endl;
	shaderBuilder << "	uint g_data[];" << std::endl;
	shaderBuilder << "};" << std::endl;

	shaderBuilder << "uvec2 Xfer_GetPixelPosition(uint pixelIndex)" << std::endl;
	shaderBuilder << "{" << std::endl;
	shaderBuilder << "	uint rrx = pixelIndex % g_rrw;" << std::endl;
	shaderBuilder << "	uint rry = pixelIndex / g_rrw;" << std::endl;
	shaderBuilder << "	return uvec2((rrx + g_dsax) % 2048, (rry + g_dsay) % 2048);" << std::endl;
	shaderBuilder << "}" << std::endl;

	shaderBuilder << "uint XferStream_Read16(uint pixelIndex)" << std::endl;
	shaderBuilder << "{" << std::endl;
	shaderBuilder << "	uint srcOffset = pixelIndex / 2;" << std::endl;
	shaderBuilder << "	uint srcShift = (pixelIndex % 2) * 16;" << std::endl;
	shaderBuilder << "	return (g_data[srcOffset] >> srcShift) & 0xFFFF;" << std::endl;
	shaderBuilder << "}" << std::endl;

	shaderBuilder << "uint XferStream_Read8(uint pixelIndex)" << std::endl;
	shaderBuilder << "{" << std::endl;
	shaderBuilder << "	uint srcOffset = pixelIndex / 4;" << std::endl;
	shaderBuilder << "	uint srcShift = (pixelIndex % 4) * 8;" << std::endl;
	shaderBuilder << "	return (g_data[srcOffset] >> srcShift) & 0xFF;" << std::endl;
	shaderBuilder << "}" << std::endl;

	shaderBuilder << "uint XferStream_Read4(uint pixelIndex)" << std::endl;
	shaderBuilder << "{" << std::endl;
	shaderBuilder << "	uint srcOffset = pixelIndex / 16;" << std::endl;
	shaderBuilder << "	uint srcShift = (pixelIndex % 16) * 4;" << std::endl;
	shaderBuilder << "	return (g_data[srcOffset] >> srcShift) & 0xF;" << std::endl;
	shaderBuilder << "}" << std::endl;

	shaderBuilder << GenerateMemoryAccessSection() << std::endl;

	return shaderBuilder.str();
}

Framework::OpenGl::ProgramPtr CGSH_OpenGL::GenerateXferProgramPSMCT32()
{
	Framework::OpenGl::CShader computeShader(GL_COMPUTE_SHADER);

	auto program = std::make_shared<Framework::OpenGl::CProgram>();

	{
		std::stringstream shaderBuilder;

		shaderBuilder << GenerateXferProgramBase();

		shaderBuilder << "void main()" << std::endl;
		shaderBuilder << "{" << std::endl;
		shaderBuilder << "	uint pixelIndex = gl_GlobalInvocationID.x;" << std::endl;
		shaderBuilder << "	uint pixel = g_data[pixelIndex];" << std::endl;
		shaderBuilder << "	uvec2 pixelPos = Xfer_GetPixelPosition(pixelIndex);" << std::endl;
		shaderBuilder << "	uint address = GetPixelAddress_PSMCT32(g_bufAddress, g_bufWidth, g_xferSwizzleTable, pixelPos);" << std::endl;
		shaderBuilder << "	Memory_Write32(address, pixel);" << std::endl;
		shaderBuilder << "}" << std::endl;
		
		auto source = shaderBuilder.str();
		computeShader.SetSource(source.c_str());
		FRAMEWORK_MAYBE_UNUSED bool result = computeShader.Compile();
		assert(result);
	}

	{
		program->AttachShader(computeShader);

		FRAMEWORK_MAYBE_UNUSED bool result = program->Link();
		assert(result);
	}

	return program;
}

Framework::OpenGl::ProgramPtr CGSH_OpenGL::GenerateXferProgramPSMCT16()
{
	Framework::OpenGl::CShader computeShader(GL_COMPUTE_SHADER);

	auto program = std::make_shared<Framework::OpenGl::CProgram>();

	{
		std::stringstream shaderBuilder;

		shaderBuilder << GenerateXferProgramBase();

		shaderBuilder << "void main()" << std::endl;
		shaderBuilder << "{" << std::endl;
		shaderBuilder << "	uint pixelIndex = gl_GlobalInvocationID.x;" << std::endl;
		shaderBuilder << "	uint pixel = XferStream_Read16(pixelIndex);" << std::endl;
		shaderBuilder << "	uvec2 pixelPos = Xfer_GetPixelPosition(pixelIndex);" << std::endl;
		shaderBuilder << "	uint address = GetPixelAddress_PSMCT16(g_bufAddress, g_bufWidth, g_xferSwizzleTable, pixelPos);" << std::endl;
		shaderBuilder << "	Memory_Write16(address, pixel);" << std::endl;
		shaderBuilder << "}" << std::endl;
		
		auto source = shaderBuilder.str();
		computeShader.SetSource(source.c_str());
		FRAMEWORK_MAYBE_UNUSED bool result = computeShader.Compile();
		assert(result);
	}

	{
		program->AttachShader(computeShader);

		FRAMEWORK_MAYBE_UNUSED bool result = program->Link();
		assert(result);
	}

	return program;
}

Framework::OpenGl::ProgramPtr CGSH_OpenGL::GenerateXferProgramPSMT8()
{
	Framework::OpenGl::CShader computeShader(GL_COMPUTE_SHADER);

	auto program = std::make_shared<Framework::OpenGl::CProgram>();

	{
		std::stringstream shaderBuilder;

		shaderBuilder << GenerateXferProgramBase();

		shaderBuilder << "void main()" << std::endl;
		shaderBuilder << "{" << std::endl;
		shaderBuilder << "	uint pixelIndex = gl_GlobalInvocationID.x;" << std::endl;
		shaderBuilder << "	uint pixel = XferStream_Read8(pixelIndex);" << std::endl;
		shaderBuilder << "	uvec2 pixelPos = Xfer_GetPixelPosition(pixelIndex);" << std::endl;
		shaderBuilder << "	uint address = GetPixelAddress_PSMT8(g_bufAddress, g_bufWidth, g_xferSwizzleTable, pixelPos);" << std::endl;
		shaderBuilder << "	Memory_Write8(address, pixel);" << std::endl;
		shaderBuilder << "}" << std::endl;
		
		auto source = shaderBuilder.str();
		computeShader.SetSource(source.c_str());
		FRAMEWORK_MAYBE_UNUSED bool result = computeShader.Compile();
		assert(result);
	}

	{
		program->AttachShader(computeShader);

		FRAMEWORK_MAYBE_UNUSED bool result = program->Link();
		assert(result);
	}

	return program;
}

Framework::OpenGl::ProgramPtr CGSH_OpenGL::GenerateXferProgramPSMT4()
{
	Framework::OpenGl::CShader computeShader(GL_COMPUTE_SHADER);

	auto program = std::make_shared<Framework::OpenGl::CProgram>();

	{
		std::stringstream shaderBuilder;

		shaderBuilder << GenerateXferProgramBase();

		shaderBuilder << "void main()" << std::endl;
		shaderBuilder << "{" << std::endl;
		shaderBuilder << "	uint pixelIndex = gl_GlobalInvocationID.x;" << std::endl;
		shaderBuilder << "	uint pixel = XferStream_Read4(pixelIndex);" << std::endl;
		shaderBuilder << "	uvec2 pixelPos = Xfer_GetPixelPosition(pixelIndex);" << std::endl;
		shaderBuilder << "	uint address = GetPixelAddress_PSMT4(g_bufAddress, g_bufWidth, g_xferSwizzleTable, pixelPos);" << std::endl;
		shaderBuilder << "	Memory_Write4(address, pixelIndex & 1, pixel);" << std::endl;
		shaderBuilder << "}" << std::endl;
		
		auto source = shaderBuilder.str();
		computeShader.SetSource(source.c_str());
		FRAMEWORK_MAYBE_UNUSED bool result = computeShader.Compile();
		assert(result);
	}

	{
		program->AttachShader(computeShader);

		FRAMEWORK_MAYBE_UNUSED bool result = program->Link();
		assert(result);
	}

	return program;
}

Framework::OpenGl::ProgramPtr CGSH_OpenGL::GenerateXferProgramPSMT8H()
{
	Framework::OpenGl::CShader computeShader(GL_COMPUTE_SHADER);

	auto program = std::make_shared<Framework::OpenGl::CProgram>();

	{
		std::stringstream shaderBuilder;

		shaderBuilder << GenerateXferProgramBase();

		shaderBuilder << "void main()" << std::endl;
		shaderBuilder << "{" << std::endl;
		shaderBuilder << "	uint pixelIndex = gl_GlobalInvocationID.x;" << std::endl;
		shaderBuilder << "	uint pixel = XferStream_Read8(pixelIndex);" << std::endl;
		shaderBuilder << "	uvec2 pixelPos = Xfer_GetPixelPosition(pixelIndex);" << std::endl;
		shaderBuilder << "	const uint c_texelSize = 4;" << std::endl;
		shaderBuilder << "	uint address = g_bufAddress + (pixelPos.y * g_bufWidth * c_texelSize) + (pixelPos.x * c_texelSize);" << std::endl;
		shaderBuilder << "	Memory_Write8(address + 3, pixel);" << std::endl;
		shaderBuilder << "}" << std::endl;
		
		auto source = shaderBuilder.str();
		computeShader.SetSource(source.c_str());
		FRAMEWORK_MAYBE_UNUSED bool result = computeShader.Compile();
		assert(result);
	}

	{
		program->AttachShader(computeShader);

		FRAMEWORK_MAYBE_UNUSED bool result = program->Link();
		assert(result);
	}

	return program;
}

Framework::OpenGl::ProgramPtr CGSH_OpenGL::GenerateXferProgramPSMT4HL()
{
	Framework::OpenGl::CShader computeShader(GL_COMPUTE_SHADER);

	auto program = std::make_shared<Framework::OpenGl::CProgram>();

	{
		std::stringstream shaderBuilder;

		shaderBuilder << GenerateXferProgramBase();

		shaderBuilder << "void main()" << std::endl;
		shaderBuilder << "{" << std::endl;
		shaderBuilder << "	uint pixelIndex = gl_GlobalInvocationID.x;" << std::endl;
		shaderBuilder << "	uint pixel = XferStream_Read4(pixelIndex);" << std::endl;
		shaderBuilder << "	uvec2 pixelPos = Xfer_GetPixelPosition(pixelIndex);" << std::endl;
		shaderBuilder << "	const uint c_texelSize = 4;" << std::endl;
		shaderBuilder << "	uint address = g_bufAddress + (pixelPos.y * g_bufWidth * c_texelSize) + (pixelPos.x * c_texelSize);" << std::endl;
		shaderBuilder << "	Memory_Write4(address + 3, 0, pixel);" << std::endl;
		shaderBuilder << "}" << std::endl;
		
		auto source = shaderBuilder.str();
		computeShader.SetSource(source.c_str());
		FRAMEWORK_MAYBE_UNUSED bool result = computeShader.Compile();
		assert(result);
	}

	{
		program->AttachShader(computeShader);

		FRAMEWORK_MAYBE_UNUSED bool result = program->Link();
		assert(result);
	}

	return program;
}

Framework::OpenGl::ProgramPtr CGSH_OpenGL::GenerateXferProgramPSMT4HH()
{
	Framework::OpenGl::CShader computeShader(GL_COMPUTE_SHADER);

	auto program = std::make_shared<Framework::OpenGl::CProgram>();

	{
		std::stringstream shaderBuilder;

		shaderBuilder << GenerateXferProgramBase();

		shaderBuilder << "void main()" << std::endl;
		shaderBuilder << "{" << std::endl;
		shaderBuilder << "	uint pixelIndex = gl_GlobalInvocationID.x;" << std::endl;
		shaderBuilder << "	uint pixel = XferStream_Read4(pixelIndex);" << std::endl;
		shaderBuilder << "	uvec2 pixelPos = Xfer_GetPixelPosition(pixelIndex);" << std::endl;
		shaderBuilder << "	const uint c_texelSize = 4;" << std::endl;
		shaderBuilder << "	uint address = g_bufAddress + (pixelPos.y * g_bufWidth * c_texelSize) + (pixelPos.x * c_texelSize);" << std::endl;
		shaderBuilder << "	Memory_Write4(address + 3, 1, pixel);" << std::endl;
		shaderBuilder << "}" << std::endl;
		
		auto source = shaderBuilder.str();
		computeShader.SetSource(source.c_str());
		FRAMEWORK_MAYBE_UNUSED bool result = computeShader.Compile();
		assert(result);
	}

	{
		program->AttachShader(computeShader);

		FRAMEWORK_MAYBE_UNUSED bool result = program->Link();
		assert(result);
	}

	return program;
}

Framework::OpenGl::ProgramPtr CGSH_OpenGL::GenerateClutLoaderProgram()
{
	auto program = std::make_shared<Framework::OpenGl::CProgram>();

	Framework::OpenGl::CShader computeShader(GL_COMPUTE_SHADER);

	{
		std::stringstream shaderBuilder;

		shaderBuilder << "#version 430" << std::endl;

		shaderBuilder << "layout(local_size_x = 16, local_size_y = 16) in;" << std::endl;

		shaderBuilder << GenerateMemoryAccessSection();

		shaderBuilder << "layout(binding = " << SHADER_IMAGE_CLUT << ", r32ui) uniform uimage2D g_clut;" << std::endl;
		shaderBuilder << "layout(binding = " << SHADER_IMAGE_CLUT_SWIZZLE << ", r32ui) readonly uniform uimage2D g_clutSwizzleTable;" << std::endl;

		shaderBuilder << "layout(binding = 0, std140) uniform clutLoadParams" << std::endl;
		shaderBuilder << "{" << std::endl;
		shaderBuilder << "	uint g_clutBufPtr;" << std::endl;
		shaderBuilder << "};" << std::endl;

		shaderBuilder << "void main()" << std::endl;
		shaderBuilder << "{" << std::endl;
		shaderBuilder << "	uvec2 colorPos = gl_GlobalInvocationID.xy;" << std::endl;
		shaderBuilder << "	uint colorAddress = GetPixelAddress_PSMCT32(g_clutBufPtr, 64, g_clutSwizzleTable, colorPos);" << std::endl;
		shaderBuilder << "	uint color = Memory_Read32(colorAddress);" << std::endl;
		shaderBuilder << "	uint clutIndex = colorPos.x + (colorPos.y * 16);" << std::endl;
		shaderBuilder << "	clutIndex = (clutIndex & ~0x18) | ((clutIndex & 0x08) << 1) | ((clutIndex & 0x10) >> 1);" << std::endl;
		shaderBuilder << "	uint colorLo = (color >>  0) & 0xFFFF;" << std::endl;
		shaderBuilder << "	uint colorHi = (color >> 16) & 0xFFFF;" << std::endl;
		shaderBuilder << "	imageStore(g_clut, ivec2(clutIndex + 0x000, 0), uvec4(colorLo));" << std::endl;
		shaderBuilder << "	imageStore(g_clut, ivec2(clutIndex + 0x100, 0), uvec4(colorHi));" << std::endl;
		shaderBuilder << "}" << std::endl;

		auto source = shaderBuilder.str();
		computeShader.SetSource(source.c_str());
		FRAMEWORK_MAYBE_UNUSED bool result = computeShader.Compile();
		assert(result);
	}

	{
		program->AttachShader(computeShader);

		FRAMEWORK_MAYBE_UNUSED bool result = program->Link();
		assert(result);
	}

	return program;
}
