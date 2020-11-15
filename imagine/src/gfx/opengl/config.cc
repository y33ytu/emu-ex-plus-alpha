/*  This file is part of Imagine.

	Imagine is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	Imagine is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with Imagine.  If not, see <http://www.gnu.org/licenses/> */

#define LOGTAG "GLRenderer"
#include <assert.h>
#include <imagine/gfx/Renderer.hh>
#include <imagine/base/Base.hh>
#include <imagine/base/Window.hh>
#include <imagine/util/string.h>
#include <imagine/fs/FS.hh>
#include "private.hh"
#include "utils.h"
#ifdef __ANDROID__
#include "../../base/android/android.hh"
#endif
#include <string>

#if defined CONFIG_BASE_GLAPI_EGL && defined CONFIG_GFX_OPENGL_ES
#define CAN_USE_EGL_SYNC
	#if __ANDROID_API__ < 18 || defined CONFIG_MACHINE_PANDORA
	#define EGL_SYNC_NEEDS_PROC_ADDR
	#endif
#endif

#ifdef CAN_USE_EGL_SYNC
#include <EGL/eglext.h>
	#ifdef CONFIG_MACHINE_PANDORA
	using EGLSyncKHR = void*;
	using EGLTimeKHR = uint64_t;
	#endif
#ifndef EGL_TIMEOUT_EXPIRED
#define EGL_TIMEOUT_EXPIRED 0x30F5
#endif
#ifndef EGL_CONDITION_SATISFIED
#define EGL_CONDITION_SATISFIED 0x30F6
#endif
#ifndef EGL_SYNC_FENCE
#define EGL_SYNC_FENCE 0x30F9
#endif
#ifndef EGL_FOREVER
#define EGL_FOREVER 0xFFFFFFFFFFFFFFFFull
#endif
#define EGLSync EGLSyncKHR
#define EGLTime EGLTimeKHR
	#ifdef EGL_SYNC_NEEDS_PROC_ADDR
	static EGLSync (EGLAPIENTRY *eglCreateSyncFunc)(EGLDisplay dpy, EGLenum type, const EGLint *attrib_list){};
	static EGLBoolean (EGLAPIENTRY *eglDestroySyncFunc)(EGLDisplay dpy, EGLSync sync){};
	static EGLint (EGLAPIENTRY *eglClientWaitSyncFunc)(EGLDisplay dpy, EGLSync sync, EGLint flags, EGLTime timeout){};
	static EGLint (EGLAPIENTRY *eglWaitSyncFunc)(EGLDisplay dpy, EGLSync sync, EGLint flags){};
	#else
	extern "C" {
	EGLAPI EGLSyncKHR EGLAPIENTRY eglCreateSyncKHR (EGLDisplay dpy, EGLenum type, const EGLint *attrib_list);
	EGLAPI EGLBoolean EGLAPIENTRY eglDestroySyncKHR (EGLDisplay dpy, EGLSyncKHR sync);
	EGLAPI EGLint EGLAPIENTRY eglClientWaitSyncKHR (EGLDisplay dpy, EGLSyncKHR sync, EGLint flags, EGLTimeKHR timeout);
	EGLAPI EGLint EGLAPIENTRY eglWaitSyncKHR (EGLDisplay dpy, EGLSyncKHR sync, EGLint flags);
	}
	#define eglCreateSyncFunc eglCreateSyncKHR
	#define eglDestroySyncFunc eglDestroySyncKHR
	#define eglClientWaitSyncFunc eglClientWaitSyncKHR
	#define eglWaitSyncFunc eglWaitSyncKHR
	#endif
#endif

#ifndef GL_TIMEOUT_EXPIRED
#define GL_TIMEOUT_EXPIRED 0x911B
#endif

#ifndef GL_CONDITION_SATISFIED
#define GL_CONDITION_SATISFIED 0x911C
#endif

#ifndef GL_WAIT_FAILED
#define GL_WAIT_FAILED 0x911D
#endif

