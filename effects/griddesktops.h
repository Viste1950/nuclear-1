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

#ifndef GRIDDESKTOPS_H
#define GRIDDESKTOPS_H

#include <list>

#include "effect.h"

class ShellGrab;
class Animation;
class Binding;

class GridDesktops : public Effect
{
public:
    GridDesktops(Shell *shell);
    ~GridDesktops();

    virtual void run(struct weston_seat *seat, uint32_t time, uint32_t key);
    void end(ShellSurface *surface);

private:
    void run(struct weston_seat *ws);

    bool m_scaled;
    struct weston_seat *m_seat;
    struct Grab *m_grab;
    Binding *m_binding;
    int m_setWs;

    friend Grab;
};

#endif
