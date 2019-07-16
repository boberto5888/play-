#pragma once

#include <list>
#include <unordered_map>
#include "../GSHandler.h"
#include "../GsCachedArea.h"
#include "../GsTextureCache.h"
#include "opengl/OpenGlDef.h"
#include "opengl/Program.h"
#include "opengl/Shader.h"
#include "opengl/Resource.h"

#define PREF_CGSH_OPENGL_RESOLUTION_FACTOR "renderer.opengl.resfactor"
#define PREF_CGSH_OPENGL_FORCEBILINEARTEXTURES "renderer.opengl.forcebilineartextures"

class CGSH_OpenGL : public CGSHandler
{
public:
	CGSH_OpenGL();
	virtual ~CGSH_OpenGL();

	static void RegisterPreferences();

	virtual void LoadState(Framework::CZipArchiveReader&) override;

	void ProcessHostToLocalTransfer() override;
	void ProcessLocalToHostTransfer() override;
	void ProcessLocalToLocalTransfer() override;
	void ProcessClutTransfer(uint32, uint32) override;
	void ReadFramebuffer(uint32, uint32, void*) override;

	Framework::CBitmap GetScreenshot() override;

protected:
	void PalCache_Flush();
	void LoadPreferences();
	void InitializeImpl() override;
	void ReleaseImpl() override;
	void ResetImpl() override;
	void NotifyPreferencesChangedImpl() override;
	void FlipImpl() override;

	GLuint m_presentFramebuffer = 0;

private:
	typedef CGsTextureCache<Framework::OpenGl::CTexture> TextureCache;

	enum SHADER_IMAGE_LOCATIONS
	{
		SHADER_IMAGE_MEMORY,
		SHADER_IMAGE_CLUT,
		SHADER_IMAGE_TEXTURE_SWIZZLE = 2,
		SHADER_IMAGE_XFER_SWIZZLE = 2,
		SHADER_IMAGE_CLUT_SWIZZLE = 2,
		SHADER_IMAGE_FRAME_SWIZZLE,
		SHADER_IMAGE_DEPTH_SWIZZLE,
	};

	struct SHADERCAPS : public convertible<uint64>
	{
		unsigned int texFunction : 2; //0 - Modulate, 1 - Decal, 2 - Highlight, 3 - Hightlight2
		unsigned int texClampS : 2;
		unsigned int texClampT : 2;
		unsigned int texSourceMode : 2;
		unsigned int texHasAlpha : 1;
		unsigned int texBilinearFilter : 1;
		unsigned int texUseAlphaExpansion : 1;
		unsigned int texBlackIsTransparent : 1;
		unsigned int hasFog : 1;
		unsigned int hasAlphaTest : 1;
		unsigned int alphaTestMethod : 3;
		unsigned int alphaFailResult : 2;
		unsigned int depthWriteEnabled : 1;
		unsigned int depthTestMethod : 2;
		unsigned int hasAlphaBlend : 1;
		unsigned int blendFactorA : 2;
		unsigned int blendFactorB : 2;
		unsigned int blendFactorC : 2;
		unsigned int blendFactorD : 2;
		unsigned int padding1 : 1;
		unsigned int texPsm : 6;
		unsigned int texCpsm : 6;
		unsigned int framePsm : 6;
		unsigned int depthPsm : 6;
		unsigned int padding2 : 8;

		bool isIndexedTextureSource() const
		{
			return texSourceMode == TEXTURE_SOURCE_MODE_IDX4 || texSourceMode == TEXTURE_SOURCE_MODE_IDX8;
		}
	};
	static_assert(sizeof(SHADERCAPS) == sizeof(uint64), "SHADERCAPS structure size must be 8 bytes.");

	struct RENDERSTATE
	{
		bool isValid;
		bool isTextureStateValid;
		bool isFramebufferStateValid;

		//Register State
		uint64 primReg;
		uint64 frameReg;
		uint64 testReg;
		uint64 alphaReg;
		uint64 zbufReg;
		uint64 scissorReg;
		uint64 tex0Reg;
		uint64 tex1Reg;
		uint64 texAReg;
		uint64 clampReg;
		uint64 fogColReg;

		//Intermediate State
		SHADERCAPS shaderCaps;