namespace Gfx
{

static constexpr bool CAN_USE_OPENGL_ES_3 = !Config::MACHINE_IS_PANDORA;

Gfx::GC orientationToGC(Base::Orientation o)
{
	using namespace Base;
	switch(o)
	{
		case VIEW_ROTATE_0: return Gfx::angleFromDegree(0.);
		case VIEW_ROTATE_90: return Gfx::angleFromDegree(-90.);
		case VIEW_ROTATE_180: return Gfx::angleFromDegree(-180.);
		case VIEW_ROTATE_270: return Gfx::angleFromDegree(90.);
		default: bug_unreachable("o == %d", o); return 0.;
	}
}

static void printFeatures(DrawContextSupport support)
{
	if(!Config::DEBUG_BUILD)
		return;
	std::string featuresStr{};
	featuresStr.reserve(256);

	featuresStr.append(" [Texture Size:");
	featuresStr.append(string_makePrintf<8>("%u", support.textureSizeSupport.maxXSize).data());
	featuresStr.append("]");
	if(support.textureSizeSupport.nonPow2)
	{
		featuresStr.append(" [NPOT Textures");
		if(support.textureSizeSupport.nonPow2CanRepeat)
			featuresStr.append(" w/ Mipmap+Repeat]");
		else if(support.textureSizeSupport.nonPow2CanMipmap)
			featuresStr.append(" w/ Mipmap]");
		else
			featuresStr.append("]");
	}
	#ifdef CONFIG_GFX_OPENGL_ES
	if(support.hasBGRPixels)
	{
		if(support.bgrInternalFormat == GL_RGBA)
			featuresStr.append(" [BGR Formats (Apple)]");
		else
			featuresStr.append(" [BGR Formats]");
	}
	#endif
	if(support.hasTextureSwizzle)
	{
		featuresStr.append(" [Texture Swizzle]");
	}
	if(support.hasImmutableTexStorage)
	{
		featuresStr.append(" [Immutable Texture Storage]");
	}
	if(support.hasImmutableBufferStorage())
	{
		featuresStr.append(" [Immutable Buffer Storage]");
	}
	if(Config::Gfx::OPENGL_ES_MAJOR_VERSION >= 2 && support.hasUnpackRowLength)
	{
		featuresStr.append(" [Unpack Sub-Images]");
	}
	if(support.hasSamplerObjects)
	{
		featuresStr.append(" [Sampler Objects]");
	}
	if(support.hasPBOFuncs)
	{
		featuresStr.append(" [PBOs]");
	}
	if(support.glMapBufferRange)
	{
		featuresStr.append(" [Map Buffer Range]");
	}
	if(support.hasSyncFences())
	{
		featuresStr.append(" [Sync Fences]");
	}
	#ifndef CONFIG_GFX_OPENGL_ES
	if(support.maximumAnisotropy)
	{
		featuresStr.append(" [Max Anisotropy:");
		featuresStr.append(string_makePrintf<8>("%.1f", support.maximumAnisotropy).data());
		featuresStr.append("]");
	}
	#endif
	#ifdef CONFIG_GFX_OPENGL_SHADER_PIPELINE
	if(!support.useFixedFunctionPipeline)
	{
		featuresStr.append(" [GLSL:");
		featuresStr.append((const char*)glGetString(GL_SHADING_LANGUAGE_VERSION));
		featuresStr.append("]");
	}
	#endif

	logMsg("features:%s", featuresStr.c_str());
}

#ifdef __ANDROID__
EGLImageKHR makeAndroidNativeBufferEGLImage(EGLDisplay dpy, EGLClientBuffer clientBuff)
{
	const EGLint eglImgAttrs[]
	{
		EGL_IMAGE_PRESERVED_KHR, EGL_TRUE,
		EGL_NONE, EGL_NONE
	};
	return eglCreateImageKHR(dpy, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
		clientBuff, eglImgAttrs);
}
#endif

void GLRenderer::setupAnisotropicFiltering()
{
	#ifndef CONFIG_GFX_OPENGL_ES
	GLfloat maximumAnisotropy{};
	glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &maximumAnisotropy);
	support.maximumAnisotropy = maximumAnisotropy;
	#endif
}

void GLRenderer::setupMultisample()
{
	#ifndef CONFIG_GFX_OPENGL_ES
	support.hasMultisample = true;
	#endif
}

void GLRenderer::setupMultisampleHints()
{
	#if !defined CONFIG_GFX_OPENGL_ES && !defined __APPLE__
	support.hasMultisampleHints = true;
	//glHint(GL_MULTISAMPLE_FILTER_HINT_NV, GL_NICEST);
	#endif
}

void GLRenderer::setupNonPow2Textures()
{
	support.textureSizeSupport.nonPow2 = true;
}

void GLRenderer::setupNonPow2MipmapTextures()
{
	support.textureSizeSupport.nonPow2 = true;
	support.textureSizeSupport.nonPow2CanMipmap = true;
}

void GLRenderer::setupNonPow2MipmapRepeatTextures()
{
	support.textureSizeSupport.nonPow2 = true;
	support.textureSizeSupport.nonPow2CanMipmap = true;
	support.textureSizeSupport.nonPow2CanRepeat = true;
}

#ifdef CONFIG_GFX_OPENGL_ES
void GLRenderer::setupBGRPixelSupport()
{
	support.hasBGRPixels = true;
}
#endif

void GLRenderer::setupFBOFuncs(bool &useFBOFuncs)
{
	useFBOFuncs = true;
	#if defined CONFIG_GFX_OPENGL_ES && CONFIG_GFX_OPENGL_ES_MAJOR_VERSION == 1
	support.generateMipmaps = glGenerateMipmapOES;
	#elif !defined CONFIG_GFX_OPENGL_ES
	support.generateMipmaps = glGenerateMipmap;
	#endif
}

void GLRenderer::setupVAOFuncs()
{
	#ifndef CONFIG_GFX_OPENGL_ES
	useStreamVAO = true;
	#endif
}

void GLRenderer::setupTextureSwizzle()
{
	support.hasTextureSwizzle = true;
}

