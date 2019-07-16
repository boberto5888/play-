#include <stdio.h>
#include <assert.h>
#include <math.h>

#include "../../Log.h"
#include "../../AppConfig.h"
#include "../GsPixelFormats.h"
#include "GSH_OpenGL.h"

#define MEMORY_TEXTURE_SIZE 1024

#ifdef GLES_COMPATIBILITY
//Standard blending constants
#define BLEND_SRC_ALPHA GL_SRC_ALPHA
#define BLEND_ONE_MINUS_SRC_ALPHA GL_ONE_MINUS_SRC_ALPHA
#else
//Dual source blending constants
#define BLEND_SRC_ALPHA GL_SRC1_ALPHA
#define BLEND_ONE_MINUS_SRC_ALPHA GL_ONE_MINUS_SRC1_ALPHA
#endif

#define NUM_SAMPLES 8
#define FRAMEBUFFER_HEIGHT 1024

const uint32 CGSH_OpenGL::g_xferWorkGroupSize = 1024;

// clang-format off
const GLenum CGSH_OpenGL::g_nativeClampModes[CGSHandler::CLAMP_MODE_MAX] =
{
	GL_REPEAT,
	GL_CLAMP_TO_EDGE,
	GL_REPEAT,
	GL_REPEAT
};

const unsigned int CGSH_OpenGL::g_shaderClampModes[CGSHandler::CLAMP_MODE_MAX] =
{
	TEXTURE_CLAMP_MODE_STD,
	TEXTURE_CLAMP_MODE_STD,
	TEXTURE_CLAMP_MODE_REGION_CLAMP,
	TEXTURE_CLAMP_MODE_REGION_REPEAT
};
// clang-format on

static uint32 MakeColor(uint8 r, uint8 g, uint8 b, uint8 a)
{
	return (a << 24) | (b << 16) | (g << 8) | (r);
}

CGSH_OpenGL::CGSH_OpenGL()
    : m_pCvtBuffer(nullptr)
{
	RegisterPreferences();
	LoadPreferences();

	m_pCvtBuffer = new uint8[CVTBUFFERSIZE];

	memset(&m_renderState, 0, sizeof(m_renderState));
	m_vertexBuffer.reserve(VERTEX_BUFFER_SIZE);
}

CGSH_OpenGL::~CGSH_OpenGL()
{
	delete[] m_pCvtBuffer;
}

void CGSH_OpenGL::InitializeImpl()
{
	InitializeRC();

	m_nVtxCount = 0;

	for(unsigned int i = 0; i < MAX_PALETTE_CACHE; i++)
	{
		m_paletteCache.push_back(PalettePtr(new CPalette()));
	}

	m_renderState.isValid = false;
	m_validGlState = 0;
}

void CGSH_OpenGL::ReleaseImpl()
{
	ResetImpl();

	m_paletteCache.clear();
	m_shaders.clear();
	m_presentProgramPSMCT32.reset();
	m_presentProgramPSMCT16.reset();
	m_presentVertexBuffer.Reset();
	m_presentVertexArray.Reset();
	m_copyToFbProgram.reset();
	m_copyToFbTexture.Reset();
	m_copyToFbVertexBuffer.Reset();
	m_copyToFbVertexArray.Reset();
	m_primBuffer.Reset();
	m_primVertexArray.Reset();
	m_vertexParamsBuffer.Reset();
	m_fragmentParamsBuffer.Reset();
	m_xferProgramPSMCT32.reset();
	m_xferProgramPSMCT16.reset();
	m_xferProgramPSMT8.reset();
	m_xferProgramPSMT4.reset();
	m_xferProgramPSMT8H.reset();
	m_xferProgramPSMT4HL.reset();
	m_xferProgramPSMT4HH.reset();
	m_xferBuffer.Reset();
	m_xferParamsBuffer.Reset();
	m_clutLoaderProgramIDX8_CSM0_PSMCT32.reset();
	m_clutLoadParamsBuffer.Reset();
}

void CGSH_OpenGL::ResetImpl()
{
	LoadPreferences();
	m_textureCache.Flush();
	PalCache_Flush();
	m_framebuffers.clear();
	m_depthbuffers.clear();
	m_vertexBuffer.clear();
	m_renderState.isValid = false;
	m_validGlState = 0;
	m_drawingToDepth = false;
	m_primitiveType = PRIM_INVALID;
}

void CGSH_OpenGL::FlipImpl()
{
	FlushVertexBuffer();
	m_renderState.isValid = false;
	m_validGlState = 0;

	DISPLAY d;
	DISPFB fb;
	{
		std::lock_guard<std::recursive_mutex> registerMutexLock(m_registerMutex);
		unsigned int readCircuit = GetCurrentReadCircuit();
		switch(readCircuit)
		{
		case 0:
			d <<= m_nDISPLAY1.value.q;
			fb <<= m_nDISPFB1.value.q;
			break;
		case 1:
			d <<= m_nDISPLAY2.value.q;
			fb <<= m_nDISPFB2.value.q;
			break;
		}
	}

	unsigned int dispWidth = (d.nW + 1) / (d.nMagX + 1);
	unsigned int dispHeight = (d.nH + 1);

	bool halfHeight = GetCrtIsInterlaced() && GetCrtIsFrameMode();
	if(halfHeight) dispHeight /= 2;

	FramebufferPtr framebuffer;
	for(const auto& candidateFramebuffer : m_framebuffers)
	{
		if(
		    (candidateFramebuffer->m_basePtr == fb.GetBufPtr()) &&
		    (GetFramebufferBitDepth(candidateFramebuffer->m_psm) == GetFramebufferBitDepth(fb.nPSM)) &&
		    (candidateFramebuffer->m_width == fb.GetBufWidth()))
		{
			//We have a winner
			framebuffer = candidateFramebuffer;
			break;
		}
	}
	
	if(!framebuffer && (fb.GetBufWidth() != 0))
	{
		framebuffer = FramebufferPtr(new CFramebuffer(fb.GetBufPtr(), fb.GetBufWidth(), FRAMEBUFFER_HEIGHT, fb.nPSM, m_fbScale, m_multisampleEnabled));
		m_framebuffers.push_back(framebuffer);
		PopulateFramebuffer(framebuffer);
	}

	if(framebuffer)
	{
		CommitFramebufferDirtyPages(framebuffer, 0, dispHeight);
		if(m_multisampleEnabled)
		{
			ResolveFramebufferMultisample(framebuffer, m_fbScale);
		}
	}

	//Clear all of our output framebuffer
	{
		glBindFramebuffer(GL_FRAMEBUFFER, m_presentFramebuffer);

		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		glDisable(GL_SCISSOR_TEST);
		glClearColor(0, 0, 0, 0);
		glViewport(0, 0, m_presentationParams.windowWidth, m_presentationParams.windowHeight);
		glClear(GL_COLOR_BUFFER_BIT);
	}

	unsigned int sourceWidth = GetCrtWidth();
	unsigned int sourceHeight = GetCrtHeight();
	switch(m_presentationParams.mode)
	{
	case PRESENTATION_MODE_FILL:
		glViewport(0, 0, m_presentationParams.windowWidth, m_presentationParams.windowHeight);
		break;
	case PRESENTATION_MODE_FIT:
	{
		int viewportWidth[2];
		int viewportHeight[2];
		{
			viewportWidth[0] = m_presentationParams.windowWidth;
			viewportHeight[0] = (sourceWidth != 0) ? (m_presentationParams.windowWidth * sourceHeight) / sourceWidth : 0;
		}
		{
			viewportWidth[1] = (sourceHeight != 0) ? (m_presentationParams.windowHeight * sourceWidth) / sourceHeight : 0;
			viewportHeight[1] = m_presentationParams.windowHeight;
		}
		int selectedViewport = 0;
		if(
		    (viewportWidth[0] > static_cast<int>(m_presentationParams.windowWidth)) ||
		    (viewportHeight[0] > static_cast<int>(m_presentationParams.windowHeight)))
		{
			selectedViewport = 1;
			assert(
			    viewportWidth[1] <= static_cast<int>(m_presentationParams.windowWidth) &&
			    viewportHeight[1] <= static_cast<int>(m_presentationParams.windowHeight));
		}
		int offsetX = static_cast<int>(m_presentationParams.windowWidth - viewportWidth[selectedViewport]) / 2;
		int offsetY = static_cast<int>(m_presentationParams.windowHeight - viewportHeight[selectedViewport]) / 2;
		glViewport(offsetX, offsetY, viewportWidth[selectedViewport], viewportHeight[selectedViewport]);
	}
	break;
	case PRESENTATION_MODE_ORIGINAL:
	{
		int offsetX = static_cast<int>(m_presentationParams.windowWidth - sourceWidth) / 2;
		int offsetY = static_cast<int>(m_presentationParams.windowHeight - sourceHeight) / 2;
		glViewport(offsetX, offsetY, sourceWidth, sourceHeight);
	}
	break;
	}

	if(framebuffer)
	{
		float u1 = static_cast<float>(dispWidth) / static_cast<float>(framebuffer->m_width);
		float v1 = static_cast<float>(dispHeight) / static_cast<float>(framebuffer->m_height);

		glDisable(GL_BLEND);
		glDisable(GL_DEPTH_TEST);

		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, 0);

		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, framebuffer->m_texture);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

		Framework::OpenGl::ProgramPtr presentProgram;
		switch(fb.nPSM)
		{
		default:
			assert(false);
		case PSMCT32:
		case PSMCT24:
			presentProgram = m_presentProgramPSMCT32;
			break;
		case PSMCT16:
		case PSMCT16S:
			presentProgram = m_presentProgramPSMCT16;
			break;
		}
		glUseProgram(*presentProgram);

		//assert(m_presentTextureUniform != -1);
		//glUniform1i(m_presentTextureUniform, 0);

		glBindImageTexture(SHADER_IMAGE_MEMORY, m_memoryTexture, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32UI);
		glBindImageTexture(SHADER_IMAGE_FRAME_SWIZZLE, GetSwizzleTable(fb.nPSM), 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32UI);

		assert(m_presentTexCoordScaleUniform != -1);
		glUniform2f(m_presentTexCoordScaleUniform, 1, 1);

		glUniform1ui(m_presentFrameBufPtr, fb.GetBufPtr());
		glUniform1ui(m_presentFrameBufWidth, fb.GetBufWidth());
		glUniform2ui(m_presentScreenSize, dispWidth, dispHeight);

		glBindBuffer(GL_ARRAY_BUFFER, m_presentVertexBuffer);
		glBindVertexArray(m_presentVertexArray);

#ifdef _DEBUG
		presentProgram->Validate();
#endif

		glDrawArrays(GL_TRIANGLES, 0, 3);
	}

	CHECKGLERROR();

	static bool g_dumpFramebuffers = false;
	if(g_dumpFramebuffers)
	{
#if 0
		for(const auto& framebuffer : m_framebuffers)
		{
			glBindTexture(GL_TEXTURE_2D, framebuffer->m_texture);
			DumpTexture(framebuffer->m_width * m_fbScale, framebuffer->m_height * m_fbScale, framebuffer->m_basePtr);
		}
		for(const auto& depthbuffer : m_depthbuffers)
		{
			glBindTexture(GL_TEXTURE_2D, depthbuffer->m_depthBufferImage);
			std::vector<uint32> data;
			uint32 realWidth = depthbuffer->m_width * m_fbScale;
			uint32 realHeight = depthbuffer->m_height * m_fbScale;
			data.resize(realWidth * realHeight);
			glGetTexImage(GL_TEXTURE_2D, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, data.data());
			CHECKGLERROR();
		}
#endif
		std::vector<uint32> data;
		data.resize(MEMORY_TEXTURE_SIZE * MEMORY_TEXTURE_SIZE);

		{
			glBindTexture(GL_TEXTURE_2D, m_memoryTexture);
			glGetTexImage(GL_TEXTURE_2D, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, data.data());
			CHECKGLERROR();
			glBindTexture(GL_TEXTURE_2D, 0);
		}

		{
			Framework::CStdStream memoryStream("gs.bin", "wb");
			memoryStream.Write(data.data(), data.size() * sizeof(uint32));
		}
	}

	PresentBackbuffer();
	CGSHandler::FlipImpl();
}

void CGSH_OpenGL::LoadState(Framework::CZipArchiveReader& archive)
{
	CGSHandler::LoadState(archive);
	m_mailBox.SendCall(
	    [this]() {
		    m_textureCache.InvalidateRange(0, RAMSIZE);
	    });
}

void CGSH_OpenGL::RegisterPreferences()
{
	CGSHandler::RegisterPreferences();
	CAppConfig::GetInstance().RegisterPreferenceInteger(PREF_CGSH_OPENGL_RESOLUTION_FACTOR, 1);
	CAppConfig::GetInstance().RegisterPreferenceBoolean(PREF_CGSH_OPENGL_FORCEBILINEARTEXTURES, false);
}

void CGSH_OpenGL::NotifyPreferencesChangedImpl()
{
	LoadPreferences();
	m_textureCache.Flush();
	PalCache_Flush();
	m_framebuffers.clear();
	m_depthbuffers.clear();
	CGSHandler::NotifyPreferencesChangedImpl();
}

void CGSH_OpenGL::LoadPreferences()
{
	//m_fbScale = CAppConfig::GetInstance().GetPreferenceInteger(PREF_CGSH_OPENGL_RESOLUTION_FACTOR);
	m_fbScale = 1;
	m_forceBilinearTextures = CAppConfig::GetInstance().GetPreferenceBoolean(PREF_CGSH_OPENGL_FORCEBILINEARTEXTURES);
}

template <typename StorageFormat>
static Framework::OpenGl::CTexture CreateSwizzleTable()
{
	auto result = Framework::OpenGl::CTexture::Create();
	glBindTexture(GL_TEXTURE_2D, result);
	glTexStorage2D(GL_TEXTURE_2D, 1, GL_R32UI,
		StorageFormat::PAGEWIDTH, StorageFormat::PAGEHEIGHT);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
		StorageFormat::PAGEWIDTH, StorageFormat::PAGEHEIGHT,
		GL_RED_INTEGER, GL_UNSIGNED_INT, CGsPixelFormats::CPixelIndexor<StorageFormat>::GetPageOffsets());
	CHECKGLERROR();
	return result;
}

