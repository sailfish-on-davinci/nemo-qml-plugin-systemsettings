/*
 * Copyright (C) 2019 Jolla Ltd.
 * Contact: Pekka Vuorela <pekka.vuorela@jolla.com>
 *
 * You may use this file under the terms of the BSD license as follows:
 *
 * "Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in
 *     the documentation and/or other materials provided with the
 *     distribution.
 *   * Neither the name of Nemo Mobile nor the names of its contributors
 *     may be used to endorse or promote products derived from this
 *     software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE."
 */

#include <QFile>
#include <QRegularExpression>
#include <QDebug>

#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#include "../src/localeconfig.h"

int main(int argc, char *argv[])
{
    if (argc != 2) {
        qWarning() << "No locale given";
        return EXIT_FAILURE;
    }

    QString configPath = localeConfigPath();

    if (configPath.isEmpty()) {
        return EXIT_FAILURE;
    }

    QString newLocale = QString(argv[1]);
    QRegularExpression allowedInput("^[a-zA-Z0-9\\.@_]*$");
    if (!allowedInput.match(newLocale).hasMatch()) {
        qWarning() << "Invalid locale input:" << newLocale;
        return EXIT_FAILURE;
    }

    QFile localeConfig(configPath);
    if (!localeConfig.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        qWarning() << "Unable to open locale configuration file for writing:" << configPath
                   << "-" << localeConfig.errorString();
        return EXIT_FAILURE;
    }

    localeConfig.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                QFileDevice::ReadGroup | QFileDevice::ReadOther);

    if (fchown(localeConfig.handle(), 0, 0)) {
        qWarning() << "Failed to set localeconfig as root:root" << strerror(errno);
    }

    localeConfig.write("# Autogenerated by settings\n");
    localeConfig.write(QString("LANG=%1\n").arg(newLocale).toLatin1());
    localeConfig.close();

    return EXIT_SUCCESS;
}