		//OpenGL state
		GLuint shaderHandle;
		GLuint framebufferHandle;
		GLuint framebufferTextureHandle;
		GLuint depthbufferTextureHandle;
		GLuint textureSwizzleTableHandle;
		GLuint frameSwizzleTableHandle;
		GLuint depthSwizzleTableHandle;
		GLuint texture0Handle;
		GLint texture0MinFilter;
		GLint texture0MagFilter;
		GLint texture0WrapS;
		GLint texture0WrapT;
		bool texture0AlphaAsIndex;
		GLuint texture1Handle;
		GLsizei viewportWidth;
		GLsizei viewportHeight;
		GLint scissorX;
		GLint scissorY;
		GLsizei scissorWidth;
		GLsizei scissorHeight;
		bool blendEnabled;
		bool colorMaskR;
		bool colorMaskG;
		bool colorMaskB;
		bool colorMaskA;
		bool depthMask;
		bool depthTest;
	};

	//These need to match the layout of the shader's uniform block
	struct VERTEXPARAMS
	{
		float projMatrix[16];
		float texMatrix[16];
	};
	static_assert(sizeof(VERTEXPARAMS) == 0x80, "Size of VERTEXPARAMS must be 128 bytes.");

	struct FRAGMENTPARAMS
	{
		//
		float textureSize[2];
		float texelSize[2];
		float clampMin[2];
		float clampMax[2];
		//
		float texA0;
		float texA1;
		uint32 alphaRef;
		uint32 depthMask;
		//
		float fogColor[3];
		uint32 alphaFix;
		//
		uint32 colorMask;
		uint32 textureBufPtr;
		uint32 textureBufWidth;
		uint32 textureCsa;
		//
		uint32 frameBufPtr;
		uint32 frameBufWidth;
		uint32 depthBufPtr;
		uint32 depthBufWidth;
	};
	static_assert(sizeof(FRAGMENTPARAMS) == 0x60, "Size of FRAGMENTPARAMS must be 96 bytes.");

	struct XFERPARAMS
	{
		uint32 pixelCount;
		uint32 bufAddress;
		uint32 bufWidth;
		uint32 rrw;
		uint32 dsax;
		uint32 dsay;
		uint32 padding[2];
	};
	static_assert(sizeof(XFERPARAMS) == 0x20, "Size of XFERPARAMS must be 32 bytes.");

	struct CLUTLOADPARAMS
	{
		uint32 clutBufPtr;
		uint32 csa;
		uint32 padding[2];
	};
	static_assert(sizeof(CLUTLOADPARAMS) == 0x10, "Size of CLUTLOADPARAMS must be 16 bytes.");

	enum
	{
		MAX_TEXTURE_CACHE = 256,
		MAX_PALETTE_CACHE = 256,
	};

	enum CVTBUFFERSIZE
	{
		CVTBUFFERSIZE = 0x800000,
	};

	typedef void (CGSH_OpenGL::*TEXTUREUPDATER)(uint32, uint32, unsigned int, unsigned int, unsigned int, unsigned int);

	struct VERTEX
	{
		uint64 nPosition;
		uint64 nRGBAQ;
		uint64 nUV;
		uint64 nST;
		uint8 nFog;
	};

	enum
	{
		TEXTURE_SOURCE_MODE_NONE = 0,
		TEXTURE_SOURCE_MODE_STD = 1,
		TEXTURE_SOURCE_MODE_IDX4 = 2,
		TEXTURE_SOURCE_MODE_IDX8 = 3
	};

	enum TEXTURE_CLAMP_MODE
	{
		TEXTURE_CLAMP_MODE_STD = 0,
		TEXTURE_CLAMP_MODE_REGION_CLAMP = 1,
		TEXTURE_CLAMP_MODE_REGION_REPEAT = 2,
		TEXTURE_CLAMP_MODE_REGION_REPEAT_SIMPLE = 3
	};

	typedef std::unordered_map<uint64, Framework::OpenGl::ProgramPtr> ShaderMap;

	class CPalette
	{
	public:
		CPalette();
		~CPalette();

		void Invalidate(uint32);
		void Free();

		bool m_live;