void CGSH_OpenGL::InitializeRC()
{
	//Initialize basic stuff
	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glClearDepthf(0.0f);

	SetupTextureUpdaters();

	m_presentProgramPSMCT32 = GeneratePresentProgram(PSMCT32);
	m_presentProgramPSMCT16 = GeneratePresentProgram(PSMCT16);

	m_presentVertexBuffer = GeneratePresentVertexBuffer();
	m_presentVertexArray = GeneratePresentVertexArray();
	//m_presentTextureUniform = glGetUniformLocation(*m_presentProgramPSMCT32, "g_texture");
	m_presentTexCoordScaleUniform = glGetUniformLocation(*m_presentProgramPSMCT32, "g_texCoordScale");
	m_presentFrameBufPtr = glGetUniformLocation(*m_presentProgramPSMCT32, "g_frameBufPtr");
	m_presentFrameBufWidth = glGetUniformLocation(*m_presentProgramPSMCT32, "g_frameBufWidth");
	m_presentScreenSize = glGetUniformLocation(*m_presentProgramPSMCT32, "g_screenSize");

	m_copyToFbProgram = GenerateCopyToFbProgram();
	m_copyToFbTexture = Framework::OpenGl::CTexture::Create();
	m_copyToFbVertexBuffer = GenerateCopyToFbVertexBuffer();
	m_copyToFbVertexArray = GenerateCopyToFbVertexArray();
	m_copyToFbSrcPositionUniform = glGetUniformLocation(*m_copyToFbProgram, "g_srcPosition");
	m_copyToFbSrcSizeUniform = glGetUniformLocation(*m_copyToFbProgram, "g_srcSize");

	m_primBuffer = Framework::OpenGl::CBuffer::Create();
	m_primVertexArray = GeneratePrimVertexArray();

	//Xfer
	m_xferProgramPSMCT32 = GenerateXferProgramPSMCT32();
	m_xferProgramPSMCT16 = GenerateXferProgramPSMCT16();
	m_xferProgramPSMT8 = GenerateXferProgramPSMT8();
	m_xferProgramPSMT4 = GenerateXferProgramPSMT4();
	m_xferProgramPSMT8H = GenerateXferProgramPSMT8H();
	m_xferProgramPSMT4HL = GenerateXferProgramPSMT4HL();
	m_xferProgramPSMT4HH = GenerateXferProgramPSMT4HH();
	m_xferParamsBuffer = GenerateUniformBlockBuffer(sizeof(XFERPARAMS));

	m_memoryTexture = Framework::OpenGl::CTexture::Create();
	glBindTexture(GL_TEXTURE_2D, m_memoryTexture);
	glTexStorage2D(GL_TEXTURE_2D, 1, GL_R32UI, MEMORY_TEXTURE_SIZE, MEMORY_TEXTURE_SIZE);
	CHECKGLERROR();

	//CLUT
	m_clutTexture = Framework::OpenGl::CTexture::Create();
	glBindTexture(GL_TEXTURE_2D, m_clutTexture);
	glTexStorage2D(GL_TEXTURE_2D, 1, GL_R32UI, CLUTENTRYCOUNT, 1);
	CHECKGLERROR();

	m_clutLoadParamsBuffer = GenerateUniformBlockBuffer(sizeof(CLUTLOADPARAMS));

	m_clutLoaderProgramIDX8_CSM0_PSMCT32 = GenerateClutLoaderProgram();

	m_swizzleTexturePSMCT32 = CreateSwizzleTable<CGsPixelFormats::STORAGEPSMCT32>();
	m_swizzleTexturePSMCT16 = CreateSwizzleTable<CGsPixelFormats::STORAGEPSMCT16>();
	m_swizzleTexturePSMCT16S = CreateSwizzleTable<CGsPixelFormats::STORAGEPSMCT16S>();
	m_swizzleTexturePSMT8 = CreateSwizzleTable<CGsPixelFormats::STORAGEPSMT8>();
	m_swizzleTexturePSMT4 = CreateSwizzleTable<CGsPixelFormats::STORAGEPSMT4>();

	m_xferBuffer = Framework::OpenGl::CBuffer::Create();

	m_vertexParamsBuffer = GenerateUniformBlockBuffer(sizeof(VERTEXPARAMS));
	m_fragmentParamsBuffer = GenerateUniformBlockBuffer(sizeof(FRAGMENTPARAMS));

	PresentBackbuffer();

	CHECKGLERROR();
}

Framework::OpenGl::CBuffer CGSH_OpenGL::GeneratePresentVertexBuffer()
{
	auto buffer = Framework::OpenGl::CBuffer::Create();

	// clang-format off
	static const float bufferContents[] =
	{
		//Pos         UV
		-1.0f, -1.0f, 0.0f,  1.0f,
		-1.0f,  3.0f, 0.0f, -1.0f,
		 3.0f, -1.0f, 2.0f,  1.0f,
	};
	// clang-format on

	glBindBuffer(GL_ARRAY_BUFFER, buffer);
	glBufferData(GL_ARRAY_BUFFER, sizeof(bufferContents), bufferContents, GL_STATIC_DRAW);

	glBindBuffer(GL_ARRAY_BUFFER, 0);

	CHECKGLERROR();

	return buffer;
}

Framework::OpenGl::CVertexArray CGSH_OpenGL::GeneratePresentVertexArray()
{
	auto vertexArray = Framework::OpenGl::CVertexArray::Create();

	glBindVertexArray(vertexArray);

	glBindBuffer(GL_ARRAY_BUFFER, m_presentVertexBuffer);

	glEnableVertexAttribArray(static_cast<GLuint>(PRIM_VERTEX_ATTRIB::POSITION));
	glVertexAttribPointer(static_cast<GLuint>(PRIM_VERTEX_ATTRIB::POSITION), 2, GL_FLOAT,
	                      GL_FALSE, sizeof(float) * 4, reinterpret_cast<const GLvoid*>(0));

	glEnableVertexAttribArray(static_cast<GLuint>(PRIM_VERTEX_ATTRIB::TEXCOORD));
	glVertexAttribPointer(static_cast<GLuint>(PRIM_VERTEX_ATTRIB::TEXCOORD), 2, GL_FLOAT,
	                      GL_FALSE, sizeof(float) * 4, reinterpret_cast<const GLvoid*>(8));

	glBindVertexArray(0);

	CHECKGLERROR();

	return vertexArray;
}

Framework::OpenGl::CBuffer CGSH_OpenGL::GenerateCopyToFbVertexBuffer()
{
	auto buffer = Framework::OpenGl::CBuffer::Create();

	// clang-format off
	static const float bufferContents[] =
	{
		//Pos       UV
		-1.0f, -1.0f, 0.0f, 0.0f,
		 1.0f, -1.0f, 1.0f, 0.0f,
		-1.0f,  1.0f, 0.0f, 1.0f,
		 1.0f,  1.0f, 1.0f, 1.0f,
	};
	// clang-format on

	glBindBuffer(GL_ARRAY_BUFFER, buffer);
	glBufferData(GL_ARRAY_BUFFER, sizeof(bufferContents), bufferContents, GL_STATIC_DRAW);

	glBindBuffer(GL_ARRAY_BUFFER, 0);

	CHECKGLERROR();

	return buffer;
}

Framework::OpenGl::CVertexArray CGSH_OpenGL::GenerateCopyToFbVertexArray()
{
	auto vertexArray = Framework::OpenGl::CVertexArray::Create();

	glBindVertexArray(vertexArray);

	glBindBuffer(GL_ARRAY_BUFFER, m_copyToFbVertexBuffer);

	glEnableVertexAttribArray(static_cast<GLuint>(PRIM_VERTEX_ATTRIB::POSITION));
	glVertexAttribPointer(static_cast<GLuint>(PRIM_VERTEX_ATTRIB::POSITION), 2, GL_FLOAT,
	                      GL_FALSE, sizeof(float) * 4, reinterpret_cast<const GLvoid*>(0));

	glEnableVertexAttribArray(static_cast<GLuint>(PRIM_VERTEX_ATTRIB::TEXCOORD));
	glVertexAttribPointer(static_cast<GLuint>(PRIM_VERTEX_ATTRIB::TEXCOORD), 2, GL_FLOAT,
	                      GL_FALSE, sizeof(float) * 4, reinterpret_cast<const GLvoid*>(8));

	glBindVertexArray(0);

	CHECKGLERROR();

	return vertexArray;
}

Framework::OpenGl::CVertexArray CGSH_OpenGL::GeneratePrimVertexArray()
{
	auto vertexArray = Framework::OpenGl::CVertexArray::Create();

	glBindVertexArray(vertexArray);

	glBindBuffer(GL_ARRAY_BUFFER, m_primBuffer);

	glEnableVertexAttribArray(static_cast<GLuint>(PRIM_VERTEX_ATTRIB::POSITION));
	glVertexAttribPointer(static_cast<GLuint>(PRIM_VERTEX_ATTRIB::POSITION), 2, GL_FLOAT,
	                      GL_FALSE, sizeof(PRIM_VERTEX), reinterpret_cast<const GLvoid*>(offsetof(PRIM_VERTEX, x)));

	glEnableVertexAttribArray(static_cast<GLuint>(PRIM_VERTEX_ATTRIB::DEPTH));
	glVertexAttribIPointer(static_cast<GLuint>(PRIM_VERTEX_ATTRIB::DEPTH), 1, GL_UNSIGNED_INT,
	                       sizeof(PRIM_VERTEX), reinterpret_cast<const GLvoid*>(offsetof(PRIM_VERTEX, z)));

	glEnableVertexAttribArray(static_cast<GLuint>(PRIM_VERTEX_ATTRIB::COLOR));
	glVertexAttribPointer(static_cast<GLuint>(PRIM_VERTEX_ATTRIB::COLOR), 4, GL_UNSIGNED_BYTE,
	                      GL_TRUE, sizeof(PRIM_VERTEX), reinterpret_cast<const GLvoid*>(offsetof(PRIM_VERTEX, color)));

	glEnableVertexAttribArray(static_cast<GLuint>(PRIM_VERTEX_ATTRIB::TEXCOORD));
	glVertexAttribPointer(static_cast<GLuint>(PRIM_VERTEX_ATTRIB::TEXCOORD), 3, GL_FLOAT,
	                      GL_FALSE, sizeof(PRIM_VERTEX), reinterpret_cast<const GLvoid*>(offsetof(PRIM_VERTEX, s)));

	glEnableVertexAttribArray(static_cast<GLuint>(PRIM_VERTEX_ATTRIB::FOG));
	glVertexAttribPointer(static_cast<GLuint>(PRIM_VERTEX_ATTRIB::FOG), 1, GL_FLOAT,
	                      GL_FALSE, sizeof(PRIM_VERTEX), reinterpret_cast<const GLvoid*>(offsetof(PRIM_VERTEX, f)));

	glBindVertexArray(0);

	CHECKGLERROR();

	return vertexArray;
}

Framework::OpenGl::CBuffer CGSH_OpenGL::GenerateUniformBlockBuffer(size_t blockSize)
{
	auto uniformBlockBuffer = Framework::OpenGl::CBuffer::Create();

	glBindBuffer(GL_UNIFORM_BUFFER, uniformBlockBuffer);
	glBufferData(GL_UNIFORM_BUFFER, blockSize, nullptr, GL_STREAM_DRAW);
	CHECKGLERROR();

	return uniformBlockBuffer;
}

void CGSH_OpenGL::MakeLinearZOrtho(float* matrix, float left, float right, float bottom, float top)
{
	matrix[0] = 2.0f / (right - left);
	matrix[1] = 0;
	matrix[2] = 0;
	matrix[3] = 0;

	matrix[4] = 0;
	matrix[5] = 2.0f / (top - bottom);
	matrix[6] = 0;
	matrix[7] = 0;

	matrix[8] = 0;
	matrix[9] = 0;
	matrix[10] = 1;
	matrix[11] = 0;

	matrix[12] = -(right + left) / (right - left);
	matrix[13] = -(top + bottom) / (top - bottom);
	matrix[14] = 0;
	matrix[15] = 1;
}

unsigned int CGSH_OpenGL::GetCurrentReadCircuit()
{
	uint32 rcMode = m_nPMODE & 0x03;
	switch(rcMode)
	{
	default:
	case 0:
		//No read circuit enabled?
		return 0;
	case 1:
		return 0;
	case 2:
		return 1;
	case 3:
	{
		//Both are enabled... See if we can find out which one is good
		//This happens in Capcom Classics Collection Vol. 2
		std::lock_guard<std::recursive_mutex> registerMutexLock(m_registerMutex);
		bool fb1Null = (m_nDISPFB1.value.q == 0);
		bool fb2Null = (m_nDISPFB2.value.q == 0);
		if(!fb1Null && fb2Null)
		{
			return 0;
		}
		if(fb1Null && !fb2Null)
		{
			return 1;
		}
		return 0;
	}
	break;
	}
}

/////////////////////////////////////////////////////////////
// Context Unpacking
/////////////////////////////////////////////////////////////

Framework::OpenGl::ProgramPtr CGSH_OpenGL::GetShaderFromCaps(const SHADERCAPS& shaderCaps)
{
	auto shaderIterator = m_shaders.find(static_cast<uint32>(shaderCaps));
	if(shaderIterator == m_shaders.end())
	{
		auto shader = GenerateShader(shaderCaps);

		glUseProgram(*shader);
		m_validGlState &= ~GLSTATE_PROGRAM;

		auto textureUniform = glGetUniformLocation(*shader, "g_texture");
		if(textureUniform != -1)
		{
			glUniform1i(textureUniform, 0);
		}

		auto paletteUniform = glGetUniformLocation(*shader, "g_palette");
		if(paletteUniform != -1)
		{
			glUniform1i(paletteUniform, 1);
		}

		auto framebufferUniform = glGetUniformLocation(*shader, "g_framebuffer");
		if(framebufferUniform != -1)
		{
			glUniform1i(framebufferUniform, 0);
		}

		auto depthbufferUniform = glGetUniformLocation(*shader, "g_depthbuffer");
		if(depthbufferUniform != -1)
		{
			glUniform1i(depthbufferUniform, 1);
		}

		auto vertexParamsUniformBlock = glGetUniformBlockIndex(*shader, "VertexParams");
		if(vertexParamsUniformBlock != GL_INVALID_INDEX)
		{
			glUniformBlockBinding(*shader, vertexParamsUniformBlock, 0);
		}

		auto fragmentParamsUniformBlock = glGetUniformBlockIndex(*shader, "FragmentParams");
		if(fragmentParamsUniformBlock != GL_INVALID_INDEX)
		{
			glUniformBlockBinding(*shader, fragmentParamsUniformBlock, 1);
		}

		CHECKGLERROR();

		m_shaders.insert(std::make_pair(static_cast<uint32>(shaderCaps), shader));
		shaderIterator = m_shaders.find(static_cast<uint32>(shaderCaps));
	}
	return shaderIterator->second;
}

