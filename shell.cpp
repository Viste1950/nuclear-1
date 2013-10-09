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

#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <linux/input.h>
#include <string.h>

#include <wayland-server.h>

#include <weston/compositor.h>

#include "shell.h"
#include "shellsurface.h"
#include "shellseat.h"
#include "workspace.h"
#include "effect.h"
#include "desktop-shell.h"
#include "animation.h"

class Splash {
public:
    Splash() {}
    void addOutput(weston_surface *s)
    {
        Animation *a = new Animation;
        splash spl{s, a};
        splashes.push_back(spl);
        a->updateSignal->connect(&splashes.back(), &splash::setAlpha);
    }
    void fadeOut()
    {
        for (splash &s: splashes) {
            s.fadeAnimation->setStart(1.f);
            s.fadeAnimation->setTarget(0.f);
            s.fadeAnimation->run(s.surface->output, 200);
        }
    }

private:
    struct splash {
        weston_surface *surface;
        Animation *fadeAnimation;

        void setAlpha(float a)
        {
            surface->alpha = a;
            weston_surface_geometry_dirty(surface);
            weston_surface_damage(surface);
        }
    };
    std::list<splash> splashes;
};

ShellGrab::ShellGrab()
         : m_shell(nullptr)
         , m_pointer(nullptr)
{
    m_grab.base.interface = &s_shellGrabInterface;
    m_grab.parent = this;
}

ShellGrab::~ShellGrab()
{
    end();
}

void ShellGrab::end()
{
    if (m_pointer) {
        weston_pointer_end_grab(m_pointer);
        m_pointer = nullptr;
    }
}

ShellGrab *ShellGrab::fromGrab(weston_pointer_grab *grab)
{
    Grab *wrapper = container_of(grab, Grab, base);
    return wrapper->parent;
}

const weston_pointer_grab_interface ShellGrab::s_shellGrabInterface = {
    [](weston_pointer_grab *base)                                                 { ShellGrab::fromGrab(base)->focus(); },
    [](weston_pointer_grab *base, uint32_t time)                                  { ShellGrab::fromGrab(base)->motion(time); },
    [](weston_pointer_grab *base, uint32_t time, uint32_t button, uint32_t state) { ShellGrab::fromGrab(base)->button(time, button, state); }
};

Binding::Binding(struct weston_binding *binding)
       : m_binding(binding)
{

}

Binding::~Binding()
{
    weston_binding_destroy(m_binding);
}


Shell::Shell(struct weston_compositor *ec)
            : m_compositor(ec)
            , m_windowsMinimized(false)
            , m_grabSurface(nullptr)
{
    srandom(weston_compositor_get_time());
    m_child.shell = this;
    m_child.deathstamp = 0;
}

Shell::~Shell()
{
    free(m_clientPath);
    if (m_child.client)
        wl_client_destroy(m_child.client);
}

void Shell::destroy()
{
    delete this;
}

static void
black_surface_configure(struct weston_surface *es, int32_t sx, int32_t sy, int32_t width, int32_t height)
{
}