		bool m_isIDTEX4;
		uint32 m_cpsm;
		uint32 m_csa;
		GLuint m_texture;
		uint32 m_contents[256];
	};
	typedef std::shared_ptr<CPalette> PalettePtr;
	typedef std::list<PalettePtr> PaletteList;

	class CFramebuffer
	{
	public:
		CFramebuffer(uint32, uint32, uint32, uint32, uint32, bool);
		~CFramebuffer();

		uint32 m_basePtr;
		uint32 m_width;
		uint32 m_height;
		uint32 m_psm;

		GLuint m_framebuffer = 0;
		GLuint m_texture = 0;

		GLuint m_resolveFramebuffer = 0;
		bool m_resolveNeeded = false;
		GLuint m_colorBufferMs = 0;

		CGsCachedArea m_cachedArea;
	};
	typedef std::shared_ptr<CFramebuffer> FramebufferPtr;
	typedef std::vector<FramebufferPtr> FramebufferList;

	class CDepthbuffer
	{
	public:
		CDepthbuffer(uint32, uint32, uint32, uint32, uint32, bool);
		~CDepthbuffer();

		uint32 m_basePtr;
		uint32 m_width;
		uint32 m_height;
		uint32 m_psm;
		GLuint m_depthBufferImage;
	};
	typedef std::shared_ptr<CDepthbuffer> DepthbufferPtr;
	typedef std::vector<DepthbufferPtr> DepthbufferList;

	struct TEXTURE_INFO
	{
		GLuint textureHandle = 0;
		float offsetX = 0;
		float scaleRatioX = 1;
		float scaleRatioY = 1;
		bool alphaAsIndex = false;
	};

	struct TEXTUREFORMAT_INFO
	{
		GLenum internalFormat;
		GLenum format;
		GLenum type;
	};

	enum class PRIM_VERTEX_ATTRIB
	{
		POSITION = 1,
		DEPTH,
		COLOR,
		TEXCOORD,
		FOG,
	};

	struct PRIM_VERTEX
	{
		float x, y;
		uint32 z;
		uint32 color;
		float s, t, q;
		float f;
	};

	enum VERTEX_BUFFER_SIZE
	{
		VERTEX_BUFFER_SIZE = 0x1000,
	};

	typedef std::vector<PRIM_VERTEX> VertexBuffer;

	void WriteRegisterImpl(uint8, uint64) override;

	void InitializeRC();
	void SetupTextureUpdaters();
	virtual void PresentBackbuffer() = 0;
	void MakeLinearZOrtho(float*, float, float, float, float);
	unsigned int GetCurrentReadCircuit();
	TEXTURE_INFO PrepareTexture(const TEX0&);
	GLuint PreparePalette(const TEX0&);

	void VertexKick(uint8, uint64);

	Framework::OpenGl::ProgramPtr GetShaderFromCaps(const SHADERCAPS&);
	Framework::OpenGl::ProgramPtr GenerateShader(const SHADERCAPS&);
	Framework::OpenGl::CShader GenerateVertexShader(const SHADERCAPS&);
	Framework::OpenGl::CShader GenerateFragmentShader(const SHADERCAPS&);
	std::string GenerateTexCoordClampingSection(TEXTURE_CLAMP_MODE, const char*);
	std::string GenerateAlphaBlendABDValue(ALPHABLEND_ABD);
	std::string GenerateAlphaBlendCValue(ALPHABLEND_C);
	std::string GenerateAlphaTestSection(ALPHA_TEST_METHOD);
	std::string GenerateXferProgramBase();
	std::string GenerateMemoryAccessSection();

	Framework::OpenGl::ProgramPtr GeneratePresentProgram(uint32);
	Framework::OpenGl::CBuffer GeneratePresentVertexBuffer();
	Framework::OpenGl::CVertexArray GeneratePresentVertexArray();

	Framework::OpenGl::ProgramPtr GenerateCopyToFbProgram();
	Framework::OpenGl::CBuffer GenerateCopyToFbVertexBuffer();
	Framework::OpenGl::CVertexArray GenerateCopyToFbVertexArray();