void CGSH_OpenGL::SetRenderingContext(uint64 primReg)
{
	auto prim = make_convertible<PRMODE>(primReg);

	unsigned int context = prim.nContext;

	uint64 testReg = m_nReg[GS_REG_TEST_1 + context];
	uint64 frameReg = m_nReg[GS_REG_FRAME_1 + context];
	uint64 alphaReg = m_nReg[GS_REG_ALPHA_1 + context];
	uint64 zbufReg = m_nReg[GS_REG_ZBUF_1 + context];
	uint64 tex0Reg = m_nReg[GS_REG_TEX0_1 + context];
	uint64 tex1Reg = m_nReg[GS_REG_TEX1_1 + context];
	uint64 texAReg = m_nReg[GS_REG_TEXA];
	uint64 clampReg = m_nReg[GS_REG_CLAMP_1 + context];
	uint64 fogColReg = m_nReg[GS_REG_FOGCOL];
	uint64 scissorReg = m_nReg[GS_REG_SCISSOR_1 + context];

	//--------------------------------------------------------
	//Get shader caps
	//--------------------------------------------------------

	auto shaderCaps = make_convertible<SHADERCAPS>(0);
	FillShaderCapsFromTexture(shaderCaps, tex0Reg, tex1Reg, texAReg, clampReg);
	FillShaderCapsFromFrame(shaderCaps, frameReg);
	FillShaderCapsFromTestAndZbuf(shaderCaps, testReg, zbufReg);
	shaderCaps.hasAlphaBlend = prim.nAlpha;
	FillShaderCapsFromAlpha(shaderCaps, alphaReg);

	if(prim.nFog)
	{
		shaderCaps.hasFog = 1;
	}

	if(!prim.nTexture)
	{
		shaderCaps.texSourceMode = TEXTURE_SOURCE_MODE_NONE;
	}

	//--------------------------------------------------------
	//Check if a different shader is needed
	//--------------------------------------------------------

	if(!m_renderState.isValid ||
	   (static_cast<uint32>(m_renderState.shaderCaps) != static_cast<uint32>(shaderCaps)))
	{
		FlushVertexBuffer();
		m_renderState.shaderCaps = shaderCaps;
	}

	//--------------------------------------------------------
	//Set render states
	//--------------------------------------------------------

	if(!m_renderState.isValid ||
	   (m_renderState.primReg != primReg))
	{
		FlushVertexBuffer();

		//Humm, not quite sure about this
		//		if(prim.nAntiAliasing)
		//		{
		//			glEnable(GL_BLEND);
		//		}
		//		else
		//		{
		//			glDisable(GL_BLEND);
		//		}

		m_renderState.blendEnabled = prim.nAlpha ? GL_TRUE : GL_FALSE;
		m_validGlState &= ~GLSTATE_BLEND;
	}

	if(!m_renderState.isValid ||
	   (m_renderState.alphaReg != alphaReg))
	{
		FlushVertexBuffer();
		SetupBlendingFunction(alphaReg);
		CHECKGLERROR();
	}

	if(!m_renderState.isValid ||
	   (m_renderState.testReg != testReg))
	{
		FlushVertexBuffer();
		SetupTestFunctions(testReg);
		CHECKGLERROR();
	}

	if(!m_renderState.isValid ||
	   (m_renderState.zbufReg != zbufReg) ||
	   (m_renderState.testReg != testReg))
	{
		FlushVertexBuffer();
		SetupDepthBuffer(zbufReg, testReg);
		CHECKGLERROR();
	}

	if(!m_renderState.isValid ||
	   !m_renderState.isFramebufferStateValid ||
	   (m_renderState.frameReg != frameReg) ||
	   (m_renderState.zbufReg != zbufReg) ||
	   (m_renderState.scissorReg != scissorReg) ||
	   (m_renderState.testReg != testReg))
	{
		FlushVertexBuffer();
		SetupFramebuffer(frameReg, zbufReg, scissorReg, testReg);
		CHECKGLERROR();
	}

	if(!m_renderState.isValid ||
	   !m_renderState.isTextureStateValid ||
	   (m_renderState.tex0Reg != tex0Reg) ||
	   (m_renderState.tex1Reg != tex1Reg) ||
	   (m_renderState.texAReg != texAReg) ||
	   (m_renderState.clampReg != clampReg) ||
	   (m_renderState.primReg != primReg))
	{
		FlushVertexBuffer();
		SetupTexture(primReg, tex0Reg, tex1Reg, texAReg, clampReg);
		CHECKGLERROR();
	}

	if(!m_renderState.isValid ||
	   (m_renderState.fogColReg != fogColReg))
	{
		FlushVertexBuffer();
		SetupFogColor(fogColReg);
		CHECKGLERROR();
	}

	auto offset = make_convertible<XYOFFSET>(m_nReg[GS_REG_XYOFFSET_1 + context]);
	m_nPrimOfsX = offset.GetX();
	m_nPrimOfsY = offset.GetY();

	CHECKGLERROR();

	m_renderState.isValid = true;
	m_renderState.isTextureStateValid = true;
	m_renderState.isFramebufferStateValid = true;
	m_renderState.primReg = primReg;
	m_renderState.alphaReg = alphaReg;
	m_renderState.testReg = testReg;
	m_renderState.zbufReg = zbufReg;
	m_renderState.scissorReg = scissorReg;
	m_renderState.frameReg = frameReg;
	m_renderState.tex0Reg = tex0Reg;
	m_renderState.tex1Reg = tex1Reg;
	m_renderState.texAReg = texAReg;
	m_renderState.clampReg = clampReg;
	m_renderState.fogColReg = fogColReg;
}

