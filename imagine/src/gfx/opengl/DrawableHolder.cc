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

#define LOGTAG "GLDrawableHolder"
#include <imagine/gfx/DrawableHolder.hh>
#include <imagine/gfx/Renderer.hh>
#include <imagine/gfx/RendererTask.hh>
#include <imagine/base/Screen.hh>
#include <imagine/base/Window.hh>
#include <imagine/base/Base.hh>
#include <imagine/logger/logger.h>

#ifndef GL_BACK_LEFT
#define GL_BACK_LEFT 0x0402
#endif

#ifndef GL_BACK_RIGHT
#define GL_BACK_RIGHT 0x0403
#endif

namespace Gfx
{

DrawableHolder::DrawableHolder(DrawableHolder &&o)
{
	*this = std::move(o);
}

DrawableHolder &DrawableHolder::operator=(DrawableHolder &&o)
{
	assert(!o.drawable_); // TODO: update delegates in makeDrawable() to support moving
	destroyDrawable();
	GLDrawableHolder::operator=(std::move(o));
	o.drawable_ = {};
	return *this;
}

GLDrawableHolder::~GLDrawableHolder()
{
	destroyDrawable();
}

DrawableHolder::operator Drawable() const
{
	return drawable_;
}

DrawableHolder::operator bool() const
{
	return (bool)drawable_;
}

bool DrawableHolder::addOnFrame(Base::OnFrameDelegate del, int priority)
{
	return onFrame.add(del, priority);
}

bool DrawableHolder::removeOnFrame(Base::OnFrameDelegate del)
{
	return onFrame.remove(del);
}

void DrawableHolder::dispatchOnFrame()
{
	auto now = IG::steadyClockTimestamp();
	FrameParams frameParams{now, screen->frameTime()};
	onFrame.runAll([&](Base::OnFrameDelegate del){ return del(frameParams); });
}

void GLDrawableHolder::makeDrawable(RendererTask &rTask, Base::Window &win)
{
	destroyDrawable();
	auto &r = rTask.renderer();
	task = &rTask;
	screen = win.screen();
	auto dpy = r.glDpy;
	auto [ec, drawable] = dpy.makeDrawable(win, r.gfxBufferConfig);
	if(ec)
	{
		logErr("Error creating GL drawable");
		return;
	}
	drawable_ = drawable;
	onResume =
		[drawable = drawable](bool focused) mutable
		{
			drawable.restoreCaches();
			return true;
		};
	Base::addOnResume(onResume, Base::RENDERER_DRAWABLE_ON_RESUME_PRIORITY);
	onExit =
		[this, dpy](bool backgrounded) mutable
		{
			if(backgrounded)
			{
				drawFinishedEvent.cancel();
				drawable_.freeCaches();
			}
			else
				drawable_.destroy(dpy);
			return true;
		};
	Base::addOnExit(onExit, Base::RENDERER_DRAWABLE_ON_EXIT_PRIORITY);
	drawFinishedEvent.attach(
		[this]()
		{
			if(!onFrame.size())
				return;
			static_cast<DrawableHolder*>(this)->dispatchOnFrame();
		});
	if(r.support.hasDrawReadBuffers())
	{
		rTask.run([glCtx = rTask.glContext(), drawable = drawable,
			&support = std::as_const(r.support)](GLTask::TaskContext ctx)
		{
			Base::GLContext::setDrawable(ctx.glDisplay(), drawable, glCtx);
			//logMsg("specifying draw/read buffers");
			const GLenum back = Config::Gfx::OPENGL_ES ? GL_BACK : GL_BACK_LEFT;
			support.glDrawBuffers(1, &back);
			support.glReadBuffer(GL_BACK);
		});
	}
}

void GLDrawableHolder::destroyDrawable()
{
	if(!drawable_)
		return;
	task->run([drawable = std::exchange(drawable_, {})](GLTask::TaskContext ctx)
	{
		// destroy drawable on GL thread in case it's currently being used
		IG::copySelf(drawable).destroy(ctx.glDisplay());
	});
	Base::removeOnExit(onResume);
	Base::removeOnExit(onExit);
	drawFinishedEvent.detach();
}

void GLDrawableHolder::notifyOnFrame()
{
	if(onFrame.size())
	{
		drawFinishedEvent.notify();
	}
}

}