void GLRenderer::setupImmutableTexStorage(bool extSuffix)
{
	if(support.hasImmutableTexStorage)
		return;
	support.hasImmutableTexStorage = true;
	#ifdef CONFIG_GFX_OPENGL_ES
	const char *procName = extSuffix ? "glTexStorage2DEXT" : "glTexStorage2D";
	support.glTexStorage2D = (typeof(support.glTexStorage2D))Base::GLContext::procAddress(procName);
	#endif
}

void GLRenderer::setupRGFormats()
{
	support.luminanceFormat = GL_RED;
	support.luminanceInternalFormat = GL_R8;
	support.luminanceAlphaFormat = GL_RG;
	support.luminanceAlphaInternalFormat = GL_RG8;
	support.alphaFormat = GL_RED;
	support.alphaInternalFormat = GL_R8;
}

void GLRenderer::setupSamplerObjects()
{
	if(support.hasSamplerObjects)
		return;
	support.hasSamplerObjects = true;
	#ifdef CONFIG_GFX_OPENGL_ES
	support.glGenSamplers = (typeof(support.glGenSamplers))Base::GLContext::procAddress("glGenSamplers");
	support.glDeleteSamplers = (typeof(support.glDeleteSamplers))Base::GLContext::procAddress("glDeleteSamplers");
	support.glBindSampler = (typeof(support.glBindSampler))Base::GLContext::procAddress("glBindSampler");
	support.glSamplerParameteri = (typeof(support.glSamplerParameteri))Base::GLContext::procAddress("glSamplerParameteri");
	#endif
}

void GLRenderer::setupPBO()
{
	support.hasPBOFuncs = true;
}

void GLRenderer::setupFenceSync()
{
	if(support.hasSyncFences())
		return;
	#ifdef CONFIG_GFX_OPENGL_ES
	support.glFenceSync = (typeof(support.glFenceSync))Base::GLContext::procAddress("glFenceSync");
	support.glDeleteSync = (typeof(support.glDeleteSync))Base::GLContext::procAddress("glDeleteSync");
	support.glClientWaitSync = (typeof(support.glClientWaitSync))Base::GLContext::procAddress("glClientWaitSync");
	support.glWaitSync = (typeof(support.glWaitSync))Base::GLContext::procAddress("glWaitSync");
	#else
	support.hasFenceSync = true;
	#endif
}

#ifdef CONFIG_GFX_OPENGL_ES
void GLRenderer::setupAppleFenceSync()
{
	if(support.hasSyncFences())
		return;
	support.glFenceSync = (typeof(support.glFenceSync))Base::GLContext::procAddress("glFenceSyncAPPLE");
	support.glDeleteSync = (typeof(support.glDeleteSync))Base::GLContext::procAddress("glDeleteSyncAPPLE");
	support.glClientWaitSync = (typeof(support.glClientWaitSync))Base::GLContext::procAddress("glClientWaitSyncAPPLE");
	support.glWaitSync = (typeof(support.glWaitSync))Base::GLContext::procAddress("glWaitSyncAPPLE");
}
#endif

#ifdef CAN_USE_EGL_SYNC
void GLRenderer::setupEGLFenceSync(bool supportsServerSync)
{
	if(support.hasSyncFences())
		return;
	logMsg("Using EGL sync fences%s", supportsServerSync ? "" : ", only client sync supported");
	#ifdef EGL_SYNC_NEEDS_PROC_ADDR
	eglCreateSyncFunc = (typeof(eglCreateSyncFunc))Base::GLContext::procAddress("eglCreateSyncKHR");
	eglDestroySyncFunc = (typeof(eglDestroySyncFunc))Base::GLContext::procAddress("eglDestroySyncKHR");
	eglClientWaitSyncFunc = (typeof(eglClientWaitSyncFunc))Base::GLContext::procAddress("eglClientWaitSyncKHR");
	if(supportsServerSync)
		eglWaitSyncFunc = (typeof(eglWaitSyncFunc))Base::GLContext::procAddress("eglWaitSyncKHR");
	#endif
	// wrap EGL sync in terms of ARB sync
	support.glFenceSync =
		[](GLenum condition, GLbitfield flags)
		{
			return (GLsync)eglCreateSyncFunc(Base::GLDisplay::getDefault().eglDisplay(), EGL_SYNC_FENCE, nullptr);
		};
	support.glDeleteSync =
		[](GLsync sync)
		{
			eglDestroySyncFunc(Base::GLDisplay::getDefault().eglDisplay(), (EGLSync)sync);
		};
	support.glClientWaitSync =
		[](GLsync sync, GLbitfield flags, GLuint64 timeout) -> GLenum
		{
			switch(eglClientWaitSyncFunc(Base::GLDisplay::getDefault().eglDisplay(), (EGLSync)sync, 0, timeout))
			{
				case EGL_TIMEOUT_EXPIRED: return GL_TIMEOUT_EXPIRED;
				case EGL_CONDITION_SATISFIED: return GL_CONDITION_SATISFIED;
				default:
					logErr("error waiting for sync object:%p", sync);
					return GL_WAIT_FAILED;
			}
		};
	if(supportsServerSync)
	{
		support.glWaitSync =
			[](GLsync sync, GLbitfield flags, GLuint64 timeout)
			{
				if(eglWaitSyncFunc(Base::GLDisplay::getDefault().eglDisplay(), (EGLSync)sync, 0) == GL_FALSE)
				{
					logErr("error waiting for sync object:%p", sync);
				}
			};
	}
	else
	{
		support.glWaitSync =
			[](GLsync sync, GLbitfield flags, GLuint64 timeout)
			{
				if(eglClientWaitSyncFunc(Base::GLDisplay::getDefault().eglDisplay(), (EGLSync)sync, 0, timeout) == GL_FALSE)
				{
					logErr("error waiting for sync object:%p", sync);
				}
			};
	}
}
#endif