void CGSH_OpenGL::SetupBlendingFunction(uint64 alphaReg)
{
	auto alpha = make_convertible<ALPHA>(alphaReg);

	m_fragmentParams.alphaFix = alpha.nFix;
	m_validGlState &= ~GLSTATE_FRAGMENT_PARAMS;

#if 0
	int nFunction = GL_FUNC_ADD;

	if((alpha.nA == alpha.nB) && (alpha.nD == ALPHABLEND_ABD_CS))
	{
		//ab*0 (when a == b) - Cs
		glBlendFuncSeparate(GL_ONE, GL_ZERO, GL_ONE, GL_ZERO);
	}
	else if((alpha.nA == alpha.nB) && (alpha.nD == ALPHABLEND_ABD_CD))
	{
		//ab*1 (when a == b) - Cd
		glBlendFuncSeparate(GL_ZERO, GL_ONE, GL_ONE, GL_ZERO);
	}
	else if((alpha.nA == alpha.nB) && (alpha.nD == ALPHABLEND_ABD_ZERO))
	{
		//ab*2 (when a == b) - Zero
		glBlendFuncSeparate(GL_ZERO, GL_ZERO, GL_ONE, GL_ZERO);
	}
	else if((alpha.nA == ALPHABLEND_ABD_CS) && (alpha.nB == ALPHABLEND_ABD_CD) && (alpha.nC == ALPHABLEND_C_AS) && (alpha.nD == ALPHABLEND_ABD_CD))
	{
		//0101 - Cs * As + Cd * (1 - As)
		glBlendFuncSeparate(BLEND_SRC_ALPHA, BLEND_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
	}
	else if((alpha.nA == 0) && (alpha.nB == 1) && (alpha.nC == 1) && (alpha.nD == 1))
	{
		//Cs * Ad + Cd * (1 - Ad)
		glBlendFuncSeparate(GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_ONE, GL_ZERO);
	}
	else if((alpha.nA == 0) && (alpha.nB == 1) && (alpha.nC == 2) && (alpha.nD == 1))
	{
		if(alpha.nFix == 0x80)
		{
			glBlendFuncSeparate(GL_ONE, GL_ZERO, GL_ONE, GL_ZERO);
		}
		else
		{
			//Source alpha value is implied in the formula
			//As = FIX / 0x80
			glBlendColor(0.0f, 0.0f, 0.0f, (float)alpha.nFix / 128.0f);
			glBlendFuncSeparate(GL_CONSTANT_ALPHA, GL_ONE_MINUS_CONSTANT_ALPHA, GL_ONE, GL_ZERO);
		}
	}
	else if((alpha.nA == 0) && (alpha.nB == 2) && (alpha.nC == 0) && (alpha.nD == 1))
	{
		glBlendFuncSeparate(BLEND_SRC_ALPHA, GL_ONE, GL_ONE, GL_ZERO);
	}
	else if((alpha.nA == 0) && (alpha.nB == 2) && (alpha.nC == 0) && (alpha.nD == 2))
	{
		//Cs * As
		glBlendFuncSeparate(BLEND_SRC_ALPHA, GL_ZERO, GL_ONE, GL_ZERO);
	}
	else if((alpha.nA == 0) && (alpha.nB == 2) && (alpha.nC == 1) && (alpha.nD == 1))
	{
		//Cs * Ad + Cd
		glBlendFuncSeparate(GL_DST_ALPHA, GL_ONE, GL_ONE, GL_ZERO);
	}
	else if((alpha.nA == 0) && (alpha.nB == 2) && (alpha.nC == 2) && (alpha.nD == 1))
	{
		if(alpha.nFix == 0x80)
		{
			glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ZERO);
		}
		else
		{
			//Cs * FIX + Cd
			glBlendColor(0, 0, 0, static_cast<float>(alpha.nFix) / 128.0f);
			glBlendFuncSeparate(GL_CONSTANT_ALPHA, GL_ONE, GL_ONE, GL_ZERO);
		}
	}
	else if((alpha.nA == ALPHABLEND_ABD_CS) && (alpha.nB == ALPHABLEND_ABD_ZERO) && (alpha.nC == ALPHABLEND_C_FIX) && (alpha.nD == ALPHABLEND_ABD_ZERO))
	{
		//0222 - Cs * FIX
		glBlendColor(0, 0, 0, static_cast<float>(alpha.nFix) / 128.0f);
		glBlendFuncSeparate(GL_CONSTANT_ALPHA, GL_ZERO, GL_ONE, GL_ZERO);
	}
	else if((alpha.nA == 1) && (alpha.nB == 0) && (alpha.nC == 0) && (alpha.nD == 0))
	{
		//(Cd - Cs) * As + Cs
		glBlendFuncSeparate(BLEND_ONE_MINUS_SRC_ALPHA, BLEND_SRC_ALPHA, GL_ONE, GL_ZERO);
	}
	else if((alpha.nA == ALPHABLEND_ABD_CD) && (alpha.nB == ALPHABLEND_ABD_CS) && (alpha.nC == ALPHABLEND_C_AS) && (alpha.nD == ALPHABLEND_ABD_CD))
	{
		//1001 -> (Cd - Cs) * As + Cd (Inaccurate, needs +1 to As)
		nFunction = GL_FUNC_REVERSE_SUBTRACT;
		glBlendFuncSeparate(BLEND_SRC_ALPHA, BLEND_SRC_ALPHA, GL_ONE, GL_ZERO);
	}
	else if((alpha.nA == ALPHABLEND_ABD_CD) && (alpha.nB == ALPHABLEND_ABD_CS) && (alpha.nC == ALPHABLEND_C_AD) && (alpha.nD == ALPHABLEND_ABD_CS))
	{
		//1010 -> Cs * (1 - Ad) + Cd * Ad
		glBlendFuncSeparate(GL_ONE_MINUS_DST_ALPHA, GL_DST_ALPHA, GL_ONE, GL_ZERO);
	}
	else if((alpha.nA == ALPHABLEND_ABD_CD) && (alpha.nB == ALPHABLEND_ABD_CS) && (alpha.nC == ALPHABLEND_C_FIX) && (alpha.nD == ALPHABLEND_ABD_CS))
	{
		//1020 -> Cs * (1 - FIX) + Cd * FIX
		glBlendColor(0, 0, 0, static_cast<float>(alpha.nFix) / 128.0f);
		glBlendFuncSeparate(GL_ONE_MINUS_CONSTANT_ALPHA, GL_CONSTANT_ALPHA, GL_ONE, GL_ZERO);
	}
	else if((alpha.nA == 1) && (alpha.nB == 0) && (alpha.nC == 2) && (alpha.nD == 2))
	{
		nFunction = GL_FUNC_REVERSE_SUBTRACT;
		glBlendColor(0.0f, 0.0f, 0.0f, (float)alpha.nFix / 128.0f);
		glBlendFuncSeparate(GL_CONSTANT_ALPHA, GL_CONSTANT_ALPHA, GL_ONE, GL_ZERO);
	}
	else if((alpha.nA == 1) && (alpha.nB == 2) && (alpha.nC == 0) && (alpha.nD == 0))
	{
		//Cd * As + Cs
		glBlendFuncSeparate(GL_ONE, BLEND_SRC_ALPHA, GL_ONE, GL_ZERO);
	}
	else if((alpha.nA == ALPHABLEND_ABD_CD) && (alpha.nB == ALPHABLEND_ABD_ZERO) && (alpha.nC == ALPHABLEND_C_AS) && (alpha.nD == ALPHABLEND_ABD_CD))
	{
		//1201 -> Cd * (As + 1)
		//TODO: Implement this properly (multiple passes?)
		glBlendFuncSeparate(GL_ZERO, GL_ONE, GL_ONE, GL_ZERO);
	}
	else if((alpha.nA == ALPHABLEND_ABD_CD) && (alpha.nB == ALPHABLEND_ABD_ZERO) && (alpha.nC == ALPHABLEND_C_AS) && (alpha.nD == ALPHABLEND_ABD_ZERO))
	{
		//1202 - Cd * As
		glBlendFuncSeparate(GL_ZERO, BLEND_SRC_ALPHA, GL_ONE, GL_ZERO);
	}
	else if((alpha.nA == ALPHABLEND_ABD_CD) && (alpha.nB == ALPHABLEND_ABD_ZERO) && (alpha.nC == ALPHABLEND_C_FIX) && (alpha.nD == ALPHABLEND_ABD_CS))
	{
		//1220 -> Cd * FIX + Cs
		glBlendColor(0, 0, 0, static_cast<float>(alpha.nFix) / 128.0f);
		glBlendFuncSeparate(GL_ONE, GL_CONSTANT_ALPHA, GL_ONE, GL_ZERO);
	}
	else if((alpha.nA == 1) && (alpha.nB == 2) && (alpha.nC == 2) && (alpha.nD == 2))
	{
		//Cd * FIX
		glBlendColor(0, 0, 0, static_cast<float>(alpha.nFix) / 128.0f);
		glBlendFuncSeparate(GL_ZERO, GL_CONSTANT_ALPHA, GL_ONE, GL_ZERO);
	}
	else if((alpha.nA == ALPHABLEND_ABD_ZERO) && (alpha.nB == ALPHABLEND_ABD_CS) && (alpha.nC == ALPHABLEND_C_AS) && (alpha.nD == ALPHABLEND_ABD_CD))
	{
		//2001 -> Cd - Cs * As
		nFunction = GL_FUNC_REVERSE_SUBTRACT;
		glBlendFuncSeparate(BLEND_SRC_ALPHA, GL_ONE, GL_ONE, GL_ZERO);
	}
	else if((alpha.nA == ALPHABLEND_ABD_ZERO) && (alpha.nB == ALPHABLEND_ABD_CS) && (alpha.nC == ALPHABLEND_C_AD) && (alpha.nD == ALPHABLEND_ABD_CD))
	{
		//2011 -> Cd - Cs * Ad
		nFunction = GL_FUNC_REVERSE_SUBTRACT;
		glBlendFuncSeparate(GL_DST_ALPHA, GL_ONE, GL_ONE, GL_ZERO);
	}
	else if((alpha.nA == ALPHABLEND_ABD_ZERO) && (alpha.nB == ALPHABLEND_ABD_CS) && (alpha.nC == ALPHABLEND_C_FIX) && (alpha.nD == ALPHABLEND_ABD_CD))
	{
		//2021 -> Cd - Cs * FIX
		nFunction = GL_FUNC_REVERSE_SUBTRACT;
		glBlendColor(0, 0, 0, static_cast<float>(alpha.nFix) / 128.0f);
		glBlendFuncSeparate(GL_CONSTANT_ALPHA, GL_ONE, GL_ONE, GL_ZERO);
	}
	else if((alpha.nA == ALPHABLEND_ABD_ZERO) && (alpha.nB == ALPHABLEND_ABD_CD) && (alpha.nC == ALPHABLEND_C_AS) && (alpha.nD == ALPHABLEND_ABD_CD))
	{
		//2101 -> Cd * (1 - As)
		glBlendFuncSeparate(GL_ZERO, BLEND_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
	}
	else
	{
		assert(0);
		//Default blending
		glBlendFuncSeparate(GL_ONE, GL_ZERO, GL_ONE, GL_ZERO);
	}

	glBlendEquationSeparate(nFunction, GL_FUNC_ADD);
#endif
}

void CGSH_OpenGL::SetupTestFunctions(uint64 testReg)
{
	auto test = make_convertible<TEST>(testReg);

	m_fragmentParams.alphaRef = test.nAlphaRef;
	m_validGlState &= ~GLSTATE_FRAGMENT_PARAMS;

	m_renderState.depthTest = (test.nDepthEnabled != 0);
	m_validGlState &= ~GLSTATE_DEPTHTEST;

	if(test.nDepthEnabled)
	{
		unsigned int nFunc = GL_NEVER;

		switch(test.nDepthMethod)
		{
		case DEPTH_TEST_NEVER:
			nFunc = GL_NEVER;
			break;
		case DEPTH_TEST_ALWAYS:
			nFunc = GL_ALWAYS;
			break;
		case DEPTH_TEST_GEQUAL:
			nFunc = GL_GEQUAL;
			break;
		case DEPTH_TEST_GREATER:
			nFunc = GL_GREATER;
			break;
		}

		glDepthFunc(nFunc);
	}
}

void CGSH_OpenGL::SetupDepthBuffer(uint64 zbufReg, uint64 testReg)
{
	auto zbuf = make_convertible<ZBUF>(zbufReg);
	auto test = make_convertible<TEST>(testReg);

	uint32 depthMask = 0;
	switch(CGsPixelFormats::GetPsmPixelSize(zbuf.nPsm))
	{
	case 16:
		depthMask = 0xFFFF;
		break;
	case 24:
		depthMask = 0xFFFFFF;
		break;
	default:
	case 32:
		depthMask = 0xFFFFFFFF;
		break;
	}

	m_fragmentParams.depthMask = depthMask;
	m_validGlState &= ~GLSTATE_FRAGMENT_PARAMS;
}

void CGSH_OpenGL::SetupFramebuffer(uint64 frameReg, uint64 zbufReg, uint64 scissorReg, uint64 testReg)
{
	if(frameReg == 0) return;

	auto frame = make_convertible<FRAME>(frameReg);
	auto zbuf = make_convertible<ZBUF>(zbufReg);
	auto scissor = make_convertible<SCISSOR>(scissorReg);
	auto test = make_convertible<TEST>(testReg);

	bool r = (frame.nMask & 0x000000FF) == 0;
	bool g = (frame.nMask & 0x0000FF00) == 0;
	bool b = (frame.nMask & 0x00FF0000) == 0;
	bool a = (frame.nMask & 0xFF000000) == 0;

	if((test.nAlphaEnabled == 1) && (test.nAlphaMethod == ALPHA_TEST_NEVER))
	{
		if(test.nAlphaFail == ALPHA_TEST_FAIL_RGBONLY)
		{
			a = false;
		}
		else if(test.nAlphaFail == ALPHA_TEST_FAIL_ZBONLY)
		{
			r = g = b = a = false;
		}
	}

	m_renderState.colorMaskR = r;
	m_renderState.colorMaskG = g;
	m_renderState.colorMaskB = b;
	m_renderState.colorMaskA = a;
	m_validGlState &= ~GLSTATE_COLORMASK;

	m_fragmentParams.colorMask = ~frame.nMask;
	m_fragmentParams.frameBufPtr = frame.GetBasePtr();
	m_fragmentParams.frameBufWidth = frame.GetWidth();
	m_fragmentParams.depthBufPtr = zbuf.GetBasePtr();
	m_fragmentParams.depthBufWidth = frame.GetWidth();
	m_validGlState &= ~GLSTATE_FRAGMENT_PARAMS;

	//Check if we're drawing into a buffer that's been used for depth before
	{
		auto zbufWrite = make_convertible<ZBUF>(frameReg);
		auto depthbuffer = FindDepthbuffer(zbufWrite, frame);
		m_drawingToDepth = (depthbuffer != nullptr);
	}

	//Look for a framebuffer that matches the specified information
	auto framebuffer = FindFramebuffer(frame);
	if(!framebuffer)
	{
		framebuffer = FramebufferPtr(new CFramebuffer(frame.GetBasePtr(), frame.GetWidth(), FRAMEBUFFER_HEIGHT, frame.nPsm, m_fbScale, m_multisampleEnabled));
		m_framebuffers.push_back(framebuffer);
		PopulateFramebuffer(framebuffer);
	}

	CommitFramebufferDirtyPages(framebuffer, scissor.scay0, scissor.scay1);

	auto depthbuffer = FindDepthbuffer(zbuf, frame);
	if(!depthbuffer)
	{
		depthbuffer = DepthbufferPtr(new CDepthbuffer(zbuf.GetBasePtr(), frame.GetWidth(), FRAMEBUFFER_HEIGHT, zbuf.nPsm, m_fbScale, m_multisampleEnabled));
		m_depthbuffers.push_back(depthbuffer);
	}

	assert(framebuffer->m_width == depthbuffer->m_width);

	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer->m_framebuffer);

	GLenum result = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	assert(result == GL_FRAMEBUFFER_COMPLETE);

	m_renderState.framebufferHandle = framebuffer->m_framebuffer;
	m_renderState.framebufferTextureHandle = framebuffer->m_texture;
	m_renderState.depthbufferTextureHandle = depthbuffer->m_depthBufferImage;
	m_renderState.frameSwizzleTableHandle = GetSwizzleTable(frame.nPsm);
	//TODO: Swizzle is actually different for depth buffers (PSM | 0x30 to convert)
	m_renderState.depthSwizzleTableHandle = GetSwizzleTable(zbuf.nPsm);
	m_validGlState &= ~GLSTATE_FRAMEBUFFER; //glBindFramebuffer used to set just above

	//We assume that we will be drawing to this framebuffer and that we'll need
	//to resolve samples at some point if multisampling is enabled
	framebuffer->m_resolveNeeded = true;

	{
		GLenum drawBufferId = GL_COLOR_ATTACHMENT0;
		glDrawBuffers(1, &drawBufferId);
		CHECKGLERROR();
	}

	m_renderState.viewportWidth = framebuffer->m_width;
	m_renderState.viewportHeight = framebuffer->m_height;
	m_validGlState &= ~GLSTATE_VIEWPORT;

	float projWidth = static_cast<float>(framebuffer->m_width);
	float projHeight = static_cast<float>(framebuffer->m_height);

	MakeLinearZOrtho(m_vertexParams.projMatrix, 0, projWidth, 0, projHeight);
	m_validGlState &= ~GLSTATE_VERTEX_PARAMS;

	m_renderState.scissorX = scissor.scax0;
	m_renderState.scissorY = scissor.scay0;
	m_renderState.scissorWidth = scissor.scax1 - scissor.scax0 + 1;
	m_renderState.scissorHeight = scissor.scay1 - scissor.scay0 + 1;
	m_validGlState &= ~GLSTATE_SCISSOR;
}

void CGSH_OpenGL::SetupFogColor(uint64 fogColReg)
{
	auto fogCol = make_convertible<FOGCOL>(fogColReg);
	m_fragmentParams.fogColor[0] = static_cast<float>(fogCol.nFCR) / 255.0f;
	m_fragmentParams.fogColor[1] = static_cast<float>(fogCol.nFCG) / 255.0f;
	m_fragmentParams.fogColor[2] = static_cast<float>(fogCol.nFCB) / 255.0f;
	m_validGlState &= ~GLSTATE_FRAGMENT_PARAMS;
}

bool CGSH_OpenGL::CanRegionRepeatClampModeSimplified(uint32 clampMin, uint32 clampMax)
{
	for(unsigned int j = 1; j < 0x3FF; j = ((j << 1) | 1))
	{
		if(clampMin < j) break;
		if(clampMin != j) continue;

		if((clampMin & clampMax) != 0) break;

		return true;
	}

	return false;
}

void CGSH_OpenGL::FillShaderCapsFromTexture(SHADERCAPS& shaderCaps, const uint64& tex0Reg, const uint64& tex1Reg, const uint64& texAReg, const uint64& clampReg)
{
	auto tex0 = make_convertible<TEX0>(tex0Reg);
	auto tex1 = make_convertible<TEX1>(tex1Reg);
	auto texA = make_convertible<TEXA>(texAReg);
	auto clamp = make_convertible<CLAMP>(clampReg);

	shaderCaps.texSourceMode = TEXTURE_SOURCE_MODE_STD;

	if((clamp.nWMS > CLAMP_MODE_CLAMP) || (clamp.nWMT > CLAMP_MODE_CLAMP))
	{
		unsigned int clampMode[2];

		clampMode[0] = g_shaderClampModes[clamp.nWMS];
		clampMode[1] = g_shaderClampModes[clamp.nWMT];

		if(clampMode[0] == TEXTURE_CLAMP_MODE_REGION_REPEAT && CanRegionRepeatClampModeSimplified(clamp.GetMinU(), clamp.GetMaxU())) clampMode[0] = TEXTURE_CLAMP_MODE_REGION_REPEAT_SIMPLE;
		if(clampMode[1] == TEXTURE_CLAMP_MODE_REGION_REPEAT && CanRegionRepeatClampModeSimplified(clamp.GetMinV(), clamp.GetMaxV())) clampMode[1] = TEXTURE_CLAMP_MODE_REGION_REPEAT_SIMPLE;

		shaderCaps.texClampS = clampMode[0];
		shaderCaps.texClampT = clampMode[1];
	}

	if(CGsPixelFormats::IsPsmIDTEX(tex0.nPsm))
	{
		if((tex1.nMinFilter != MIN_FILTER_NEAREST) || (tex1.nMagFilter != MIN_FILTER_NEAREST))
		{
			//We'll need to filter the texture manually
			shaderCaps.texBilinearFilter = 1;
		}

		if(m_forceBilinearTextures)
		{
			shaderCaps.texBilinearFilter = 1;
		}
	}

	if(tex0.nColorComp == 1)
	{
		shaderCaps.texHasAlpha = 1;
	}

	if((tex0.nPsm == PSMCT16) || (tex0.nPsm == PSMCT16S) || (tex0.nPsm == PSMCT24))
	{
		shaderCaps.texUseAlphaExpansion = 1;
	}

	if(CGsPixelFormats::IsPsmIDTEX(tex0.nPsm))
	{
		if((tex0.nCPSM == PSMCT16) || (tex0.nCPSM == PSMCT16S))
		{
			shaderCaps.texUseAlphaExpansion = 1;
		}

		shaderCaps.texSourceMode = CGsPixelFormats::IsPsmIDTEX4(tex0.nPsm) ? TEXTURE_SOURCE_MODE_IDX4 : TEXTURE_SOURCE_MODE_IDX8;
	}

	if(texA.nAEM)
	{
		shaderCaps.texBlackIsTransparent = 1;
	}
	
	shaderCaps.texFunction = tex0.nFunction;
	shaderCaps.texPsm = tex0.nPsm;
	shaderCaps.texCpsm = tex0.nCPSM;
}