void Shell::init()
{
#define _this reinterpret_cast<ShellSurface *>(shsurf)
    m_compositor->shell_interface.shell = this;
    m_compositor->shell_interface.create_shell_surface = [](void *shell, weston_surface *surface, const weston_shell_client *client) {
        return (shell_surface *)static_cast<Shell *>(shell)->createShellSurface(surface, client);
    };
    m_compositor->shell_interface.set_toplevel = [](shell_surface *shsurf) { _this->setTopLevel(); };
    m_compositor->shell_interface.set_transient = [](shell_surface *shsurf, weston_surface *parent, int x, int y, uint32_t flags) { _this->setTransient(parent, x, y, flags); };
    m_compositor->shell_interface.set_fullscreen = [](shell_surface *shsurf, uint32_t method, uint32_t framerate, weston_output *output) { _this->setFullscreen(method, framerate, output);};
    m_compositor->shell_interface.resize = [](shell_surface *shsurf, weston_seat *ws, uint32_t edges) { _this->dragResize(ws, edges); return 0; };
    m_compositor->shell_interface.move = [](shell_surface *shsurf, weston_seat *ws) { _this->dragMove(ws); return 0; };
    m_compositor->shell_interface.set_xwayland = [](shell_surface *shsurf, int x, int y, uint32_t flags) { _this->setXWayland(x, y, flags); };
    m_compositor->shell_interface.set_title = [](shell_surface *shsurf, const char *t) { _this->setTitle(t); };
#undef _this

    m_destroyListener.listen(&m_compositor->destroy_signal);
    m_destroyListener.signal->connect(this, &Shell::destroy);

    if (!wl_global_create(m_compositor->wl_display, &wl_shell_interface, 1, this,
        [](struct wl_client *client, void *data, uint32_t version, uint32_t id) { static_cast<Shell *>(data)->bind(client, version, id); }))
        return;

    struct weston_seat *seat;
    wl_list_for_each(seat, &m_compositor->seat_list, link) {
        ShellSeat *shseat = ShellSeat::shellSeat(seat);
        shseat->pointerFocusSignal.connect(this, &Shell::pointerFocus);
    }

    m_splashLayer.insert(&m_compositor->cursor_layer);
    m_overlayLayer.insert(&m_splashLayer);
    m_fullscreenLayer.insert(&m_overlayLayer);
    m_panelsLayer.insert(&m_fullscreenLayer);
    m_backgroundLayer.insert(&m_panelsLayer);

    m_currentWorkspace = 0;
    m_splash = new Splash;

    struct weston_output *out;
    wl_list_for_each(out, &m_compositor->output_list, link) {
        int x = out->x, y = out->y;
        int w = out->width, h = out->height;

        weston_surface *blackSurface = createBlackSurface(x, y, w, h);
        m_backgroundLayer.addSurface(blackSurface);
        m_blackSurfaces.push_back(blackSurface);

        weston_surface *splashSurface = createBlackSurface(x, y, w, h);
        m_splashLayer.addSurface(splashSurface);
        m_splash->addOutput(splashSurface);
    }

    struct wl_event_loop *loop = wl_display_get_event_loop(m_compositor->wl_display);
    wl_event_loop_add_idle(loop, [](void *data) { static_cast<Shell *>(data)->launchShellProcess(); }, this);

    weston_compositor_add_button_binding(compositor(), BTN_LEFT, (weston_keyboard_modifier)0,
                                         [](struct weston_seat *seat, uint32_t time, uint32_t button, void *data) {
                                             static_cast<Shell *>(data)->activateSurface(seat, time, button); }, this);

    bindKey(KEY_LEFT, MODIFIER_CTRL, [](struct weston_seat *seat, uint32_t time, uint32_t key, void *data) {
                                            static_cast<Shell *>(data)->selectPreviousWorkspace(); }, this);
    bindKey(KEY_RIGHT, MODIFIER_CTRL, [](struct weston_seat *seat, uint32_t time, uint32_t key, void *data) {
                                            static_cast<Shell *>(data)->selectNextWorkspace(); }, this);
}

void Shell::addWorkspace(Workspace *ws)
{
    m_workspaces.push_back(ws);
    ws->destroyedSignal.connect(this, &Shell::workspaceRemoved);
    if (ws->number() == 0) {
        activateWorkspace(nullptr);
    }
}

void Shell::workspaceRemoved(Workspace *ws)
{
    for (auto i = m_workspaces.begin(); i != m_workspaces.end(); ++i) {
        if (*i == ws) {
            m_workspaces.erase(i);
            break;
        }
    }

    int nextWs = ws->number();
    if (nextWs >= (int)m_workspaces.size()) {
        nextWs = m_workspaces.size() - 1;
    }
    if (ws->active()) {
        m_currentWorkspace = nextWs;
    }

    for (const weston_surface *s: ws->layer()) {
        ShellSurface *shsurf = Shell::getShellSurface(s);
        if (!shsurf)
            continue;

        workspace(nextWs)->addSurface(shsurf);
        shsurf->m_workspace = workspace(nextWs);
    }

    activateWorkspace(nullptr);
}

