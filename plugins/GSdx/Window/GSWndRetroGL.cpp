/*
 *	Copyright (C) 2007-2012 Gabest
 *	http://www.gabest.org
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

#ifdef __LIBRETRO__
#include "../stdafx.h"
#include "GSWndRetroGL.h"
#include <libretro.h>


extern struct retro_hw_render_callback hw_render;
extern retro_video_refresh_t video_cb;
extern retro_environment_t environ_cb;

// static method
int GSWndRetroGL::SelectPlatform()
{
	return 0;
}


GSWndRetroGL::GSWndRetroGL()
{
}

void GSWndRetroGL::CreateContext(int major, int minor)
{
}

void GSWndRetroGL::AttachContext()
{
	m_ctx_attached = true;
}

void GSWndRetroGL::DetachContext()
{
	m_ctx_attached = false;
}

void GSWndRetroGL::PopulateWndGlFunction()
{
}

void GSWndRetroGL::BindAPI()
{
}

bool GSWndRetroGL::Attach(void* handle, bool managed)
{
	m_managed = managed;
	return true;
}

void GSWndRetroGL::Detach()
{
	DetachContext();
	DestroyNativeResources();
}

bool GSWndRetroGL::Create(const std::string& title, int w, int h)
{
	m_managed = true;
	FullContextInit();
	return true;
}

void* GSWndRetroGL::GetProcAddress(const char* name, bool opt)
{
	void* ptr = (void*)hw_render.get_proc_address(name);
	if (ptr == nullptr) {
		if (theApp.GetConfigB("debug_opengl"))
			fprintf(stderr, "Failed to find %s\n", name);

		if (!opt)
			throw GSDXRecoverableError();
	}
	return ptr;
}

GSVector4i GSWndRetroGL::GetClientRect()
{
//	return GSVector4i(0, 0, 640, 480);
	return GSVector4i(0, 0, 1280, 896);
}

void GSWndRetroGL::SetSwapInterval()
{
}

void GSWndRetroGL::Flip()
{
//	video_cb(NULL, 0, 0, 0);
//	video_cb(RETRO_HW_FRAME_BUFFER_VALID, 640, 480, 0);
	video_cb(RETRO_HW_FRAME_BUFFER_VALID, 1280, 896, 0);
}


void* GSWndRetroGL::CreateNativeDisplay()
{
	return nullptr;
}

void* GSWndRetroGL::CreateNativeWindow(int w, int h)
{
	return nullptr;
}

void* GSWndRetroGL::AttachNativeWindow(void* handle)
{
	return handle;
}

void GSWndRetroGL::DestroyNativeResources()
{
}

bool GSWndRetroGL::SetWindowText(const char* title)
{
	return true;
}

#endif