void CGSH_OpenGL::FillShaderCapsFromFrame(SHADERCAPS& shaderCaps, const uint64& frameReg)
{
	auto frame = make_convertible<FRAME>(frameReg);
	shaderCaps.framePsm = frame.nPsm;
}

void CGSH_OpenGL::FillShaderCapsFromTestAndZbuf(SHADERCAPS& shaderCaps, const uint64& testReg, const uint64& zbufReg)
{
	auto test = make_convertible<TEST>(testReg);
	auto zbuf = make_convertible<ZBUF>(zbufReg);

	if(test.nAlphaEnabled)
	{
		//Handle special way of turning off color or depth writes
		//Proper write masks will be set at other places
		if((test.nAlphaMethod == ALPHA_TEST_NEVER) && (test.nAlphaFail != ALPHA_TEST_FAIL_KEEP))
		{
			shaderCaps.hasAlphaTest = 0;
		}
		else
		{
			shaderCaps.hasAlphaTest = 1;
			shaderCaps.alphaTestMethod = test.nAlphaMethod;
			shaderCaps.alphaFailResult = test.nAlphaFail;
		}
	}
	else
	{
		shaderCaps.hasAlphaTest = 0;
	}

	if(test.nDepthEnabled)
	{
		shaderCaps.depthTestMethod = test.nDepthMethod;
	}
	else
	{
		shaderCaps.depthTestMethod = DEPTH_TEST_ALWAYS;
	}

	bool depthWriteEnabled = (zbuf.nMask ? false : true);
	//If alpha test is enabled for always failing and update only colors, depth writes are disabled
	if(
	    (test.nAlphaEnabled == 1) &&
	    (test.nAlphaMethod == ALPHA_TEST_NEVER) &&
	    ((test.nAlphaFail == ALPHA_TEST_FAIL_FBONLY) || (test.nAlphaFail == ALPHA_TEST_FAIL_RGBONLY)))
	{
		depthWriteEnabled = false;
	}
	shaderCaps.depthWriteEnabled = depthWriteEnabled;
	shaderCaps.depthPsm = zbuf.nPsm | 0x30;
}

void CGSH_OpenGL::FillShaderCapsFromAlpha(SHADERCAPS& shaderCaps, const uint64& alphaReg)
{
	auto alpha = make_convertible<ALPHA>(alphaReg);
	shaderCaps.blendFactorA = alpha.nA;
	shaderCaps.blendFactorB = alpha.nB;
	shaderCaps.blendFactorC = alpha.nC;
	shaderCaps.blendFactorD = alpha.nD;
}

void CGSH_OpenGL::SetupTexture(uint64 primReg, uint64 tex0Reg, uint64 tex1Reg, uint64 texAReg, uint64 clampReg)
{
	m_renderState.texture0Handle = 0;
	m_renderState.texture1Handle = 0;
	m_renderState.texture0MinFilter = GL_NEAREST;
	m_renderState.texture0MagFilter = GL_NEAREST;
	m_renderState.texture0WrapS = GL_CLAMP_TO_EDGE;
	m_renderState.texture0WrapT = GL_CLAMP_TO_EDGE;
	m_validGlState &= ~GLSTATE_TEXTURE;

	auto prim = make_convertible<PRMODE>(primReg);

	if(tex0Reg == 0 || prim.nTexture == 0)
	{
		return;
	}

	auto tex0 = make_convertible<TEX0>(tex0Reg);
	auto tex1 = make_convertible<TEX1>(tex1Reg);
	auto texA = make_convertible<TEXA>(texAReg);
	auto clamp = make_convertible<CLAMP>(clampReg);

	m_nTexWidth = tex0.GetWidth();
	m_nTexHeight = tex0.GetHeight();

	auto texInfo = PrepareTexture(tex0);
	m_renderState.texture0Handle = texInfo.textureHandle;
	m_renderState.texture0AlphaAsIndex = texInfo.alphaAsIndex;

	//Setup sampling modes
	switch(tex1.nMagFilter)
	{
	case MAG_FILTER_NEAREST:
		m_renderState.texture0MagFilter = GL_NEAREST;
		break;
	case MAG_FILTER_LINEAR:
		m_renderState.texture0MagFilter = GL_LINEAR;
		break;
	}

	switch(tex1.nMinFilter)
	{
	case MIN_FILTER_NEAREST:
	case MIN_FILTER_NEAREST_MIP_NEAREST:
	case MIN_FILTER_NEAREST_MIP_LINEAR:
		m_renderState.texture0MinFilter = GL_NEAREST;
		break;
	case MIN_FILTER_LINEAR:
	case MIN_FILTER_LINEAR_MIP_NEAREST:
	case MIN_FILTER_LINEAR_MIP_LINEAR:
		m_renderState.texture0MinFilter = GL_LINEAR;
		break;
	default:
		assert(0);
		break;
	}

	if(m_forceBilinearTextures)
	{
		m_renderState.texture0MagFilter = GL_LINEAR;
		m_renderState.texture0MinFilter = GL_LINEAR;
	}

	unsigned int clampMin[2] = {0, 0};
	unsigned int clampMax[2] = {0, 0};
	float textureScaleRatio[2] = {texInfo.scaleRatioX, texInfo.scaleRatioY};
	m_renderState.texture0WrapS = g_nativeClampModes[clamp.nWMS];
	m_renderState.texture0WrapT = g_nativeClampModes[clamp.nWMT];

	if((clamp.nWMS > CLAMP_MODE_CLAMP) || (clamp.nWMT > CLAMP_MODE_CLAMP))
	{
		unsigned int clampMode[2];

		clampMode[0] = g_shaderClampModes[clamp.nWMS];
		clampMode[1] = g_shaderClampModes[clamp.nWMT];

		clampMin[0] = clamp.GetMinU();
		clampMin[1] = clamp.GetMinV();

		clampMax[0] = clamp.GetMaxU();
		clampMax[1] = clamp.GetMaxV();

		for(unsigned int i = 0; i < 2; i++)
		{
			if(clampMode[i] == TEXTURE_CLAMP_MODE_REGION_REPEAT)
			{
				if(CanRegionRepeatClampModeSimplified(clampMin[i], clampMax[i]))
				{
					clampMin[i]++;
				}
			}
			if(clampMode[i] == TEXTURE_CLAMP_MODE_REGION_CLAMP)
			{
				clampMin[i] = static_cast<unsigned int>(static_cast<float>(clampMin[i]) * textureScaleRatio[i]);
				clampMax[i] = static_cast<unsigned int>(static_cast<float>(clampMax[i]) * textureScaleRatio[i]);
			}
		}
	}

	if(CGsPixelFormats::IsPsmIDTEX(tex0.nPsm) &&
	   (m_renderState.texture0MinFilter != GL_NEAREST || m_renderState.texture0MagFilter != GL_NEAREST))
	{
		//We'll need to filter the texture manually
		m_renderState.texture0MinFilter = GL_NEAREST;
		m_renderState.texture0MagFilter = GL_NEAREST;
	}

	if(CGsPixelFormats::IsPsmIDTEX(tex0.nPsm))
	{
		m_renderState.texture1Handle = PreparePalette(tex0);
	}

	m_renderState.textureSwizzleTableHandle = GetSwizzleTable(tex0.nPsm);

	memset(m_vertexParams.texMatrix, 0, sizeof(m_vertexParams.texMatrix));
	m_vertexParams.texMatrix[0 + (0 * 4)] = texInfo.scaleRatioX;
	m_vertexParams.texMatrix[1 + (1 * 4)] = texInfo.scaleRatioY;
	m_vertexParams.texMatrix[2 + (2 * 4)] = 1;
	m_vertexParams.texMatrix[0 + (3 * 4)] = texInfo.offsetX;
	m_vertexParams.texMatrix[3 + (3 * 4)] = 1;
	m_validGlState &= ~GLSTATE_VERTEX_PARAMS;

	m_fragmentParams.textureSize[0] = static_cast<float>(tex0.GetWidth());
	m_fragmentParams.textureSize[1] = static_cast<float>(tex0.GetHeight());
	m_fragmentParams.texelSize[0] = 1.0f / static_cast<float>(tex0.GetWidth());
	m_fragmentParams.texelSize[1] = 1.0f / static_cast<float>(tex0.GetHeight());
	m_fragmentParams.clampMin[0] = static_cast<float>(clampMin[0]);
	m_fragmentParams.clampMin[1] = static_cast<float>(clampMin[1]);
	m_fragmentParams.clampMax[0] = static_cast<float>(clampMax[0]);
	m_fragmentParams.clampMax[1] = static_cast<float>(clampMax[1]);
	m_fragmentParams.texA0 = static_cast<float>(texA.nTA0) / 255.f;
	m_fragmentParams.texA1 = static_cast<float>(texA.nTA1) / 255.f;
	m_fragmentParams.textureBufPtr = tex0.GetBufPtr();
	m_fragmentParams.textureBufWidth = tex0.GetBufWidth();
	m_fragmentParams.textureCsa = tex0.nCSA & 0x0F;
	m_validGlState &= ~GLSTATE_FRAGMENT_PARAMS;
}

CGSH_OpenGL::FramebufferPtr CGSH_OpenGL::FindFramebuffer(const FRAME& frame) const
{
	auto framebufferIterator = std::find_if(std::begin(m_framebuffers), std::end(m_framebuffers),
	                                        [&](const FramebufferPtr& framebuffer) {
		                                        return (framebuffer->m_basePtr == frame.GetBasePtr()) &&
		                                               (framebuffer->m_psm == frame.nPsm) &&
		                                               (framebuffer->m_width == frame.GetWidth());
	                                        });

	return (framebufferIterator != std::end(m_framebuffers)) ? *(framebufferIterator) : FramebufferPtr();
}

CGSH_OpenGL::DepthbufferPtr CGSH_OpenGL::FindDepthbuffer(const ZBUF& zbuf, const FRAME& frame) const
{
	auto depthbufferIterator = std::find_if(std::begin(m_depthbuffers), std::end(m_depthbuffers),
	                                        [&](const DepthbufferPtr& depthbuffer) {
		                                        return (depthbuffer->m_basePtr == zbuf.GetBasePtr()) && (depthbuffer->m_width == frame.GetWidth());
	                                        });

	return (depthbufferIterator != std::end(m_depthbuffers)) ? *(depthbufferIterator) : DepthbufferPtr();
}

GLuint CGSH_OpenGL::GetSwizzleTable(uint32 psm) const
{
	switch(psm)
	{
	case PSMCT32:
	case PSMCT24:
	case PSMT8H:
	case PSMT4HL:
	case PSMT4HH:
		return m_swizzleTexturePSMCT32;
	case PSMCT16:
		return m_swizzleTexturePSMCT16;
	case PSMCT16S:
		return m_swizzleTexturePSMCT16S;
	case PSMT8:
		return m_swizzleTexturePSMT8;
	case PSMT4:
		return m_swizzleTexturePSMT4;
	default:
		assert(false);
		return 0;
	}
}

/////////////////////////////////////////////////////////////
// Individual Primitives Implementations
/////////////////////////////////////////////////////////////

void CGSH_OpenGL::Prim_Point()
{
	auto xyz = make_convertible<XYZ>(m_VtxBuffer[0].nPosition);
	auto rgbaq = make_convertible<RGBAQ>(m_VtxBuffer[0].nRGBAQ);

	float x = xyz.GetX();
	float y = xyz.GetY();
	uint32 z = xyz.nZ;

	x -= m_nPrimOfsX;
	y -= m_nPrimOfsY;

	auto color = MakeColor(
	    rgbaq.nR, rgbaq.nG,
	    rgbaq.nB, rgbaq.nA);

	// clang-format off
	PRIM_VERTEX vertex =
	{
		//x, y, z, color, s, t, q, f
		  x, y, z, color, 0, 0, 1, 0,
	};
	// clang-format on

	assert((m_vertexBuffer.size() + 1) <= VERTEX_BUFFER_SIZE);
	m_vertexBuffer.push_back(vertex);
}

void CGSH_OpenGL::Prim_Line()
{
	XYZ xyz[2];
	xyz[0] <<= m_VtxBuffer[1].nPosition;
	xyz[1] <<= m_VtxBuffer[0].nPosition;

	float nX1 = xyz[0].GetX();
	float nY1 = xyz[0].GetY();
	uint32 nZ1 = xyz[0].nZ;
	float nX2 = xyz[1].GetX();
	float nY2 = xyz[1].GetY();
	uint32 nZ2 = xyz[1].nZ;

	nX1 -= m_nPrimOfsX;
	nX2 -= m_nPrimOfsX;

	nY1 -= m_nPrimOfsY;
	nY2 -= m_nPrimOfsY;

	RGBAQ rgbaq[2];
	rgbaq[0] <<= m_VtxBuffer[1].nRGBAQ;
	rgbaq[1] <<= m_VtxBuffer[0].nRGBAQ;

	float nS[2] = {0, 0};
	float nT[2] = {0, 0};
	float nQ[2] = {1, 1};

	auto color1 = MakeColor(
	    rgbaq[0].nR, rgbaq[0].nG,
	    rgbaq[0].nB, rgbaq[0].nA);

	auto color2 = MakeColor(
	    rgbaq[1].nR, rgbaq[1].nG,
	    rgbaq[1].nB, rgbaq[1].nA);

	// clang-format off
	PRIM_VERTEX vertices[] =
	{
		{	nX1,	nY1,	nZ1,	color1,	nS[0],	nT[0],	nQ[0],	0	},
		{	nX2,	nY2,	nZ2,	color2,	nS[1],	nT[1],	nQ[1],	0	},
	};
	// clang-format on

	assert((m_vertexBuffer.size() + 2) <= VERTEX_BUFFER_SIZE);
	m_vertexBuffer.insert(m_vertexBuffer.end(), std::begin(vertices), std::end(vertices));
}

