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

#ifndef SHELL_H
#define SHELL_H

#include <list>
#include <vector>

#include "utils.h"
#include "layer.h"

class ShellSurface;
struct ShellGrab;
class Effect;
class Workspace;
class ShellSeat;
class Animation;

typedef std::list<ShellSurface *> ShellSurfaceList;

class Shell;

class ShellGrab {
public:
    ShellGrab();
    virtual ~ShellGrab();

    void end();

    Shell *shell() const { return m_shell; }
    weston_pointer *pointer() const { return m_pointer; }

    static ShellGrab *fromGrab(weston_pointer_grab *grab);

protected:
    virtual void focus() {}
    virtual void motion(uint32_t time) {}
    virtual void button(uint32_t time, uint32_t button, uint32_t state) {}

private:
    Shell *m_shell;
    weston_pointer *m_pointer;
    struct Grab {
        weston_pointer_grab base;
        ShellGrab *parent;
    } m_grab;

    static const weston_pointer_grab_interface s_shellGrabInterface;

    friend class Shell;
};

class Binding {
public:
    virtual ~Binding();

protected:
    explicit Binding(struct weston_binding *binding);

private:
    struct weston_binding *m_binding;

    friend class Shell;
};

class Shell {
public:
    enum class PanelPosition {
        Top = 0,
        Left = 1,
        Bottom = 2,
        Right = 3
    };

    template<class T>
    static Shell *load(struct weston_compositor *ec, char *client);
    virtual ~Shell();

    void launchShellProcess();
    ShellSurface *createShellSurface(struct weston_surface *surface, const struct weston_shell_client *client);
    ShellSurface *getShellSurface(struct wl_client *client, struct wl_resource *resource, uint32_t id, struct wl_resource *surface_resource);
    void removeShellSurface(ShellSurface *surface);
    static ShellSurface *getShellSurface(const struct weston_surface *surf);
    Binding *bindKey(uint32_t key, enum weston_keyboard_modifier modifier, weston_key_binding_handler_t handler, void *data);
    template<class T>
    Binding *bindKey(uint32_t key, enum weston_keyboard_modifier modifier,
                                   void (T::*func)(struct weston_seat *seat, uint32_t time, uint32_t key), T *obj);

    Binding *bindAxis(uint32_t axis, enum weston_keyboard_modifier modifier, weston_axis_binding_handler_t handler, void *data);
    template<class T>
    Binding *bindAxis(uint32_t axis, enum weston_keyboard_modifier modifier,
                      void (T::*func)(struct weston_seat *seat, uint32_t time, uint32_t axis, wl_fixed_t value), T *obj);

    void registerEffect(Effect *effect);

    void configureSurface(ShellSurface *surface, int32_t sx, int32_t sy, int32_t width, int32_t height);

    void setBackgroundSurface(struct weston_surface *surface, struct weston_output *output);
    void setGrabSurface(struct weston_surface *surface);
    void addPanelSurface(weston_surface *surface, weston_output *output, PanelPosition pos);
    void addOverlaySurface(struct weston_surface *surface, struct weston_output *output);

    void startGrab(ShellGrab *grab, weston_seat *seat, int32_t cursor = -1);

    void showPanels();
    void hidePanels();
    bool isInFullscreen() const;

    virtual IRect2D windowsArea(struct weston_output *output) const;

    inline struct weston_compositor *compositor() const { return m_compositor; }
    struct weston_output *getDefaultOutput() const;
    Workspace *currentWorkspace() const;
    Workspace *workspace(uint32_t id) const;
    void selectPreviousWorkspace();
    void selectNextWorkspace();
    void selectWorkspace(int32_t id);
    uint32_t numWorkspaces() const;

    void showAllWorkspaces();
    void resetWorkspaces();

    void minimizeWindows();
    void restoreWindows();

    struct wl_client *shellClient() { return m_child.client; }
    struct wl_resource *shellClientResource() { return m_child.desktop_shell; }

protected:
    Shell(struct weston_compositor *ec);
    virtual void init();
    void quit();
    inline const ShellSurfaceList &surfaces() const { return m_surfaces; }
    virtual void setGrabCursor(uint32_t cursor) {}
    virtual void setBusyCursor(ShellSurface *shsurf, struct weston_seat *seat) {}
    virtual void endBusyCursor(struct weston_seat *seat) {}
    void fadeSplash();
    void addWorkspace(Workspace *ws);