void GLRenderer::setupSpecifyDrawReadBuffers()
{
	#ifdef CONFIG_GFX_OPENGL_ES
	support.glDrawBuffers = (typeof(support.glDrawBuffers))Base::GLContext::procAddress("glDrawBuffers");
	support.glReadBuffer = (typeof(support.glReadBuffer))Base::GLContext::procAddress("glReadBuffer");
	#endif
}

bool DrawContextSupport::hasDrawReadBuffers() const
{
	#ifdef CONFIG_GFX_OPENGL_ES
	return glDrawBuffers;
	#else
	return true;
	#endif
}

bool DrawContextSupport::hasSyncFences() const
{
	#ifdef CONFIG_GFX_OPENGL_ES
	return glFenceSync;
	#else
	return hasFenceSync;
	#endif
}

#ifdef __ANDROID__
bool DrawContextSupport::hasEGLTextureStorage() const
{
	return glEGLImageTargetTexStorageEXT;
}
#endif

bool DrawContextSupport::hasImmutableBufferStorage() const
{
	#ifdef CONFIG_GFX_OPENGL_ES
	return glBufferStorage;
	#else
	return hasBufferStorage;
	#endif
}

void GLRenderer::setupUnmapBufferFunc()
{
	#ifdef CONFIG_GFX_OPENGL_ES
	if(!support.glUnmapBuffer)
	{
		if constexpr(Config::envIsAndroid || Config::envIsIOS)
		{
			support.glUnmapBuffer = (DrawContextSupport::UnmapBufferProto)glUnmapBufferOES;
		}
		else
		{
			if constexpr(Config::Gfx::OPENGL_ES)
			{
				support.glUnmapBuffer = (typeof(support.glUnmapBuffer))Base::GLContext::procAddress("glUnmapBufferOES");
			}
			else
			{
				support.glUnmapBuffer = (typeof(support.glUnmapBuffer))Base::GLContext::procAddress("glUnmapBuffer");
			}
		}
	}
	#endif
}

void GLRenderer::setupImmutableBufferStorage()
{
	if(support.hasImmutableBufferStorage())
		return;
	#ifdef CONFIG_GFX_OPENGL_ES
	support.glBufferStorage = (typeof(support.glBufferStorage))Base::GLContext::procAddress("glBufferStorageEXT");
	#else
	support.hasBufferStorage = true;
	#endif
}

