/*

Pencil2D - Traditional Animation Software
Copyright (C) 2012-2020 Matthew Chiawen Chang

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

*/

#define CATCH_CONFIG_RUNNER
#include "catch.hpp"

#include <QApplication>
#ifdef STANLEE_AUTOMATION
#include <QTemporaryDir>
#include <QSettings>
#include "pencildef.h"
#endif

int main(int argc, char* argv[])
{
#ifdef STANLEE_AUTOMATION
    QTemporaryDir profile;
    if (!profile.isValid()) { return 1; }
    qputenv("STANLEE_SETTINGS_DIR", profile.path().toUtf8());
#endif
    QApplication a(argc, argv);

    int result = Catch::Session().run(argc, argv);

#ifdef STANLEE_AUTOMATION
    QSettings settings(PENCIL2D, PENCIL2D);
    settings.clear();
    settings.sync();
#endif
    return result;
}