weston_surface *Shell::createBlackSurface(int x, int y, int w, int h)
{
    weston_surface *surface = weston_surface_create(m_compositor);

    surface->configure = black_surface_configure;
    surface->configure_private = 0;
    weston_surface_configure(surface, x, y, w, h);
    weston_surface_set_color(surface, 0.0, 0.0, 0.0, 1);
    pixman_region32_fini(&surface->input);
    pixman_region32_init_rect(&surface->input, 0, 0, 0, 0);

    return surface;
}

void Shell::quit()
{
    wl_display_terminate(compositor()->wl_display);
}

void Shell::configureSurface(ShellSurface *surface, int32_t sx, int32_t sy, int32_t width, int32_t height)
{
    if (width == 0) {
        surface->unmapped();
        return;
    }

    if (surface->m_type == ShellSurface::Type::Fullscreen && surface->m_pendingType != ShellSurface::Type::Fullscreen &&
        surface->m_pendingType != ShellSurface::Type::None) {
        if (surface->m_fullscreen.type == WL_SHELL_SURFACE_FULLSCREEN_METHOD_DRIVER && surfaceIsTopFullscreen(surface)) {
            weston_output_switch_mode(surface->m_fullscreen.output, surface->m_fullscreen.output->original_mode, surface->m_fullscreen.output->original_scale, WESTON_MODE_SWITCH_RESTORE_NATIVE);
        }
    }
    bool changedType = surface->updateType();

    if (!surface->isMapped()) {
        switch (surface->m_type) {
            case ShellSurface::Type::TopLevel:
                surface->map(10 + random() % 400, 10 + random() % 400, width, height);
                break;
            default:
                surface->map(surface->m_surface->geometry.x + sx, surface->m_surface->geometry.y + sy, width, height);
        }

        for (Effect *e: m_effects) {
            e->addSurface(surface);
        }

        switch (surface->m_type) {
            case ShellSurface::Type::Transient:
            case ShellSurface::Type::Popup:
                if (ShellSurface *p = getShellSurface(surface->m_parent)) {
                    surface->m_workspace = p->m_workspace;
                } else {
                    surface->m_workspace = 0;
                }
                surface->m_surface->output = surface->m_parent->output;
                wl_list_insert(surface->m_parent->layer_link.prev, &surface->m_surface->layer_link);
                break;
            case ShellSurface::Type::Fullscreen:
                stackFullscreen(surface);
                configureFullscreen(surface);
                break;
            case ShellSurface::Type::None:
                break;
            default:
                surface->m_workspace->addSurface(surface);
                m_surfaces.push_back(surface);
        }

        switch (surface->m_type) {
            case ShellSurface::Type::XWayland:
            case ShellSurface::Type::Transient:
                if (surface->m_transient.flags == WL_SHELL_SURFACE_TRANSIENT_INACTIVE) {
                    break;
                }
            case ShellSurface::Type::TopLevel:
            case ShellSurface::Type::Maximized: {
                struct weston_seat *seat;
                wl_list_for_each(seat, &m_compositor->seat_list, link) {
                    ShellSeat::shellSeat(seat)->activate(surface);
                }
            } break;
            default:
                break;
        }

        if (m_windowsMinimized) {
            switch (surface->m_type) {
                case ShellSurface::Type::Transient:
                case ShellSurface::Type::Popup:
                    if (surface->m_transient.flags == WL_SHELL_SURFACE_TRANSIENT_INACTIVE) {
                        break;
                    }
                default:
                    surface->hide();
            }
        }
    } else if (changedType || sx != 0 || sy != 0 || surface->width() != width || surface->height() != height) {
        float from_x, from_y;
        float to_x, to_y;

        struct weston_surface *es = surface->m_surface;
        weston_surface_to_global_float(es, 0, 0, &from_x, &from_y);
        weston_surface_to_global_float(es, sx, sy, &to_x, &to_y);
        int x = surface->x() + to_x - from_x;
        int y = surface->y() + to_y - from_y;

        weston_surface_configure(es, x, y, width, height);

        switch (surface->m_type) {
            case ShellSurface::Type::Fullscreen:
                stackFullscreen(surface);
                configureFullscreen(surface);
                break;
            case ShellSurface::Type::Maximized: {
                IRect2D rect = windowsArea(surface->output());
                IRect2D bbox = surface->surfaceTreeBoundingBox();
                weston_surface_set_position(surface->m_surface, rect.x - bbox.x, rect.y - bbox.y);
            } break;
            default:
                if (!m_windowsMinimized) {
                    surface->m_workspace->addSurface(surface);
                }
                break;
        }

        if (surface->m_surface->output) {
            weston_surface_update_transform(surface->m_surface);

            if (surface->m_type == ShellSurface::Type::Maximized) {
                surface->m_surface->output = surface->m_output;
            }
        }
    }
}