void GLRenderer::checkExtensionString(const char *extStr, bool &useFBOFuncs)
{
	//logMsg("checking %s", extStr);
	if(string_equal(extStr, "GL_ARB_texture_non_power_of_two")
		|| (Config::Gfx::OPENGL_ES && string_equal(extStr, "GL_OES_texture_npot")))
	{
		// allows mipmaps and repeat modes
		setupNonPow2MipmapRepeatTextures();
	}
	#ifdef CONFIG_GFX_OPENGL_DEBUG_CONTEXT
	else if(Config::DEBUG_BUILD && string_equal(extStr, "GL_KHR_debug"))
	{
		support.hasDebugOutput = true;
		#ifdef __ANDROID__
		// older GPU drivers like Tegra 3 can crash when using debug output,
		// only enable on recent Android version to be safe
		if(Base::androidSDK() < 23)
		{
			support.hasDebugOutput = false;
		}
		#endif
	}
	#endif
	#ifdef CONFIG_GFX_OPENGL_ES
	else if(Config::Gfx::OPENGL_ES_MAJOR_VERSION == 1
		&& (string_equal(extStr, "GL_APPLE_texture_2D_limited_npot") || string_equal(extStr, "GL_IMG_texture_npot")))
	{
		// no mipmaps or repeat modes
		setupNonPow2Textures();
	}
	else if(Config::Gfx::OPENGL_ES_MAJOR_VERSION >= 2
		&& !Config::envIsIOS && string_equal(extStr, "GL_NV_texture_npot_2D_mipmap"))
	{
		// no repeat modes
		setupNonPow2MipmapTextures();
	}
	else if(Config::Gfx::OPENGL_ES_MAJOR_VERSION >= 2 && string_equal(extStr, "GL_EXT_unpack_subimage"))
	{
		support.hasUnpackRowLength = true;
	}
	else if(string_equal(extStr, "GL_APPLE_texture_format_BGRA8888"))
	{
		support.bgrInternalFormat = GL_RGBA;
		setupBGRPixelSupport();
	}
	else if(string_equal(extStr, "GL_EXT_texture_format_BGRA8888"))
	{
		setupBGRPixelSupport();
	}
	else if(Config::Gfx::OPENGL_ES_MAJOR_VERSION == 1 && string_equal(extStr, "GL_OES_framebuffer_object"))
	{
		if(!useFBOFuncs)
			setupFBOFuncs(useFBOFuncs);
	}
	else if(string_equal(extStr, "GL_EXT_texture_storage"))
	{
		setupImmutableTexStorage(true);
	}
	#if defined __ANDROID__ || defined __APPLE__
	else if(string_equal(extStr, "GL_APPLE_sync"))
	{
		setupAppleFenceSync();
	}
	#endif
	#if defined __ANDROID__
	else if(string_equal(extStr, "GL_OES_EGL_image"))
	{
		support.hasEGLImages = true;
	}
	else if(Config::Gfx::OPENGL_ES_MAJOR_VERSION >= 2 &&
		string_equal(extStr, "GL_OES_EGL_image_external"))
	{
		support.hasExternalEGLImages = true;
	}
	else if(Config::Gfx::OPENGL_ES_MAJOR_VERSION >= 2 &&
		string_equal(extStr, "GL_EXT_EGL_image_storage"))
	{
		support.glEGLImageTargetTexStorageEXT = (typeof(support.glEGLImageTargetTexStorageEXT))Base::GLContext::procAddress("glEGLImageTargetTexStorageEXT");
	}
	#endif
	else if(Config::Gfx::OPENGL_ES_MAJOR_VERSION >= 2 && string_equal(extStr, "GL_NV_pixel_buffer_object"))
	{
		setupPBO();
	}
	else if(Config::Gfx::OPENGL_ES_MAJOR_VERSION >= 2 && string_equal(extStr, "GL_NV_map_buffer_range"))
	{
		if(!support.glMapBufferRange)
			support.glMapBufferRange = (typeof(support.glMapBufferRange))Base::GLContext::procAddress("glMapBufferRangeNV");
		setupUnmapBufferFunc();
	}
	else if(string_equal(extStr, "GL_EXT_map_buffer_range"))
	{
		if(!support.glMapBufferRange)
			support.glMapBufferRange = (typeof(support.glMapBufferRange))Base::GLContext::procAddress("glMapBufferRangeEXT");
		// Only using ES 3.0 version currently
		//if(!support.glFlushMappedBufferRange)
		//	support.glFlushMappedBufferRange = (typeof(support.glFlushMappedBufferRange))Base::GLContext::procAddress("glFlushMappedBufferRangeEXT");
		setupUnmapBufferFunc();
	}
	else if(Config::Gfx::OPENGL_ES_MAJOR_VERSION >= 2 && string_equal(extStr, "GL_EXT_buffer_storage"))
	{
		setupImmutableBufferStorage();
	}
	/*else if(string_equal(extStr, "GL_OES_mapbuffer"))
	{
		// handled in *_map_buffer_range currently
	}*/
	#endif
	#ifndef CONFIG_GFX_OPENGL_ES
	else if(string_equal(extStr, "GL_EXT_texture_filter_anisotropic"))
	{
		setupAnisotropicFiltering();
	}
	else if(string_equal(extStr, "GL_ARB_multisample"))
	{
		setupMultisample();
	}
	else if(string_equal(extStr, "GL_NV_multisample_filter_hint"))
	{
		setupMultisampleHints();
	}
	else if(string_equal(extStr, "GL_EXT_framebuffer_object"))
	{
		#ifndef __APPLE__
		if(!useFBOFuncs)
		{
			setupFBOFuncs(useFBOFuncs);
			support.generateMipmaps = glGenerateMipmapEXT;
		}
		#endif
	}
	else if(string_equal(extStr, "GL_ARB_framebuffer_object"))
	{
		if(!useFBOFuncs)
			setupFBOFuncs(useFBOFuncs);
	}
	else if(string_equal(extStr, "GL_ARB_texture_storage"))
	{
		setupImmutableTexStorage(false);
	}
	else if(string_equal(extStr, "GL_ARB_pixel_buffer_object"))
	{
		setupPBO();
	}
	else if(string_equal(extStr, "GL_ARB_sync"))
	{
		setupFenceSync();
	}
	else if(string_equal(extStr, "GL_ARB_buffer_storage"))
	{
		setupImmutableBufferStorage();
	}
	#endif
}

void GLRenderer::checkFullExtensionString(const char *fullExtStr)
{
	char fullExtStrTemp[strlen(fullExtStr)+1];
	strcpy(fullExtStrTemp, fullExtStr);
	char *savePtr;
	auto extStr = strtok_r(fullExtStrTemp, " ", &savePtr);
	bool useFBOFuncs = false;
	while(extStr)
	{
		checkExtensionString(extStr, useFBOFuncs);
		extStr = strtok_r(nullptr, " ", &savePtr);
	}
}