void CGSH_OpenGL::Prim_Triangle()
{
	float nF1, nF2, nF3;

	XYZ vertex[3];
	vertex[0] <<= m_VtxBuffer[2].nPosition;
	vertex[1] <<= m_VtxBuffer[1].nPosition;
	vertex[2] <<= m_VtxBuffer[0].nPosition;

	float nX1 = vertex[0].GetX(), nX2 = vertex[1].GetX(), nX3 = vertex[2].GetX();
	float nY1 = vertex[0].GetY(), nY2 = vertex[1].GetY(), nY3 = vertex[2].GetY();
	uint32 nZ1 = vertex[0].nZ, nZ2 = vertex[1].nZ, nZ3 = vertex[2].nZ;

	RGBAQ rgbaq[3];
	rgbaq[0] <<= m_VtxBuffer[2].nRGBAQ;
	rgbaq[1] <<= m_VtxBuffer[1].nRGBAQ;
	rgbaq[2] <<= m_VtxBuffer[0].nRGBAQ;

	nX1 -= m_nPrimOfsX;
	nX2 -= m_nPrimOfsX;
	nX3 -= m_nPrimOfsX;

	nY1 -= m_nPrimOfsY;
	nY2 -= m_nPrimOfsY;
	nY3 -= m_nPrimOfsY;

	if(m_PrimitiveMode.nFog)
	{
		nF1 = (float)(0xFF - m_VtxBuffer[2].nFog) / 255.0f;
		nF2 = (float)(0xFF - m_VtxBuffer[1].nFog) / 255.0f;
		nF3 = (float)(0xFF - m_VtxBuffer[0].nFog) / 255.0f;
	}
	else
	{
		nF1 = nF2 = nF3 = 0.0;
	}

	float nS[3] = {0, 0, 0};
	float nT[3] = {0, 0, 0};
	float nQ[3] = {1, 1, 1};

	if(m_PrimitiveMode.nTexture)
	{
		if(m_PrimitiveMode.nUseUV)
		{
			UV uv[3];
			uv[0] <<= m_VtxBuffer[2].nUV;
			uv[1] <<= m_VtxBuffer[1].nUV;
			uv[2] <<= m_VtxBuffer[0].nUV;

			nS[0] = uv[0].GetU() / static_cast<float>(m_nTexWidth);
			nS[1] = uv[1].GetU() / static_cast<float>(m_nTexWidth);
			nS[2] = uv[2].GetU() / static_cast<float>(m_nTexWidth);

			nT[0] = uv[0].GetV() / static_cast<float>(m_nTexHeight);
			nT[1] = uv[1].GetV() / static_cast<float>(m_nTexHeight);
			nT[2] = uv[2].GetV() / static_cast<float>(m_nTexHeight);
		}
		else
		{
			ST st[3];
			st[0] <<= m_VtxBuffer[2].nST;
			st[1] <<= m_VtxBuffer[1].nST;
			st[2] <<= m_VtxBuffer[0].nST;

			nS[0] = st[0].nS;
			nS[1] = st[1].nS;
			nS[2] = st[2].nS;
			nT[0] = st[0].nT;
			nT[1] = st[1].nT;
			nT[2] = st[2].nT;

			bool isQ0Neg = (rgbaq[0].nQ < 0);
			bool isQ1Neg = (rgbaq[1].nQ < 0);
			bool isQ2Neg = (rgbaq[2].nQ < 0);

			assert(isQ0Neg == isQ1Neg);
			assert(isQ1Neg == isQ2Neg);

			nQ[0] = rgbaq[0].nQ;
			nQ[1] = rgbaq[1].nQ;
			nQ[2] = rgbaq[2].nQ;
		}
	}

	auto color1 = MakeColor(
	    rgbaq[0].nR, rgbaq[0].nG,
	    rgbaq[0].nB, rgbaq[0].nA);

	auto color2 = MakeColor(
	    rgbaq[1].nR, rgbaq[1].nG,
	    rgbaq[1].nB, rgbaq[1].nA);

	auto color3 = MakeColor(
	    rgbaq[2].nR, rgbaq[2].nG,
	    rgbaq[2].nB, rgbaq[2].nA);

	if(m_PrimitiveMode.nShading == 0)
	{
		//Flat shaded triangles use the last color set
		color1 = color2 = color3;
	}

	// clang-format off
	PRIM_VERTEX vertices[] =
	{
		{	nX1,	nY1,	nZ1,	color1,	nS[0],	nT[0],	nQ[0],	nF1	},
		{	nX2,	nY2,	nZ2,	color2,	nS[1],	nT[1],	nQ[1],	nF2	},
		{	nX3,	nY3,	nZ3,	color3,	nS[2],	nT[2],	nQ[2],	nF3	},
	};
	// clang-format on

	assert((m_vertexBuffer.size() + 3) <= VERTEX_BUFFER_SIZE);
	m_vertexBuffer.insert(m_vertexBuffer.end(), std::begin(vertices), std::end(vertices));
}

void CGSH_OpenGL::Prim_Sprite()
{
	XYZ xyz[2];
	xyz[0] <<= m_VtxBuffer[1].nPosition;
	xyz[1] <<= m_VtxBuffer[0].nPosition;

	float nX1 = xyz[0].GetX();
	float nY1 = xyz[0].GetY();
	float nX2 = xyz[1].GetX();
	float nY2 = xyz[1].GetY();
	uint32 nZ = xyz[1].nZ;

	RGBAQ rgbaq[2];
	rgbaq[0] <<= m_VtxBuffer[1].nRGBAQ;
	rgbaq[1] <<= m_VtxBuffer[0].nRGBAQ;

	nX1 -= m_nPrimOfsX;
	nX2 -= m_nPrimOfsX;

	nY1 -= m_nPrimOfsY;
	nY2 -= m_nPrimOfsY;

	float nS[2] = {0, 0};
	float nT[2] = {0, 0};

	if(m_PrimitiveMode.nTexture)
	{
		if(m_PrimitiveMode.nUseUV)
		{
			UV uv[2];
			uv[0] <<= m_VtxBuffer[1].nUV;
			uv[1] <<= m_VtxBuffer[0].nUV;

			nS[0] = uv[0].GetU() / static_cast<float>(m_nTexWidth);
			nS[1] = uv[1].GetU() / static_cast<float>(m_nTexWidth);

			nT[0] = uv[0].GetV() / static_cast<float>(m_nTexHeight);
			nT[1] = uv[1].GetV() / static_cast<float>(m_nTexHeight);
		}
		else
		{
			ST st[2];

			st[0] <<= m_VtxBuffer[1].nST;
			st[1] <<= m_VtxBuffer[0].nST;

			float nQ1 = rgbaq[1].nQ;
			float nQ2 = rgbaq[0].nQ;
			if(nQ1 == 0) nQ1 = 1;
			if(nQ2 == 0) nQ2 = 1;

			nS[0] = st[0].nS / nQ1;
			nS[1] = st[1].nS / nQ2;

			nT[0] = st[0].nT / nQ1;
			nT[1] = st[1].nT / nQ2;
		}
	}

	auto color = MakeColor(
	    rgbaq[1].nR, rgbaq[1].nG,
	    rgbaq[1].nB, rgbaq[1].nA);

	// clang-format off
	PRIM_VERTEX vertices[] =
	{
		{nX1, nY1, nZ, color, nS[0], nT[0], 1, 0},
		{nX2, nY1, nZ, color, nS[1], nT[0], 1, 0},
		{nX1, nY2, nZ, color, nS[0], nT[1], 1, 0},

		{nX1, nY2, nZ, color, nS[0], nT[1], 1, 0},
		{nX2, nY1, nZ, color, nS[1], nT[0], 1, 0},
		{nX2, nY2, nZ, color, nS[1], nT[1], 1, 0},
	};
	// clang-format on

	assert((m_vertexBuffer.size() + 6) <= VERTEX_BUFFER_SIZE);
	m_vertexBuffer.insert(m_vertexBuffer.end(), std::begin(vertices), std::end(vertices));
}

void CGSH_OpenGL::FlushVertexBuffer()
{
	if(m_vertexBuffer.empty()) return;

	assert(m_renderState.isValid == true);

	auto shader = GetShaderFromCaps(m_renderState.shaderCaps);
	if(*shader != m_renderState.shaderHandle)
	{
		m_renderState.shaderHandle = *shader;
		m_validGlState &= ~GLSTATE_PROGRAM;
	}
	DoRenderPass();

	m_vertexBuffer.clear();
}

void CGSH_OpenGL::DoRenderPass()
{
	if((m_validGlState & GLSTATE_VERTEX_PARAMS) == 0)
	{
		glBindBuffer(GL_UNIFORM_BUFFER, m_vertexParamsBuffer);
		glBufferData(GL_UNIFORM_BUFFER, sizeof(VERTEXPARAMS), &m_vertexParams, GL_STREAM_DRAW);
		CHECKGLERROR();
		m_validGlState |= GLSTATE_VERTEX_PARAMS;
	}

	if((m_validGlState & GLSTATE_FRAGMENT_PARAMS) == 0)
	{
		glBindBuffer(GL_UNIFORM_BUFFER, m_fragmentParamsBuffer);
		glBufferData(GL_UNIFORM_BUFFER, sizeof(FRAGMENTPARAMS), &m_fragmentParams, GL_STREAM_DRAW);
		CHECKGLERROR();
		m_validGlState |= GLSTATE_FRAGMENT_PARAMS;
	}

	if((m_validGlState & GLSTATE_PROGRAM) == 0)
	{
		glUseProgram(m_renderState.shaderHandle);
		m_validGlState |= GLSTATE_PROGRAM;
	}

	if((m_validGlState & GLSTATE_VIEWPORT) == 0)
	{
		glViewport(0, 0, m_renderState.viewportWidth * m_fbScale, m_renderState.viewportHeight * m_fbScale);
		m_validGlState |= GLSTATE_VIEWPORT;
	}

	if((m_validGlState & GLSTATE_SCISSOR) == 0)
	{
		glEnable(GL_SCISSOR_TEST);
		glScissor(m_renderState.scissorX * m_fbScale, m_renderState.scissorY * m_fbScale,
		          m_renderState.scissorWidth * m_fbScale, m_renderState.scissorHeight * m_fbScale);
		m_validGlState |= GLSTATE_SCISSOR;
	}

	if((m_validGlState & GLSTATE_BLEND) == 0)
	{
		//m_renderState.blendEnabled ? glEnable(GL_BLEND) : glDisable(GL_BLEND);
		m_validGlState |= GLSTATE_BLEND;
	}

	if((m_validGlState & GLSTATE_DEPTHTEST) == 0)
	{
		//m_renderState.depthTest ? glEnable(GL_DEPTH_TEST) : glDisable(GL_DEPTH_TEST);
		m_validGlState |= GLSTATE_DEPTHTEST;
	}

	if((m_validGlState & GLSTATE_COLORMASK) == 0)
	{
		//glColorMask(
		//    m_renderState.colorMaskR, m_renderState.colorMaskG,
		//    m_renderState.colorMaskB, m_renderState.colorMaskA);
		m_validGlState |= GLSTATE_COLORMASK;
	}

	if((m_validGlState & GLSTATE_DEPTHMASK) == 0)
	{
		//glDepthMask(m_renderState.depthMask ? GL_TRUE : GL_FALSE);
		m_validGlState |= GLSTATE_DEPTHMASK;
	}

	if((m_validGlState & GLSTATE_TEXTURE) == 0)
	{
		glBindImageTexture(SHADER_IMAGE_MEMORY, m_memoryTexture, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
		CHECKGLERROR();

		glBindImageTexture(SHADER_IMAGE_CLUT, m_clutTexture, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32UI);
		CHECKGLERROR();

		glBindImageTexture(SHADER_IMAGE_TEXTURE_SWIZZLE, m_renderState.textureSwizzleTableHandle, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32UI);
		CHECKGLERROR();

#if 0
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, m_renderState.texture0Handle);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, m_renderState.texture0MinFilter);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, m_renderState.texture0MagFilter);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, m_renderState.texture0WrapS);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, m_renderState.texture0WrapT);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, m_renderState.texture0AlphaAsIndex ? GL_ALPHA : GL_RED);

		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, m_renderState.texture1Handle);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
#endif

		m_validGlState |= GLSTATE_TEXTURE;
	}

	if((m_validGlState & GLSTATE_FRAMEBUFFER) == 0)
	{
		glBindFramebuffer(GL_FRAMEBUFFER, m_renderState.framebufferHandle);
		glBindImageTexture(SHADER_IMAGE_FRAME_SWIZZLE, m_renderState.frameSwizzleTableHandle, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32UI);
		glBindImageTexture(SHADER_IMAGE_DEPTH_SWIZZLE, m_renderState.depthSwizzleTableHandle, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32UI);
		CHECKGLERROR();
		m_validGlState |= GLSTATE_FRAMEBUFFER;
	}

	glBindBufferBase(GL_UNIFORM_BUFFER, 0, m_vertexParamsBuffer);
	glBindBufferBase(GL_UNIFORM_BUFFER, 1, m_fragmentParamsBuffer);

	glBindBuffer(GL_ARRAY_BUFFER, m_primBuffer);
	glBufferData(GL_ARRAY_BUFFER, sizeof(PRIM_VERTEX) * m_vertexBuffer.size(), m_vertexBuffer.data(), GL_STREAM_DRAW);

	glBindVertexArray(m_primVertexArray);

	GLenum primitiveMode = GL_NONE;
	switch(m_primitiveType)
	{
	case PRIM_POINT:
		primitiveMode = GL_POINTS;
		break;
	case PRIM_LINE:
	case PRIM_LINESTRIP:
		primitiveMode = GL_LINES;
		break;
	case PRIM_TRIANGLE:
	case PRIM_TRIANGLESTRIP:
	case PRIM_TRIANGLEFAN:
	case PRIM_SPRITE:
		primitiveMode = GL_TRIANGLES;
		break;
	default:
		assert(false);
		break;
	}

	glDrawArrays(primitiveMode, 0, m_vertexBuffer.size());

	m_drawCallCount++;
}