struct weston_surface *Shell::createBlackSurface(ShellSurface *fs_surface, float x, float y, int w, int h)
{
    struct weston_surface *surface = weston_surface_create(m_compositor);
    if (!surface) {
        weston_log("no memory\n");
        return nullptr;
    }

    surface->configure = black_surface_configure;
    surface->configure_private = fs_surface;
    weston_surface_configure(surface, x, y, w, h);
    weston_surface_set_color(surface, 0.0, 0.0, 0.0, 1);
    pixman_region32_fini(&surface->opaque);
    pixman_region32_init_rect(&surface->opaque, 0, 0, w, h);
    pixman_region32_fini(&surface->input);
    pixman_region32_init_rect(&surface->input, 0, 0, w, h);

    return surface;
}

void Shell::configureFullscreen(ShellSurface *shsurf)
{
    struct weston_output *output = shsurf->m_fullscreen.output;
    struct weston_surface *surface = shsurf->m_surface;

    if (!shsurf->m_fullscreen.blackSurface) {
        shsurf->m_fullscreen.blackSurface = createBlackSurface(shsurf, output->x, output->y, output->width, output->height);
    }

    m_fullscreenLayer.stackBelow(shsurf->m_fullscreen.blackSurface, shsurf->m_surface);

    switch (shsurf->m_fullscreen.type) {
    case WL_SHELL_SURFACE_FULLSCREEN_METHOD_DEFAULT:
        if (surface->buffer_ref.buffer) {
            shsurf->centerOnOutput(shsurf->m_fullscreen.output);
        } break;
    case WL_SHELL_SURFACE_FULLSCREEN_METHOD_SCALE: {
        /* 1:1 mapping between surface and output dimensions */
        if (output->width == surface->geometry.width && output->height == surface->geometry.height) {
            weston_surface_set_position(surface, output->x, output->y);
            break;
        }

        struct weston_matrix *matrix = &shsurf->m_fullscreen.transform.matrix;
        weston_matrix_init(matrix);

        float output_aspect = (float) output->width / (float) output->height;
        float surface_aspect = (float) surface->geometry.width / (float) surface->geometry.height;
        float scale;
        if (output_aspect < surface_aspect) {
            scale = (float) output->width / (float) surface->geometry.width;
        } else {
            scale = (float) output->height / (float) surface->geometry.height;
        }

        weston_matrix_scale(matrix, scale, scale, 1);
        shsurf->addTransform(&shsurf->m_fullscreen.transform);
        float x = output->x + (output->width - surface->geometry.width * scale) / 2;
        float y = output->y + (output->height - surface->geometry.height * scale) / 2;
        weston_surface_set_position(surface, x, y);

    } break;
    case WL_SHELL_SURFACE_FULLSCREEN_METHOD_DRIVER:
        if (surfaceIsTopFullscreen(shsurf)) {
            struct weston_mode mode = {0, surface->geometry.width * surface->buffer_scale, surface->geometry.height * surface->buffer_scale, shsurf->m_fullscreen.framerate, { nullptr, nullptr } };

            if (weston_output_switch_mode(output, &mode, surface->buffer_scale, WESTON_MODE_SWITCH_SET_TEMPORARY) == 0) {
                weston_surface_configure(shsurf->m_fullscreen.blackSurface, output->x, output->y, output->width, output->height);
                weston_surface_set_position(surface, output->x, output->y);
                break;
            }
        }
        break;
    case WL_SHELL_SURFACE_FULLSCREEN_METHOD_FILL:
        break;
    default:
        break;
    }
}