static int glVersionFromStr(const char *versionStr)
{
	// skip to version number
	while(!isdigit(*versionStr) && *versionStr != '\0')
		versionStr++;
	int major = 1, minor = 0;
	if(sscanf(versionStr, "%d.%d", &major, &minor) != 2)
	{
		logErr("unable to parse GL version string");
	}
	return 10 * major + minor;
}

static Base::GLContextAttributes makeGLContextAttributes(uint32_t majorVersion, uint32_t minorVersion)
{
	Base::GLContextAttributes glAttr;
	if(Config::DEBUG_BUILD)
		glAttr.setDebug(true);
	glAttr.setMajorVersion(majorVersion);
	#ifdef CONFIG_GFX_OPENGL_ES
	glAttr.setOpenGLESAPI(true);
	#else
	glAttr.setMinorVersion(minorVersion);
	#endif
	return glAttr;
}

Base::GLContextAttributes GLRenderer::makeKnownGLContextAttributes()
{
	#ifdef CONFIG_GFX_OPENGL_ES
	if(Config::Gfx::OPENGL_ES_MAJOR_VERSION == 1)
	{
		return makeGLContextAttributes(1, 0);
	}
	else
	{
		assert(glMajorVer);
		return makeGLContextAttributes(glMajorVer, 0);
	}
	#else
	if(Config::Gfx::OPENGL_SHADER_PIPELINE)
	{
		return makeGLContextAttributes(3, 3);
	}
	else
	{
		return makeGLContextAttributes(1, 3);
	}
	#endif
}

void GLRenderer::finishContextCreation(Base::GLContext ctx)
{
	#if CONFIG_GFX_OPENGL_ES_MAJOR_VERSION > 1
	if(Config::envIsAndroid && Config::MACHINE == Config::Machine::GENERIC_ARMV7 && glMajorVer == 2)
	{
		// Vivante "GC1000 core" GPU (Samsung Galaxy S3 Mini, Galaxy Tab 3), possibly others, will fail
		// setting context in render thread with EGL_BAD_ACCESS unless it's first set in the creation thread,
		// exact cause unknown and is most likely a driver bug
		logDMsg("toggling newly created context current on this thread to avoid driver issues");
		ctx.setCurrent(glDpy, ctx, {});
		ctx.setCurrent(glDpy, {}, {});
	}
	#endif
}

Renderer::Renderer() {}

Renderer::Renderer(IG::PixelFormat pixelFormat, Error &err)
{
	auto [ec, dpy] = Base::GLDisplay::makeDefault(glAPI);
	if(ec)
	{
		logErr("error getting GL display");
		err = std::runtime_error("error creating GL display");
		return;
	}
	glDpy = dpy;
	dpy.logInfo();
	if(!pixelFormat)
		pixelFormat = Base::Window::defaultPixelFormat();
	Base::GLBufferConfigAttributes glBuffAttr;
	glBuffAttr.setPixelFormat(pixelFormat);
	#if CONFIG_GFX_OPENGL_ES_MAJOR_VERSION == 1
	auto glAttr = makeGLContextAttributes(1, 0);
	auto [found, config] = gfxResourceContext.makeBufferConfig(dpy, glAttr, glBuffAttr);
	assert(found);
	gfxBufferConfig = config;
	IG::ErrorCode ec{};
	gfxResourceContext = {dpy, glAttr, gfxBufferConfig, ec};
	#elif CONFIG_GFX_OPENGL_ES_MAJOR_VERSION > 1
	if(CAN_USE_OPENGL_ES_3)
	{
		auto glAttr = makeGLContextAttributes(3, 0);
		auto [found, config] = gfxResourceContext.makeBufferConfig(dpy, glAttr, glBuffAttr);
		if(found)
		{
			gfxBufferConfig = config;
			IG::ErrorCode ec{};
			gfxResourceContext = {dpy, glAttr, gfxBufferConfig, ec};
			glMajorVer = glAttr.majorVersion();
		}
	}
	if(!gfxResourceContext) // fall back to OpenGL ES 2.0
	{
		auto glAttr = makeGLContextAttributes(2, 0);
		auto [found, config] = gfxResourceContext.makeBufferConfig(dpy, glAttr, glBuffAttr);
		assert(found);
		gfxBufferConfig = config;
		IG::ErrorCode ec{};
		gfxResourceContext = {dpy, glAttr, gfxBufferConfig, ec};
		glMajorVer = glAttr.majorVersion();
	}
	#else
	if(Config::Gfx::OPENGL_SHADER_PIPELINE)
	{
		#ifdef CONFIG_GFX_OPENGL_FIXED_FUNCTION_PIPELINE
		support.useFixedFunctionPipeline = false;
		#endif
		auto glAttr = makeGLContextAttributes(3, 3);
		auto [found, config] = gfxResourceContext.makeBufferConfig(dpy, glAttr, glBuffAttr);
		assert(found);
		IG::ErrorCode ec{};
		gfxBufferConfig = config;
		gfxResourceContext = {dpy, glAttr, gfxBufferConfig, ec};
		if(!gfxResourceContext)
		{
			logMsg("3.3 context not supported");
		}
	}
	if(Config::Gfx::OPENGL_FIXED_FUNCTION_PIPELINE && !gfxResourceContext)
	{
		#ifdef CONFIG_GFX_OPENGL_FIXED_FUNCTION_PIPELINE
		support.useFixedFunctionPipeline = true;
		#endif
		auto glAttr = makeGLContextAttributes(1, 3);
		auto [found, config] = gfxResourceContext.makeBufferConfig(dpy, glAttr, glBuffAttr);
		assert(found);
		IG::ErrorCode ec{};
		gfxBufferConfig = config;
		gfxResourceContext = {dpy, glAttr, gfxBufferConfig, ec};
		if(!gfxResourceContext)
		{
			logMsg("1.3 context not supported");
		}
	}
	#endif
	if(!gfxResourceContext)
	{
		err = std::runtime_error("error creating GL context");
		return;
	}
	err = {};
	finishContextCreation(gfxResourceContext);
	mainTask = std::make_unique<GLMainTask>();
	mainTask->start(gfxResourceContext);
}

