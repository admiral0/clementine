/* This file is part of Clementine.
   Copyright 2010, David Sansome <me@davidsansome.com>

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

#include "musicstorage.h"
#include "organise.h"
#include "taskmanager.h"
#include "core/logging.h"
#include "core/tagreaderclient.h"

#include <QDir>
#include <QFileInfo>
#include <QTimer>
#include <QThread>
#include <QUrl>

#include <boost/bind.hpp>

const int Organise::kBatchSize = 10;
const int Organise::kTranscodeProgressInterval = 500;

Organise::Organise(TaskManager* task_manager,
                   boost::shared_ptr<MusicStorage> destination,
                   const OrganiseFormat &format, bool copy, bool overwrite,
                   const QStringList& files, bool eject_after)
                     : thread_(NULL),
                       task_manager_(task_manager),
                       transcoder_(new Transcoder(this)),
                       destination_(destination),
                       format_(format),
                       copy_(copy),
                       overwrite_(overwrite),
                       eject_after_(eject_after),
                       task_count_(files.count()),
                       transcode_suffix_(1),
                       tasks_complete_(0),
                       started_(false),
                       task_id_(0),
                       current_copy_progress_(0)
{
  original_thread_ = thread();

  foreach (const QString& filename, files) {
    tasks_pending_ << Task(filename);
  }
}

void Organise::Start() {
  if (thread_)
    return;

  task_id_ = task_manager_->StartTask(tr("Organising files"));
  task_manager_->SetTaskBlocksLibraryScans(true);

  thread_ = new QThread;
  connect(thread_, SIGNAL(started()), SLOT(ProcessSomeFiles()));
  connect(transcoder_, SIGNAL(JobComplete(QString,bool)), SLOT(FileTranscoded(QString,bool)));

  moveToThread(thread_);
  thread_->start();
}

void Organise::ProcessSomeFiles() {
  if (!started_) {
    transcode_temp_name_.open();

    if (!destination_->StartCopy(&supported_filetypes_)) {
      // Failed to start - mark everything as failed :(
      foreach (const Task& task, tasks_pending_)
        files_with_errors_ << task.filename_;
      tasks_pending_.clear();
    }
    started_ = true;
  }

  // None left?
  if (tasks_pending_.isEmpty()) {
    if (!tasks_transcoding_.isEmpty()) {
      // Just wait - FileTranscoded will start us off again in a little while
      qLog(Debug) << "Waiting for transcoding jobs";
      transcode_progress_timer_.start(kTranscodeProgressInterval, this);
      return;
    }

    UpdateProgress();

    destination_->FinishCopy(files_with_errors_.isEmpty());
    if (eject_after_)
      destination_->Eject();

    task_manager_->SetTaskFinished(task_id_);

    emit Finished(files_with_errors_);

    // Move back to the original thread so deleteLater() can get called in
    // the main thread's event loop
    moveToThread(original_thread_);
    deleteLater();

    // Stop this thread
    thread_->quit();
    return;
  }

  // We process files in batches so we can be cancelled part-way through.
  for (int i=0 ; i<kBatchSize ; ++i) {
    SetSongProgress(0);

    if (tasks_pending_.isEmpty())
      break;

    Task task = tasks_pending_.takeFirst();
    qLog(Info) << "Processing" << task.filename_;

    // Is it a directory?
    if (QFileInfo(task.filename_).isDir()) {
      QDir dir(task.filename_);
      foreach (const QString& entry, dir.entryList(
          QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot | QDir::Readable)) {
        tasks_pending_ << Task(task.filename_ + "/" + entry);
        task_count_ ++;
      }
      continue;
    }

    // Read metadata from the file
    Song song;
    TagReaderClient::Instance()->ReadFileBlocking(task.filename_, &song);
    if (!song.is_valid())
      continue;

    // Maybe this file is one that's been transcoded already?
    if (!task.transcoded_filename_.isEmpty()) {
      qLog(Debug) << "This file has already been transcoded";

      // Set the new filetype on the song so the formatter gets it right
      song.set_filetype(task.new_filetype_);

      // Fiddle the filename extension as well to match the new type
      song.set_url(QUrl::fromLocalFile(FiddleFileExtension(song.url().toLocalFile(), task.new_extension_)));
      song.set_basefilename(FiddleFileExtension(song.basefilename(), task.new_extension_));

      // Have to set this to the size of the new file or else funny stuff happens
      song.set_filesize(QFileInfo(task.transcoded_filename_).size());
    } else {
      // Figure out if we need to transcode it
      Song::FileType dest_type = CheckTranscode(song.filetype());
      if (dest_type != Song::Type_Unknown) {
        // Get the preset
        TranscoderPreset preset = Transcoder::PresetForFileType(dest_type);
        qLog(Debug) << "Transcoding with" << preset.name_;

        // Get a temporary name for the transcoded file
        task.transcoded_filename_ = transcode_temp_name_.fileName() + "-" +
                                    QString::number(transcode_suffix_++);
        task.new_extension_ = preset.extension_;
        task.new_filetype_ = dest_type;
        tasks_transcoding_[task.filename_] = task;

        qLog(Debug) << "Transcoding to" << task.transcoded_filename_;

        // Start the transcoding - this will happen in the background and
        // FileTranscoded() will get called when it's done.  At that point the
        // task will get re-added to the pending queue with the new filename.
        transcoder_->AddJob(task.filename_, preset, task.transcoded_filename_);
        transcoder_->Start();
        continue;
      }
    }

    MusicStorage::CopyJob job;
    job.source_ = task.transcoded_filename_.isEmpty() ?
                  task.filename_ : task.transcoded_filename_;
    job.destination_ = format_.GetFilenameForSong(song);
    job.metadata_ = song;
    job.overwrite_ = overwrite_;
    job.remove_original_ = !copy_;
    job.progress_ = boost::bind(&Organise::SetSongProgress,
                                this, _1, !task.transcoded_filename_.isEmpty());

    if (!destination_->CopyToStorage(job)) {
      files_with_errors_ << task.filename_;
    }

    // Clean up the temporary transcoded file
    if (!task.transcoded_filename_.isEmpty())
      QFile::remove(task.transcoded_filename_);

    tasks_complete_++;
  }
  SetSongProgress(0);

  QTimer::singleShot(0, this, SLOT(ProcessSomeFiles()));
}

Song::FileType Organise::CheckTranscode(Song::FileType original_type) const {
  if (original_type == Song::Type_Stream)
    return Song::Type_Unknown;

  const MusicStorage::TranscodeMode mode = destination_->GetTranscodeMode();
  const Song::FileType format = destination_->GetTranscodeFormat();

  switch (mode) {
    case MusicStorage::Transcode_Never:
      return Song::Type_Unknown;

    case MusicStorage::Transcode_Always:
      if (original_type == format)
        return Song::Type_Unknown;
      return format;

    case MusicStorage::Transcode_Unsupported:
      if (supported_filetypes_.isEmpty() || supported_filetypes_.contains(original_type))
        return Song::Type_Unknown;

      if (format != Song::Type_Unknown)
        return format;

      // The user hasn't visited the device properties page yet to set a
      // preferred format for the device, so we have to pick the best
      // available one.
      return Transcoder::PickBestFormat(supported_filetypes_);
  }
  return Song::Type_Unknown;
}

void Organise::SetSongProgress(float progress, bool transcoded) {
  const int max = transcoded ? 50 : 100;
  current_copy_progress_ = (transcoded ? 50 : 0) +
                           qBound(0, int(progress * max), max-1);
  UpdateProgress();
}

void Organise::UpdateProgress() {
  const int total = task_count_ * 100;

  // Update transcoding progress
  QMap<QString, float> transcode_progress = transcoder_->GetProgress();
  foreach (const QString& filename, transcode_progress.keys()) {
    if (!tasks_transcoding_.contains(filename))
      continue;
    tasks_transcoding_[filename].transcode_progress_ = transcode_progress[filename];
  }

  // Count the progress of all tasks that are in the queue.  Files that need
  // transcoding total 50 for the transcode and 50 for the copy, files that
  // only need to be copied total 100.
  int progress = tasks_complete_ * 100;

  foreach (const Task& task, tasks_pending_) {
    progress += qBound(0, int(task.transcode_progress_ * 50), 50);
  }
  foreach (const Task& task, tasks_transcoding_.values()) {
    progress += qBound(0, int(task.transcode_progress_ * 50), 50);
  }

  // Add the progress of the track that's currently copying
  progress += current_copy_progress_;

  task_manager_->SetTaskProgress(task_id_, progress, total);
}

void Organise::FileTranscoded(const QString& filename, bool success) {
  qLog(Info) << "File finished" << filename << success;
  transcode_progress_timer_.stop();

  Task task = tasks_transcoding_.take(filename);
  if (!success) {
    files_with_errors_ << filename;
  } else {
    tasks_pending_ << task;
  }
  QTimer::singleShot(0, this, SLOT(ProcessSomeFiles()));
}

QString Organise::FiddleFileExtension(const QString& filename, const QString& new_extension) {
  if (filename.section('/', -1, -1).contains('.'))
    return filename.section('.', 0, -2) + "." + new_extension;
  return filename + "." + new_extension;
}

void Organise::timerEvent(QTimerEvent* e) {
  QObject::timerEvent(e);

  if (e->timerId() == transcode_progress_timer_.timerId()) {
    UpdateProgress();
  }
}