void Shell::stackFullscreen(ShellSurface *shsurf)
{
    m_fullscreenLayer.addSurface(shsurf);
    weston_surface_damage(shsurf->m_surface);

    if (!shsurf->m_fullscreen.blackSurface) {
        struct weston_output *output = shsurf->m_fullscreen.output;
        shsurf->m_fullscreen.blackSurface = createBlackSurface(shsurf, output->x, output->y, output->width, output->height);
    }

    m_fullscreenLayer.stackBelow(shsurf->m_fullscreen.blackSurface, shsurf->m_surface);
    weston_surface_damage(shsurf->m_fullscreen.blackSurface);
}

bool Shell::surfaceIsTopFullscreen(ShellSurface *surface)
{
    if (m_fullscreenLayer.isEmpty()) {
        return false;
    }

    struct weston_surface *top = *m_fullscreenLayer.begin();
    return top == surface->m_surface;
}

bool Shell::isInFullscreen() const
{
    return m_fullscreenLayer.isVisible();
}

static void shell_surface_configure(struct weston_surface *surf, int32_t sx, int32_t sy, int32_t w, int32_t h)
{
    ShellSurface *shsurf = Shell::getShellSurface(surf);
    if (shsurf) {
        shsurf->shell()->configureSurface(shsurf, sx, sy, w, h);
    }
}

ShellSurface *Shell::createShellSurface(struct weston_surface *surface, const struct weston_shell_client *client)
{
    ShellSurface *shsurf = new ShellSurface(this, surface);

    surface->configure = shell_surface_configure;
    surface->configure_private = shsurf;
    shsurf->m_client = client;
    shsurf->m_workspace = currentWorkspace();
    return shsurf;
}

void Shell::sendConfigure(struct weston_surface *surface, uint32_t edges, int32_t width, int32_t height)
{
    ShellSurface *shsurf = Shell::getShellSurface(surface);
    wl_shell_surface_send_configure(shsurf->m_resource, edges, width, height);
}

const struct weston_shell_client Shell::shell_client = {
    Shell::sendConfigure
};

ShellSurface *Shell::getShellSurface(struct wl_client *client, struct wl_resource *resource, uint32_t id,
                                     struct wl_resource *surface_resource)
{
    struct weston_surface *surface = static_cast<weston_surface *>(wl_resource_get_user_data(surface_resource));

    ShellSurface *shsurf = getShellSurface(surface);
    if (shsurf) {
        wl_resource_post_error(surface_resource,
                               WL_DISPLAY_ERROR_INVALID_OBJECT,
                               "desktop_shell::get_shell_surface already requested");
        return shsurf;
    }

    shsurf = createShellSurface(surface, &shell_client);
    if (!shsurf) {
        wl_resource_post_error(surface_resource,
                               WL_DISPLAY_ERROR_INVALID_OBJECT,
                               "surface->configure already set");
        return nullptr;
    }

    shsurf->init(client, id);
    shsurf->pingTimeoutSignal.connect(this, &Shell::pingTimeout);

    return shsurf;
}

ShellSurface *Shell::getShellSurface(const struct weston_surface *surf)
{
    if (surf->configure == shell_surface_configure) {
        return static_cast<ShellSurface *>(surf->configure_private);
    }

    return nullptr;
}

void Shell::removeShellSurface(ShellSurface *surface)
{
    for (Effect *e: m_effects) {
        e->removeSurface(surface);
    }
    m_surfaces.remove(surface);
}

