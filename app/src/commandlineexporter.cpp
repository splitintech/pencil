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

#include <QFileInfo>
#include <QTextStream>

#include "editor.h"
#include "fileformat.h"
#include "layercamera.h"
#include "layermanager.h"
#include "mainwindow2.h"
#include "movieexporter.h"
#include "object.h"
#include "playbackmanager.h"

#include "commandlineexporter.h"

#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
const auto qEndl = Qt::endl;
#else
const auto qEndl = endl;
#endif

CommandLineExporter::CommandLineExporter(Editor *editor) :
    mEditor(editor),
    mOut(stdout, QIODevice::WriteOnly),
    mErr(stderr, QIODevice::WriteOnly)
{

}

bool CommandLineExporter::process(const QString &inputPath,
                                  const QStringList &outputPaths,
                                  const QString &camera,
                                  int width,
                                  int height,
                                  int startFrame,
                                  int endFrame,
                                  bool transparency)
{
    LayerManager *layerManager = mEditor->layers();

    if(inputPath.isEmpty())
    {
        mErr << tr("Error: No input file specified. An input project file argument is required when output path(s) are specified.") << qEndl;
        return false;
    }

    Status s = mEditor->openObject(inputPath, [](int){}, [](int){});
    if (!s.ok())
    {
        mErr << qEndl << qEndl << s.title() << qEndl << qEndl;
        mErr << s.description() << qEndl << qEndl;
        mErr << s.details().str() << qEndl << qEndl;
        return false;
    }

    LayerCamera *cameraLayer = nullptr;
    if (!camera.isEmpty())
    {
        int matches = 0;
        for (auto candidate : mEditor->object()->getLayersByType<LayerCamera>())
        {
            if (candidate->name() == camera) { cameraLayer = candidate; ++matches; }
        }
        if (matches != 1)
        {
            mErr << tr("Error: the specified camera layer %1 is missing or ambiguous.").arg(camera) << qEndl;
            return false;
        }
    }
    if (cameraLayer == nullptr)
    {
        cameraLayer = dynamic_cast<LayerCamera*>(layerManager->getLastCameraLayer());
    }
    if (cameraLayer == nullptr)
    {
        mErr << tr("Error: No camera layer is available.") << qEndl;
        return false;
    }

    if (width < 0)
    {
        width = cameraLayer->getViewRect().width();
    }
    if (height < 0)
    {
        height = cameraLayer->getViewRect().height();
    }
    QSize exportSize(width, height);

    if (width <= 0 || height <= 0 || qint64(width)*height > 250000000 || startFrame < 1 || outputPaths.isEmpty())
    {
        mErr << tr("Error: Invalid export dimensions, range or destinations.") << qEndl;
        return false;
    }
    if (endFrame < 0)
    {
        endFrame = layerManager->animationLength(endFrame < -1);
    }

    if (endFrame < startFrame)
    {
        mErr << tr("Error: The last export frame precedes the first frame.") << qEndl;
        return false;
    }
    for (const QString& outputPath : outputPaths)
    {
        // Detect format
        QString format = detectFormatByFileNameExtension(outputPath);
        if (format.isNull())
        {
            if (!QFileInfo(outputPath).suffix().isEmpty())
            {
                mErr << tr("Error: The specified output format is unsupported.") << qEndl;
                return false;
            }
            format = "PNG";
        }

        if (isMovieFormat(format))
        {
            if (!exportMovie(outputPath, cameraLayer, exportSize, startFrame, endFrame, transparency)) { return false; }
            continue;
        }

        if (!exportImageSequence(outputPath, format, cameraLayer, exportSize, startFrame, endFrame, transparency)) { return false; }
    }

    return true;
}

bool CommandLineExporter::exportMovie(const QString &outputPath,
                                      const LayerCamera *cameraLayer,
                                      const QSize &exportSize,
                                      int startFrame,
                                      int endFrame,
                                      bool transparency)
{
    if (transparency)
    {
        mErr << tr("Error: Transparency is not currently supported in command-line movie exports.") << qEndl;
        return false;
    }

    mOut << tr("Exporting movie...", "Command line task progress") << qEndl;

    ExportMovieDesc desc;
    desc.strFileName = outputPath;
    desc.startFrame = startFrame;
    desc.endFrame = endFrame;
    desc.fps = mEditor->playback()->fps();
    desc.exportSize = exportSize;
    desc.strCameraName = cameraLayer->name();
    desc.cameraLayerId = cameraLayer->id();

    MovieExporter ex;
    auto status = ex.run(mEditor->object(), desc, [](float, float){}, [](float){}, [](const QString &){});
    if (!status.ok()) { mErr << status.title() << qEndl << status.description() << qEndl; return false; }
    mOut << tr("Done.", "Command line task done") << qEndl;
    return true;
}

bool CommandLineExporter::exportImageSequence(const QString &outputPath,
                                              const QString &format,
                                              const LayerCamera *cameraLayer,
                                              const QSize &exportSize,
                                              int startFrame,
                                              int endFrame,
                                              bool transparency)
{
    mOut << tr("Exporting image sequence...", "Command line task progress") << qEndl;
    auto status = mEditor->object()->exportFrames(startFrame,
                                    endFrame,
                                    cameraLayer,
                                    exportSize,
                                    outputPath,
                                    format,
                                    transparency,
                                    false,
                                    "",
                                    true,
                                    nullptr,
                                    0);
    if (!status.ok()) { mErr << status.title() << qEndl << status.description() << qEndl; return false; }
    mOut << tr("Done.", "Command line task done") << qEndl;
    return true;
}