	Framework::OpenGl::ProgramPtr GenerateXferProgramPSMCT32();
	Framework::OpenGl::ProgramPtr GenerateXferProgramPSMCT16();
	Framework::OpenGl::ProgramPtr GenerateXferProgramPSMT8();
	Framework::OpenGl::ProgramPtr GenerateXferProgramPSMT4();
	Framework::OpenGl::ProgramPtr GenerateXferProgramPSMT8H();
	Framework::OpenGl::ProgramPtr GenerateXferProgramPSMT4HL();
	Framework::OpenGl::ProgramPtr GenerateXferProgramPSMT4HH();

	Framework::OpenGl::ProgramPtr GenerateClutLoaderProgram(uint8);

	Framework::OpenGl::CVertexArray GeneratePrimVertexArray();
	Framework::OpenGl::CBuffer GenerateUniformBlockBuffer(size_t);

	void Prim_Point();
	void Prim_Line();
	void Prim_Triangle();
	void Prim_Sprite();

	void FlushVertexBuffer();
	void DoRenderPass();

	void CopyToFb(int32, int32, int32, int32, int32, int32, int32, int32, int32, int32);
	void DrawToDepth(unsigned int, uint64);

	void SetRenderingContext(uint64);
	void SetupTestFunctions(uint64);
	void SetupDepthBuffer(uint64, uint64);
	void SetupFramebuffer(uint64, uint64, uint64, uint64);
	void SetupBlendingFunction(uint64);
	void SetupFogColor(uint64);

	static bool CanRegionRepeatClampModeSimplified(uint32, uint32);
	void FillShaderCapsFromTexture(SHADERCAPS&, const uint64&, const uint64&, const uint64&, const uint64&);
	void FillShaderCapsFromFrame(SHADERCAPS&, const uint64&);
	void FillShaderCapsFromTestAndZbuf(SHADERCAPS&, const uint64&, const uint64&);
	void FillShaderCapsFromAlpha(SHADERCAPS&, const uint64&);

	void SetupTexture(uint64, uint64, uint64, uint64, uint64);
	static uint32 GetFramebufferBitDepth(uint32);
	static TEXTUREFORMAT_INFO GetTextureFormatInfo(uint32);

	FramebufferPtr FindFramebuffer(const FRAME&) const;
	DepthbufferPtr FindDepthbuffer(const ZBUF&, const FRAME&) const;
	
	GLuint GetSwizzleTable(uint32) const;

	void DumpTexture(unsigned int, unsigned int, uint32);

	//Texture updaters
	void TexUpdater_Invalid(uint32, uint32, unsigned int, unsigned int, unsigned int, unsigned int);

	void TexUpdater_Psm32(uint32, uint32, unsigned int, unsigned int, unsigned int, unsigned int);
	template <typename>
	void TexUpdater_Psm16(uint32, uint32, unsigned int, unsigned int, unsigned int, unsigned int);

	template <typename>
	void TexUpdater_Psm48(uint32, uint32, unsigned int, unsigned int, unsigned int, unsigned int);
	template <uint32, uint32>
	void TexUpdater_Psm48H(uint32, uint32, unsigned int, unsigned int, unsigned int, unsigned int);

	//Context variables (put this in a struct or something?)
	float m_nPrimOfsX;
	float m_nPrimOfsY;
	uint32 m_nTexWidth;
	uint32 m_nTexHeight;

	bool m_forceBilinearTextures = false;
	unsigned int m_fbScale = 1;
	bool m_multisampleEnabled = false;
	bool m_accurateAlphaTestEnabled = false;

	uint8* m_pCvtBuffer;

	GLuint PalCache_Search(const TEX0&);
	GLuint PalCache_Search(unsigned int, const uint32*);
	void PalCache_Insert(const TEX0&, const uint32*, GLuint);
	void PalCache_Invalidate(uint32);

	void PopulateFramebuffer(const FramebufferPtr&);
	void CommitFramebufferDirtyPages(const FramebufferPtr&, unsigned int, unsigned int);
	void ResolveFramebufferMultisample(const FramebufferPtr&, uint32);

	Framework::OpenGl::ProgramPtr m_presentProgramPSMCT32;
	Framework::OpenGl::ProgramPtr m_presentProgramPSMCT16;
	Framework::OpenGl::CBuffer m_presentVertexBuffer;
	Framework::OpenGl::CVertexArray m_presentVertexArray;
	GLint m_presentTextureUniform = -1;
	GLint m_presentTexCoordScaleUniform = -1;
	GLint m_presentFrameBufPtr = -1;
	GLint m_presentFrameBufWidth = -1;
	GLint m_presentScreenSize = -1;