Binding *Shell::bindKey(uint32_t key, enum weston_keyboard_modifier modifier, weston_key_binding_handler_t handler, void *data)
{
    return new Binding(weston_compositor_add_key_binding(compositor(), key, modifier, handler, data));
}

void Shell::registerEffect(Effect *effect)
{
    m_effects.push_back(effect);
    for (ShellSurface *s: m_surfaces) {
        effect->addSurface(s);
    }
}

void Shell::activateSurface(struct weston_seat *seat, uint32_t time, uint32_t button)
{
    struct weston_surface *focus = (struct weston_surface *) seat->pointer->focus;
    if (!focus)
        return;

    if (seat->pointer->grab == &seat->pointer->default_grab) {
        ShellSurface *shsurf = getShellSurface(focus);
        if (shsurf && shsurf->type() == ShellSurface::Type::Fullscreen) {
            return;
        }

        ShellSeat *shseat = ShellSeat::shellSeat(seat);
        if (shsurf) {
            shseat->activate(shsurf);
        } else {
            // Dont't activate the nullptr ShellSurface, as that would call setActive(false) on the previous
            // active ShellSurface, and that would break minimizing windows by clicking on the taskbar:
            // a click on the taskbar would deactivate the previous active surface, and the taskbar would
            // activate instead of minimizing it.
            weston_surface_activate(focus, seat);
        }
    };
}

void Shell::startGrab(ShellGrab *grab, weston_seat *seat, int32_t cursor)
{
    ShellSeat::shellSeat(seat)->endPopupGrab();

    grab->m_shell = this;
    weston_pointer *pointer = seat->pointer;
    grab->m_pointer = pointer;

    weston_pointer_start_grab(pointer, &grab->m_grab.base);
    if (cursor != -1) {
        setGrabCursor(cursor);
        weston_pointer_set_focus(pointer, m_grabSurface, wl_fixed_from_int(0), wl_fixed_from_int(0));
    }
}

static void configure_static_surface(struct weston_surface *es, Layer *layer, int32_t width, int32_t height)
{
    if (width == 0)
        return;

    weston_surface_configure(es, es->geometry.x, es->geometry.y, width, height);

    if (wl_list_empty(&es->layer_link) || es->layer_link.next == es->layer_link.prev) {
        layer->addSurface(es);
        weston_compositor_schedule_repaint(es->compositor);
    }
}

void Shell::backgroundConfigure(struct weston_surface *es, int32_t sx, int32_t sy, int32_t width, int32_t height)
{
    configure_static_surface(es, &m_backgroundLayer, width, height);
}

void Shell::panelConfigure(struct weston_surface *es, int32_t sx, int32_t sy, int32_t width, int32_t height)
{
    configure_static_surface(es, &static_cast<Shell *>(es->configure_private)->m_panelsLayer, width, height);
}

void Shell::setBackgroundSurface(struct weston_surface *surface, struct weston_output *output)
{
    surface->configure = [](struct weston_surface *es, int32_t sx, int32_t sy, int32_t width, int32_t height) {
        static_cast<Shell *>(es->configure_private)->backgroundConfigure(es, sx, sy, width, height); };
    surface->configure_private = this;
    surface->output = output;
    weston_surface_set_position(surface, output->x, output->y);
}

void Shell::setGrabSurface(struct weston_surface *surface)
{
    m_grabSurface = surface;
}

void Shell::addPanelSurface(struct weston_surface *surface, struct weston_output *output)
{
    surface->configure = [](struct weston_surface *es, int32_t sx, int32_t sy, int32_t width, int32_t height) {
        static_cast<Shell *>(es->configure_private)->panelConfigure(es, sx, sy, width, height); };
    surface->configure_private = this;
    surface->output = output;
    weston_surface_set_position(surface, output->x, output->y);
}