Renderer::Renderer(Error &err): Renderer(Base::Window::defaultPixelFormat(), err) {}

void Renderer::configureRenderer(ThreadMode threadMode)
{
	runGLTaskSync(
		[this]()
		{
			auto version = (const char*)glGetString(GL_VERSION);
			assert(version);
			auto rendererName = (const char*)glGetString(GL_RENDERER);
			logMsg("version: %s (%s)", version, rendererName);

			int glVer = glVersionFromStr(version);

			bool useFBOFuncs = false;
			#ifndef CONFIG_GFX_OPENGL_ES
			// core functionality
			if(glVer >= 15)
			{
				support.hasVBOFuncs = true;
			}
			if(glVer >= 20)
			{
				setupNonPow2MipmapRepeatTextures();
				setupSpecifyDrawReadBuffers();
			}
			if(glVer >= 21)
			{
				setupPBO();
			}
			if(glVer >= 30)
			{
				if(!support.useFixedFunctionPipeline)
				{
					// must render via VAOs/VBOs in 3.1+ without compatibility context
					setupVAOFuncs();
					setupTextureSwizzle();
					setupRGFormats();
					setupSamplerObjects();
				}
				setupFBOFuncs(useFBOFuncs);
			}
			if(glVer >= 32)
			{
				setupFenceSync();
			}

			// extension functionality
			if(glVer >= 30)
			{
				GLint numExtensions;
				glGetIntegerv(GL_NUM_EXTENSIONS, &numExtensions);
				if(Config::DEBUG_BUILD)
				{
					logMsgNoBreak("extensions: ");
					iterateTimes(numExtensions, i)
					{
						logger_printf(LOG_M, "%s ", (const char*)glGetStringi(GL_EXTENSIONS, i));
					}
					logger_printf(LOG_M, "\n");
				}
				iterateTimes(numExtensions, i)
				{
					checkExtensionString((const char*)glGetStringi(GL_EXTENSIONS, i), useFBOFuncs);
				}
			}
			else
			{
				auto extensions = (const char*)glGetString(GL_EXTENSIONS);
				assert(extensions);
				logMsg("extensions: %s", extensions);
				checkFullExtensionString(extensions);
			}
			#else
			// core functionality
			if(Config::Gfx::OPENGL_ES_MAJOR_VERSION == 1 && glVer >= 11)
			{
				// safe to use VBOs
			}
			if(Config::Gfx::OPENGL_ES_MAJOR_VERSION > 1)
			{
				if(glVer >= 30)
					setupNonPow2MipmapRepeatTextures();
				else
					setupNonPow2Textures();
				setupFBOFuncs(useFBOFuncs);
				if(glVer >= 30)
				{
					support.glMapBufferRange = (typeof(support.glMapBufferRange))Base::GLContext::procAddress("glMapBufferRange");
					support.glUnmapBuffer = (typeof(support.glUnmapBuffer))Base::GLContext::procAddress("glUnmapBuffer");
					support.glFlushMappedBufferRange = (typeof(support.glFlushMappedBufferRange))Base::GLContext::procAddress("glFlushMappedBufferRange");
					setupImmutableTexStorage(false);
					setupTextureSwizzle();
					setupRGFormats();
					setupSamplerObjects();
					setupPBO();
					setupFenceSync();
					if(!Config::envIsIOS)
						setupSpecifyDrawReadBuffers();
					support.hasUnpackRowLength = true;
					support.useLegacyGLSL = false;
				}
			}

			#ifdef CAN_USE_EGL_SYNC
			// check for fence sync via EGL extensions
			bool checkFenceSync = glVer < 30
					&& !Config::MACHINE_IS_PANDORA; // TODO: driver waits for full timeout even if commands complete,
																					// possibly broken glFlush() behavior?
			if(checkFenceSync)
			{
				auto extStr = glDpy.queryExtensions();
				if(strstr(extStr, "EGL_KHR_fence_sync"))
				{
					auto supportsServerSync = strstr(extStr, "EGL_KHR_wait_sync");
					setupEGLFenceSync(supportsServerSync);
				}
			}
			#endif

			// extension functionality
			auto extensions = (const char*)glGetString(GL_EXTENSIONS);
			assert(extensions);
			logMsg("extensions: %s", extensions);
			checkFullExtensionString(extensions);
			#endif // CONFIG_GFX_OPENGL_ES

			GLint texSize;
			glGetIntegerv(GL_MAX_TEXTURE_SIZE, &texSize);
			support.textureSizeSupport.maxXSize = support.textureSizeSupport.maxYSize = texSize;
			assert(support.textureSizeSupport.maxXSize > 0 && support.textureSizeSupport.maxYSize > 0);

			printFeatures(support);
		});

	if(Config::DEBUG_BUILD && defaultToFullErrorChecks)
	{
		setCorrectnessChecks(true);
		setDebugOutput(true);
	}

	if(!support.hasSyncFences())
		threadMode = ThreadMode::SINGLE;
	if(threadMode == ThreadMode::AUTO)
	{
		useSeparateDrawContext = support.hasSyncFences();
		#if defined __ANDROID__
		if(Base::androidSDK() < 26 && !support.hasImmutableBufferStorage())
		{
			useSeparateDrawContext = false; // disable by default due to various devices with driver bugs
		}
		#endif
	}
	else
	{
		useSeparateDrawContext = threadMode == ThreadMode::MULTI;
	}
	if(useSeparateDrawContext)
	{
		auto overridePath = FS::makePathStringPrintf("%s/imagine_force_single_gl_context", Base::sharedStoragePath().data());
		if(FS::exists(overridePath))
		{
			logMsg("disabling separate draw context due to file:%s", overridePath.data());
			useSeparateDrawContext = false;
		}
	}
	support.isConfigured = true;
}

