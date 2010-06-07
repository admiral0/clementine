/* This file is part of Clementine.

   Clementine is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   Clementine is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with Clementine.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "projectmvisualisation.h"
#include "visualisationcontainer.h"

#include <QTimerEvent>
#include <QPainter>
#include <QPaintEngine>
#include <QtDebug>
#include <QGLWidget>
#include <QGraphicsView>
#include <QSettings>

#include <projectM.hpp>
#include <GL/gl.h>

ProjectMVisualisation::ProjectMVisualisation(QObject *parent)
  : QGraphicsScene(parent),
    projectm_(NULL),
    mode_(0),
    texture_size_(512)
{
  connect(this, SIGNAL(sceneRectChanged(QRectF)), SLOT(SceneRectChanged(QRectF)));
}

ProjectMVisualisation::~ProjectMVisualisation() {
}

void ProjectMVisualisation::drawBackground(QPainter* p, const QRectF&) {
  p->beginNativePainting();

  if (!projectm_) {
    projectm_.reset(new projectM("/usr/share/projectM/config.inp"));
    projectm_->changeTextureSize(texture_size_);
    Load();
  }

  projectm_->projectM_resetGL(sceneRect().width(), sceneRect().height());
  projectm_->renderFrame();

  p->endNativePainting();
}

void ProjectMVisualisation::SceneRectChanged(const QRectF &rect) {
  if (projectm_)
    projectm_->projectM_resetGL(rect.width(), rect.height());
}

void ProjectMVisualisation::SetTextureSize(int size) {
  texture_size_ = size;

  if (projectm_)
    projectm_->changeTextureSize(texture_size_);
}

void ProjectMVisualisation::ConsumeBuffer(GstBuffer *buffer, GstEnginePipeline*) {
  const int samples_per_channel = GST_BUFFER_SIZE(buffer) / sizeof(short) / 2;
  const short* data = reinterpret_cast<short*>(GST_BUFFER_DATA(buffer));

  if (projectm_)
    projectm_->pcm()->addPCM16Data(data, samples_per_channel);
  gst_buffer_unref(buffer);
}

void ProjectMVisualisation::set_selected(int preset, bool selected) {
  if (selected)
    selected_indices_.insert(preset);
  else
    selected_indices_.remove(preset);

  Save();
}

void ProjectMVisualisation::set_all_selected(bool selected) {
  selected_indices_.clear();
  if (selected) {
    int count = projectm_->getPlaylistSize();
    for (int i=0 ; i<count ; ++i) {
      selected_indices_ << i;
    }
  }
  Save();
}

void ProjectMVisualisation::Load() {
  QSettings s;
  s.beginGroup(VisualisationContainer::kSettingsGroup);
  QVariantList presets(s.value("presets").toList());

  int count = projectm_->getPlaylistSize();
  selected_indices_.clear();

  if (presets.isEmpty()) {
    for (int i=0 ; i<count ; ++i)
      selected_indices_ << i;
  } else {
    foreach (const QVariant& var, presets)
      if (var.toInt() < count)
        selected_indices_ << var.toInt();
  }

  mode_ = s.value("mode", 0).toInt();
}

void ProjectMVisualisation::Save() {
  QVariantList list;

  foreach (int index, selected_indices_.values()) {
    list << index;
  }

  QSettings s;
  s.beginGroup(VisualisationContainer::kSettingsGroup);
  s.setValue("presets", list);
  s.setValue("mode", mode_);
}

void ProjectMVisualisation::set_mode(int mode) {
  mode_ = mode;
  Save();
}