	Framework::OpenGl::ProgramPtr m_copyToFbProgram;
	Framework::OpenGl::CTexture m_copyToFbTexture;
	Framework::OpenGl::CBuffer m_copyToFbVertexBuffer;
	Framework::OpenGl::CVertexArray m_copyToFbVertexArray;
	GLint m_copyToFbSrcPositionUniform = -1;
	GLint m_copyToFbSrcSizeUniform = -1;

	TextureCache m_textureCache;
	PaletteList m_paletteCache;
	FramebufferList m_framebuffers;
	DepthbufferList m_depthbuffers;

	Framework::OpenGl::CBuffer m_primBuffer;
	Framework::OpenGl::CVertexArray m_primVertexArray;

	Framework::OpenGl::ProgramPtr m_xferProgramPSMCT32;
	Framework::OpenGl::ProgramPtr m_xferProgramPSMCT16;
	Framework::OpenGl::ProgramPtr m_xferProgramPSMT8;
	Framework::OpenGl::ProgramPtr m_xferProgramPSMT4;
	Framework::OpenGl::ProgramPtr m_xferProgramPSMT8H;
	Framework::OpenGl::ProgramPtr m_xferProgramPSMT4HL;
	Framework::OpenGl::ProgramPtr m_xferProgramPSMT4HH;
	static const uint32 g_xferWorkGroupSize;
	Framework::OpenGl::CBuffer m_xferParamsBuffer;
	Framework::OpenGl::CBuffer m_xferBuffer;

	Framework::OpenGl::CTexture m_memoryTexture;

	Framework::OpenGl::ProgramPtr m_clutLoaderProgramIDX8_CSM0_PSMCT32;
	Framework::OpenGl::ProgramPtr m_clutLoaderProgramIDX4_CSM0_PSMCT32;
	Framework::OpenGl::CBuffer m_clutLoadParamsBuffer;
	Framework::OpenGl::CTexture m_clutTexture;

	Framework::OpenGl::CTexture m_swizzleTexturePSMCT32;
	Framework::OpenGl::CTexture m_swizzleTexturePSMCT16;
	Framework::OpenGl::CTexture m_swizzleTexturePSMCT16S;
	Framework::OpenGl::CTexture m_swizzleTexturePSMT8;
	Framework::OpenGl::CTexture m_swizzleTexturePSMT4;

	VERTEX m_VtxBuffer[3];
	int m_nVtxCount;

	PRMODE m_PrimitiveMode;
	unsigned int m_primitiveType;
	bool m_drawingToDepth = false;

	static const GLenum g_nativeClampModes[CGSHandler::CLAMP_MODE_MAX];
	static const unsigned int g_shaderClampModes[CGSHandler::CLAMP_MODE_MAX];

	TEXTUREUPDATER m_textureUpdater[CGSHandler::PSM_MAX];

	enum GLSTATE_BITS : uint32
	{
		GLSTATE_VERTEX_PARAMS = 0x0001,
		GLSTATE_FRAGMENT_PARAMS = 0x0002,
		GLSTATE_PROGRAM = 0x0004,
		GLSTATE_SCISSOR = 0x0008,
		GLSTATE_BLEND = 0x0010,
		GLSTATE_COLORMASK = 0x0020,
		GLSTATE_DEPTHMASK = 0x0040,
		GLSTATE_TEXTURE = 0x0080,
		GLSTATE_FRAMEBUFFER = 0x0100,
		GLSTATE_VIEWPORT = 0x0200,
		GLSTATE_DEPTHTEST = 0x0400,
	};

	ShaderMap m_shaders;
	RENDERSTATE m_renderState;
	uint32 m_validGlState = 0;
	VERTEXPARAMS m_vertexParams;
	FRAGMENTPARAMS m_fragmentParams;
	Framework::OpenGl::CBuffer m_vertexParamsBuffer;
	Framework::OpenGl::CBuffer m_fragmentParamsBuffer;
	VertexBuffer m_vertexBuffer;
};