bool Renderer::isConfigured() const
{
	return support.isConfigured;
}

Renderer Renderer::makeConfiguredRenderer(ThreadMode threadMode, IG::PixelFormat pixelFormat, Error &err)
{
	auto renderer = Renderer{pixelFormat, err};
	if(err)
		return {};
	renderer.configureRenderer(threadMode);
	return renderer;
}

Renderer Renderer::makeConfiguredRenderer(ThreadMode threadMode, Error &err)
{
	return makeConfiguredRenderer(threadMode, Base::Window::defaultPixelFormat(), err);
}

Renderer Renderer::makeConfiguredRenderer(Error &err)
{
	return makeConfiguredRenderer(Gfx::Renderer::ThreadMode::AUTO, err);
}

Renderer::ThreadMode Renderer::threadMode() const
{
	return useSeparateDrawContext ? ThreadMode::MULTI : ThreadMode::SINGLE;
}

bool Renderer::supportsThreadMode() const
{
	return support.hasSyncFences();
}

Base::WindowConfig Renderer::addWindowConfig(Base::WindowConfig config)
{
	assert(isConfigured());
	config.setFormat(gfxBufferConfig.windowFormat(glDpy));
	return config;
}

static void updateSensorStateForWindowOrientations(Base::Window &win)
{
	// activate orientation sensor if doing rotation in software and the main window
	// has multiple valid orientations
	if(Config::SYSTEM_ROTATES_WINDOWS || win != Base::mainWindow())
		return;
	Base::setDeviceOrientationChangeSensor(IG::bitsSet(win.validSoftOrientations()) > 1);
}

void Renderer::initWindow(Base::Window &win, Base::WindowConfig config)
{
	win.init(addWindowConfig(config));
	updateSensorStateForWindowOrientations(win);
}

void Renderer::setWindowValidOrientations(Base::Window &win, Base::Orientation validO)
{
	if(win != Base::mainWindow())
		return;
	auto oldWinO = win.softOrientation();
	if(win.setValidOrientations(validO) && !Config::SYSTEM_ROTATES_WINDOWS)
	{
		animateProjectionMatrixRotation(win, orientationToGC(oldWinO), orientationToGC(win.softOrientation()));
	}
	updateSensorStateForWindowOrientations(win);
}

void GLRenderer::addEventHandlers()
{
	if(onExit)
		return;
	onExit =
		[this](bool backgrounded)
		{
			releaseShaderCompilerEvent.cancel();
			if(backgrounded)
			{
				runGLTaskSync(
					[=]()
					{
						#ifdef CONFIG_GFX_OPENGL_SHADER_PIPELINE
						glReleaseShaderCompiler();
						#endif
						glFinish();
					});
			}
			else
			{
				if(!gfxResourceContext)
					return true;
				mainTask->stop();
				gfxResourceContext.deinit(glDpy);
				glDpy.deinit();
				contextDestroyed = true;
			}
			return true;
		};
	Base::addOnExit(onExit, Base::RENDERER_ON_EXIT_PRIORITY);
	#ifdef CONFIG_GFX_OPENGL_SHADER_PIPELINE
	releaseShaderCompilerEvent.attach(
		[this]()
		{
			logMsg("automatically releasing shader compiler");
			static_cast<Renderer*>(this)->releaseShaderCompiler();
		});
	#endif
}

}
