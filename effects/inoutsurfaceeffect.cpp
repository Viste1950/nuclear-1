/*
 * Copyright 2013  Giulio Camuffo <giuliocamuffo@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "inoutsurfaceeffect.h"
#include "animation.h"
#include "shellsurface.h"

const int ALPHA_ANIM_DURATION = 200;

struct InOutSurfaceEffect::Surface {
    weston_surface *surface;
    Animation animation;
    InOutSurfaceEffect *effect;
    struct Listener {
        struct wl_listener destroyListener;
        Surface *parent;
    } listener;

    void setAlpha(float alpha)
    {
        surface->alpha = alpha;
        weston_surface_damage(surface);
    }
    void done()
    {
        weston_surface_destroy(surface);
        effect->m_surfaces.remove(this);
        delete this;
    }
    static void destroyed(struct wl_listener *listener, void *data)
    {
        Surface *surf = container_of(listener, Listener, destroyListener)->parent;

        surf->animation.setStart(surf->surface->alpha);
        surf->animation.setTarget(0);
        surf->animation.run(surf->surface->output, ALPHA_ANIM_DURATION, Animation::Flags::SendDone);
    }
};

InOutSurfaceEffect::InOutSurfaceEffect(Shell *shell)
                  : Effect(shell)
{
}

InOutSurfaceEffect::~InOutSurfaceEffect()
{
    for (auto i = m_surfaces.begin(); i != m_surfaces.end(); ++i) {
        weston_surface_destroy((*i)->surface);
        delete *i;
        m_surfaces.erase(i);
    }
}

void InOutSurfaceEffect::addedSurface(ShellSurface *surface)
{
    Surface *surf = new Surface;
    surf->surface = surface->weston_surface();
    surf->effect = this;

    ++surface->weston_surface()->ref_count;
    surf->listener.parent = surf;
    surf->listener.destroyListener.notify = Surface::destroyed;
    wl_resource_add_destroy_listener(surf->surface->resource, &surf->listener.destroyListener);

    surf->animation.updateSignal->connect(surf, &Surface::setAlpha);
    surf->animation.doneSignal->connect(surf, &Surface::done);
    m_surfaces.push_back(surf);

    surf->animation.setStart(0);
    surf->animation.setTarget(1);
    surf->animation.run(surface->output(), ALPHA_ANIM_DURATION);
}