void Shell::addOverlaySurface(struct weston_surface *surface, struct weston_output *output)
{
    surface->configure = [](struct weston_surface *es, int32_t sx, int32_t sy, int32_t width, int32_t height) {
        configure_static_surface(es, &static_cast<Shell *>(es->configure_private)->m_overlayLayer, width, height); };
    surface->configure_private = this;
    surface->output = output;
    weston_surface_set_position(surface, output->x, output->y);
}

void Shell::showPanels()
{
    m_panelsLayer.show();
}

void Shell::hidePanels()
{
    m_panelsLayer.hide();
}

IRect2D Shell::windowsArea(struct weston_output *output) const
{
    int panelsHeight = 0;
    for (weston_surface *surface: m_panelsLayer) {
        if (surface->output == output) {
            pixman_box32_t *extents = pixman_region32_extents(&surface->input);
            panelsHeight = extents->y2 - extents->y1;
        }
    }
    return IRect2D(output->x, output->y + panelsHeight, output->width, output->height - panelsHeight);
}

struct weston_output *Shell::getDefaultOutput() const
{
    return container_of(m_compositor->output_list.next, struct weston_output, link);
}

Workspace *Shell::currentWorkspace() const
{
    return m_workspaces[m_currentWorkspace];
}

Workspace *Shell::workspace(uint32_t id) const
{
    if (id >= m_workspaces.size()) {
        return nullptr;
    }

    return m_workspaces[id];
}

void Shell::selectPreviousWorkspace()
{
    Workspace *old = currentWorkspace();
    int i = m_currentWorkspace;
    if (--i < 0) {
        i = m_workspaces.size() - 1;
    }
    m_currentWorkspace = i;
    activateWorkspace(old);
}

void Shell::selectNextWorkspace()
{
    Workspace *old = currentWorkspace();
    if (++m_currentWorkspace == m_workspaces.size()) {
        m_currentWorkspace = 0;
    }
    activateWorkspace(old);
}

void Shell::selectWorkspace(int32_t id)
{
    if (id >= (int32_t)m_workspaces.size()) {
        return;
    }

    Workspace *old = currentWorkspace();

    if (id < 0) {
        old->remove();
        return;
    }
    m_currentWorkspace = id;
    activateWorkspace(old);
}

void Shell::activateWorkspace(Workspace *old)
{
    if (old) {
        old->setActive(false);
        old->remove();
    }

    currentWorkspace()->setActive(true);
    currentWorkspace()->insert(&m_panelsLayer);

    for (const weston_surface *surf: currentWorkspace()->layer()) {
        ShellSurface *shsurf = getShellSurface(surf);
        if (shsurf) {
            weston_seat *seat;
            wl_list_for_each(seat, &m_compositor->seat_list, link) {
                ShellSeat::shellSeat(seat)->activate(shsurf);
            }
            return;
        }
    }
    weston_seat *seat;
    wl_list_for_each(seat, &m_compositor->seat_list, link) {
        ShellSeat::shellSeat(seat)->activate((weston_surface *)nullptr);
    }
}

uint32_t Shell::numWorkspaces() const
{
    return m_workspaces.size();
}

void Shell::showAllWorkspaces()
{
    currentWorkspace()->remove();

    Workspace *prev = nullptr;
    for (Workspace *w: m_workspaces) {
        if (prev) {
            w->insert(prev);
        } else {
            w->insert(&m_panelsLayer);
        }
        prev = w;
    }
}

void Shell::resetWorkspaces()
{
    for (Workspace *w: m_workspaces) {
        w->remove();
    }
    activateWorkspace(nullptr);
}

void Shell::minimizeWindows()
{
    for (ShellSurface *shsurf: surfaces()) {
        switch (shsurf->m_type) {
            case ShellSurface::Type::Transient:
            case ShellSurface::Type::Popup:
                if (shsurf->m_transient.flags == WL_SHELL_SURFACE_TRANSIENT_INACTIVE) {
                    break;
                }
            default:
                if (!shsurf->isMinimized()) {
                    shsurf->minimize();
                }
                shsurf->setAcceptNewState(false);
        }
    }
    m_windowsMinimized = true;
}

