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

#include "config.h"
#include "commandlineoptions.h"
#include "version.h"
#include "core/logging.h"

#include <cstdlib>
#include <getopt.h>
#include <iostream>

#include <QBuffer>
#include <QCoreApplication>
#include <QFileInfo>


const char* CommandlineOptions::kHelpText =
    "%1: clementine [%2] [%3]\n"
    "\n"
    "%4:\n"
    "  -p, --play                %5\n"
    "  -t, --play-pause          %6\n"
    "  -u, --pause               %7\n"
    "  -s, --stop                %8\n"
    "  -r, --previous            %9\n"
    "  -f, --next                %10\n"
    "  -v, --volume <value>      %11\n"
    "  --volume-up               %12\n"
    "  --volume-down             %13\n"
    "  --seek-to <seconds>       %14\n"
    "  --seek-by <seconds>       %15\n"
    "\n"
    "%16:\n"
    "  -a, --append              %17\n"
    "  -l, --load                %18\n"
    "  -k, --play-track <n>      %19\n"
    "\n"
    "%20:\n"
    "  -o, --show-osd            %21\n"
    "  -y, --toggle-pretty-osd   %22\n"
    "  -g, --language <lang>     %24\n"
    "      --quiet               %25\n"
    "      --verbose             %26\n"
    "      --log-levels <levels> %27\n"
    "      --version             %28\n";

const char* CommandlineOptions::kVersionText =
    "Clementine %1";

CommandlineOptions::CommandlineOptions(int argc, char** argv)
  : argc_(argc),
    argv_(argv),
    url_list_action_(UrlList_Append),
    player_action_(Player_None),
    set_volume_(-1),
    volume_modifier_(0),
    seek_to_(-1),
    seek_by_(0),
    play_track_at_(-1),
    show_osd_(false),
    toggle_pretty_osd_(false),
    log_levels_(logging::kDefaultLogLevels)
{
#ifdef Q_OS_DARWIN
  // Remove -psn_xxx option that Mac passes when opened from Finder.
  RemoveArg("-psn", 1);
#endif

  // Remove the -session option that KDE passes
  RemoveArg("-session", 2);
}

void CommandlineOptions::RemoveArg(const QString& starts_with, int count) {
  for (int i = 0; i < argc_; ++i) {
    QString opt(argv_[i]);
    if (opt.startsWith(starts_with)) {
      for (int j = i; j < argc_ - count + 1; ++j) {
        argv_[j] = argv_[j+count];
      }
      argc_ -= count;
      break;
    }
  }
}