void CGSH_OpenGL::DrawToDepth(unsigned int primitiveType, uint64 primReg)
{
	//A game might be attempting to clear depth by using the zbuffer
	//as a frame buffer and drawing a black sprite into it
	//Space Harrier does this

	//Must be flat, no texture map, no fog, no blend and no aa
	if((primReg & 0x1F8) != 0) return;

	//Must be a sprite
	if(primitiveType != PRIM_SPRITE) return;

	//assert(false);
#if 0
	//Invalidate state
	FlushVertexBuffer();
	m_renderState.isValid = false;

	auto prim = make_convertible<PRMODE>(primReg);

	uint64 frameReg = m_nReg[GS_REG_FRAME_1 + prim.nContext];

	auto frame = make_convertible<FRAME>(frameReg);
	auto zbufWrite = make_convertible<ZBUF>(frameReg);

	auto depthbuffer = FindDepthbuffer(zbufWrite, frame);
	assert(depthbuffer);

	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthbuffer->m_depthBuffer);
	CHECKGLERROR();

	GLenum result = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	assert(result == GL_FRAMEBUFFER_COMPLETE);

	glDepthMask(GL_TRUE);
	glClearDepthf(0);
	glClear(GL_DEPTH_BUFFER_BIT);

	m_validGlState &= ~GLSTATE_DEPTHMASK;
#endif
}

void CGSH_OpenGL::CopyToFb(
    int32 srcX0, int32 srcY0, int32 srcX1, int32 srcY1,
    int32 srcWidth, int32 srcHeight,
    int32 dstX0, int32 dstY0, int32 dstX1, int32 dstY1)
{
	m_validGlState &= ~(GLSTATE_BLEND | GLSTATE_COLORMASK | GLSTATE_SCISSOR | GLSTATE_PROGRAM);
	m_validGlState &= ~(GLSTATE_VIEWPORT | GLSTATE_DEPTHTEST | GLSTATE_DEPTHMASK);

	assert(srcX1 >= srcX0);
	assert(srcY1 >= srcY0);
	assert(dstX1 >= dstX0);
	assert(dstY1 >= dstY0);

	float positionX = static_cast<float>(srcX0) / static_cast<float>(srcWidth);
	float positionY = static_cast<float>(srcY0) / static_cast<float>(srcHeight);
	float sizeX = static_cast<float>(srcX1 - srcX0) / static_cast<float>(srcWidth);
	float sizeY = static_cast<float>(srcY1 - srcY0) / static_cast<float>(srcHeight);

	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_SCISSOR_TEST);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glDepthMask(GL_FALSE);

	glUseProgram(*m_copyToFbProgram);

	glUniform2f(m_copyToFbSrcPositionUniform, positionX, positionY);
	glUniform2f(m_copyToFbSrcSizeUniform, sizeX, sizeY);
	glViewport(dstX0, dstY0, dstX1 - dstX0, dstY1 - dstY0);

	glBindBuffer(GL_ARRAY_BUFFER, m_copyToFbVertexBuffer);
	glBindVertexArray(m_copyToFbVertexArray);

#ifdef _DEBUG
	m_copyToFbProgram->Validate();
#endif

	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

	CHECKGLERROR();
}

/////////////////////////////////////////////////////////////
// Other Functions
/////////////////////////////////////////////////////////////

void CGSH_OpenGL::WriteRegisterImpl(uint8 nRegister, uint64 nData)
{
	CGSHandler::WriteRegisterImpl(nRegister, nData);

	switch(nRegister)
	{
	case GS_REG_PRIM:
	{
		unsigned int newPrimitiveType = static_cast<unsigned int>(nData & 0x07);
		if(newPrimitiveType != m_primitiveType)
		{
			FlushVertexBuffer();
		}
		m_primitiveType = newPrimitiveType;
		switch(m_primitiveType)
		{
		case PRIM_POINT:
			m_nVtxCount = 1;
			break;
		case PRIM_LINE:
		case PRIM_LINESTRIP:
			m_nVtxCount = 2;
			break;
		case PRIM_TRIANGLE:
		case PRIM_TRIANGLESTRIP:
		case PRIM_TRIANGLEFAN:
			m_nVtxCount = 3;
			break;
		case PRIM_SPRITE:
			m_nVtxCount = 2;
			break;
		}
	}
	break;

	case GS_REG_XYZ2:
	case GS_REG_XYZ3:
	case GS_REG_XYZF2:
	case GS_REG_XYZF3:
		VertexKick(nRegister, nData);
		break;
	}
}

void CGSH_OpenGL::VertexKick(uint8 nRegister, uint64 nValue)
{
	if(m_nVtxCount == 0) return;

	bool nDrawingKick = (nRegister == GS_REG_XYZ2) || (nRegister == GS_REG_XYZF2);
	bool nFog = (nRegister == GS_REG_XYZF2) || (nRegister == GS_REG_XYZF3);

	if(!m_drawEnabled) nDrawingKick = false;

	if(nFog)
	{
		m_VtxBuffer[m_nVtxCount - 1].nPosition = nValue & 0x00FFFFFFFFFFFFFFULL;
		m_VtxBuffer[m_nVtxCount - 1].nRGBAQ = m_nReg[GS_REG_RGBAQ];
		m_VtxBuffer[m_nVtxCount - 1].nUV = m_nReg[GS_REG_UV];
		m_VtxBuffer[m_nVtxCount - 1].nST = m_nReg[GS_REG_ST];
		m_VtxBuffer[m_nVtxCount - 1].nFog = (uint8)(nValue >> 56);
	}
	else
	{
		m_VtxBuffer[m_nVtxCount - 1].nPosition = nValue;
		m_VtxBuffer[m_nVtxCount - 1].nRGBAQ = m_nReg[GS_REG_RGBAQ];
		m_VtxBuffer[m_nVtxCount - 1].nUV = m_nReg[GS_REG_UV];
		m_VtxBuffer[m_nVtxCount - 1].nST = m_nReg[GS_REG_ST];
		m_VtxBuffer[m_nVtxCount - 1].nFog = (uint8)(m_nReg[GS_REG_FOG] >> 56);
	}

	m_nVtxCount--;

	if(m_nVtxCount == 0)
	{
		if((m_nReg[GS_REG_PRMODECONT] & 1) != 0)
		{
			m_PrimitiveMode <<= m_nReg[GS_REG_PRIM];
		}
		else
		{
			m_PrimitiveMode <<= m_nReg[GS_REG_PRMODE];
		}

		if(nDrawingKick)
		{
			SetRenderingContext(m_PrimitiveMode);
		}

		switch(m_primitiveType)
		{
		case PRIM_POINT:
			if(nDrawingKick) Prim_Point();
			m_nVtxCount = 1;
			break;
		case PRIM_LINE:
			if(nDrawingKick) Prim_Line();
			m_nVtxCount = 2;
			break;
		case PRIM_LINESTRIP:
			if(nDrawingKick) Prim_Line();
			memcpy(&m_VtxBuffer[1], &m_VtxBuffer[0], sizeof(VERTEX));
			m_nVtxCount = 1;
			break;
		case PRIM_TRIANGLE:
			if(nDrawingKick) Prim_Triangle();
			m_nVtxCount = 3;
			break;
		case PRIM_TRIANGLESTRIP:
			if(nDrawingKick) Prim_Triangle();
			memcpy(&m_VtxBuffer[2], &m_VtxBuffer[1], sizeof(VERTEX));
			memcpy(&m_VtxBuffer[1], &m_VtxBuffer[0], sizeof(VERTEX));
			m_nVtxCount = 1;
			break;
		case PRIM_TRIANGLEFAN:
			if(nDrawingKick) Prim_Triangle();
			memcpy(&m_VtxBuffer[1], &m_VtxBuffer[0], sizeof(VERTEX));
			m_nVtxCount = 1;
			break;
		case PRIM_SPRITE:
			if(nDrawingKick) Prim_Sprite();
			m_nVtxCount = 2;
			break;
		}

		if(nDrawingKick && m_drawingToDepth)
		{
			DrawToDepth(m_primitiveType, m_PrimitiveMode);
		}
	}
}

void CGSH_OpenGL::ProcessHostToLocalTransfer()
{
	FlushVertexBuffer();
	m_renderState.isTextureStateValid = false;
	m_renderState.isFramebufferStateValid = false;

	auto bltBuf = make_convertible<BITBLTBUF>(m_nReg[GS_REG_BITBLTBUF]);
	auto trxReg = make_convertible<TRXREG>(m_nReg[GS_REG_TRXREG]);
	auto trxPos = make_convertible<TRXPOS>(m_nReg[GS_REG_TRXPOS]);

	uint32 transferAddress = bltBuf.GetDstPtr();

	//if(m_trxCtx.nDirty)
	{
		uint32 pixelCount = 0;
		Framework::OpenGl::ProgramPtr xferProgram;
		GLuint xferSwizzleTable = GetSwizzleTable(bltBuf.nDstPsm);

		switch(bltBuf.nDstPsm)
		{
		case PSMCT32:
			pixelCount = m_trxCtx.offset / 4;
			xferProgram = m_xferProgramPSMCT32;
			break;
		case PSMCT16:
			pixelCount = m_trxCtx.offset / 2;
			xferProgram = m_xferProgramPSMCT16;
			break;
		case PSMT8:
			pixelCount = m_trxCtx.offset;
			xferProgram = m_xferProgramPSMT8;
			break;
		case PSMT4:
			pixelCount = m_trxCtx.offset * 2;
			xferProgram = m_xferProgramPSMT4;
			break;
		case PSMT8H:
			pixelCount = m_trxCtx.offset;
			xferProgram = m_xferProgramPSMT8H;
			break;
		case PSMT4HL:
			pixelCount = m_trxCtx.offset * 2;
			xferProgram = m_xferProgramPSMT4HL;
			break;
		case PSMT4HH:
			pixelCount = m_trxCtx.offset * 2;
			xferProgram = m_xferProgramPSMT4HH;
			break;
		default:
			assert(false);
			break;
		}

		uint32 workUnits = (pixelCount + g_xferWorkGroupSize - 1) / g_xferWorkGroupSize;

		///////

		//Update trx buffer
		glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_xferBuffer);
		glBufferData(GL_SHADER_STORAGE_BUFFER, m_trxCtx.offset, m_trxBuffer.data(), GL_STREAM_DRAW);
		CHECKGLERROR();

		XFERPARAMS xferParams;
		xferParams.pixelCount = pixelCount;
		xferParams.bufAddress = bltBuf.GetDstPtr();
		xferParams.bufWidth = bltBuf.GetDstWidth();
		xferParams.rrw = trxReg.nRRW;
		xferParams.dsax = trxPos.nDSAX;
		xferParams.dsay = trxPos.nDSAY;

		glBindBuffer(GL_UNIFORM_BUFFER, m_xferParamsBuffer);
		glBufferData(GL_UNIFORM_BUFFER, sizeof(XFERPARAMS), &xferParams, GL_STREAM_DRAW);
		CHECKGLERROR();

		//Setup compute dispatch
		glUseProgram(*xferProgram);
		m_validGlState &= ~GLSTATE_PROGRAM;

		glBindBufferBase(GL_UNIFORM_BUFFER, 0, m_xferParamsBuffer);
		CHECKGLERROR();

		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_xferBuffer);
		CHECKGLERROR();

		glBindImageTexture(SHADER_IMAGE_MEMORY, m_memoryTexture, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32UI);
		CHECKGLERROR();

		glBindImageTexture(SHADER_IMAGE_XFER_SWIZZLE, xferSwizzleTable, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32UI);
		CHECKGLERROR();

		m_validGlState &= ~GLSTATE_TEXTURE;

#ifdef _DEBUG
		xferProgram->Validate();
#endif

		glDispatchCompute(workUnits, 1, 1);
		CHECKGLERROR();

#if 0
		glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
		CHECKGLERROR();

		glBindTexture(GL_TEXTURE_2D, m_memoryTexture);
		std::vector<uint32> data;
		data.resize(MEMORY_TEXTURE_SIZE * MEMORY_TEXTURE_SIZE);
		glGetTexImage(GL_TEXTURE_2D, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, data.data());
		CHECKGLERROR();

		uint8* dataBlah = reinterpret_cast<uint8*>(data.data());
#endif

		///////
#if 0
		auto trxReg = make_convertible<TRXREG>(m_nReg[GS_REG_TRXREG]);
		auto trxPos = make_convertible<TRXPOS>(m_nReg[GS_REG_TRXPOS]);

		//Find the pages that are touched by this transfer
		auto transferPageSize = CGsPixelFormats::GetPsmPageSize(bltBuf.nDstPsm);

		uint32 pageCountX = (bltBuf.GetDstWidth() + transferPageSize.first - 1) / transferPageSize.first;
		uint32 pageCountY = (trxReg.nRRH + transferPageSize.second - 1) / transferPageSize.second;

		uint32 pageCount = pageCountX * pageCountY;
		uint32 transferSize = pageCount * CGsPixelFormats::PAGESIZE;
		uint32 transferOffset = (trxPos.nDSAY / transferPageSize.second) * pageCountX * CGsPixelFormats::PAGESIZE;

		m_textureCache.InvalidateRange(transferAddress + transferOffset, transferSize);

		bool isUpperByteTransfer = (bltBuf.nDstPsm == PSMT8H) || (bltBuf.nDstPsm == PSMT4HL) || (bltBuf.nDstPsm == PSMT4HH);
		for(const auto& framebuffer : m_framebuffers)
		{
			if((framebuffer->m_psm == PSMCT24) && isUpperByteTransfer) continue;
			framebuffer->m_cachedArea.Invalidate(transferAddress + transferOffset, transferSize);
		}
#endif
	}
}