void Shell::restoreWindows()
{
    for (ShellSurface *shsurf: surfaces()) {
        switch (shsurf->m_type) {
            case ShellSurface::Type::Transient:
            case ShellSurface::Type::Popup:
                if (shsurf->m_transient.flags == WL_SHELL_SURFACE_TRANSIENT_INACTIVE) {
                    break;
                }
            default:
                if (!shsurf->isMinimized()) {
                    shsurf->unminimize();
                }
                shsurf->setAcceptNewState(true);
        }
    }
    m_windowsMinimized = false;
}

void Shell::pointerFocus(ShellSeat *, struct weston_pointer *pointer)
{
    struct weston_surface *surface = pointer->focus;

    if (!surface)
        return;

    struct weston_compositor *compositor = surface->compositor;
    ShellSurface *shsurf = Shell::getShellSurface(surface);
    if (!shsurf)
        return;

    if (!shsurf->isResponsive()) {
        shsurf->shell()->setBusyCursor(shsurf, pointer->seat);
    } else {
        uint32_t serial = wl_display_next_serial(compositor->wl_display);
        shsurf->ping(serial);
    }
}

void Shell::pingTimeout(ShellSurface *shsurf)
{
    struct weston_seat *seat;
    wl_list_for_each(seat, &m_compositor->seat_list, link) {
        if (seat->pointer->focus == shsurf->m_surface)
            setBusyCursor(shsurf, seat);
    }
}

void Shell::pong(ShellSurface *shsurf)
{
    if (!shsurf->isResponsive()) {
        struct weston_seat *seat;
        /* Received pong from previously unresponsive client */
        wl_list_for_each(seat, &m_compositor->seat_list, link) {
            struct weston_pointer *pointer = seat->pointer;
            if (pointer) {
                endBusyCursor(seat);
            }
        }
    }
}

void Shell::fadeSplash()
{
    m_splash->fadeOut();
}

const struct wl_shell_interface Shell::shell_implementation = {
    [](struct wl_client *client, struct wl_resource *resource, uint32_t id, struct wl_resource *surface_resource) {
        static_cast<Shell *>(wl_resource_get_user_data(resource))->getShellSurface(client, resource, id, surface_resource);
    }
};

void Shell::bind(struct wl_client *client, uint32_t version, uint32_t id)
{
    struct wl_resource *resource = wl_resource_create(client, &wl_shell_interface, version, id);
    if (resource)
        wl_resource_set_implementation(resource, &shell_implementation, this, nullptr);
}

void Shell::sigchld(int status)
{
    uint32_t time;

    m_child.process.pid = 0;
    m_child.client = nullptr; /* already destroyed by wayland */

    /* if desktop-shell dies more than 5 times in 30 seconds, give up */
    time = weston_compositor_get_time();
    if (time - m_child.deathstamp > 30000) {
        m_child.deathstamp = time;
        m_child.deathcount = 0;
    }

    m_child.deathcount++;
    if (m_child.deathcount > 5) {
        weston_log("weston-desktop-shell died, giving up.\n");
        return;
    }

    weston_log("weston-desktop-shell died, respawning...\n");
    launchShellProcess();
}

void Shell::launchShellProcess()
{
    m_child.client = weston_client_launch(m_compositor,
                                          &m_child.process,
                                          m_clientPath,
                                          [](struct weston_process *process, int status) {
                                              Child *child = container_of(process, Child, process);
                                              child->shell->sigchld(status);
                                          });

    if (!m_child.client)
        weston_log("not able to start %s\n", m_clientPath);
}


WL_EXPORT int
module_init(struct weston_compositor *ec, int *argc, char *argv[])
{

    char *client = nullptr;

    for (int i = 0; i < *argc; ++i) {
        if (char *s = strstr(argv[i], "--orbital-client=")) {
            client = strdup(s + 17);
            --*argc;
            break;
        }
    }

    Shell *shell = Shell::load<DesktopShell>(ec, client);
    if (!shell) {
        return -1;
    }

    return 0;
}