bool CommandlineOptions::Parse() {
  static const struct option kOptions[] = {
    {"help",        no_argument,       0, 'h'},

    {"play",        no_argument,       0, 'p'},
    {"play-pause",  no_argument,       0, 't'},
    {"pause",       no_argument,       0, 'u'},
    {"stop",        no_argument,       0, 's'},
    {"previous",    no_argument,       0, 'r'},
    {"next",        no_argument,       0, 'f'},
    {"volume",      required_argument, 0, 'v'},
    {"volume-up",   no_argument,       0, VolumeUp},

    {"volume-down", no_argument,       0, VolumeDown},
    {"seek-to",     required_argument, 0, SeekTo},
    {"seek-by",     required_argument, 0, SeekBy},

    {"append",            no_argument,       0, 'a'},
    {"load",              no_argument,       0, 'l'},
    {"play-track",        required_argument, 0, 'k'},
    {"show-osd",          no_argument,       0, 'o'},
    {"toggle-pretty-osd", no_argument,       0, 'y'},
    {"language",          required_argument, 0, 'g'},
    {"quiet",             no_argument,       0, Quiet},
    {"verbose",           no_argument,       0, Verbose},
    {"log-levels",        required_argument, 0, LogLevels},
    {"version",           no_argument,       0, Version},

    {0, 0, 0, 0}
  };

  // Parse the arguments
  bool ok = false;
  forever {
    int c = getopt_long(argc_, argv_, "hptusrfv:alk:oyg:", kOptions, NULL);

    // End of the options
    if (c == -1)
      break;

    switch (c) {
      case 'h': {
        QString translated_help_text = QString(kHelpText).arg(
            tr("Usage"), tr("options"), tr("URL(s)"), tr("Player options"),
            tr("Start the playlist currently playing"),
            tr("Play if stopped, pause if playing"),
            tr("Pause playback"),
            tr("Stop playback"),
            tr("Skip backwards in playlist")).arg(
            tr("Skip forwards in playlist"),
            tr("Set the volume to <value> percent"),
            tr("Increase the volume by 4%"),
            tr("Decrease the volume by 4%"),
            tr("Seek the currently playing track to an absolute position"),
            tr("Seek the currently playing track by a relative amount"),
            tr("Playlist options"),
            tr("Append files/URLs to the playlist"),
            tr("Loads files/URLs, replacing current playlist")).arg(
            tr("Play the <n>th track in the playlist"),
            tr("Other options"),
            tr("Display the on-screen-display"),
            tr("Toggle visibility for the pretty on-screen-display"),
            tr("Change the language"),
            tr("Equivalent to --log-levels *:1"),
            tr("Equivalent to --log-levels *:3"),
            tr("Comma separated list of class:level, level is 0-3")).arg(
            tr("Print out version information"));

        std::cout << translated_help_text.toLocal8Bit().constData();
        return false;
      }

      case 'p': player_action_     = Player_Play;       break;
      case 't': player_action_     = Player_PlayPause;  break;
      case 'u': player_action_     = Player_Pause;      break;
      case 's': player_action_     = Player_Stop;       break;
      case 'r': player_action_     = Player_Previous;   break;
      case 'f': player_action_     = Player_Next;       break;
      case 'a': url_list_action_   = UrlList_Append;    break;
      case 'l': url_list_action_   = UrlList_Load;      break;
      case 'o': show_osd_          = true;              break;
      case 'y': toggle_pretty_osd_ = true;              break;
      case 'g': language_          = QString(optarg);   break;
      case VolumeUp:   volume_modifier_ = +4;           break;
      case VolumeDown: volume_modifier_ = -4;           break;
      case Quiet:      log_levels_ = "1";               break;
      case Verbose:    log_levels_ = "3";               break;
      case LogLevels:  log_levels_ = QString(optarg);   break;
      case Version: {
        QString version_text = QString(kVersionText).arg(CLEMENTINE_VERSION_DISPLAY);
        std::cout << version_text.toLocal8Bit().constData() << std::endl;
        std::exit(0);
      }
      case 'v':
        set_volume_ = QString(optarg).toInt(&ok);
        if (!ok) set_volume_ = -1;
        break;

      case SeekTo:
        seek_to_ = QString(optarg).toInt(&ok);
        if (!ok) seek_to_ = -1;
        break;

      case SeekBy:
        seek_by_ = QString(optarg).toInt(&ok);
        if (!ok) seek_by_ = 0;
        break;

      case 'k':
        play_track_at_ = QString(optarg).toInt(&ok);
        if (!ok) play_track_at_ = -1;
        break;

      case '?':
      default:
        return false;
    }
  }

  // Get any filenames or URLs following the arguments
  for (int i=optind ; i<argc_ ; ++i) {
    QString value = QFile::decodeName(argv_[i]);
    QFileInfo file_info(value);
    if (file_info.exists())
      urls_ << QUrl::fromLocalFile(file_info.canonicalFilePath());
    else
      urls_ << QUrl::fromUserInput(value);
  }

  return true;
}

bool CommandlineOptions::is_empty() const {
  return player_action_ == Player_None &&
         set_volume_ == -1 &&
         volume_modifier_ == 0 &&
         seek_to_ == -1 &&
         seek_by_ == 0 &&
         play_track_at_ == -1 &&
         show_osd_ == false &&
         toggle_pretty_osd_ == false &&
         urls_.isEmpty();
}

QByteArray CommandlineOptions::Serialize() const {
  QBuffer buf;
  buf.open(QIODevice::WriteOnly);

  QDataStream s(&buf);
  s << *this;
  buf.close();

  return buf.data();
}

void CommandlineOptions::Load(const QByteArray &serialized) {
  QByteArray copy(serialized);
  QBuffer buf(&copy);
  buf.open(QIODevice::ReadOnly);

  QDataStream s(&buf);
  s >> *this;
}

QString CommandlineOptions::tr(const char *source_text) {
  return QObject::tr(source_text);
}

QDataStream& operator<<(QDataStream& s, const CommandlineOptions& a) {
  s << qint32(a.player_action_)
    << qint32(a.url_list_action_)
    << a.set_volume_
    << a.volume_modifier_
    << a.seek_to_
    << a.seek_by_
    << a.play_track_at_
    << a.show_osd_
    << a.urls_
    << a.log_levels_
    << a.toggle_pretty_osd_;

  return s;
}

QDataStream& operator>>(QDataStream& s, CommandlineOptions& a) {
  quint32 player_action = 0;
  quint32 url_list_action = 0;
  s >> player_action
    >> url_list_action
    >> a.set_volume_
    >> a.volume_modifier_
    >> a.seek_to_
    >> a.seek_by_
    >> a.play_track_at_
    >> a.show_osd_
    >> a.urls_
    >> a.log_levels_
    >> a.toggle_pretty_osd_;
  a.player_action_ = CommandlineOptions::PlayerAction(player_action);
  a.url_list_action_ = CommandlineOptions::UrlListAction(url_list_action);

  return s;
}