void CGSH_OpenGL::ProcessLocalToHostTransfer()
{
	//This is constrained to work only with ps2autotest, will be unconstrained later

	auto bltBuf = make_convertible<BITBLTBUF>(m_nReg[GS_REG_BITBLTBUF]);
	auto trxPos = make_convertible<TRXPOS>(m_nReg[GS_REG_TRXPOS]);
	auto trxReg = make_convertible<TRXREG>(m_nReg[GS_REG_TRXREG]);

	if(bltBuf.nSrcPsm != PSMCT32) return;

	uint32 transferAddress = bltBuf.GetSrcPtr();
	if(transferAddress != 0) return;

	if((trxReg.nRRW != 32) || (trxReg.nRRH != 32)) return;
	if((trxPos.nSSAX != 0) || (trxPos.nSSAY != 0)) return;

	auto framebufferIterator = std::find_if(m_framebuffers.begin(), m_framebuffers.end(),
	                                        [](const FramebufferPtr& framebuffer) {
		                                        return (framebuffer->m_psm == PSMCT32) && (framebuffer->m_basePtr == 0);
	                                        });
	if(framebufferIterator == std::end(m_framebuffers)) return;
	const auto& framebuffer = (*framebufferIterator);

	FlushVertexBuffer();
	m_renderState.isValid = false;

	auto pixels = new uint32[trxReg.nRRW * trxReg.nRRH];

	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer->m_framebuffer);
	glReadPixels(trxPos.nSSAX, trxPos.nSSAY, trxReg.nRRW, trxReg.nRRH, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	CHECKGLERROR();

	//Write back to RAM
	{
		CGsPixelFormats::CPixelIndexorPSMCT32 indexor(m_pRAM, bltBuf.GetSrcPtr(), bltBuf.nSrcWidth);
		for(uint32 y = trxPos.nSSAY; y < (trxPos.nSSAY + trxReg.nRRH); y++)
		{
			for(uint32 x = trxPos.nSSAX; x < (trxPos.nSSAX + trxReg.nRRH); x++)
			{
				uint32 pixel = pixels[x + (y * trxReg.nRRW)];
				indexor.SetPixel(x, y, pixel);
			}
		}
	}

	delete[] pixels;
}

void CGSH_OpenGL::ProcessLocalToLocalTransfer()
{
	auto bltBuf = make_convertible<BITBLTBUF>(m_nReg[GS_REG_BITBLTBUF]);
	auto srcFramebufferIterator = std::find_if(m_framebuffers.begin(), m_framebuffers.end(),
	                                           [&](const FramebufferPtr& framebuffer) {
		                                           return (framebuffer->m_basePtr == bltBuf.GetSrcPtr()) &&
		                                                  (framebuffer->m_width == bltBuf.GetSrcWidth());
	                                           });
	auto dstFramebufferIterator = std::find_if(m_framebuffers.begin(), m_framebuffers.end(),
	                                           [&](const FramebufferPtr& framebuffer) {
		                                           return (framebuffer->m_basePtr == bltBuf.GetDstPtr()) &&
		                                                  (framebuffer->m_width == bltBuf.GetDstWidth());
	                                           });
	if(
	    srcFramebufferIterator != std::end(m_framebuffers) &&
	    dstFramebufferIterator != std::end(m_framebuffers))
	{
		FlushVertexBuffer();
		m_renderState.isValid = false;

		const auto& srcFramebuffer = (*srcFramebufferIterator);
		const auto& dstFramebuffer = (*dstFramebufferIterator);

		glBindFramebuffer(GL_FRAMEBUFFER, dstFramebuffer->m_framebuffer);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFramebuffer->m_framebuffer);

		//Copy buffers
		glBlitFramebuffer(
		    0, 0, srcFramebuffer->m_width * m_fbScale, srcFramebuffer->m_height * m_fbScale,
		    0, 0, srcFramebuffer->m_width * m_fbScale, srcFramebuffer->m_height * m_fbScale,
		    GL_COLOR_BUFFER_BIT, GL_NEAREST);
		CHECKGLERROR();

		glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	}
}

void CGSH_OpenGL::ProcessClutTransfer(uint32 clutPtr, uint32)
{
	FlushVertexBuffer();
	m_renderState.isTextureStateValid = false;

	CLUTLOADPARAMS clutLoadParams;
	clutLoadParams.clutBufPtr = clutPtr;

	glBindBuffer(GL_UNIFORM_BUFFER, m_clutLoadParamsBuffer);
	glBufferData(GL_UNIFORM_BUFFER, sizeof(CLUTLOADPARAMS), &clutLoadParams, GL_STREAM_DRAW);
	CHECKGLERROR();

	auto program = m_clutLoaderProgramIDX8_CSM0_PSMCT32;

	//Setup compute dispatch
	glUseProgram(*program);
	m_validGlState &= ~GLSTATE_PROGRAM;

	glBindBufferBase(GL_UNIFORM_BUFFER, 0, m_clutLoadParamsBuffer);
	CHECKGLERROR();

	glBindImageTexture(SHADER_IMAGE_MEMORY, m_memoryTexture, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32UI);
	CHECKGLERROR();

	glBindImageTexture(SHADER_IMAGE_CLUT, m_clutTexture, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32UI);
	CHECKGLERROR();

	glBindImageTexture(SHADER_IMAGE_CLUT_SWIZZLE, m_swizzleTexturePSMCT32, 0, GL_FALSE, 0, GL_READ_ONLY, GL_R32UI);
	CHECKGLERROR();

	m_validGlState &= ~GLSTATE_TEXTURE;

#ifdef _DEBUG
	program->Validate();
#endif

	glDispatchCompute(1, 1, 1);
	CHECKGLERROR();

#if 0
	glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);

	std::vector<uint32> data;
	data.resize(CLUTENTRYCOUNT);

	{
		glBindTexture(GL_TEXTURE_2D, m_clutTexture);
		glGetTexImage(GL_TEXTURE_2D, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, data.data());
		CHECKGLERROR();
		glBindTexture(GL_TEXTURE_2D, 0);
	}
#endif

#if 0
	PalCache_Invalidate(csa);
#endif
}

void CGSH_OpenGL::ReadFramebuffer(uint32 width, uint32 height, void* buffer)
{
	//TODO: Implement this in a better way. This is only used for movie recording on Win32 for now.
#ifdef GLES_COMPATIBILITY
	assert(false);
#else
	glFlush();
	glFinish();
	glReadPixels(0, 0, width, height, GL_BGR, GL_UNSIGNED_BYTE, buffer);
#endif
}

Framework::CBitmap CGSH_OpenGL::GetScreenshot()
{
	GLint viewport[4];
	glGetIntegerv(GL_VIEWPORT, viewport);
	auto imgbuffer = Framework::CBitmap(viewport[2], viewport[3], 32);
	glReadPixels(viewport[0], viewport[1], viewport[2], viewport[3], GL_RGBA, GL_UNSIGNED_BYTE, imgbuffer.GetPixels());
	return imgbuffer.FlipVertical();
}

/////////////////////////////////////////////////////////////
// Framebuffer
/////////////////////////////////////////////////////////////

CGSH_OpenGL::CFramebuffer::CFramebuffer(uint32 basePtr, uint32 width, uint32 height, uint32 psm, uint32 scale, bool multisampled)
    : m_basePtr(basePtr)
    , m_width(width)
    , m_height(height)
    , m_psm(psm)
{
	m_cachedArea.SetArea(psm, basePtr, width, height);

	//Build color attachment
	glGenTextures(1, &m_texture);
	glBindTexture(GL_TEXTURE_2D, m_texture);
	glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, m_width * scale, m_height * scale);
	CHECKGLERROR();

	if(multisampled)
	{
		//We also need an attachment for multisampled color
		glGenRenderbuffers(1, &m_colorBufferMs);
		glBindRenderbuffer(GL_RENDERBUFFER, m_colorBufferMs);
		glRenderbufferStorageMultisample(GL_RENDERBUFFER, NUM_SAMPLES, GL_RGBA8, m_width * scale, m_height * scale);
		CHECKGLERROR();
	}

	//Build framebuffer
	glGenFramebuffers(1, &m_framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer);
	if(multisampled)
	{
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, m_colorBufferMs);
	}
	else
	{
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_texture, 0);
	}
	CHECKGLERROR();

	if(multisampled)
	{
		glGenFramebuffers(1, &m_resolveFramebuffer);
		glBindFramebuffer(GL_FRAMEBUFFER, m_resolveFramebuffer);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_texture, 0);
		CHECKGLERROR();

		auto status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
		assert(status == GL_FRAMEBUFFER_COMPLETE);
	}

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

CGSH_OpenGL::CFramebuffer::~CFramebuffer()
{
	if(m_framebuffer != 0)
	{
		glDeleteFramebuffers(1, &m_framebuffer);
	}
	if(m_resolveFramebuffer != 0)
	{
		glDeleteFramebuffers(1, &m_resolveFramebuffer);
	}
	if(m_texture != 0)
	{
		glDeleteTextures(1, &m_texture);
	}
	if(m_colorBufferMs != 0)
	{
		glDeleteRenderbuffers(1, &m_colorBufferMs);
	}
}

void CGSH_OpenGL::PopulateFramebuffer(const FramebufferPtr& framebuffer)
{
	return;

	auto texFormat = GetTextureFormatInfo(framebuffer->m_psm);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, m_copyToFbTexture);
	glTexImage2D(GL_TEXTURE_2D, 0, texFormat.internalFormat, framebuffer->m_width, framebuffer->m_height,
	             0, texFormat.format, texFormat.type, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	((this)->*(m_textureUpdater[framebuffer->m_psm]))(framebuffer->m_basePtr,
	                                                  framebuffer->m_width / 64, 0, 0, framebuffer->m_width, framebuffer->m_height);
	CHECKGLERROR();

	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer->m_framebuffer);

	CopyToFb(
	    0, 0, framebuffer->m_width, framebuffer->m_height,
	    framebuffer->m_width, framebuffer->m_height,
	    0, 0, framebuffer->m_width * m_fbScale, framebuffer->m_height * m_fbScale);
	framebuffer->m_resolveNeeded = true;

	CHECKGLERROR();
}

void CGSH_OpenGL::CommitFramebufferDirtyPages(const FramebufferPtr& framebuffer, unsigned int minY, unsigned int maxY)
{
	return;

	class CCopyToFbEnabler
	{
	public:
		void EnableCopyToFb(const FramebufferPtr& framebuffer, GLuint copyToFbTexture)
		{
			if(m_copyToFbEnabled) return;

			glDisable(GL_SCISSOR_TEST);

			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, copyToFbTexture);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, framebuffer->m_width, framebuffer->m_height,
			             0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

			glBindFramebuffer(GL_FRAMEBUFFER, framebuffer->m_framebuffer);

			m_copyToFbEnabled = true;
		}

	private:
		bool m_copyToFbEnabled = false;
	};

	auto& cachedArea = framebuffer->m_cachedArea;

	auto areaRect = cachedArea.GetAreaPageRect();
	auto texturePageSize = CGsPixelFormats::GetPsmPageSize(framebuffer->m_psm);

	CCopyToFbEnabler copyToFbEnabler;
	while(cachedArea.HasDirtyPages())
	{
		auto dirtyRect = cachedArea.GetDirtyPageRect();
		assert((dirtyRect.width != 0) && (dirtyRect.height != 0));
		cachedArea.ClearDirtyPages(dirtyRect);

		uint32 texX = dirtyRect.x * texturePageSize.first;
		uint32 texY = dirtyRect.y * texturePageSize.second;
		uint32 texWidth = dirtyRect.width * texturePageSize.first;
		uint32 texHeight = dirtyRect.height * texturePageSize.second;

		if(texY >= maxY)
		{
			//Don't bother
			continue;
		}

		assert(texX < framebuffer->m_width);
		assert(texY < framebuffer->m_height);
		//assert(texY >= minY);
		//assert(texY < maxY);
		if((texX + texWidth) > framebuffer->m_width)
		{
			texWidth = framebuffer->m_width - texX;
		}
		if((texY + texHeight) > framebuffer->m_height)
		{
			texHeight = framebuffer->m_height - texY;
		}

		m_validGlState &= ~(GLSTATE_SCISSOR | GLSTATE_FRAMEBUFFER | GLSTATE_TEXTURE);
		copyToFbEnabler.EnableCopyToFb(framebuffer, m_copyToFbTexture);

		((this)->*(m_textureUpdater[framebuffer->m_psm]))(framebuffer->m_basePtr, framebuffer->m_width / 64,
		                                                  texX, texY, texWidth, texHeight);

		CopyToFb(
		    texX, texY, (texX + texWidth), (texY + texHeight),
		    framebuffer->m_width, framebuffer->m_height,
		    texX * m_fbScale, texY * m_fbScale, (texX + texWidth) * m_fbScale, (texY + texHeight) * m_fbScale);
		framebuffer->m_resolveNeeded = true;

		CHECKGLERROR();
	}

	//Mark all pages as clean, but might be wrong due to range not
	//covering an area that might be used later on
	cachedArea.ClearDirtyPages();
}

void CGSH_OpenGL::ResolveFramebufferMultisample(const FramebufferPtr& framebuffer, uint32 scale)
{
	if(!framebuffer->m_resolveNeeded) return;

	m_validGlState &= ~(GLSTATE_SCISSOR | GLSTATE_FRAMEBUFFER);

	glDisable(GL_SCISSOR_TEST);
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer->m_resolveFramebuffer);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer->m_framebuffer);
	glBlitFramebuffer(
	    0, 0, framebuffer->m_width * scale, framebuffer->m_height * scale,
	    0, 0, framebuffer->m_width * scale, framebuffer->m_height * scale,
	    GL_COLOR_BUFFER_BIT, GL_NEAREST);
	CHECKGLERROR();

	framebuffer->m_resolveNeeded = false;
}

/////////////////////////////////////////////////////////////
// Depthbuffer
/////////////////////////////////////////////////////////////

CGSH_OpenGL::CDepthbuffer::CDepthbuffer(uint32 basePtr, uint32 width, uint32 height, uint32 psm, uint32 scale, bool multisampled)
    : m_basePtr(basePtr)
    , m_width(width)
    , m_height(height)
    , m_psm(psm)
    , m_depthBufferImage(0)
{
	glGenTextures(1, &m_depthBufferImage);
	glBindTexture(GL_TEXTURE_2D, m_depthBufferImage);
	glTexStorage2D(GL_TEXTURE_2D, 1, GL_R32UI, m_width * scale, m_height * scale);

	CHECKGLERROR();
}

CGSH_OpenGL::CDepthbuffer::~CDepthbuffer()
{
	if(m_depthBufferImage != 0)
	{
		glDeleteTextures(1, &m_depthBufferImage);
	}
}
