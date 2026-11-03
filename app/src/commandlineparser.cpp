/*

Pencil2D - Traditional Animation Software
Copyright (C) 2005-2007 Patrick Corrieri & Pascal Naidon
Copyright (C) 2012-2020 Matthew Chiawen Chang

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

*/

#include "commandlineparser.h"

#include <QTextStream>

#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
const auto qEndl = Qt::endl;
#else
const auto qEndl = endl;
#endif

CommandLineParser::CommandLineParser() : mParser(), mInputPath(), mOutputPaths(), mCamera()
{
    mParser.setApplicationDescription(tr("Pencil2D is an animation/drawing software for Mac OS X, Windows, and Linux. "
                                         "It lets you create traditional hand-drawn animation (cartoon) using both bitmap and vector graphics."));
    mParser.addHelpOption();
    mParser.addVersionOption();
    mParser.addPositionalArgument("input", tr("Path to the input pencil file."));

    QCommandLineOption exportOutOption(QStringList() << "o" << "export",
                                       tr("Render the file to <output_path>"),
                                       tr("output_path"));
    mParser.addOption(exportOutOption);

    // for backwards compatibility
    QCommandLineOption exportSeqOption(QStringList() << "export-sequence",
                                       tr("Render the file to <output_path>"),
                                       tr("output_path"));
#if QT_VERSION >= QT_VERSION_CHECK(5, 8, 0)
    exportSeqOption.setFlags(QCommandLineOption::HiddenFromHelp);
#endif
    mParser.addOption(exportSeqOption);

    QCommandLineOption cameraOption(QStringList() << "camera",
                                    tr("Name of the camera layer to use"),
                                    tr("layer_name"));
    mParser.addOption(cameraOption);

    QCommandLineOption widthOption(QStringList() << "width",
                                   tr("Width of the output frames"),
                                   tr("integer"));
    mParser.addOption(widthOption);

    QCommandLineOption heightOption(QStringList() << "height",
                                    tr("Height of the output frames"),
                                    tr("integer"));
    mParser.addOption(heightOption);

    QCommandLineOption startOption(QStringList() << "start",
                                   tr("The first frame you want to include in the exported movie"),
                                   tr("frame"));
    mParser.addOption(startOption);

    QCommandLineOption endOption(QStringList() << "end",
                                 tr("The last frame you want to include in the exported movie. "
                                    "Can also be last or last-sound to automatically use the last "
                                    "frame containing animation or sound, respectively"),
                                 tr("frame"));
    mParser.addOption(endOption);

    QCommandLineOption transparencyOption(QStringList() << "transparency",
                                          tr("Render transparency when possible"));
    mParser.addOption(transparencyOption);
}

bool CommandLineParser::process(QStringList arguments)
{
    QTextStream err(stderr);

    arguments.removeOne("-NSDocumentRevisionsDebugMode");

    mParser.process(arguments);

    QStringList posArgs = mParser.positionalArguments();
    if (!posArgs.isEmpty())
    {
        mInputPath = posArgs.at(0);
    }

    mOutputPaths << mParser.values("export") << mParser.values("export-sequence");

    // Explicit invalid options must not silently select another export contract.
    for (const auto& option : {QStringLiteral("width"), QStringLiteral("height"), QStringLiteral("start")})
    {
        if (!mParser.isSet(option)) { continue; }
        bool ok = false;
        int value = mParser.value(option).toInt(&ok);
        if (!ok || value < 1)
        {
            err << tr("Error: %1 must be a positive integer.").arg(option) << qEndl;
            return false;
        }
        if (option == "width") { mWidth = value; }
        else if (option == "height") { mHeight = value; }
        else { mStartFrame = value; }
    }

    if (mParser.isSet("end"))
    {
        const QString value = mParser.value("end");
        if (value == "last") { mEndFrame = -1; }
        else if (value == "last-sound") { mEndFrame = -2; }
        else
        {
            bool ok = false;
            mEndFrame = value.toInt(&ok);
            if (!ok || mEndFrame < mStartFrame)
            {
                err << tr("Error: end must be last, last-sound or an integer at least as large as start.") << qEndl;
                return false;
            }
        }
    }
    if (mParser.isSet("camera") && mParser.value("camera").isEmpty())
    {
        err << tr("Error: The camera name must not be empty.") << qEndl;
        return false;
    }

    mTransparency = mParser.isSet("transparency");

    mCamera = mParser.value("camera");
    return true;
}
