/*
 *	Copyright (C) 2011-2013 Gregory hainaut
 *	Copyright (C) 2007-2009 Gabest
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with GNU Make; see the file COPYING.  If not, write to
 *  the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#include "stdafx.h"
#include "GSDeviceOGL.h"

#include "res/glsl_source.h"

//#define LOUD_DEBUGGING
//#define PRINT_FRAME_NUMBER
//#define ONLY_LINES

static uint32 g_draw_count = 0;
static uint32 g_frame_count = 1;		

static const uint32 g_merge_cb_index      = 10;
static const uint32 g_interlace_cb_index  = 11;
static const uint32 g_shadeboost_cb_index = 12;
static const uint32 g_fxaa_cb_index       = 13;

GSDeviceOGL::GSDeviceOGL()
	: m_free_window(false)
	  , m_window(NULL)
	  , m_pipeline(0)
	  , m_fbo(0)
	  , m_fbo_read(0)
	  , m_vb_sr(NULL)
	  , m_shader(NULL)
{
	memset(&m_merge_obj, 0, sizeof(m_merge_obj));
	memset(&m_interlace, 0, sizeof(m_interlace));
	memset(&m_convert, 0, sizeof(m_convert));
	memset(&m_date, 0, sizeof(m_date));
	memset(&m_state, 0, sizeof(m_state));

	// Reset the debug file
	#ifdef ENABLE_OGL_DEBUG
	FILE* f = fopen("Debug.txt","w");
	fclose(f);
	#endif
}

GSDeviceOGL::~GSDeviceOGL()
{
	// If the create function wasn't called nothing to do.
	if (m_shader == NULL)
		return;

	// Clean vertex buffer state
	delete (m_vb_sr);

	// Clean m_merge_obj
	for (uint32 i = 0; i < 2; i++)
		m_shader->Delete(m_merge_obj.ps[i]);
	delete (m_merge_obj.cb);
	delete (m_merge_obj.bs);

	// Clean m_interlace
	for (uint32 i = 0; i < 2; i++)
		m_shader->Delete(m_interlace.ps[i]);
	delete (m_interlace.cb);

	// Clean m_convert
	m_shader->Delete(m_convert.vs);
	for (uint32 i = 0; i < 2; i++)
		m_shader->Delete(m_convert.ps[i]);
	gl_DeleteSamplers(1, &m_convert.ln);
	gl_DeleteSamplers(1, &m_convert.pt);
	delete m_convert.dss;
	delete m_convert.bs;

	// Clean m_fxaa
	delete m_fxaa.cb;
	m_shader->Delete(m_fxaa.ps);

	// Clean m_date
	delete m_date.dss;
	delete m_date.bs;

	// Clean shadeboost
	delete m_shadeboost.cb;
	m_shader->Delete(m_shadeboost.ps);


	// Clean various opengl allocation
	gl_DeleteFramebuffers(1, &m_fbo);
	gl_DeleteFramebuffers(1, &m_fbo_read);

	// Delete HW FX
	delete m_vs_cb;
	delete m_ps_cb;
	gl_DeleteSamplers(1, &m_palette_ss);
	delete m_vb;

	for (uint32 key = 0; key < VSSelector::size(); key++) m_shader->Delete(m_vs[key]);
	for (uint32 key = 0; key < GSSelector::size(); key++) m_shader->Delete(m_gs[key]);
	for (auto it = m_ps.begin(); it != m_ps.end() ; it++) m_shader->Delete(it->second);

	m_ps.clear();

	gl_DeleteSamplers(PSSamplerSelector::size(), m_ps_ss);

	for (uint32 key = 0; key < OMDepthStencilSelector::size(); key++) delete m_om_dss[key];

	for (auto it = m_om_bs.begin(); it != m_om_bs.end(); it++) delete it->second;
	m_om_bs.clear();

	// Must be done after the destruction of all shader/program objects
	delete m_shader;
	m_shader = NULL;
}

GSTexture* GSDeviceOGL::CreateSurface(int type, int w, int h, bool msaa, int format)
{
	// A wrapper to call GSTextureOGL, with the different kind of parameter
	GSTextureOGL* t = NULL;
	t = new GSTextureOGL(type, w, h, format, m_fbo_read);

	switch(type)
	{
		case GSTexture::RenderTarget:
			ClearRenderTarget(t, 0);
			break;
		case GSTexture::DepthStencil:
			ClearDepth(t, 0);
			// No need to clear the stencil now.
			break;
	}
	return t;
}

GSTexture* GSDeviceOGL::FetchSurface(int type, int w, int h, bool msaa, int format)
{
	return GSDevice::FetchSurface(type, w, h, false, format);
}

bool GSDeviceOGL::Create(GSWnd* wnd)
{
	if (m_window == NULL) {
		if (!GLLoader::check_gl_version(3, 0)) return false;

		if (!GLLoader::check_gl_supported_extension()) return false;
	}

	m_window = wnd;

	// ****************************************************************
	// Various object
	// ****************************************************************
	m_shader = new GSShaderOGL(!!theApp.GetConfig("debug_ogl_shader", 1), GLLoader::found_GL_ARB_separate_shader_objects, GLLoader::found_GL_ARB_shading_language_420pack);

	gl_GenFramebuffers(1, &m_fbo);
	gl_GenFramebuffers(1, &m_fbo_read);

	// ****************************************************************
	// Vertex buffer state
	// ****************************************************************
	GSInputLayoutOGL il_convert[2] =
	{
		{0, 4, GL_FLOAT, GL_FALSE, sizeof(GSVertexPT1), (const GLvoid*)offsetof(struct GSVertexPT1, p) },
		{1, 2, GL_FLOAT, GL_FALSE, sizeof(GSVertexPT1), (const GLvoid*)offsetof(struct GSVertexPT1, t) },
	};
	m_vb_sr = new GSVertexBufferStateOGL(sizeof(GSVertexPT1), il_convert, countof(il_convert));

	// ****************************************************************
	// convert
	// ****************************************************************
	m_convert.vs = m_shader->Compile("convert.glsl", "vs_main", GL_VERTEX_SHADER, convert_glsl);
	for(size_t i = 0; i < countof(m_convert.ps); i++)
		m_convert.ps[i] = m_shader->Compile("convert.glsl", format("ps_main%d", i), GL_FRAGMENT_SHADER, convert_glsl);

	// Note the following object are initialized to 0 so disabled.
	// Note: maybe enable blend with a factor of 1
	// m_convert.dss, m_convert.bs

	m_convert.ln = CreateSampler(true, false, false);
	m_convert.pt = CreateSampler(false, false, false);

	m_convert.dss = new GSDepthStencilOGL();
	m_convert.bs  = new GSBlendStateOGL();

	// ****************************************************************
	// merge
	// ****************************************************************
	m_merge_obj.cb = new GSUniformBufferOGL(g_merge_cb_index, sizeof(MergeConstantBuffer));

	for(size_t i = 0; i < countof(m_merge_obj.ps); i++)
		m_merge_obj.ps[i] = m_shader->Compile("merge.glsl", format("ps_main%d", i), GL_FRAGMENT_SHADER, merge_glsl);

	m_merge_obj.bs = new GSBlendStateOGL();
	m_merge_obj.bs->EnableBlend();
	m_merge_obj.bs->SetRGB(GL_FUNC_ADD, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	// ****************************************************************
	// interlace
	// ****************************************************************
	m_interlace.cb = new GSUniformBufferOGL(g_interlace_cb_index, sizeof(InterlaceConstantBuffer));

	for(size_t i = 0; i < countof(m_interlace.ps); i++)
		m_interlace.ps[i] = m_shader->Compile("interlace.glsl", format("ps_main%d", i), GL_FRAGMENT_SHADER, interlace_glsl);
	// ****************************************************************
	// Shade boost
	// ****************************************************************
	m_shadeboost.cb = new GSUniformBufferOGL(g_shadeboost_cb_index, sizeof(ShadeBoostConstantBuffer));

	int ShadeBoost_Contrast = theApp.GetConfig("ShadeBoost_Contrast", 50);
	int ShadeBoost_Brightness = theApp.GetConfig("ShadeBoost_Brightness", 50);
	int ShadeBoost_Saturation = theApp.GetConfig("ShadeBoost_Saturation", 50);
	std::string shade_macro = format("#define SB_SATURATION %d.0\n", ShadeBoost_Saturation)
		+ format("#define SB_BRIGHTNESS %d.0\n", ShadeBoost_Brightness)
		+ format("#define SB_CONTRAST %d.0\n", ShadeBoost_Contrast);

	m_shadeboost.ps = m_shader->Compile("shadeboost.glsl", "ps_main", GL_FRAGMENT_SHADER, shadeboost_glsl, shade_macro);

	// ****************************************************************
	// rasterization configuration
	// ****************************************************************
#ifndef ENABLE_GLES
	glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
#endif
	glDisable(GL_CULL_FACE);
	glEnable(GL_SCISSOR_TEST);
	// FIXME enable it when multisample code will be here
	// DX: rd.MultisampleEnable = true;
#ifndef ENABLE_GLES
	glDisable(GL_MULTISAMPLE);
#endif
#ifdef ONLY_LINES
	glLineWidth(5.0);
	glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
#endif
	// Hum I don't know for those options but let's hope there are not activated
#if 0
	rd.FrontCounterClockwise = false;
	rd.DepthBias = false;
	rd.DepthBiasClamp = 0;
	rd.SlopeScaledDepthBias = 0;
	rd.DepthClipEnable = false; // ???
	rd.AntialiasedLineEnable = false;
#endif

	// ****************************************************************
	// fxaa
	// ****************************************************************
	std::string fxaa_macro = "#define FXAA_GLSL_130 1\n";
	if (GLLoader::found_GL_ARB_gpu_shader5) {
		// This extension become core on openGL4
		fxaa_macro += "#extension GL_ARB_gpu_shader5 : enable\n";
		fxaa_macro += "#define FXAA_GATHER4_ALPHA 1\n";
	}
	m_fxaa.cb = new GSUniformBufferOGL(g_fxaa_cb_index, sizeof(FXAAConstantBuffer));
	m_fxaa.ps = m_shader->Compile("fxaa.fx", "ps_main", GL_FRAGMENT_SHADER, fxaa_fx, fxaa_macro);

	// ****************************************************************
	// DATE
	// ****************************************************************

	m_date.dss = new GSDepthStencilOGL();
	m_date.dss->EnableStencil();
	m_date.dss->SetStencil(GL_ALWAYS, GL_REPLACE);

	m_date.bs = new GSBlendStateOGL();
#ifndef ENABLE_OGL_STENCIL_DEBUG
	// Only keep stencil data
	m_date.bs->SetMask(false, false, false, false);
#endif

	// ****************************************************************
	// HW renderer shader
	// ****************************************************************
	CreateTextureFX();

	// ****************************************************************
	// Finish window setup and backbuffer
	// ****************************************************************
	if(!GSDevice::Create(wnd))
		return false;

	GSVector4i rect = wnd->GetClientRect();
	Reset(rect.z, rect.w);

	return true;
}

bool GSDeviceOGL::Reset(int w, int h)
{
	if(!GSDevice::Reset(w, h))
		return false;

	// TODO
	// Opengl allocate the backbuffer with the window. The render is done in the backbuffer when
	// there isn't any FBO. Only a dummy texture is created to easily detect when the rendering is done
	// in the backbuffer
	m_backbuffer = new GSTextureOGL(GSTextureOGL::Backbuffer, w, h, 0, m_fbo_read);

	return true;
}

void GSDeviceOGL::SetVSync(bool enable)
{
	m_wnd->SetVSync(enable);
}

void GSDeviceOGL::Flip()
{
	// FIXME: disable it when code is working
	#ifdef ENABLE_OGL_DEBUG
	CheckDebugLog();
	#endif

	m_wnd->Flip();

#ifdef PRINT_FRAME_NUMBER
	fprintf(stderr, "Draw %d (Frame %d)\n", g_draw_count, g_frame_count);
#endif
#if defined(ENABLE_OGL_DEBUG) || defined(PRINT_FRAME_NUMBER)
	g_frame_count++;
#endif
}

void GSDeviceOGL::BeforeDraw()
{
	m_shader->UseProgram();
//#ifdef ENABLE_OGL_STENCIL_DEBUG
//	if (m_date.t)
//		static_cast<GSTextureOGL*>(m_date.t)->Save(format("/tmp/date_before_%04ld.csv", g_draw_count));
//#endif
}

void GSDeviceOGL::AfterDraw()
{
//#ifdef ENABLE_OGL_STENCIL_DEBUG
//	if (m_date.t)
//		static_cast<GSTextureOGL*>(m_date.t)->Save(format("/tmp/date_after_%04ld.csv", g_draw_count));
//#endif
#if defined(ENABLE_OGL_DEBUG) || defined(PRINT_FRAME_NUMBER)
	g_draw_count++;
#endif
}

void GSDeviceOGL::DrawPrimitive()
{
	BeforeDraw();
	m_state.vb->DrawPrimitive();
	AfterDraw();
}

void GSDeviceOGL::DrawIndexedPrimitive()
{
	BeforeDraw();
	m_state.vb->DrawIndexedPrimitive();
	AfterDraw();
}

void GSDeviceOGL::DrawIndexedPrimitive(int offset, int count)
{
	ASSERT(offset + count <= (int)m_index.count);

	BeforeDraw();
	m_state.vb->DrawIndexedPrimitive(offset, count);
	AfterDraw();
}

void GSDeviceOGL::ClearRenderTarget(GSTexture* t, const GSVector4& c)
{
	glDisable(GL_SCISSOR_TEST);
	if (static_cast<GSTextureOGL*>(t)->IsBackbuffer()) {
		OMSetFBO(0);

		// glDrawBuffer(GL_BACK); // this is the default when there is no FB
		// 0 will select the first drawbuffer ie GL_BACK
		gl_ClearBufferfv(GL_COLOR, 0, c.v);
	} else {
		OMSetFBO(m_fbo);
		OMSetWriteBuffer();
		OMAttachRt(t);

		gl_ClearBufferfv(GL_COLOR, 0, c.v);
	}
	glEnable(GL_SCISSOR_TEST);
}

void GSDeviceOGL::ClearRenderTarget(GSTexture* t, uint32 c)
{
	GSVector4 color = GSVector4::rgba32(c) * (1.0f / 255);
	ClearRenderTarget(t, color);
}

void GSDeviceOGL::ClearRenderTarget_ui(GSTexture* t, uint32 c)
{
	uint32 col[4] = {c, c, c, c};

	glDisable(GL_SCISSOR_TEST);

	OMSetFBO(m_fbo);
	OMSetWriteBuffer();
	OMAttachRt(t);

	gl_ClearBufferuiv(GL_COLOR, 0, col);

	glEnable(GL_SCISSOR_TEST);
}

void GSDeviceOGL::ClearDepth(GSTexture* t, float c)
{
	OMSetFBO(m_fbo);
	OMSetWriteBuffer();
	OMAttachDs(t);

	glDisable(GL_SCISSOR_TEST);
	if (m_state.dss != NULL && m_state.dss->IsMaskEnable()) {
		gl_ClearBufferfv(GL_DEPTH, 0, &c);
	} else {
		glDepthMask(true);
		gl_ClearBufferfv(GL_DEPTH, 0, &c);
		glDepthMask(false);
	}
	glEnable(GL_SCISSOR_TEST);
}

void GSDeviceOGL::ClearStencil(GSTexture* t, uint8 c)
{
	OMSetFBO(m_fbo);
	OMSetWriteBuffer();
	OMAttachDs(t);
	GLint color = c;

	glDisable(GL_SCISSOR_TEST);
	gl_ClearBufferiv(GL_STENCIL, 0, &color);
	glEnable(GL_SCISSOR_TEST);
}

GLuint GSDeviceOGL::CreateSampler(bool bilinear, bool tau, bool tav)
{
	GLuint sampler;
	gl_GenSamplers(1, &sampler);
	if (bilinear) {
		gl_SamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		gl_SamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	} else {
		gl_SamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		gl_SamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	}

	// FIXME ensure U -> S,  V -> T and W->R
	if (tau)
		gl_SamplerParameteri(sampler, GL_TEXTURE_WRAP_S, GL_REPEAT);
	else
		gl_SamplerParameteri(sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	if (tav)
		gl_SamplerParameteri(sampler, GL_TEXTURE_WRAP_T, GL_REPEAT);
	else
		gl_SamplerParameteri(sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	gl_SamplerParameteri(sampler, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

	// FIXME which value for GL_TEXTURE_MIN_LOD
	gl_SamplerParameterf(sampler, GL_TEXTURE_MAX_LOD, FLT_MAX);

	// FIXME: seems there is 2 possibility in opengl
	// DX: sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
	// gl_SamplerParameteri(sampler, GL_TEXTURE_COMPARE_MODE, GL_NONE);
	gl_SamplerParameteri(sampler, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
	gl_SamplerParameteri(sampler, GL_TEXTURE_COMPARE_FUNC, GL_NEVER);
	// FIXME: need ogl extension sd.MaxAnisotropy = 16;

	return sampler;
}

void GSDeviceOGL::InitPrimDateTexture(int w, int h)
{
	// Create a texture to avoid the useless clean@0
	if (m_date.t == NULL)
		m_date.t = CreateTexture(w, h, GL_R32UI);

	ClearRenderTarget_ui(m_date.t, 0xFFFFFFFF);

#ifdef ENABLE_OGL_STENCIL_DEBUG
	static_cast<GSTextureOGL*>(m_date.t)->EnableUnit(6);
#endif

	gl_BindImageTexture(0, static_cast<GSTextureOGL*>(m_date.t)->GetID(), 0, false, 0, GL_READ_WRITE, GL_R32UI);
}

void GSDeviceOGL::RecycleDateTexture()
{
	if (m_date.t) {
#ifdef ENABLE_OGL_STENCIL_DEBUG
		//static_cast<GSTextureOGL*>(m_date.t)->Save(format("/tmp/date_adv_%04ld.csv", g_draw_count));
#endif

		// FIXME invalidate data
		Recycle(m_date.t);
		m_date.t = NULL;
	}
}

GLuint GSDeviceOGL::CompileVS(VSSelector sel)
{
	std::string macro = format("#define VS_BPPZ %d\n", sel.bppz)
		+ format("#define VS_LOGZ %d\n", sel.logz)
		+ format("#define VS_TME %d\n", sel.tme)
		+ format("#define VS_FST %d\n", sel.fst);

	return m_shader->Compile("tfx.glsl", "vs_main", GL_VERTEX_SHADER, tfx_glsl, macro);
}

GLuint GSDeviceOGL::CompileGS(GSSelector sel)
{
	// Easy case
	if(! (sel.prim > 0 && (sel.iip == 0 || sel.prim == 3)))
		return 0;

	std::string macro = format("#define GS_IIP %d\n", sel.iip)
		+ format("#define GS_PRIM %d\n", sel.prim);

#ifdef ENABLE_GLES
	return 0;
#else
	return m_shader->Compile("tfx.glsl", "gs_main", GL_GEOMETRY_SHADER, tfx_glsl, macro);
#endif
}

GLuint GSDeviceOGL::CompilePS(PSSelector sel)
{
	std::string macro = format("#define PS_FST %d\n", sel.fst)
		+ format("#define PS_WMS %d\n", sel.wms)
		+ format("#define PS_WMT %d\n", sel.wmt)
		+ format("#define PS_FMT %d\n", sel.fmt)
		+ format("#define PS_AEM %d\n", sel.aem)
		+ format("#define PS_TFX %d\n", sel.tfx)
		+ format("#define PS_TCC %d\n", sel.tcc)
		+ format("#define PS_ATST %d\n", sel.atst)
		+ format("#define PS_FOG %d\n", sel.fog)
		+ format("#define PS_CLR1 %d\n", sel.clr1)
		+ format("#define PS_FBA %d\n", sel.fba)
		+ format("#define PS_AOUT %d\n", sel.aout)
		+ format("#define PS_LTF %d\n", sel.ltf)
		+ format("#define PS_COLCLIP %d\n", sel.colclip)
		+ format("#define PS_DATE %d\n", sel.date)
		+ format("#define PS_SPRITEHACK %d\n", sel.spritehack)
		+ format("#define PS_TCOFFSETHACK %d\n", sel.tcoffsethack)
		+ format("#define PS_POINT_SAMPLER %d\n", sel.point_sampler);

	return m_shader->Compile("tfx.glsl", "ps_main", GL_FRAGMENT_SHADER, tfx_glsl, macro);
}

GSTexture* GSDeviceOGL::CreateRenderTarget(int w, int h, bool msaa, int format)
{
	return GSDevice::CreateRenderTarget(w, h, msaa, format ? format : GL_RGBA8);
}

GSTexture* GSDeviceOGL::CreateDepthStencil(int w, int h, bool msaa, int format)
{
	return GSDevice::CreateDepthStencil(w, h, msaa, format ? format : GL_DEPTH32F_STENCIL8);
}

GSTexture* GSDeviceOGL::CreateTexture(int w, int h, int format)
{
	return GSDevice::CreateTexture(w, h, format ? format : GL_RGBA8);
}

GSTexture* GSDeviceOGL::CreateOffscreen(int w, int h, int format)
{
	return GSDevice::CreateOffscreen(w, h, format ? format : GL_RGBA8);
}

// blit a texture into an offscreen buffer
GSTexture* GSDeviceOGL::CopyOffscreen(GSTexture* src, const GSVector4& sr, int w, int h, int format)
{
	ASSERT(src);
	ASSERT(format == GL_RGBA8 || format == GL_R16UI);

	if(format == 0) format = GL_RGBA8;

	if(format != GL_RGBA8 && format != GL_R16UI) return NULL;

	GSTexture* dst = CreateOffscreen(w, h, format);
	GSVector4 dr(0, 0, w, h);

	StretchRect(src, sr, dst, dr, m_convert.ps[format == GL_R16UI ? 1 : 0]);

	return dst;
}

// Copy a sub part of a texture into another
// Several question to answer did texture have same size?
// From a sub-part to the same sub-part
// From a sub-part to a full texture
void GSDeviceOGL::CopyRect(GSTexture* st, GSTexture* dt, const GSVector4i& r)
{
	ASSERT(st && dt);

	// FIXME: the extension was integrated in opengl 4.3 (now we need driver that support OGL4.3)
	if (GLLoader::found_GL_NV_copy_image) {
#ifndef ENABLE_GLES
		gl_CopyImageSubDataNV( static_cast<GSTextureOGL*>(st)->GetID(), GL_TEXTURE_2D,
				0, r.x, r.y, 0,
				static_cast<GSTextureOGL*>(dt)->GetID(), GL_TEXTURE_2D,
				0, r.x, r.y, 0,
				r.width(), r.height(), 1);
#endif
	} else if (GLLoader::found_GL_ARB_copy_image) {
		// Would need an update of GL definition. For the moment it isn't supported by driver anyway.
#if 0
		gl_CopyImageSubData( static_cast<GSTextureOGL*>(st)->GetID(), GL_TEXTURE_2D,
				0, r.x, r.y, 0,
				static_cast<GSTextureOGL*>(dt)->GetID(), GL_TEXTURE_2D,
				0, r.x, r.y, 0,
				r.width(), r.height(), 1);
#endif
	} else {

		GSTextureOGL* st_ogl = (GSTextureOGL*) st;
		GSTextureOGL* dt_ogl = (GSTextureOGL*) dt;

		gl_BindFramebuffer(GL_READ_FRAMEBUFFER, m_fbo_read);

		gl_FramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, static_cast<GSTextureOGL*>(st_ogl)->GetID(), 0);
		glReadBuffer(GL_COLOR_ATTACHMENT0);

		dt_ogl->EnableUnit(6);
		glCopyTexSubImage2D(GL_TEXTURE_2D, 0, r.x, r.y, r.x, r.y, r.width(), r.height());

		gl_BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	}
}

void GSDeviceOGL::StretchRect(GSTexture* st, const GSVector4& sr, GSTexture* dt, const GSVector4& dr, int shader, bool linear)
{
	StretchRect(st, sr, dt, dr, m_convert.ps[shader], linear);
}

void GSDeviceOGL::StretchRect(GSTexture* st, const GSVector4& sr, GSTexture* dt, const GSVector4& dr, GLuint ps, bool linear)
{
	StretchRect(st, sr, dt, dr, ps, m_convert.bs, linear);
}

void GSDeviceOGL::StretchRect(GSTexture* st, const GSVector4& sr, GSTexture* dt, const GSVector4& dr, GLuint ps, GSBlendStateOGL* bs, bool linear)
{

	if(!st || !dt)
	{
		ASSERT(0);
		return;
	}

	// ************************************
	// Init
	// ************************************

	BeginScene();

	GSVector2i ds = dt->GetSize();

	// ************************************
	// om
	// ************************************

	OMSetDepthStencilState(m_convert.dss, 0);
	OMSetBlendState(bs, 0);
	OMSetRenderTargets(dt, NULL);

	// ************************************
	// ia
	// ************************************


	// Original code from DX
	float left = dr.x * 2 / ds.x - 1.0f;
	float right = dr.z * 2 / ds.x - 1.0f;
#if 0
	float top = 1.0f - dr.y * 2 / ds.y;
	float bottom = 1.0f - dr.w * 2 / ds.y;
#else
	// Opengl get some issues with the coordinate
	// I flip top/bottom to fix scaling of the internal resolution
	float top = -1.0f + dr.y * 2 / ds.y;
	float bottom = -1.0f + dr.w * 2 / ds.y;
#endif

	// Flip y axis only when we render in the backbuffer
	// By default everything is render in the wrong order (ie dx).
	// 1/ consistency between several pass rendering (interlace)
	// 2/ in case some GSdx code expect thing in dx order.
	// Only flipping the backbuffer is transparent (I hope)...
	GSVector4 flip_sr = sr;
	if (static_cast<GSTextureOGL*>(dt)->IsBackbuffer()) {
		flip_sr.y = sr.w;
		flip_sr.w = sr.y;
	}

	GSVertexPT1 vertices[] =
	{
		{GSVector4(left, top, 0.5f, 1.0f), GSVector2(flip_sr.x, flip_sr.y)},
		{GSVector4(right, top, 0.5f, 1.0f), GSVector2(flip_sr.z, flip_sr.y)},
		{GSVector4(left, bottom, 0.5f, 1.0f), GSVector2(flip_sr.x, flip_sr.w)},
		{GSVector4(right, bottom, 0.5f, 1.0f), GSVector2(flip_sr.z, flip_sr.w)},
	};
	//fprintf(stderr, "A:%fx%f B:%fx%f\n", left, top, bottom, right);
	//fprintf(stderr, "SR: %f %f %f %f\n", sr.x, sr.y, sr.z, sr.w);

	IASetVertexState(m_vb_sr);
	IASetVertexBuffer(vertices, 4);
	IASetPrimitiveTopology(GL_TRIANGLE_STRIP);

	// ************************************
	// vs
	// ************************************

	m_shader->VS(m_convert.vs);

	// ************************************
	// gs
	// ************************************

	m_shader->GS(0);

	// ************************************
	// ps
	// ************************************

	PSSetShaderResource(0, st);
	PSSetSamplerState(0, linear ? m_convert.ln : m_convert.pt);
	m_shader->PS(ps);

	// ************************************
	// Draw
	// ************************************
	DrawPrimitive();

	// ************************************
	// End
	// ************************************

	EndScene();
}

void GSDeviceOGL::DoMerge(GSTexture* st[2], GSVector4* sr, GSTexture* dt, GSVector4* dr, bool slbg, bool mmod, const GSVector4& c)
{
	ClearRenderTarget(dt, c);

	if(st[1] && !slbg)
	{
		StretchRect(st[1], sr[1], dt, dr[1], m_merge_obj.ps[0]);
	}

	if(st[0])
	{
		SetUniformBuffer(m_merge_obj.cb);
		m_merge_obj.cb->upload(&c.v);

		StretchRect(st[0], sr[0], dt, dr[0], m_merge_obj.ps[mmod ? 1 : 0], m_merge_obj.bs);
	}
}

void GSDeviceOGL::DoInterlace(GSTexture* st, GSTexture* dt, int shader, bool linear, float yoffset)
{
	GSVector4 s = GSVector4(dt->GetSize());

	GSVector4 sr(0, 0, 1, 1);
	GSVector4 dr(0.0f, yoffset, s.x, s.y + yoffset);

	InterlaceConstantBuffer cb;

	cb.ZrH = GSVector2(0, 1.0f / s.y);
	cb.hH = s.y / 2;

	SetUniformBuffer(m_interlace.cb);
	m_interlace.cb->upload(&cb);

	StretchRect(st, sr, dt, dr, m_interlace.ps[shader], linear);
}

void GSDeviceOGL::DoFXAA(GSTexture* st, GSTexture* dt)
{
	GSVector2i s = dt->GetSize();

	GSVector4 sr(0, 0, 1, 1);
	GSVector4 dr(0, 0, s.x, s.y);

	FXAAConstantBuffer cb;

	// FIXME optimize: remove rcpFrameOpt. And reduce rcpFrame to vec2
	cb.rcpFrame = GSVector4(1.0f / s.x, 1.0f / s.y, 0.0f, 0.0f);
	cb.rcpFrameOpt = GSVector4::zero();

	SetUniformBuffer(m_fxaa.cb);
	m_fxaa.cb->upload(&cb);

	StretchRect(st, sr, dt, dr, m_fxaa.ps, true);
}

void GSDeviceOGL::DoShadeBoost(GSTexture* st, GSTexture* dt)
{
	GSVector2i s = dt->GetSize();

	GSVector4 sr(0, 0, 1, 1);
	GSVector4 dr(0, 0, s.x, s.y);

	ShadeBoostConstantBuffer cb;

	cb.rcpFrame = GSVector4(1.0f / s.x, 1.0f / s.y, 0.0f, 0.0f);
	cb.rcpFrameOpt = GSVector4::zero();

	SetUniformBuffer(m_shadeboost.cb);
	m_shadeboost.cb->upload(&cb);

	StretchRect(st, sr, dt, dr, m_shadeboost.ps, true);
}

void GSDeviceOGL::SetupDATE(GSTexture* rt, GSTexture* ds, const GSVertexPT1* vertices, bool datm)
{
#ifdef ENABLE_OGL_STENCIL_DEBUG
	const GSVector2i& size = rt->GetSize();
	GSTexture* t = CreateRenderTarget(size.x, size.y, false);
#else
	GSTexture* t = NULL;
#endif
	// sfex3 (after the capcom logo), vf4 (first menu fading in), ffxii shadows, rumble roses shadows, persona4 shadows

	BeginScene();

	ClearStencil(ds, 0);

	// om

	OMSetDepthStencilState(m_date.dss, 1);
	OMSetBlendState(m_date.bs, 0);
	OMSetRenderTargets(t, ds);

	// ia

	IASetVertexState(m_vb_sr);
	IASetVertexBuffer(vertices, 4);
	IASetPrimitiveTopology(GL_TRIANGLE_STRIP);

	// vs

	m_shader->VS(m_convert.vs);

	// gs

	m_shader->GS(0);

	// ps

	PSSetShaderResource(0, rt);
	PSSetSamplerState(0, m_convert.pt);
	m_shader->PS(m_convert.ps[datm ? 2 : 3]);

	//

	DrawPrimitive();

	//

	EndScene();

#ifdef ENABLE_OGL_STENCIL_DEBUG
	// FIXME invalidate data
	Recycle(t);
#endif
}

void GSDeviceOGL::EndScene()
{
	m_state.vb->EndScene();
}

void GSDeviceOGL::SetUniformBuffer(GSUniformBufferOGL* cb)
{
	if (m_state.cb != cb) {
		 m_state.cb = cb;
		 cb->bind();
	}
}

void GSDeviceOGL::IASetVertexState(GSVertexBufferStateOGL* vb)
{
	if (vb == NULL) vb = m_vb;

	if (m_state.vb != vb) {
		m_state.vb = vb;
		vb->bind();
	}
}

void GSDeviceOGL::IASetVertexBuffer(const void* vertices, size_t count)
{
	m_state.vb->UploadVB(vertices, count);
}

bool GSDeviceOGL::IAMapVertexBuffer(void** vertex, size_t stride, size_t count)
{
	return m_state.vb->MapVB(vertex, count);
}

void GSDeviceOGL::IAUnmapVertexBuffer()
{
	m_state.vb->UnmapVB();
}

void GSDeviceOGL::IASetIndexBuffer(const void* index, size_t count)
{
	m_state.vb->UploadIB(index, count);
}

void GSDeviceOGL::IASetPrimitiveTopology(GLenum topology)
{
	m_state.vb->SetTopology(topology);
}

void GSDeviceOGL::PSSetShaderResource(const int i, GSTexture* sr)
{
	ASSERT(sr);

	static_cast<GSTextureOGL*>(sr)->EnableUnit(i);
}

void GSDeviceOGL::PSSetSamplerState(const int i, GLuint ss)
{
	if (m_state.ps_ss[i] != ss) {
		m_state.ps_ss[i] = ss;
		gl_BindSampler(i, ss);
	}
}

void GSDeviceOGL::OMAttachRt(GSTexture* rt)
{
	if (m_state.rt != rt) {
		m_state.rt = rt;
		gl_FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, static_cast<GSTextureOGL*>(rt)->GetID(), 0);
	}
}

void GSDeviceOGL::OMAttachDs(GSTexture* ds)
{
	if (m_state.ds != ds) {
		m_state.ds = ds;
		gl_FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, static_cast<GSTextureOGL*>(ds)->GetID(), 0);
	}
}

void GSDeviceOGL::OMSetFBO(GLuint fbo)
{
	if (m_state.fbo != fbo) {
		m_state.fbo = fbo;
		gl_BindFramebuffer(GL_FRAMEBUFFER, fbo);
	}
}

void GSDeviceOGL::OMSetWriteBuffer(GLenum buffer)
{
	// Note if fbo is 0, standard GL_BACK will be used instead
	if (m_state.fbo && m_state.draw != buffer) {
		m_state.draw = buffer;

		GLenum target[1] = {buffer};
		gl_DrawBuffers(1, target);
	}
}

void GSDeviceOGL::OMSetDepthStencilState(GSDepthStencilOGL* dss, uint8 sref)
{
	if (m_state.dss != dss) {
		m_state.dss = dss;

		dss->SetupDepth();
		dss->SetupStencil();
	}
}

void GSDeviceOGL::OMSetBlendState(GSBlendStateOGL* bs, float bf)
{
	if ( m_state.bs != bs || (m_state.bf != bf && bs->HasConstantFactor()) )
	{
		m_state.bs = bs;
		m_state.bf = bf;

		bs->SetupBlend(bf);
	}
}

void GSDeviceOGL::OMSetRenderTargets(GSTexture* rt, GSTexture* ds, const GSVector4i* scissor)
{
	if (rt == NULL || !static_cast<GSTextureOGL*>(rt)->IsBackbuffer()) {
		if (rt) {
			OMSetFBO(m_fbo);
			OMSetWriteBuffer();
			OMAttachRt(rt);
		} else {
			// Note: NULL rt is only used in DATE so far. Color writing is disabled
			// on the blend setup
			OMSetFBO(m_fbo);
			OMSetWriteBuffer(GL_NONE);
		}

		// Note: it must be done after OMSetFBO
		if (ds)
			OMAttachDs(ds);

	} else {
		// Render in the backbuffer
		OMSetFBO(0);
	}



	GSVector2i size = rt ? rt->GetSize() : ds->GetSize();
	if(m_state.viewport != size)
	{
		m_state.viewport = size;
		glViewport(0, 0, size.x, size.y);
	}

	GSVector4i r = scissor ? *scissor : GSVector4i(size).zwxy();

	if(!m_state.scissor.eq(r))
	{
		m_state.scissor = r;
		glScissor( r.x, r.y, r.width(), r.height() );
	}
}

void GSDeviceOGL::CheckDebugLog()
{
#ifndef ENABLE_GLES
       unsigned int count = 16; // max. num. of messages that will be read from the log
       int bufsize = 2048;
	   unsigned int sources[16] = {};
	   unsigned int types[16] = {};
	   unsigned int ids[16]   = {};
	   unsigned int severities[16] = {};
	   int lengths[16] = {};
       char* messageLog = new char[bufsize];

       unsigned int retVal = gl_GetDebugMessageLogARB(count, bufsize, sources, types, ids, severities, lengths, messageLog);

       if(retVal > 0)
       {
             unsigned int pos = 0;
             for(unsigned int i=0; i<retVal; i++)
             {
                    DebugOutputToFile(sources[i], types[i], ids[i], severities[i],
 &messageLog[pos]);
                    pos += lengths[i];
              }
       }

	   delete[] messageLog;
#endif
}

void GSDeviceOGL::DebugOutputToFile(unsigned int source, unsigned int type, unsigned int id, unsigned int severity, const char* message)
{
#ifndef ENABLE_GLES
	char debType[20], debSev[5];
	static int sev_counter = 0;

	if(type == GL_DEBUG_TYPE_ERROR_ARB)
		strcpy(debType, "Error");
	else if(type == GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR_ARB)
		strcpy(debType, "Deprecated behavior");
	else if(type == GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR_ARB)
		strcpy(debType, "Undefined behavior");
	else if(type == GL_DEBUG_TYPE_PORTABILITY_ARB)
		strcpy(debType, "Portability");
	else if(type == GL_DEBUG_TYPE_PERFORMANCE_ARB)
		strcpy(debType, "Performance");
	else if(type == GL_DEBUG_TYPE_OTHER_ARB)
		strcpy(debType, "Other");
	else
		strcpy(debType, "UNKNOWN");

	if(severity == GL_DEBUG_SEVERITY_HIGH_ARB) {
		strcpy(debSev, "High");
		sev_counter++;
	}
	else if(severity == GL_DEBUG_SEVERITY_MEDIUM_ARB)
		strcpy(debSev, "Med");
	else if(severity == GL_DEBUG_SEVERITY_LOW_ARB)
		strcpy(debSev, "Low");

	#ifdef LOUD_DEBUGGING
	fprintf(stderr,"Type:%s\tID:%d\tSeverity:%s\tMessage:%s\n", debType, g_draw_count, debSev,message);
	#endif

	FILE* f = fopen("Debug.txt","a");
	if(f)
	{
		fprintf(f,"Type:%s\tID:%d\tSeverity:%s\tMessage:%s\n", debType, g_draw_count, debSev,message);
		fclose(f);
	}
	//if (sev_counter > 2) assert(0);
#endif
}

// (A - B) * C + D
// A: Cs/Cd/0
// B: Cs/Cd/0
// C: As/Ad/FIX
// D: Cs/Cd/0

// bogus: 0100, 0110, 0120, 0200, 0210, 0220, 1001, 1011, 1021
// tricky: 1201, 1211, 1221

// Source.rgb = float3(1, 1, 1);
// 1201 Cd*(1 + As) => Source * Dest color + Dest * Source alpha
// 1211 Cd*(1 + Ad) => Source * Dest color + Dest * Dest alpha
// 1221 Cd*(1 + F) => Source * Dest color + Dest * Factor

// Copy Dx blend table and convert it to ogl
#define D3DBLENDOP_ADD			GL_FUNC_ADD
#define D3DBLENDOP_SUBTRACT		GL_FUNC_SUBTRACT
#define D3DBLENDOP_REVSUBTRACT	GL_FUNC_REVERSE_SUBTRACT

#define D3DBLEND_ONE			GL_ONE
#define D3DBLEND_ZERO			GL_ZERO
#define D3DBLEND_INVDESTALPHA	GL_ONE_MINUS_DST_ALPHA
#define D3DBLEND_DESTALPHA		GL_DST_ALPHA
#define D3DBLEND_DESTCOLOR		GL_DST_COLOR
#define D3DBLEND_BLENDFACTOR	GL_CONSTANT_COLOR
#define D3DBLEND_INVBLENDFACTOR GL_ONE_MINUS_CONSTANT_COLOR

#ifdef ENABLE_GLES
#define D3DBLEND_SRCALPHA		GL_SRC_ALPHA
#define D3DBLEND_INVSRCALPHA	GL_ONE_MINUS_SRC_ALPHA
#else
#define D3DBLEND_SRCALPHA		GL_SRC1_ALPHA
#define D3DBLEND_INVSRCALPHA	GL_ONE_MINUS_SRC1_ALPHA
#endif
                        
const GSDeviceOGL::D3D9Blend GSDeviceOGL::m_blendMapD3D9[3*3*3*3] =
{
	{0, D3DBLENDOP_ADD, D3DBLEND_ONE, D3DBLEND_ZERO},						// 0000: (Cs - Cs)*As + Cs ==> Cs
	{0, D3DBLENDOP_ADD, D3DBLEND_ZERO, D3DBLEND_ONE},						// 0001: (Cs - Cs)*As + Cd ==> Cd
	{0, D3DBLENDOP_ADD, D3DBLEND_ZERO, D3DBLEND_ZERO},						// 0002: (Cs - Cs)*As +  0 ==> 0
	{0, D3DBLENDOP_ADD, D3DBLEND_ONE, D3DBLEND_ZERO},						// 0010: (Cs - Cs)*Ad + Cs ==> Cs
	{0, D3DBLENDOP_ADD, D3DBLEND_ZERO, D3DBLEND_ONE},						// 0011: (Cs - Cs)*Ad + Cd ==> Cd
	{0, D3DBLENDOP_ADD, D3DBLEND_ZERO, D3DBLEND_ZERO},						// 0012: (Cs - Cs)*Ad +  0 ==> 0
	{0, D3DBLENDOP_ADD, D3DBLEND_ONE, D3DBLEND_ZERO},						// 0020: (Cs - Cs)*F  + Cs ==> Cs
	{0, D3DBLENDOP_ADD, D3DBLEND_ZERO, D3DBLEND_ONE},						// 0021: (Cs - Cs)*F  + Cd ==> Cd
	{0, D3DBLENDOP_ADD, D3DBLEND_ZERO, D3DBLEND_ZERO},						// 0022: (Cs - Cs)*F  +  0 ==> 0
	{1, D3DBLENDOP_SUBTRACT, D3DBLEND_SRCALPHA, D3DBLEND_SRCALPHA},			//*0100: (Cs - Cd)*As + Cs ==> Cs*(As + 1) - Cd*As
	{0, D3DBLENDOP_ADD, D3DBLEND_SRCALPHA, D3DBLEND_INVSRCALPHA},			// 0101: (Cs - Cd)*As + Cd ==> Cs*As + Cd*(1 - As)
	{0, D3DBLENDOP_SUBTRACT, D3DBLEND_SRCALPHA, D3DBLEND_SRCALPHA},			// 0102: (Cs - Cd)*As +  0 ==> Cs*As - Cd*As
	{1, D3DBLENDOP_SUBTRACT, D3DBLEND_DESTALPHA, D3DBLEND_DESTALPHA},		//*0110: (Cs - Cd)*Ad + Cs ==> Cs*(Ad + 1) - Cd*Ad
	{0, D3DBLENDOP_ADD, D3DBLEND_DESTALPHA, D3DBLEND_INVDESTALPHA},			// 0111: (Cs - Cd)*Ad + Cd ==> Cs*Ad + Cd*(1 - Ad)
	{0, D3DBLENDOP_SUBTRACT, D3DBLEND_DESTALPHA, D3DBLEND_DESTALPHA},		// 0112: (Cs - Cd)*Ad +  0 ==> Cs*Ad - Cd*Ad
	{1, D3DBLENDOP_SUBTRACT, D3DBLEND_BLENDFACTOR, D3DBLEND_BLENDFACTOR},	//*0120: (Cs - Cd)*F  + Cs ==> Cs*(F + 1) - Cd*F
	{0, D3DBLENDOP_ADD, D3DBLEND_BLENDFACTOR, D3DBLEND_INVBLENDFACTOR},		// 0121: (Cs - Cd)*F  + Cd ==> Cs*F + Cd*(1 - F)
	{0, D3DBLENDOP_SUBTRACT, D3DBLEND_BLENDFACTOR, D3DBLEND_BLENDFACTOR},	// 0122: (Cs - Cd)*F  +  0 ==> Cs*F - Cd*F
	{1, D3DBLENDOP_ADD, D3DBLEND_SRCALPHA, D3DBLEND_ZERO},					//*0200: (Cs -  0)*As + Cs ==> Cs*(As + 1)
	{0, D3DBLENDOP_ADD, D3DBLEND_SRCALPHA, D3DBLEND_ONE},					// 0201: (Cs -  0)*As + Cd ==> Cs*As + Cd
	{0, D3DBLENDOP_ADD, D3DBLEND_SRCALPHA, D3DBLEND_ZERO},					// 0202: (Cs -  0)*As +  0 ==> Cs*As
	{1, D3DBLENDOP_ADD, D3DBLEND_DESTALPHA, D3DBLEND_ZERO},					//*0210: (Cs -  0)*Ad + Cs ==> Cs*(Ad + 1)
	{0, D3DBLENDOP_ADD, D3DBLEND_DESTALPHA, D3DBLEND_ONE},					// 0211: (Cs -  0)*Ad + Cd ==> Cs*Ad + Cd
	{0, D3DBLENDOP_ADD, D3DBLEND_DESTALPHA, D3DBLEND_ZERO},					// 0212: (Cs -  0)*Ad +  0 ==> Cs*Ad
	{1, D3DBLENDOP_ADD, D3DBLEND_BLENDFACTOR, D3DBLEND_ZERO},				//*0220: (Cs -  0)*F  + Cs ==> Cs*(F + 1)
	{0, D3DBLENDOP_ADD, D3DBLEND_BLENDFACTOR, D3DBLEND_ONE},				// 0221: (Cs -  0)*F  + Cd ==> Cs*F + Cd
	{0, D3DBLENDOP_ADD, D3DBLEND_BLENDFACTOR, D3DBLEND_ZERO},				// 0222: (Cs -  0)*F  +  0 ==> Cs*F
	{0, D3DBLENDOP_ADD, D3DBLEND_INVSRCALPHA, D3DBLEND_SRCALPHA},			// 1000: (Cd - Cs)*As + Cs ==> Cd*As + Cs*(1 - As)
	{1, D3DBLENDOP_REVSUBTRACT, D3DBLEND_SRCALPHA, D3DBLEND_SRCALPHA},		//*1001: (Cd - Cs)*As + Cd ==> Cd*(As + 1) - Cs*As
	{0, D3DBLENDOP_REVSUBTRACT, D3DBLEND_SRCALPHA, D3DBLEND_SRCALPHA},		// 1002: (Cd - Cs)*As +  0 ==> Cd*As - Cs*As
	{0, D3DBLENDOP_ADD, D3DBLEND_INVDESTALPHA, D3DBLEND_DESTALPHA},			// 1010: (Cd - Cs)*Ad + Cs ==> Cd*Ad + Cs*(1 - Ad)
	{1, D3DBLENDOP_REVSUBTRACT, D3DBLEND_DESTALPHA, D3DBLEND_DESTALPHA},	//*1011: (Cd - Cs)*Ad + Cd ==> Cd*(Ad + 1) - Cs*Ad
	{0, D3DBLENDOP_REVSUBTRACT, D3DBLEND_DESTALPHA, D3DBLEND_DESTALPHA},	// 1012: (Cd - Cs)*Ad +  0 ==> Cd*Ad - Cs*Ad
	{0, D3DBLENDOP_ADD, D3DBLEND_INVBLENDFACTOR, D3DBLEND_BLENDFACTOR},		// 1020: (Cd - Cs)*F  + Cs ==> Cd*F + Cs*(1 - F)
	{1, D3DBLENDOP_REVSUBTRACT, D3DBLEND_BLENDFACTOR, D3DBLEND_BLENDFACTOR},//*1021: (Cd - Cs)*F  + Cd ==> Cd*(F + 1) - Cs*F
	{0, D3DBLENDOP_REVSUBTRACT, D3DBLEND_BLENDFACTOR, D3DBLEND_BLENDFACTOR},// 1022: (Cd - Cs)*F  +  0 ==> Cd*F - Cs*F
	{0, D3DBLENDOP_ADD, D3DBLEND_ONE, D3DBLEND_ZERO},						// 1100: (Cd - Cd)*As + Cs ==> Cs
	{0, D3DBLENDOP_ADD, D3DBLEND_ZERO, D3DBLEND_ONE},						// 1101: (Cd - Cd)*As + Cd ==> Cd
	{0, D3DBLENDOP_ADD, D3DBLEND_ZERO, D3DBLEND_ZERO},						// 1102: (Cd - Cd)*As +  0 ==> 0
	{0, D3DBLENDOP_ADD, D3DBLEND_ONE, D3DBLEND_ZERO},						// 1110: (Cd - Cd)*Ad + Cs ==> Cs
	{0, D3DBLENDOP_ADD, D3DBLEND_ZERO, D3DBLEND_ONE},						// 1111: (Cd - Cd)*Ad + Cd ==> Cd
	{0, D3DBLENDOP_ADD, D3DBLEND_ZERO, D3DBLEND_ZERO},						// 1112: (Cd - Cd)*Ad +  0 ==> 0
	{0, D3DBLENDOP_ADD, D3DBLEND_ONE, D3DBLEND_ZERO},						// 1120: (Cd - Cd)*F  + Cs ==> Cs
	{0, D3DBLENDOP_ADD, D3DBLEND_ZERO, D3DBLEND_ONE},						// 1121: (Cd - Cd)*F  + Cd ==> Cd
	{0, D3DBLENDOP_ADD, D3DBLEND_ZERO, D3DBLEND_ZERO},						// 1122: (Cd - Cd)*F  +  0 ==> 0
	{0, D3DBLENDOP_ADD, D3DBLEND_ONE, D3DBLEND_SRCALPHA},					// 1200: (Cd -  0)*As + Cs ==> Cs + Cd*As
	{2, D3DBLENDOP_ADD, D3DBLEND_DESTCOLOR, D3DBLEND_SRCALPHA},				//#1201: (Cd -  0)*As + Cd ==> Cd*(1 + As)  // ffxii main menu background glow effect
	{0, D3DBLENDOP_ADD, D3DBLEND_ZERO, D3DBLEND_SRCALPHA},					// 1202: (Cd -  0)*As +  0 ==> Cd*As
	{0, D3DBLENDOP_ADD, D3DBLEND_ONE, D3DBLEND_DESTALPHA},					// 1210: (Cd -  0)*Ad + Cs ==> Cs + Cd*Ad
	{2, D3DBLENDOP_ADD, D3DBLEND_DESTCOLOR, D3DBLEND_DESTALPHA},			//#1211: (Cd -  0)*Ad + Cd ==> Cd*(1 + Ad)
	{0, D3DBLENDOP_ADD, D3DBLEND_ZERO, D3DBLEND_DESTALPHA},					// 1212: (Cd -  0)*Ad +  0 ==> Cd*Ad
	{0, D3DBLENDOP_ADD, D3DBLEND_ONE, D3DBLEND_BLENDFACTOR},				// 1220: (Cd -  0)*F  + Cs ==> Cs + Cd*F
	{2, D3DBLENDOP_ADD, D3DBLEND_DESTCOLOR, D3DBLEND_BLENDFACTOR},			//#1221: (Cd -  0)*F  + Cd ==> Cd*(1 + F)
	{0, D3DBLENDOP_ADD, D3DBLEND_ZERO, D3DBLEND_BLENDFACTOR},				// 1222: (Cd -  0)*F  +  0 ==> Cd*F
	{0, D3DBLENDOP_ADD, D3DBLEND_INVSRCALPHA, D3DBLEND_ZERO},				// 2000: (0  - Cs)*As + Cs ==> Cs*(1 - As)
	{0, D3DBLENDOP_REVSUBTRACT, D3DBLEND_SRCALPHA, D3DBLEND_ONE},			// 2001: (0  - Cs)*As + Cd ==> Cd - Cs*As
	{0, D3DBLENDOP_REVSUBTRACT, D3DBLEND_SRCALPHA, D3DBLEND_ZERO},			// 2002: (0  - Cs)*As +  0 ==> 0 - Cs*As
	{0, D3DBLENDOP_ADD, D3DBLEND_INVDESTALPHA, D3DBLEND_ZERO},				// 2010: (0  - Cs)*Ad + Cs ==> Cs*(1 - Ad)
	{0, D3DBLENDOP_REVSUBTRACT, D3DBLEND_DESTALPHA, D3DBLEND_ONE},			// 2011: (0  - Cs)*Ad + Cd ==> Cd - Cs*Ad
	{0, D3DBLENDOP_REVSUBTRACT, D3DBLEND_DESTALPHA, D3DBLEND_ZERO},			// 2012: (0  - Cs)*Ad +  0 ==> 0 - Cs*Ad
	{0, D3DBLENDOP_ADD, D3DBLEND_INVBLENDFACTOR, D3DBLEND_ZERO},			// 2020: (0  - Cs)*F  + Cs ==> Cs*(1 - F)
	{0, D3DBLENDOP_REVSUBTRACT, D3DBLEND_BLENDFACTOR, D3DBLEND_ONE},		// 2021: (0  - Cs)*F  + Cd ==> Cd - Cs*F
	{0, D3DBLENDOP_REVSUBTRACT, D3DBLEND_BLENDFACTOR, D3DBLEND_ZERO},		// 2022: (0  - Cs)*F  +  0 ==> 0 - Cs*F
	{0, D3DBLENDOP_SUBTRACT, D3DBLEND_ONE, D3DBLEND_SRCALPHA},				// 2100: (0  - Cd)*As + Cs ==> Cs - Cd*As
	{0, D3DBLENDOP_ADD, D3DBLEND_ZERO, D3DBLEND_INVSRCALPHA},				// 2101: (0  - Cd)*As + Cd ==> Cd*(1 - As)
	{0, D3DBLENDOP_SUBTRACT, D3DBLEND_ZERO, D3DBLEND_SRCALPHA},				// 2102: (0  - Cd)*As +  0 ==> 0 - Cd*As
	{0, D3DBLENDOP_SUBTRACT, D3DBLEND_ONE, D3DBLEND_DESTALPHA},				// 2110: (0  - Cd)*Ad + Cs ==> Cs - Cd*Ad
	{0, D3DBLENDOP_ADD, D3DBLEND_ZERO, D3DBLEND_INVDESTALPHA},				// 2111: (0  - Cd)*Ad + Cd ==> Cd*(1 - Ad)
	{0, D3DBLENDOP_SUBTRACT, D3DBLEND_ONE, D3DBLEND_DESTALPHA},				// 2112: (0  - Cd)*Ad +  0 ==> 0 - Cd*Ad
	{0, D3DBLENDOP_SUBTRACT, D3DBLEND_ONE, D3DBLEND_BLENDFACTOR},			// 2120: (0  - Cd)*F  + Cs ==> Cs - Cd*F
	{0, D3DBLENDOP_ADD, D3DBLEND_ZERO, D3DBLEND_INVBLENDFACTOR},			// 2121: (0  - Cd)*F  + Cd ==> Cd*(1 - F)
	{0, D3DBLENDOP_SUBTRACT, D3DBLEND_ONE, D3DBLEND_BLENDFACTOR},			// 2122: (0  - Cd)*F  +  0 ==> 0 - Cd*F
	{0, D3DBLENDOP_ADD, D3DBLEND_ONE, D3DBLEND_ZERO},						// 2200: (0  -  0)*As + Cs ==> Cs
	{0, D3DBLENDOP_ADD, D3DBLEND_ZERO, D3DBLEND_ONE},						// 2201: (0  -  0)*As + Cd ==> Cd
	{0, D3DBLENDOP_ADD, D3DBLEND_ZERO, D3DBLEND_ZERO},						// 2202: (0  -  0)*As +  0 ==> 0
	{0, D3DBLENDOP_ADD, D3DBLEND_ONE, D3DBLEND_ZERO},						// 2210: (0  -  0)*Ad + Cs ==> Cs
	{0, D3DBLENDOP_ADD, D3DBLEND_ZERO, D3DBLEND_ONE},						// 2211: (0  -  0)*Ad + Cd ==> Cd
	{0, D3DBLENDOP_ADD, D3DBLEND_ZERO, D3DBLEND_ZERO},						// 2212: (0  -  0)*Ad +  0 ==> 0
	{0, D3DBLENDOP_ADD, D3DBLEND_ONE, D3DBLEND_ZERO},						// 2220: (0  -  0)*F  + Cs ==> Cs
	{0, D3DBLENDOP_ADD, D3DBLEND_ZERO, D3DBLEND_ONE},						// 2221: (0  -  0)*F  + Cd ==> Cd
	{0, D3DBLENDOP_ADD, D3DBLEND_ZERO, D3DBLEND_ZERO},						// 2222: (0  -  0)*F  +  0 ==> 0
};