    struct Child {
        Shell *shell;
        struct weston_process process;
        struct wl_client *client;
        struct wl_resource *desktop_shell;

        unsigned deathcount;
        uint32_t deathstamp;
    };
    Child m_child;

private:
    void destroy();
    void bind(struct wl_client *client, uint32_t version, uint32_t id);
    void sigchld(int status);
    void backgroundConfigure(struct weston_surface *es, int32_t sx, int32_t sy, int32_t width, int32_t height);
    void panelConfigure(struct weston_surface *es, int32_t sx, int32_t sy, int32_t width, int32_t height, PanelPosition pos);
    void activateSurface(struct weston_seat *seat, uint32_t time, uint32_t button);
    void configureFullscreen(ShellSurface *surface);
    void stackFullscreen(ShellSurface *surface);
    struct weston_surface *createBlackSurface(ShellSurface *fs_surface, float x, float y, int w, int h);
    static void sendConfigure(struct weston_surface *surface, uint32_t edges, int32_t width, int32_t height);
    bool surfaceIsTopFullscreen(ShellSurface *surface);
    void activateWorkspace(Workspace *old);
    void pointerFocus(ShellSeat *shseat, struct weston_pointer *pointer);
    void pingTimeout(ShellSurface *shsurf);
    void pong(ShellSurface *shsurf);
    weston_surface *createBlackSurface(int x, int y, int w, int h);
    void workspaceRemoved(Workspace *ws);

    struct weston_compositor *m_compositor;
    WlListener m_destroyListener;
    char *m_clientPath;
    Layer m_backgroundLayer;
    Layer m_panelsLayer;
    Layer m_fullscreenLayer;
    Layer m_overlayLayer;
    Layer m_splashLayer;
    std::vector<Effect *> m_effects;
    ShellSurfaceList m_surfaces;
    std::vector<Workspace *> m_workspaces;
    uint32_t m_currentWorkspace;
    bool m_windowsMinimized;

    std::list<weston_surface *> m_blackSurfaces;
    class Splash *m_splash;
    struct weston_surface *m_grabSurface;

    static void staticPanelConfigure(weston_surface *es, int32_t sx, int32_t sy, int32_t width, int32_t height);

    static const struct wl_shell_interface shell_implementation;
    static const struct weston_shell_client shell_client;

    friend class Effect;
};

template<class T>
Shell *Shell::load(struct weston_compositor *ec, char *client)
{
    Shell *shell = new T(ec);
    if (shell) {
        shell->m_clientPath = client;
        shell->init();
    }

    return shell;
}

template<class T, class... Args>
class MemberBinding : public Binding {
public:

private:
    typedef void (T::*Func)(Args...);

    MemberBinding(T *obj, Func func) : Binding(nullptr), m_obj(obj), m_func(func) {}
    static void handler(Args... args, void *data) {
        MemberBinding<T, Args...> *bind = static_cast<MemberBinding<T, Args...> *>(data);
        (bind->m_obj->*bind->m_func)(args...);
    }

    T *m_obj;
    Func m_func;

    friend class Shell;
};

template<class T>
Binding *Shell::bindKey(uint32_t key, enum weston_keyboard_modifier modifier,
                        void (T::*func)(struct weston_seat *seat, uint32_t time, uint32_t key), T *obj) {
    MemberBinding<T, struct weston_seat *, uint32_t, uint32_t> *binding = new MemberBinding<T, struct weston_seat *, uint32_t, uint32_t>(obj, func);
    binding->m_binding = weston_compositor_add_key_binding(m_compositor, key, modifier,
                                                           MemberBinding<T, struct weston_seat *, uint32_t, uint32_t>::handler, binding);
    return binding;
}

template<class T>
Binding *Shell::bindAxis(uint32_t axis, enum weston_keyboard_modifier modifier,
                         void (T::*func)(struct weston_seat *seat, uint32_t time, uint32_t axis, wl_fixed_t value), T *obj) {
    MemberBinding<T, struct weston_seat *, uint32_t, uint32_t, wl_fixed_t> *binding =
    new MemberBinding<T, struct weston_seat *, uint32_t, uint32_t, wl_fixed_t>(obj, func);
    binding->m_binding = weston_compositor_add_axis_binding(m_compositor, axis, modifier,
                         MemberBinding<T, struct weston_seat *, uint32_t, uint32_t, wl_fixed_t>::handler, binding);
    return binding;
}

#endif
