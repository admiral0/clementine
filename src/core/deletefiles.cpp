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

#include "deletefiles.h"
#include "musicstorage.h"
#include "taskmanager.h"

#include <QStringList>
#include <QTimer>
#include <QThread>

const int DeleteFiles::kBatchSize = 50;

DeleteFiles::DeleteFiles(TaskManager* task_manager, MusicStorage* storage)
  : thread_(NULL),
    task_manager_(task_manager),
    storage_(storage),
    started_(false),
    task_id_(0),
    progress_(0)
{
  original_thread_ = thread();
}

void DeleteFiles::Start(const SongList& songs) {
  if (thread_)
    return;

  songs_ = songs;

  task_id_ = task_manager_->StartTask(tr("Deleting files"));
  task_manager_->SetTaskBlocksLibraryScans(true);

  thread_ = new QThread;
  connect(thread_, SIGNAL(started()), SLOT(ProcessSomeFiles()));

  moveToThread(thread_);
  thread_->start();
}

void DeleteFiles::Start(const QStringList& filenames) {
  SongList songs;
  foreach (const QString& filename, filenames) {
    Song song;
    song.set_filename(filename);
    songs << song;
  }

  Start(songs);
}

void DeleteFiles::ProcessSomeFiles() {
  if (!started_) {
    storage_->StartDelete();
    started_ = true;
  }

  // None left?
  if (progress_ >= songs_.count()) {
    task_manager_->SetTaskProgress(task_id_, progress_, songs_.count());

    storage_->FinishCopy();

    task_manager_->SetTaskFinished(task_id_);

    // Move back to the original thread so deleteLater() can get called in
    // the main thread's event loop
    moveToThread(original_thread_);
    deleteLater();

    // Stop this thread
    thread_->quit();
    return;
  }

  // We process files in batches so we can be cancelled part-way through.

  const int n = qMin(songs_.count(), progress_ + kBatchSize);
  for ( ; progress_<n ; ++progress_) {
    task_manager_->SetTaskProgress(task_id_, progress_, songs_.count());

    storage_->DeleteFromStorage(songs_.at(progress_));
  }

  QTimer::singleShot(0, this, SLOT(ProcessSomeFiles()));
}