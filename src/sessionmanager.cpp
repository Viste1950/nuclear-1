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
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sstream>
#include <iostream>
#include <signal.h>

#include "sessionmanager.h"

SessionManager::SessionManager(const char *sessionFile)
              : m_sessionFile(sessionFile)
{
    printf("Using session file \"%s\".\n", sessionFile);
}

void SessionManager::restore()
{
    FILE *session = fopen(m_sessionFile.c_str(), "r");
    if (!session) {
        return;
    }

    char buf[512];

    while (1) {
        if (fgets(buf, sizeof(buf), session)) {
            for (size_t i = 0; i < strlen(buf); ++i) {
                if (buf[i] == '\n') {
                    buf[i] = ' ';
                }
            }
            start(buf);
        } else {
            break;
        }
    }

    fclose(session);
}

void SessionManager::save(const std::list<pid_t> &pids)
{
    FILE *session = fopen(m_sessionFile.c_str(), "w");
    if (!session) {
        return;
    }

    char file[32];
    char buf[512];

    for (pid_t pid: pids) {
        sprintf(file, "/proc/%i/cmdline", pid);
        FILE *f = fopen(file, "r");
        size_t size = fread(buf, 1, sizeof(buf), f);
        fclose(f);
        for (size_t i = 0; i < size; ++i) {
            if (buf[i] == '\0') {
                buf[i] = ' ';
            }
        }
        buf[size - 1] = '\n';
        buf[size] = '\0';

        fputs(buf, session);
    }

    fclose(session);
}

void SessionManager::start(const char *cmd)
{
    std::list<char *> strings;
    std::istringstream f(cmd);
    std::string s;
    while (std::getline(f, s, ' ')) {
        strings.push_back(strdup(s.c_str()));
    }

    int argc = strings.size();
    const char *path = strings.front();
    char *argv[argc + 1];
    int i = 0;
    for (char *str: strings) {
        argv[i++] = str;
    }
    argv[argc + 1] = nullptr;

    pid_t pid = fork();
    if (pid == 0) {
        pid_t p2 = fork();
        if (p2 == 0) {
            struct sigaction noaction;
            memset(&noaction, 0, sizeof(noaction));
            noaction.sa_handler = SIG_IGN;
            sigaction(SIGPIPE, &noaction, 0);
            setsid();

            execvp(path, argv);
            _exit(0);
        }

        _exit(0);
    }

    for (char *str: strings) {
        free(str);
    }
}
